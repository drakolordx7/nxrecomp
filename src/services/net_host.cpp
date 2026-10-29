// SPDX-License-Identifier: GPL-3.0-or-later
// Host sockets for bsd (nn::socket::sf::IClient) when [net] sockets = host: guest socket calls run on real Winsock sockets, so a title
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
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include "net_host.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nxrt/services.h"

namespace nxrt::services::nethost {
namespace {

// ---- Linux errno numbers (Ryujinx LinuxError.cs) ----------------------------------------------------------------------------------------
constexpr uint32_t kEBADF = 9, kEAGAIN = 11, kEFAULT = 14, kEINVAL = 22, kEMFILE = 24, kENOTSOCK = 88,
                   kENOPROTOOPT = 92, kEOPNOTSUPP = 95, kEAFNOSUPPORT = 97, kEINPROGRESS = 115;

// Winsock error -> Linux errno (Ryujinx WinSockHelper._errorMap), WSAEWOULDBLOCK -> EAGAIN.
uint32_t linux_errno(int wsa) {
    switch (wsa) {
        case 0: return 0;
        case WSAEINTR: return 4;
        case WSAEBADF: return kEBADF;
        case WSAEACCES: return 13;
        case WSAEFAULT: return kEFAULT;
        case WSAEINVAL: return kEINVAL;
        case WSAEMFILE: return kEMFILE;
        case WSAEWOULDBLOCK: return kEAGAIN;
        case WSAEINPROGRESS: return kEINPROGRESS;
        case WSAEALREADY: return 114;
        case WSAENOTSOCK: return kENOTSOCK;
        case WSAEDESTADDRREQ: return 89;
        case WSAEMSGSIZE: return 90;
        case WSAEPROTOTYPE: return 91;
        case WSAENOPROTOOPT: return kENOPROTOOPT;
        case WSAEPROTONOSUPPORT: return 93;
        case WSAESOCKTNOSUPPORT: return 94;
        case WSAEOPNOTSUPP: return kEOPNOTSUPP;
        case WSAEPFNOSUPPORT: return 96;
        case WSAEAFNOSUPPORT: return kEAFNOSUPPORT;
        case WSAEADDRINUSE: return 98;
        case WSAEADDRNOTAVAIL: return 99;
        case WSAENETDOWN: return 100;
        case WSAENETUNREACH: return 101;
        case WSAENETRESET: return 102;
        case WSAECONNABORTED: return 103;
        case WSAECONNRESET: return 104;
        case WSAENOBUFS: return 105;
        case WSAEISCONN: return 106;
        case WSAENOTCONN: return 107;
        case WSAESHUTDOWN: return 108;
        case WSAETOOMANYREFS: return 109;
        case WSAETIMEDOUT: return 110;
        case WSAECONNREFUSED: return 111;
        case WSAELOOP: return 40;
        case WSAENAMETOOLONG: return 36;
        case WSAEHOSTDOWN: return 112;
        case WSAEHOSTUNREACH: return 113;
        case WSAENOTEMPTY: return 39;
        case WSAEUSERS: return 87;
        case WSAEDQUOT: return 122;
        case WSAESTALE: return 116;
        case WSAEREMOTE: return 66;
        default: return kEINVAL;
    }
}

// ---- BSD constants (Ryujinx Bsd/Types) ---------------------------------------------------------------------------------------------------
constexpr int kBsdAfInet = 2;
constexpr uint32_t kBsdTypeMask = 0x0FFFFFFF, kBsdNonBlockingFlag = 2, kBsdFlagsShift = 28;   // BsdSocketCreationFlags
constexpr uint32_t kMsgOob = 0x1, kMsgPeek = 0x2, kMsgDontRoute = 0x4, kMsgWaitAll = 0x40, kMsgDontWait = 0x80;
constexpr uint16_t kPollIn = 1, kPollPri = 2, kPollOut = 4, kPollErr = 8, kPollHup = 0x10, kPollNval = 0x20, kPollRdNorm = 0x40,
                   kPollRdBand = 0x80, kPollWrBand = 0x100;
constexpr uint32_t kFionread = 0x4004667F, kFionbio = 0x8004667E, kSiocatmark = 0x40047307;   // FreeBSD _IOR/_IOW('f', 127/126), ('s', 7)
constexpr int kFGetFl = 3, kFSetFl = 4, kONonBlock = 0x800;
constexpr int kSolSocket = 0xFFFF, kIpprotoIp = 0, kIpprotoTcp = 6;

// ---- config ------------------------------------------------------------------------------------------------------------------------------
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}
// "k=v;k2=v2" -> pairs (empty items skipped).
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
    in_addr a{};
    if (inet_pton(AF_INET, s.c_str(), &a) != 1) return false;
    std::memcpy(ip, &a, 4);
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

void wsa_start() {
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA d{};
        const int rc = WSAStartup(MAKEWORD(2, 2), &d);
        if (rc != 0) NXLOG_E("net", "WSAStartup failed: %d", rc);
    });
}

