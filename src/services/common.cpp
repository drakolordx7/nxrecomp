// SPDX-License-Identifier: GPL-3.0-or-later
// Shared pieces of the service library: registration, events, shared memory, the title profile and the host folders.
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <mutex>
#include <set>

#include "nxrt/config.h"
#include "nxrt/ipc.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/services.h"
#include "services_internal.h"

namespace nxrt::services {

namespace {
std::mutex g_mu;
std::set<std::string>& names() {
    static std::set<std::string> s;
    return s;
}
}  // namespace

void add(const std::string& name, ipc::ServiceFactory f) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        names().insert(name);
    }
    ipc::register_service(name, std::move(f));
}

std::shared_ptr<ipc::Service> make_iface(const char* name, const std::function<void(ipc::Service&)>& setup) {
    auto s = std::make_shared<ipc::Service>(name);
    setup(*s);
    return s;
}

void register_all() {
    register_fs();
    register_settings();
    register_time();
    register_power();
    register_logfault();
    register_am();
    register_acc();
    register_hid();
    register_pl();
    register_network();
    register_misc();
    register_audio();
    register_vi_nv();
}

void reset_all() {
    fs_reset();
    settings_reset();
    time_reset();
    power_reset();
    logfault_reset();
    am_reset();
    acc_reset();
    hid_reset();
    pl_reset();
    network_reset();
    misc_reset();
    audio_reset();
    vi_nv_reset();
    profile_reload();
}

std::vector<std::string> registered_names() {
    std::lock_guard<std::mutex> lk(g_mu);
    return std::vector<std::string>(names().begin(), names().end());
}

// ---- ServiceEvent --------------------------------------------------------------------------------------------------------------

ServiceEvent::ServiceEvent(bool signaled) : st_(std::make_shared<kernel::EventState>()) { st_->signaled = signaled; }

uint32_t ServiceEvent::new_handle() const {
    auto e = std::make_shared<kernel::KEvent>();
    e->state = st_;
    e->writable = false;
    return kernel::handle_add(std::move(e));
}

void ServiceEvent::signal() {
    st_->signaled = true;
    kernel::notify_waitable_changed();
}

void ServiceEvent::clear() { st_->signaled = false; }

bool ServiceEvent::signaled() const { return st_->signaled; }

// ---- ServiceSharedMemory ---------------------------------------------------------------------------------------------------------

ServiceSharedMemory::ServiceSharedMemory(uint64_t size, const char* name) {
    const uint64_t rounded = (size + mem::kPage - 1) & ~(mem::kPage - 1);
    const uint64_t host = mem::alloc_kernel(rounded);
    if (!host) {
        NXLOG_E("services", "shared memory '%s' (%llu bytes): kernel region exhausted", name, static_cast<unsigned long long>(size));
        return;
    }
    obj_ = std::make_shared<kernel::KSharedMemory>();
    obj_->size = rounded;
    obj_->host = host;
    obj_->name = name;
}

uint32_t ServiceSharedMemory::new_handle() const { return obj_ ? kernel::handle_add(obj_) : 0; }

// ---- title profile ---------------------------------------------------------------------------------------------------------------

namespace {

struct LangName {
    const char* name;
    Language lang;
    const char* code;
};
constexpr LangName kLangs[] = {
    {"Japanese", Language::Japanese, "ja"},
    {"AmericanEnglish", Language::AmericanEnglish, "en-US"},
    {"French", Language::French, "fr"},
    {"German", Language::German, "de"},
    {"Italian", Language::Italian, "it"},
    {"Spanish", Language::Spanish, "es"},
    {"Chinese", Language::Chinese, "zh-CN"},
    {"Korean", Language::Korean, "ko"},
    {"Dutch", Language::Dutch, "nl"},
    {"Portuguese", Language::Portuguese, "pt"},
    {"Russian", Language::Russian, "ru"},
    {"Taiwanese", Language::Taiwanese, "zh-TW"},
    {"BritishEnglish", Language::BritishEnglish, "en-GB"},
    {"CanadianFrench", Language::CanadianFrench, "fr-CA"},
    {"LatinAmericanSpanish", Language::LatinAmericanSpanish, "es-419"},
    {"SimplifiedChinese", Language::SimplifiedChinese, "zh-Hans"},
    {"TraditionalChinese", Language::TraditionalChinese, "zh-Hant"},
    {"BrazilianPortuguese", Language::BrazilianPortuguese, "pt-BR"},
};

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}

bool parse_language(const std::string& s, Language& out) {
    const std::string l = lower(trim(s));
    for (const LangName& n : kLangs)
        if (lower(n.name) == l || lower(n.code) == l) {
            out = n.lang;
            return true;
        }
    return false;
}

int parse_region(const std::string& s, int def) {
    const std::string l = lower(trim(s));
    if (l.empty()) return def;
    if (std::isdigit(static_cast<unsigned char>(l[0]))) return static_cast<int>(std::strtol(l.c_str(), nullptr, 0));
    static const char* kNames[] = {"japan", "usa", "europe", "australia", "china", "korea", "taiwan"};
    for (int i = 0; i < 7; ++i)
        if (l == kNames[i]) return i;
    if (l == "us") return 1;
    return def;
}

uint32_t default_language_mask() {
    // control.nacp SupportedLanguage of Metroid Prime Remastered 1.0.0: 13 languages.
    uint32_t m = 0;
    for (Language l : {Language::AmericanEnglish, Language::BritishEnglish, Language::Japanese, Language::French, Language::German,
                       Language::LatinAmericanSpanish, Language::Spanish, Language::Italian, Language::Dutch, Language::CanadianFrench,
                       Language::Korean, Language::TraditionalChinese, Language::SimplifiedChinese})
        m |= 1u << static_cast<uint32_t>(l);
    return m;
}

