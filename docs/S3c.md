# S3c: boot-path OS services (IPC level)

Stream S3c of the Metroid Prime Remastered recompilation. The recompiled SDK (`sdk`, `subsdk0`, `main`) talks to Horizon through SVCs and IPC sessions. The SVC layer is in `src/kernel`; this stream answers the IPC services a Switch application needs at boot and while it runs. Everything lives in `src/services/` and is registered by `nxrt::services::register_all()` (called from `nxrt::init`, switched off with `InitOptions.services = false`). The full command-by-command table is generated: `docs/S3c_commands.md` (`python -I scripts\svc_table.py --out docs\S3c_commands.md`).

## Which services

`game/exefs/main.npdm` carries the Service Access Control list of the title (47 names): `acc:u0 aoc:u apm appletOE audin:u audout:u audren:u banana bcat:u bsd:u bsdcfg caps:su caps:u csrng ectx:aw friend:u fsp-srv hid htc htc:tenv htcs hwopus irs ldn:u ldr:ro lm mii:u mm:u nfc:mf:u nfc:user nfp:user ngct:u nifm:u nim:eca notif:a nsd:u ntc nvdrv pcm pctl pl:u prepo:u set sfdnsres ssl time:u vi:u`. All of them answer (test `svc_boot_flow`). `fatal:u/p`, `set:sys`, `psm`, `time:a/s/r`, `apm:sys` are provided too because the SDK's client library contains code for them. The `nn::` calls the game's `main` imports (422 imports) use: `nn::oe` (am), `nn::time`, `nn::account`, `nn::settings`, `nn::fs` (ROM, save data), `nn::audio` (the audio renderer), `nn::vi`, `nn::hid` (npad, six-axis, vibration, touch), `nn::os`.

control.nacp facts (also the defaults of `TitleProfile`, section `[title]`): 13 supported languages, startup user account required, user account save data 4 MiB plus 1 MiB journal, no device or cache save data, no BCAT, add-on content base id 010012101468D000. main.npdm: main thread priority 44 on core 0, stack 0x100000, 39-bit address space, system resource size 0.

## What is real and what answers "not here"

| Group | Services | State |
|---|---|---|
| Process info | GetInfo ProgramId from the profile; the rest of GetInfo was already in `src/kernel` | real |
| File system | `fsp-srv` | RomFS served as an IStorage over an image synthesised from the loose game folder (the SDK parses it), read-only IFileSystem over the same folder, save data in folders under `%APPDATA%\MPRRecomp\save` with journal semantics and two-phase commit, SD card / game card / BIS absent |
| System | `set`, `set:sys`, `time:u/a/s/r` (+ time zone rules from the host zone), `apm`, `apm:sys`, `psm`, `lm` (guest log into the runtime log), `fatal:u/p` (crash report file) | real |
| Applet | `appletOE` (+ `appletAE` proxy): message queue with focus / operation mode / performance mode events, launch parameters, EnsureSaveData, desired language; host-side control in `services_api.h` (`amctl::set_docked`, `set_focus`) | real |
| Account | `acc:u0`, `acc:u1`: one local user | real |
| Input | `hid`: applet resource shared memory (npad, touch, keyboard, mouse, debug pad), 200 Hz sampling from `hid::InputSource` (`include/nxrt/input_source.h`), vibration and six-axis commands accepted | real; the real input source comes from the input stream |
| Fonts | `pl:u/s` with the shared-font memory (DejaVu Sans stands in for the console fonts, documented below) | real |
| Display and GPU init | `vi:u/m/s` (display, layer, binder parcels), `nvdrv` (+ `:a/:s/:t`) with the nvmap / ctrl / ctrl-gpu / as-gpu / gpu ioctls the graphics init issues | answers with plausible values; no GPU channel submission (the graphics stream replaces NVN) |
| Audio | `audren:u` (renderer ported from Ryujinx.Audio: session, update protocol, DSP, mix, host sink), `audout:u`, `audin:u` | real |
| Offline | `nifm`, `bsd`, `bsdcfg`, `ssl`, `nim:eca`, `friend`, `prepo`, `pctl`, `bcat`, `ldn:u`, `nsd:u`, `sfdnsres`, `ntc`, `ngct:u`, `notif:a` | a console without a connection, friends or BCAT |
| Misc | `csrng`, `ldr:ro`, `mm:u`, `hwopus`, `aoc:u`, `caps:u/su`, `mii:u`, `irs`, `nfc:*`, `nfp:user`, `ectx:aw`, `pcm`, `banana`, `htc*` | small real answers; devices that are not there report so |

