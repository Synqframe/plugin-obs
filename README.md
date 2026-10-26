# Synqframe for OBS

The OBS Studio plugin for [Synqframe](https://synqfra.me). It adds a **Synqframe Bundle**
filter that marks what a source sends to a show, keeps OBS's scenes in step with layouts
edited on the web, and publishes a creator's feeds when they press Start Streaming. Sign-in,
show selection and Go Live are in the Synqframe dock, a web page hosted in an OBS browser dock.

The plugin builds against stock OBS Studio. It uses only `libobs`, `obs-frontend-api` and Qt,
and never patches OBS or reaches into its private widgets. [plugin/README.md](plugin/README.md)
describes its design and the rules it follows.

## Requirements

- OBS Studio 32.2 or later, 64-bit Windows. Windows is the only platform built and tested today.
- obs-browser (included with OBS) for the Synqframe dock. Without it, the dock offers to open
  the page in your own browser.
- [Aitum Vertical](https://github.com/Aitum/obs-vertical-canvas), optional, for a vertical
  program.

## Building on Windows

You need Visual Studio 2026 with C++, CMake 3.28 or later, and an OBS Studio 32.2.1 source
tree that has been configured with its `windows-x64` preset, so that its `.deps` are present.
Clone with submodules, since `vendor/synqframe-protocol` is one:

```powershell
git clone --recursive https://github.com/Synqframe/plugin-obs.git
cd plugin-obs
.\scripts\build-windows.ps1 -ObsSdkRoot C:\path\to\obs-studio -Test
```

This builds `libobs` and `obs-frontend-api` in the OBS tree when they're missing, then the
plugin, then runs the unit tests. The plugin lands at
`build_x64\plugin\RelWithDebInfo\synqframe.dll`. `-InstallPlugin` also copies it into a
portable OBS (`-ObsRuntimeRoot`, by default `.runtime\obs-portable`).

## Layout

| Path | What it is |
|---|---|
| `plugin/` | The plugin |
| `plugin/core/` | Its frontend-neutral parts: the program publisher, capture-time stamping, speech detection |
| `vendor/synqframe-protocol/` | The wire formats and clients the plugin shares with the Synqframe service ([Synqframe/protocol](https://github.com/Synqframe/protocol), MIT; a submodule) |
| `vendor/libfvad/` | WebRTC's voice activity detector as plain C (BSD) |
| `scripts/` | The Windows build, and checks for the layout socket and audio clock |

## License

The plugin is licensed under the GNU General Public License, version 3 or later
([LICENSE](LICENSE)). Copyright (C) 2026 Synqframe.

Some parts carry their own licences:
- `vendor/synqframe-protocol` is MIT ([its LICENSE](vendor/synqframe-protocol/LICENSE)).
- `vendor/libfvad` is BSD-3-Clause ([its LICENSE](vendor/libfvad/LICENSE)).
- `plugin/obs-browser-panel.hpp` is obs-browser's panel interface, GPL-2.0-or-later.
- Parts of the GoLive and multitrack code follow OBS Studio's own (GPL-2.0-or-later); those
  files say where.
