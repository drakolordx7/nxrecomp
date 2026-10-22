// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode: factory settings and the settings file (reader and writer). The file format is described in play_types.h.
#include "play_types.h"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace nxrt::play {
namespace {

const std::string::size_type kNpos = std::string::npos;

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// "%.9g" for doubles, as the file format asks.
std::string num(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.9g", v);
    return buf;
}

// As num(), but a value that would not read back to the same double gets all 17 digits (16/9 needs them).
std::string num_exact(double v) {
    std::string s = num(v);
    if (std::strtod(s.c_str(), nullptr) != v) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.17g", v);
        return buf;
    }
    return s;
}

// Whole numbers and reals: the whole string must be a number, and reals must be finite.
bool to_int(const std::string& v, long long* out) {
    if (v.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const long long x = std::strtoll(v.c_str(), &end, 10);
    if (errno != 0 || end != v.c_str() + v.size()) return false;
    *out = x;
    return true;
}

bool to_double(const std::string& v, double* out) {
    if (v.empty()) return false;
    char* end = nullptr;
    const double x = std::strtod(v.c_str(), &end);
    if (end != v.c_str() + v.size() || !std::isfinite(x)) return false;
    *out = x;
    return true;
}

bool to_bool(const std::string& v, bool* out) {
    const std::string l = lower(v);
    if (l == "1" || l == "true" || l == "yes" || l == "on") {
        *out = true;
        return true;
    }
    if (l == "0" || l == "false" || l == "no" || l == "off") {
        *out = false;
        return true;
    }
    return false;
}

// Splits on '\n'. Line i of the result is line i+1 of the file (for error messages).
std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t nl = text.find('\n', pos);
        if (nl == kNpos) {
            lines.push_back(text.substr(pos));
            break;
        }
        lines.push_back(text.substr(pos, nl - pos));
        pos = nl + 1;
    }
    return lines;
}

// Cuts a trailing comment: a '#' or ';' that follows a space or tab.
std::string strip_inline_comment(const std::string& line) {
    for (size_t i = 1; i < line.size(); ++i) {
        if ((line[i] == '#' || line[i] == ';') && (line[i - 1] == ' ' || line[i - 1] == '\t')) return trim(line.substr(0, i));
    }
    return line;
}

// The settings with the built-in tables only: the outputs from ActInfo, the struct defaults, and no bindings.
Settings builtin_settings() {
    Settings s;
    for (int i = 0; i < kActCount; ++i) {
        Output o;
        parse_output(act_info(static_cast<Act>(i)).default_out, &o, nullptr);   // our own table, always valid
        s.out[i] = o;
    }
    return s;
}

enum class Sec { None, Unknown, Mouse, Pad, Timing, Display, Switch, Bindings };

const char* sec_label(Sec s) {
    switch (s) {
        case Sec::Mouse: return "[mouse]";
        case Sec::Pad: return "[pad]";
        case Sec::Timing: return "[timing]";
        case Sec::Display: return "[display]";
        case Sec::Switch: return "[switch]";
        default: return "";
    }
}

