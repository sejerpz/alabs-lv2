#!/usr/bin/make -f
# Euclides - DPF build

NAME = euclides

FILES_DSP = \
	EuclidesPlugin.cpp

include ../../dpf/Makefile.plugins.mk

BUILD_CXX_FLAGS += -ffast-math

TARGETS += lv2_dsp

all: $(TARGETS)