uint32_t parse_language_mask(const std::string& s, uint32_t def) {
    const std::string t = trim(s);
    if (t.empty()) return def;
    if (std::isdigit(static_cast<unsigned char>(t[0]))) return static_cast<uint32_t>(std::strtoul(t.c_str(), nullptr, 0));
    uint32_t m = 0;
    size_t pos = 0;
    while (pos <= t.size()) {
        size_t comma = t.find(',', pos);
        if (comma == std::string::npos) comma = t.size();
        Language l;
        if (parse_language(t.substr(pos, comma - pos), l)) m |= 1u << static_cast<uint32_t>(l);
        pos = comma + 1;
    }
    return m ? m : def;
}

bool parse_uid(const std::string& s, uint64_t& hi, uint64_t& lo) {
    std::string h;
    for (char c : s)
        if (std::isxdigit(static_cast<unsigned char>(c))) h += c;
    if (h.size() != 32) return false;
    hi = std::strtoull(h.substr(0, 16).c_str(), nullptr, 16);
    lo = std::strtoull(h.substr(16).c_str(), nullptr, 16);
    return true;
}

TitleProfile load_profile() {
    TitleProfile p;
    p.program_id = static_cast<uint64_t>(config_get_int("title", "program_id", static_cast<int64_t>(p.program_id)));
    // [process] program_id (kernel GetInfo ProgramId) overrides when it is set explicitly.
    if (config_has("process", "program_id")) p.program_id = static_cast<uint64_t>(config_get_int("process", "program_id", 0));
    p.name = config_get_str("title", "name", p.name.c_str());
    p.display_version = config_get_str("title", "display_version", p.display_version.c_str());
    p.supported_languages = parse_language_mask(config_get_str("title", "supported_languages", ""), default_language_mask());
    p.save_data_owner_id = static_cast<uint64_t>(config_get_int("title", "save_owner_id", static_cast<int64_t>(p.program_id)));
    p.account_save_size = static_cast<uint64_t>(config_get_int("title", "account_save_size", static_cast<int64_t>(p.account_save_size)));
    p.account_save_journal = static_cast<uint64_t>(config_get_int("title", "account_save_journal", static_cast<int64_t>(p.account_save_journal)));
    p.device_save_size = static_cast<uint64_t>(config_get_int("title", "device_save_size", 0));
    p.device_save_journal = static_cast<uint64_t>(config_get_int("title", "device_save_journal", 0));
    p.cache_save_size = static_cast<uint64_t>(config_get_int("title", "cache_save_size", 0));
    p.cache_save_journal = static_cast<uint64_t>(config_get_int("title", "cache_save_journal", 0));
    p.main_thread_stack = static_cast<uint64_t>(config_get_int("title", "main_thread_stack", static_cast<int64_t>(p.main_thread_stack)));
    p.main_thread_priority = static_cast<int>(config_get_int("title", "main_thread_priority", p.main_thread_priority));
    p.main_thread_core = static_cast<int>(config_get_int("title", "main_thread_core", p.main_thread_core));

    Language l;
    if (parse_language(config_get_str("system", "language", "AmericanEnglish"), l)) p.language = l;
    p.region = parse_region(config_get_str("system", "region", ""), p.region);
    p.firmware = config_get_str("system", "firmware", p.firmware.c_str());
    p.device_nickname = config_get_str("system", "nickname", p.device_nickname.c_str());
    p.docked = lower(config_get_str("am", "operation_mode", "docked")) != "handheld";
    p.user_nickname = config_get_str("account", "nickname", p.user_nickname.c_str());
    uint64_t hi, lo;
    if (parse_uid(config_get_str("account", "uid", ""), hi, lo)) {
        p.user_uid_hi = hi;
        p.user_uid_lo = lo;
    }
    return p;
}

std::mutex g_prof_mu;
TitleProfile g_prof;
bool g_prof_loaded = false;

}  // namespace

const TitleProfile& profile() {
    std::lock_guard<std::mutex> lk(g_prof_mu);
    if (!g_prof_loaded) {
        g_prof = load_profile();
        g_prof_loaded = true;
    }
    return g_prof;
}

uint64_t program_id_now() {
    if (config_has("process", "program_id")) return static_cast<uint64_t>(config_get_int("process", "program_id", 0));
    return static_cast<uint64_t>(config_get_int("title", "program_id", static_cast<int64_t>(TitleProfile{}.program_id)));
}

void profile_reload() {
    std::lock_guard<std::mutex> lk(g_prof_mu);
    g_prof = load_profile();
    g_prof_loaded = true;
}

const char* language_code(Language l) {
    for (const LangName& n : kLangs)
        if (n.lang == l) return n.code;
    return "en-US";
}

uint64_t language_code_u64(Language l) {
    const char* c = language_code(l);
    uint64_t v = 0;
    for (size_t i = 0; i < 8 && c[i]; ++i) v |= static_cast<uint64_t>(static_cast<uint8_t>(c[i])) << (8 * i);
    return v;
}

std::string romfs_dir() { return config_get_str("fs", "romfs", "game/romfs"); }

std::string save_dir() {
    std::string d = config_get_str("fs", "save_dir", "");
    if (!d.empty()) return d;
    const char* appdata = std::getenv("APPDATA");
    return std::string(appdata && *appdata ? appdata : ".") + "\\MPRRecomp\\save";
}

}  // namespace nxrt::services
