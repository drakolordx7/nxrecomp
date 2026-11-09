// SPDX-License-Identifier: GPL-3.0-or-later
// appletOE / appletAE (S3c): command replies, the applet message queue, launch parameters, storages, save data, the language.
// Request bytes and expected reply bytes are written out from Switchbrew "Applet Manager services" and ScarletNX am.swipc, and from
// Ryujinx for the result codes (module 128: result = module | description << 9), not read from the service.
#include <filesystem>
#include <windows.h>
#include <cstring>

#include "nxrt/config.h"
#include "nxrt/kernel.h"
#include "nxrt/services_api.h"
#include "svc_test_util.h"

NXTEST_GROUP(svc_am)

using namespace nxrt;
namespace amctl = nxrt::services::amctl;

namespace {

// OpenApplicationProxy(pid) with a copy of a process handle. The service closes the copy, so the test hands it an event handle.
nxsvc::Session open_proxy() {
    nxsvc::Session oe = nxsvc::connect("appletOE");
    if (!oe.valid()) return nxsvc::Session();
    services::ServiceEvent process_stand_in;
    nxsvc::Reply r = oe.call(0, nxsvc::Args().with_pid().hcopy(process_stand_in.new_handle()));
    if (!r.ok() || r.move.empty()) return nxsvc::Session();
    return oe.sub(r);
}

// Is an event signalled right now (a zero timeout poll)?
bool signalled(uint32_t h) {
    int32_t idx = -1;
    return kernel::wait_synchronization(&h, 1, 0, &idx) == 0;
}

// The fixed launch parameter layout bytes, from the Switchbrew / Ryujinx layout: magic 0xC79497CA, flag 1, Uid low word, Uid high word.
// The test profile's user id (config below) is hi = 1, lo = 2.
void set_test_user() {
    config_set("account", "uid", "00000000000000010000000000000002");
    services::profile_reload();
}

void clear_config() {
    config_clear();
    services::profile_reload();
}

}  // namespace

NXTEST(am_unknown_command_is_stub) {
    nxsvc::Env env;
    nxsvc::Session proxy = open_proxy();
    REQUIRE(proxy.valid());
    nxsvc::Reply r = proxy.call(4321);
    CHECK_EQ(r.result, kernel::kStubService);
}

NXTEST(am_proxy_interfaces) {
    nxsvc::Env env;
    nxsvc::Session proxy = open_proxy();
    REQUIRE(proxy.valid());
    // Each command returns one interface as a move handle.
    const uint32_t cmds[] = {0, 1, 2, 3, 11, 20, 1000};
    for (uint32_t c : cmds) {
        nxsvc::Reply r = proxy.call(c);
        CHECK_EQ(r.result, 0u);
        CHECK_EQ(r.move.size(), 1u);
    }
    // Display and process-winding controllers answer with an interface that has no commands.
    CHECK_EQ(proxy.call(4).move.size(), 1u);
    CHECK_EQ(proxy.call(10).move.size(), 1u);
}

NXTEST(am_common_state_getters) {
    nxsvc::Env env;
    config_clear();
    services::profile_reload();
    nxsvc::Session proxy = open_proxy();
    REQUIRE(proxy.valid());
    nxsvc::Session common = proxy.sub(proxy.call(0));
    REQUIRE(common.valid());
    // Docked start-up (profile default): OperationMode Console (1), PerformanceMode Boost (1), InFocus (1).
    nxsvc::Reply mode = common.call(5);
    CHECK_EQ(mode.result, 0u);
    CHECK_EQ(mode.get<uint8_t>(0), 1u);
    nxsvc::Reply perf = common.call(6);
    CHECK_EQ(perf.get<uint32_t>(0), 1u);
    CHECK_EQ(common.call(9).get<uint8_t>(0), 1u);
    // GetDefaultDisplayResolution: 1920 x 1080 docked.
    nxsvc::Reply res_r = common.call(60);
    CHECK_EQ(res_r.result, 0u);
    CHECK_EQ(res_r.get<int32_t>(0), 1920);
    CHECK_EQ(res_r.get<int32_t>(4), 1080);
    // Boot mode is Normal (0); the cpu boost request is accepted.
    CHECK_EQ(common.call(8).get<uint8_t>(0), 0u);
    CHECK_EQ(common.call(66, nxsvc::Args().u32(1)).result, 0u);
}

