# SLogic16U3 host software: libslogic, tools, tests.
#
#   make            # libslogic (static/shared), tools.
#   make check      # Unit tests (simulated device).
#   make install    # PREFIX=/usr/local.

PREFIX  ?= /usr/local
VERSION := 0.1.0

LIBUSB_CFLAGS := $(shell pkg-config --cflags libusb-1.0)
LIBUSB_LIBS   := $(shell pkg-config --libs libusb-1.0)

CC      ?= gcc
AR      ?= ar
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wextra -Wno-unused-parameter -std=gnu11 -fPIC -MMD -Ilibslogic $(LIBUSB_CFLAGS)
LDLIBS  := $(LIBUSB_LIBS) -lm

BUILD   := build
LIB_SRC := libslogic/slogic.c libslogic/slogic_usb.c libslogic/slogic_sim.c libslogic/slogic_flash.c \
           libslogic/slogic_dfu.c
LIB_OBJ := $(LIB_SRC:%.c=$(BUILD)/%.o)
TOOLS   := slogic_util slogic_record
TESTS   := test_libslogic

.PHONY: all lib tools check install clean
all: lib tools

lib: $(BUILD)/libslogic.a $(BUILD)/libslogic.so

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/libslogic.a: $(LIB_OBJ)
	$(AR) rcs $@ $^

$(BUILD)/libslogic.so: $(LIB_OBJ)
	$(CC) -shared -Wl,-soname,libslogic.so.0 -o $@ $^ $(LDLIBS)

tools: $(TOOLS:%=$(BUILD)/%)

$(BUILD)/slogic_%: $(BUILD)/slogic_%.o $(BUILD)/libslogic.a
	$(CC) -o $@ $^ $(LDLIBS)

$(BUILD)/test_%: $(BUILD)/test/test_%.o $(BUILD)/libslogic.a
	$(CC) -o $@ $^ $(LDLIBS)

check: $(TESTS:%=$(BUILD)/%)
	@for t in $^; do echo "== $$t"; ./$$t || exit 1; done

install: all
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/include
	install -m 755 $(TOOLS:%=$(BUILD)/%) $(DESTDIR)$(PREFIX)/bin
	install -m 644 $(BUILD)/libslogic.a $(DESTDIR)$(PREFIX)/lib
	install -m 755 $(BUILD)/libslogic.so $(DESTDIR)$(PREFIX)/lib/libslogic.so.$(VERSION)
	ln -sf libslogic.so.$(VERSION) $(DESTDIR)$(PREFIX)/lib/libslogic.so.0
	ln -sf libslogic.so.0 $(DESTDIR)$(PREFIX)/lib/libslogic.so
	install -m 644 libslogic/slogic.h libslogic/slogic_regs.h $(DESTDIR)$(PREFIX)/include

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
