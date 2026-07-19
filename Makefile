PLUGIN  := libvolume.so
PKGS    := gtk+-3.0 gio-2.0 gio-unix-2.0 gtk-layer-shell-0
WBCOMMON ?= common
CFLAGS  ?= -O2 -Wall -Wextra
CFLAGS  += -fPIC -I$(WBCOMMON) $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS)) -lm
PREFIX  ?= $(HOME)/.local/lib/waybar
DATADIR ?= $(HOME)/.local/share/waybar-volume

$(PLUGIN): src/volume.c $(WBCOMMON)/wbcommon.h
	$(CC) $(CFLAGS) -shared -o $@ $< $(LDLIBS)

install: $(PLUGIN)
	install -Dm755 $(PLUGIN) $(PREFIX)/$(PLUGIN)
	install -Dm644 -t $(DATADIR) assets/vol-mute.svg assets/vol-low.svg assets/vol-med.svg assets/vol-high.svg assets/mic.svg assets/mic-mute.svg
	@echo "installed to $(PREFIX)/$(PLUGIN) + icons in $(DATADIR)"

test_volume: tests/test_volume.c src/volume.c $(WBCOMMON)/wbcommon.h
	$(CC) $(CFLAGS) -o $@ tests/test_volume.c $(LDLIBS)

test: test_volume
	./test_volume

clean:
	rm -f $(PLUGIN) test_volume
.PHONY: install clean test
