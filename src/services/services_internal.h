// SPDX-License-Identifier: GPL-3.0-or-later
// Internal to src/services/: the per-service registration hooks that services::register_all() / reset_all() call.
// Each service file defines its own pair; a service that has nothing to reset defines an empty reset.
#pragma once
#include <functional>
#include <memory>
#include <string>

#include "nxrt/ipc.h"
#include "nxrt/services.h"

namespace nxrt::services {

// Registers `name` in the ipc registry and records it for registered_names().
void add(const std::string& name, ipc::ServiceFactory f);

// Convenience for "a service made of a command table": builds a Service named `name`, lets `setup` add its commands.
std::shared_ptr<ipc::Service> make_iface(const char* name, const std::function<void(ipc::Service&)>& setup);

void register_fs();          void fs_reset();          // fs.cpp          fsp-srv (+ fsp-ldr, fsp-pr stubs)
void register_settings();    void settings_reset();    // settings.cpp    set, set:sys
void register_time();        void time_reset();        // time.cpp        time:u, time:a, time:s, time:r
void register_power();       void power_reset();       // power.cpp       apm, apm:p, apm:am, psm
void register_logfault();    void logfault_reset();    // logfault.cpp    lm, fatal:u, fatal:p, err:u
void register_am();          void am_reset();          // am.cpp          appletOE (appletAE)
void register_acc();         void acc_reset();         // acc.cpp         acc:u0, acc:u1
void register_hid();         void hid_reset();         // hid.cpp         hid
void register_pl();          void pl_reset();          // pl.cpp          pl:u (pl:s)
void register_network();     void network_reset();     // network.cpp     nifm, bsd, ssl, nim, friend, prepo, pctl, bcat, ldn, nsd, ...
void register_misc();        void misc_reset();        // misc.cpp        csrng, ldr:ro, mm:u, hwopus, aoc:u, caps, mii, irs, nfc, nfp, ...
void register_audio();       void audio_reset();       // audio.cpp       audren:u (calls the two below)
void register_audio_out();   void audio_out_reset();   // audio_out.cpp   audout:u, audin:u
void register_vi_nv();       void vi_nv_reset();       // vi_nv.cpp       vi:u (vi:m, vi:s), nvdrv (nvdrv:a, nvdrv:s)

}  // namespace nxrt::services
