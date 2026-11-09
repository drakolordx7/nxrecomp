// SPDX-License-Identifier: GPL-3.0-or-later
// Host sockets ([net] sockets = host): sfdnsres answers from [net] hosts in the serialized formats libnx's resolver reads (runtime/resolver.c),
// and bsd commands run on real Winsock sockets against a loopback listener opened here. The request layouts are libnx's (services/bsd.c,
// services/sfdnsres.c); the expected bytes are written out from those layouts, not read from the service under test.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include "nxrt/config.h"
#include "svc_test_util.h"

NXTEST_GROUP(svc_network_host)

using namespace nxrt;

namespace {
// GetAddrInfoRequest (6): in (u32 use_nsd, u32 cancel_handle, u64 pid placeholder) + pid, A: host, service, hints; B: response.
// The buffers are copied to the stack: HIPC A/B addresses must fit in 39 bits, which the exe's read-only data may not.
nxsvc::Reply getaddrinfo_call(nxsvc::Session& dns, const char* host, const char* service, uint8_t* out, uint32_t out_size) {
    char h[64] = {}, s[16] = {};
    uint8_t hints[24] = {};
    std::strncpy(h, host, sizeof h - 1);
    std::strncpy(s, service, sizeof s - 1);
    return dns.call(6, nxsvc::Args()
                           .with_pid()
                           .u32(0)
                           .u32(0)
                           .u64(0)
                           .buf_a(h, std::strlen(h) + 1)
                           .buf_a(s, std::strlen(s) + 1)
                           .buf_a(hints, sizeof hints)
                           .buf_b(out, out_size));
}
}  // namespace

NXTEST(sfdnsres_resolves_listed_name) {
    nxsvc::Env env;
    config_clear();
    config_set("net", "sockets", "host");
    config_set("net", "hosts", "test.local=127.0.0.1;*.wild.example=10.1.2.3");
    nxsvc::Session dns = nxsvc::connect("sfdnsres");
    REQUIRE(dns.valid());

    // GetAddrInfoRequest -> (bsd_errno, gai_error, serialized size)
    uint8_t out[0x200] = {};
    nxsvc::Reply r = getaddrinfo_call(dns, "test.local", "8080", out, sizeof out);
    CHECK_EQ(r.result, 0u);
    CHECK_EQ(r.get<uint32_t>(0), 0u);
    CHECK_EQ(r.get<int32_t>(4), 0);
    CHECK_EQ(r.get<uint32_t>(8), 24u + 16u + 11u + 4u);   // header, sockaddr_in, "test.local\0", sentinel
    const uint8_t want[] = {
        0xBE, 0xEF, 0xCA, 0xFE,  0, 0, 0, 0,  0, 0, 0, 2,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 16,   // magic, flags, AF_INET, socktype, protocol, addrlen
        4, 2, 0x90, 0x1F, 1, 0, 0, 127,  0, 0, 0, 0, 0, 0, 0, 0,                                   // sockaddr_in swapped once more: port 8080, 127.0.0.1
        't', 'e', 's', 't', '.', 'l', 'o', 'c', 'a', 'l', 0,
        0, 0, 0, 0,
    };
    CHECK(std::memcmp(out, want, sizeof want) == 0);

    // A wildcard entry
    std::memset(out, 0, sizeof out);
    r = getaddrinfo_call(dns, "api.wild.example", "", out, sizeof out);
    CHECK_EQ(r.get<int32_t>(4), 0);
    CHECK(out[28] == 3 && out[29] == 2 && out[30] == 1 && out[31] == 10);

    // A name that is not listed: EAI_NONAME, nothing reaches the PC's resolver ([net] dns = hosts by default)
    r = getaddrinfo_call(dns, "not-listed.invalid", "80", out, sizeof out);
    CHECK_EQ(r.get<int32_t>(4), -2);
    CHECK_EQ(r.get<uint32_t>(8), 0u);

    // GetHostByNameRequest (2): in (u32 use_nsd, u32 cancel_handle, u64 pid placeholder) + pid, A: name, B: hostent -> (h_errno, errno, size)
    char name[] = "test.local";
    uint8_t he[0x100] = {};
    nxsvc::Reply h = dns.call(2, nxsvc::Args().with_pid().u32(0).u32(0).u64(0).buf_a(name, sizeof name).buf_b(he, sizeof he));
    CHECK_EQ(h.result, 0u);
    CHECK_EQ(h.get<uint32_t>(0), 0u);
    CHECK_EQ(h.get<uint32_t>(4), 0u);
    CHECK_EQ(h.get<uint32_t>(8), 11u + 4u + 2u + 2u + 4u + 4u);
    const uint8_t want_he[] = {'t', 'e', 's', 't', '.', 'l', 'o', 'c', 'a', 'l', 0, 0, 0, 0, 0, 0, 2, 0, 4, 0, 0, 0, 1, 1, 0, 0, 127};
    CHECK(std::memcmp(he, want_he, sizeof want_he) == 0);
    config_clear();
}

