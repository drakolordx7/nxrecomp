// SPDX-License-Identifier: GPL-3.0-or-later
// Moved from the base's runtime/tests (test_svc_fs.cpp) in the untangle: these cases check Metroid Prime Remastered's tables (title defaults,
// play-mode vocabulary and factory settings, test-tooling probes), so they run with the mpr module registered.
// fsp-srv at IPC level: RomFS storage, read-only data file system, save data (create / open / write / commit / drop), SD card, account
// save API. Request bytes follow Switchbrew "Filesystem services" / libnx fs_service.c layouts.
#define _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING
#include <windows.h>

#include <cstring>
#include <filesystem>
#include <fstream>

#include "nxrt/config.h"
#include "nxrt/services_api.h"
#include "svc_test_util.h"

NXTEST_GROUP(mpr_svc_fs)

namespace stdfs = std::filesystem;
using nxsvc::Args;
using nxsvc::Reply;
using nxsvc::Session;

namespace {

constexpr uint32_t kPathNotFound = 0x202, kPathExists = 0x402, kTargetNotFound = 0x7D402, kSdNoDevice = 0xFA202, kOutOfRange = 0x177A02;
constexpr uint32_t kNoAppend = 0x307202;

struct Fixture {
    stdfs::path dir, rom, save;
    nxsvc::Env env;
    Fixture() {
        dir = stdfs::temp_directory_path() / ("nxrt_svcfs_" + std::to_string(GetCurrentProcessId()));
        std::error_code ec;
        stdfs::remove_all(dir, ec);
        rom = dir / "romfs";
        save = dir / "save";
        stdfs::create_directories(rom / "Worlds", ec);
        stdfs::create_directories(save, ec);
        write(rom / "a.bin", std::string(1000, 'a'));
        write(rom / "Worlds" / "b.pak", "bee");
        nxrt::config_set("fs", "romfs", rom.string().c_str());
        nxrt::config_set("fs", "save_dir", save.string().c_str());
        nxrt::services::reset_all();
    }
    ~Fixture() {
        nxrt::config_clear();
        nxrt::services::reset_all();
        std::error_code ec;
        stdfs::remove_all(dir, ec);
    }
    static void write(const stdfs::path& p, const std::string& s) {
        std::ofstream o(p, std::ios::binary);
        o << s;
    }
};

// A 0x301-byte path buffer for the A/X buffer of the IFileSystem commands.
struct PathBuf {
    char b[0x301] = {};
    explicit PathBuf(const char* s) { std::strncpy(b, s, 0x300); }
};

// SaveDataAttribute (0x40): application id, uid (hi, lo), system save data id, type, rank, index.
std::vector<uint8_t> attr(uint8_t type, uint64_t uid_hi, uint64_t uid_lo, uint16_t index = 0) {
    std::vector<uint8_t> a(0x40, 0);
    const uint64_t app = 0;   // 0 = the calling title
    std::memcpy(&a[0], &app, 8);
    std::memcpy(&a[8], &uid_hi, 8);
    std::memcpy(&a[16], &uid_lo, 8);
    a[32] = type;
    std::memcpy(&a[34], &index, 2);
    return a;
}

Session open_fsp() {
    Session s = nxsvc::connect("fsp-srv");
    CHECK(s.valid());
    CHECK(s.call(1, Args().with_pid().u64(0)).ok());   // SetCurrentProcess(pid)
    return s;
}

// CreateSaveDataFileSystem(attr, creation info, meta info): creation info = size, journal size, available, owner, flags, space id.
Reply create_save(Session& s, const std::vector<uint8_t>& a, int64_t size, int64_t journal) {
    Args args;
    args.bytes(a.data(), a.size());
    args.i64(size).i64(journal).u64(size).u64(0x010012101468C000ull).u32(0).u8(1).pad(0x1B);   // creation info, 0x40 bytes
    args.pad(0x10);                                                                          // meta info
    return s.call(22, args);
}

Reply open_save(Session& s, const std::vector<uint8_t>& a, uint8_t space = 1, uint32_t cmd = 51) {
    Args args;
    args.u8(space).pad(7).bytes(a.data(), a.size());
    return s.call(cmd, args);
}

Reply fs_call_path(Session& fsys, uint32_t cmd, const char* path, Args extra = {}) {
    auto pb = std::make_unique<PathBuf>(path);   // heap: the X descriptor holds 39-bit addresses
    extra.buf_x(pb->b, 0x301);
    return fsys.call(cmd, extra);
}

}  // namespace

NXTEST(account_save_api) {
    Fixture f;
    bool created = false;
    int64_t size = 0, journal = 0;
    CHECK_EQ(nxrt::services::fsapi::account_save_size(1, 2, &size, &journal), kTargetNotFound);
    CHECK_EQ(nxrt::services::fsapi::ensure_account_save(1, 2, &created), 0u);
    CHECK(created);
    CHECK_EQ(nxrt::services::fsapi::ensure_account_save(1, 2, &created), 0u);
    CHECK(!created);
    CHECK_EQ(nxrt::services::fsapi::account_save_size(1, 2, &size, &journal), 0u);
    CHECK_EQ(size, 4ll << 20);
    CHECK_EQ(journal, 1ll << 20);
    // the same save is then openable through fsp-srv with the attribute the SDK builds for MountSaveData
    Session s = open_fsp();
    CHECK(open_save(s, attr(1, 1, 2)).ok());
}

