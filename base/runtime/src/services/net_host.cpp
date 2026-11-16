// SPDX-License-Identifier: GPL-3.0-or-later
// Host sockets for bsd (nn::socket::sf::IClient) when [net] sockets = host: guest socket calls run on real host sockets (nxrt/os/net.h), so a title
// can reach a server on this PC (or the LAN). Guest descriptors are small ints from 0 up (the first free slot), each mapped to a host
// SOCKET; DuplicateSocket shares one. Blocking calls block the calling host thread (guest threads are host threads).
// Name lookups (sfdnsres, network.cpp) resolve through [net] hosts first; [net] dns = host lets other names reach the PC's resolver.
// [net] port_map rewrites the remote port in Connect and SendTo.
//
// Adapted from Ryujinx (MIT): src/Ryujinx.HLE/HOS/Services/Sockets/Bsd/IClient.cs, BsdContext.cs, Impl/ManagedSocket.cs,
//   Impl/ManagedSocketPollManager.cs, Impl/WinSockHelper.cs, Types/BsdSockAddr.cs, BsdSocketOption.cs, BsdSocketFlags.cs,
//   BsdSocketCreationFlags.cs, BsdIoctl.cs, LinuxError.cs, PollEventTypeMask.cs (Ryubing tree, Codeberg archive a82350bb77). Command ids,
//   argument and buffer layouts, the BSD sockaddr (sin_len, sin_family bytes, big-endian port), option numbers, Linux errno numbers in
//   replies, poll bits, the fd_set bit order and fcntl O_NONBLOCK = 0x800.
// Checked against libnx (ISC): nx/source/services/bsd.c (Select input: nfds, then a 16-byte timeval at offset 8 and an is_null byte;
//   the extra u32 length output of Accept/GetPeerName/GetSockName/RecvFrom/GetSockOpt) and runtime/devices/socket.c (O_NONBLOCK 0x800).
//
// Logs (Info, tag "net"): the first Connect of each socket ("bsd connect fd=3 -> 127.0.0.1:8443 (mapped from 443)") and errors that are
// not EAGAIN / EINPROGRESS, once per socket, command and errno.
#include "net_host.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "nxrt/strutil.h"
#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nxrt/os/net.h"
#include "nxrt/services.h"