## Hooks for the other streams

* `services::amctl::set_docked / set_focus` (window focus, handheld switch), `services::profile()` (language, region, user, save sizes), `nxrt::hid::set_input_source`, `nxrt::audio::set_sink` (`[audio] sink = xaudio2` selects the XAudio2 sink), `[fs] romfs`, `[fs] save_dir`. Keys are in `docs/CONFIG.md`.
* The input stream may override `nn::hid::GetNpadState*` at the import: the shared memory is kept up to date either way.
* A loose-file mod folder over RomFS is not implemented: `RomfsImage::build` takes one folder; layering several folders is a change inside `fs_romfs.cpp`.

## Tests

CTest groups `svc_*` (one per service unit) plus `svc_fs_volume`, `svc_fs_romfs`, `svc_boot_flow` (every SAC service answers; the start-up am -> acc -> fs sequence agrees on the user id) and the `svc_audren_*` groups (the ported renderer). Request and expected reply bytes are written out from Switchbrew / libnx layouts, not derived from the service code. Rerun: `pwsh scripts\build.ps1 -Test` (everything) or `pwsh scripts\t.ps1 <group> ...` (build + named groups).

## Decisions not in the brief

* The service library compiles into `nxrt` and registers itself from `nxrt::init`; a service object is thin and its process-wide state is a singleton with a `*_reset()` hook (`services::reset_all()`).
* Uid byte order: the first u64 in memory is `uid_hi` everywhere (debug note 017).
* Firmware version reported: 12.1.0 (SDK 12.3.1 era), `[system] firmware`.
* apm performance mode follows the operation mode (docked = Boost) until `apm:sys RequestPerformanceMode` is called.
* Region numbers are Switchbrew's RegionCode (0 Japan, 1 USA, 2 Europe, 3 Australia, 4 HongKongTaiwanKorea, 5 China).
* Save data lives in a committed folder plus a per-process work folder; uncommitted writes are dropped at unmount, as on the console. The journal size limit is not enforced (GetFreeSpaceSize reports total minus used).
* RomFS: an image is synthesised from `game/romfs` (7.2 GB image, 341 files), not an extraction of the original archive; the SDK's own parser reads it through IStorage.
* `err:u` does not exist in any reference; the "err" service the SAC list names is `ectx:aw`.
* The kernel was changed where the real SDK needed it (debug notes 014-016): Release does not close the handle, MapMemory takes a module data source, map calls accept the ASLR region.

## Gaps

| Gap | Why |
|---|---|
| `set` GetKeyCodeMap, set:sys Tv/Notification/Account settings | single-source layouts, not verified |
| Fonts | the real console fonts are not in the dump; DejaVu Sans fills every slot (no CJK glyphs); a folder `[pl] font_dir` with the plain `.bfttf` files replaces slots |
| `nfc:mf:u` IUser, `pcm` | no command table in any reference |
| Journal size limit of save data | not enforced |
| hid: real input | arrives with the input stream (`InputSource`); six-axis and vibration are accepted and ignored |
| Uncertain result codes | listed in the worker notes of each service file header; they come from one source only (ScarletNX or Ryujinx) |
| Nothing was run against a console | all answers come from Switchbrew, ScarletNX, Ryujinx and Wonder; the real game was booted against them up to the first graphics call (debug note 018) |
