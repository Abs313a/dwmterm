#!/bin/sh
set -eu

# Standalone POSIX uninstaller for pre-built DWM-Terminal distribution.

DESTDIR="${DESTDIR:-}"

if [ -z "${PREFIX:-}" ]; then
	if [ "$(id -u)" -eq 0 ]; then
		PREFIX="/usr/local"
	else
		PREFIX="${HOME}/.local"
	fi
fi

BINDIR="${PREFIX}/bin"
DATADIR="${PREFIX}/share"
MANDIR="${DATADIR}/man/man1"
FONTDIR="${DATADIR}/fonts/TTF"
APPDIR="${DATADIR}/applications"
PIXDIR="${DATADIR}/pixmaps"
ICONDIR="${DATADIR}/icons/hicolor"

printf "Uninstalling DWM-Terminal from %s%s...\n" "${DESTDIR}" "${PREFIX}"

rm -f "${DESTDIR}${BINDIR}/dwmterm"
rm -f "${DESTDIR}${APPDIR}/dwmterm.desktop"
rm -f "${DESTDIR}${MANDIR}/dwmterm.1"
rm -f "${DESTDIR}${FONTDIR}/MesloLGSNerdFont-Regular.ttf"
rm -f "${DESTDIR}${DATADIR}/dwmterm/fonts/MesloLGSNerdFont-Regular.ttf"
rm -f "${DESTDIR}${DATADIR}/dwmterm/config.example"
rm -f "${DESTDIR}${DATADIR}/dwmterm/colors.example"
rm -f "${DESTDIR}${PIXDIR}/dwmterm.png"
for sz in 16x16 32x32 64x64 128x128 256x256 512x512; do
	rm -f "${DESTDIR}${ICONDIR}/${sz}/apps/dwmterm.png"
done

rmdir "${DESTDIR}${DATADIR}/dwmterm/fonts" 2>/dev/null || true
rmdir "${DESTDIR}${DATADIR}/dwmterm" 2>/dev/null || true

if [ -z "${DESTDIR}" ]; then
	if command -v fc-cache >/dev/null 2>&1; then
		fc-cache -f "${FONTDIR}" 2>/dev/null || true
	fi
	if command -v gtk-update-icon-cache >/dev/null 2>&1; then
		gtk-update-icon-cache -q "${ICONDIR}" 2>/dev/null || true
	fi
fi

printf "Successfully uninstalled dwmterm.\n"
