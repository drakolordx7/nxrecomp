// SPDX-License-Identifier: GPL-3.0-or-later
// texq: real-data BC7 quality probe. For every ASTC texture of a RomFS folder (level 0, layer 0): decode the ASTC blocks to RGBA8
// (astcenc via astc2bc7), encode BC7 with each requested configuration, decode that BC7 with the reference decoder
// (bc7enc_rdo bc7decomp_ref, the oracle's) and compare with the ASTC decode: PSNR RGB / A and SSIM on Rec.601 luma (Gaussian
// 11x11, sigma 1.5, K1 0.01, K2 0.03: the metric of tools/textures/oracle/metrics.py).
//
//   mpr_texq --romfs DIR --configs q3,q3m,q5 [--stride N] [--threads N] [--out file.tsv] [--dump DIR --dump-below DB]
// A configuration is "q<0-6>" plus letters: p = perceptual weights, r = raw bc7e on opaque blocks (alpha may drift), l = legacy
// (mode 6 and rotation off for opaque blocks), No letter: the library default (mode 6 off for opaque
// blocks, rotation on, alpha of rotated blocks fixed up to exactly 255).
// Output: one TSV row per texture (properties, then psnr_rgb psnr_a ssim ms for every configuration).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "astc2bc7.h"
#include "bc7decomp.h"
#include "log.h"
#include "pak.h"
#include "storage.h"
#include "texcache.h"
#include "txtr.h"
#include "common.h"

using namespace mprs;

namespace {

struct Config {
    std::string name;
    a2b_options opt{};
};

bool parse_config(const std::string& s, Config& c) {
    if (s.size() < 2 || s[0] != 'q' || s[1] < '0' || s[1] > '6') return false;
    c.name = s;
    c.opt = a2b_options{};
    c.opt.quality = s[1] - '0';
    c.opt.threads = 1;
    for (size_t i = 2; i < s.size(); ++i) {
        if (s[i] == 'p') c.opt.perceptual = 1;
        else if (s[i] == 'r') c.opt.allow_opaque_mode6 = 1;   // raw bc7e
        else if (s[i] == 'l') c.opt.allow_opaque_mode6 = 2;   // legacy: mode 6 off
        else return false;
    }
    return true;
}

double psnr_from_mse(double mse) { return mse <= 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse); }

// Gaussian-window SSIM of two float planes (scipy gaussian_filter, mode reflect, radius 5).
double ssim_plane(const std::vector<float>& x, const std::vector<float>& y, int w, int h) {
    static float k[11];
    static bool init = false;
    if (!init) {
        float sum = 0;
        for (int i = -5; i <= 5; ++i) sum += (k[i + 5] = std::exp(-0.5f * i * i / (1.5f * 1.5f)));
        for (float& v : k) v /= sum;
        init = true;
    }
    const size_t n = static_cast<size_t>(w) * h;
    auto refl = [](int i, int n) {
        if (n == 1) return 0;
        const int p = 2 * n;
        i %= p;
        if (i < 0) i += p;
        return i < n ? i : p - 1 - i;
    };
    std::vector<float> tmp(n), outp(n);
    auto blur = [&](const std::vector<float>& in, std::vector<float>& out) {
        for (int yy = 0; yy < h; ++yy)
            for (int xx = 0; xx < w; ++xx) {
                float a = 0;
                for (int t = -5; t <= 5; ++t) a += k[t + 5] * in[static_cast<size_t>(yy) * w + refl(xx + t, w)];
                tmp[static_cast<size_t>(yy) * w + xx] = a;
            }
        for (int yy = 0; yy < h; ++yy)
            for (int xx = 0; xx < w; ++xx) {
                float a = 0;
                for (int t = -5; t <= 5; ++t) a += k[t + 5] * tmp[static_cast<size_t>(refl(yy + t, h)) * w + xx];
                out[static_cast<size_t>(yy) * w + xx] = a;
            }
    };
    std::vector<float> mx(n), my(n), sxx(n), syy(n), sxy(n), t(n);
    blur(x, mx);
    blur(y, my);
    for (size_t i = 0; i < n; ++i) t[i] = x[i] * x[i];
    blur(t, sxx);
    for (size_t i = 0; i < n; ++i) t[i] = y[i] * y[i];
    blur(t, syy);
    for (size_t i = 0; i < n; ++i) t[i] = x[i] * y[i];
    blur(t, sxy);
    const double c1 = (0.01 * 255) * (0.01 * 255), c2 = (0.03 * 255) * (0.03 * 255);
    double acc = 0;
    for (size_t i = 0; i < n; ++i) {
        const double vx = sxx[i] - mx[i] * mx[i], vy = syy[i] - my[i] * my[i], cv = sxy[i] - mx[i] * my[i];
        acc += ((2.0 * mx[i] * my[i] + c1) * (2.0 * cv + c2)) / ((double)mx[i] * mx[i] + (double)my[i] * my[i] + c1) / (vx + vy + c2);
    }
    return acc / static_cast<double>(n);
}

