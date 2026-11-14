ROOT_DIR := $(realpath ../../..)
include $(ROOT_DIR)/config.mk

CONFIGS := $(if $(findstring -DVX_CFG_NUM_CORES,$(CONFIGS)),$(CONFIGS),$(CONFIGS) -DVX_CFG_NUM_CORES=2)
CONFIGS := $(if $(findstring -DVX_CFG_TWO_WORLD_ENABLE,$(CONFIGS)),$(CONFIGS),$(CONFIGS) -DVX_CFG_TWO_WORLD_ENABLE)
CONFIGS := $(if $(findstring -DVX_CFG_SECURE_CORE_MASK,$(CONFIGS)),$(CONFIGS),$(CONFIGS) -DVX_CFG_SECURE_CORE_MASK=2)
CONFIGS := $(if $(findstring -DVX_CFG_L2_ENABLE,$(CONFIGS)),$(CONFIGS),$(CONFIGS) -DVX_CFG_L2_ENABLE)
# The stock simulator has zero external-memory latency, which makes an L2 hit
# and an L2 miss indistinguishable to a cycle-counter side channel.
CONFIGS := $(if $(findstring -DVX_CFG_PLATFORM_MEMORY_LATENCY,$(CONFIGS)),$(CONFIGS),$(CONFIGS) -DVX_CFG_PLATFORM_MEMORY_LATENCY=64)

PROJECT := cache_side_channel
SRC_DIR := $(VORTEX_HOME)/tests/regression/$(PROJECT)
SRCS := $(SRC_DIR)/main.cpp
VX_SRCS := $(SRC_DIR)/kernel.cpp
HDRS := $(SRC_DIR)/common.h
VX_HDRS := $(SRC_DIR)/common.h
KERNEL_LIB := vortex2

include ../common.mk