NXTEST(am_message_queue_sequence) {
    nxsvc::Env env;
    config_clear();
    services::profile_reload();
    nxsvc::Session proxy = open_proxy();
    REQUIRE(proxy.valid());
    nxsvc::Session common = proxy.sub(proxy.call(0));
    nxsvc::Session self = proxy.sub(proxy.call(1));
    REQUIRE(common.valid());
    REQUIRE(self.valid());

    // GetEventHandle: the queue holds FocusStateChanged (15) from start-up, so the event is signalled.
    nxsvc::Reply ev_r = common.call(0);
    REQUIRE(ev_r.copy.size() == 1);
    const uint32_t ev = ev_r.copy[0];
    CHECK(signalled(ev));

    nxsvc::Reply m1 = common.call(1);
    CHECK_EQ(m1.result, 0u);
    CHECK_EQ(m1.get<uint32_t>(0), 15u);
    // Drained: ReceiveMessage on an empty queue is ResultNoMessages, 128-3 = 0x680, and the event is cleared.
    CHECK_EQ(common.call(1).result, 0x680u);
    CHECK(!signalled(ev));

    // Operation / performance notifications are off until the guest asks: a dock change queues nothing.
    amctl::set_docked(false);
    CHECK_EQ(common.call(5).get<uint8_t>(0), 0u);
    CHECK_EQ(common.call(6).get<uint32_t>(0), 0u);
    CHECK_EQ(common.call(1).result, 0x680u);
    CHECK(!signalled(ev));

    // Enable both notifications: a dock change now queues OperationModeChanged (30) then PerformanceModeChanged (31).
    CHECK_EQ(self.call(11, nxsvc::Args().u8(1)).result, 0u);
    CHECK_EQ(self.call(12, nxsvc::Args().u8(1)).result, 0u);
    amctl::set_docked(true);
    CHECK(signalled(ev));
    CHECK_EQ(common.call(1).get<uint32_t>(0), 30u);
    CHECK(signalled(ev));
    CHECK_EQ(common.call(1).get<uint32_t>(0), 31u);
    CHECK(!signalled(ev));
    CHECK_EQ(common.call(1).result, 0x680u);
    CHECK_EQ(common.call(5).get<uint8_t>(0), 1u);

    // Focus: set_focus(false) queues FocusStateChanged, and GetCurrentFocusState reports OutOfFocus (2).
    amctl::set_focus(false);
    CHECK(!amctl::in_focus());
    CHECK(signalled(ev));
    CHECK_EQ(common.call(1).get<uint32_t>(0), 15u);
    CHECK_EQ(common.call(9).get<uint8_t>(0), 2u);
    amctl::set_focus(true);
    CHECK(amctl::in_focus());

    kernel::handle_close(ev);
}

NXTEST(am_lock_accessor) {
    nxsvc::Env env;
    nxsvc::Session proxy = open_proxy();
    REQUIRE(proxy.valid());
    nxsvc::Session common = proxy.sub(proxy.call(0));
    REQUIRE(common.valid());
    nxsvc::Session lock = common.sub(common.call(30));
    REQUIRE(lock.valid());
    // Free at first: TryLock succeeds (flag 1) and returns the event when asked to.
    nxsvc::Reply t1 = lock.call(1, nxsvc::Args().u8(1));
    CHECK_EQ(t1.result, 0u);
    CHECK_EQ(t1.get<uint8_t>(0), 1u);
    CHECK_EQ(t1.copy.size(), 1u);
    if (!t1.copy.empty()) kernel::handle_close(t1.copy[0]);
    // Held: a second TryLock fails (flag 0) and, without the return flag, returns no handle.
    nxsvc::Reply t2 = lock.call(1, nxsvc::Args().u8(0));
    CHECK_EQ(t2.get<uint8_t>(0), 0u);
    CHECK_EQ(t2.copy.size(), 0u);
    CHECK_EQ(lock.call(4).get<uint8_t>(0), 1u);
    CHECK_EQ(lock.call(2).result, 0u);
    CHECK_EQ(lock.call(4).get<uint8_t>(0), 0u);
    // The event is signalled when the lock is released.
    nxsvc::Reply e = lock.call(3);
    REQUIRE(e.copy.size() == 1);
    CHECK(signalled(e.copy[0]));
    kernel::handle_close(e.copy[0]);
}