// Applies one `key = value` of a fixed section. On a bad value returns false with *msg set (no line prefix).
// *known is false for a key the section does not have.
bool apply_fixed(Sec sec, const std::string& k, const std::string& v, Settings* s, bool* known, std::string* msg) {
    *known = true;
    auto set_num = [&](double* dst) {
        double x = 0;
        if (!to_double(v, &x)) {
            *msg = k + " must be a number, got '" + v + "'";
            return false;
        }
        *dst = x;
        return true;
    };
    auto set_real = [&](double lo, double hi, double* dst) {
        double x = 0;
        if (!to_double(v, &x)) {
            *msg = k + " must be a number, got '" + v + "'";
            return false;
        }
        if (x < lo || x > hi) {
            *msg = k + " must be " + num(lo) + ".." + num(hi) + ", got " + v;
            return false;
        }
        *dst = x;
        return true;
    };
    auto set_int = [&](long long lo, long long hi, int* dst) {
        long long x = 0;
        if (!to_int(v, &x)) {
            *msg = k + " must be a whole number, got '" + v + "'";
            return false;
        }
        if (x < lo || x > hi) {
            *msg = k + " must be " + std::to_string(lo) + ".." + std::to_string(hi) + ", got " + v;
            return false;
        }
        *dst = static_cast<int>(x);
        return true;
    };
    auto set_bool = [&](bool* dst) {
        if (!to_bool(v, dst)) {
            *msg = k + " must be 1/0 (or true/false, yes/no, on/off), got '" + v + "'";
            return false;
        }
        return true;
    };
    // A fixed render size: 0 (follow the window) or 320..16384.
    auto set_size = [&](int* dst) {
        long long x = 0;
        if (!to_int(v, &x)) {
            *msg = k + " must be a whole number, got '" + v + "'";
            return false;
        }
        if (x != 0 && (x < 320 || x > 16384)) {
            *msg = k + " must be 0 or 320..16384, got " + v;
            return false;
        }
        *dst = static_cast<int>(x);
        return true;
    };
    // The forced aspect ratio: 0 (auto) or 1.0..5.0.
    auto set_aspect = [&](double* dst) {
        double x = 0;
        if (!to_double(v, &x)) {
            *msg = k + " must be a number, got '" + v + "'";
            return false;
        }
        if (x != 0 && (x < 1.0 || x > 5.0)) {
            *msg = k + " must be 0 (auto) or 1.0..5.0, got " + v;
            return false;
        }
        *dst = x;
        return true;
    };

    switch (sec) {
        case Sec::Mouse:
            if (k == "yaw") return set_num(&s->mouse.yaw_rad);
            if (k == "pitch") return set_num(&s->mouse.pitch_rad);
            if (k == "morph_yaw") return set_num(&s->mouse.morph_yaw_rad);
            if (k == "morph_pitch") return set_num(&s->mouse.morph_pitch_rad);
            if (k == "yaw_source" || k == "pitch_source") {   // Source scale, converted to radians per count
                double x = 0;
                if (!set_num(&x)) return false;
                (k == "yaw_source" ? s->mouse.yaw_rad : s->mouse.pitch_rad) = rad_per_count(x);
                return true;
            }
            if (k == "invert_y") return set_bool(&s->mouse.invert_y);
            if (k == "invert_x") return set_bool(&s->mouse.invert_x);
            if (k == "menu_touch") return set_bool(&s->mouse.menu_touch);
            if (k == "wheel_beams") return set_bool(&s->mouse.wheel_beams);
            break;
        case Sec::Pad:
            if (k == "label_abxy") return set_bool(&s->pad.label_abxy);
            if (k == "deadzone") return set_real(0.0, 0.5, &s->pad.stick_deadzone);
            if (k == "trigger_threshold") return set_real(0.05, 1.0, &s->pad.trigger_threshold);
            if (k == "gyro") return set_bool(&s->pad.gyro);
            if (k == "gyro_scale") return set_num(&s->pad.gyro_scale);
            if (k == "rumble") return set_bool(&s->pad.rumble);
            if (k == "rumble_scale") return set_num(&s->pad.rumble_scale);
            break;
        case Sec::Timing:
            if (k == "tap_ms") return set_int(1, 5000, &s->timing.tap_ms);
            if (k == "hold_ms") return set_int(1, 5000, &s->timing.hold_ms);
            if (k == "double_ms") return set_int(1, 5000, &s->timing.double_ms);
            if (k == "pulse_ms") return set_int(1, 5000, &s->timing.pulse_ms);
            break;
        case Sec::Display:
            if (k == "window_mode") {
                const std::string l = lower(v);
                if (l != "windowed" && l != "borderless" && l != "exclusive") {
                    *msg = "window_mode must be windowed, borderless or exclusive, got '" + v + "'";
                    return false;
                }
                s->display.window_mode = l;
                return true;
            }
            if (k == "vsync") return set_bool(&s->display.vsync);
            if (k == "fps_cap") return set_int(0, 1000, &s->display.fps_cap);
            if (k == "width") return set_int(320, 16384, &s->display.width);
            if (k == "height") return set_int(320, 16384, &s->display.height);
            if (k == "keep_aspect") return set_bool(&s->display.keep_aspect);
            if (k == "render_scale") return set_int(25, 400, &s->display.render_scale);
            if (k == "render_width") return set_size(&s->display.render_width);
            if (k == "render_height") return set_size(&s->display.render_height);
            if (k == "aspect") return set_aspect(&s->display.aspect);
            if (k == "fov_offset") return set_real(-30.0, 40.0, &s->display.fov_offset);
            if (k == "dynamic_resolution") return set_bool(&s->display.dynamic_resolution);
            if (k == "anisotropy") {
                long long x = 0;
                if (!to_int(v, &x)) {
                    *msg = k + " must be a whole number, got '" + v + "'";
                    return false;
                }
                if (x != 0 && x != 2 && x != 4 && x != 8 && x != 16) {
                    *msg = "anisotropy must be 0, 2, 4, 8 or 16, got " + v;
                    return false;
                }
                s->display.anisotropy = static_cast<int>(x);
                return true;
            }
            if (k == "lod_bias") return set_real(-2.0, 2.0, &s->display.lod_bias);
            if (k == "bloom") return set_bool(&s->display.bloom);
            if (k == "chromatic_aberration") return set_bool(&s->display.chromatic_aberration);
            break;
        case Sec::Switch: {
            Act a = Act::None;
            if (!parse_act(k, &a) || a == Act::None) break;   // unknown action name: a warning
            Output o;
            std::string perr;
            if (!parse_output(v, &o, &perr)) {
                *msg = perr;
                return false;
            }
            s->out[static_cast<int>(a)] = o;
            return true;
        }
        default:
            break;
    }
    *known = false;
    return true;
}

