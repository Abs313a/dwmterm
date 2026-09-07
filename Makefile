CC ?= gcc
VERSION ?= 0.1
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share
MANDIR ?= $(DATADIR)/man

FONTDIR ?= $(DATADIR)/fonts/TTF

BIN = dwmterm
PKG_NAME = dwmterm

PKG_CFLAGS = $(shell pkg-config --cflags freetype2 fontconfig)
PKG_LIBS = $(shell pkg-config --libs freetype2 fontconfig) -lX11 -lXext -lutil

CFLAGS ?= -O2 -Wall -Wextra -pedantic
CPPFLAGS ?=
LDFLAGS ?=

all: $(BIN)

$(BIN): main.c
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -DDATADIR=\"$(DATADIR)\" -DVERSION=\"$(VERSION)\" $(CPPFLAGS) $(LDFLAGS) main.c $(PKG_LIBS) -o $(BIN)

install: $(BIN)
	install -d $(DESTDIR)$(BINDIR)
	install -d $(DESTDIR)$(DATADIR)/applications
	install -d $(DESTDIR)$(MANDIR)/man1
	install -d $(DESTDIR)$(FONTDIR)
	install -d $(DESTDIR)$(DATADIR)/$(PKG_NAME)
	install -d $(DESTDIR)$(DATADIR)/$(PKG_NAME)/fonts
	install -m 755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
	install -m 644 dwmterm.desktop $(DESTDIR)$(DATADIR)/applications/dwmterm.desktop
	install -m 644 dwmterm.1 $(DESTDIR)$(MANDIR)/man1/dwmterm.1
	install -m 644 fonts/MesloLGSNerdFont-Regular.ttf $(DESTDIR)$(DATADIR)/$(PKG_NAME)/fonts/MesloLGSNerdFont-Regular.ttf
	install -m 644 fonts/MesloLGSNerdFont-Regular.ttf $(DESTDIR)$(FONTDIR)/MesloLGSNerdFont-Regular.ttf
	install -m 644 config.example $(DESTDIR)$(DATADIR)/$(PKG_NAME)/config.example
	install -m 644 colors.example $(DESTDIR)$(DATADIR)/$(PKG_NAME)/colors.example
	install -d $(DESTDIR)$(DATADIR)/pixmaps
	install -m 644 icons/dwmterm.png $(DESTDIR)$(DATADIR)/pixmaps/dwmterm.png
	@for sz in 16x16 32x32 64x64 128x128 256x256 512x512; do \
		install -d $(DESTDIR)$(DATADIR)/icons/hicolor/$$sz/apps; \
		install -m 644 icons/$$sz/dwmterm.png $(DESTDIR)$(DATADIR)/icons/hicolor/$$sz/apps/dwmterm.png; \
	done
	@if [ -z "$(DESTDIR)" ] && command -v fc-cache >/dev/null 2>&1; then \
		echo "Updating font cache..."; \
		fc-cache -f $(FONTDIR) 2>/dev/null || true; \
	fi
	@if [ -z "$(DESTDIR)" ] && command -v gtk-update-icon-cache >/dev/null 2>&1; then \
		echo "Updating icon cache..."; \
		gtk-update-icon-cache -q $(DATADIR)/icons/hicolor 2>/dev/null || true; \
	fi

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)
	rm -f $(DESTDIR)$(DATADIR)/applications/dwmterm.desktop
	rm -f $(DESTDIR)$(MANDIR)/man1/dwmterm.1
	rm -f $(DESTDIR)$(DATADIR)/$(PKG_NAME)/fonts/MesloLGSNerdFont-Regular.ttf
	rm -f $(DESTDIR)$(FONTDIR)/MesloLGSNerdFont-Regular.ttf
	rm -f $(DESTDIR)$(DATADIR)/$(PKG_NAME)/config.example
	rm -f $(DESTDIR)$(DATADIR)/$(PKG_NAME)/colors.example
	rm -f $(DESTDIR)$(DATADIR)/pixmaps/dwmterm.png
	@for sz in 16x16 32x32 64x64 128x128 256x256 512x512; do \
		rm -f $(DESTDIR)$(DATADIR)/icons/hicolor/$$sz/apps/dwmterm.png; \
	done
	-rmdir $(DESTDIR)$(FONTDIR) 2>/dev/null
	-rmdir $(DESTDIR)$(DATADIR)/$(PKG_NAME)/fonts 2>/dev/null
	-rmdir $(DESTDIR)$(DATADIR)/$(PKG_NAME) 2>/dev/null
	@if [ -z "$(DESTDIR)" ] && command -v fc-cache >/dev/null 2>&1; then \
		echo "Updating font cache..."; \
		fc-cache -f $(FONTDIR) 2>/dev/null || true; \
	fi
	@if [ -z "$(DESTDIR)" ] && command -v gtk-update-icon-cache >/dev/null 2>&1; then \
		echo "Updating icon cache..."; \
		gtk-update-icon-cache -q $(DATADIR)/icons/hicolor 2>/dev/null || true; \
	fi