namespace nxrt::services::nethost {
namespace {

// ---- Linux errno numbers (Ryujinx LinuxError.cs) ----------------------------------------------------------------------------------------
constexpr uint32_t kEBADF = 9, kEAGAIN = 11, kEFAULT = 14, kEINVAL = 22, kEMFILE = 24, kENOTSOCK = 88,
                   kENOPROTOOPT = 92, kEOPNOTSUPP = 95, kEAFNOSUPPORT = 97, kEINPROGRESS = 115;

// ---- BSD constants (Ryujinx Bsd/Types) ---------------------------------------------------------------------------------------------------
constexpr int kBsdAfInet = 2;
constexpr uint32_t kBsdTypeMask = 0x0FFFFFFF, kBsdNonBlockingFlag = 2, kBsdFlagsShift = 28;   // BsdSocketCreationFlags
constexpr uint32_t kMsgOob = 0x1, kMsgPeek = 0x2, kMsgDontRoute = 0x4, kMsgWaitAll = 0x40, kMsgDontWait = 0x80;
constexpr uint16_t kPollIn = 1, kPollPri = 2, kPollOut = 4, kPollErr = 8, kPollHup = 0x10, kPollNval = 0x20, kPollRdNorm = 0x40,
                   kPollRdBand = 0x80, kPollWrBand = 0x100;
constexpr uint32_t kFionread = 0x4004667F, kFionbio = 0x8004667E, kSiocatmark = 0x40047307;   // FreeBSD _IOR/_IOW('f', 127/126), ('s', 7)
constexpr int kFGetFl = 3, kFSetFl = 4, kONonBlock = 0x800;
constexpr int kSolSocket = 0xFFFF, kIpprotoIp = 0, kIpprotoTcp = 6;
constexpr int kSockStream = 1, kSockDgram = 2;   // BSD socket types
namespace hn = nxrt::os::net;

// ---- config ------------------------------------------------------------------------------------------------------------------------------
using nxrt::str::lower;
using nxrt::str::trim;
std::vector<std::pair<std::string, std::string>> pairs(const std::string& text) {
    std::vector<std::pair<std::string, std::string>> out;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find(';', pos);
        if (end == std::string::npos) end = text.size();
        const std::string item = text.substr(pos, end - pos);
        const size_t eq = item.find('=');
        if (eq != std::string::npos) {
            std::string k = trim(item.substr(0, eq)), v = trim(item.substr(eq + 1));
            if (!k.empty() && !v.empty()) out.emplace_back(std::move(k), std::move(v));
        }
        pos = end + 1;
    }
    return out;
}

bool parse_ipv4(const std::string& s, uint8_t ip[4]) {
    unsigned v[4];
    char tail = 0;
    if (std::sscanf(s.c_str(), "%3u.%3u.%3u.%3u%c", &v[0], &v[1], &v[2], &v[3], &tail) != 4) return false;
    for (int i = 0; i < 4; ++i) {
        if (v[i] > 255) return false;
        ip[i] = static_cast<uint8_t>(v[i]);
    }
    return true;
}

// [net] port_map: the remote port a guest connect/sendto uses -> the port actually used. Host byte order.
uint16_t map_port(uint16_t port) {
    for (const auto& [k, v] : pairs(config_get_str("net", "port_map", ""))) {
        if (std::atoi(k.c_str()) == port) {
            const int to = std::atoi(v.c_str());
            if (to > 0 && to < 65536) return static_cast<uint16_t>(to);
        }
    }
    return port;
}

// ---- descriptor table ------------------------------------------------------------------------------------------------------------------
struct HostSock {
    hn::Socket s = hn::kInvalidSocket;
    bool nonblock = false;
    bool connect_logged = false;
    bool sendto_target_logged = false;
    std::set<std::string> errors_logged;
    ~HostSock() {
        if (s != hn::kInvalidSocket) hn::close(s);
    }
};
using SockPtr = std::shared_ptr<HostSock>;

struct Table {
    std::mutex mu;
    std::vector<SockPtr> fds;
};
Table& tab() {
    static Table t;
    return t;
}

int fd_add(SockPtr p) {
    Table& t = tab();
    std::lock_guard<std::mutex> lk(t.mu);
    for (size_t i = 0; i < t.fds.size(); ++i) {
        if (!t.fds[i]) {
            t.fds[i] = std::move(p);
            return static_cast<int>(i);
        }
    }
    t.fds.push_back(std::move(p));
    return static_cast<int>(t.fds.size() - 1);
}
SockPtr fd_get(int fd) {
    Table& t = tab();
    std::lock_guard<std::mutex> lk(t.mu);
    if (fd < 0 || static_cast<size_t>(fd) >= t.fds.size()) return nullptr;
    return t.fds[static_cast<size_t>(fd)];
}
bool fd_close(int fd) {
    SockPtr p;
    Table& t = tab();
    std::lock_guard<std::mutex> lk(t.mu);
    if (fd < 0 || static_cast<size_t>(fd) >= t.fds.size() || !t.fds[static_cast<size_t>(fd)]) return false;
    p = std::move(t.fds[static_cast<size_t>(fd)]);
    return true;
}

// ---- replies ----------------------------------------------------------------------------------------------------------------------------
// (i32 ret, u32 bsd_errno); ret is -1 whenever errno is set (Ryujinx WriteBsdResult).
uint32_t reply(ipc::Response& rsp, int32_t ret, uint32_t err) {
    rsp.push(err ? int32_t{-1} : ret);
    rsp.push(err);
    return 0;
}

void log_error(const SockPtr& p, int fd, const char* what, uint32_t err) {
    if (err == 0 || err == kEAGAIN || err == kEINPROGRESS) return;
    if (p) {
        std::lock_guard<std::mutex> lk(tab().mu);
        if (!p->errors_logged.insert(std::string(what) + " " + std::to_string(err)).second) return;
    }
    NXLOG_I("net", "bsd %s fd=%d: errno %u", what, fd, err);
}

// The errno of a failed os::net call (they return the negative errno), logged once per socket, command and errno.
uint32_t fail_errno(const SockPtr& p, int fd, const char* what, int64_t rc) {
    const uint32_t e = static_cast<uint32_t>(rc < 0 ? -rc : rc);
    log_error(p, fd, what, e);
    return e;
}

// ---- sockaddr ---------------------------------------------------------------------------------------------------------------------------
// BSD sockaddr_in: u8 sin_len, u8 sin_family, u16 sin_port (big-endian), u8 sin_addr[4], u8 zero[8].
bool read_sockaddr(const ipc::Buffer& b, hn::Ipv4& out) {
    uint8_t raw[16] = {};
    if (!b.addr || b.size < 8) return false;
    buf_read(b, raw, 16);
    if (raw[1] != kBsdAfInet && raw[1] != 0) return false;
    out = {};
    out.port = static_cast<uint16_t>(raw[2] << 8 | raw[3]);
    std::memcpy(out.ip, raw + 4, 4);
    return true;
}
void write_sockaddr(const ipc::Buffer& b, const hn::Ipv4& a) {
    uint8_t raw[16] = {};
    raw[0] = 16;
    raw[1] = kBsdAfInet;
    raw[2] = static_cast<uint8_t>(a.port >> 8);
    raw[3] = static_cast<uint8_t>(a.port & 0xFF);
    std::memcpy(raw + 4, a.ip, 4);
    buf_write(b, raw, 16);
}
std::string addr_text(const hn::Ipv4& a) {
    return std::to_string(a.ip[0]) + "." + std::to_string(a.ip[1]) + "." + std::to_string(a.ip[2]) + "." + std::to_string(a.ip[3]) + ":" + std::to_string(a.port);
}

uint32_t host_msg_flags(uint32_t f) {
    uint32_t h = 0;
    if (f & kMsgOob) h |= hn::kMsgOob;
    if (f & kMsgPeek) h |= hn::kMsgPeek;
    if (f & kMsgDontRoute) h |= hn::kMsgDontRoute;
    if (f & kMsgWaitAll) h |= hn::kMsgWaitAll;
    return h;
}

// MSG_DONTWAIT on a blocking socket: non-blocking for this one call (Ryujinx ManagedSocket.Receive).
struct DontWait {
    hn::Socket s;
    bool active;
    DontWait(const HostSock& h, uint32_t flags) : s(h.s), active(!h.nonblock && (flags & kMsgDontWait)) {
        if (active) hn::set_nonblocking(s, true);
    }
    ~DontWait() {
        if (active) hn::set_nonblocking(s, false);
    }
};

// ---- commands ---------------------------------------------------------------------------------------------------------------------------

uint32_t do_socket(ipc::Request& req, ipc::Response& rsp) {
    hn::init();
    const int32_t domain = req.arg<int32_t>(0);
    const uint32_t type_raw = req.arg<uint32_t>(4);
    int32_t protocol = req.arg<int32_t>(8);
    const uint32_t flags = type_raw >> kBsdFlagsShift;
    const int type = static_cast<int>(type_raw & kBsdTypeMask);
    if (domain != kBsdAfInet && domain != 0) return reply(rsp, -1, kEAFNOSUPPORT);   // IPv4 only (the sockaddr layout here is sockaddr_in)
    if (type != kSockStream && type != kSockDgram) return reply(rsp, -1, kEINVAL);
    hn::Socket s = hn::kInvalidSocket;
    if (const int rc = hn::open(type == kSockStream ? hn::Type::Stream : hn::Type::Datagram, protocol, &s)) return reply(rsp, -1, fail_errno(nullptr, -1, "socket", rc));
    auto p = std::make_shared<HostSock>();
    p->s = s;
    if (flags & kBsdNonBlockingFlag) {
        p->nonblock = true;
        hn::set_nonblocking(s, true);
    }
    return reply(rsp, fd_add(std::move(p)), 0);
}

uint32_t do_connect(ipc::Request& req, ipc::Response& rsp) {
    const int fd = req.arg<int32_t>(0);
    SockPtr p = fd_get(fd);
    if (!p) return reply(rsp, -1, kEBADF);
    hn::Ipv4 a;
    if (!read_sockaddr(req.in_buf(0), a)) return reply(rsp, -1, kEAFNOSUPPORT);
    const uint16_t asked = a.port;
    const uint16_t port = map_port(asked);
    a.port = port;
    bool log_it = false;
    {
        std::lock_guard<std::mutex> lk(tab().mu);
        if (!p->connect_logged) log_it = p->connect_logged = true;
    }
    if (log_it) {
        if (port != asked) NXLOG_I("net", "bsd connect fd=%d -> %s (mapped from %u)", fd, addr_text(a).c_str(), asked);
        else NXLOG_I("net", "bsd connect fd=%d -> %s", fd, addr_text(a).c_str());
    }
    const int rc = hn::connect(p->s, a);
    if (rc == 0) return reply(rsp, 0, 0);
    return reply(rsp, -1, fail_errno(p, fd, "connect", rc));
}

uint32_t do_bind(ipc::Request& req, ipc::Response& rsp) {
    const int fd = req.arg<int32_t>(0);
    SockPtr p = fd_get(fd);
    if (!p) return reply(rsp, -1, kEBADF);
    hn::Ipv4 a;
    if (!read_sockaddr(req.in_buf(0), a)) return reply(rsp, -1, kEAFNOSUPPORT);
    const int rc = hn::bind(p->s, a);
    return rc == 0 ? reply(rsp, 0, 0) : reply(rsp, -1, fail_errno(p, fd, "bind", rc));
}

uint32_t do_listen(ipc::Request& req, ipc::Response& rsp) {
    const int fd = req.arg<int32_t>(0);
    SockPtr p = fd_get(fd);
    if (!p) return reply(rsp, -1, kEBADF);
    const int rc = hn::listen(p->s, req.arg<int32_t>(4));
    return rc == 0 ? reply(rsp, 0, 0) : reply(rsp, -1, fail_errno(p, fd, "listen", rc));
}

// Accept(u32 socket) -> (i32 ret, u32 bsd_errno, u32 addrlen, buffer<sockaddr_in, 0x22> addr)
uint32_t do_accept(ipc::Request& req, ipc::Response& rsp) {
    const int fd = req.arg<int32_t>(0);
    SockPtr p = fd_get(fd);
    if (!p) {
        reply(rsp, -1, kEBADF);
        rsp.push(uint32_t{0});
        return 0;
    }
    hn::Socket s = hn::kInvalidSocket;
    hn::Ipv4 a;
    const int rc = hn::accept(p->s, &s, &a);
    if (rc != 0) {
        reply(rsp, -1, fail_errno(p, fd, "accept", rc));
        rsp.push(uint32_t{0});
        return 0;
    }
    auto n = std::make_shared<HostSock>();
    n->s = s;
    n->nonblock = p->nonblock;   // an accepted socket keeps the listener's mode (as FreeBSD does)
    hn::set_nonblocking(s, n->nonblock);
    write_sockaddr(req.out_buf(0), a);
    reply(rsp, fd_add(std::move(n)), 0);
    rsp.push(uint32_t{16});
    return 0;
}

uint32_t do_name(ipc::Request& req, ipc::Response& rsp, bool peer) {
    const int fd = req.arg<int32_t>(0);
    SockPtr p = fd_get(fd);
    if (!p) {
        reply(rsp, -1, kEBADF);
        rsp.push(uint32_t{0});
        return 0;
    }
    hn::Ipv4 a;
    const int rc = peer ? hn::peer_addr(p->s, &a) : hn::local_addr(p->s, &a);
    if (rc != 0) {
        if (!peer && -rc == static_cast<int>(kEINVAL)) {   // not bound yet: the any address, port 0 (Ryujinx writes an empty sockaddr)
            a = {};
        } else {
            const uint32_t e = fail_errno(p, fd, peer ? "getpeername" : "getsockname", rc);
            reply(rsp, -1, e);
            rsp.push(uint32_t{0});
            return 0;
        }
    }
    write_sockaddr(req.out_buf(0), a);
    reply(rsp, 0, 0);
    rsp.push(uint32_t{16});
    return 0;
}

uint32_t do_send(ipc::Request& req, ipc::Response& rsp, bool to, bool plain_write) {
    const int fd = req.arg<int32_t>(0);
    const uint32_t flags = plain_write ? 0 : req.arg<uint32_t>(4);
    SockPtr p = fd_get(fd);
    if (!p) return reply(rsp, -1, kEBADF);
    const ipc::Buffer data = req.in_buf(0);
    const void* ptr = reinterpret_cast<const void*>(static_cast<uintptr_t>(data.addr));
    const size_t len = static_cast<size_t>(std::min<uint64_t>(data.size, 0x7FFFFFFF));
    DontWait dw(*p, flags);
    int64_t n;
    if (to) {
        hn::Ipv4 a;
        if (!read_sockaddr(req.in_buf(1), a)) return reply(rsp, -1, kEAFNOSUPPORT);
        a.port = map_port(a.port);
        n = hn::send(p->s, ptr, len, host_msg_flags(flags), &a);
        if (n < 0) {   // name the target once per fd: a bad address (0.0.0.0, port 0) is the usual cause
            if (!p->sendto_target_logged && (p->sendto_target_logged = true))
                NXLOG_I("net", "bsd sendto fd=%d target %u.%u.%u.%u:%u (%u bytes)", fd, a.ip[0], a.ip[1], a.ip[2], a.ip[3], a.port, static_cast<unsigned>(data.size));
        }
    } else {
        n = hn::send(p->s, ptr, len, host_msg_flags(flags), nullptr);
    }
    if (n < 0) return reply(rsp, -1, fail_errno(p, fd, to ? "sendto" : "send", n));
    return reply(rsp, static_cast<int32_t>(n), 0);
}

// Recv / Read: (i32 ret, u32 bsd_errno, buffer<0x22> message). RecvFrom adds (u32 addrlen) and the sockaddr in output buffer 1.
uint32_t do_recv(ipc::Request& req, ipc::Response& rsp, bool from, bool plain_read) {
    const int fd = req.arg<int32_t>(0);
    const uint32_t flags = plain_read ? 0 : req.arg<uint32_t>(4);
    SockPtr p = fd_get(fd);
    auto fail = [&](uint32_t e) {
        reply(rsp, -1, e);
        if (from) rsp.push(uint32_t{0});
        return 0u;
    };
    if (!p) return fail(kEBADF);
    const ipc::Buffer data = req.out_buf(0);
    void* ptr = reinterpret_cast<void*>(static_cast<uintptr_t>(data.addr));
    const size_t len = static_cast<size_t>(std::min<uint64_t>(data.size, 0x7FFFFFFF));
    DontWait dw(*p, flags);
    hn::Ipv4 a;
    const int64_t n = hn::recv(p->s, ptr, len, host_msg_flags(flags), from ? &a : nullptr);
    if (n < 0) return fail(fail_errno(p, fd, from ? "recvfrom" : "recv", n));
    reply(rsp, static_cast<int32_t>(n), 0);
    if (from) {
        const ipc::Buffer ab = req.out_buf(1);
        if (ab.addr && ab.size >= 16) {
            write_sockaddr(ab, a);
            rsp.push(uint32_t{16});
        } else {
            rsp.push(uint32_t{0});
        }
    }
    return 0;
}

// A guest option (level, BSD name) as a host option. False: unknown (ENOPROTOOPT). `ignored`: accepted and not applied.
struct HostOpt {
    hn::Level level = hn::Level::Socket;
    hn::Opt opt = hn::Opt::Debug;
    bool ignored = false;
};
bool host_opt(int level, int name, HostOpt* o) {
    using hn::Opt;
    if (level == kSolSocket) {
        o->level = hn::Level::Socket;
        switch (name) {
            case 0x1: o->opt = Opt::Debug; return true;
            case 0x2: o->opt = Opt::AcceptConn; return true;
            case 0x4: o->opt = Opt::ReuseAddr; return true;
            case 0x8: o->opt = Opt::KeepAlive; return true;
            case 0x10: o->opt = Opt::DontRoute; return true;
            case 0x20: o->opt = Opt::Broadcast; return true;
            case 0x40: o->opt = Opt::UseLoopback; return true;
            case 0x80: o->opt = Opt::Linger; return true;
            case 0x100: o->opt = Opt::OobInline; return true;
            case 0x200: o->opt = Opt::ReusePort; return true;   // Ryujinx maps it to ReuseAddress
            case 0x800: o->ignored = true; return true;          // SO_NOSIGPIPE: the host raises no signal
            case 0x1001: o->opt = Opt::SndBuf; return true;
            case 0x1002: o->opt = Opt::RcvBuf; return true;
            case 0x1003: o->opt = Opt::SndLowat; return true;
            case 0x1004: o->opt = Opt::RcvLowat; return true;
            case 0x1005: o->opt = Opt::SndTimeo; return true;
            case 0x1006: o->opt = Opt::RcvTimeo; return true;
            case 0x1007: o->opt = Opt::Error; return true;
            case 0x1008: o->opt = Opt::SockType; return true;
            default: return false;
        }
    }
    if (level == kIpprotoIp) {
        o->level = hn::Level::Ip;
        switch (name) {
            case 1: o->opt = Opt::IpOptions; return true;
            case 2: o->opt = Opt::IpHdrIncl; return true;
            case 3: o->opt = Opt::IpTos; return true;
            case 4: o->opt = Opt::IpTtl; return true;
            case 9: o->opt = Opt::IpMulticastIf; return true;
            case 10: o->opt = Opt::IpMulticastTtl; return true;
            case 11: o->opt = Opt::IpMulticastLoop; return true;
            case 12: o->opt = Opt::IpAddMembership; return true;
            case 13: o->opt = Opt::IpDropMembership; return true;
            case 67: o->opt = Opt::IpDontFragment; return true;
            default: return false;
        }
    }
    if (level == kIpprotoTcp) {
        o->level = hn::Level::Tcp;
        switch (name) {
            case 1: o->opt = Opt::TcpNoDelay; return true;
            case 2: o->opt = Opt::TcpMaxSeg; return true;
            case 0x100: o->opt = Opt::TcpKeepIdle; return true;
            case 0x200: o->opt = Opt::TcpKeepIntvl; return true;
            case 0x400: o->opt = Opt::TcpKeepCnt; return true;
            case 4: o->ignored = true; return true;   // TCP_NOPUSH
            default: return false;
        }
    }
    return false;
}
bool is_timeo(int level, int name) { return level == kSolSocket && (name == 0x1005 || name == 0x1006); }
bool is_linger(int level, int name) { return level == kSolSocket && name == 0x80; }

uint32_t do_setsockopt(ipc::Request& req, ipc::Response& rsp) {
    const int fd = req.arg<int32_t>(0);
    const int level = req.arg<int32_t>(4), name = req.arg<int32_t>(8);
    SockPtr p = fd_get(fd);
    if (!p) return reply(rsp, -1, kEBADF);
    HostOpt ho;
    if (!host_opt(level, name, &ho)) {
        log_error(p, fd, "setsockopt", kENOPROTOOPT);
        return reply(rsp, -1, kENOPROTOOPT);
    }
    if (ho.ignored) return reply(rsp, 0, 0);
    const ipc::Buffer b = req.in_buf(0);
    uint8_t raw[16] = {};
    const uint64_t got = buf_read(b, raw, sizeof raw);
    int rc;
    if (is_timeo(level, name)) {   // BSD struct timeval { s64 sec; s64 usec } -> milliseconds
        int64_t sec = 0, usec = 0;
        std::memcpy(&sec, raw, 8);
        std::memcpy(&usec, raw + 8, 8);
        const int64_t ms64 = sec * 1000 + usec / 1000;
        int64_t ms = ms64 <= 0 ? 0 : std::min<int64_t>(ms64, 0x7FFFFFFF);
        if (ms == 0 && (sec > 0 || usec > 0)) ms = 1;
        rc = hn::set_timeout_ms(p->s, ho.opt, ms);
    } else if (is_linger(level, name)) {   // BSD { int l_onoff; int l_linger }
        int32_t on = 0, secs = 0;
        std::memcpy(&on, raw, 4);
        std::memcpy(&secs, raw + 4, 4);
        rc = hn::set_linger(p->s, on != 0, std::clamp<int32_t>(secs, 0, 0xFFFF));
    } else if (level == kIpprotoIp && (name == 12 || name == 13)) {   // ip_mreq: two in_addr
        rc = hn::set_membership(p->s, ho.opt, raw);
    } else {
        int32_t v = 0;
        if (got >= 4) std::memcpy(&v, raw, 4);
        else if (got >= 1) v = raw[0];   // u_char options (IP_MULTICAST_TTL / LOOP)
        rc = hn::set_int(p->s, ho.level, ho.opt, v);
    }
    if (rc == 0) return reply(rsp, 0, 0);
    return reply(rsp, -1, fail_errno(p, fd, "setsockopt", rc));
}

// GetSockOpt(u32 socket, u32 level, u32 option_name) -> (i32 ret, u32 bsd_errno, u32 optlen, buffer<0x22> value)
uint32_t do_getsockopt(ipc::Request& req, ipc::Response& rsp) {
    const int fd = req.arg<int32_t>(0);
    const int level = req.arg<int32_t>(4), name = req.arg<int32_t>(8);
    SockPtr p = fd_get(fd);
    auto fail = [&](uint32_t e) {
        reply(rsp, -1, e);
        rsp.push(uint32_t{0});
        return 0u;
    };
    if (!p) return fail(kEBADF);
    const ipc::Buffer out = req.out_buf(0);
    HostOpt ho;
    if (!host_opt(level, name, &ho)) {
        log_error(p, fd, "getsockopt", kENOPROTOOPT);
        return fail(kENOPROTOOPT);
    }
    if (ho.ignored) {   // ignored option: reads back as 0
        const int32_t z = 0;
        buf_write(out, &z, 4);
        reply(rsp, 0, 0);
        rsp.push(uint32_t{4});
        return 0;
    }
    uint8_t res[16] = {};
    uint32_t res_len = 4;
    if (is_timeo(level, name)) {
        int64_t ms = 0;
        if (const int rc = hn::get_timeout_ms(p->s, ho.opt, &ms)) return fail(fail_errno(p, fd, "getsockopt", rc));
        const int64_t sec = ms / 1000, usec = (ms % 1000) * 1000;
        std::memcpy(res, &sec, 8);
        std::memcpy(res + 8, &usec, 8);
        res_len = 16;
    } else if (is_linger(level, name)) {
        bool on = false;
        int secs = 0;
        if (const int rc = hn::get_linger(p->s, &on, &secs)) return fail(fail_errno(p, fd, "getsockopt", rc));
        const int32_t o = on ? 1 : 0, s = secs;
        std::memcpy(res, &o, 4);
        std::memcpy(res + 4, &s, 4);
        res_len = 8;
    } else {
        int32_t v = 0;
        if (const int rc = hn::get_int(p->s, ho.level, ho.opt, &v)) return fail(fail_errno(p, fd, "getsockopt", rc));   // SO_ERROR: a Linux errno
        std::memcpy(res, &v, 4);
    }
    if (out.size < res_len) res_len = static_cast<uint32_t>(out.size);
    buf_write(out, res, res_len);
    reply(rsp, 0, 0);
    rsp.push(res_len);
    return 0;
}

// Ioctl(u32 fd, u32 request, u32 bufcount, 4 x buffer<0x21>, 4 x buffer<0x22>) -> (i32 ret, u32 bsd_errno)
uint32_t do_ioctl(ipc::Request& req, ipc::Response& rsp) {
    const int fd = req.arg<int32_t>(0);
    const uint32_t request = req.arg<uint32_t>(4);
    SockPtr p = fd_get(fd);
    if (!p) return reply(rsp, -1, kEBADF);
    if (request == kFionbio) {
        int32_t v = 0;
        buf_read(req.in_buf(0), &v, 4);
        p->nonblock = v != 0;
        hn::set_nonblocking(p->s, p->nonblock);
        return reply(rsp, 0, 0);
    }
    if (request == kFionread) {
        uint32_t n = 0;
        if (const int rc = hn::bytes_available(p->s, &n)) return reply(rsp, -1, fail_errno(p, fd, "ioctl", rc));
        const int32_t v = static_cast<int32_t>(std::min<uint32_t>(n, 0x7FFFFFFF));
        buf_write(req.out_buf(0), &v, 4);
        return reply(rsp, 0, 0);
    }
    if (request == kSiocatmark) {   // no out-of-band data (Ryujinx answers 0 too)
        const int32_t z = 0;
        buf_write(req.out_buf(0), &z, 4);
        return reply(rsp, 0, 0);
    }
    log_error(p, fd, "ioctl", kEOPNOTSUPP);
    return reply(rsp, -1, kEOPNOTSUPP);
}

// Fcntl(u32 socket, u32 cmd, u32 arg) -> (i32 ret, u32 bsd_errno). F_GETFL / F_SETFL with O_NONBLOCK 0x800.
uint32_t do_fcntl(ipc::Request& req, ipc::Response& rsp) {
    const int fd = req.arg<int32_t>(0);
    const int cmd = req.arg<int32_t>(4), arg = req.arg<int32_t>(8);
    SockPtr p = fd_get(fd);
    if (!p) return reply(rsp, -1, kEBADF);
    if (cmd == kFGetFl) return reply(rsp, p->nonblock ? kONonBlock : 0, 0);
    if (cmd == kFSetFl) {
        p->nonblock = (arg & kONonBlock) != 0;
        hn::set_nonblocking(p->s, p->nonblock);
        return reply(rsp, 0, 0);
    }
    return reply(rsp, -1, kEOPNOTSUPP);
}

uint32_t do_shutdown(ipc::Request& req, ipc::Response& rsp) {
    const int fd = req.arg<int32_t>(0);
    const int how = req.arg<int32_t>(4);
    SockPtr p = fd_get(fd);
    if (!p) return reply(rsp, -1, kEBADF);
    if (how < 0 || how > 2) return reply(rsp, -1, kEINVAL);
    const int rc = hn::shutdown(p->s, static_cast<hn::Shut>(how));   // SHUT_RD/WR/RDWR
    return rc == 0 ? reply(rsp, 0, 0) : reply(rsp, -1, fail_errno(p, fd, "shutdown", rc));
}

uint32_t do_shutdown_all(ipc::Request& req, ipc::Response& rsp) {
    const int how = req.arg<int32_t>(0);
    if (how < 0 || how > 2) return reply(rsp, -1, kEINVAL);
    std::vector<SockPtr> all;
    {
        std::lock_guard<std::mutex> lk(tab().mu);
        for (const SockPtr& p : tab().fds)
            if (p) all.push_back(p);
    }
    for (const SockPtr& p : all) hn::shutdown(p->s, static_cast<hn::Shut>(how));
    return reply(rsp, 0, 0);
}

// Poll(u32 nfds, i32 timeout_ms, buffer<pollfd, 0x21>) -> (i32 ret, u32 bsd_errno, buffer<pollfd, 0x22>). pollfd = {i32 fd, u16 events, u16 revents}.
uint32_t do_poll(ipc::Request& req, ipc::Response& rsp) {
    const int32_t nfds = req.arg<int32_t>(0);
    const int32_t timeout = req.arg<int32_t>(4);
    const ipc::Buffer in = req.in_buf(0);
    if (nfds < 0 || timeout < -1 || static_cast<uint64_t>(nfds) * 8 > in.size) return reply(rsp, -1, kEINVAL);
    std::vector<uint8_t> fds(static_cast<size_t>(nfds) * 8);
    buf_read(in, fds.data(), fds.size());
    std::vector<hn::PollItem> host;
    std::vector<int> slot_of;
    std::vector<SockPtr> keep;
    int ready = 0;
    for (int i = 0; i < nfds; ++i) {
        int32_t fd;
        uint16_t ev, rev = 0;
        std::memcpy(&fd, &fds[i * 8], 4);
        std::memcpy(&ev, &fds[i * 8 + 4], 2);
        if (fd >= 0) {
            SockPtr p = fd_get(fd);
            if (!p) {
                rev = kPollNval;
                ++ready;
            } else {
                hn::PollItem w;
                w.s = p->s;
                w.events = ev;   // the poll bits of os::net are the BSD ones
                host.push_back(w);
                slot_of.push_back(i);
                keep.push_back(std::move(p));
            }
        }
        std::memcpy(&fds[i * 8 + 6], &rev, 2);
    }
    if (!host.empty()) {
        const int rc = hn::poll(host.data(), host.size(), ready ? 0 : timeout);
        if (rc < 0) return reply(rsp, -1, fail_errno(nullptr, -1, "poll", rc));
        for (size_t k = 0; k < host.size(); ++k) {
            const int i = slot_of[k];
            const uint16_t rev = host[k].revents;
            if (rev) ++ready;
            std::memcpy(&fds[i * 8 + 6], &rev, 2);
        }
    } else if (!ready && timeout > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout));
    }
    buf_write(req.out_buf(0), fds.data(), fds.size());
    return reply(rsp, ready, 0);
}

