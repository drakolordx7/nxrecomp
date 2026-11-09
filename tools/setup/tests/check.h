// SPDX-License-Identifier: GPL-3.0-or-later
// Tiny test helpers: CHECK / CHECK_EQ / CHECK_THROWS print the failing line and count; test mains return TEST_SUMMARY(name).
#pragma once
#include <cstdio>
#include <exception>
#include <string>

namespace nxs_test {
inline int& failures() { static int n = 0; return n; }
inline int& checks() { static int n = 0; return n; }
}  // namespace nxs_test

#define CHECK(cond)                                                                                  \
    do {                                                                                             \
        ++nxs_test::checks();                                                                       \
        if (!(cond)) { ++nxs_test::failures(); std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)
#define CHECK_EQ(a, b)                                                                               \
    do {                                                                                             \
        ++nxs_test::checks();                                                                       \
        if (!((a) == (b))) { ++nxs_test::failures(); std::printf("FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); } \
    } while (0)
#define CHECK_THROWS(expr)                                                                           \
    do {                                                                                             \
        ++nxs_test::checks();                                                                       \
        bool threw_ = false;                                                                         \
        try { (void)(expr); } catch (const std::exception&) { threw_ = true; }                       \
        if (!threw_) { ++nxs_test::failures(); std::printf("FAIL %s:%d: expected a throw: %s\n", __FILE__, __LINE__, #expr); } \
    } while (0)
#define TEST_SUMMARY(name)                                                                           \
    (std::printf("%s: %d checks, %d failed\n", name, nxs_test::checks(), nxs_test::failures()), nxs_test::failures() != 0 ? 1 : 0)