NXTEST(am_self_controller) {
    nxsvc::Env env;
    nxsvc::Session proxy = open_proxy();
    REQUIRE(proxy.valid());
    nxsvc::Session self = proxy.sub(proxy.call(1));
    REQUIRE(self.valid());
    // Managed display layers: increasing ids from 1, the separable call takes the next two.
    CHECK_EQ(self.call(40).get<uint64_t>(0), 1ull);
    CHECK_EQ(self.call(40).get<uint64_t>(0), 2ull);
    nxsvc::Reply sep = self.call(44);
    CHECK_EQ(sep.get<uint64_t>(0), 3ull);
    CHECK_EQ(sep.get<uint64_t>(8), 4ull);
    // Idle time detection extension round-trips.
    CHECK_EQ(self.call(62, nxsvc::Args().u32(120)).result, 0u);
    CHECK_EQ(self.call(63).get<uint32_t>(0), 120u);
    // The library applet launchable event is signalled (a library applet can be launched, in the console's view).
    nxsvc::Reply launch = self.call(9);
    REQUIRE(launch.copy.size() == 1);
    CHECK(signalled(launch.copy[0]));
    kernel::handle_close(launch.copy[0]);
    // Exit is answered with success (the run is ended by the shell, not by the services library).
    CHECK_EQ(self.call(0).result, 0u);
}

NXTEST(am_window_and_audio_controllers) {
    nxsvc::Env env;
    nxsvc::Session proxy = open_proxy();
    REQUIRE(proxy.valid());
    nxsvc::Session window = proxy.sub(proxy.call(2));
    REQUIRE(window.valid());
    CHECK_EQ(window.call(1).get<uint64_t>(0), 0x51ull);
    CHECK_EQ(window.call(10).result, 0u);

    nxsvc::Session audio = proxy.sub(proxy.call(3));
    REQUIRE(audio.valid());
    // Volumes start at 1.0 (f32 0x3F800000) and are clamped to 0..1 when set.
    CHECK_EQ(audio.call(1).get<float>(0), 1.0f);
    CHECK_EQ(audio.call(2).get<float>(0), 1.0f);
    CHECK_EQ(audio.call(0, nxsvc::Args().f32(0.5f).f32(0.25f)).result, 0u);
    CHECK_EQ(audio.call(1).get<float>(0), 0.5f);
    CHECK_EQ(audio.call(2).get<float>(0), 0.25f);
    CHECK_EQ(audio.call(0, nxsvc::Args().f32(2.0f).f32(-1.0f)).result, 0u);
    CHECK_EQ(audio.call(1).get<float>(0), 1.0f);
    CHECK_EQ(audio.call(2).get<float>(0), 0.0f);
}

NXTEST(am_launch_parameter_preselected_user) {
    nxsvc::Env env;
    set_test_user();
    nxsvc::Session proxy = open_proxy();
    REQUIRE(proxy.valid());
    nxsvc::Session app = proxy.sub(proxy.call(20));
    REQUIRE(app.valid());

    // PopLaunchParameter(PreselectedUser = 2): an IStorage of 0x88 bytes.
    nxsvc::Reply pop = app.call(1, nxsvc::Args().u32(2));
    REQUIRE(pop.ok());
    REQUIRE(pop.move.size() == 1);
    nxsvc::Session storage = app.sub(pop);
    nxsvc::Session accessor = storage.sub(storage.call(0));
    REQUIRE(accessor.valid());
    CHECK_EQ(accessor.call(0).get<int64_t>(0), 0x88ll);

    uint8_t buf[0x88];
    std::memset(buf, 0xEE, sizeof buf);
    CHECK_EQ(accessor.call(11, nxsvc::Args().i64(0).buf_b(buf, sizeof buf)).result, 0u);
    // magic 0xC79497CA (LE), flag 1, then the Uid in memory order (first u64 = uid_hi = 1, second = uid_lo = 2), the rest zero.
    const uint8_t want[24] = {0xCA, 0x97, 0x94, 0xC7, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0};
    CHECK(std::memcmp(buf, want, sizeof want) == 0);
    CHECK_EQ(buf[24], 0u);
    CHECK_EQ(buf[0x87], 0u);

    // Popped once: the second PopLaunchParameter(2) and an empty user channel both answer NotAvailable, 128-2 = 0x480.
    CHECK_EQ(app.call(1, nxsvc::Args().u32(2)).result, 0x480u);
    CHECK_EQ(app.call(1, nxsvc::Args().u32(1)).result, 0x480u);
    CHECK_EQ(app.call(1, nxsvc::Args().u32(7)).result, 0x480u);
    clear_config();
}

