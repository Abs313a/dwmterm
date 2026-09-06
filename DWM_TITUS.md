# DWM-Titus Integration Guide: DWM Terminal (`dwmterm`)

This guide details how to integrate `dwmterm` as the native, drop-in default terminal emulator for [ChrisTitusTech/dwm-titus](https://github.com/ChrisTitusTech/dwm-titus).

---

## 1. Why `dwmterm` for `dwm-titus`?

* **Resource Footprint:** ~12 MB active RSS (<15 MB target), significantly lighter than Alacritty, Kitty, and WezTerm.
* **MIT-SHM Shared Memory Blitting:** Zero-copy X11 shared memory rendering (`XShmPutImage`) with automatic socket fallback.
* **Embedded Top-Bar & Quickshell Icon (`_NET_WM_ICON`):** Built-in 16×16 and 32×32 32-bit ARGB Nord-themed icons embedded via `XA_CARDINAL`. Renders immediately in DWM's winicon patch (`SHOWWINICON 1`) and Quickshell taskbar without external `.png` or icon theme lookups.
* **Full Alternate Screen Isolation (`DECSET 1049 / 1047 / 47`):** Seamless grid swapping for full-screen TUIs (`vim`, `htop`, `tmux`, `herdr`) preventing scrollback history pollution.
* **CLI Contract:** Standard `-e <cmd> [args...]`, `-T <title>`, `-t <title>`, `-d <dir>`, `--working-directory <dir>`, `-v`, and `-h` flags.
* **Dynamic Theming Integration:** Reads `${XDG_CONFIG_HOME:-~/.config}/dwmterm/colors` and live-reloads palette on `SIGUSR1`, plugging directly into `dwm-titus`'s `themes.toml` and `theme-apply.sh`.

---

## 2. Build Dependencies

### Fedora
```bash
sudo dnf install gcc make pkgconf libX11-devel libXext-devel freetype-devel fontconfig-devel
```

### Arch Linux / CachyOS
```bash
sudo pacman -S gcc make pkgconf libx11 libxext freetype2 fontconfig
```

### Debian / Ubuntu / Pop!_OS
```bash
sudo apt install build-essential pkg-config libx11-dev libxext-dev libfreetype6-dev libfontconfig1-dev
```

---

## 3. Installation

Compile and install system-wide:
```bash
make clean && make
sudo make install
```

This installs:
* `/usr/local/bin/dwmterm` (executable)
* `/usr/local/share/applications/dwmterm.desktop` (freedesktop launcher)
* `/usr/local/share/man/man1/dwmterm.1` (man page)
* `/usr/local/share/fonts/TTF/MesloLGSNerdFont-Regular.ttf` (system font via `fc-cache`)
* `/usr/local/share/dwmterm/fonts/MesloLGSNerdFont-Regular.ttf` (fallback font copy)

---

## 4. `dwm-titus` Upstream Integration Steps

In the `dwm-titus` repository, apply the following updates:

### Step A: Window Swallowing Rule in `config/window-rules.toml`

`dwm-titus` parses window rules at runtime via `window-rules.toml` (watched live via `inotify`, zero recompilation required).

Add `Dwmterm` to the terminal rules section in `config/window-rules.toml`:
```toml
rules = [
  # ── terminals (isterminal=1 enables window swallowing) ───────────────────
  { class="Dwmterm",        isterminal=1 },
  { class="St",             isterminal=1 },
  { class="kitty",          isterminal=1 },
  { class="Alacritty",      isterminal=1 },
  { class="warp-terminal",  isterminal=1 },
  { class="Terminator",     isterminal=1 },
```

* `class = "Dwmterm"`: Matches `dwmterm`'s `WM_CLASS` (`res_class = "Dwmterm"`).
* `isterminal = 1`: Marks the window as eligible for window swallowing. When launching graphical applications (e.g. `sxiv`, `mpv`) from `dwmterm`, the terminal window is swallowed until the child exits.

### Step B: Add `dwmterm` to `scripts/dwm-terminal`

Edit `scripts/dwm-terminal` and add `dwmterm` as the first entry:
```bash
DEFAULT_TERMINALS=(
	dwmterm
	alacritty
	kitty
	st
	warp-terminal
	xterm
)
```

`scripts/dwm-terminal` integrates directly with `dwmterm`:
* When Herdr workspace integration is enabled, `dwm-terminal` executes:
  ```bash
  exec "$terminal" -e "$herdr"
  ```
  `dwmterm -e <cmd> [args...]` handles this delegation via `execvp()`.
* When called with custom commands or maintenance scripts (`dwm-terminal -e <command>`), arguments are forwarded directly.

### Step C: Register in Quickshell Default Applications (`scripts/dwm-default-apps`)

To enable selecting DWM Terminal in Quickshell's Settings UI (Default Applications), edit `scripts/dwm-default-apps`:

1. Map `dwmterm.desktop` to `dwmterm`:
```bash
terminal_command_for_id() {
	case $1 in
	dwmterm.desktop) printf '%s\n' dwmterm ;;
	Alacritty.desktop | org.alacritty.Alacritty.desktop) printf '%s\n' alacritty ;;
```

2. Add `dwmterm.desktop` to the terminal candidate scan list:
```bash
terminal_id_for_command() {
	local command_name=$1
	local id command_candidate file
	for id in dwmterm.desktop Alacritty.desktop org.alacritty.Alacritty.desktop kitty.desktop st.desktop \
		dev.warp.Warp.desktop warp-terminal.desktop xterm.desktop; do
```

### Step D: Update Dependency & Diagnostic Checkers

Ensure system health and dependency checks recognize `dwmterm`:

1. In `scripts/check-deps.sh`:
```bash
# ── Terminal emulators ──────────────────────────────────
echo "Terminal Emulators (at least one required):"
TERM_FOUND=0
for term in dwmterm alacritty kitty st; do
```

2. In `scripts/dwm-diagnostics`:
```bash
for cmd in dwmterm alacritty kitty st warp-terminal xterm; do
```

3. In `scripts/dwm-utils.sh`:
```bash
detect_terminal() {
	for t in dwmterm alacritty kitty st warp-terminal xterm; do
```

### Step E: Configure Default Hotkeys (`config/hotkeys.toml`)

To make `dwmterm` the primary terminal spawned by `Super + X`, set in `${XDG_CONFIG_HOME:-$HOME/.config}/dwm-titus/hotkeys.toml`:
```toml
[vars]
terminal = "dwmterm"
```

### Step F: Thunar "Open Terminal Here" Action (`config/Thunar/uca.xml`)

Update Thunar's right-click terminal action to use `dwmterm`'s `--working-directory` flag:
```xml
<action>
    <icon>utilities-terminal</icon>
    <name>Open Terminal Here</name>
    <unique-id>terminal-here</unique-id>
    <command>dwmterm --working-directory %f</command>
    <description>Open DWM Terminal in this directory</description>
    <patterns>*</patterns>
    <directories/>
</action>
```

### Step G: Quickshell Dynamic Palette Theming (`scripts/theme-apply.sh`)

To have `dwmterm` automatically adopt the active theme selected in `themes.toml` or Quickshell Settings, add the following block to `scripts/theme-apply.sh`:

```bash
# ══════════════════════════════════════════════════════════════════════════════
# DWMTERM — write ~/.config/dwmterm/colors
# ══════════════════════════════════════════════════════════════════════════════
DWMTERM_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/dwmterm"
if [[ $RUNTIME_ONLY == 0 && $LIVE_ONLY == 0 ]]; then
	mkdir -p "$DWMTERM_DIR"
	cat >"$DWMTERM_DIR/colors" <<EOF
# Auto-generated by theme-apply.sh — do not edit manually.
# Change the theme in ~/.config/dwm-titus/themes.toml instead.

background  = $TERM_BG
foreground  = $TERM_FG
cursor      = $TERM_CURSOR

color0  = $TERM_C0
color1  = $TERM_C1
color2  = $TERM_C2
color3  = $TERM_C3
color4  = $TERM_C4
color5  = $TERM_C5
color6  = $TERM_C6
color7  = $TERM_C7
color8  = $TERM_C8
color9  = $TERM_C9
color10 = $TERM_C10
color11 = $TERM_C11
color12 = $TERM_C12
color13 = $TERM_C13
color14 = $TERM_C14
color15 = $TERM_C15
EOF
fi

# Signal running dwmterm instances to reload colors live
if [[ $STAGED_OUTPUT == 0 ]] && command -v dwmterm &>/dev/null; then
	while IFS= read -r dwmterm_pid; do
		[[ $dwmterm_pid =~ ^[1-9][0-9]*$ ]] || continue
		kill -SIGUSR1 "$dwmterm_pid" 2>/dev/null || true
	done < <(pgrep -x dwmterm 2>/dev/null || true)
fi
```

---

## 5. Verification & Testing

### 1. Verify Window Properties & Swallowing
Launch `dwmterm` and inspect properties:
```bash
xprop -id $(xdotool getactivewindow) WM_CLASS _NET_WM_ICON _NET_WM_NAME _NET_WM_PID
```
Expected output:
* `WM_CLASS(STRING) = "dwmterm", "Dwmterm"`
* `_NET_WM_NAME(UTF8_STRING) = "DWM Terminal"`
* `_NET_WM_ICON(CARDINAL)`: Displays both 16×16 and 32×32 icons.

### 2. Verify Working Directory Spawning
```bash
dwmterm -d /tmp -e pwd
dwmterm --working-directory=/var/log -e pwd
```

### 3. Verify Live Dynamic Theme Reloading
Write a test color file and signal the terminal:
```bash
mkdir -p ~/.config/dwmterm
cat << 'EOF' > ~/.config/dwmterm/colors
background = #282A36
foreground = #F8F8F2
cursor = #BD93F9
EOF

kill -SIGUSR1 $(pgrep -x dwmterm)
```
The terminal window immediately adopts the new background, foreground, and cursor colors without restarting.

### 4. Verify MIT-SHM Shared Memory Rendering
Run:
```bash
ipcs -m -p | grep $(pgrep -n dwmterm)
```
Confirm the presence of an active shared memory segment attached to both the client process and the X server.

### 5. Verify Alternate Screen Buffer Isolation
Launch full-screen TUIs inside `dwmterm`:
```bash
dwmterm -e htop
# or
dwmterm -e vim
```
Exit the application and verify:
* The previous shell prompt and primary screen contents are cleanly restored.
* Scrollback (`Shift+PageUp`) does not contain TUI screen artifacts.
