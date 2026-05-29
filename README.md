<p align="center">
  <img src="assets/logo.png" alt="axctl.c" width="420">
  <p align="center">
    Universal IPC daemon and CLI for Wayland compositors<br>
    <sub>C rewrite — <strong>33× smaller, 6× leaner, starts in 1 ms</strong></sub>
  </p>
</p>

<p align="center">
  <a href="https://github.com/Leriart/axctl.c"><img src="https://img.shields.io/badge/repo-0A0A0A?style=for-the-badge&logo=github&logoColor=fff"></a>
  <a href="https://github.com/Leriart/axctl"><img src="https://img.shields.io/badge/original_Go-007D9C?style=for-the-badge&logo=go&logoColor=fff"></a>
  <a href="https://github.com/Leriart/NothingLess"><img src="https://img.shields.io/badge/NothingLess-0A0A0A?style=for-the-badge&logo=github&logoColor=fff&labelColor=E80012"></a>
  <a href="./LICENSE"><img src="https://img.shields.io/badge/license-AGPLv3-0A0A0A?style=for-the-badge&labelColor=333"></a>
</p>

---

## Why axctl.c?

A line-by-line C port of [axctl](https://github.com/Axenide/axctl) (Go), rebuilt for **zero-overhead deployment** on embedded and low-resource systems.

| | Go | axctl.c |
|---|---|---|
| Binary | ~6 MB | **~180 KB** |
| RAM (daemon idle) | ~20-30 MB | **~3-5 MB** |
| Startup | ~50-100 ms | **~1 ms** |
| Threads (idle) | 8-12 | **3-5** |
| Dependencies | none (static) | libjson-c, libwayland-client |
| Hot-reload | poll | **inotify (instant)** |
| Config output | `.conf` | `.conf` **+ `.lua`** |
| Keybinds | ambxst JSON | **ambxst + TOML** (hot-reload) |

---

## Install

```bash
# Arch
sudo pacman -S json-c wayland
# Debian/Ubuntu
sudo apt install libjson-c-dev libwayland-dev pkg-config
# Fedora
sudo dnf install json-c-devel wayland-devel pkg-config

git clone https://github.com/Leriart/axctl.c.git
cd axctl.c && make && sudo make install
```

---

## Usage

```bash
axctl daemon                  # auto-detects Hyprland / Niri / Mango
axctl subscribe               # real-time event stream

axctl window focus-dir l      # focus left
axctl workspace switch 3      # jump to workspace 3
axctl config apply "$json"    # push config from Ambxst / NothingLess
```

The daemon listens on `/tmp/axctl-<uid>.sock`. TOML config at `~/.config/axctl/config.toml` is loaded on start and **hot-reloaded on save** via inotify — no restart needed.

---

## What it does

- **3 compositor backends** — Hyprland (socket + Lua dispatch), Niri (JSON-RPC), Mango (Wayland)
- **60+ RPC methods** — windows, workspaces, monitors, layout, keybinds, idle, system
- **Config generation** — single TOML source → `.conf` + `.lua` files per compositor
- **Idle protocols** — `ext_idle_notify_v1`, `zwp_idle_inhibit_v1`, `systemd-inhibit`
- **Thread-safe state cache** — `pthread_rwlock`, push-based event broadcast to subscribers
- **Keybind pipeline** — parses Ambxst JSON + TOML keybinds, generates compositor-native bind syntax with full modifier, flag, and Lua support

---

## Credits

**Leriart** — C port · [**Axenide**](https://github.com/Axenide) — original Ambxst & axctl · [**outfoxxed**](https://git.outfoxxed.me/outfoxxed/quickshell) — Quickshell

---

axctl.c — AGPLv3 · See [LICENSE](./LICENSE)
