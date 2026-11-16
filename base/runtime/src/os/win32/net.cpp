// SPDX-License-Identifier: GPL-3.0-or-later
// nxrt::os::net on Windows: Winsock (include/nxrt/os/net.h). Winsock errors are mapped to Linux errno numbers (Ryujinx WinSockHelper._errorMap).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include "nxrt/os/net.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace nxrt::os::net {
namespace {

constexpr int kEINVAL = 22, kENOPROTOOPT = 92, kEAGAIN = 11, kEINPROGRESS = 115, kEAFNOSUPPORT = 97;

// Winsock error -> Linux errno, WSAEWOULDBLOCK -> EAGAIN.
int linux_errno(int wsa) {
    switch (wsa) {
        case 0: return 0;
        case WSAEINTR: return 4;
        case WSAEBADF: return 9;
        case WSAEACCES: return 13;
        case WSAEFAULT: return 14;
        case WSAEINVAL: return kEINVAL;
        case WSAEMFILE: return 24;
        case WSAEWOULDBLOCK: return kEAGAIN;
        case WSAEINPROGRESS: return kEINPROGRESS;
        case WSAEALREADY: return 114;
        case WSAENOTSOCK: return 88;
        case WSAEDESTADDRREQ: return 89;
        case WSAEMSGSIZE: return 90;
        case WSAEPROTOTYPE: return 91;
        case WSAENOPROTOOPT: return kENOPROTOOPT;
        case WSAEPROTONOSUPPORT: return 93;
        case WSAESOCKTNOSUPPORT: return 94;
        case WSAEOPNOTSUPP: return 95;
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

int last() { return -linux_errno(WSAGetLastError()); }

SOCKET sk(Socket s) { return static_cast<SOCKET>(s); }

sockaddr_in to_sa(const Ipv4& a) {
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(a.port);
    std::memcpy(&sa.sin_addr, a.ip, 4);
    return sa;
}
Ipv4 from_sa(const sockaddr_in& sa) {
    Ipv4 a;
    std::memcpy(a.ip, &sa.sin_addr, 4);
    a.port = ntohs(sa.sin_port);
    return a;
}

int host_flags(uint32_t f) {
    int h = 0;
    if (f & kMsgOob) h |= MSG_OOB;
    if (f & kMsgPeek) h |= MSG_PEEK;
    if (f & kMsgDontRoute) h |= MSG_DONTROUTE;
    if (f & kMsgWaitAll) h |= MSG_WAITALL;
    return h;
}

int clamp_len(size_t n) { return n > 0x7FFFFFFF ? 0x7FFFFFFF : static_cast<int>(n); }

// Option -> Winsock (level, name); false when the host has none.
bool host_opt(Level level, Opt opt, int* hl, int* hn) {
    switch (level) {
        case Level::Socket:
            *hl = SOL_SOCKET;
            switch (opt) {
                case Opt::Debug: *hn = SO_DEBUG; return true;
                case Opt::AcceptConn: *hn = SO_ACCEPTCONN; return true;
                case Opt::ReuseAddr: *hn = SO_REUSEADDR; return true;
                case Opt::ReusePort: *hn = SO_REUSEADDR; return true;   // Ryujinx maps it to ReuseAddress
                case Opt::KeepAlive: *hn = SO_KEEPALIVE; return true;
                case Opt::DontRoute: *hn = SO_DONTROUTE; return true;
                case Opt::Broadcast: *hn = SO_BROADCAST; return true;
                case Opt::UseLoopback: *hn = SO_USELOOPBACK; return true;
                case Opt::Linger: *hn = SO_LINGER; return true;
                case Opt::OobInline: *hn = SO_OOBINLINE; return true;
                case Opt::SndBuf: *hn = SO_SNDBUF; return true;
                case Opt::RcvBuf: *hn = SO_RCVBUF; return true;
                case Opt::SndLowat: *hn = SO_SNDLOWAT; return true;
                case Opt::RcvLowat: *hn = SO_RCVLOWAT; return true;
                case Opt::SndTimeo: *hn = SO_SNDTIMEO; return true;
                case Opt::RcvTimeo: *hn = SO_RCVTIMEO; return true;
                case Opt::Error: *hn = SO_ERROR; return true;
                case Opt::SockType: *hn = SO_TYPE; return true;
                default: return false;
            }
        case Level::Ip:
            *hl = IPPROTO_IP;
            switch (opt) {
                case Opt::IpOptions: *hn = IP_OPTIONS; return true;
                case Opt::IpHdrIncl: *hn = IP_HDRINCL; return true;
                case Opt::IpTos: *hn = IP_TOS; return true;
                case Opt::IpTtl: *hn = IP_TTL; return true;
                case Opt::IpMulticastIf: *hn = IP_MULTICAST_IF; return true;
                case Opt::IpMulticastTtl: *hn = IP_MULTICAST_TTL; return true;
                case Opt::IpMulticastLoop: *hn = IP_MULTICAST_LOOP; return true;
                case Opt::IpAddMembership: *hn = IP_ADD_MEMBERSHIP; return true;
                case Opt::IpDropMembership: *hn = IP_DROP_MEMBERSHIP; return true;
                case Opt::IpDontFragment: *hn = IP_DONTFRAGMENT; return true;
                default: return false;
            }
        case Level::Tcp:
            *hl = IPPROTO_TCP;
            switch (opt) {
                case Opt::TcpNoDelay: *hn = TCP_NODELAY; return true;
                case Opt::TcpMaxSeg: *hn = TCP_MAXSEG; return true;
                case Opt::TcpKeepIdle: *hn = TCP_KEEPIDLE; return true;
                case Opt::TcpKeepIntvl: *hn = TCP_KEEPINTVL; return true;
                case Opt::TcpKeepCnt: *hn = TCP_KEEPCNT; return true;
                default: return false;
            }
    }
    return false;
}

SHORT host_events(uint16_t e) {   // POLLPRI is not accepted by WSAPoll: urgent data is POLLRDBAND
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

}  // namespace

bool init() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        WSADATA d{};
        ok = WSAStartup(MAKEWORD(2, 2), &d) == 0;
    });
    return ok;
}