// The core reader. Applies `text` on top of `*s`. Only writes what it parsed into `*s`; the caller decides what to keep.
// If the text has no [bindings.*] section and with_default_bindings is set, the factory bindings are loaded first.
bool parse_onto(const std::string& text, Settings* s, std::string* err, std::vector<std::string>* warnings, bool with_default_bindings) {
    const std::vector<std::string> lines = split_lines(text);

    // Any [bindings.*] section means the file is the whole truth about bindings, so the defaults never apply.
    bool has_bindings = false;
    for (const std::string& raw : lines) {
        const std::string l = trim(raw);
        if (!l.empty() && l[0] == '[' && lower(l).compare(0, 10, "[bindings.") == 0) has_bindings = true;
    }
    if (has_bindings) {
        s->bindings.clear();
    } else if (with_default_bindings) {
        Settings factory = builtin_settings();
        std::string ferr;
        // The factory text has bindings sections, so this inner call never comes back here.
        if (!parse_onto(default_settings_text(), &factory, &ferr, nullptr, false)) {
            if (err) *err = "internal: factory bindings do not parse: " + ferr;
            return false;
        }
        s->bindings = factory.bindings;
    }

    auto bail = [&](size_t i, const std::string& m) {
        if (err) *err = "line " + std::to_string(i + 1) + ": " + m;
    };
    auto warn = [&](size_t i, const std::string& m) {
        if (warnings) warnings->push_back("line " + std::to_string(i + 1) + ": " + m);
    };

    Sec sec = Sec::None;
    Ctx ctx = Ctx::All;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string line = trim(lines[i]);
        if (line.empty()) continue;

        if (line[0] == '#' || line[0] == ';') {
            // Full-line comment. Exception: in a bindings section the semicolon key (";") is a real binding; a line whose key
            // before the last '=' parses as an input is kept.
            bool binding_line = false;
            if (sec == Sec::Bindings) {
                const size_t eq = line.rfind('=');
                InputId probe;
                binding_line = eq != kNpos && parse_input(trim(line.substr(0, eq)), &probe);
            }
            if (!binding_line) continue;
        } else {
            line = strip_inline_comment(line);
        }

        if (line[0] == '[') {
            if (line.back() != ']') {
                bail(i, "section header without ']'");
                return false;
            }
            const std::string name = trim(line.substr(1, line.size() - 2));
            const std::string key = lower(name);
            if (key == "mouse") sec = Sec::Mouse;
            else if (key == "pad") sec = Sec::Pad;
            else if (key == "timing") sec = Sec::Timing;
            else if (key == "display") sec = Sec::Display;
            else if (key == "switch") sec = Sec::Switch;
            else if (key.compare(0, 9, "bindings.") == 0 && parse_ctx(name.substr(9), &ctx)) sec = Sec::Bindings;
            else {
                sec = Sec::Unknown;
                warn(i, "unknown section '[" + name + "]'");
            }
            continue;
        }

        if (sec == Sec::Unknown) continue;   // the keys of an unknown section are skipped

        // Bindings keep the key before the LAST '=' (the key may itself be "="); their value never contains '='.
        const size_t eq = sec == Sec::Bindings ? line.rfind('=') : line.find('=');
        if (eq == kNpos) {
            bail(i, "expected 'key = value', got '" + line + "'");
            return false;
        }
        const std::string key = trim(line.substr(0, eq));
        const std::string val = trim(line.substr(eq + 1));
        if (sec == Sec::None) {
            warn(i, "'" + key + "' is outside any section");
            continue;
        }

        if (sec == Sec::Bindings) {
            InputId id;
            if (!parse_input(key, &id)) {
                bail(i, "unknown input '" + key + "'");
                return false;
            }
            // value: action [trigger [ms]]
            std::vector<std::string> tok;
            std::istringstream words(val);
            for (std::string w; words >> w;) tok.push_back(w);
            if (tok.empty() || tok.size() > 3) {
                bail(i, "expected '<action> [trigger [ms]]', got '" + val + "'");
                return false;
            }
            Binding b;
            b.in = id;
            b.ctx = ctx;
            if (!parse_act(tok[0], &b.act)) {
                bail(i, "unknown action '" + tok[0] + "'");
                return false;
            }
            if (tok.size() >= 2 && !parse_trig(tok[1], &b.trig)) {
                bail(i, "unknown trigger '" + tok[1] + "'");
                return false;
            }
            if (tok.size() == 3) {
                long long ms = 0;
                if (!to_int(tok[2], &ms) || ms < 1 || ms > 5000) {
                    bail(i, "ms must be 1..5000, got '" + tok[2] + "'");
                    return false;
                }
                b.ms = static_cast<int>(ms);
            }
            s->bindings.push_back(b);
            continue;
        }

        bool known = false;
        std::string msg;
        if (!apply_fixed(sec, lower(key), val, s, &known, &msg)) {
            bail(i, msg);
            return false;
        }
        if (!known) warn(i, "unknown key '" + key + "' in " + sec_label(sec));
    }
    return true;
}

}  // namespace