// ---- descriptor table ------------------------------------------------------------------------------------------------------------------
struct HostSock {
    SOCKET s = INVALID_SOCKET;
    bool nonblock = false;
    bool connect_logged = false;
    std::set<std::string> errors_logged;   // "<cmd> <errno>"
    ~HostSock() {
        if (s != INVALID_SOCKET) closesocket(s);
    }
};
using SockPtr = std::shared_ptr<HostSock>;

struct Table {
    std::mutex mu;
    std::vector<SockPtr> fds;   // index = guest descriptor; null = free
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
    SockPtr p;   // released outside the lock (closesocket on the last reference)
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

void log_error(const SockPtr& p, int fd, const char* what, uint32_t err, int wsa) {
    if (err == 0 || err == kEAGAIN || err == kEINPROGRESS) return;
    if (p) {
        std::lock_guard<std::mutex> lk(tab().mu);
        if (!p->errors_logged.insert(std::string(what) + " " + std::to_string(err)).second) return;
    }
    NXLOG_I("net", "bsd %s fd=%d: errno %u (WSA %d)", what, fd, err, wsa);
}

// The errno of the last Winsock call, logged when it is a real error.
uint32_t last_error(const SockPtr& p, int fd, const char* what) {
    const int wsa = WSAGetLastError();
    const uint32_t e = linux_errno(wsa);
    log_error(p, fd, what, e, wsa);
    return e;
}

// ---- sockaddr ---------------------------------------------------------------------------------------------------------------------------
// BSD sockaddr_in: u8 sin_len, u8 sin_family, u16 sin_port (big-endian), u8 sin_addr[4], u8 zero[8].
bool read_sockaddr(const ipc::Buffer& b, sockaddr_in& out) {
    uint8_t raw[16] = {};
    if (!b.addr || b.size < 8) return false;
    buf_read(b, raw, 16);
    if (raw[1] != kBsdAfInet && raw[1] != 0) return false;
    out = {};
    out.sin_family = AF_INET;
    std::memcpy(&out.sin_port, raw + 2, 2);
    std::memcpy(&out.sin_addr, raw + 4, 4);
    return true;
}
void write_sockaddr(const ipc::Buffer& b, const sockaddr_in& a) {
    uint8_t raw[16] = {};
    raw[0] = 16;
    raw[1] = kBsdAfInet;
    std::memcpy(raw + 2, &a.sin_port, 2);
    std::memcpy(raw + 4, &a.sin_addr, 4);
    buf_write(b, raw, 16);
}
std::string addr_text(const sockaddr_in& a) {
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof ip);
    return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}

int host_msg_flags(uint32_t f) {
    int h = 0;
    if (f & kMsgOob) h |= MSG_OOB;
    if (f & kMsgPeek) h |= MSG_PEEK;
    if (f & kMsgDontRoute) h |= MSG_DONTROUTE;
    if (f & kMsgWaitAll) h |= MSG_WAITALL;
    return h;
}

void set_nonblock(SOCKET s, bool on) {
    u_long v = on ? 1 : 0;
    ioctlsocket(s, FIONBIO, &v);
}

