// SPDX-License-Identifier: GPL-3.0-or-later
// Shared basics for the setup tool: integer names, one error type, hex and UTF-8 helpers.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mprs {

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using Key128 = std::array<u8, 16>;

// Why a run stopped. The CLI maps this to its exit code; the GUI picks the message title from it.
enum class Fail {
    Generic = 1,
    Input,       // the dump is not what we need: wrong container, wrong title or version, corrupt or truncated
    Keys,        // keys missing or unable to open the dump
    Disk,        // not enough space, cannot write
    Cancelled,   // the user stopped the run (progress is kept; run again to resume)
};

struct Error : std::runtime_error {
    Fail kind;
    Error(Fail k, const std::string& m) : std::runtime_error(m), kind(k) {}
};

// printf-style; throws Error. Messages are for the player: plain sentences, no jargon where avoidable.
[[noreturn]] void fail(Fail kind, const char* fmt, ...);

std::string hex_encode(const void* data, size_t n, bool upper = false);
// Decodes exactly n bytes (2n hex digits, either case). False on bad length or digit.
bool hex_decode(std::string_view s, u8* out, size_t n);

std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view wide);

}  // namespace mprs