NXTEST(bsd_loopback_tcp_exchange) {
    nxsvc::Env env;
    WSADATA wd{};
    REQUIRE(WSAStartup(MAKEWORD(2, 2), &wd) == 0);
    SOCKET ls = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    REQUIRE(ls != INVALID_SOCKET);
    sockaddr_in la{};
    la.sin_family = AF_INET;
    la.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(::bind(ls, reinterpret_cast<sockaddr*>(&la), sizeof la) == 0);
    int la_len = sizeof la;
    REQUIRE(::getsockname(ls, reinterpret_cast<sockaddr*>(&la), &la_len) == 0);
    REQUIRE(::listen(ls, 4) == 0);
    const uint16_t real_port = ntohs(la.sin_port);

    config_clear();
    config_set("net", "sockets", "host");
    config_set("net", "port_map", ("7=" + std::to_string(real_port)).c_str());   // the guest dials port 7, the map sends it to the listener
    nxsvc::Session bsd = nxsvc::connect("bsd:u");
    REQUIRE(bsd.valid());
    // RegisterClient(BsdBufferConfig 0x20, u64 pid placeholder, u64 tmem size) + pid -> u32 bsd_errno
    nxsvc::Reply reg = bsd.call(0, nxsvc::Args().with_pid().pad(0x20).u64(0).u64(0));
    CHECK(reg.ok());
    CHECK_EQ(reg.get<uint32_t>(0), 0u);

    // Socket(AF_INET 2, SOCK_STREAM 1, 0) -> (i32 fd, u32 errno)
    nxsvc::Reply so = bsd.call(2, nxsvc::Args().i32(2).i32(1).i32(0));
    CHECK_EQ(so.get<uint32_t>(4), 0u);
    const int32_t fd = so.get<int32_t>(0);
    REQUIRE(fd >= 0);

    // Connect(fd, A: BSD sockaddr_in {len 16, family 2, port 7 big-endian, 127.0.0.1})
    uint8_t sa[16] = {16, 2, 0, 7, 127, 0, 0, 1};   // not const: kept on the stack (39-bit HIPC addresses)
    nxsvc::Reply c = bsd.call(14, nxsvc::Args().i32(fd).buf_a(sa, sizeof sa));
    CHECK_EQ(c.get<int32_t>(0), 0);
    CHECK_EQ(c.get<uint32_t>(4), 0u);
    SOCKET peer = ::accept(ls, nullptr, nullptr);
    REQUIRE(peer != INVALID_SOCKET);

    // Send(fd, flags 0, A: data) -> (i32 sent, u32 errno)
    char hello[] = "hello";
    nxsvc::Reply s = bsd.call(10, nxsvc::Args().i32(fd).i32(0).buf_a(hello, 5));
    CHECK_EQ(s.get<int32_t>(0), 5);
    char got[16] = {};
    int n = 0;
    while (n < 5) {
        const int k = ::recv(peer, got + n, 5 - n, 0);
        if (k <= 0) break;
        n += k;
    }
    CHECK_EQ(n, 5);
    CHECK(std::memcmp(got, "hello", 5) == 0);

    // Recv(fd, flags 0, B: buffer) -> (i32 received, u32 errno)
    REQUIRE(::send(peer, "world!", 6, 0) == 6);
    char back[64] = {};
    nxsvc::Reply rv = bsd.call(8, nxsvc::Args().i32(fd).i32(0).buf_b(back, sizeof back));
    CHECK_EQ(rv.get<uint32_t>(4), 0u);
    CHECK_EQ(rv.get<int32_t>(0), 6);
    CHECK(std::memcmp(back, "world!", 6) == 0);

    // Close(fd) -> (0, 0); a second Close: (-1, EBADF 9)
    nxsvc::Reply cl = bsd.call(26, nxsvc::Args().i32(fd));
    CHECK_EQ(cl.get<int32_t>(0), 0);
    CHECK_EQ(cl.get<uint32_t>(4), 0u);
    nxsvc::Reply cl2 = bsd.call(26, nxsvc::Args().i32(fd));
    CHECK_EQ(cl2.get<int32_t>(0), -1);
    CHECK_EQ(cl2.get<uint32_t>(4), 9u);

    ::closesocket(peer);
    ::closesocket(ls);
    config_clear();
}

NXTEST(sockets_off_keeps_offline_answers) {
    nxsvc::Env env;
    config_clear();
    config_set("net", "hosts", "test.local=127.0.0.1");   // ignored while [net] sockets is off
    nxsvc::Session bsd = nxsvc::connect("bsd:u");
    REQUIRE(bsd.valid());
    nxsvc::Reply r = bsd.call(2, nxsvc::Args().i32(2).i32(1).i32(0));
    CHECK_EQ(r.get<int32_t>(0), -1);
    CHECK_EQ(r.get<uint32_t>(4), 100u);   // ENETDOWN
    nxsvc::Session dns = nxsvc::connect("sfdnsres");
    REQUIRE(dns.valid());
    uint8_t out[0x100] = {};
    nxsvc::Reply g = getaddrinfo_call(dns, "test.local", "80", out, sizeof out);
    CHECK_EQ(g.get<uint32_t>(0), 0u);
    CHECK_EQ(g.get<int32_t>(4), -2);      // EAI_NONAME
    CHECK_EQ(g.get<uint32_t>(8), 0u);
    config_clear();
}
