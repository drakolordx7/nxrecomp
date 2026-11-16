// SPDX-License-Identifier: GPL-3.0-or-later
// nxrt::os::net: IPv4 sockets of the host (Winsock on Windows, BSD sockets on Linux). Errors are reported as Linux errno numbers, which is the
// numbering a Switch title sees, so the caller passes them on unchanged: every function returns 0 or a count on success and the NEGATIVE errno
// on failure (-EAGAIN is -11, -ECONNREFUSED is -111). Nothing here throws. Poll bits and message flags use the BSD (Horizon) values; options are
// named by the enums below, the mapping to the host's numbers is inside the platform file.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace nxrt::os::net {

using Socket = intptr_t;   // SOCKET or file descriptor
constexpr Socket kInvalidSocket = -1;

struct Ipv4 {
    uint8_t ip[4] = {};
    uint16_t port = 0;     // host byte order
};

// Starts the socket library of the process (Winsock); safe to call more than once and from several threads.
bool init();

enum class Type : int { Stream = 1, Datagram = 2 };
// protocol 0: TCP for Stream, UDP for Datagram.
int open(Type type, int protocol, Socket* out);
void close(Socket s);
int set_nonblocking(Socket s, bool on);
int connect(Socket s, const Ipv4& to);   // a non-blocking connect under way gives -EINPROGRESS (115)
int bind(Socket s, const Ipv4& at);
int listen(Socket s, int backlog);
int accept(Socket s, Socket* out, Ipv4* peer);
// -EINVAL (22) from local_addr: the socket is not bound yet.
int local_addr(Socket s, Ipv4* out);
int peer_addr(Socket s, Ipv4* out);
enum class Shut : int { Read = 0, Write = 1, Both = 2 };
int shutdown(Socket s, Shut how);
// Bytes that can be read without blocking.
int bytes_available(Socket s, uint32_t* out);

// Message flags, the BSD values.
constexpr uint32_t kMsgOob = 0x1, kMsgPeek = 0x2, kMsgDontRoute = 0x4, kMsgWaitAll = 0x40;
// Byte counts or -errno. A datagram longer than the buffer returns the part that fits (not an error). `to` and `from` may be null.
int64_t send(Socket s, const void* data, size_t n, uint32_t flags, const Ipv4* to);
int64_t recv(Socket s, void* data, size_t n, uint32_t flags, Ipv4* from);

// ---- options ------------------------------------------------------------------------------------------------------------------------

enum class Level : int { Socket, Ip, Tcp };
enum class Opt : int {
    // Level::Socket
    Debug, AcceptConn, ReuseAddr, KeepAlive, DontRoute, Broadcast, UseLoopback, Linger, OobInline, ReusePort,
    SndBuf, RcvBuf, SndLowat, RcvLowat, SndTimeo, RcvTimeo, Error, SockType,
    // Level::Ip
    IpOptions, IpHdrIncl, IpTos, IpTtl, IpMulticastIf, IpMulticastTtl, IpMulticastLoop, IpAddMembership, IpDropMembership, IpDontFragment,
    // Level::Tcp
    TcpNoDelay, TcpMaxSeg, TcpKeepIdle, TcpKeepIntvl, TcpKeepCnt,
};
// -ENOPROTOOPT (92) for an option the host does not have.
int set_int(Socket s, Level level, Opt opt, int32_t value);
int get_int(Socket s, Level level, Opt opt, int32_t* value);   // Opt::Error reads the pending error as a Linux errno
// Opt::SndTimeo / Opt::RcvTimeo in milliseconds (0: none).
int set_timeout_ms(Socket s, Opt opt, int64_t ms);
int get_timeout_ms(Socket s, Opt opt, int64_t* ms);
int set_linger(Socket s, bool on, int seconds);
int get_linger(Socket s, bool* on, int* seconds);
// IP_ADD_MEMBERSHIP / IP_DROP_MEMBERSHIP: 4 bytes of group, 4 bytes of interface.
int set_membership(Socket s, Opt opt, const uint8_t group_and_interface[8]);

// ---- poll ---------------------------------------------------------------------------------------------------------------------------

constexpr uint16_t kPollIn = 0x1, kPollPri = 0x2, kPollOut = 0x4, kPollErr = 0x8, kPollHup = 0x10, kPollNval = 0x20, kPollRdNorm = 0x40,
                   kPollRdBand = 0x80, kPollWrBand = 0x100;
struct PollItem {
    Socket s = kInvalidSocket;
    uint16_t events = 0;
    uint16_t revents = 0;
};
// Waits up to `timeout_ms` (-1: without end) for one of the items. Returns how many have revents set, or -errno.
int poll(PollItem* items, size_t count, int timeout_ms);

// ---- names --------------------------------------------------------------------------------------------------------------------------

// The first IPv4 address of `name` from the system's resolver.
bool resolve_ipv4(const std::string& name, uint8_t ip[4]);

}  // namespace nxrt::os::net