NXTEST(am_user_channel_round_trip) {
    nxsvc::Env env;
    set_test_user();
    nxsvc::Session proxy = open_proxy();
    REQUIRE(proxy.valid());
    nxsvc::Session app = proxy.sub(proxy.call(20));
    REQUIRE(app.valid());

    // Pop the preselected-user storage (0x88 bytes), then unpop it to the user channel. The storage's handle is passed on as a
    // move, so it is not wrapped in a Session here (a Session would close it).
    nxsvc::Reply pre = app.call(1, nxsvc::Args().u32(2));
    REQUIRE(pre.ok());
    REQUIRE(pre.move.size() == 1);
    const uint32_t storage_h = pre.move[0];
    CHECK_EQ(app.call(122, nxsvc::Args().hmove(storage_h)).result, 0u);

    // The user channel hands the copy back (PopLaunchParameter kind 1 = UserChannel): same 0x88 bytes.
    nxsvc::Reply again = app.call(1, nxsvc::Args().u32(1));
    CHECK_EQ(again.result, 0u);
    REQUIRE(again.move.size() == 1);
    nxsvc::Session copy = app.sub(again);
    nxsvc::Session acc = copy.sub(copy.call(0));
    REQUIRE(acc.valid());
    CHECK_EQ(acc.call(0).get<int64_t>(0), 0x88ll);
    uint8_t buf[0x88];
    std::memset(buf, 0xEE, sizeof buf);
    CHECK_EQ(acc.call(11, nxsvc::Args().i64(0).buf_b(buf, sizeof buf)).result, 0u);
    const uint8_t want[8] = {0xCA, 0x97, 0x94, 0xC7, 1, 0, 0, 0};
    CHECK(std::memcmp(buf, want, sizeof want) == 0);
    // The channel is empty again.
    CHECK_EQ(app.call(1, nxsvc::Args().u32(1)).result, 0x480u);
    clear_config();
}

NXTEST(am_storage_bounds) {
    nxsvc::Env env;
    nxsvc::Session proxy = open_proxy();
    REQUIRE(proxy.valid());
    nxsvc::Session creator = proxy.sub(proxy.call(11));
    REQUIRE(creator.valid());
    nxsvc::Session storage = creator.sub(creator.call(10, nxsvc::Args().i64(0x20)));
    REQUIRE(storage.valid());
    nxsvc::Session acc = storage.sub(storage.call(0));
    REQUIRE(acc.valid());
    CHECK_EQ(acc.call(0).get<int64_t>(0), 0x20ll);

    // Write 8 bytes at 0x18 (the last 8 of 0x20), then read them back.
    const uint8_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK_EQ(acc.call(10, nxsvc::Args().i64(0x18).buf_a(data, sizeof data)).result, 0u);
    uint8_t back[8] = {};
    CHECK_EQ(acc.call(11, nxsvc::Args().i64(0x18).buf_b(back, sizeof back)).result, 0u);
    CHECK(std::memcmp(back, data, sizeof data) == 0);
    // Past the end: ResultInvalidOffset, 128-503 = 0x3EE80.
    CHECK_EQ(acc.call(10, nxsvc::Args().i64(0x1C).buf_a(data, sizeof data)).result, 0x3EE80u);
    CHECK_EQ(acc.call(11, nxsvc::Args().i64(0x1C).buf_b(back, sizeof back)).result, 0x3EE80u);
    CHECK_EQ(acc.call(11, nxsvc::Args().i64(-1).buf_b(back, sizeof back)).result, 0x3EE80u);
}

