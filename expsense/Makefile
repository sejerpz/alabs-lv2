# ExpSense (aLabs) — LV2 plugin build
# Cross-compile for MOD Dwarf (aarch64): make CC=aarch64-linux-gnu-gcc
# or use mod-plugin-builder. LV2_CFLAGS points to the LV2 headers when they
# are not installed system-wide (e.g. LV2_CFLAGS=-I/path/to/lv2/include).
#
# The bundle is assembled in build/expsense.lv2 with the modgui theme chosen
# by UI (a folder under ui/):
#   make UI=classic   (default) dark panel, amber display
#   make UI=tin       lithographed mint-tin look

CC         ?= gcc
LV2_CFLAGS ?= $(shell pkg-config --cflags lv2 2>/dev/null)
CFLAGS     ?= -O2 -ffast-math
CFLAGS     += -std=c99 -D_DEFAULT_SOURCE -fPIC -Wall -Wextra -fvisibility=hidden $(LV2_CFLAGS)
LDFLAGS    += -shared -lm

UI     ?= classic
NAME    = expsense.lv2
SRCB    = $(NAME)
BUNDLE  = build/$(NAME)
GUI     = $(BUNDLE)/modgui
PREFIX ?= $(HOME)/.lv2
DWARF  ?= 192.168.51.1

ifeq ($(wildcard ui/$(UI)/icon-expsense.html),)
$(error unknown UI '$(UI)': available themes are $(notdir $(patsubst %/,%,$(wildcard ui/*/))))
endif

all: $(BUNDLE)/expsense.so gui

$(BUNDLE)/expsense.so: src/expsense.c
	@mkdir -p $(BUNDLE)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# Rebuilt on every run, so switching UI= always takes effect.
gui:
	@mkdir -p $(GUI)
	rm -f $(GUI)/*
	cp $(SRCB)/*.ttl $(BUNDLE)/
	cp $(SRCB)/modgui/script-expsense.js $(GUI)/
	cp ui/$(UI)/icon-expsense.html ui/$(UI)/*.png $(GUI)/
	cat ui/base.css $(wildcard ui/$(UI)/theme.css) > $(GUI)/stylesheet-expsense.css
	@echo "modgui theme: $(UI)"

test/sim: test/sim.c
	$(CC) -O2 -std=c99 -D_DEFAULT_SOURCE -Wall $(LV2_CFLAGS) -o $@ $< -ldl -lm

test: all test/sim
	./test/sim $(BUNDLE)/expsense.so test/out.csv

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/$(NAME)
	cp -r $(BUNDLE)/* $(DESTDIR)$(PREFIX)/$(NAME)/

# Hot install on the device through mod-ui (no restart needed). The plugin must
# not be in the current pedalboard. Build for the Dwarf first, e.g.:
#   make CC=aarch64-linux-gnu-gcc LV2_CFLAGS=... UI=tin publish [DWARF=192.168.52.1]
publish: all
	@if [ "$$(od -An -tu2 -j18 -N2 $(BUNDLE)/expsense.so | tr -d ' ')" != "183" ]; then \
		echo "error: $(BUNDLE)/expsense.so is not an aarch64 build (rebuild with CC=aarch64-linux-gnu-gcc)"; \
		exit 1; \
	fi
	@echo "publishing $(NAME) ($(UI) theme) to $(DWARF)..."
	@resp=$$(tar -cz -C build $(NAME) | base64 | curl -sS -F 'package=@-' http://$(DWARF)/sdk/install) || exit 1; \
	echo "$$resp"; \
	echo "$$resp" | grep -q '"ok": *true' || { \
		echo "error: install failed (is ExpSense still in the current pedalboard?)"; \
		exit 1; \
	}

clean:
	rm -rf build test/sim test/out.csv

.PHONY: all gui test install publish clean
