#!/usr/bin/make -f
# BlipMachine - DPF build

NAME = blipmachine

FILES_DSP = \
	BlipMachinePlugin.cpp

include ../../dpf/Makefile.plugins.mk

BUILD_CXX_FLAGS += -ffast-math

TARGETS += lv2_dsp

all: $(TARGETS)
