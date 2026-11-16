// SPDX-License-Identifier: GPL-3.0-or-later
// nxrt::os::net on Linux: BSD sockets (include/nxrt/os/net.h). Linux errno numbers are the ones the guest sees, so errno passes through.
#include "nxrt/os/net.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <vector>

namespace nxrt::os::net {
namespace {

constexpr int kENOPROTOOPT = 92;

int last() { return -errno; }
int fd(Socket s) { return static_cast<int>(s); }

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
    int h = MSG_NOSIGNAL;   // a closed peer is an error (EPIPE), not a signal
    if (f & kMsgOob) h |= MSG_OOB;
    if (f & kMsgPeek) h |= MSG_PEEK;
    if (f & kMsgDontRoute) h |= MSG_DONTROUTE;
    if (f & kMsgWaitAll) h |= MSG_WAITALL;
    return h;
}

bool host_opt(Level level, Opt opt, int* hl, int* hn) {
    switch (level) {
        case Level::Socket:
            *hl = SOL_SOCKET;
            switch (opt) {
                case Opt::Debug: *hn = SO_DEBUG; return true;
                case Opt::AcceptConn: *hn = SO_ACCEPTCONN; return true;
                case Opt::ReuseAddr: *hn = SO_REUSEADDR; return true;
                case Opt::ReusePort: *hn = SO_REUSEADDR; return true;   // as on Windows (Ryujinx maps it to ReuseAddress)
                case Opt::KeepAlive: *hn = SO_KEEPALIVE; return true;
                case Opt::DontRoute: *hn = SO_DONTROUTE; return true;
                case Opt::Broadcast: *hn = SO_BROADCAST; return true;
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
                default: return false;   // UseLoopback: no such option on Linux
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
                case Opt::IpDontFragment: *hn = IP_MTU_DISCOVER; return true;   // value mapped in set_int / get_int
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

short host_events(uint16_t e) {
    short h = 0;
    if (e & (kPollIn | kPollRdNorm)) h |= POLLIN;
    if (e & (kPollPri | kPollRdBand)) h |= POLLPRI;
    if (e & kPollOut) h |= POLLOUT;
    if (e & kPollWrBand) h |= POLLWRBAND;
    return h;
}
uint16_t bsd_revents(short r, uint16_t asked) {
    uint16_t b = 0;
    if (r & (POLLIN | POLLRDNORM)) b |= asked & (kPollIn | kPollRdNorm);
    if (r & (POLLPRI | POLLRDBAND)) b |= asked & (kPollPri | kPollRdBand);
    if (r & (POLLOUT | POLLWRNORM)) b |= kPollOut;
    if (r & POLLWRBAND) b |= asked & kPollWrBand;
    if (r & POLLERR) b |= kPollErr;
    if (r & POLLHUP) b |= kPollHup;
    if (r & POLLNVAL) b |= kPollNval;
    return b;
}

}  // namespace

bool init() { return true; }

int open(Type type, int protocol, Socket* out) {
    const int t = (type == Type::Stream ? SOCK_STREAM : SOCK_DGRAM) | SOCK_CLOEXEC;
    if (protocol == 0) protocol = type == Type::Stream ? IPPROTO_TCP : IPPROTO_UDP;
    const int s = ::socket(AF_INET, t, protocol);
    if (s < 0) return last();
    *out = s;
    return 0;
}

void close(Socket s) {
    if (s != kInvalidSocket) ::close(fd(s));
}

int set_nonblocking(Socket s, bool on) {
    const int fl = ::fcntl(fd(s), F_GETFL, 0);
    if (fl < 0) return last();
    return ::fcntl(fd(s), F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK)) == 0 ? 0 : last();
}

int connect(Socket s, const Ipv4& to) {
    const sockaddr_in sa = to_sa(to);
    return ::connect(fd(s), reinterpret_cast<const sockaddr*>(&sa), sizeof sa) == 0 ? 0 : last();   // EINPROGRESS when non-blocking
}

int bind(Socket s, const Ipv4& at) {
    const sockaddr_in sa = to_sa(at);
    return ::bind(fd(s), reinterpret_cast<const sockaddr*>(&sa), sizeof sa) == 0 ? 0 : last();
}

int listen(Socket s, int backlog) { return ::listen(fd(s), backlog) == 0 ? 0 : last(); }

int accept(Socket s, Socket* out, Ipv4* peer) {
    sockaddr_in sa{};
    socklen_t len = sizeof sa;
    const int n = ::accept4(fd(s), reinterpret_cast<sockaddr*>(&sa), &len, SOCK_CLOEXEC);
    if (n < 0) return last();
    *out = n;
    if (peer) *peer = from_sa(sa);
    return 0;
}

int local_addr(Socket s, Ipv4* out) {
    sockaddr_in sa{};
    socklen_t len = sizeof sa;
    if (::getsockname(fd(s), reinterpret_cast<sockaddr*>(&sa), &len) != 0) return last();
    *out = from_sa(sa);
    return 0;
}

int peer_addr(Socket s, Ipv4* out) {
    sockaddr_in sa{};
    socklen_t len = sizeof sa;
    if (::getpeername(fd(s), reinterpret_cast<sockaddr*>(&sa), &len) != 0) return last();
    *out = from_sa(sa);
    return 0;
}

int shutdown(Socket s, Shut how) {
    const int h = how == Shut::Read ? SHUT_RD : (how == Shut::Write ? SHUT_WR : SHUT_RDWR);
    return ::shutdown(fd(s), h) == 0 ? 0 : last();
}

int bytes_available(Socket s, uint32_t* out) {
    int n = 0;
    if (::ioctl(fd(s), FIONREAD, &n) != 0) return last();
    *out = static_cast<uint32_t>(n < 0 ? 0 : n);
    return 0;
}

int64_t send(Socket s, const void* data, size_t n, uint32_t flags, const Ipv4* to) {
    ssize_t r;
    if (to) {
        const sockaddr_in sa = to_sa(*to);
        r = ::sendto(fd(s), data, n, host_flags(flags), reinterpret_cast<const sockaddr*>(&sa), sizeof sa);
    } else {
        r = ::send(fd(s), data, n, host_flags(flags));
    }
    return r < 0 ? last() : r;
}

int64_t recv(Socket s, void* data, size_t n, uint32_t flags, Ipv4* from) {
    ssize_t r;
    sockaddr_in sa{};
    if (from) {
        socklen_t len = sizeof sa;
        r = ::recvfrom(fd(s), data, n, host_flags(flags) & ~MSG_NOSIGNAL, reinterpret_cast<sockaddr*>(&sa), &len);
    } else {
        r = ::recv(fd(s), data, n, host_flags(flags) & ~MSG_NOSIGNAL);
    }
    if (r < 0) return last();
    if (from) *from = from_sa(sa);
    return r;   // a longer datagram is cut to the buffer by the kernel
}

int set_int(Socket s, Level level, Opt opt, int32_t value) {
    int hl, hn;
    if (!host_opt(level, opt, &hl, &hn)) return -kENOPROTOOPT;
    if (opt == Opt::IpDontFragment) value = value ? IP_PMTUDISC_DO : IP_PMTUDISC_DONT;
    return ::setsockopt(fd(s), hl, hn, &value, sizeof value) == 0 ? 0 : last();
}

int get_int(Socket s, Level level, Opt opt, int32_t* value) {
    int hl, hn;
    if (!host_opt(level, opt, &hl, &hn)) return -kENOPROTOOPT;
    uint8_t raw[16] = {};
    socklen_t len = sizeof raw;
    if (::getsockopt(fd(s), hl, hn, raw, &len) != 0) return last();
    int32_t v = 0;
    std::memcpy(&v, raw, len < 4 ? len : 4);
    if (len == 1) v = raw[0];
    if (opt == Opt::IpDontFragment) v = v == IP_PMTUDISC_DO ? 1 : 0;
    *value = v;   // SO_ERROR is a Linux errno already
    return 0;
}

int set_timeout_ms(Socket s, Opt opt, int64_t ms) {
    int hl, hn;
    if (!host_opt(Level::Socket, opt, &hl, &hn)) return -kENOPROTOOPT;
    if (ms < 0) ms = 0;
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(ms / 1000);
    tv.tv_usec = static_cast<suseconds_t>((ms % 1000) * 1000);
    return ::setsockopt(fd(s), hl, hn, &tv, sizeof tv) == 0 ? 0 : last();
}

int get_timeout_ms(Socket s, Opt opt, int64_t* ms) {
    int hl, hn;
    if (!host_opt(Level::Socket, opt, &hl, &hn)) return -kENOPROTOOPT;
    timeval tv{};
    socklen_t len = sizeof tv;
    if (::getsockopt(fd(s), hl, hn, &tv, &len) != 0) return last();
    *ms = static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
    return 0;
}

int set_linger(Socket s, bool on, int seconds) {
    linger l{};
    l.l_onoff = on ? 1 : 0;
    l.l_linger = seconds < 0 ? 0 : (seconds > 0xFFFF ? 0xFFFF : seconds);
    return ::setsockopt(fd(s), SOL_SOCKET, SO_LINGER, &l, sizeof l) == 0 ? 0 : last();
}

int get_linger(Socket s, bool* on, int* seconds) {
    linger l{};
    socklen_t len = sizeof l;
    if (::getsockopt(fd(s), SOL_SOCKET, SO_LINGER, &l, &len) != 0) return last();
    *on = l.l_onoff != 0;
    *seconds = l.l_linger;
    return 0;
}

int set_membership(Socket s, Opt opt, const uint8_t g[8]) {
    int hl, hn;
    if (!host_opt(Level::Ip, opt, &hl, &hn)) return -kENOPROTOOPT;
    return ::setsockopt(fd(s), hl, hn, g, 8) == 0 ? 0 : last();   // struct ip_mreq: two in_addr
}

int poll(PollItem* items, size_t count, int timeout_ms) {
    std::vector<pollfd> host(count);
    for (size_t i = 0; i < count; ++i) {
        host[i] = {};
        host[i].fd = fd(items[i].s);
        host[i].events = host_events(items[i].events);
    }
    const int rc = ::poll(host.data(), static_cast<nfds_t>(count), timeout_ms);
    if (rc < 0) return last();
    int ready = 0;
    for (size_t i = 0; i < count; ++i) {
        items[i].revents = bsd_revents(host[i].revents, items[i].events);
        if (items[i].revents) ++ready;
    }
    return ready;
}

bool resolve_ipv4(const std::string& name, uint8_t ip[4]) {
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
