# TTerminal for Linux
#   make                  build build/tterminal
#   sudo make install     install to /usr/local (PREFIX, DESTDIR supported)
#   make deb VERSION=x    build a .deb in build/
#   make tar VERSION=x    build a .tar.gz in build/

PREFIX  ?= /usr/local
VERSION ?= 1.0.0
ARCH    ?= $(shell dpkg --print-architecture 2>/dev/null || uname -m)
PKGS    := gtk+-3.0 webkit2gtk-4.1
B       := build

CFLAGS  ?= -O2 -pipe
CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-parameter $(shell pkg-config --cflags $(PKGS))
LDFLAGS += -Wl,-O1,--as-needed -s
LDLIBS  := $(shell pkg-config --libs $(PKGS)) -lutil

WEB := $(sort $(wildcard ../web/*))

all: $(B)/tterminal

# Web UI and icon are compiled into the binary, zlib-compressed
$(B)/resources.xml: $(WEB) Makefile | $(B)
	@{ echo '<?xml version="1.0" encoding="UTF-8"?><gresources><gresource prefix="/tterminal">'; \
	   for f in $(notdir $(WEB)); do echo "<file compressed=\"true\">web/$$f</file>"; done; \
	   echo '<file>tterminal.png</file></gresource></gresources>'; } > $@

$(B)/resources.c: $(B)/resources.xml tterminal.png
	glib-compile-resources --sourcedir=.. --sourcedir=. --generate-source --target=$@ $<

$(B)/tterminal: tterminal.c $(B)/resources.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(B):
	mkdir -p $@

install: $(B)/tterminal
	install -Dm755 $(B)/tterminal $(DESTDIR)$(PREFIX)/bin/tterminal
	install -Dm644 tterminal.desktop $(DESTDIR)$(PREFIX)/share/applications/tterminal.desktop
	install -Dm644 tterminal.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps/tterminal.png

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/tterminal \
	      $(DESTDIR)$(PREFIX)/share/applications/tterminal.desktop \
	      $(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps/tterminal.png

deb: $(B)/tterminal
	rm -rf $(B)/deb
	$(MAKE) install DESTDIR=$(B)/deb PREFIX=/usr
	mkdir -p $(B)/deb/DEBIAN
	sed -e 's/@VERSION@/$(VERSION)/' -e 's/@ARCH@/$(ARCH)/' \
	    -e "s/@SIZE@/$$(du -sk $(B)/deb/usr | cut -f1)/" control.in > $(B)/deb/DEBIAN/control
	dpkg-deb --root-owner-group -Zxz --build $(B)/deb $(B)/tterminal_$(VERSION)_$(ARCH).deb

tar: $(B)/tterminal
	rm -rf $(B)/tterminal-$(VERSION)
	mkdir -p $(B)/tterminal-$(VERSION)
	cp $(B)/tterminal tterminal.desktop tterminal.png ../LICENSE $(B)/tterminal-$(VERSION)/
	tar -C $(B) -czf $(B)/tterminal-$(VERSION)-linux-$(ARCH).tar.gz tterminal-$(VERSION)

clean:
	rm -rf $(B)

.PHONY: all install uninstall deb tar clean
