// SPDX-License-Identifier: GPL-3.0-or-later
// nxrt::os::process: this process and its surroundings: ids, executable, environment, standard folders, locks between processes. Paths are
// UTF-8 (see fs.h). Nothing here throws.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>

namespace nxrt::os::process {

uint32_t pid();
// Whether the process `pid` still runs. True when that cannot be told (no permission to ask).
bool is_alive(uint32_t pid);
// Ends this process now with `code`, without running destructors or atexit handlers.
[[noreturn]] void terminate(int code);

// The executable of this process (absolute, UTF-8; empty when the system does not say) and its folder ("." when unknown).
std::string exe_path();
std::string exe_dir();

// ---- environment --------------------------------------------------------------------------------------------------------------------

std::optional<std::string> getenv(const char* name);
// Sets a variable of this process (children inherit it). An empty value removes it, as _putenv_s does on Windows. Inline, so a test program
// that does not link the runtime can use it too.
inline bool setenv(const char* name, const char* value) {
#ifdef _WIN32
    return ::_putenv_s(name, value ? value : "") == 0;
#else
    if (!value || !*value) return ::unsetenv(name) == 0;
    return ::setenv(name, value, 1) == 0;
#endif
}

// ---- folders ------------------------------------------------------------------------------------------------------------------------
// The release is a portable folder: config, saves and logs live next to the executable. `portable_dir` is that folder when files can be made
// in it, else the user data folder below, so a read-only install (a package, a mounted image) still works. The folder functions return a path
// without a trailing separator and never create it.

std::string home_dir();
// Per-user data and configuration folders for `app` (a folder name): Windows %APPDATA%\<app> (or "." without it); Linux $XDG_DATA_HOME/<app>
// and $XDG_CONFIG_HOME/<app> (defaults ~/.local/share and ~/.config).
std::string user_data_dir(const char* app);
std::string user_config_dir(const char* app);
// exe_dir() when it is writable, else user_data_dir(app).
std::string portable_dir(const char* app);
// Where save data goes when [fs] save_dir is empty. Windows: %APPDATA%\<app>\save (what it always was). Linux: <portable_dir(app)>/save.
std::string default_save_dir(const char* app);

// The language of the user interface as a BCP 47 tag ("en-US", "pt-BR", "zh-TW"); empty when unknown.
std::string ui_language();

// Fills `buf` from the system's cryptographic random source.
bool random_bytes(void* buf, size_t n);

// ---- processes and users ----------------------------------------------------------------------------------------------------------

// A lock that every process on this machine (and user session) taking the same `name` shares, released when the object goes or the process ends
// (also when it crashes). Used for "one game per folder" and for the commit of a save folder.
class NamedLock {
public:
    NamedLock() = default;
    NamedLock(NamedLock&& o) noexcept : h_(o.h_) { o.h_ = 0; }
    NamedLock& operator=(NamedLock&& o) noexcept {
        if (this != &o) {
            release();
            h_ = o.h_;
            o.h_ = 0;
        }
        return *this;
    }
    NamedLock(const NamedLock&) = delete;
    NamedLock& operator=(const NamedLock&) = delete;
    ~NamedLock() { release(); }

    // wait: block until the lock is free. Without it a lock held elsewhere gives an object for which held() is false.
    static NamedLock acquire(const std::string& name, bool wait);
    bool held() const { return h_ != 0; }
    void release();

private:
    intptr_t h_ = 0;
};

// Shows `text` to the user in a box when there is no console (a GUI-subsystem exe on Windows); elsewhere it does nothing (the caller has
// written the text to stderr).
void error_box(const char* title, const char* text);
// A GUI-subsystem exe started from a terminal writes to that terminal when its output is not redirected. No effect elsewhere.
void attach_parent_console();

}  // namespace nxrt::os::process