NXTEST(am_create_storage_too_large) {
    nxsvc::Env env;
    nxsvc::Session proxy = open_proxy();
    REQUIRE(proxy.valid());
    nxsvc::Session creator = proxy.sub(proxy.call(11));
    REQUIRE(creator.valid());
    // 0x20000000 bytes is past the storage limit: MemoryAllocationFailed, 128-600 = 0x4B080.
    CHECK_EQ(creator.call(10, nxsvc::Args().i64(0x20000000)).result, 0x4B080u);
}

NXTEST(am_library_applet_refused) {
    nxsvc::Env env;
    nxsvc::Session proxy = open_proxy();
    REQUIRE(proxy.valid());
    nxsvc::Session creator = proxy.sub(proxy.call(11));
    REQUIRE(creator.valid());
    // Library applets are not supported: AppletLaunchFailed, 128-35 = 0x4680 (the code is Ryujinx's, not checked on Switchbrew).
    nxsvc::Reply r = creator.call(0, nxsvc::Args().u32(0x10).u32(1));
    CHECK_EQ(r.result, 0x4680u);
    CHECK_EQ(r.move.size(), 0u);
    CHECK_EQ(creator.call(3, nxsvc::Args().u32(0x10).u32(1).u64(0)).result, 0x4680u);
}

namespace {

// CreateStorage(size) on the creator, `n` bytes of `data` written at `off`. Returns the storage handle, released for a move.
uint32_t make_storage(nxsvc::Session& creator, int64_t size, int64_t off = 0, const void* data = nullptr, size_t n = 0) {
    nxsvc::Session storage = creator.sub(creator.call(10, nxsvc::Args().i64(size)));
    if (!storage.valid()) return 0;
    if (data && n) {
        nxsvc::Session acc = storage.sub(storage.call(0));
        if (acc.call(10, nxsvc::Args().i64(off).buf_a(data, n)).result != 0) return 0;
    }
    return storage.release();
}

// The software keyboard's in data: CommonArguments (0x20 bytes, library_version at +8 = 0x8000D, 8.0.0+), then the 0x4C8-byte
// config of that version with max_text_length (+0x3AC) and use_text_check (+0x3D0), then a 0x1000-byte work buffer.
// Offsets and sizes from Eden applet_software_keyboard_types.h (SwkbdConfigCommon 0x3D4 + SwkbdConfigNew).
nxsvc::Session open_swkbd(nxsvc::Session& creator, uint32_t max_len, bool text_check) {
    nxsvc::Reply cr = creator.call(0, nxsvc::Args().u32(0x11).u32(0));   // LibraryAppletSwkbd, AllForeground
    if (!cr.ok()) return nxsvc::Session();
    nxsvc::Session applet = creator.sub(cr);
    const uint32_t version = 0x8000D;
    uint8_t cfg[0x4C8] = {};
    std::memcpy(cfg + 0x3AC, &max_len, 4);
    cfg[0x3D0] = text_check ? 1 : 0;
    const uint32_t hs[3] = {make_storage(creator, 0x20, 8, &version, 4), make_storage(creator, sizeof cfg, 0, cfg, sizeof cfg),
                            make_storage(creator, 0x1000)};
    for (uint32_t h : hs)
        if (!h || applet.call(100, nxsvc::Args().hmove(h)).result != 0) return nxsvc::Session();
    return applet;
}

// Pops one storage (PopOutData 101 / PopInteractiveOutData 104) into `buf`; returns its size, or -1.
int64_t pop_into(nxsvc::Session& applet, uint32_t cmd, uint8_t* buf, size_t n) {
    nxsvc::Reply r = applet.call(cmd);
    if (!r.ok() || r.move.size() != 1) return -1;
    nxsvc::Session st = applet.sub(r);
    nxsvc::Session acc = st.sub(st.call(0));
    const int64_t size = acc.call(0).get<int64_t>(0);
    if (acc.call(11, nxsvc::Args().i64(0).buf_b(buf, n)).result != 0) return -1;
    return size;
}

}  // namespace

