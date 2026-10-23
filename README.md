# mGBA Switch

A Nintendo Switch homebrew version of mGBA with a game library, cover art, and controller-friendly menus. Plays Game Boy, Game Boy Color, and Game Boy Advance games.

- Shelf and console library layouts, with SteamGridDB artwork search.
- Softcore RetroAchievements, with account settings and achievement progress.
- Background downloads and update checks. Updates ask before installing and restarting.

Download `mgba.nro` from this fork's Releases and place it in your SD card's `switch` folder. Launch it through the homebrew menu. Games, BIOS files, and service accounts are not included.

This fork changes the Switch frontend. The emulator core and upstream history are preserved. See [upstream mGBA](https://github.com/mgba-emu/mgba) for emulator documentation and other platforms.

Switch releases use tags such as `switch-v1.0.0` and include `mgba.nro` and `update.json`. The manifest contains `version`, `url`, `sha256`, and `size`. Build the matching version with `SWITCH_UPDATE_VERSION` before publishing a release.

Copyright
---------

mGBA is Copyright © 2013 – 2026 Jeffrey Pfau. It is distributed under the [Mozilla Public License version 2.0](https://www.mozilla.org/MPL/2.0/). A copy of the license is available in the distributed LICENSE file.

mGBA contains the following third-party libraries:

- [inih](https://github.com/benhoyt/inih), which is copyright © 2009 – 2020 Ben Hoyt and used under a BSD 3-clause license.
- [LZMA SDK](http://www.7-zip.org/sdk.html), which is public domain.
- [MurmurHash3](https://github.com/aappleby/smhasher) implementation by Austin Appleby, which is public domain.
- [getopt for MSVC](https://github.com/skandhurkat/Getopt-for-Visual-Studio/), which is public domain.
- [SQLite3](https://www.sqlite.org), which is public domain.

If you are a game publisher and wish to license mGBA for commercial usage, please email [licensing@mgba.io](mailto:licensing@mgba.io) for more information.
