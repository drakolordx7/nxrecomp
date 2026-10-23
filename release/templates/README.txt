Metroid Prime Remastered for Windows (unofficial PC port)
=========================================================

What this is

This is an unofficial PC port of Metroid Prime Remastered that runs the game natively on Windows.
It contains no Nintendo data or keys: you supply your own legally dumped copy of version 1.0.0 of
the game (.nsp, .xci, .nsz or .xcz) and the prod.keys file from your own Switch.


What you need

- Windows 10 or Windows 11, 64-bit.
- A graphics card and driver that support Vulkan 1.3.
- 13 GB of free disk space on the drive you install to.
- Your dump of the game, version 1.0.0. Updates and patches are not supported.
- Your prod.keys file. If your dump was bought on another console, also put that console's
  title.keys file next to prod.keys.


First run

1. Start mpr_launcher.exe from the game folder. On a fresh folder it says there are no game files yet.
2. Press "Set up game files...". The setup tool opens with the game folder already filled in. (You can
   also start mpr_setup.exe yourself; its install folder defaults to the folder it is in.)
3. Pick your dump file, or drag it into the window. Check the green line under it.
4. Pick the keys file if setup did not find it, and check the install folder.
5. Press Start setup. On a 16-core PC it takes about 5 minutes; fewer cores take longer.
6. Press Stop at any time. Run setup again later and it carries on where it stopped.
7. When the setup window closes, the launcher says "Ready to play." and the Play button works.

When it finishes, the install folder holds these, next to mpr.exe:

- exefs\          the game program, copied from the dump unchanged
- romfs\          the game's data files
- bc7_cache\      the textures, converted for PC graphics cards (about 4.4 GB)
- shader_cache\   translated shaders
- mpr.ini         settings the game reads from this folder
- setup.log       what happened, every run
- setup_state.tsv what is finished, so a repeat run only checks sizes

Textures are converted once, during setup, at quality 3. Leave the bc7_quality value in mpr.ini
alone: the game must use the same value that setup wrote.

Command line users can run mpr_setup_cli --input dump.nsp --out D:\MPR. Exit codes: 0 done,
1 error, 2 the dump cannot be used, 3 keys problem, 4 disk problem, 5 stopped (run it again).


The launcher

mpr_launcher.exe is a small settings window that opens before the game. It needs no installer. It
reads and writes controls.ini in the game folder, the same file the F1 page in the game edits, so
the two always agree. Tabs:

- Display: window mode and size, frame rate limit, VSync, field of view.
- Extras: HDR, low latency (NVIDIA Reflex or AMD Anti-Lag 2), and the frame rate counter (on or off,
  corner, how much it shows, and its hotkey).
- Controls: mouse sensitivity, inverting, the morph ball camera, and which button icons the game shows.
- Keyboard: every action with up to two keys or mouse buttons. Double-click a key to change it and
  press the new key or mouse button. Delete clears the selected key.
- Controller: dead zone, trigger point, rumble, gyro, and what each controller button presses.

"Game folder" at the top shows where the game files are. "Change..." points the launcher at another
folder (it remembers the choice in mpr_launcher.ini next to the launcher). "Run setup again..."
starts the setup tool. Save keeps your changes; Play saves them and starts the game. Reset to
defaults puts every setting back to the factory value (nothing is written until you press Save or Play).


Playing

Press Play in the launcher, or double-click mpr.exe in the game folder. mpr.exe finds exefs, romfs,
bc7_cache, shader_cache and mpr.ini next to itself, so a shortcut to mpr.exe works too, and it
uses the settings the launcher saved. If setup has not been run yet, a message box tells you so.

The game opens in a resizable window. Alt+Enter switches borderless fullscreen on and off. F1 in
the game opens the settings page, which edits controls.ini. controls.ini is written with the
factory settings the first time you start the game (or the launcher saves it).

F2 shows and hides the frame rate counter, a small box in a corner with the frame rate and the frame
time. It is off by default. The corner, the detail and the key are set in the launcher (Extras
tab), on the F1 page (Display tab) or in controls.ini ([fps]).

Keyboard and mouse, by default:

