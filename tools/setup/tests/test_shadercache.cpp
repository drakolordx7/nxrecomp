// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for shadercache.h (folder source and MPRSHC01 pack, verify, cancel, refusals) and the fileio.h helpers it uses
// (AtomicFileWriter, free_disk_bytes, path helpers). Everything runs in a fresh temp folder that is deleted at the end.
#include "check.h"
#include "crypto.h"
#include "fileio.h"
#include "shadercache.h"
#include "storage.h"

#ifdef MPRS_HAVE_ZSTD
#include <zstd.h>
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

using namespace mprs;

namespace {

struct Fake {
    std::string name;
    std::string data;
};

// Five fake modules (binary content, varied sizes) and results.tsv.
std::vector<Fake> fake_cache() {
    const char* names[5] = {"0000000000000001.spv", "00004b0d6e7742b4.spv", "0f0e0d0c0b0a0908.spv", "a1b2c3d4e5f60718.spv",
                            "ffffffffffffffff.spv"};
    std::vector<Fake> v;
    for (int i = 0; i < 5; ++i) {
        std::string d;
        for (int k = 0; k < 100 + i * 37; ++k) d.push_back(static_cast<char>((k * 31 + i * 7) & 0xFF));
        v.push_back({names[i], d});
    }
    v.push_back({"results.tsv", "name\tstatus\n0000000000000001\tok\n00004b0d6e7742b4\tok\n"});
    return v;
}

std::string manifest_for(const std::vector<Fake>& files) {
    std::string m;
    for (const Fake& f : files) {
        auto h = sha256(f.data.data(), f.data.size());
        m += hex_encode(h.data(), h.size()) + "  " + f.name + "\n";
    }
    return m;
}

void write_folder(const std::string& dir, const std::vector<Fake>& files, const std::string* manifest) {
    if (!create_dirs(dir)) std::printf("FAIL test setup: cannot create %s\n", dir.c_str()), ++mprs_test::failures();
    for (const Fake& f : files)
        if (!write_text_file_atomic(path_join(dir, f.name), f.data)) std::printf("FAIL test setup: write %s\n", f.name.c_str()), ++mprs_test::failures();
    if (manifest && !write_text_file_atomic(path_join(dir, "manifest.sha256"), *manifest))
        std::printf("FAIL test setup: manifest\n"), ++mprs_test::failures();
}

bool same_content(const std::string& path, const std::string& expect) {
    std::string got;
    return read_text_file(path, got) && got == expect;
}

// Runs f and returns the Fail kind it threw: -1 if it did not throw, -2 for a non-mprs exception.
int thrown_kind(const std::function<void()>& f) {
    try {
        f();
        return -1;
    } catch (const Error& e) {
        return static_cast<int>(e.kind);
    } catch (...) {
        return -2;
    }
}

void put_le(std::string& s, u64 v, int bytes) {
    for (int i = 0; i < bytes; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

struct PackEntry {
    std::string name;
    std::string stored;   // bytes in the pack
    std::string raw;      // bytes that get installed (the hash is over these)
    u8 method;
    u64 declared_raw = ~u64(0);   // raw_size written in the record; ~0 means raw.size()
    bool wrong_hash = false;      // write a hash that does not match raw
};

// Builds an MPRSHC01 pack by hand, following the layout in shadercache.h.
std::string build_pack_bytes(const std::vector<PackEntry>& entries) {
    std::string out = "MPRSHC01";
    put_le(out, entries.size(), 4);
    put_le(out, 0, 4);
    u64 table = 0;
    for (const PackEntry& e : entries) table += 2 + e.name.size() + 8 + 8 + 8 + 32 + 1;
    u64 off = 16 + table;
    std::string records, data;
    for (const PackEntry& e : entries) {
        put_le(records, e.name.size(), 2);
        records += e.name;
        put_le(records, off, 8);
        put_le(records, e.stored.size(), 8);
        put_le(records, e.declared_raw == ~u64(0) ? e.raw.size() : e.declared_raw, 8);
        auto h = sha256(e.raw.data(), e.raw.size());
        if (e.wrong_hash) h[0] = static_cast<u8>(h[0] ^ 0x01);
        records.append(reinterpret_cast<const char*>(h.data()), h.size());
        records.push_back(static_cast<char>(e.method));
        off += e.stored.size();
        data += e.stored;
    }
    return out + records + data;
}

// Runs exe with args (already quoted). Returns the exit code, or -1 if the process could not be started.
int run_process(const std::wstring& exe, const std::wstring& args) {
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

void remove_tree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            std::wstring full = dir + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                remove_tree(full);
            else
                DeleteFileW(full.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

// ---- fileio.h

void test_fileio(const std::string& root) {
    // finish() returns the hash of what was written, and the file appears only at finish().
    std::string p = path_join(root, "aw\\sub\\out.bin");
    {
        AtomicFileWriter w(p);
        w.write("hello ", 6);
        w.write("world", 5);
        CHECK_EQ(w.bytes(), u64(11));
        CHECK(!path_exists(p));
        CHECK(path_exists(p + ".part"));
        auto h = w.finish();
        CHECK(h == sha256("hello world", 11));
    }
    CHECK(!path_exists(p + ".part"));
    std::string got;
    CHECK(read_text_file(p, got));
    CHECK_EQ(got, std::string("hello world"));

    // Destructor without finish(): no .part, no final file.
    std::string q = path_join(root, "aw\\dropped.bin");
    {
        AtomicFileWriter w(q);
        w.write("data", 4);
    }
    CHECK(!path_exists(q));
    CHECK(!path_exists(q + ".part"));

    // Destructor without finish() leaves an existing file at the final path alone.
    std::string keep = path_join(root, "aw\\keep.bin");
    CHECK(write_text_file_atomic(keep, "old"));
    {
        AtomicFileWriter w(keep);
        w.write("new", 3);
    }
    CHECK(same_content(keep, "old"));
    CHECK(!path_exists(keep + ".part"));

    // Overwrite is atomic: the old file is intact until finish(), then the new one is there.
    std::string o = path_join(root, "aw\\over.bin");
    CHECK(write_text_file_atomic(o, "old contents"));
    {
        AtomicFileWriter w(o);
        w.write("new", 3);
        CHECK(same_content(o, "old contents"));
        w.finish();
    }
    CHECK(same_content(o, "new"));

    // Disk space and path helpers.
    CHECK(free_disk_bytes(root) > 0);
    CHECK(free_disk_bytes(path_join(root, "no\\such\\deep\\folder")) > 0);
    CHECK_EQ(path_join("D:\\a", "b.txt"), std::string("D:\\a\\b.txt"));
    CHECK_EQ(path_join("D:\\a\\", "\\b"), std::string("D:\\a\\b"));
    CHECK_EQ(path_join("", "b"), std::string("b"));
    std::string sized = path_join(root, "size.txt");
    CHECK(write_text_file_atomic(sized, "12345"));
    CHECK(file_size_is(sized, 5));
    CHECK(!file_size_is(sized, 4));
    CHECK(!file_size_is(root, 0));
    std::string deep = path_join(root, "d1\\d2\\d3");
    CHECK(create_dirs(deep));
    CHECK(create_dirs(deep));
    CHECK(path_exists(deep));
    CHECK(path_exists(path_join(exe_dir(), "test_shadercache.exe")));
}

// ---- shadercache.h

void test_folder_source(const std::string& root) {
    const std::vector<Fake> files = fake_cache();
    const std::string manifest = manifest_for(files);
    const std::string src = path_join(root, "src");
    write_folder(src, files, &manifest);
    const std::string dest = path_join(root, "dest");
    u64 total_bytes = 0;
    for (const Fake& f : files) total_bytes += f.data.size();

    // First install: everything is written, progress counts every file.
    u64 calls = 0, last_done = 0, last_total = 0;
    auto progress = [&](u64 d, u64 t) {
        ++calls;
        last_done = d;
        last_total = t;
    };
    ShaderInstallResult r = install_shader_cache(src, dest, false, progress, nullptr);
    CHECK_EQ(r.files_total, 6u);
    CHECK_EQ(r.files_installed, 6u);
    CHECK_EQ(r.files_skipped, 0u);
    CHECK_EQ(r.bytes_written, total_bytes);
    CHECK(r.had_manifest);
    CHECK_EQ(calls, u64(6));
    CHECK_EQ(last_done, u64(6));
    CHECK_EQ(last_total, u64(6));
    for (const Fake& f : files) {
        CHECK(same_content(path_join(dest, f.name), f.data));
        CHECK(!path_exists(path_join(dest, f.name + ".part")));
    }

    // Second run: all present, all skipped, nothing written.
    r = install_shader_cache(src, dest, false, nullptr, nullptr);
    CHECK_EQ(r.files_skipped, 6u);
    CHECK_EQ(r.files_installed, 0u);
    CHECK_EQ(r.bytes_written, u64(0));

    // Corrupt one destination file, keeping its size. Without verify it is still skipped (size check only).
    const Fake& victim = files[2];
    std::string bad = victim.data;
    bad[0] = static_cast<char>(bad[0] ^ 0x55);
    CHECK(write_text_file_atomic(path_join(dest, victim.name), bad));
    r = install_shader_cache(src, dest, false, nullptr, nullptr);
    CHECK_EQ(r.files_skipped, 6u);

    // With verify it is caught by the hash and rewritten.
    r = install_shader_cache(src, dest, true, nullptr, nullptr);
    CHECK_EQ(r.files_installed, 1u);
    CHECK_EQ(r.files_skipped, 5u);
    CHECK_EQ(r.bytes_written, u64(victim.data.size()));
    CHECK(same_content(path_join(dest, victim.name), victim.data));

    // A manifest hash that does not match the source: refused as Input, and that file is not written.
    std::string src_badhash = path_join(root, "src_badhash");
    std::string bad_manifest = manifest;
    bad_manifest[0] = bad_manifest[0] == '0' ? '1' : '0';   // first line is files[0], which is also the first file installed
    write_folder(src_badhash, files, &bad_manifest);
    const std::string dest_badhash = path_join(root, "dest_badhash");
    int k = thrown_kind([&] { install_shader_cache(src_badhash, dest_badhash, false, nullptr, nullptr); });
    CHECK_EQ(k, static_cast<int>(Fail::Input));
    CHECK(!path_exists(path_join(dest_badhash, files[0].name)));

    // A file name that is not part of a cache: Input.
    std::string src_name = path_join(root, "src_badname");
    write_folder(src_name, files, nullptr);
    CHECK(write_text_file_atomic(path_join(src_name, "notashader.bin"), "x"));
    k = thrown_kind([&] { install_shader_cache(src_name, path_join(root, "dest_badname"), false, nullptr, nullptr); });
    CHECK_EQ(k, static_cast<int>(Fail::Input));

    // No results.tsv: Input, with the explanation in the message.
    std::string src_nores = path_join(root, "src_nores");
    std::vector<Fake> no_results(files.begin(), files.begin() + 5);
    write_folder(src_nores, no_results, nullptr);
    std::string msg;
    try {
        install_shader_cache(src_nores, path_join(root, "dest_nores"), false, nullptr, nullptr);
    } catch (const Error& e) {
        msg = e.what();
    }
    CHECK(msg.find("results.tsv is missing") != std::string::npos);
    k = thrown_kind([&] { install_shader_cache(src_nores, path_join(root, "dest_nores"), false, nullptr, nullptr); });
    CHECK_EQ(k, static_cast<int>(Fail::Input));

    // Cancel after two files: Cancelled, two files on disk. A later run picks up where it stopped.
    const std::string dest_cancel = path_join(root, "dest_cancel");
    int polls = 0;
    auto cancel_after_two = [&]() -> bool { return ++polls > 2; };
    k = thrown_kind([&] { install_shader_cache(src, dest_cancel, false, nullptr, cancel_after_two); });
    CHECK_EQ(k, static_cast<int>(Fail::Cancelled));
    int on_disk = 0;
    for (const Fake& f : files) on_disk += path_exists(path_join(dest_cancel, f.name)) ? 1 : 0;
    CHECK_EQ(on_disk, 2);
    r = install_shader_cache(src, dest_cancel, false, nullptr, nullptr);
    CHECK_EQ(r.files_skipped, 2u);
    CHECK_EQ(r.files_installed, 4u);
}

void test_pack_source(const std::string& root) {
    const std::vector<Fake> files = fake_cache();
    std::vector<PackEntry> entries;
    for (const Fake& f : files) entries.push_back({f.name, f.data, f.data, 0});

    // A hand-built stored pack installs like a folder.
    std::string pack = path_join(root, "stored.mprshc");
    CHECK(write_text_file_atomic(pack, build_pack_bytes(entries)));
    const std::string dest = path_join(root, "dest_pack");
    ShaderInstallResult r = install_shader_cache(pack, dest, false, nullptr, nullptr);
    CHECK_EQ(r.files_total, 6u);
    CHECK_EQ(r.files_installed, 6u);
    CHECK(r.had_manifest);
    for (const Fake& f : files) CHECK(same_content(path_join(dest, f.name), f.data));
    r = install_shader_cache(pack, dest, true, nullptr, nullptr);
    CHECK_EQ(r.files_skipped, 6u);

#ifndef MPRS_HAVE_ZSTD
    // Without zstd in the build, a pack entry with method 1 is refused as Input before anything is written.
    std::vector<PackEntry> zstd_entries = entries;
    zstd_entries.back().method = 1;
    zstd_entries.back().stored = "abc";
    std::string zpack = path_join(root, "zstd.mprshc");
    CHECK(write_text_file_atomic(zpack, build_pack_bytes(zstd_entries)));
    const std::string zdest = path_join(root, "dest_zstd");
    int k = thrown_kind([&] { install_shader_cache(zpack, zdest, false, nullptr, nullptr); });
    CHECK_EQ(k, static_cast<int>(Fail::Input));
    CHECK(!path_exists(path_join(zdest, "results.tsv")));
#endif

    // A pack built by make_shader_pack.py from the folder, when Python can be started.
    const std::wstring py = L"C:\\mpr\\tools\\.venv\\Scripts\\python.exe";
    const std::wstring script = L"C:\\mpr\\tools\\setup\\scripts\\make_shader_pack.py";
    if (!path_exists(narrow(py)) || !path_exists(narrow(script))) {
        std::printf("SKIP python pack check: %s or %s is not present\n", narrow(py).c_str(), narrow(script).c_str());
        return;
    }
    const std::string src = path_join(root, "src");
    const std::string py_pack = path_join(root, "py.mprshc");
    const std::wstring args = L"\"" + script + L"\" \"" + widen(src) + L"\" \"" + widen(py_pack) + L"\"";
    int code = run_process(py, args);
    if (code < 0) {
        std::printf("SKIP python pack check: could not start %s\n", narrow(py).c_str());
        return;
    }
    CHECK_EQ(code, 0);
    if (code != 0) return;
    const std::string py_dest = path_join(root, "dest_py");
    r = install_shader_cache(py_pack, py_dest, true, nullptr, nullptr);
    CHECK_EQ(r.files_installed, 6u);
    for (const Fake& f : files) CHECK(same_content(path_join(py_dest, f.name), f.data));
}

#ifdef MPRS_HAVE_ZSTD
std::string zstd_compress(const std::string& raw) {
    std::string out(ZSTD_compressBound(raw.size()), '\0');
    const size_t n = ZSTD_compress(out.data(), out.size(), raw.data(), raw.size(), 12);
    if (ZSTD_isError(n)) {
        std::printf("FAIL test setup: ZSTD_compress: %s\n", ZSTD_getErrorName(n));
        ++mprs_test::failures();
        return std::string();
    }
    out.resize(n);
    return out;
}

// Method-1 (zstd) entries: built by hand with the libzstd in this build, and by make_shader_pack.py --zstd.
void test_zstd_pack(const std::string& root) {
    const std::vector<Fake> files = fake_cache();
    u64 total_bytes = 0;
    for (const Fake& f : files) total_bytes += f.data.size();
    std::vector<PackEntry> entries;
    for (const Fake& f : files) entries.push_back({f.name, zstd_compress(f.data), f.data, 1});

    // Every entry zstd: installs, and a verify run finds every file in place.
    const std::string pack = path_join(root, "zstd_ok.mprshc");
    CHECK(write_text_file_atomic(pack, build_pack_bytes(entries)));
    const std::string dest = path_join(root, "dest_zstd_ok");
    ShaderInstallResult r = install_shader_cache(pack, dest, false, nullptr, nullptr);
    CHECK_EQ(r.files_total, 6u);
    CHECK_EQ(r.files_installed, 6u);
    CHECK_EQ(r.bytes_written, total_bytes);
    CHECK(r.had_manifest);
    for (const Fake& f : files) CHECK(same_content(path_join(dest, f.name), f.data));
    r = install_shader_cache(pack, dest, true, nullptr, nullptr);
    CHECK_EQ(r.files_skipped, 6u);

    // A mixed pack (stored and zstd entries) installs too.
    std::vector<PackEntry> mixed = entries;
    mixed[1].stored = files[1].data;
    mixed[1].method = 0;
    const std::string mixed_pack = path_join(root, "zstd_mixed.mprshc");
    CHECK(write_text_file_atomic(mixed_pack, build_pack_bytes(mixed)));
    const std::string mixed_dest = path_join(root, "dest_zstd_mixed");
    r = install_shader_cache(mixed_pack, mixed_dest, false, nullptr, nullptr);
    CHECK_EQ(r.files_installed, 6u);
    for (const Fake& f : files) CHECK(same_content(path_join(mixed_dest, f.name), f.data));

    // A flipped byte in the compressed payload: Input, and the damaged entry is not written.
    std::vector<PackEntry> corrupt = entries;
    std::string& cs = corrupt[1].stored;   // 00004b0d6e7742b4.spv
    cs[cs.size() / 2] = static_cast<char>(cs[cs.size() / 2] ^ 0x55);
    const std::string corrupt_pack = path_join(root, "zstd_corrupt.mprshc");
    CHECK(write_text_file_atomic(corrupt_pack, build_pack_bytes(corrupt)));
    const std::string corrupt_dest = path_join(root, "dest_zstd_corrupt");
    int k = thrown_kind([&] { install_shader_cache(corrupt_pack, corrupt_dest, false, nullptr, nullptr); });
    CHECK_EQ(k, static_cast<int>(Fail::Input));
    CHECK(!path_exists(path_join(corrupt_dest, files[1].name)));

    // A raw_size field that lies: too large, too small, and beyond the installer's cap. All Input, nothing written for that entry.
    const u64 real = files[2].data.size();   // 0f0e0d0c0b0a0908.spv
    const u64 lies[3] = {real + 1, real - 1, u64(1) << 31};
    for (int i = 0; i < 3; ++i) {
        std::vector<PackEntry> lying = entries;
        lying[2].declared_raw = lies[i];
        const std::string lie_pack = path_join(root, "zstd_lie" + std::to_string(i) + ".mprshc");
        CHECK(write_text_file_atomic(lie_pack, build_pack_bytes(lying)));
        const std::string lie_dest = path_join(root, "dest_zstd_lie" + std::to_string(i));
        k = thrown_kind([&] { install_shader_cache(lie_pack, lie_dest, false, nullptr, nullptr); });
        CHECK_EQ(k, static_cast<int>(Fail::Input));
        CHECK(!path_exists(path_join(lie_dest, files[2].name)));
    }

    // A checksum that does not match the raw bytes (the payload itself decodes fine): Input, entry not written.
    std::vector<PackEntry> bad_hash = entries;
    bad_hash[2].wrong_hash = true;
    const std::string hash_pack = path_join(root, "zstd_hash.mprshc");
    CHECK(write_text_file_atomic(hash_pack, build_pack_bytes(bad_hash)));
    const std::string hash_dest = path_join(root, "dest_zstd_hash");
    k = thrown_kind([&] { install_shader_cache(hash_pack, hash_dest, false, nullptr, nullptr); });
    CHECK_EQ(k, static_cast<int>(Fail::Input));
    CHECK(!path_exists(path_join(hash_dest, files[2].name)));

    // A pack built by make_shader_pack.py --zstd from the folder, when Python can be started.
    const std::wstring py = L"C:\\mpr\\tools\\.venv\\Scripts\\python.exe";
    const std::wstring script = L"C:\\mpr\\tools\\setup\\scripts\\make_shader_pack.py";
    if (!path_exists(narrow(py)) || !path_exists(narrow(script))) {
        std::printf("SKIP python zstd pack check: %s or %s is not present\n", narrow(py).c_str(), narrow(script).c_str());
        return;
    }
    const std::string src = path_join(root, "src");
    const std::string py_pack = path_join(root, "py_zstd.mprshc");
    const std::wstring args = L"\"" + script + L"\" \"" + widen(src) + L"\" \"" + widen(py_pack) + L"\" --zstd";
    const int code = run_process(py, args);
    if (code < 0) {
        std::printf("SKIP python zstd pack check: could not start %s\n", narrow(py).c_str());
        return;
    }
    CHECK_EQ(code, 0);
    if (code != 0) return;
    const std::string py_dest = path_join(root, "dest_py_zstd");
    r = install_shader_cache(py_pack, py_dest, true, nullptr, nullptr);
    CHECK_EQ(r.files_installed, 6u);
    for (const Fake& f : files) CHECK(same_content(path_join(py_dest, f.name), f.data));
}
#endif

// test_shadercache install <pack or folder> <dest folder> [verify]: runs the real installer and prints what it did.
int run_install(int argc, char** argv) {
    const bool verify = argc >= 5 && std::string(argv[4]) == "verify";
    const auto t0 = std::chrono::steady_clock::now();
    try {
        ShaderInstallResult r = install_shader_cache(argv[2], argv[3], verify, nullptr, nullptr);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("installed %u files, %u already present, %llu bytes written, %.2f s\n", r.files_installed, r.files_skipped,
                    static_cast<unsigned long long>(r.bytes_written), secs);
        return 0;
    } catch (const Error& e) {
        std::printf("install failed (kind %d): %s\n", static_cast<int>(e.kind), e.what());
        return 1;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 4 && std::string(argv[1]) == "install") return run_install(argc, argv);
    wchar_t tmp[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, tmp);
    if (n == 0 || n > MAX_PATH) {
        std::printf("FAIL: no temp folder\n");
        return 1;
    }
    std::string base = narrow(std::wstring(tmp, n));
    std::string root = path_join(base, "mprs_shc_test_" + std::to_string(GetCurrentProcessId()) + "_" +
                                           std::to_string(GetTickCount64()));
    if (!create_dirs(root)) {
        std::printf("FAIL: cannot create temp folder %s\n", root.c_str());
        return 1;
    }

    test_fileio(root);
    test_folder_source(root);
    test_pack_source(root);
#ifdef MPRS_HAVE_ZSTD
    test_zstd_pack(root);
#endif

    remove_tree(widen(root));
    return TEST_SUMMARY("test_shadercache");
}