Settings default_settings() {
    Settings s;
    std::string err;
    // The factory text has a [bindings.*] section, so parse_settings does not look at the factory bindings again.
    if (!parse_settings(default_settings_text(), &s, &err)) return builtin_settings();
    return s;
}

bool parse_settings(const std::string& text, Settings* out, std::string* err, std::vector<std::string>* warnings) {
    Settings s = builtin_settings();
    std::string perr;
    if (!parse_onto(text, &s, &perr, warnings, true)) {
        if (err) *err = perr;
        return false;
    }
    *out = s;
    return true;
}

std::string format_settings(const Settings& s) {
    std::string o;
    auto kv = [&o](const char* key, const std::string& value) { o += std::string(key) + " = " + value + "\n"; };
    auto flag = [](bool b) { return std::string(b ? "1" : "0"); };

    o += "# Metroid Prime Remastered, play mode: controls and display.\n";
    o += "\n[mouse]\n";
    kv("yaw", num(s.mouse.yaw_rad));
    kv("pitch", num(s.mouse.pitch_rad));
    kv("morph_yaw", num(s.mouse.morph_yaw_rad));
    kv("morph_pitch", num(s.mouse.morph_pitch_rad));
    kv("invert_y", flag(s.mouse.invert_y));
    kv("invert_x", flag(s.mouse.invert_x));
    kv("menu_touch", flag(s.mouse.menu_touch));
    kv("wheel_beams", flag(s.mouse.wheel_beams));

    o += "\n[pad]\n";
    kv("label_abxy", flag(s.pad.label_abxy));
    kv("deadzone", num(s.pad.stick_deadzone));
    kv("trigger_threshold", num(s.pad.trigger_threshold));
    kv("gyro", flag(s.pad.gyro));
    kv("gyro_scale", num(s.pad.gyro_scale));
    kv("rumble", flag(s.pad.rumble));
    kv("rumble_scale", num(s.pad.rumble_scale));

    o += "\n[timing]\n";
    kv("tap_ms", std::to_string(s.timing.tap_ms));
    kv("hold_ms", std::to_string(s.timing.hold_ms));
    kv("double_ms", std::to_string(s.timing.double_ms));
    kv("pulse_ms", std::to_string(s.timing.pulse_ms));

    o += "\n[display]\n";
    kv("window_mode", s.display.window_mode);
    kv("vsync", flag(s.display.vsync));
    kv("fps_cap", std::to_string(s.display.fps_cap));
    kv("width", std::to_string(s.display.width));
    kv("height", std::to_string(s.display.height));
    kv("keep_aspect", flag(s.display.keep_aspect));
    kv("render_scale", std::to_string(s.display.render_scale));
    kv("render_width", std::to_string(s.display.render_width));
    kv("render_height", std::to_string(s.display.render_height));
    kv("aspect", num_exact(s.display.aspect));
    kv("fov_offset", num_exact(s.display.fov_offset));
    kv("dynamic_resolution", flag(s.display.dynamic_resolution));
    kv("anisotropy", std::to_string(s.display.anisotropy));
    kv("lod_bias", num_exact(s.display.lod_bias));
    kv("bloom", flag(s.display.bloom));
    kv("chromatic_aberration", flag(s.display.chromatic_aberration));

    o += "\n[switch]\n";
    for (int a = 1; a < kActCount; ++a) {   // every action except none
        kv(act_info(static_cast<Act>(a)).name, output_text(s.out[a]));
    }

    // Always written, even when empty, so that a file with every binding removed does not fall back to the defaults.
    for (int c = 0; c < kCtxCount; ++c) {
        o += std::string("\n[bindings.") + ctx_name(static_cast<Ctx>(c)) + "]\n";
        for (const Binding& b : s.bindings) {
            if (static_cast<int>(b.ctx) != c) continue;
            const std::string key = input_name(b.in);
            if (key.empty()) continue;   // an input with no name cannot be written; it cannot be read back either
            o += key + " = " + act_info(b.act).name;
            if (b.trig != Trig::Press || b.ms != 0) o += std::string(" ") + trig_name(b.trig);
            if (b.ms != 0) o += " " + std::to_string(b.ms);
            o += "\n";
        }
    }
    return o;
}