test: test_terminal.c main.c
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -DDATADIR=\"$(DATADIR)\" -DVERSION=\"$(VERSION)\" $(CPPFLAGS) $(LDFLAGS) test_terminal.c $(PKG_LIBS) -o test_terminal
	./test_terminal
	@rm -f test_terminal

clean:
	rm -f $(BIN) miniterm test_terminal *.o
	rm -rf dist dwmterm-v*

dist:
	$(MAKE) clean
	mkdir -p dist/dwmterm-v$(VERSION)-linux-x86_64/fonts
	mkdir -p dist/dwmterm-v$(VERSION)-linux-x86_64/icons
	$(MAKE) $(BIN) CFLAGS="-O3 -Wall -Wextra -pedantic" LDFLAGS="-s"
	cp $(BIN) dist/dwmterm-v$(VERSION)-linux-x86_64/
	cp dwmterm.desktop dist/dwmterm-v$(VERSION)-linux-x86_64/
	cp dwmterm.1 dist/dwmterm-v$(VERSION)-linux-x86_64/
	cp fonts/MesloLGSNerdFont-Regular.ttf dist/dwmterm-v$(VERSION)-linux-x86_64/fonts/
	cp -r icons/* dist/dwmterm-v$(VERSION)-linux-x86_64/icons/
	cp LICENSE dist/dwmterm-v$(VERSION)-linux-x86_64/
	cp README.md dist/dwmterm-v$(VERSION)-linux-x86_64/
	cp config.example dist/dwmterm-v$(VERSION)-linux-x86_64/
	cp colors.example dist/dwmterm-v$(VERSION)-linux-x86_64/
	cp scripts/dist-install.sh dist/dwmterm-v$(VERSION)-linux-x86_64/install.sh
	cp scripts/dist-uninstall.sh dist/dwmterm-v$(VERSION)-linux-x86_64/uninstall.sh
	chmod 755 dist/dwmterm-v$(VERSION)-linux-x86_64/$(BIN)
	chmod 755 dist/dwmterm-v$(VERSION)-linux-x86_64/install.sh
	chmod 755 dist/dwmterm-v$(VERSION)-linux-x86_64/uninstall.sh
	cd dist && tar -czvf dwmterm-v$(VERSION)-linux-x86_64.tar.gz dwmterm-v$(VERSION)-linux-x86_64
	cd dist && sha256sum dwmterm-v$(VERSION)-linux-x86_64.tar.gz > dwmterm-v$(VERSION)-linux-x86_64.tar.gz.sha256
	rm -rf dist/dwmterm-v$(VERSION)-linux-x86_64
	@printf "Distribution package built:\n"
	@ls -lh dist/dwmterm-v$(VERSION)-linux-x86_64.tar.gz

.PHONY: all install uninstall clean test dist
