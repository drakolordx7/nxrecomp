# Frame step: high refresh with a real variable timestep

Age of Calamity is a 30 fps game: every update steps the game by whole 60 Hz vsyncs (2 per update) or by 1/30 s. On PC every update
(one per rendered frame) is stepped by the real time it took. The base measures and paces it (`nxrt/timestep.h`, Info::frame_update);
`runtime/hwaoc_timestep.cpp` hands that time to the game's own mechanisms. Sites: `recomp/addrs.tsv`, section "frame step".

## How the game steps (1.4.1, checked in IDA on our ELF)
- KIDS frame task `main+0x166fff0`, once per update, before the scene: reads the task fps (`[x19,#0x30]`, 30), waits until
  1/fps has passed since the last update (sleep 1 ms or run jobs), measures the real time (`[x19,#0x24]`), then takes the update's
  step from the frame-rate object (`x23`: `+0x34` step = timescale / fps, `+0x44` a second step, `+0x48` fps 30), or with no
  object 1/fps or the measured time. The step goes to `[x19,#0x18]` and into the system clocks (`+0x3348`, `+0x3358`).
- Frame-rate block `main+0x26a2280` = {fps, interval, fps, float fps} = {30, 2, 30, 30.0}, written only by the game's interval
  request (`main+0x8a35e0`, `main+0xfc3c50`: fps = 60 / interval). The battle steps by it: 117 places read the interval as a float
  multiplier (`ldr` + `ucvtf`), about 55 as an integer (timers `+= interval`, `-= interval`, `== 2` tuning switches, copies into
  script state), 25 divide by the float fps, one reads the integer fps as a float (`main+0xb09ac8`).
- KTGL device frame rate (device `+0x32d0`, 59.94 / interval): the event scripts step by it.
- Event cutscenes count whole 30 fps frames: the scene update (`main+0x857510`) adds 1 to the cutscene's frame and takes 1 from
  its frames left on every update.
- The present interval (device config, 2) is ignored by the base's NVN present: presents follow the updates.
Sources: seedds/nxnative issue 536 and its `patches.py` (fixed 60 fps on 1.3.0/1.4.1) for where to look; every site re-read here.

## What runs variable
| Mechanism | On PC |
|---|---|
| Update rate | one update per rendered frame; the task's own 1/fps wait is skipped (`kTaskWait`), the base paces to the frame cap at the task's entry |
| Game clock (frame-rate object steps) | timescale x real time (`kObjStep`; `kClockStep` without the object) |
| Interval read as a float (117 sites) | fractional 60 Hz ticks of this update, `dt x 60` (`kTick_*`) |
| Float fps (25 divisions) and integer fps read as float | `1 / dt` (the block, `kFpsRecip`) |
| Device frame rate (event scripts) | `59.94 / (dt x 60)` |
| Input, camera | read and stepped in every update, so every rendered frame |

## What keeps its native rate (and why)
- Integer interval users (timers, counters, `interval == 2` switches): the integer interval is the whole 60 Hz ticks that real time
  has accumulated by this update (0, 1, 2, ...), so they keep real time on average and step in 1/60 s quanta. Above 60 fps some updates
  get 0 (nothing counts down that update); at 30 fps it is 2 as on the Switch. A rate that alternates 1 and 2 (40..59 fps) also flips
  the `== 2` tuning switches between updates.
- Event cutscenes stay on 30 fps frames: the frame and frames left advance by the whole 30 fps frames of real time (0, 1 or 2 per
  update), so a cutscene keeps its speed and sync with its voices at any rate but its camera and animation move in 1/30 s steps.
  Interpolating them needs the cutscene's own timeline: not done.
- Movies play on the host clock (base media).

## Settings
- Frame rate limit: the base's Display page and F1 (`controls.ini` `[display] fps_cap`, VSync), applied at the frame boundary.
- Game step: `[display] game_step = real | retail` (`runtime/hwaoc_settings.cpp`), a row under "Game speed" on the same page and F1
  tab: retail leaves the game's own 30 fps step and wait alone (the hooks stay registered and pass through; F1 switches it live).
- `[time]` in `hwaoc.ini` (base keys, `nxrt/timestep.h`): `mode = real` (default) | `off` (the retail 30 fps step, nothing hooked),
  `max_rate` (the step is never shorter: an uncapped run is held to it), `min_rate = 30` (a slower update makes the game run slow, as
  on the Switch), `smooth`, `log_every`.

## Checked (prologue battle, headless 720p, `--preset fps`)
- 60 cap: 60.0 Hz steady until the battle; 144 cap: 140-150 Hz in the menus and movies; uncapped: 480 Hz (the base's max_rate) in menus, 170 Hz in a loading screen.
- Speed: Link held forward for 3 s of host time ends at the same place on the bridge with the 30 fps step (cap 30, identical to retail), the 60 cap run and
  the 144 cap run (the battle itself ran 33-45 Hz on the busy test PC in both: a fractional-tick step). The battle was not reached above 60 Hz.
- Limits seen: an uncapped run (480 Hz) keeps the loading screen up much longer on a busy PC (the update loop competes with the loader threads);
  use a cap (the default is 60). A start freezes in 2 of 6 starts at frame 2 with the hooks off as well (the main thread waits in the game's start-up
  job sync): not caused by the frame step.

## Risks not checked
- Places that copy the integer interval into object or script state at creation (`main+0x6d6968`, `0x7644e0`, `0x6e7c94`,
  `0x6f3af8`) take whatever the current update's whole ticks are; a 0 there (above 60 fps) could stall that object's timer.
- Two reads pass the interval to a virtual call in `s0` as raw bits (`main+0x158c10`, `0x158c60`) and one (`main+0x1621d0`) converts
  it far from the load: they get the integer interval.
- Per-update logic that steps by neither the block nor the clock would run faster above 30 fps.