int open(Type type, int protocol, Socket* out) {
    init();
    const int t = type == Type::Stream ? SOCK_STREAM : SOCK_DGRAM;
    if (protocol == 0) protocol = type == Type::Stream ? IPPROTO_TCP : IPPROTO_UDP;
    SOCKET s = ::socket(AF_INET, t, protocol);
    if (s == INVALID_SOCKET) return last();
    *out = static_cast<Socket>(s);
    return 0;
}

void close(Socket s) {
    if (s != kInvalidSocket) closesocket(sk(s));
}

int set_nonblocking(Socket s, bool on) {
    u_long v = on ? 1 : 0;
    return ioctlsocket(sk(s), FIONBIO, &v) == 0 ? 0 : last();
}

int connect(Socket s, const Ipv4& to) {
    const sockaddr_in sa = to_sa(to);
    if (::connect(sk(s), reinterpret_cast<const sockaddr*>(&sa), sizeof sa) == 0) return 0;
    if (WSAGetLastError() == WSAEWOULDBLOCK) return -kEINPROGRESS;   // a non-blocking connect under way: BSD says EINPROGRESS
    return last();
}

int bind(Socket s, const Ipv4& at) {
    const sockaddr_in sa = to_sa(at);
    return ::bind(sk(s), reinterpret_cast<const sockaddr*>(&sa), sizeof sa) == 0 ? 0 : last();
}

int listen(Socket s, int backlog) { return ::listen(sk(s), backlog) == 0 ? 0 : last(); }

int accept(Socket s, Socket* out, Ipv4* peer) {
    sockaddr_in sa{};
    int len = sizeof sa;
    SOCKET n = ::accept(sk(s), reinterpret_cast<sockaddr*>(&sa), &len);
    if (n == INVALID_SOCKET) return last();
    *out = static_cast<Socket>(n);
    if (peer) *peer = from_sa(sa);
    return 0;
}

int local_addr(Socket s, Ipv4* out) {
    sockaddr_in sa{};
    int len = sizeof sa;
    if (::getsockname(sk(s), reinterpret_cast<sockaddr*>(&sa), &len) != 0) return last();
    *out = from_sa(sa);
    return 0;
}

int peer_addr(Socket s, Ipv4* out) {
    sockaddr_in sa{};
    int len = sizeof sa;
    if (::getpeername(sk(s), reinterpret_cast<sockaddr*>(&sa), &len) != 0) return last();
    *out = from_sa(sa);
    return 0;
}

int shutdown(Socket s, Shut how) { return ::shutdown(sk(s), static_cast<int>(how)) == 0 ? 0 : last(); }   // SD_RECEIVE/SEND/BOTH = 0/1/2

int bytes_available(Socket s, uint32_t* out) {
    u_long n = 0;
    if (::ioctlsocket(sk(s), FIONREAD, &n) != 0) return last();
    *out = static_cast<uint32_t>(n);
    return 0;
}

int64_t send(Socket s, const void* data, size_t n, uint32_t flags, const Ipv4* to) {
    const char* p = static_cast<const char*>(data);
    int r;
    if (to) {
        const sockaddr_in sa = to_sa(*to);
        r = ::sendto(sk(s), p, clamp_len(n), host_flags(flags), reinterpret_cast<const sockaddr*>(&sa), sizeof sa);
    } else {
        r = ::send(sk(s), p, clamp_len(n), host_flags(flags));
    }
    return r == SOCKET_ERROR ? last() : r;
}

