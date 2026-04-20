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
  - CPU — total load with actual frequency (GHz), per-logical-core, per-physical-core, or load+temperature view (click to cycle)
  - GPU — core load, VRAM usage, temperature, or combined view (click to cycle); NVML support for NVIDIA RTX GPUs
  - RAM — used / total
  - Disk I/O — aggregate or per-disk view (click to cycle)
- **Per-process top lists** — CPU, GPU, RAM, and Disk top consumers; click a tile to toggle between percent and absolute values; hover freezes the list so you can read it; right-click an entry to kill the process
- **Weather panel** — current conditions and 3-day forecast fetched from [wttr.in](https://wttr.in), with ASCII-art icons; refreshed automatically every 10 minutes; click the panel to force an immediate refresh
- **RSS feed** — configurable RSS source (default: Habr), refreshed on a configurable interval; click a headline to open it in the browser
- **Acrylic / Aero blur** — uses the undocumented `SetWindowCompositionAttribute` API with DWM fallback, so it works across Windows 10 and 11; rounded corners via DWM
- **Resizable window** — drag any edge or corner to resize, in addition to dragging the internal panel dividers
- **Persistent layout** — window position, size, and divider positions are saved to `config.json` next to the executable
- **Configurable font scale** — set `font_scale` in `config.json` to scale all text
- **CPU temperature sources** — automatically tries HWiNFO64 shared memory, then OpenHardwareMonitor WMI, then LibreHardwareMonitor WMI, then MSAcpi thermal zone, then PDH; no manual configuration needed

## Requirements

- Windows 10 or Windows 11 (64-bit recommended)
- NVIDIA GPU with drivers that ship `nvml.dll` for GPU temperature (optional; falls back to PDH)
- [HWiNFO64](https://www.hwinfo.com/) with **Shared Memory Support** enabled for best CPU temperature coverage on AMD Ryzen (optional; other sources are tried automatically)

## Building

1. Open `BlurBox.sln` in **Visual Studio 2022** (v143 toolset).
2. Select the **Release | x64** configuration.
3. Build — no external dependencies beyond the Windows SDK.

The binary is written to `bin\x64\Release\BlurBox.exe`.

> **Debug log:** on every run BlurBox writes weather fetch diagnostics to `weather_debug.log` next to the executable. You can safely delete this file.

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
    "rss_feed_url": "https://habr.com/ru/rss/all/all/",
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
| `habr_refresh_minutes` | RSS feed refresh interval in minutes (default `5`) |
| `rss_feed_url` | RSS feed URL (default: Habr all articles); change to any valid RSS 2.0 feed |
| `autostart` | Launch with Windows (auto-saved) |

## Usage tips

- **Click** a chart row to cycle through its display modes.
- **Click** a process list tile to toggle between percent and absolute value display.
- **Click** the weather panel to force an immediate weather refresh.
- **Right-click** a process entry to kill that process.
- **Right-click** anywhere else on the widget to close it (same as Escape).
- **Drag** the vertical or horizontal dividers to resize panels.
- **Drag** any edge or corner of the window to resize it.
- **Escape** closes the widget.
- The widget lives in the **system tray** — right-click the tray icon to toggle autostart or exit.
- Hovering over a process list tile **freezes** its updates so the list stays readable.

## License

Distributed under the terms of the [LICENSE](LICENSE) file included in this repository.