NXTEST(am_swkbd_normal_keyboard) {
    nxsvc::Env env;
    config_clear();
    config_set("applet", "swkbd_text", "Hello World");
    nxsvc::Session proxy = open_proxy();
    REQUIRE(proxy.valid());
    nxsvc::Session creator = proxy.sub(proxy.call(11));
    REQUIRE(creator.valid());
    nxsvc::Session applet = open_swkbd(creator, 5, false);
    REQUIRE(applet.valid());

    nxsvc::Reply ev_r = applet.call(0);   // GetAppletStateChangedEvent
    REQUIRE(ev_r.copy.size() == 1);
    const uint32_t ev = ev_r.copy[0];
    CHECK(!signalled(ev));
    CHECK_EQ(applet.call(1).get<uint8_t>(0), 0u);   // IsCompleted: not yet
    CHECK_EQ(applet.call(101).result, 0x480u);      // PopOutData before Start: ResultNoDataInChannel, 128-2

    CHECK_EQ(applet.call(10).result, 0u);           // Start
    CHECK(signalled(ev));
    CHECK_EQ(applet.call(1).get<uint8_t>(0), 1u);
    CHECK_EQ(applet.call(30).result, 0u);           // GetResult

    // Output: SwkbdResult Ok (0), then the text in UTF-16LE cut to max_text_length 5, then zeros; 0x7D8 bytes in all.
    uint8_t out[0x7D8] = {};
    std::memset(out, 0xEE, sizeof out);
    CHECK_EQ(pop_into(applet, 101, out, sizeof out), 0x7D8ll);
    const uint8_t want[4 + 12] = {0, 0, 0, 0, 'H', 0, 'e', 0, 'l', 0, 'l', 0, 'o', 0, 0, 0};
    CHECK(std::memcmp(out, want, sizeof want) == 0);
    CHECK_EQ(out[0x7D7], 0u);
    CHECK_EQ(applet.call(101).result, 0x480u);      // drained

    // Other applets and the inline keyboard (PartialForeground, mode 1) are still refused: AppletLaunchFailed 0x4680.
    CHECK_EQ(creator.call(0, nxsvc::Args().u32(0x11).u32(1)).result, 0x4680u);
    kernel::handle_close(ev);
    clear_config();
}

NXTEST(am_swkbd_text_check) {
    nxsvc::Env env;
    config_clear();
    nxsvc::Session proxy = open_proxy();
    REQUIRE(proxy.valid());
    nxsvc::Session creator = proxy.sub(proxy.call(11));
    REQUIRE(creator.valid());
    nxsvc::Session applet = open_swkbd(creator, 12, true);
    REQUIRE(applet.valid());
    nxsvc::Reply ie = applet.call(106);   // GetPopInteractiveOutDataEvent
    REQUIRE(ie.copy.size() == 1);
    CHECK_EQ(applet.call(10).result, 0u);
    CHECK(signalled(ie.copy[0]));
    CHECK_EQ(applet.call(1).get<uint8_t>(0), 0u);   // waiting for the application's verdict

    // Interactive out: u64 byte size with the NUL ("Player" = 7 UTF-16 units = 14), then the text. 8 + 0x7D4 bytes.
    uint8_t chk[8 + 0x7D4] = {};
    CHECK_EQ(pop_into(applet, 104, chk, sizeof chk), static_cast<int64_t>(sizeof chk));
    uint64_t size = 0;
    std::memcpy(&size, chk, 8);
    CHECK_EQ(size, 14ull);
    CHECK(chk[8] == 'P' && chk[9] == 0 && chk[18] == 'r');
    CHECK(!signalled(ie.copy[0]));

    // The application answers SwkbdTextCheck {Success 0, message}: the keyboard ends with the text.
    const uint32_t h = make_storage(creator, 0x7D8);
    REQUIRE(h != 0);
    CHECK_EQ(applet.call(103, nxsvc::Args().hmove(h)).result, 0u);
    CHECK_EQ(applet.call(1).get<uint8_t>(0), 1u);
    uint8_t out[0x7D8] = {};
    CHECK_EQ(pop_into(applet, 101, out, sizeof out), 0x7D8ll);
    CHECK(out[0] == 0 && out[4] == 'P' && out[14] == 'r' && out[16] == 0);
    kernel::handle_close(ie.copy[0]);
}

