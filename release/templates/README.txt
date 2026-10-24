Metroid Prime Remastered for Windows (unofficial PC port)
=========================================================

A native Windows port of Metroid Prime Remastered, made by recompiling the Nintendo Switch release (version
1.0.0) into a normal Windows program. It is not an emulator. It has a launcher, a setup tool, mouse and keyboard
controls, any resolution and aspect ratio, and high refresh rates.

Latest release and source: https://github.com/drakolordx7/MetroidPrimeRemasteredRecomp/releases/latest

No game files are included. You need your own dump of Metroid Prime Remastered version 1.0.0 and the keys from
your own Switch. Not affiliated with Nintendo or Retro Studios.


Requirements
------------

- Windows 10 (1903 or later) or Windows 11, 64-bit
- A GPU and driver with Vulkan 1.3. It has been tested on an NVIDIA GeForce RTX 3080 only; AMD and Intel GPUs
  are untested
- About 14 GB of free disk space
- Your dump of the game, version 1.0.0 (.nsp, .xci, .nsz or .xcz; updates and patches are not supported) and
  your prod.keys. If the dump was bought on another console, also put that console's title.keys next to
  prod.keys


Usage
-----

1. Unzip the release into a folder with a short path (under about 180 characters, for example D:\Games\MPR) and
   run mpr_launcher.exe.
2. Press "Set up game files...", choose your dump and your prod.keys, and press Start setup. It takes about 5
   minutes on a 16-core PC, longer on fewer cores. You can stop it and run it again later; it carries on where it
   stopped.
3. When setup closes, the launcher prepares shaders for your graphics card (a few minutes, once for each driver
   version). When it says "Ready to play.", press Play.

Settings are in the launcher (display, mouse, keyboard, controller, button icons) and on the F1 page in the game.
They are saved in controls.ini in the game folder. Saves are in %APPDATA%\MPRRecomp\save. The logs (mpr.log and
setup.log) are in the game folder. To update, unzip a new release over the old folder. To uninstall, delete the
folder, and the save folder if you want your saves gone.

Controls: WASD move, the mouse aims, the left button fires, the right button or Q locks on, the middle button or F
fires missiles, Space jumps, Ctrl or C is the morph ball, Tab or M opens the map, Escape pauses, 1 to 4 pick the
beam and 5 to 8 pick the visor. Xbox, PlayStation and Switch controllers work. Alt+Enter switches fullscreen and
F2 shows the frame rate. HELP.txt lists every key, every setting and the fixes for common problems.


Reporting problems
------------------

Open an issue at https://github.com/drakolordx7/MetroidPrimeRemasteredRecomp/issues and attach mpr.log from the
run where it happened (the next run keeps the old one as mpr.log.prev). The version is in the top right corner of
the launcher and on the "mpr 0.1.0" line near the top of mpr.log. For a crash, also attach mpr_crash.txt or
mpr_fatal.txt if they exist. Never attach game files, your dump or your keys.


Credits
-------

Maintained by drakolordx7 (https://github.com/drakolordx7).

Built on the work of others: smo-recomp, SuperMarioWonderRecomp, ssbu-recomp, ScarletNX and LASRecomp (recompiler
and runtime design), Ryujinx (shader translator and audio renderer), Eden and yuzu (service layouts and a
reference to check against), retrotool and PrimeRemasterStructs (file formats), SDL3, Dear ImGui, volk, Vulkan
Memory Allocator, Vulkan-Headers, astc-encoder, bc7enc_rdo, zstd, AMD FidelityFX FSR 1, DejaVu Sans and the Kenney
Input Prompts icons. Links and licences are in THIRD_PARTY_NOTICES.txt and the licenses folder, and the project
page lists everyone with links.


License
-------

GPL-3.0, see LICENSE. Third-party components keep their own licences (THIRD_PARTY_NOTICES.txt, licenses folder).
Metroid Prime Remastered is copyright Nintendo and Retro Studios. It is not included in this package.
