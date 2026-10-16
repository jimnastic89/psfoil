
# PSFoil

A PS5 homebrew payload for browsing Ownfoil/Tinfoil stores and managing Switch games stored on your console.

<img width="1550" height="696" alt="Main" src="https://github.com/user-attachments/assets/4832693c-4faa-4467-a3f0-df3a8fb908ab" />
<img width="1530" height="689" alt="Installed" src="https://github.com/user-attachments/assets/57dfc772-30e1-43aa-93b0-947e633c1b4a" />

## Features

- Save multiple Ownfoil API and Tinfoil JSON stores.
- On-console NSZ-to-NSP conversion, including bulk conversion of installed NSZ games.
- Reads your ProsperoEden configuration to automatically save downloaded games to the correct directories.
- Show games in a grid or detailed list view with artwork, version, file size, and available DLC.
- Download game, updates and DLC in one click, or select individual files.
- Upload NSP, NSZ and XCI files/folders from a browser.
- Progress and download history view.
- Notifications when games are ready to play.
- View and manage games installed in your ProsperoEden games path.
- Light/dark theme with configurable accent colours.

No game files, credentials or keys are included.

## Installation

1. Load `psfoil.elf` via an elf loader or Payload Manager.
2. Open PSFoil from the home-screen tile or visit `http://<PS5-IP>:8466` from a browser on the same LAN.
3. Add a store or open Installed games to manage/upload local files.

Use Settings to repair the home-screen tile if needed. Quit stops the payload; reload the ELF to restart it. Replacing the ELF preserves saved stores and preferences. Runtime behavior of new builds still requires PS5 testing.

## Configuration

PSFoil uses the path defined by `game_files` in `/data/prosperoeden/config/prosperoeden.json` for:

- Keys: `<game_files>/keys/prod.keys`.
- Default destinations: `<game_files>/roms` and `<game_files>/updates`.
- Shared working directory: `<game_files>/.psfoil`, including with a custom destination.
- Settings: `/data/psfoil/settings.json`, with backup, metadata and artwork caches in `/data/psfoil`.

Choose a custom destination and whether to split content into subfolders in Settings. Changes persist only after Save settings. The JSON viewer at the bottom shows saved values, not unsaved edits. Existing destination files are never overwritten. Local NSZ originals are removed only after a verified NSP is published. Keep enough space for both formats during conversion.

## Build

Requirements: [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk), Python 3 and PS5-target OpenSSL headers/archives.

```sh
make PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
```

Outputs: `build/psfoil.elf` and `build/manifest.json` with version, size and SHA-256. Web assets and icons are embedded.

Replace `assets/icon0.png` for the home-screen image. Optional web icon overrides are documented in `web/icons/README.md`.

The repository allowlist retains build inputs, this README and licenses. Build outputs, private configuration, local tests, investigation notes and original design files remain outside version control.

## License

Application source: GPL-3.0-or-later. Vendored dependencies retain their licenses: cpp-httplib (MIT), nlohmann/json (MIT), zlib (zlib license) and Zstandard (BSD-3-Clause). OpenSSL and ps5-payload-sdk are supplied by the build environment.