// Select(u32 nfds, timeval timeout @8 {s64 sec, s64 usec}, u8 is_null @24, 3 x buffer<fd_set, 0x21>) -> (i32 ret, u32 bsd_errno,
// 3 x buffer<fd_set, 0x22>). fd_set: bit (fd & 7) of byte (fd >> 3).
uint32_t do_select(ipc::Request& req, ipc::Response& rsp) {
    const int32_t nfds = req.arg<int32_t>(0);
    const int64_t sec = req.arg<int64_t>(8), usec = req.arg<int64_t>(16);
    const bool is_null = req.arg<uint8_t>(24) != 0;
    int timeout = -1;
    if (!is_null && sec >= 0 && usec >= 0) {
        const int64_t ms = sec * 1000 + (usec + 999) / 1000;
        timeout = static_cast<int>(std::min<int64_t>(ms, 0x7FFFFFFF));
    }
    std::vector<uint8_t> set_in[3], set_out[3];
    for (int k = 0; k < 3; ++k) {
        const ipc::Buffer b = req.in_buf(static_cast<size_t>(k));
        set_in[k].assign(static_cast<size_t>(b.size), 0);
        buf_read(b, set_in[k].data(), b.size);
        set_out[k].assign(static_cast<size_t>(req.out_buf(static_cast<size_t>(k)).size), 0);
    }
    auto bit = [](const std::vector<uint8_t>& v, int fd) { return static_cast<size_t>(fd >> 3) < v.size() && (v[fd >> 3] >> (fd & 7)) & 1; };
    struct Entry { int fd; uint8_t want; };
    std::vector<hn::PollItem> host;
    std::vector<Entry> ent;
    std::vector<SockPtr> keep;
    for (int fd = 0; fd < nfds; ++fd) {
        const uint8_t want = (bit(set_in[0], fd) ? 1 : 0) | (bit(set_in[1], fd) ? 2 : 0) | (bit(set_in[2], fd) ? 4 : 0);
        if (!want) continue;
        SockPtr p = fd_get(fd);
        if (!p) return reply(rsp, -1, kEBADF);
        hn::PollItem w;
        w.s = p->s;
        if (want & 1) w.events |= kPollRdNorm;
        if (want & 2) w.events |= kPollOut;
        if (want & 4) w.events |= kPollRdBand;
        host.push_back(w);
        ent.push_back({fd, want});
        keep.push_back(std::move(p));
    }
    int count = 0;
    if (!host.empty()) {
        const int rc = hn::poll(host.data(), host.size(), timeout);
        if (rc < 0) return reply(rsp, -1, fail_errno(nullptr, -1, "select", rc));
        for (size_t k = 0; k < host.size(); ++k) {
            const uint16_t r = host[k].revents;
            const int fd = ent[k].fd;
            const bool err = (r & (kPollErr | kPollHup)) != 0;
            const bool hit[3] = {(ent[k].want & 1) && ((r & kPollRdNorm) || err), (ent[k].want & 2) && ((r & kPollOut) || err),
                                 (ent[k].want & 4) && ((r & kPollRdBand) || (r & kPollErr))};
            for (int s = 0; s < 3; ++s) {
                if (hit[s] && static_cast<size_t>(fd >> 3) < set_out[s].size()) {
                    set_out[s][fd >> 3] |= static_cast<uint8_t>(1u << (fd & 7));
                    ++count;
                }
            }
        }
    } else if (timeout > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout));
    }
    for (int k = 0; k < 3; ++k) buf_write(req.out_buf(static_cast<size_t>(k)), set_out[k].data(), set_out[k].size());
    return reply(rsp, count, 0);
}

