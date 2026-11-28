ifeq ($(filter test irdb,$(MAKECMDGOALS)),)
ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitPro")
endif

export DEVKITPPC ?= $(DEVKITPRO)/devkitPPC
export WUT_ROOT  ?= $(DEVKITPRO)/wut

include $(WUT_ROOT)/share/wut_rules

# devkitPPC's own archiver: no LTO here, and a gcc-ar elsewhere on the PATH
# may not find its plugin
AR      := $(DEVKITPPC)/bin/powerpc-eabi-ar
endif

CFLAGS  := -O2 -Wall -Wextra -ffunction-sections -fdata-sections $(MACHDEP) \
           -D__WIIU__ -D__WUT__ -Iinclude -I$(WUT_ROOT)/include

SRCS    := $(wildcard source/*.c)
OBJS    := $(patsubst source/%.c,build/%.o,$(SRCS))
LIB     := lib/libwiiuir.a

all: $(LIB)

$(LIB): $(OBJS)
	@mkdir -p lib
	@rm -f $@
	$(AR) rcs $@ $(OBJS)

build/%.o: source/%.c source/ir_platform.h include/ir_wiiu.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

install: $(LIB)
	@mkdir -p $(DESTDIR)$(DEVKITPRO)/portlibs/wiiu/lib $(DESTDIR)$(DEVKITPRO)/portlibs/wiiu/include
	cp $(LIB) $(DESTDIR)$(DEVKITPRO)/portlibs/wiiu/lib/
	cp include/ir_wiiu.h $(DESTDIR)$(DEVKITPRO)/portlibs/wiiu/include/

clean:
	rm -rf build lib build-test

# host test of the encoders, the .ir reader and the SIR bytes (no devkitPro needed)
test:
	@mkdir -p build-test
	cc -Wall -Wextra -fsanitize=address,undefined -Iinclude -Isource tests/ir_test.c \
	   source/ir_encode.c source/ir_file.c source/ir_db.c -o build-test/ir_test
	cc -Wall -Wextra -fsanitize=address,undefined -Iinclude -Isource tests/sir_test.c \
	   source/ir_sir.c -o build-test/sir_test
	mkdir -p build && build-test/ir_test
	build-test/sir_test

# irdb.xml from a Flipper-IRDB checkout: make irdb IRDB=path/to/Flipper-IRDB
IRDB ?= Flipper-IRDB
irdb:
	@mkdir -p build-test
	cc -O2 -Wall -Wextra -Iinclude -Isource tools/irdb_xml.c source/ir_encode.c source/ir_file.c -o build-test/irdb_xml
	build-test/irdb_xml "$(IRDB)" irdb.xml "$$(git -C "$(IRDB)" log -1 --format='commit %h, %cs' 2>/dev/null)"
	build-test/irdb_xml --converted "$(IRDB)" irdb_converted.xml "$$(git -C "$(IRDB)" log -1 --format='commit %h, %cs, _Converted_' 2>/dev/null)"

.PHONY: all install clean test irdb
