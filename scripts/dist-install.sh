#!/bin/sh
set -eu

# Standalone POSIX installer for pre-built DWM-Terminal distribution.
# Does not require gcc, make, or git.

DESTDIR="${DESTDIR:-}"

if [ -z "${PREFIX:-}" ]; then
	if [ "$(id -u)" -eq 0 ]; then
		PREFIX="/usr/local"
	else
		PREFIX="${HOME}/.local"
	fi
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
if [ -f "${SCRIPT_DIR}/dwmterm" ]; then
	SRC_DIR="${SCRIPT_DIR}"
elif [ -f "${SCRIPT_DIR}/../dwmterm" ]; then
	SRC_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
else
	SRC_DIR="$(pwd)"
fi

BINDIR="${PREFIX}/bin"
DATADIR="${PREFIX}/share"
MANDIR="${DATADIR}/man/man1"
FONTDIR="${DATADIR}/fonts/TTF"
APPDIR="${DATADIR}/applications"
PIXDIR="${DATADIR}/pixmaps"
ICONDIR="${DATADIR}/icons/hicolor"

printf "Installing DWM-Terminal to %s%s...\n" "${DESTDIR}" "${PREFIX}"

# 1. Binary
install -d "${DESTDIR}${BINDIR}"
install -m 755 "${SRC_DIR}/dwmterm" "${DESTDIR}${BINDIR}/dwmterm"

# 2. Desktop Launcher
install -d "${DESTDIR}${APPDIR}"
install -m 644 "${SRC_DIR}/dwmterm.desktop" "${DESTDIR}${APPDIR}/dwmterm.desktop"

# 3. Man Page
install -d "${DESTDIR}${MANDIR}"
install -m 644 "${SRC_DIR}/dwmterm.1" "${DESTDIR}${MANDIR}/dwmterm.1"

# 4. Fonts
install -d "${DESTDIR}${FONTDIR}"
install -d "${DESTDIR}${DATADIR}/dwmterm/fonts"
install -m 644 "${SRC_DIR}/fonts/MesloLGSNerdFont-Regular.ttf" "${DESTDIR}${FONTDIR}/MesloLGSNerdFont-Regular.ttf"
install -m 644 "${SRC_DIR}/fonts/MesloLGSNerdFont-Regular.ttf" "${DESTDIR}${DATADIR}/dwmterm/fonts/MesloLGSNerdFont-Regular.ttf"

# 5. Icons
install -d "${DESTDIR}${PIXDIR}"
install -m 644 "${SRC_DIR}/icons/dwmterm.png" "${DESTDIR}${PIXDIR}/dwmterm.png"
for sz in 16x16 32x32 64x64 128x128 256x256 512x512; do
	install -d "${DESTDIR}${ICONDIR}/${sz}/apps"
	install -m 644 "${SRC_DIR}/icons/${sz}/dwmterm.png" "${DESTDIR}${ICONDIR}/${sz}/apps/dwmterm.png"
done

# 6. Configuration Templates & User Config
install -d "${DESTDIR}${DATADIR}/dwmterm"
if [ -f "${SRC_DIR}/config.example" ]; then
	install -m 644 "${SRC_DIR}/config.example" "${DESTDIR}${DATADIR}/dwmterm/config.example"
fi
if [ -f "${SRC_DIR}/colors.example" ]; then
	install -m 644 "${SRC_DIR}/colors.example" "${DESTDIR}${DATADIR}/dwmterm/colors.example"
fi

if [ -z "${DESTDIR}" ] && [ -n "${HOME:-}" ]; then
	USER_CFG_DIR="${XDG_CONFIG_HOME:-${HOME}/.config}/dwmterm"
	install -d "${USER_CFG_DIR}"
	if [ -f "${SRC_DIR}/colors.example" ]; then
		install -m 644 "${SRC_DIR}/colors.example" "${USER_CFG_DIR}/colors.example"
	fi
	if [ -f "${SRC_DIR}/config.example" ] && [ ! -f "${USER_CFG_DIR}/config" ]; then
		install -m 644 "${SRC_DIR}/config.example" "${USER_CFG_DIR}/config"
		printf "Created default configuration: %s/config\n" "${USER_CFG_DIR}"
	fi
fi

# 7. Update Caches (if not staging into DESTDIR)
if [ -z "${DESTDIR}" ]; then
	if command -v fc-cache >/dev/null 2>&1; then
		printf "Refreshing font cache...\n"
		fc-cache -f "${FONTDIR}" 2>/dev/null || true
	fi
	if command -v gtk-update-icon-cache >/dev/null 2>&1; then
		printf "Refreshing icon cache...\n"
		gtk-update-icon-cache -q "${ICONDIR}" 2>/dev/null || true
	fi
fi

printf "Successfully installed dwmterm into %s%s/bin/dwmterm\n" "${DESTDIR}" "${PREFIX}"
