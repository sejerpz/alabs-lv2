#!/usr/bin/make -f
# Euclides (aLabs) - LV2 plugin build (DPF)
# Native build: make. Cross build for the MOD Dwarf: see build-moddwarf.sh
# (or mod-plugin-builder, make moddwarf).

include dpf/Makefile.base.mk

# detect cross builds (MOD toolchain, aarch64-linux-gnu-g++, ...)
ifneq ($(shell $(CC) -dumpmachine | cut -d- -f1),$(shell uname -m))
CROSS_COMPILING = true
endif

NAME   = euclides.lv2
BUNDLE = bin/$(NAME)
PREFIX ?= $(HOME)/.lv2
DWARF  ?= 192.168.51.1

all: plugins gen

plugins:
	$(MAKE) all -C plugins/Euclides

ifneq ($(CROSS_COMPILING),true)
gen: plugins dpf/utils/lv2_ttl_generator
	@$(CURDIR)/dpf/utils/generate-ttl.sh
	@mkdir -p lv2-ttl && cp -f bin/euclides.lv2/*.ttl lv2-ttl/
else
# cross build (e.g. make moddwarf): the generator can't run, use the committed TTL
gen: plugins
	cp -f lv2-ttl/*.ttl bin/euclides.lv2/
endif

dpf/utils/lv2_ttl_generator:
	$(MAKE) -C dpf/utils/lv2-ttl-generator

sequence: test/sequence.cpp plugins/Euclides/*.hpp
	$(CXX) -O2 -std=c++11 -Iplugins/Euclides test/sequence.cpp -o build/sequence

clean:
	$(MAKE) clean -C plugins/Euclides
	rm -rf bin build dist

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/$(NAME)
	cp -r $(BUNDLE)/* $(DESTDIR)$(PREFIX)/$(NAME)/

# Hot install on the device through mod-ui (no restart needed). The plugin must
# not be in the current pedalboard. Build for the Dwarf first, e.g.:
#   ./build-moddwarf.sh && make publish [DWARF=192.168.52.1]
publish:
	@if [ "$$(od -An -tu2 -j18 -N2 $(BUNDLE)/euclides_dsp.so | tr -d ' ')" != "183" ]; then \
		echo "error: $(BUNDLE)/euclides_dsp.so is not an aarch64 build (rebuild with ./build-moddwarf.sh)"; \
		exit 1; \
	fi
	@echo "publishing $(NAME) to $(DWARF)..."
	@resp=$$(tar -cz -C bin $(NAME) | base64 | curl -sS -F 'package=@-' http://$(DWARF)/sdk/install) || exit 1; \
	echo "$$resp"; \
	echo "$$resp" | grep -q '"ok": *true' || { \
		echo "error: install failed (is Euclides still in the current pedalboard?)"; \
		exit 1; \
	}

.PHONY: plugins gen sequence install publish
