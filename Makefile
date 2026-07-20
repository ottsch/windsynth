# Project Name
TARGET = windsynth
.DEFAULT_GOAL := all

# Sources
CPP_SOURCES = windsynth.cpp

# Library Locations
LIBDAISY_DIR = third_party/libDaisy
DAISYSP_DIR = third_party/DaisySP

USE_DAISYSP_LGPL = 1

# Core location, and generic Makefile.
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

.PHONY: deps
deps:
	$(MAKE) -C $(LIBDAISY_DIR)
	$(MAKE) -C $(DAISYSP_DIR)
