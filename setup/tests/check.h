// SPDX-License-Identifier: GPL-3.0-or-later
// Tiny test helpers: CHECK / CHECK_EQ / CHECK_THROWS print the failing line and count; test mains return TEST_SUMMARY(name).
#pragma once
#include <cstdio>
#include <exception>
#include <string>

namespace mprs_test {
inline int& failures() { static int n = 0; return n; }
inline int& checks() { static int n = 0; return n; }
}  // namespace mprs_test

#define CHECK(cond)                                                                                  \
    do {                                                                                             \
        ++mprs_test::checks();                                                                       \
        if (!(cond)) { ++mprs_test::failures(); std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)
#define CHECK_EQ(a, b)                                                                               \
    do {                                                                                             \
        ++mprs_test::checks();                                                                       \
        if (!((a) == (b))) { ++mprs_test::failures(); std::printf("FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); } \
    } while (0)
#define CHECK_THROWS(expr)                                                                           \
    do {                                                                                             \
        ++mprs_test::checks();                                                                       \
        bool threw_ = false;                                                                         \
        try { (void)(expr); } catch (const std::exception&) { threw_ = true; }                       \
        if (!threw_) { ++mprs_test::failures(); std::printf("FAIL %s:%d: expected a throw: %s\n", __FILE__, __LINE__, #expr); } \
    } while (0)
#define TEST_SUMMARY(name)                                                                           \
    (std::printf("%s: %d checks, %d failed\n", name, mprs_test::checks(), mprs_test::failures()), mprs_test::failures() != 0 ? 1 : 0)
