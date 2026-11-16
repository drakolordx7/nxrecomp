Hyrule Warriors: Age of Calamity PC (unofficial port), TEST BUILD 0.1.0
=======================================================================

An early test build of a native Windows and Linux port of Hyrule Warriors: Age of Calamity, made by recompiling
the Nintendo Switch release (version 1.4.1) into a normal PC program. It is not an emulator. It has a launcher,
a setup tool, keyboard and mouse controls, any resolution and aspect ratio including ultrawide, and a frame rate
above 30.

This is a test build. The game starts and plays the opening battle, but almost nothing past it has been looked at.
It is released so people can try it on their own PCs and report what breaks. HELP.txt lists the known problems.

Release page and issues: https://github.com/drakolordx7/nxrecomp/releases/tag/hwaoc-v0.1.0

No game files are included. You need your own copy of the game, its 1.4.1 update and the keys from your own
Switch. Not affiliated with Nintendo or Koei Tecmo.


Requirements
------------

- Windows 10 (1903 or later) or Windows 11, 64-bit; or 64-bit Linux (glibc 2.35 or newer, X11 or Wayland)
- A GPU and driver with Vulkan 1.3. Tested on one PC with an NVIDIA GeForce RTX 3080 only. On Linux it has only
  been run with software rendering (lavapipe), not on a real GPU
- About 15 GB of free disk space and 8 GB of free memory
- Your dump of the game (.nsp, .xci, .nsz or .xcz) AND its 1.4.1 update (an .nsp). Any other game version is
  refused. DLC files (.nsp) are optional
- Your prod.keys. If the dump was bought on another console, also put that console's title.keys next to prod.keys


Usage
-----

1. Unpack the release into a folder with a short path (under about 180 characters, for example D:\Games\HWAOC)
   and run hwaoc_launcher.exe (Linux: ./hwaoc_launcher).
2. Press "Set up game files...", choose the base game, the 1.4.1 update and any DLC together (pick or drag all
   of them), then your prod.keys, and press Start setup. It takes a few minutes. You can stop it and run it again
   later; it carries on where it stopped.
3. When setup closes, press Play. The first start can sit for a while with nothing on screen. Give it up to two
   minutes.

On the first start the game asks for a difficulty and the text and voice language, then plays the opening movies.
The game's title screen only comes after the first battle.

Settings are in the launcher (display, mouse, keyboard, controller, button icons) and on the F1 page in the game.
They are saved in controls.ini in the game folder. Saves are in %APPDATA%\HWAOCRecomp\save (Linux:
~/.local/share/HWAOCRecomp/save). The logs (hwaoc.log and setup.log) are in the game folder. To update, unpack a
new release over the old folder. To uninstall, delete the folder, and the save folder if you want your saves gone.

Controls: WASD move, the mouse turns the camera, the left button attacks, the right button is the strong attack,
Space jumps, Left Shift dodges, Q or the middle button locks on, E is the character's unique action, F is the
special attack, Left Ctrl or the side button is the Sheikah Slate, Escape pauses and Tab opens the map. Xbox,
PlayStation and Switch controllers work. HELP.txt lists every key and setting.


Reporting problems
------------------

Open an issue at https://github.com/drakolordx7/nxrecomp/issues and attach
hwaoc.log from the run where it happened (the next run keeps the old one as hwaoc.log.prev). Say what you did,
what you expected, your GPU, driver and operating system, and attach a screenshot for anything visual. The
version is in the top right corner of the launcher and on the "hwaoc 0.1.0" line near the top of hwaoc.log. For a
crash, also attach hwaoc_crash.txt, hwaoc_fatal.txt or hwaoc_freeze.txt if they exist. Never attach game files, your
dump or your keys.


Credits
-------

Maintained by drakolordx7 (https://github.com/drakolordx7).

Built on the work of others: smo-recomp, SuperMarioWonderRecomp, ssbu-recomp, ScarletNX and LASRecomp (recompiler
and runtime design), Ryujinx (shader translator and audio renderer), Eden and yuzu (service layouts and a
reference to check against), SDL3, Dear ImGui, volk, Vulkan Memory Allocator, Vulkan-Headers, astc-encoder,
bc7enc_rdo, zstd, FFmpeg (movies), DejaVu Sans and the Kenney Input Prompts icons. Links and licences are in
THIRD_PARTY_NOTICES.txt and the licenses folder, and the project page lists everyone with links.


License
-------

GPL-3.0, see LICENSE. Third-party components keep their own licences (THIRD_PARTY_NOTICES.txt, licenses folder).
Hyrule Warriors: Age of Calamity is copyright Nintendo and Koei Tecmo. It is not included in this package.
