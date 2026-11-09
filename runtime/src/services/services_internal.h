// SPDX-License-Identifier: GPL-3.0-or-later
// Internal to src/services/: the per-service registration hooks that services::register_all() / reset_all() call.
// Each service file defines its own pair; a service that has nothing to reset defines an empty reset.
#pragma once
#include <functional>
#include <memory>
#include <string>

#include "nxrt/input_source.h"
#include "nxrt/ipc.h"
#include "nxrt/services.h"

namespace nxrt::services {

void add(const std::string& name, ipc::ServiceFactory f);

std::shared_ptr<ipc::Service> make_iface(const char* name, const std::function<void(ipc::Service&)>& setup);

// Forwards one stored vibration value to the sink registered with nxrt::hid::set_vibration_sink (input_source.cpp). npad_slot is the input
// slot (0..9), hand 0 = left or the only device, 1 = right. Does nothing when no sink is registered.
void vibration_dispatch(uint32_t npad_slot, int hand, const nxrt::hid::VibrationValue& v);

void register_fs();          void fs_reset();
void register_settings();    void settings_reset();
void register_time();        void time_reset();
void register_power();       void power_reset();
void register_logfault();    void logfault_reset();
void register_am();          void am_reset();
void register_acc();         void acc_reset();
void register_hid();         void hid_reset();
void register_pl();          void pl_reset();
void register_network();     void network_reset();
void register_misc();        void misc_reset();
void register_audio();       void audio_reset();
void register_audio_out();   void audio_out_reset();
void register_vi_nv();       void vi_nv_reset();

}
