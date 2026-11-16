// SPDX-License-Identifier: GPL-3.0-or-later
// nxrt::os spawn: runs another program and waits for it. For tests that need a real process death (a crash, a freeze) and for tools. Windows:
// CreateProcess. Linux: posix_spawn and waitpid. The child inherits this process's environment plus `env`.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace nxrt::os::process {

struct RunResult {
    bool started = false;
    bool timed_out = false;   // the child was still running after timeout_ms and was killed
    int exit_code = 0;        // the exit code (Windows: also an NTSTATUS for a crash); Linux: 128 + the signal number when a signal ended it
};
// argv[0] is `exe` itself; `args` follow. Arguments must not contain quotes. Standard handles are inherited.
RunResult run_and_wait(const std::string& exe, const std::vector<std::string>& args, const std::vector<std::pair<std::string, std::string>>& env,
                       uint32_t timeout_ms);

}  // namespace nxrt::os::process
