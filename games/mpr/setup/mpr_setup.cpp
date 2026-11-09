// SPDX-License-Identifier: GPL-3.0-or-later
// Metroid Prime Remastered's game hook for the setup tool (mpr_setup.h): the TXTR walk over the .pak archives, the shader program scan and
// the disk estimate. Retro's formats are read by pak.* and txtr.* (the runtime's button prompts use the same readers).
#include "mpr_setup.h"

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <filesystem>

#include "fileio.h"
#include "log.h"
#include "programs.h"
#include "storage.h"

namespace nxs::mpr {
namespace {

std::vector<std::filesystem::path> pak_files(const std::string& romfs_dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root(widen(romfs_dir));
    if (!fs::is_directory(root, ec)) fail(Fail::Input, "The game files folder %s does not exist.", romfs_dir.c_str());
    std::vector<fs::path> paths;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        if (_wcsicmp(it->path().extension().wstring().c_str(), L".pak") == 0) paths.push_back(it->path());
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

TexTarget target_of(TxtrKind k) {
    switch (k) {
        case TxtrKind::D2: return TexTarget::D2;
        case TxtrKind::D3: return TexTarget::D3;
        case TxtrKind::Cube: return TexTarget::Cube;
        case TxtrKind::D2Array: return TexTarget::D2Array;
        default: return TexTarget::Unsupported;
    }
}

}  // namespace

AstcTexture astc_texture_of(const TxtrInfo& info, std::function<std::vector<u8>()> read_tiled) {
    const TxtrFormat& f = txtr_format(info.format);
    AstcTexture t;
    t.name = info.id.str();
    t.target = target_of(info.kind);
    t.nvn_format = f.astc && f.bytes == 16 ? static_cast<u32>(nvn_astc_format(f.block_w, f.block_h, f.srgb)) : 0;
    t.block_w = f.block_w;
    t.block_h = f.block_h;
    t.srgb = f.srgb;
    t.width = info.width;
    t.height = info.height;
    t.layers = info.layers;
    t.mips = info.mips;
    t.gpu_size = info.gpu_size;
    t.weight = static_cast<u64>(info.width) * info.height * std::max<u32>(1, info.layers);
    t.read_tiled = std::move(read_tiled);
    return t;
}

AstcTexture astc_texture_of(const TxtrInfo& info, std::shared_ptr<const Pak> pak, const PakEntry& entry) {
    return astc_texture_of(info, [pak, entry, info] { return txtr_read_tiled(*pak, entry, info); });
}

TextureList list_textures(const std::string& romfs_dir, const std::function<bool()>& cancelled) {
    TextureList out;
    for (const std::filesystem::path& pp : pak_files(romfs_dir)) {
        if (cancelled && cancelled()) fail(Fail::Cancelled, "Stopped by the user.");
        const std::string utf8 = narrow(pp.wstring());
        std::shared_ptr<const Pak> pak;
        try {
            pak = Pak::open(open_file_storage(utf8));
        } catch (const std::exception& e) {
            NXS_LOGW("Skipping %s: %s", utf8.c_str(), e.what());
            continue;
        }
        for (const PakEntry& e : pak->assets()) {
            if (std::strcmp(e.type, "TXTR") != 0) continue;
            try {
                const TxtrInfo info = txtr_read_info(*pak, e);
                if (!txtr_format(info.format).astc) continue;
                out.textures.push_back(astc_texture_of(info, pak, e));
            } catch (const std::exception& err) {
                ++out.failed;
                NXS_LOGW("Texture %s in %s: header unreadable, not cached: %s", e.id.str().c_str(), utf8.c_str(), err.what());
            }
        }
    }
    return out;
}

ProgramList list_programs(const std::string& romfs_dir, int threads, const std::function<bool()>& cancelled) {
    ProgramScanStats st;
    ProgramList out;
    out.programs = scan_shader_programs(romfs_dir, threads, [](u64, u64) {}, cancelled, &st);
    char text[160];
    std::snprintf(text, sizeof text, "%u from MaterialArchive.arc, %u from paks, %u of them duplicates", st.arc.programs, st.pak.programs,
                  static_cast<unsigned>(st.arc.programs + st.pak.programs - out.programs.size()));
    out.summary = text;
    return out;
}

}  // namespace nxs::mpr

namespace nxs {

const GameSetup* game_setup() {
    static const GameSetup hook = [] {
        GameSetup h;
        h.texture_cache = mpr::list_textures;
        h.program_list = mpr::list_programs;
        // measured 4.35 GB of BC7 for the 4,369 ASTC textures of v1.0.0; 1,008 MB of raw SPIR-V + results.tsv
        h.disk_estimate = [] { return DiskEstimate{4'500'000'000ull, 1'100'000'000ull}; };
        return h;
    }();
    return &hook;
}

}  // namespace nxs
