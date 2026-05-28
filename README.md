<p align="center">
  <h1 align="center">axctl.c</h1>
  <p align="center">
    Universal IPC daemon and CLI for Wayland compositors.
    <br>
    C port of the original axctl (Go), built for lower resource usage and zero-overhead deployment.
  </p>
</p>

<p align="center">
  <a href="https://github.com/Leriart/axctl.c">
    <img src="https://img.shields.io/badge/axctl.c-0A0A0A?style=for-the-badge&logo=github&logoColor=FFFFFF" alt="repo">
  </a>
  <a href="https://github.com/Leriart/axctl">
    <img src="https://img.shields.io/badge/Original%20Go%20port-007D9C?style=for-the-badge&logo=go&logoColor=FFFFFF" alt="go version">
  </a>
  <a href="https://github.com/Leriart/NothingLess">
    <img src="https://img.shields.io/badge/Powered%20by%20NothingLess-0A0A0A?style=for-the-badge&logo=github&logoColor=FFFFFF&labelColor=E80012" alt="nothingless">
  </a>
</p>

---

## Installation

### Dependencies

```bash
# Debian / Ubuntu
sudo apt install build-essential libjson-c-dev libwayland-dev pkg-config

# Arch Linux
sudo pacman -S json-c wayland

# Fedora
sudo dnf install json-c-devel wayland-devel pkg-config
```

### Build and install

```bash
git clone https://github.com/Leriart/axctl.c.git
cd axctl.c
make
sudo make install
```

### Daemon setup

```bash
axctl daemon                          # Start the IPC daemon
axctl -c /path/to/config.toml daemon  # With custom config
```

The daemon listens on `/tmp/axctl-<uid>.sock` and auto-detects the compositor (Hyprland, Niri, Mango). Loads TOML configuration on startup and hot-reloads on file change via inotify.

```bash
axctl subscribe                       # Subscribe to real-time compositor events
```

---

## Features

- **Three compositor backends** — Hyprland (socket IPC + Lua), Niri (JSON-RPC), Mango (Wayland protocols) with unified vtable interface
- **60+ RPC methods** — Window, Workspace, Monitor, Layout, Config, and System operations
- **Config generators** — Compositor-specific `.conf` / `.lua` files from a single TOML source
- **Hot-reload** — Inotify-based file watcher, applies config changes without restarting
- **Idle management** — ext_idle_notify_v1, zwp_idle_inhibit_v1, and systemd-inhibit support
- **Event subscriptions** — Real-time state broadcasting to connected clients
- **Thread-safe cache** — Window, workspace, and monitor state with pthread_rwlock
- **TOML configuration** — Type-safe sections for appearance, input, keybinds, and window rules

---

## Commands

```bash
axctl daemon                          # Start the daemon
axctl subscribe                       # Stream events from the daemon
axctl window list                     # List all windows
axctl window active                   # Get active window ID
axctl window focus <id>               # Focus a window
axctl window focus-dir l|r|u|d        # Focus in direction
axctl window close [id]               # Close a window
axctl window move <dir> [id]          # Move window in direction
axctl window resize <w> <h> [id]      # Resize window
axctl window toggle-floating [id]     # Toggle floating state
axctl window fullscreen 0|1 [id]      # Set fullscreen
axctl window maximize 0|1 [id]        # Set maximized
axctl window pin 0|1 [id]             # Pin window
axctl window toggle-group [id]        # Toggle window group
axctl window group-nav f|b            # Navigate group tabs
axctl window layout-prop <k> <v> [id] # Set layout property
axctl window move-pixel <x> <y> [id]  # Move window by pixel offset
axctl workspace list                  # List all workspaces
axctl workspace active                # Get active workspace
axctl workspace switch <id>           # Switch to workspace
axctl workspace move-to <ws> [win]    # Move window to workspace
axctl workspace toggle-special [name] # Toggle special workspace
axctl monitor list                    # List all monitors
axctl monitor focus <id>              # Focus a monitor
axctl monitor move-to <mon> [win]     # Move window to monitor
axctl monitor set-dpms <id> 0|1       # Toggle DPMS
axctl layout set <name>               # Set compositor layout
axctl config get <key>                # Get a config value
axctl config set <key> <value>        # Set a config key
axctl config batch <json>             # Batch apply configurations
axctl config apply <json>             # Apply declarative universal config
axctl config reload                   # Reload config from disk
axctl config bind-key <mods> <k> <c>  # Bind a key
axctl config unbind-key <mods> <k>    # Unbind a key
axctl system execute <cmd>            # Execute a command
axctl system get-cursor-position      # Get cursor position
axctl system get-capabilities         # Get compositor capabilities
axctl system switch-keyboard-layout   # Switch keyboard layout
axctl system idle-inhibit 0|1         # Toggle idle inhibition
axctl system idle-wait <ms>           # Wait for idle state
axctl system is-idle <ms>             # Check idle status
axctl system exit                     # Exit the compositor
```

---

## Differences from Go axctl

| Area | Go version | C version |
|------|------------|-----------|
| Lines of code | ~19,400 | **~5,000** (3.9x smaller) |
| Binary size | ~6 MB | **~180 KB** (33x smaller) |
| RSS (daemon) | ~20-30 MB | **~3-5 MB** (6x less) |
| Threads (idle) | 8-12 (goroutines) | **3-5** (explicit pthread) |
| Startup time | ~50-100 ms (Go runtime) | **~1-2 ms** |
| Dependencies | None (self-contained) | libjson-c + libwayland-client |
| Compositor backends | Hyprland, Niri, Mango | Hyprland, Niri, **Mango** |
| TOML config | Basic parsing | Include resolution, typesafe sections |
| Hot-reload | Poll-based | **Inotify-based** (instant) |
| Idle management | systemd-inhibit | ext_idle_notify_v1, zwp_idle_inhibit_v1, systemd-inhibit |
| Memory model | Go GC | **Manual (pthread_rwlock cache)** |
| Config generation | Basic | **Multi-format (.conf + .lua)** |
| Event subscription | Poll-based | **Push-based** with real-time broadcast |

---

## Credits

- **Leriart** — C port author and axctl.c maintainer
- **Axenide** — original [Ambxst](https://github.com/Axenide/Ambxst) creator and axctl Go author
- **outfoxxed** — creator of [Quickshell](https://git.outfoxxed.me/outfoxxed/quickshell)

---

## License

Same as the original axctl project.
