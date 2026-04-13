# BlurBox

[![Release](https://img.shields.io/github/v/release/semenovi/aero-widget?style=flat-square)](https://github.com/semenovi/aero-widget/releases/latest)
[![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11-blue?style=flat-square&logo=windows)](https://github.com/semenovi/aero-widget/releases/latest)
[![License](https://img.shields.io/github/license/semenovi/aero-widget?style=flat-square)](LICENSE)
[![Language](https://img.shields.io/badge/language-C%2B%2B17-informational?style=flat-square&logo=c%2B%2B)](BlurBox.vcxproj)

A lightweight Windows desktop widget with an Acrylic/Aero blur background. Displays real-time system stats, weather, and a news feed — all rendered with Direct2D on a frosted-glass overlay that sits on your desktop.

![BlurBox screenshot](img/1.png)

---

## Features

- **System monitoring**
  - CPU — total load, per-logical-core, per-physical-core, or load+temperature view (click to cycle)
  - GPU — core load, VRAM usage, temperature, or combined view (click to cycle); NVML support for NVIDIA RTX GPUs
  - RAM — used / total
  - Disk I/O — aggregate or per-disk view (click to cycle)
- **Per-process top lists** — CPU, GPU, RAM, and Disk top consumers shown at a glance
- **Weather panel** — current conditions and 3-day forecast fetched from [wttr.in](https://wttr.in), with ASCII-art icons
- **Habr RSS feed** — latest tech articles, refreshed every 5 minutes; click a headline to open it in the browser
- **Acrylic / Aero blur** — uses the undocumented `SetWindowCompositionAttribute` API with DWM fallback, so it works across Windows 10 and 11
- **Draggable dividers** — resize panels at runtime by dragging the separators
- **Persistent layout** — window position, size, and divider positions are saved to `config.json` next to the executable
- **Configurable font scale** — set `font_scale` in `config.json` to scale all text

## Requirements

- Windows 10 or Windows 11 (64-bit recommended)
- NVIDIA GPU with drivers that ship `nvml.dll` for GPU temperature (optional; falls back to PDH)

## Building

1. Open `BlurBox.sln` in **Visual Studio 2022** (v143 toolset).
2. Select the **Release | x64** configuration.
3. Build — no external dependencies beyond the Windows SDK.

The binary is written to `bin\x64\Release\BlurBox.exe`.

## Configuration

On first launch a `config.json` file is created next to the executable. You can edit it manually:

```jsonc
{
    "location": "",
    "monitor_left": 0,
    "monitor_top": 0,
    "win_x": 100,
    "win_y": 100,
    "win_w": 800,
    "win_h": 600,
    "divider_x": 300.0,
    "divider_y": 200.0,
    "divider_x2": 500.0,
    "cpu_mode": 0,
    "gpu_mode": 0,
    "disk_mode": 0,
    "font_scale": 1.5,
    "habr_refresh_minutes": 5,
    "autostart": false
}
```

| Key | Description |
|-----|-------------|
| `location` | Weather location string passed to wttr.in (e.g. `"Moscow"`); empty = auto-detect |
| `monitor_left`, `monitor_top` | Monitor origin used to restore position on multi-monitor setups (auto-saved) |
| `win_x`, `win_y` | Window position relative to the monitor it was last on (auto-saved) |
| `win_w`, `win_h` | Window size in pixels (auto-saved) |
| `divider_x`, `divider_x2` | Positions of the two vertical dividers (auto-saved on drag-end) |
| `divider_y` | Position of the horizontal divider inside the left column (auto-saved on drag-end) |
| `cpu_mode` | CPU display mode: `0` total, `1` logical cores, `2` physical cores, `3` load+temp (auto-saved) |
| `gpu_mode` | GPU display mode: `0` core load, `1` VRAM, `2` temperature, `3` core+VRAM (auto-saved) |
| `disk_mode` | Disk display mode: `0` aggregate, `1`–`N` individual disk index (auto-saved) |
| `font_scale` | Global text scale factor (default `1.5`) |
| `habr_refresh_minutes` | Habr feed refresh interval in minutes (default `5`) |
| `autostart` | Launch with Windows (auto-saved) |

## Usage tips

- **Click** a chart row to cycle through its display modes.
- **Right-click** a process entry to kill that process.
- **Drag** the vertical or horizontal dividers to resize panels.
- The widget lives in the **system tray** — right-click the tray icon to exit.

## License

Distributed under the terms of the [LICENSE](LICENSE) file included in this repository.
