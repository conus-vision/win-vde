# win-vde — Virtual Desktop Extension for Windows 11

Tray utility that remembers which **virtual desktop** each browser window
belongs to and restores that layout automatically after a reboot or a browser
restart. Firefox is supported today; Chrome/Edge and other multi-window apps
are on the roadmap.

- Author: **Volodymyr Moskvin** — info@conus.vision
- Repo: https://github.com/conus-vision/win-vde
- License: MIT

## Build

Requires Visual Studio 2022 or 2017 (x64 tools). From a normal shell in the
repo root:

    build.bat

This produces `build\vde.exe`. Run `vde.exe` for the tray app, or
`vde.exe list | status | save | restore` for the command line.

> Full documentation lands with Phase 1 completion.
