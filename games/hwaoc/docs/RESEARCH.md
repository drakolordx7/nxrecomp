# Research: Hyrule Warriors: Age of Calamity

Checked 2026-11-16. Facts from our own dump are marked (dump); the rest has a source link.

## Title and versions
- Title ID `01002B00111A2000` (base), update `01002B00111A2800`, DLC `01002B00111A3001..3004`; AddOnContentBaseId `0x01002b00111a3000`, SaveDataOwnerId `0x01002b00111a2000`, DisplayVersion `1.4.1` (dump).
- Target: update 1.4.1 (v458752), the newest update in the dump. Its only listed change is the Switch 2 Edition upgrade discount via the main menu "eShop" option ([GoNintendo](https://gonintendo.com/contents/64815-hyrule-warriors-age-of-calamity-updated-to-ver-1-4-1), [OatmealDome post, about 2026-09-10](https://x.com/OatmealDome/status/2097903655319978090)). So 1.4.1 is code-wise a small step above 1.3.0.
- Another native-port project, [seedds/nxnative](https://github.com/seedds/nxnative/issues/536) (Apple silicon, Metal), carries a 60 fps patch set for 1.3.0 (main `c3cf52bf...`) and an issue that ports it to 1.4.1. Its main hash `e5f275b5d0889899e55de1a164eef060` matches our build ID, which confirms the dump is the real 1.4.1 main. That issue also lists where the engine keeps its frame rate (see "Frame rate" below).
- Switch 2 Edition is a separate title and not our target.

## Dump (all six NSPs copied to `game/dump`)
| File | Content | Size |
|---|---|---|
| base `[01002B00111A2000] [USA] [v0]` | Program (ExeFS + RomFS), Control, Manual | 10.9 GB |
| update `[01002B00111A2800][v458752][Up v1.4.1]` | patch Program (full ExeFS + BKTR RomFS), Control, Manual | 436 MB |
| `...3001` Digital Version Preorder Bonus | PublicData: `DLCDATA.dat` only | 118 KB |
| `...3002` Expansion Pass Purchase Bonus | PublicData: 4 files (`asset/ScreenLayout.rdb.bin1_0` etc.) | 88 MB |
| `...3003` Pulse of the Ancients | PublicData: 15 files (`asset/data/0x....file`) | 218 MB |
| `...3004` Guardian of Remembrance | PublicData: 186 files (`movie_dlc2/*.webm`, `asset/`) | 1.1 GB |

Key generation 11 (master key 0a), rights-ID crypto with fake-signed tickets; all keys from `games/mpr/game/keys`.

## ExeFS (update 1.4.1; `game/extract/update/exefs`)
Only one subsdk (`subsdk0`), like mpr and ap.

| NSO | NSO file | text / ro / data (decompressed) | bss | Build ID |
|---|---|---|---|---|
| main | 17,908,454 | 28,662,064 / 11,432,496 / 1,299,392 | 4,422,720 | `E5F275B5D0889899E55DE1A164EEF060D547B0A8` |
| rtld | 6,790 | 5,728 / 3,208 / 640 | 3,456 | `51B657503637C8A8D6729CE7D8DDAA6A163C356A` |
| sdk | 5,368,143 | 5,519,040 / 6,642,000 / 397,408 | 692,128 | `95832210B36121E88B4F9B7B66239DADE9C82A67` |
| subsdk0 | 3,522,072 | 3,582,656 / 2,447,056 / 198,344 | 1,161,528 | `3AB92C3444D4ECE2F52417B5D8EE86EB115212A3` |

- Base v0 main: 16,814,561 bytes, text 26,608,592, build ID `935EBE83C4A0658348A8A1C05CF8DAC3F43CEB84`. rtld, sdk and subsdk0 are byte-identical between base and update (only main and main.npdm change).
- ELFs (nx2elf): `work/elf/{main,rtld,sdk,subsdk0}.elf`; main.elf is 41.4 MB (mpr 33 MB, ap 27.5 MB).
- SDK: `NintendoSdk_nnSdk-9_6_0-Release` in sdk; main has `NintendoSDK_gfx-9_6_0` and `libz-9_6_0`; sdk has `NintendoSDK_NVN-9_6_0`; subsdk0 has `NintendoSDK_movie-9_6_0`. **SDK 9.6.0** (every program NCA reports SdkAddon 9.6.0). Older than ap (10.4.1) and mpr (15+); main.npdm kernel version is 9.1.
- main.npdm: main thread priority 44 on core 0, stack 1 MiB, thread priority range 28..59, cores 0..3, handle table 0x200, SystemResourceSize 0, 64-bit application, debug flag on. Services requested: acc:u0 aoc:u apm appletOE audin:u audout:u audren:u banana bcat:u bsd:u bsdcfg caps:su caps:u csrng friend:u fsp-srv hid htc htc:tenv htcs hwopus irs ldn:u ldr:ro lm mii:u mm:u nfc:mf:u nfc:user nfp:user ngct:u nifm:u nim:eca notif:a nsd:u ntc nvdrv pcm pctl pl:u prepo:u set sfdnsres ssl time:u vi:u.
- NACP: 13 languages (AmericanEnglish, BritishEnglish, Japanese, French, German, LatinAmericanSpanish, Spanish, Italian, Dutch, CanadianFrench, Korean, Traditional and Simplified Chinese), startup account required, AddOnContentRegistrationType OnDemand, user save 2 MiB + 2 MiB journal, local communication ID = title ID, video capture enabled.

## Graphics API: NVN
- main, sdk and subsdk0 all reference `nvnBootstrapLoader`; main carries 517 `nvn*` names (the nn::gfx NVN backend is linked into main and resolves entry points through `nvnDeviceGetProcAddress`), sdk holds the NVN loader (28 names). No Vulkan strings, no `nvnGlslc*` entry points, no GLSLC library found (sdk only has `glslc_Alloc/Free/Realloc/GetAllocator` allocator-hook names). main does contain `#version 440 core` GLSL text and many `#define` preprocessor strings; whether shaders are compiled on the device or precompiled in the RomFS is open (look at the first boot). ap's GLSLC risk does not show in this dump.
- Same API as mpr and ap, so the base's NVN-on-Vulkan path applies.

## Engine and middleware
- Koei Tecmo **KTGL** (Koei Tecmo Game Library, since renamed Katana Engine, by Future Tech Base): main has 453 `KTGL*` strings, `KIDS*` strings (Koei Tecmo's tool/system layer) and `mud::from_kidshost::EvaluateScript` script bindings. Sources: [PCGamingWiki Katana Engine](https://www.pcgamingwiki.com/wiki/Engine:Katana_Engine), [Wikipedia](https://en.wikipedia.org/wiki/Katana_Engine).
- Audio: nn::audio renderer (88 audio symbols in main), nn::codec `HardwareOpusMultiStreamDecoder` (service `hwopus`). Audio archives are `IDRK0000/SRST` ([srst-extractor](https://github.com/hackyourlife/srst-extractor) writes `kns` tracks).
- Movies: subsdk0 is Nintendo's nn::movie 9.6.0: Android-derived stagefright/OMX (`SoftOpus`, `SoftVorbis`, `video_decoder.vpx`, libwebm, `jpegTegra*`, `mmnv`, `SfNvnUtil`, a curl HTTP connection class). The RomFS has 97 `.webm` movies plus 4 logo movies, so playback goes through mm:u and the nvdrv decode path.
- Input and extras: nn::hid with vibration, amiibo (nfp, 14 symbols, an amiibo window in the UI), nn::ec shop (the eShop upgrade offer), nn::oe, nn::account, nn::nifm, nn::aoc.
- Not found in main or subsdk0: CRI, Havok, Wwise, FMOD, Bink, Umbra, SpeedTree, PhysX (a single `Bullet` hit is UI text). Physics, animation and render code is KTGL's own.

## RomFS layout (update, `game/extract/update/romfs`, 1,102 files, about 11 GB; the base alone has 1,086)
- `asset/` (33 files, 9.7 GB): eight `.rdb` indexes (`system`, `ScreenLayout`, `EventMaker3`, `CharacterEditor`, `FieldEditor4`, `KIDSSystemResource`, `MaterialEditor`, `RRPreview`; magic `_DRK0000`), each with `.rdb.bin` plus split data `.bin1` to `.bin4` (magic `IDRK0000`; `MaterialEditor.rdb.bin` 3.8 GB, `FieldEditor4` 2.5 GB, `RRPreview` 1.1 GB, `system` 0.9 GB).
- `asset/data/` (970 files, 1.4 GB): `0x<hash>.file` containers, `IDRK0000` magic, 13 KB to 221 MB; names are hashes.
- `data/`: `LinkData2.bin` (37 MB) and `LinkInfo2.bin` (314 KB), the text and link database (a community text dump lists text blocks by language from "LinkData").
- `movie/` (93 `.webm`, 1.3 GB) and `movie_logo/` (4 `.webm`).
- Extension tally: 970 `.file`, 97 `.webm`, 10 `.bin`, 8 `.rdb`, 6 `.bin2`, 5 `.bin3`, 5 `.bin1`, 1 `.bin4`.
- DLC adds `asset/*.rdb.bin*` pieces, `asset/data/*.file` and `movie_dlc2/*.webm` under `game/extract/dlc/<id>`.
- Mods: the content sits inside hashed archives, so a RomFS override needs per-file overrides in `asset/data/` or an rdb-level layer in our runtime.

## Frame rate (from the nxnative 1.4.1 issue; not yet checked by us)
- The engine builds a KTGL device config on the stack at startup (`{1920, 1080, 3, 9, 5, interval 2, ...}`), keeps the present interval and a frame rate of 59.94 / interval, and its frame-rate block holds `{30, 2, 30, 30.0}`. The default is a 30 fps game (present interval 2). Interval 1 with only the task wait changed makes the game clock run at twice real time, so a real variable timestep has to handle the clock, the per-object step and the task rate together. Addresses on 1.4.1 (offsets in the main image): device config `0x8d9ae0`, task fps `0x1670124`, object step `0x16702d0`, frame-rate block `0x26a2280`, device pointer `0x2bb0798`, 59.94 constant `0x1ef8ddc`.
- Emulator cheat sets: [NX-60FPS-RES-GFX-Cheats entry](https://github.com/ChanseyIsTheBest/NX-60FPS-RES-GFX-Cheats/blob/main/titles/01002B00111A2000/Hyrule%20Warriors%20Age%20of%20Calamity.txt) (FPS codes by iCris4, resolution by Hazerou): at 60 fps the game slows when frames drop and some cutscenes run fast; resolution above 810p glitches. [Fl4sh9174 ultrawide/FPS mods](https://github.com/Fl4sh9174/Switch-Emulator-Ultrawide-FPS-Mods/issues/230) (2026): ultrawide codes crash story mode in some emulator builds, fine with the codes off. Lesson: cutscenes and some effects are tied to the 30 fps step, so cutscenes may need to stay at their native rate.

## Emulator issues
- Ryujinx: crash-on-launch report ([actions run](https://github.com/Ryujinx/Ryujinx/actions/runs/8863125424), 2024) and a frame-rate guide thread ([r/Ryujinx](https://www.reddit.com/r/Ryujinx/comments/mjpy1z/hyrule_warriors_age_of_calamity_status/); the page text was not readable by our fetcher).
- yuzu era: cutscene low fps and a freeze after the first cutscenes ([r/yuzu](https://www.reddit.com/r/yuzu/comments/oq6725/hyrule_warriors_age_of_calamity_crashing/)), shader stutter and 60 fps mod threads ([1](https://www.reddit.com/r/yuzu/comments/11ikkt1/hyrule_warriors_age_of_calamity_60_fps_mod/), [2](https://www.reddit.com/r/yuzu/comments/ytrhp2/i_have_been_trying_to_get_age_of_calamity_to_run/)).
- Eden/Citron/Ryujinx: the [ultrawide crash note](https://github.com/Fl4sh9174/Switch-Emulator-Ultrawide-FPS-Mods/issues/230) says builds differ on the same save (works in some, loading-screen hang in others). The Eden cutscene freeze reports found are for Breath of the Wild (movie segment transition, macro engine assert), not this game. Takeaway: video decode and story cutscenes are where emulators struggle, which fits nn::movie being in subsdk0 here.

## File format tools
- [eterniti/rdbtool](https://github.com/eterniti/rdbtool) (CLI) and [qrdbtool](https://github.com/eterniti/qrdbtool) (GUI): extract and insert files in KT `.rdb`. Made for PC KT games; the Switch IDRK/_DRK variants need checking.
- [DeathChaos25/fdata_dump](https://github.com/DeathChaos25/fdata_dump): other Koei Tecmo archive (`.fdata`), different format.
- [hackyourlife/srst-extractor](https://github.com/hackyourlife/srst-extractor): this game's IDRK0000/SRST audio.
- [SolidLink95/Age_of_Calamity_Datamining](https://github.com/SolidLink95/Age_of_Calamity_Datamining) and [zoalasaurusrawr/hwaoc](https://github.com/zoalasaurusrawr/hwaoc) (text from `LinkData`); [PythWare](https://github.com/PythWare/) GUI tools for Koei Tecmo / Omega Force games; [ren-/KTFileLoaderLogger](https://github.com/ren-/KTFileLoaderLogger) (logs resources KTGL mounts, PC); [TitanUnpacker](https://github.com/TraduSquare/TitanUnpacker) (other KT titles).
- No public PC port or recomp of this game was found. nxnative has a profile for it (native on Apple silicon).

## Extraction recipe (what worked here)
1. NSPs: `nstool -k keys/prod.keys -x / <outdir> <nsp>` (set `MSYS_NO_PATHCONV=1` in Git Bash so `/` is not rewritten). nstool prints "Failed to load title.keys": harmless.
2. Base program NCA: `nstool --tik <base .tik> --part0 <exefs dir> <nca>` and `--part1 <romfs dir>` (partition 0 is ExeFS, 1 is RomFS, 2 is the logo).
3. Update ExeFS: `nstool --tik <update .tik> --basenca <base nca> --part0 <dir> <update nca>`.
4. Update RomFS (BKTR): nstool cannot, and hactool needs both title keys. Make a scratch HOME with `.switch/prod.keys` (hactool aborts on keys whose value is not 32, 64, 256 or 512 hex digits, so drop the `mariko_master_kek_source_*` and `eticket_rsa_keypair` lines) and `.switch/title.keys` holding `<rightsid>=<encrypted title key from each .tik>` for the base and update rights IDs, then `HOME=<scratch> hactool --disablekeywarns --suppresskeys --basenca=<base nca> --romfsdir=<out> <update nca>` with output redirected to a file. It writes the full patched RomFS. Delete the scratch HOME afterwards (it holds keys).
5. DLC: `nstool --tik <dlc .tik> --part0 <dir> <PublicData nca>`.
6. ELFs: `nx2elf <exefs>/<nso>` writes `<nso>.elf` beside the input; move into `work/elf`.
The base setup tool (`base/tools/setup`) also applies updates and DLC; using it for this game needs the manifest in `game.json` and a build of the setup exe (not done in M0).