int64_t recv(Socket s, void* data, size_t n, uint32_t flags, Ipv4* from) {
    char* p = static_cast<char*>(data);
    int r;
    sockaddr_in sa{};
    if (from) {
        int len = sizeof sa;
        r = ::recvfrom(sk(s), p, clamp_len(n), host_flags(flags), reinterpret_cast<sockaddr*>(&sa), &len);
    } else {
        r = ::recv(sk(s), p, clamp_len(n), host_flags(flags));
    }
    if (r == SOCKET_ERROR) {
        // A datagram longer than the buffer: BSD returns the part that fits, Winsock fails with WSAEMSGSIZE after filling the buffer.
        if (WSAGetLastError() != WSAEMSGSIZE) return last();
        r = clamp_len(n);
    }
    if (from) *from = from_sa(sa);
    return r;
}

int set_int(Socket s, Level level, Opt opt, int32_t value) {
    int hl, hn;
    if (!host_opt(level, opt, &hl, &hn)) return -kENOPROTOOPT;
    DWORD dv = static_cast<DWORD>(value);
    return ::setsockopt(sk(s), hl, hn, reinterpret_cast<const char*>(&dv), sizeof dv) == 0 ? 0 : last();
}

int get_int(Socket s, Level level, Opt opt, int32_t* value) {
    int hl, hn;
    if (!host_opt(level, opt, &hl, &hn)) return -kENOPROTOOPT;
    uint8_t raw[16] = {};
    int len = sizeof raw;
    if (::getsockopt(sk(s), hl, hn, reinterpret_cast<char*>(raw), &len) != 0) return last();
    int32_t v = 0;
    std::memcpy(&v, raw, std::min<int>(len, 4));
    if (len == 1) v = raw[0];
    if (level == Level::Socket && opt == Opt::Error) v = linux_errno(v);   // the pending Winsock error as a Linux errno
    *value = v;
    return 0;
}

int set_timeout_ms(Socket s, Opt opt, int64_t ms) {
    int hl, hn;
    if (!host_opt(Level::Socket, opt, &hl, &hn)) return -kENOPROTOOPT;
    DWORD v = ms <= 0 ? 0 : static_cast<DWORD>(std::min<int64_t>(ms, 0x7FFFFFFF));
    return ::setsockopt(sk(s), hl, hn, reinterpret_cast<const char*>(&v), sizeof v) == 0 ? 0 : last();
}

int get_timeout_ms(Socket s, Opt opt, int64_t* ms) {
    int hl, hn;
    if (!host_opt(Level::Socket, opt, &hl, &hn)) return -kENOPROTOOPT;
    DWORD v = 0;
    int len = sizeof v;
    if (::getsockopt(sk(s), hl, hn, reinterpret_cast<char*>(&v), &len) != 0) return last();
    *ms = v;
    return 0;
}

int set_linger(Socket s, bool on, int seconds) {
    linger l{static_cast<u_short>(on ? 1 : 0), static_cast<u_short>(std::clamp(seconds, 0, 0xFFFF))};
    return ::setsockopt(sk(s), SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&l), sizeof l) == 0 ? 0 : last();
}

int get_linger(Socket s, bool* on, int* seconds) {
    linger l{};
    int len = sizeof l;
    if (::getsockopt(sk(s), SOL_SOCKET, SO_LINGER, reinterpret_cast<char*>(&l), &len) != 0) return last();
    *on = l.l_onoff != 0;
    *seconds = l.l_linger;
    return 0;
}

int set_membership(Socket s, Opt opt, const uint8_t g[8]) {
    int hl, hn;
    if (!host_opt(Level::Ip, opt, &hl, &hn)) return -kENOPROTOOPT;
    return ::setsockopt(sk(s), hl, hn, reinterpret_cast<const char*>(g), 8) == 0 ? 0 : last();
}

int poll(PollItem* items, size_t count, int timeout_ms) {
    std::vector<WSAPOLLFD> host(count);
    for (size_t i = 0; i < count; ++i) {
        host[i] = {};
        host[i].fd = sk(items[i].s);
        host[i].events = host_events(items[i].events);
    }
    const int rc = WSAPoll(host.data(), static_cast<ULONG>(count), timeout_ms);
    if (rc == SOCKET_ERROR) return last();
    int ready = 0;
    for (size_t i = 0; i < count; ++i) {
        items[i].revents = bsd_revents(host[i].revents, items[i].events);
        if (items[i].revents) ++ready;
    }
    return ready;
}

bool resolve_ipv4(const std::string& name, uint8_t ip[4]) {
    init();
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
    return ok;
}

}  // namespace nxrt::os::net