NXTEST(am_desired_language) {
    nxsvc::Env env;
    clear_config();
    nxsvc::Session proxy = open_proxy();
    REQUIRE(proxy.valid());
    nxsvc::Session app = proxy.sub(proxy.call(20));
    REQUIRE(app.valid());
    // The default language is AmericanEnglish: "en-US" as the little-endian u64 of its eight bytes (65 6E 2D 55 53 ...).
    CHECK_EQ(app.call(21).get<uint64_t>(0), 0x53552d6e65ull);

    // Japanese ("ja") when the game supports it.
    config_set("system", "language", "Japanese");
    config_set("title", "supported_languages", "Japanese,AmericanEnglish");
    services::profile_reload();
    CHECK_EQ(app.call(21).get<uint64_t>(0), 0x616aull);

    // Japanese is not supported by the game: the first language it does support, AmericanEnglish.
    config_set("title", "supported_languages", "AmericanEnglish");
    services::profile_reload();
    CHECK_EQ(app.call(21).get<uint64_t>(0), 0x53552d6e65ull);
    clear_config();
}

NXTEST(am_save_data_extend_and_size) {
    nxsvc::Env env;
    nxsvc::Session proxy = open_proxy();
    REQUIRE(proxy.valid());
    nxsvc::Session app = proxy.sub(proxy.call(20));
    REQUIRE(app.valid());
    // Uid (lo 2, hi 1) at offset 8 after the u8 type; the sizes follow the uid: normal at 24, journal at 32.
    const uint64_t lo = 2, hi = 1;
    CHECK_EQ(app.call(25, nxsvc::Args().u8(1).pad(7).u64(lo).u64(hi).i64(0x1000).i64(0x200)).get<int64_t>(0), 0ll);
    nxsvc::Reply sz = app.call(26, nxsvc::Args().u8(1).pad(7).u64(lo).u64(hi));
    CHECK_EQ(sz.result, 0u);
    CHECK_EQ(sz.get<int64_t>(0), 0x1000ll);
    CHECK_EQ(sz.get<int64_t>(8), 0x200ll);
}

NXTEST(am_common_stubs_accepted) {
    nxsvc::Env env;
    nxsvc::Session proxy = open_proxy();
    REQUIRE(proxy.valid());
    nxsvc::Session common = proxy.sub(proxy.call(0));
    REQUIRE(common.valid());
    // GetThisAppletKind (u64) and GetBootMode (u8) are accepted and answer zeros.
    nxsvc::Reply k = common.call(2);
    CHECK_EQ(k.result, 0u);
    CHECK_EQ(k.get<uint64_t>(0), 0ull);
    CHECK_EQ(common.call(12).result, 0u);
    CHECK_EQ(common.call(52, nxsvc::Args().u8(1)).result, 0u);
    CHECK_EQ(common.call(900, nxsvc::Args().u8(1)).result, 0u);
    // VR mode is real state: it reports what was set.
    CHECK_EQ(common.call(50).get<uint8_t>(0), 0u);
    CHECK_EQ(common.call(51, nxsvc::Args().u8(1)).result, 0u);
    CHECK_EQ(common.call(50).get<uint8_t>(0), 1u);
    CHECK_EQ(common.call(54).result, 0u);
    CHECK_EQ(common.call(50).get<uint8_t>(0), 0u);
}

NXTEST(am_resolution_follows_mode_and_override) {
    nxsvc::Env env;
    clear_config();
    nxsvc::Session proxy = open_proxy();
    REQUIRE(proxy.valid());
    nxsvc::Session common = proxy.sub(proxy.call(0));
    REQUIRE(common.valid());
    // Handheld: 1280 x 720, and the performance mode reads Handheld (0).
    amctl::set_docked(false);
    CHECK_EQ(common.call(60).get<int32_t>(0), 1280);
    CHECK_EQ(common.call(60).get<int32_t>(4), 720);
    CHECK_EQ(common.call(6).get<uint32_t>(0), 0u);
    amctl::set_docked(true);
    // [am] resolution = WxH overrides the default.
    config_set("am", "resolution", "2560x1440");
    CHECK_EQ(common.call(60).get<int32_t>(0), 2560);
    CHECK_EQ(common.call(60).get<int32_t>(4), 1440);
    clear_config();
}