- W A S D move. The mouse aims in first-person view only.
- Left button fires, hold to charge. Right button or Q locks on. Middle button or F fires missiles.
- Space jumps. Left Ctrl or C is morph ball. Left Alt is free look.
- Tab or M opens the map. Escape pauses. Arrow keys are the d-pad.
- 1 to 4 pick the beam, 5 to 8 pick the visor. The mouse wheel cycles the beams.
- In menus, Return, Space and the left button confirm. Escape, Backspace and the right button
  go back.

Controllers: Xbox, PlayStation and Switch pads work through SDL, and can be used with the keyboard
at the same time. Button names follow the printed label by default (Xbox A is Switch A). The
launcher or the settings page can switch to button position instead, and can make any controller
button press any Switch button ([padmap] in controls.ini). The mouse aim, the Switch button layout and
the beam and visor combinations have not been played through yet, so expect rough edges there and
use the F1 settings page to change what feels wrong. The game shows Xbox, PlayStation, keyboard or Switch button icons, chosen in the launcher (Controls tab), with [prompts] style in controls.ini (auto, keyboard, xbox, playstation or switch) or on the F1 settings page, Controller tab. The default, auto, shows keyboard and mouse icons until you use a controller, then that controller's icons. The keyboard icons show the keys you bound, so a rebound jump key shows the new key.

Settings you can change. Each line is optional; a missing key keeps its default.

- controls.ini, [display] window_mode: windowed (default), borderless or exclusive.
- controls.ini, [display] vsync: 0 (default) or 1. fps_cap: frame limit, 60 by default, 0 for none.
  The launcher's Display tab sets these.
- mpr.ini, [gfx] gpu: discrete or integrated, or part of a GPU name, if the wrong card is chosen.
- mpr.ini, [am] operation_mode: docked (default) or handheld.
- mpr.ini, [audio] volume: 0.0 to 1.0. device: default, or part of an output device's name.
- mpr.ini, [fs] save_dir: the folder for save data. Default: %APPDATA%\MPRRecomp\save.

mpr.ini is a plain text file that setup writes next to mpr.exe. Add a section name in square
brackets and the key = value lines under it. Do not change [nvn] bc7_quality. Setup rewrites
mpr.ini each time it runs while the first line (a comment that starts with "; Written by
mpr_setup") is there, so delete that line once you have edits you want to keep.


Updating

Extract the new release over the old game folder. The zip does not contain your game files, so
exefs, romfs, bc7_cache, shader_cache and mpr.ini stay. controls.ini is not in the zip either, so
it stays too. Run setup again only if the release notes say so.


Troubleshooting

- "The keys cannot open this dump": your keys are older than the game. Dump the keys again from a
  console on recent firmware.
- "This is an update" or "different game": use the original version 1.0.0 base game dump.
- "The dump is damaged": copy the file again, or dump the game again. Setup checks the dump's own
  hashes before it writes anything.
- "Not enough free space": free some space or pick another drive. Running setup again is safe.
- Setup stopped halfway (power cut, crash): run it again. Finished files are kept.
- Black window, or a Vulkan message: the game needs a Vulkan 1.3 driver. The messages
  "Vulkan 1.3 is required", "no usable Vulkan 1.3 GPU" and "no Vulkan loader found
  (vulkan-1.dll)" mean the driver is missing or too old. Update the graphics driver.
  If the window stays black without a message, send mpr.log (see below).
- Missing DLL: the three astcdec-*-shared.dll files must stay in the same folder as mpr.exe and
  mpr_setup.exe. Extract the whole zip, not single files.
- The launcher says "controls.ini could not be read": the file has a typo or a bad value. The launcher
  shows the defaults and, when you save, keeps your old file as controls.ini.bak and writes a new one.
- The launcher says the game files are missing but you ran setup into another folder: press "Change..."
  and pick that folder.
- The game does not start: look for mpr_fatal.txt next to mpr.exe, which is written after a fatal
  error, and for mpr.log.
- Log files: the game writes mpr.log in the game folder (the previous run is kept as mpr.log.prev).
  Setup writes setup.log in the install folder.
- When you ask for help, send mpr.log, mpr_fatal.txt if it exists, and setup.log. setup.log
  contains no key data. Never post prod.keys or title.keys, and never post your dump.


Licence

This port is free software under the GNU General Public License, version 3 (GPL-3.0). See LICENSE.
Third-party licence notes are in THIRD_PARTY_NOTICES.txt.

Metroid Prime Remastered is copyright Nintendo and Retro Studios. It is not included in this
package.