uint32_t do_close(ipc::Request& req, ipc::Response& rsp) {
    return fd_close(req.arg<int32_t>(0)) ? reply(rsp, 0, 0) : reply(rsp, -1, kEBADF);
}

// DuplicateSocket(u32 fd, u64 reserved) -> (i32 ret, u32 bsd_errno): a second descriptor for the same host socket.
uint32_t do_dup(ipc::Request& req, ipc::Response& rsp) {
    SockPtr p = fd_get(req.arg<int32_t>(0));
    if (!p) return reply(rsp, -1, kEBADF);
    return reply(rsp, fd_add(std::move(p)), 0);
}

}

bool sockets_host() { return lower(trim(config_get_str("net", "sockets", "off"))) == "host"; }

void install_bsd(ipc::Service& s) {
    // [net] trace: every bsd command is logged with its first argument (the fd for most) and its (ret, errno) reply words.
    const bool trace = config_get_bool("net", "trace", false);
    auto add = [&s, trace](uint32_t id, const char* name, ipc::Handler h) {
        if (!trace) {
            s.add_cmd(id, name, std::move(h));
            return;
        }
        s.add_cmd(id, name, [id, name, h = std::move(h)](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            const int32_t a0 = req.arg<int32_t>(0);
            if (id == 5 || id == 6) {
                std::string fds;
                const ipc::Buffer in = req.in_buf(0);
                if (id == 6 && in.addr)
                    for (int i = 0; i < a0 && i < 16 && static_cast<uint64_t>(i) * 8 + 8 <= in.size; ++i) {
                        const uint8_t* e = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(in.addr)) + i * 8;
                        int32_t fd;
                        uint16_t ev;
                        std::memcpy(&fd, e, 4);
                        std::memcpy(&ev, e + 4, 2);
                        fds += " " + std::to_string(fd) + ":0x" + std::to_string(ev);
                    }
                NXLOG_I("net", "bsd %u %s(%d, timeout %d)%s ...", id, name, a0, req.arg<int32_t>(4), fds.c_str());
            }
            const uint32_t rc = h(req, rsp);
            int32_t w0 = 0, w1 = 0;
            if (rsp.out.size() >= 4) std::memcpy(&w0, rsp.out.data(), 4);
            if (rsp.out.size() >= 8) std::memcpy(&w1, rsp.out.data() + 4, 4);
            NXLOG_I("net", "bsd %u %s(%d) -> rc=0x%x out=[%d, %d] (%zu bytes)", id, name, a0, rc, w0, w1, rsp.out.size());
            return rc;
        });
    };
    hn::init();
    add(0, "RegisterClient", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push(uint32_t{0});   // bsd_errno; the transfer memory is not used
        return 0;
    });
    add(1, "StartMonitoring", [](ipc::Request&, ipc::Response&) -> uint32_t { return 0; });
    add(2, "Socket", do_socket);
    add(3, "SocketExempt", do_socket);
    add(5, "Select", do_select);
    add(6, "Poll", do_poll);
    add(8, "Recv", [](ipc::Request& q, ipc::Response& r) { return do_recv(q, r, false, false); });
    add(9, "RecvFrom", [](ipc::Request& q, ipc::Response& r) { return do_recv(q, r, true, false); });
    add(10, "Send", [](ipc::Request& q, ipc::Response& r) { return do_send(q, r, false, false); });
    add(11, "SendTo", [](ipc::Request& q, ipc::Response& r) { return do_send(q, r, true, false); });
    add(12, "Accept", do_accept);
    add(13, "Bind", do_bind);
    add(14, "Connect", do_connect);
    add(15, "GetPeerName", [](ipc::Request& q, ipc::Response& r) { return do_name(q, r, true); });
    add(16, "GetSockName", [](ipc::Request& q, ipc::Response& r) { return do_name(q, r, false); });
    add(17, "GetSockOpt", do_getsockopt);
    add(18, "Listen", do_listen);
    add(19, "Ioctl", do_ioctl);
    add(20, "Fcntl", do_fcntl);
    add(21, "SetSockOpt", do_setsockopt);
    add(22, "Shutdown", do_shutdown);
    add(23, "ShutdownAllSockets", do_shutdown_all);
    add(24, "Write", [](ipc::Request& q, ipc::Response& r) { return do_send(q, r, false, true); });
    add(25, "Read", [](ipc::Request& q, ipc::Response& r) { return do_recv(q, r, false, true); });
    add(26, "Close", do_close);
    add(27, "DuplicateSocket", do_dup);
}

