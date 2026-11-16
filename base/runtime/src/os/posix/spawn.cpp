// SPDX-License-Identifier: GPL-3.0-or-later
// Linux implementation of nxrt/os/spawn.h: posix_spawn, then waitpid polled until the deadline.
#include "nxrt/os/spawn.h"

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstring>

#include "nxrt/os/time.h"

extern char** environ;

namespace nxrt::os::process {

RunResult run_and_wait(const std::string& exe, const std::vector<std::string>& args, const std::vector<std::pair<std::string, std::string>>& env, uint32_t timeout_ms) {
    RunResult r;
    std::vector<std::string> envs;
    for (char** e = environ; e && *e; ++e) {
        bool replaced = false;
        for (const auto& kv : env) {
            const std::string k = kv.first + "=";
            if (std::strncmp(*e, k.c_str(), k.size()) == 0) replaced = true;
        }
        if (!replaced) envs.emplace_back(*e);
    }
    for (const auto& kv : env) envs.push_back(kv.first + "=" + kv.second);
    std::vector<char*> envp;
    for (std::string& s : envs) envp.push_back(s.data());
    envp.push_back(nullptr);
    std::vector<std::string> av;
    av.push_back(exe);
    for (const std::string& a : args) av.push_back(a);
    std::vector<char*> argv;
    for (std::string& s : av) argv.push_back(s.data());
    argv.push_back(nullptr);
    pid_t pid = 0;
    if (posix_spawn(&pid, exe.c_str(), nullptr, nullptr, argv.data(), envp.data()) != 0) return r;
    r.started = true;
    const uint64_t deadline = monotonic_ns() + static_cast<uint64_t>(timeout_ms) * 1000000ull;
    for (;;) {
        int status = 0;
        const pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            r.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 0;
            return r;
        }
        if (w < 0) return r;
        if (monotonic_ns() >= deadline) break;
        sleep_ms(5);
    }
    r.timed_out = true;
    kill(pid, SIGKILL);
    int status = 0;
    waitpid(pid, &status, 0);
    return r;
}

}  // namespace nxrt::os::process
