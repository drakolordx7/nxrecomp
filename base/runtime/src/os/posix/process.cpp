// SPDX-License-Identifier: GPL-3.0-or-later
// nxrt::os::process on Linux (include/nxrt/os/process.h).
#include "nxrt/os/process.h"

#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <sched.h>
#include <sys/random.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>

#include "nxrt/os/fs.h"

namespace nxrt::os::process {

uint32_t pid() { return static_cast<uint32_t>(::getpid()); }

bool is_alive(uint32_t pid) { return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM; }

std::string exe_path() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return std::string();
    return std::string(buf, static_cast<size_t>(n));
}

std::string exe_dir() {
    const std::string p = exe_path();
    const size_t slash = p.find_last_of('/');
    if (slash == std::string::npos) return ".";
    return slash == 0 ? std::string("/") : p.substr(0, slash);
}

std::optional<std::string> getenv(const char* name) {
    const char* v = std::getenv(name);
    if (!v) return std::nullopt;
    return std::string(v);
}

std::string home_dir() {
    if (const char* h = std::getenv("HOME"); h && *h) return h;
    if (const passwd* pw = ::getpwuid(::getuid()); pw && pw->pw_dir && *pw->pw_dir) return pw->pw_dir;
    return ".";
}

namespace {
// $VAR when it is an absolute path (the XDG rule), else <home>/<fallback>.
std::string xdg(const char* var, const char* fallback, const char* app) {
    const char* v = std::getenv(var);
    std::string base = v && v[0] == '/' ? std::string(v) : home_dir() + "/" + fallback;
    return base + "/" + (app && *app ? app : "nxrt");
}
}  // namespace

std::string user_data_dir(const char* app) { return xdg("XDG_DATA_HOME", ".local/share", app); }
std::string user_config_dir(const char* app) { return xdg("XDG_CONFIG_HOME", ".config", app); }

std::string portable_dir(const char* app) {
    static const std::string dir = exe_dir();
    static const bool writable = fs::dir_writable(dir);
    return writable ? dir : user_data_dir(app);
}

std::string default_save_dir(const char* app) { return portable_dir(app) + "/save"; }

std::string ui_language() {
    const char* vars[] = {"LC_ALL", "LC_MESSAGES", "LANG"};
    std::string v;
    for (const char* n : vars)
        if (const char* e = std::getenv(n); e && *e) {
            v = e;
            break;
        }
    if (v.empty()) {
        if (const char* e = std::getenv("LANGUAGE"); e && *e) v = e;   // "de:en": the first
    }
    v = v.substr(0, v.find(':'));
    v = v.substr(0, v.find('.'));
    v = v.substr(0, v.find('@'));
    if (v.empty() || v == "C" || v == "POSIX") return std::string();
    for (char& c : v)
        if (c == '_') c = '-';
    return v;
}

bool random_bytes(void* buf, size_t n) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    while (n > 0) {
        const ssize_t r = ::getrandom(p, n, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

// flock on a file named after the lock, in the runtime folder (or /tmp): the kernel drops it when the process ends.
NamedLock NamedLock::acquire(const std::string& name, bool wait) {
    std::string clean = "nxrt_" + name;
    for (char& c : clean)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) c = '_';
    const char* rt = std::getenv("XDG_RUNTIME_DIR");
    const std::string path = std::string(rt && rt[0] == '/' ? rt : "/tmp") + "/" + clean + ".lock";
    NamedLock l;
    int fd;
    do fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    while (fd < 0 && errno == EINTR);
    if (fd < 0) return l;
    int r;
    do r = ::flock(fd, wait ? LOCK_EX : (LOCK_EX | LOCK_NB));
    while (r != 0 && errno == EINTR);
    if (r != 0) {
        ::close(fd);
        return l;
    }
    l.h_ = static_cast<intptr_t>(fd) + 1;
    return l;
}

void NamedLock::release() {
    if (!h_) return;
    ::close(static_cast<int>(h_ - 1));   // closing the descriptor drops the flock
    h_ = 0;
}

// ---- child processes ----------------------------------------------------------------------------------------------------------------

Child Child::spawn(const std::string& exe, const std::vector<std::string>& args, const SpawnOptions& opt) {
    Child c;
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(exe.c_str()));
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    const pid_t parent = ::getpid();
    const pid_t pid = ::fork();
    if (pid < 0) return c;
    if (pid == 0) {                       // only async-signal-safe calls from here to exec
        if (opt.kill_with_parent) {
            ::prctl(PR_SET_PDEATHSIG, SIGKILL);
            if (::getppid() != parent) ::_exit(127);        // the parent ended before the request took hold
        }
        if (opt.cpu_mask) {
            cpu_set_t set;
            CPU_ZERO(&set);
            for (int i = 0; i < 64; i++)
                if (opt.cpu_mask >> i & 1) CPU_SET(i, &set);
            ::sched_setaffinity(0, sizeof set, &set);
        }
        ::execv(exe.c_str(), argv.data());
        ::_exit(127);
    }
    c.h_ = static_cast<intptr_t>(pid);
    c.pid_ = static_cast<uint32_t>(pid);
    return c;
}

bool Child::wait(uint32_t timeout_ms, int* exit_code) {
    if (h_ <= 0) return true;
    const pid_t pid = static_cast<pid_t>(h_);
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        int st = 0;
        const pid_t r = ::waitpid(pid, &st, WNOHANG);
        if (r == pid || (r < 0 && errno != EINTR)) {
            if (r == pid && exit_code) *exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
            h_ = -1;                       // reaped: nothing left to kill or wait for
            return true;
        }
        if (timeout_ms != 0xFFFFFFFFu && std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(timeout_ms)) return false;
        ::usleep(1000);
    }
}

void Child::kill() {
    if (h_ <= 0) return;
    ::kill(static_cast<pid_t>(h_), SIGKILL);
    wait(10000);
}

void Child::close() {
    if (!h_) return;
    kill();
    h_ = 0;
    pid_ = 0;
}

void error_box(const char*, const char*) {}
void attach_parent_console() {}

}  // namespace nxrt::os::process
