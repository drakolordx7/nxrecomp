# Configuration of the game module (Metroid Prime Remastered)

The base's reference is `runtime/docs/CONFIG.md`. This page holds what only this game has: the keys its module reads and the sections it adds to
`controls.ini` (the module registers them as a table, `runtime/mpr_settings.cpp`; the F1 page and the launcher show the rows the table lists).

## Keys of `nxrt.ini` read by the game module

| Section | Key | Default | Environment variable | Source | Meaning |
|---|---|---|---|---|---|
| `game` | `modcon_query_guard` | `true` | `NXRT_GAME_MODCON_QUERY_GUARD` | `games/mpr/play_scenefix.cpp` | A decal render query skips a model scene node that is already in the scene's lists but whose model data has not loaded yet, as the normal draw path does. Without it, such a node crashes the game in `CFrustumPlanes::BoxInFrustumPlanes` (S42). `false` runs the game's code unchanged, for reproducing the crash. Needs the hook sites in `main.sites`. Read once. |
| `physics` | `jump_fix` | `true` | `NXRT_PHYSICS_JUMP_FIX` | `games/mpr/play_jump.cpp` | In the step where the jump timer ends partway, the jump force may raise the vertical speed only up to the rise cap, as on a fixed 1/60 step. Without it, a jump that is at the cap when its timer ends inside a long step goes up to about 6.9 x the step higher (+3.3 % with 16 ms frames; S49, S50). `false` runs the game's code unchanged. Needs the hook sites in `main.sites`. Read once. |
| `time` | `fix_scaled_float` | `true` | `NXRT_TIME_FIX_SCALED_FLOAT` | `runtime/mpr_timestep.cpp` | Per-tick float constants loaded into a vector register (a game's per-tick damping, `Load::ScaledFloat`) are scaled by the step. Old name `fix_friction` still works. |
| `time` | `fix_frame_counter` | `true` | `NXRT_TIME_FIX_FRAME_COUNTER` | `runtime/mpr_timestep.cpp` | The per-frame counter (texture scroll, UV animation, water; `Sites::tick_render_timings`) advances by the step instead of one frame per call. Old name `fix_render_timings` still works. |
| `time` | `fix_stream_step` | `true` | `NXRT_TIME_FIX_STREAM_STEP` | `runtime/mpr_timestep.cpp` | Texture streaming ages its entries by the step instead of 1/60 (`Load::Stream1`, `Stream2`). Old name `fix_streaming` still works. |
| `time` | `fix_scaled_reg` | `true` | `NXRT_TIME_FIX_SCALED_REG` | `runtime/mpr_timestep.cpp` | Per-tick float constants loaded into a general register (a background-work budget, `Load::ScaledReg`) scale with the step. Old name `fix_budget` still works. |
| `time` | `fix_tick_sampler` | `true` | `NXRT_TIME_FIX_TICK_SAMPLER` | `runtime/mpr_timestep.cpp` | A per-tick sampler (`Sites::per_tick_sampler`, a rolling window of one sample per tick) takes one sample per 1/60 s of game time instead of one per tick; above 60 ticks per second its window was too short. Old name `fix_stuck_tracker` still works. |

The `fix_*` keys of `[time]` are read by `runtime/mpr_timestep.cpp` when the step hooks are installed. The main module is always at the base's `mem::kMainBase`: the old keys `[play] game_base` and `[time] game_base` no longer exist.

## Keyboard beam and visor keys (`controls.ini`)

These live in the play-mode file `controls.ini` (key `[play] controls`), not in `nxrt.ini`. The F1 settings page writes the same file.

| Input | Action | What the game does |
|---|---|---|
| `1` | `beam_1` | Power beam |
| `2` | `beam_2` | Wave beam |
| `3` | `beam_3` | Ice beam |
| `4` | `beam_4` | Plasma beam |
| `5` | `visor_1` | Combat visor |
| `6` | `visor_2` | Scan visor |
| `7` | `visor_3` | Thermal visor |
| `8` | `visor_4` | X-ray visor |
| Mouse wheel up / down | `beam_prev` / `beam_next` | Cycle the beams you own, in the order power, wave, ice, plasma. Off when `[mouse] wheel_beams = 0`. |

How it works: the game only knows the Switch pad, so an action is turned into pad buttons by the `[switch]` table. By default `beam_1` to `beam_4` hold X and press the d-pad (up, right, down, left), and `visor_1` to `visor_4` press the d-pad alone (up, right, down, left). The wheel skips beams the player does not have yet. In the menu and map contexts they are switched off (`1` to `8 = none` in `[bindings.menu]` and `[bindings.map]`) and the wheel moves the cursor or the map instead.

To change a key, add a line `Key = action` to `[bindings.all]` (every context) or to one context (`[bindings.gameplay]`, `[bindings.morph]`, `[bindings.scan]`, `[bindings.menu]`, `[bindings.map]`). `Key = none` switches an input off in that context. Key names are SDL scancode names (`Left Ctrl`, `Page Up`, `Mouse1`, `WheelUp`).

`[meta] defaults_rev` records the revision of the factory bindings that the file was written with. The game writes it. A file without the section counts as `0`. When a release adds a new factory binding, the game adds it to an existing `controls.ini` at the next start, but only where that key is still unbound in that context. A key you bound yourself is left alone. A file written before `[meta]` existed has no record of what you removed, so a key you had unbound there gets its new default once. After that, a key you remove stays removed. The first time this happens, the old file is kept next to it as `controls.ini.bak` (an existing `.bak` is never overwritten), and the upgraded file is written at once. The log has one line saying what was added. A file with no `[bindings.*]` section gets the whole factory set and is not rewritten. A revision can also change a factory value (revision 3: `[aim] lock_mode` to `free`). A file that still has the old factory value takes the new one at the same upgrade; any other value stays.

`[mouse] menu_cursor`, default `1`: in menus the mouse works like a pointer: a visible cursor, hovering highlights an item, left click activates, right click goes back, the wheel scrolls. While it is on, the left click drives no binding in the menu and map contexts. The old key `menu_touch` is ignored; a file that still has it loads without a warning and nothing changes.

### Lock-on and camera feel (`controls.ini`, `[aim]` and `[camera]`)

`[aim] lock_mode`, default `free` (it was `classic` before defaults revision 3; a `controls.ini` still on `classic` from an older revision moves to `free` once, and a later choice of `classic` stays). `classic` is the game's own lock-on: the view turns to face the target. `free` starts the lock the same way, then centres the view on the target once. After that the mouse and right stick still turn the view, and the view is held so the locked target stays on screen inside the margin. The gun and reticle stay on the target, and shots home and strafing circle it, as in the normal lock. Releasing the lock returns to normal aim. The scan visor always uses the classic lock. See `docs/S32.md` for details.

`[aim] free_lock_margin`, default `0.15`, range `0` to `0.4`: the share of the screen kept clear along every edge while `lock_mode = free` holds the view. `0` lets the target reach the edge; `0.15` keeps it in the middle 70 percent.

`[camera] view_sway` (a game may name the key itself: Metroid Prime Remastered writes and reads `gun_drives_view`; the generic name is read too), default `1`. `1` is the game's own behaviour: the held item (Metroid: the arm cannon) is fixed to the view, so its idle drift, walking bob and landing dip move the whole view. `0` makes the view follow only the mouse and the stick. Those movements move the gun on screen instead, and the view stays still. Takes effect at once, no restart needed. Set it from the F1 page or the launcher, where it shows as "Steady camera" (ticked means `0`).

Checked in a real game run with `tools/test/keys_check.py` (warp to `tallon_canyon_cavern`, grant the other beams and visors, press each key and read the game's own beam and visor numbers): `1` to `4` give power, wave, ice and plasma, the wheel steps wave, ice, wave, and `5` to `8` give combat, scan, thermal and x-ray.

### Start-up and menus (`controls.ini`, `[tweaks]`: Metroid Prime Remastered's own section)

The `[tweaks]` section is not the base's: the game registers it (`nxrt/game/settings.h`, `Info::settings`; Metroid: `games/mpr/runtime/mpr_settings.cpp`), with its F1 tab and launcher pages. A game without one has no such section, tab or page, and `[system] language` falls back to the Windows language. The keys below are Metroid's.

All off by default; with them off the game starts and plays as it always did. They are on the F1 page (Extras tab, "Start-up and menus") and on the launcher's Extras tab. See `docs/S46.md`.

`[tweaks] skip_intro`, default `0`. `1` skips the opening cutscene of a new game (the gunship landing on the Frigate Orpheon) without a key press. It uses the game's own cutscene skip: in the Exterior Docking Hangar the cutscene counts as skippable, and the skip button is pressed for the game the moment the game accepts it (about two seconds in), then the game fades out and jumps to the state the end of the cutscene leaves. Other cutscenes play as usual. Takes effect at the next new game.

`[tweaks] skip_boot_logos`, default `0`. `1` ends the title's 14 second intro movie (the logo forming) as soon as it starts, so the title screen with "press start" comes up at once.

`[tweaks] language`, default `auto`. The game's language: `auto` uses the Windows display language when the game has it, else English; a name such as `French` or `LatinAmericanSpanish` picks one (names as in `[system] language`). The launcher's Extras tab sets it. `[system] language` in the game ini wins over it. Applies at the next start.

`[tweaks] fast_menus`, default `0`, and `[tweaks] menu_speed`, default `2`, range `1` to `4`. With `fast_menus = 1` the front end (title, main menu, save slots, difficulty, options, extras) runs its clock `menu_speed` times faster: its animations, fades, waits and the attract-mode timer. The transition movies between the title, the main menu and Extras are cut after 1/`menu_speed` of their length. The looping movies behind the menus, their sound and the game itself keep their speed. `menu_speed` is read only while `fast_menus` is on. Applies at once.

### Glow colour (`controls.ini`, `[tweaks]`)

`[tweaks] glow_color`, default `off`: `off` keeps the game's colours. `R,G,B` (0 to 255 each, as a colour picker shows them) recolours the glows of Samus's equipment: the morph ball's glowing stripes, inner glow, boost glow, trail and light, the arm cannon's charge glow and lit panels, and the suit's lights (the visor glow and the small lights on the chest, belt, knees and shins, on all four suits, from their glow maps). The colour multiplies the game's glow, so the ball's white glow takes it exactly, the charge's yellow mixes with it (magenta gives a pink-red charge), and a darker colour gives a dimmer glow. The morph ball's shell keeps its colours. The cannon's panels and the suit's lights keep the game's brightness and take the setting's hue, for every beam. Their glow textures are recoloured as the game uploads them, which takes about a second in the background. A change applies at once, with no restart. Details in `docs/S44.md`.

`[tweaks] suit_glow`, `ball_glow` and `gun_glow`, default `auto`: `auto` follows `glow_color`, `off` keeps the game's colour for that part even when `glow_color` is set, and `R,G,B` gives the part its own colour.

The launcher's Tweaks tab and the F1 page's Tweaks tab have the switch, three sliders with a colour swatch, a Reset button that goes back to the game's colours, and the per-part choice. The F1 page shows the colour in the game while the sliders move; Apply and save keeps it.

`NXRT_GLOW_TEST_FILE` (environment, test aid): a file of `[tweaks]` lines. When it changes, its values replace the saved ones in the running game (checked twice a second).

`NXRT_GLOW_LOG_TEXTURES` (environment, discovery aid): `1` logs the identity (tail hash) of every sRGB ASTC texture the glow's texture hook sees, once each, while a glow colour is on. `WxH,WxH,...` also claims every texture of those sizes under a numbered discovery id without changing it, so the runtime's `edited by the host (id ...)` lines match each texture id to its tail. See `docs/S44.md`.

`NXRT_GLOW_TEST_FILL` (environment, test aid): `1` paints every glow texture texel in the glow colour at full strength, to show whether a texture reaches the screen.

### Suit and arm cannon colour (`controls.ini`, `[tweaks]`)

`[tweaks] suit_color`, default `off`: `off` keeps the game's paint. `R,G,B` (0 to 255 each) repaints Samus's suit in that colour, anything from black to white, keeping the shading and detail of the game's textures. The morph ball and the spider ball follow it. The visor, the lights painted on the suit and the glow keep their colours (the glow has `glow_color`).

`[tweaks] gun_color`, default `off`: the same for the arm cannon's shell (first person, every suit and beam).

Both are on the launcher's Tweaks tab and the F1 page's Tweaks tab (a switch, three sliders and a swatch each, and "Reset the paint"). A change shows in the game about a quarter of a second after the slider stops; the textures are repainted in the background (about 0.1 to 0.15 s per texture). Details in `docs/S45.md`.

`NXRT_TINT_TEST_FILE` (environment, test aid): a file of `[tweaks]` lines (`suit_color = ...`, `gun_color = ...`). When it changes, its values replace the saved ones in the running game (checked twice a second).