// MSG_DONTWAIT on a blocking socket: non-blocking for this one call (Ryujinx ManagedSocket.Receive).
struct DontWait {
    SOCKET s;
    bool active;
    DontWait(const HostSock& h, uint32_t flags) : s(h.s), active(!h.nonblock && (flags & kMsgDontWait)) {
        if (active) set_nonblock(s, true);
    }
    ~DontWait() {
        if (active) set_nonblock(s, false);
    }
};

int clamp_len(uint64_t n) { return n > 0x7FFFFFFF ? 0x7FFFFFFF : static_cast<int>(n); }

// ---- commands ---------------------------------------------------------------------------------------------------------------------------

uint32_t do_socket(ipc::Request& req, ipc::Response& rsp) {
    wsa_start();
    const int32_t domain = req.arg<int32_t>(0);
    const uint32_t type_raw = req.arg<uint32_t>(4);
    int32_t protocol = req.arg<int32_t>(8);
    const uint32_t flags = type_raw >> kBsdFlagsShift;
    const int type = static_cast<int>(type_raw & kBsdTypeMask);
    if (domain != kBsdAfInet && domain != 0) return reply(rsp, -1, kEAFNOSUPPORT);   // IPv4 only (the sockaddr layout here is sockaddr_in)
    if (protocol == 0) protocol = type == SOCK_STREAM ? IPPROTO_TCP : (type == SOCK_DGRAM ? IPPROTO_UDP : 0);
    SOCKET s = ::socket(AF_INET, type, protocol);
    if (s == INVALID_SOCKET) return reply(rsp, -1, last_error(nullptr, -1, "socket"));
    auto p = std::make_shared<HostSock>();
    p->s = s;
    if (flags & kBsdNonBlockingFlag) {
        p->nonblock = true;
        set_nonblock(s, true);
    }
    return reply(rsp, fd_add(std::move(p)), 0);
}

