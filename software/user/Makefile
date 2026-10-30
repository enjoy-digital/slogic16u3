# SLogic16U3 host software: libslogic16u3, tools, tests.
#
#   make            # libslogic16u3 (static/shared), tools.
#   make check      # Unit tests (simulated device).
#   make install    # PREFIX=/usr/local.

PREFIX  ?= /usr/local
VERSION := 0.1.0

LIBUSB_CFLAGS := $(shell pkg-config --cflags libusb-1.0)
LIBUSB_LIBS   := $(shell pkg-config --libs libusb-1.0)

CC      ?= gcc
AR      ?= ar
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wextra -Wno-unused-parameter -std=gnu11 -fPIC -MMD -Ilibslogic16u3 $(LIBUSB_CFLAGS)
LDLIBS  := $(LIBUSB_LIBS) -lm

BUILD   := build
LIB_SRC := libslogic16u3/slogic.c libslogic16u3/slogic_usb.c libslogic16u3/slogic_sim.c libslogic16u3/slogic_flash.c \
           libslogic16u3/slogic_dfu.c
LIB_OBJ := $(LIB_SRC:%.c=$(BUILD)/%.o)
TOOLS   := slogic16u3_util slogic16u3_record slogic16u3_scopehal_bridge
TESTS   := test_libslogic16u3

.PHONY: all lib tools check install clean
all: lib tools

lib: $(BUILD)/libslogic16u3.a $(BUILD)/libslogic16u3.so

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/libslogic16u3.a: $(LIB_OBJ)
	$(AR) rcs $@ $^

$(BUILD)/libslogic16u3.so: $(LIB_OBJ)
	$(CC) -shared -Wl,-soname,libslogic16u3.so.0 -o $@ $^ $(LDLIBS)

tools: $(TOOLS:%=$(BUILD)/%)

$(BUILD)/slogic16u3_scopehal_bridge.o: ../scopehal/slogic16u3_scopehal_bridge.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/slogic16u3_%: $(BUILD)/slogic16u3_%.o $(BUILD)/libslogic16u3.a
	$(CC) -o $@ $^ $(LDLIBS)

$(BUILD)/test_%: $(BUILD)/test/test_%.o $(BUILD)/libslogic16u3.a
	$(CC) -o $@ $^ $(LDLIBS)

check: $(TESTS:%=$(BUILD)/%)
	@for t in $^; do echo "== $$t"; ./$$t || exit 1; done

install: all
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/include
	install -m 755 $(TOOLS:%=$(BUILD)/%) $(DESTDIR)$(PREFIX)/bin
	install -m 644 $(BUILD)/libslogic16u3.a $(DESTDIR)$(PREFIX)/lib
	install -m 755 $(BUILD)/libslogic16u3.so $(DESTDIR)$(PREFIX)/lib/libslogic16u3.so.$(VERSION)
	ln -sf libslogic16u3.so.$(VERSION) $(DESTDIR)$(PREFIX)/lib/libslogic16u3.so.0
	ln -sf libslogic16u3.so.0 $(DESTDIR)$(PREFIX)/lib/libslogic16u3.so
	install -m 644 libslogic16u3/slogic.h libslogic16u3/slogic_regs.h $(DESTDIR)$(PREFIX)/include

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