struct Metrics {
    double psnr_rgb = 0, psnr_a = 0, ssim = 0;
    double psnr_r = 0, psnr_g = 0, psnr_b = 0;
    double ms = 0;
    int modes[8] = {};
};

void decode_bc7(const std::vector<u8>& bc7, int w, int h, std::vector<u8>& rgba, int* modes) {
    const int bx = (w + 3) / 4, by = (h + 3) / 4;
    rgba.assign(static_cast<size_t>(w) * h * 4, 0);
    for (int y = 0; y < by; ++y)
        for (int x = 0; x < bx; ++x) {
            const u8* bp = bc7.data() + (static_cast<size_t>(y) * bx + x) * 16;
            bc7decomp::color_rgba blk[16];
            if (!bc7decomp_ref::unpack_bc7(bp, blk)) std::memset(blk, 0, sizeof blk);
            if (modes) {
                int m = 0;
                while (m < 8 && !(bp[0] & (1 << m))) ++m;
                if (m < 8) ++modes[m];
            }
            for (int j = 0; j < 4; ++j)
                for (int i = 0; i < 4; ++i) {
                    const int px = x * 4 + i, py = y * 4 + j;
                    if (px >= w || py >= h) continue;
                    u8* d = &rgba[(static_cast<size_t>(py) * w + px) * 4];
                    const bc7decomp::color_rgba& c = blk[j * 4 + i];
                    d[0] = c.r;
                    d[1] = c.g;
                    d[2] = c.b;
                    d[3] = c.a;
                }
        }
}

Metrics measure(const std::vector<u8>& ref, const std::vector<u8>& dec, int w, int h) {
    Metrics m;
    const size_t n = static_cast<size_t>(w) * h;
    double se[4] = {0, 0, 0, 0};
    std::vector<float> lx(n), ly(n);
    for (size_t i = 0; i < n; ++i) {
        for (int c = 0; c < 4; ++c) {
            const double d = double(ref[i * 4 + c]) - dec[i * 4 + c];
            se[c] += d * d;
        }
        lx[i] = 0.299f * ref[i * 4] + 0.587f * ref[i * 4 + 1] + 0.114f * ref[i * 4 + 2];
        ly[i] = 0.299f * dec[i * 4] + 0.587f * dec[i * 4 + 1] + 0.114f * dec[i * 4 + 2];
    }
    m.psnr_r = psnr_from_mse(se[0] / n);
    m.psnr_g = psnr_from_mse(se[1] / n);
    m.psnr_b = psnr_from_mse(se[2] / n);
    m.psnr_rgb = psnr_from_mse((se[0] + se[1] + se[2]) / (3.0 * n));
    m.psnr_a = psnr_from_mse(se[3] / n);
    m.ssim = ssim_plane(lx, ly, w, h);
    return m;
}