uint32_t do_connect(ipc::Request& req, ipc::Response& rsp) {
    const int fd = req.arg<int32_t>(0);
    SockPtr p = fd_get(fd);
    if (!p) return reply(rsp, -1, kEBADF);
    sockaddr_in a{};
    if (!read_sockaddr(req.in_buf(0), a)) return reply(rsp, -1, kEAFNOSUPPORT);
    const uint16_t asked = ntohs(a.sin_port);
    const uint16_t port = map_port(asked);
    a.sin_port = htons(port);
    bool log_it = false;
    {
        std::lock_guard<std::mutex> lk(tab().mu);
        if (!p->connect_logged) log_it = p->connect_logged = true;
    }
    if (log_it) {
        if (port != asked) NXLOG_I("net", "bsd connect fd=%d -> %s (mapped from %u)", fd, addr_text(a).c_str(), asked);
        else NXLOG_I("net", "bsd connect fd=%d -> %s", fd, addr_text(a).c_str());
    }
    if (::connect(p->s, reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0) return reply(rsp, 0, 0);
    const int wsa = WSAGetLastError();
    uint32_t e = linux_errno(wsa);
    if (wsa == WSAEWOULDBLOCK) e = kEINPROGRESS;   // a non-blocking connect under way: BSD says EINPROGRESS
    log_error(p, fd, "connect", e, wsa);
    return reply(rsp, -1, e);
}

uint32_t do_bind(ipc::Request& req, ipc::Response& rsp) {
    const int fd = req.arg<int32_t>(0);
    SockPtr p = fd_get(fd);
    if (!p) return reply(rsp, -1, kEBADF);
    sockaddr_in a{};
    if (!read_sockaddr(req.in_buf(0), a)) return reply(rsp, -1, kEAFNOSUPPORT);
    if (::bind(p->s, reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0) return reply(rsp, 0, 0);
    return reply(rsp, -1, last_error(p, fd, "bind"));
}

uint32_t do_listen(ipc::Request& req, ipc::Response& rsp) {
    const int fd = req.arg<int32_t>(0);
    SockPtr p = fd_get(fd);
    if (!p) return reply(rsp, -1, kEBADF);
    if (::listen(p->s, req.arg<int32_t>(4)) == 0) return reply(rsp, 0, 0);
    return reply(rsp, -1, last_error(p, fd, "listen"));
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
    sockaddr_in a{};
    int len = sizeof a;
    SOCKET s = ::accept(p->s, reinterpret_cast<sockaddr*>(&a), &len);
    if (s == INVALID_SOCKET) {
        reply(rsp, -1, last_error(p, fd, "accept"));
        rsp.push(uint32_t{0});
        return 0;
    }
    auto n = std::make_shared<HostSock>();
    n->s = s;
    n->nonblock = p->nonblock;   // Winsock accepted sockets keep the listener's mode (as FreeBSD does)
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
    sockaddr_in a{};
    int len = sizeof a;
    const int rc = peer ? ::getpeername(p->s, reinterpret_cast<sockaddr*>(&a), &len) : ::getsockname(p->s, reinterpret_cast<sockaddr*>(&a), &len);
    if (rc != 0) {
        const int wsa = WSAGetLastError();
        if (!peer && wsa == WSAEINVAL) {   // not bound yet: the any address, port 0 (Ryujinx writes an empty sockaddr)
            a = {};
            a.sin_family = AF_INET;
        } else {
            const uint32_t e = linux_errno(wsa);
            log_error(p, fd, peer ? "getpeername" : "getsockname", e, wsa);
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
    const char* ptr = reinterpret_cast<const char*>(static_cast<uintptr_t>(data.addr));
    DontWait dw(*p, flags);
    int n;
    if (to) {
        sockaddr_in a{};
        if (!read_sockaddr(req.in_buf(1), a)) return reply(rsp, -1, kEAFNOSUPPORT);
        a.sin_port = htons(map_port(ntohs(a.sin_port)));
        n = ::sendto(p->s, ptr, clamp_len(data.size), host_msg_flags(flags), reinterpret_cast<const sockaddr*>(&a), sizeof a);
    } else {
        n = ::send(p->s, ptr, clamp_len(data.size), host_msg_flags(flags));
    }
    if (n == SOCKET_ERROR) return reply(rsp, -1, last_error(p, fd, to ? "sendto" : "send"));
    return reply(rsp, n, 0);
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
    char* ptr = reinterpret_cast<char*>(static_cast<uintptr_t>(data.addr));
    DontWait dw(*p, flags);
    int n;
    sockaddr_in a{};
    if (from) {
        int len = sizeof a;
        n = ::recvfrom(p->s, ptr, clamp_len(data.size), host_msg_flags(flags), reinterpret_cast<sockaddr*>(&a), &len);
    } else {
        n = ::recv(p->s, ptr, clamp_len(data.size), host_msg_flags(flags));
    }
    if (n == SOCKET_ERROR) {
        // A datagram larger than the buffer: BSD returns the truncated part, Winsock fails with WSAEMSGSIZE after filling the buffer.
        if (WSAGetLastError() == WSAEMSGSIZE) {
            n = clamp_len(data.size);
        } else {
            return fail(last_error(p, fd, from ? "recvfrom" : "recv"));
        }
    }
    reply(rsp, n, 0);
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

// Option (level, BSD name) -> Winsock name. -1: unknown (ENOPROTOOPT), -2: accepted and ignored.
int host_opt(int level, int name) {
    if (level == kSolSocket) {
        switch (name) {
            case 0x1: case 0x2: case 0x4: case 0x8: case 0x10: case 0x20: case 0x40: case 0x80: case 0x100:   // DEBUG..OOBINLINE: same values
            case 0x1001: case 0x1002: case 0x1003: case 0x1004: case 0x1005: case 0x1006: case 0x1007: case 0x1008:
                return name;
            case 0x200: return SO_REUSEADDR;   // SO_REUSEPORT (Ryujinx maps it to ReuseAddress)
            case 0x800: return -2;             // SO_NOSIGPIPE: Winsock raises no signal
            default: return -1;
        }
    }
    if (level == kIpprotoIp) {
        switch (name) {
            case 1: case 2: case 3: case 4: case 9: case 10: case 11: case 12: case 13: return name;   // OPTIONS..DROP_MEMBERSHIP: same values
            case 67: return IP_DONTFRAGMENT;
            default: return -1;
        }
    }
    if (level == kIpprotoTcp) {
        switch (name) {
            case 1: return TCP_NODELAY;
            case 2: return TCP_MAXSEG;
            case 0x100: return TCP_KEEPIDLE;
            case 0x200: return TCP_KEEPINTVL;
            case 0x400: return TCP_KEEPCNT;
            case 4: return -2;   // TCP_NOPUSH
            default: return -1;
        }
    }
    return -1;
}
bool is_timeo(int level, int name) { return level == kSolSocket && (name == 0x1005 || name == 0x1006); }
bool is_linger(int level, int name) { return level == kSolSocket && name == 0x80; }

uint32_t do_setsockopt(ipc::Request& req, ipc::Response& rsp) {
    const int fd = req.arg<int32_t>(0);
    const int level = req.arg<int32_t>(4), name = req.arg<int32_t>(8);
    SockPtr p = fd_get(fd);
    if (!p) return reply(rsp, -1, kEBADF);
    const int hname = host_opt(level, name);
    if (hname == -2) return reply(rsp, 0, 0);
    if (hname == -1) {
        log_error(p, fd, "setsockopt", kENOPROTOOPT, 0);
        return reply(rsp, -1, kENOPROTOOPT);
    }
    const ipc::Buffer b = req.in_buf(0);
    uint8_t raw[16] = {};
    const uint64_t got = buf_read(b, raw, sizeof raw);
    int rc;
    if (is_timeo(level, name)) {   // BSD struct timeval { s64 sec; s64 usec } -> DWORD milliseconds
        int64_t sec = 0, usec = 0;
        std::memcpy(&sec, raw, 8);
        std::memcpy(&usec, raw + 8, 8);
        const int64_t ms64 = sec * 1000 + usec / 1000;
        DWORD ms = ms64 <= 0 ? 0 : static_cast<DWORD>(std::min<int64_t>(ms64, 0x7FFFFFFF));
        if (ms == 0 && (sec > 0 || usec > 0)) ms = 1;
        rc = ::setsockopt(p->s, SOL_SOCKET, hname, reinterpret_cast<const char*>(&ms), sizeof ms);
    } else if (is_linger(level, name)) {   // BSD { int l_onoff; int l_linger } -> Winsock { u_short, u_short }
        int32_t on = 0, secs = 0;
        std::memcpy(&on, raw, 4);
        std::memcpy(&secs, raw + 4, 4);
        linger l{static_cast<u_short>(on ? 1 : 0), static_cast<u_short>(std::clamp<int32_t>(secs, 0, 0xFFFF))};
        rc = ::setsockopt(p->s, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&l), sizeof l);
    } else if (level == kIpprotoIp && (name == 12 || name == 13)) {   // ip_mreq: two in_addr, same layout
        rc = ::setsockopt(p->s, IPPROTO_IP, hname, reinterpret_cast<const char*>(raw), 8);
    } else {
        int32_t v = 0;
        if (got >= 4) std::memcpy(&v, raw, 4);
        else if (got >= 1) v = raw[0];   // u_char options (IP_MULTICAST_TTL / LOOP)
        DWORD dv = static_cast<DWORD>(v);
        rc = ::setsockopt(p->s, level == kSolSocket ? SOL_SOCKET : level, hname, reinterpret_cast<const char*>(&dv), sizeof dv);
    }
    if (rc == 0) return reply(rsp, 0, 0);
    return reply(rsp, -1, last_error(p, fd, "setsockopt"));
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
    const int hname = host_opt(level, name);
    if (hname == -2) {   // ignored option: reads back as 0
        const int32_t z = 0;
        buf_write(out, &z, 4);
        reply(rsp, 0, 0);
        rsp.push(uint32_t{4});
        return 0;
    }
    if (hname == -1) {
        log_error(p, fd, "getsockopt", kENOPROTOOPT, 0);
        return fail(kENOPROTOOPT);
    }
    const int hlevel = level == kSolSocket ? SOL_SOCKET : level;
    uint8_t raw[16] = {};
    int len = sizeof raw;
    if (::getsockopt(p->s, hlevel, hname, reinterpret_cast<char*>(raw), &len) != 0) return fail(last_error(p, fd, "getsockopt"));
    uint8_t res[16] = {};
    uint32_t res_len = 4;
    if (is_timeo(level, name)) {
        DWORD ms = 0;
        std::memcpy(&ms, raw, 4);
        const int64_t sec = ms / 1000, usec = (ms % 1000) * 1000;
        std::memcpy(res, &sec, 8);
        std::memcpy(res + 8, &usec, 8);
        res_len = 16;
    } else if (is_linger(level, name)) {
        linger l{};
        std::memcpy(&l, raw, sizeof l);
        const int32_t on = l.l_onoff ? 1 : 0, secs = l.l_linger;
        std::memcpy(res, &on, 4);
        std::memcpy(res + 4, &secs, 4);
        res_len = 8;
    } else if (level == kSolSocket && name == 0x1007) {   // SO_ERROR: the pending Winsock error as a Linux errno
        int32_t wsa = 0;
        std::memcpy(&wsa, raw, 4);
        const int32_t e = static_cast<int32_t>(linux_errno(wsa));
        std::memcpy(res, &e, 4);
    } else {
        int32_t v = 0;
        std::memcpy(&v, raw, std::min<int>(len, 4));
        if (len == 1) v = raw[0];
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
        set_nonblock(p->s, p->nonblock);
        return reply(rsp, 0, 0);
    }
    if (request == kFionread) {
        u_long n = 0;
        if (::ioctlsocket(p->s, FIONREAD, &n) != 0) return reply(rsp, -1, last_error(p, fd, "ioctl"));
        const int32_t v = static_cast<int32_t>(std::min<u_long>(n, 0x7FFFFFFF));
        buf_write(req.out_buf(0), &v, 4);
        return reply(rsp, 0, 0);
    }
    if (request == kSiocatmark) {   // no out-of-band data (Ryujinx answers 0 too)
        const int32_t z = 0;
        buf_write(req.out_buf(0), &z, 4);
        return reply(rsp, 0, 0);
    }
    log_error(p, fd, "ioctl", kEOPNOTSUPP, 0);
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
        set_nonblock(p->s, p->nonblock);
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
    if (::shutdown(p->s, how) == 0) return reply(rsp, 0, 0);   // SHUT_RD/WR/RDWR = SD_RECEIVE/SEND/BOTH
    return reply(rsp, -1, last_error(p, fd, "shutdown"));
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
    for (const SockPtr& p : all) ::shutdown(p->s, how);
    return reply(rsp, 0, 0);
}

// BSD poll events -> WSAPoll events (POLLPRI is not accepted by WSAPoll; urgent data maps to POLLRDBAND).
SHORT host_events(uint16_t e) {
    SHORT h = 0;
    if (e & (kPollIn | kPollRdNorm)) h |= POLLRDNORM;
    if (e & (kPollPri | kPollRdBand)) h |= POLLRDBAND;
    if (e & kPollOut) h |= POLLWRNORM;
    if (e & kPollWrBand) h |= POLLWRBAND;
    return h;
}
uint16_t bsd_revents(SHORT r, uint16_t asked) {
    uint16_t b = 0;
    if (r & POLLRDNORM) b |= asked & (kPollIn | kPollRdNorm);
    if (r & POLLRDBAND) b |= asked & (kPollPri | kPollRdBand);
    if (r & POLLWRNORM) b |= kPollOut;
    if (r & POLLWRBAND) b |= asked & kPollWrBand;
    if (r & POLLERR) b |= kPollErr;
    if (r & POLLHUP) b |= kPollHup;
    if (r & POLLNVAL) b |= kPollNval;
    return b;
}

// Poll(u32 nfds, i32 timeout_ms, buffer<pollfd, 0x21>) -> (i32 ret, u32 bsd_errno, buffer<pollfd, 0x22>). pollfd = {i32 fd, u16 events, u16 revents}.
uint32_t do_poll(ipc::Request& req, ipc::Response& rsp) {
    const int32_t nfds = req.arg<int32_t>(0);
    const int32_t timeout = req.arg<int32_t>(4);
    const ipc::Buffer in = req.in_buf(0);
    if (nfds < 0 || timeout < -1 || static_cast<uint64_t>(nfds) * 8 > in.size) return reply(rsp, -1, kEINVAL);
    std::vector<uint8_t> fds(static_cast<size_t>(nfds) * 8);
    buf_read(in, fds.data(), fds.size());
    std::vector<WSAPOLLFD> host;
    std::vector<int> slot_of;          // host entry -> pollfd index
    std::vector<SockPtr> keep;         // the sockets stay open while polled
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
                WSAPOLLFD w{};
                w.fd = p->s;
                w.events = host_events(ev);
                host.push_back(w);
                slot_of.push_back(i);
                keep.push_back(std::move(p));
            }
        }
        std::memcpy(&fds[i * 8 + 6], &rev, 2);
    }
    if (!host.empty()) {
        const int rc = WSAPoll(host.data(), static_cast<ULONG>(host.size()), ready ? 0 : timeout);
        if (rc == SOCKET_ERROR) return reply(rsp, -1, last_error(nullptr, -1, "poll"));
        for (size_t k = 0; k < host.size(); ++k) {
            const int i = slot_of[k];
            uint16_t ev;
            std::memcpy(&ev, &fds[i * 8 + 4], 2);
            const uint16_t rev = bsd_revents(host[k].revents, ev);
            if (rev) ++ready;
            std::memcpy(&fds[i * 8 + 6], &rev, 2);
        }
    } else if (!ready && timeout > 0) {
        Sleep(static_cast<DWORD>(timeout));
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
    std::vector<WSAPOLLFD> host;
    std::vector<Entry> ent;
    std::vector<SockPtr> keep;
    for (int fd = 0; fd < nfds; ++fd) {
        const uint8_t want = (bit(set_in[0], fd) ? 1 : 0) | (bit(set_in[1], fd) ? 2 : 0) | (bit(set_in[2], fd) ? 4 : 0);
        if (!want) continue;
        SockPtr p = fd_get(fd);
        if (!p) return reply(rsp, -1, kEBADF);
        WSAPOLLFD w{};
        w.fd = p->s;
        if (want & 1) w.events |= POLLRDNORM;
        if (want & 2) w.events |= POLLWRNORM;
        if (want & 4) w.events |= POLLRDBAND;
        host.push_back(w);
        ent.push_back({fd, want});
        keep.push_back(std::move(p));
    }
    int count = 0;
    if (!host.empty()) {
        const int rc = WSAPoll(host.data(), static_cast<ULONG>(host.size()), timeout);
        if (rc == SOCKET_ERROR) return reply(rsp, -1, last_error(nullptr, -1, "select"));
        for (size_t k = 0; k < host.size(); ++k) {
            const SHORT r = host[k].revents;
            const int fd = ent[k].fd;
            const bool err = (r & (POLLERR | POLLHUP)) != 0;
            const bool hit[3] = {(ent[k].want & 1) && ((r & POLLRDNORM) || err), (ent[k].want & 2) && ((r & POLLWRNORM) || err),
                                 (ent[k].want & 4) && ((r & POLLRDBAND) || (r & POLLERR))};
            for (int s = 0; s < 3; ++s) {
                if (hit[s] && static_cast<size_t>(fd >> 3) < set_out[s].size()) {
                    set_out[s][fd >> 3] |= static_cast<uint8_t>(1u << (fd & 7));
                    ++count;
                }
            }
        }
    } else if (timeout > 0) {
        Sleep(static_cast<DWORD>(timeout));
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

}  // namespace

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
            if (id == 5 || id == 6) {   // Select / Poll may block: log the entry too, with the fd list for Poll
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
    wsa_start();
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
            const std::string suffix = key.substr(1);   // ".example.com"
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
    wsa_start();
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    if (getaddrinfo(name.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    bool ok = false;
    for (addrinfo* a = res; a; a = a->ai_next) {
        if (a->ai_family == AF_INET && a->ai_addrlen >= sizeof(sockaddr_in)) {
            std::memcpy(ip, &reinterpret_cast<const sockaddr_in*>(a->ai_addr)->sin_addr, 4);
            ok = true;
            break;
        }
    }
    freeaddrinfo(res);
    return ok ? said("dns") : false;
}

void reset() {
    std::vector<SockPtr> drop;
    {
        std::lock_guard<std::mutex> lk(tab().mu);
        drop.swap(tab().fds);
    }
}

}  // namespace nxrt::services::nethost
