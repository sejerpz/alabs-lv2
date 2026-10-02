# Rootline (aLabs) — LV2 plugin build
# Cross-compile for MOD Dwarf (aarch64): make CXX=aarch64-linux-gnu-g++
# or use mod-plugin-builder. LV2_CFLAGS points to the LV2 headers when they
# are not installed system-wide (e.g. LV2_CFLAGS=-I/path/to/lv2/include).
#
# OLD_GLIBC=1 binds expf/exp2f to GLIBC_2.17, so a binary built with a recent
# generic cross compiler (e.g. Ubuntu g++-aarch64-linux-gnu) still loads on the
# Dwarf. Not needed with mod-plugin-builder:
#   make CXX=aarch64-linux-gnu-g++ OLD_GLIBC=1 publish [DWARF=192.168.52.1]
#
# The bundle is assembled in build/rootline.lv2.

CXX        ?= g++
LV2_CFLAGS ?= $(shell pkg-config --cflags lv2 2>/dev/null)
CXXFLAGS   ?= -O3 -ffast-math -fno-finite-math-only
CXXFLAGS   += -std=c++17 -fPIC -fvisibility=hidden -fno-exceptions -fno-rtti -Wall -Wextra $(LV2_CFLAGS)
# no libstdc++ at runtime: only libc/libm are needed on the device
LDFLAGS    += -shared -Wl,--no-undefined -nostdlib++ -lm

ifeq ($(OLD_GLIBC),1)
CXXFLAGS += -DROOTLINE_OLD_GLIBC
endif
ifneq ($(findstring aarch64,$(CXX)),)
CXXFLAGS += -mcpu=cortex-a35
endif

NAME    = rootline.lv2
SRCB    = $(NAME)
BUNDLE  = build/$(NAME)
PREFIX ?= $(HOME)/.lv2
DWARF  ?= 192.168.51.1

all: $(BUNDLE)/rootline.so ttl

$(BUNDLE)/rootline.so: src/rootline_lv2.cpp src/rootline_dsp.hpp
	@mkdir -p $(BUNDLE)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

ttl:
	@mkdir -p $(BUNDLE)
	cp $(SRCB)/*.ttl $(BUNDLE)/

# Offline tests (native build only): synthetic guitar signals, see test/
test/run_engine: test/run_engine.cpp src/rootline_dsp.hpp
	g++ -O2 -std=c++17 -Wall -o $@ $<

test/bench: test/bench.cpp src/rootline_dsp.hpp
	g++ -O3 -ffast-math -fno-finite-math-only -std=c++17 -Wall -o $@ $<

test: test/run_engine test/bench
	./test/bench
	cd test && python3 summary.py && python3 test_modes.py

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/$(NAME)
	cp -r $(BUNDLE)/* $(DESTDIR)$(PREFIX)/$(NAME)/

# Hot install on the device through mod-ui (no restart needed). The plugin must
# not be in the current pedalboard. Build for the Dwarf first (see above).
publish: all
	@if [ "$$(od -An -tu2 -j18 -N2 $(BUNDLE)/rootline.so | tr -d ' ')" != "183" ]; then \
		echo "error: $(BUNDLE)/rootline.so is not an aarch64 build (rebuild with CXX=aarch64-linux-gnu-g++)"; \
		exit 1; \
	fi
	@echo "publishing $(NAME) to $(DWARF)..."
	@resp=$$(tar -cz -C build $(NAME) | base64 | curl -sS -F 'package=@-' http://$(DWARF)/sdk/install) || exit 1; \
	echo "$$resp"; \
	echo "$$resp" | grep -q '"ok": *true' || { \
		echo "error: install failed (is Rootline still in the current pedalboard?)"; \
		exit 1; \
	}

clean:
	rm -rf build test/run_engine test/bench test/__pycache__

.PHONY: all ttl test install publish clean
