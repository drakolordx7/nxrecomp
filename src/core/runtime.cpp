// SPDX-License-Identifier: GPL-3.0-or-later
#include "nxrt/runtime.h"

#include <cstdlib>
#include <mutex>

#include "nxrt/config.h"
#include "nxrt/diag.h"
#include "nxrt/dispatch.h"
#include "nxrt/ipc.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/memguard.h"
#include "nxrt/memory.h"
#include "nxrt/module.h"
#include "nxrt/svc.h"
#include "nxrt/symbols.h"

namespace nxrt {
namespace {
std::mutex g_lock;
bool g_up = false;
}  // namespace

bool init(const InitOptions& opt) {
    std::lock_guard<std::mutex> lk(g_lock);
    if (g_up) return true;
    const char* ini = opt.ini_path ? opt.ini_path : std::getenv("NXRT_INI");
    config_load_file(ini && *ini ? ini : "nxrt.ini");
    LogOptions lo;
    lo.path = opt.log_path;
    log_init(lo);
    NXLOG_I("runtime", "nxrt starting (ABI %s)", abi_source());
    if (opt.load_symbols) {
        size_t n = symbols_load_map();
        NXLOG_I("runtime", "host symbols: %zu from the linker map%s", n, n ? "" : " (no map next to the exe: host frames show module+offset only)");
    }
    if (opt.crash_handler) crash_install();
    bool ok = true;
    if (opt.memory_init && !mem::init()) {
        NXLOG_E("runtime", "address-space layout could not be reserved");
        ok = false;
    }
    svc_init();
    if (opt.memguard) memguard_start();
    if (opt.watchdog) watchdog_start();
    config_log_effective();
    g_up = ok;
    return ok;
}

std::string summary() {
    std::string s;
    s += svc_summary() + "\n";
    s += ipc::ipc_summary();
    size_t stubs = 0, called_stubs = 0;
    for (auto& h : hle_list()) {
        stubs += h.stub;
        if (h.stub && h.calls) {
            called_stubs++;
            s += "hle stub called " + std::to_string(h.calls) + "x: " + h.name + "\n";
        }
    }
    s += "hle: " + std::to_string(hle_count()) + " thunks, " + std::to_string(stubs) + " logging stubs (" + std::to_string(called_stubs) + " called)\n";
    s += "lifted functions: " + std::to_string(fn_count()) + ", modules: " + std::to_string(modules().size()) + ", threads created: " +
         std::to_string(kernel::threads().size()) + ", handles open: " + std::to_string(kernel::handle_count()) + "\n";
    return s;
}

void shutdown() {
    std::lock_guard<std::mutex> lk(g_lock);
    if (!g_up) return;
    NXLOG_I("runtime", "end-of-run summary:\n%s", summary().c_str());
    watchdog_stop();
    memguard_stop();
    log_flush();
    g_up = false;
}

bool initialised() {
    std::lock_guard<std::mutex> lk(g_lock);
    return g_up;
}

}  // namespace nxrt