bool resolve_ipv4(const std::string& name_in, uint8_t ip[4], const char** how) {
    const std::string name = lower(trim(name_in));
    if (name.empty()) return false;
    auto said = [&](const char* h) {
        if (how) *how = h;
        return true;
    };
    // [net] hosts: an exact name wins over a "*.suffix" wildcard; the longest wildcard wins among wildcards.
    size_t best = 0;
    std::string best_ip;
    for (const auto& [k, v] : pairs(config_get_str("net", "hosts", ""))) {
        const std::string key = lower(k);
        if (key == name) {
            if (parse_ipv4(v, ip)) return said("hosts");
        } else if (key.size() > 2 && key.compare(0, 2, "*.") == 0) {
            const std::string suffix = key.substr(1);
            if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0 && suffix.size() > best) {
                best = suffix.size();
                best_ip = v;
            }
        }
    }
    if (best && parse_ipv4(best_ip, ip)) return said("hosts");
    if (parse_ipv4(name, ip)) return said("literal");
    if (name == "localhost") {
        ip[0] = 127, ip[1] = 0, ip[2] = 0, ip[3] = 1;
        return said("hosts");
    }
    if (lower(trim(config_get_str("net", "dns", "hosts"))) != "host") return false;
    hn::init();
    return hn::resolve_ipv4(name, ip) ? said("dns") : false;
}

void reset() {
    std::vector<SockPtr> drop;
    {
        std::lock_guard<std::mutex> lk(tab().mu);
        drop.swap(tab().fds);
    }
}

}