bool load_settings(const std::string& path, Settings* out, std::string* err) {
    namespace fs = std::filesystem;
    if (err) err->clear();
    std::error_code ec;
    const bool exists = fs::exists(path, ec);
    if (ec) {
        *out = default_settings();
        if (err) *err = "cannot check " + path + ": " + ec.message();
        return false;
    }
    if (!exists) {   // a first run: no file yet, not an error
        *out = default_settings();
        return false;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *out = default_settings();
        if (err) *err = "cannot open " + path;
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);   // a UTF-8 byte order mark, written by some editors

    Settings s;
    std::string perr;
    if (!parse_settings(text, &s, &perr)) {
        *out = default_settings();
        if (err) *err = path + ": " + perr;
        return false;
    }
    *out = s;
    return true;
}

bool save_settings(const std::string& path, const Settings& s, std::string* err) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path target(path);
    if (target.has_parent_path()) {
        fs::create_directories(target.parent_path(), ec);
        if (ec) {
            if (err) *err = "cannot create folder for " + path + ": " + ec.message();
            return false;
        }
    }
    // Write the whole file to a temp name first, then rename it over the target, so a crash never leaves half a file.
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (err) *err = "cannot write " + tmp;
            return false;
        }
        out << format_settings(s);
        out.flush();
        if (!out) {
            if (err) *err = "cannot write " + tmp;
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, target, ec);   // replaces an existing file on Windows
    if (ec) {
        std::error_code rm_ec;
        fs::remove(tmp, rm_ec);
        if (err) *err = "cannot replace " + path + ": " + ec.message();
        return false;
    }
    return true;
}

}  // namespace nxrt::play