struct Props {
    double mean[4] = {}, sd[4] = {};
    double alpha_nonopaque = 0;      // fraction of pixels with alpha != 255
    double alpha_mid = 0;            // fraction with alpha strictly between 0 and 255
    double unit_xyz = 0;             // fraction whose (2c-1) RGB vector has length within 0.1 of 1 (normal map, 3 channels)
    double unit_xy_z = 0;            // same for RG with Z reconstructed (always true if length<=1): fraction with |xy|<=1.02
    double grey = 0;                 // fraction with R==G==B
    int alpha_min = 255;             // smallest alpha value
    size_t alpha_nonop_n = 0;        // pixels with alpha != 255
};

Props props_of(const std::vector<u8>& px, int w, int h) {
    Props p;
    const size_t n = static_cast<size_t>(w) * h;
    double s[4] = {}, s2[4] = {};
    size_t nonop = 0, mid = 0, unit = 0, unitxy = 0, grey = 0;
    for (size_t i = 0; i < n; ++i) {
        const u8* c = &px[i * 4];
        for (int k = 0; k < 4; ++k) {
            s[k] += c[k];
            s2[k] += double(c[k]) * c[k];
        }
        if (c[3] < p.alpha_min) p.alpha_min = c[3];
        if (c[3] != 255) ++nonop;
        if (c[3] != 255 && c[3] != 0) ++mid;
        const double x = c[0] / 127.5 - 1, y = c[1] / 127.5 - 1, z = c[2] / 127.5 - 1;
        if (std::fabs(std::sqrt(x * x + y * y + z * z) - 1.0) < 0.1) ++unit;
        if (x * x + y * y <= 1.04) ++unitxy;
        if (c[0] == c[1] && c[1] == c[2]) ++grey;
    }
    for (int k = 0; k < 4; ++k) {
        p.mean[k] = s[k] / n;
        p.sd[k] = std::sqrt(std::max(0.0, s2[k] / n - p.mean[k] * p.mean[k]));
    }
    p.alpha_nonop_n = nonop;
    p.alpha_nonopaque = double(nonop) / n;
    p.alpha_mid = double(mid) / n;
    p.unit_xyz = double(unit) / n;
    p.unit_xy_z = double(unitxy) / n;
    p.grey = double(grey) / n;
    return p;
}

void write_raw(const std::filesystem::path& p, const std::vector<u8>& v, int w, int h) {
    FILE* f = _wfopen(p.c_str(), L"wb");
    if (!f) return;
    std::fwrite(&w, 4, 1, f);
    std::fwrite(&h, 4, 1, f);
    std::fwrite(v.data(), 1, v.size(), f);
    std::fclose(f);
}

}  // namespace

