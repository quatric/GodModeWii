.SUFFIXES:
ifeq ($(strip $(DEVKITPPC)),)
$(error "Please set DEVKITPPC in your environment. export DEVKITPPC=<path to>devkitPPC")
endif

include $(DEVKITPPC)/wii_rules

TARGET		:=	GodModeWii
BUILD		:=	build
UNARR		:=	source/unarr source/unarr/common source/unarr/zip source/unarr/rar source/unarr/_7z source/unarr/lzmasdk
SOURCES		:=	source libinput-wii/source $(UNARR)
DATA		:=	data
INCLUDES	:=	source $(UNARR)

CFLAGS	= -g -O2 -Wall $(MACHDEP) $(INCLUDE) -DHAVE_7Z
CXXFLAGS	=	$(CFLAGS)
LDFLAGS	=	-g $(MACHDEP) -Wl,-Map,$(notdir $@).map -Wl,-wrap,__lwp_heap_allocate -Wl,-wrap,__lwp_heap_free

LIBS	:=	-ldi -lfat -lwiikeyboard -lwiiuse -lbte -logc -lm
LIBDIRS	:=	$(CURDIR)/libinput-wii # headers: <input_wii.h>
# Optional controller support (build the libraries first with
# scripts/build_controller_portlibs.sh):
#   make WITH_WIIDRC=1     Wii U GamePad (vWii)
#   make WITH_WUPC=1       Wii U Pro Controller (vWii)
#   make WITH_SICKSAXIS=1  PS3 SIXAXIS / DualShock 3
WITH_WIIDRC ?= 0
WITH_WUPC ?= 0
WITH_SICKSAXIS ?= 0

ifneq ($(filter 1,$(WITH_WIIDRC) $(WITH_WUPC) $(WITH_SICKSAXIS)),)
LIBDIRS	+=	$(CURDIR)/portlibs/ppc
endif
ifeq ($(WITH_WIIDRC),1)
CFLAGS	+=	-DHAVE_WIIDRC
LIBS	:=	-lwiidrc $(LIBS)
endif
ifeq ($(WITH_WUPC),1)
CFLAGS	+=	-DHAVE_WUPC
LDFLAGS	+=	-Wl,-wrap,wiiuse_accept
LIBS	:=	-lwupc $(LIBS)
endif
ifeq ($(WITH_SICKSAXIS),1)
CFLAGS	+=	-DHAVE_SICKSAXIS
LIBS	:=	-lsicksaxis $(LIBS)
endif

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
					$(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=
sFILES		:=
SFILES		:=
BINFILES	:=

export LD	:=	$(CC)
export OFILES	:=	$(addsuffix .o,$(BINFILES)) \
					$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) \
					$(sFILES:.s=.o) $(SFILES:.S=.o)
export INCLUDE	:=	$(foreach dir,$(INCLUDES), -iquote $(CURDIR)/$(dir)) \
					$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
					-I$(CURDIR)/$(BUILD) \
					-I$(LIBOGC_INC)
export LIBPATHS	:=	-L$(LIBOGC_LIB) $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
export OUTPUT	:=	$(CURDIR)/$(TARGET)

.PHONY: $(BUILD) clean run

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).dol

run:
	wiiload $(TARGET).dol

else

DEPENDS	:=	$(OFILES:.o=.d)

$(OUTPUT).dol: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif
