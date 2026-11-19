# SPDX-License-Identifier: GPL-2.0-only
# make LVGL=<lvgl source> O=<object dir> OUT=<binary>
CC ?= $(CROSS_COMPILE)gcc
HERE := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
CFLAGS := -Os -ffunction-sections -fdata-sections -DLV_CONF_INCLUDE_SIMPLE -I$(HERE) -I$(LVGL) -I$(LVGL)/..
SRCS := $(shell find $(LVGL)/src -name '*.c')
OBJS := $(patsubst $(LVGL)/%.c,$(O)/lvgl/%.o,$(SRCS)) $(O)/erhai-ui.o

$(OUT): $(OBJS)
	$(CC) -static -s -Wl,--gc-sections -o $@ $(OBJS) -lm

$(O)/lvgl/%.o: $(LVGL)/%.c $(HERE)lv_conf.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(O)/erhai-ui.o: $(HERE)erhai-ui.c $(HERE)lv_conf.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wall -c -o $@ $<