int wmain(int argc, wchar_t** wargv) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) args.push_back(narrow(wargv[i]));
    std::string romfs, outp, dump, cfgs = "q3";
    int stride = 1, threads = 0;
    double dump_below = 0;
    std::string only;
    for (size_t i = 1; i < args.size(); ++i) {
        auto arg = [&](const char* n) { return args[i] == n && i + 1 < args.size(); };
        if (arg("--romfs")) romfs = args[++i];
        else if (arg("--configs")) cfgs = args[++i];
        else if (arg("--stride")) stride = std::max(1, std::atoi(args[++i].c_str()));
        else if (arg("--threads")) threads = std::atoi(args[++i].c_str());
        else if (arg("--out")) outp = args[++i];
        else if (arg("--dump")) dump = args[++i];
        else if (arg("--dump-below")) dump_below = std::atof(args[++i].c_str());
        else if (arg("--only")) only = args[++i];
        else {
            std::fprintf(stderr, "texq: bad argument %s\n", args[i].c_str());
            return 2;
        }
    }
    if (romfs.empty()) {
        std::fprintf(stderr, "usage: mpr_texq --romfs DIR --configs q3,q5 [--stride N] [--threads N] [--out f.tsv] [--dump DIR --dump-below DB]\n");
        return 2;
    }
    std::vector<Config> configs;
    for (size_t b = 0; b <= cfgs.size();) {
        size_t e = cfgs.find(',', b);
        if (e == std::string::npos) e = cfgs.size();
        Config c;
        if (!parse_config(cfgs.substr(b, e - b), c)) {
            std::fprintf(stderr, "texq: bad configuration '%s'\n", cfgs.substr(b, e - b).c_str());
            return 2;
        }
        configs.push_back(c);
        b = e + 1;
    }
    if (a2b_init(nullptr) != A2B_OK) {
        std::fprintf(stderr, "texq: ASTC decoder DLLs not found next to the exe\n");
        return 1;
    }
    if (!dump.empty()) std::filesystem::create_directories(widen(dump));

    namespace fs = std::filesystem;
    std::vector<fs::path> pak_paths;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(widen(romfs), ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file() && _wcsicmp(it->path().extension().c_str(), L".pak") == 0) pak_paths.push_back(it->path());
    std::sort(pak_paths.begin(), pak_paths.end());
    struct Job {
        std::shared_ptr<const Pak> pak;
        size_t entry;
        TxtrInfo info;
    };
    std::vector<Job> jobs;
    for (const fs::path& pp : pak_paths) {
        std::shared_ptr<const Pak> pak;
        try {
            pak = Pak::open(open_file_storage(narrow(pp.wstring())));
        } catch (...) {
            continue;
        }
        const auto& assets = pak->assets();
        for (size_t ei = 0; ei < assets.size(); ++ei) {
            if (std::strcmp(assets[ei].type, "TXTR") != 0) continue;
            try {
                TxtrInfo info = txtr_read_info(*pak, assets[ei]);
                if (!txtr_format(info.format).astc) continue;
                if (!only.empty()) {
                    bool hit = false;
                    for (size_t b = 0; b <= only.size();) {
                        size_t e = only.find(',', b);
                        if (e == std::string::npos) e = only.size();
                        if (e > b && info.id.str().find(only.substr(b, e - b)) != std::string::npos) hit = true;
                        b = e + 1;
                    }
                    if (!hit) continue;
                }
                jobs.push_back(Job{pak, ei, info});
            } catch (...) {
            }
        }
    }
    // unique by id, stable order
    std::stable_sort(jobs.begin(), jobs.end(), [](const Job& a, const Job& b) { return a.info.id.str() < b.info.id.str(); });
    jobs.erase(std::unique(jobs.begin(), jobs.end(), [](const Job& a, const Job& b) { return a.info.id.str() == b.info.id.str(); }), jobs.end());
    std::vector<Job> picked;
    for (size_t i = 0; i < jobs.size(); i += stride) picked.push_back(jobs[i]);
    std::fprintf(stderr, "texq: %zu ASTC textures, %zu probed, %zu configuration(s)\n", jobs.size(), picked.size(), configs.size());

    struct Row {
        std::string id, fmt, kind;
        int w = 0, h = 0;
        bool srgb = false;
        Props pr;
        std::vector<Metrics> m;
        bool ok = false;
    };
    std::vector<Row> rows(picked.size());
    std::atomic<size_t> next{0}, done{0};
    if (threads <= 0) threads = static_cast<int>(std::thread::hardware_concurrency());
    std::mutex lm;
    auto worker = [&]() {
        for (;;) {
            const size_t idx = next.fetch_add(1);
            if (idx >= picked.size()) return;
            const Job& j = picked[idx];
            Row& r = rows[idx];
            try {
                const PakEntry& e = j.pak->assets()[j.entry];
                const std::vector<u8> tiled = txtr_read_tiled(*j.pak, e, j.info);
                TexLevel0 L0;
                if (!texcache_level0(j.info, tiled.data(), tiled.size(), L0)) continue;
                a2b_input in{};
                in.format = L0.srgb ? A2B_ASTC_SRGB : A2B_ASTC_UNORM;
                in.width = L0.width;
                in.height = L0.height;
                in.block_w = L0.block_w;
                in.block_h = L0.block_h;
                std::vector<u8> ref(static_cast<size_t>(L0.width) * L0.height * 4);
                if (a2b_decode_astc_rgba8(&in, L0.astc.data(), L0.astc.size(), 1, ref.data(), nullptr) != A2B_OK) continue;
                r.id = j.info.id.str();
                r.fmt = txtr_format(j.info.format).name;
                r.kind = std::to_string(static_cast<int>(j.info.kind));
                r.w = static_cast<int>(L0.width);
                r.h = static_cast<int>(L0.height);
                r.srgb = L0.srgb;
                r.pr = props_of(ref, r.w, r.h);
                for (const Config& c : configs) {
                    std::vector<u8> bc7(a2b_output_size(&in));
                    const auto t0 = std::chrono::steady_clock::now();
                    if (a2b_encode_bc7(L0.width, L0.height, ref.data(), &c.opt, bc7.data(), bc7.size(), nullptr) != A2B_OK) {
                        r.m.push_back(Metrics{});
                        continue;
                    }
                    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                    Metrics m;
                    std::vector<u8> dec;
                    int modes[8] = {};
                    decode_bc7(bc7, r.w, r.h, dec, modes);
                    m = measure(ref, dec, r.w, r.h);
                    m.ms = ms;
                    std::memcpy(m.modes, modes, sizeof modes);
                    r.m.push_back(m);
                    if (!dump.empty() && dump_below > 0 && m.psnr_rgb < dump_below) {
                        const fs::path d = fs::path(widen(dump));
                        write_raw(d / widen(r.id + "_ref.rgba"), ref, r.w, r.h);
                        write_raw(d / widen(r.id + "_" + c.name + ".rgba"), dec, r.w, r.h);
                    }
                }
                r.ok = true;
            } catch (const std::exception& ex) {
                std::lock_guard<std::mutex> lk(lm);
                std::fprintf(stderr, "texq: %s: %s\n", j.info.id.str().c_str(), ex.what());
            }
            const size_t d = ++done;
            if (d % 100 == 0) std::fprintf(stderr, "texq: %zu / %zu\n", d, picked.size());
        }
    };
    std::vector<std::thread> pool;
    for (int i = 0; i < threads; ++i) pool.emplace_back(worker);
    for (auto& t : pool) t.join();

    FILE* out = outp.empty() ? stdout : _wfopen(widen(outp).c_str(), L"w");
    if (!out) return 1;
    std::fprintf(out, "id\tfmt\tkind\tw\th\tsrgb\tmeanR\tmeanG\tmeanB\tmeanA\tsdR\tsdG\tsdB\tsdA\talpha_nonop\talpha_mid\tunit_xyz\tunit_xy\tgrey\talpha_min\talpha_nonop_n");
    for (const Config& c : configs)
        std::fprintf(out, "\t%s_rgb\t%s_a\t%s_ssim\t%s_r\t%s_g\t%s_b\t%s_ms\t%s_modes", c.name.c_str(), c.name.c_str(), c.name.c_str(),
                     c.name.c_str(), c.name.c_str(), c.name.c_str(), c.name.c_str(), c.name.c_str());
    std::fprintf(out, "\n");
    for (const Row& r : rows) {
        if (!r.ok) continue;
        std::fprintf(out, "%s\t%s\t%s\t%d\t%d\t%d\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.6f\t%.6f\t%.4f\t%.4f\t%.4f\t%d\t%zu", r.id.c_str(),
                     r.fmt.c_str(), r.kind.c_str(), r.w, r.h, r.srgb ? 1 : 0, r.pr.mean[0], r.pr.mean[1], r.pr.mean[2], r.pr.mean[3],
                     r.pr.sd[0], r.pr.sd[1], r.pr.sd[2], r.pr.sd[3], r.pr.alpha_nonopaque, r.pr.alpha_mid, r.pr.unit_xyz, r.pr.unit_xy_z,
                     r.pr.grey, r.pr.alpha_min, r.pr.alpha_nonop_n);
        for (const Metrics& m : r.m) {
            std::fprintf(out, "\t%.2f\t%.2f\t%.4f\t%.2f\t%.2f\t%.2f\t%.1f\t", m.psnr_rgb, m.psnr_a, m.ssim, m.psnr_r, m.psnr_g, m.psnr_b, m.ms);
            for (int k = 0; k < 8; ++k) std::fprintf(out, "%s%d", k ? "/" : "", m.modes[k]);
        }
        std::fprintf(out, "\n");
    }
    if (out != stdout) std::fclose(out);
    return 0;
}
