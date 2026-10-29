.SUFFIXES:
ifeq ($(strip $(DEVKITPPC)),)
$(error "Please set DEVKITPPC in your environment. export DEVKITPPC=<path to>devkitPPC")
endif

include $(DEVKITPPC)/wii_rules

TARGET		:=	GodModeWii
BUILD		:=	build
UNARR		:=	source/unarr source/unarr/common source/unarr/zip source/unarr/rar source/unarr/_7z source/unarr/lzmasdk
SOURCES		:=	source libwiiinput/source $(UNARR)
DATA		:=	data
INCLUDES	:=	source $(UNARR)

CFLAGS	= -g -O2 -Wall $(MACHDEP) $(INCLUDE) -DHAVE_7Z
CXXFLAGS	=	$(CFLAGS)
LDFLAGS	=	-g $(MACHDEP) -Wl,-Map,$(notdir $@).map -Wl,-wrap,__lwp_heap_allocate -Wl,-wrap,__lwp_heap_free

LIBS	:=	-ldi -lfat -lwiikeyboard -lwiiuse -lbte -logc -lm
LIBS	:=	-lwiiupdate $(LIBS)
LIBDIRS	:=	$(CURDIR)/libwiiinput # headers: <input_wii.h>
LIBDIRS	+=	$(CURDIR)/libwiiupdate
# Optional controller support (build the libraries first with
# scripts/build_controller_portlibs.sh):
#   make WITH_WIIDRC=1     Wii U GamePad (vWii)
#   make WITH_WUPC=1       Wii U Pro Controller (vWii)
#   make WITH_SICKSAXIS=1  PS3 SIXAXIS / DualShock 3
# exFAT on SD and USB (build libfat first with scripts/build_exfat_libfat.sh):
#   make WITH_EXFAT=1
# NTFS on SD and USB (build libntfs first with scripts/build_libntfs.sh):
#   make WITH_NTFS=1
WITH_WIIDRC ?= 0
WITH_WUPC ?= 0
WITH_SICKSAXIS ?= 0
WITH_EXFAT ?= 0
WITH_NTFS ?= 0

ifeq ($(WITH_EXFAT),1)
EXFAT_LIB	:=	$(CURDIR)/portlibs/exfat/lib
endif

ifeq ($(WITH_NTFS),1)
CFLAGS	+=	-DHAVE_NTFS
LIBDIRS	+=	$(CURDIR)/portlibs/ntfs
LIBS	:=	-lntfs $(LIBS)
endif

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

# The .dol that goes into the Homebrew Channel package.
ifeq ($(NO_DOLXZ),1)
BOOT_DOL	=	$(TARGET).dol
else
BOOT_DOL	=	$(TARGET)-xz.dol
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
ifneq ($(EXFAT_LIB),)
ifeq ($(wildcard $(EXFAT_LIB)/libfat.a),)
$(error WITH_EXFAT=1 needs scripts/build_exfat_libfat.sh to be run first)
endif
endif
ifeq ($(WITH_NTFS),1)
ifeq ($(wildcard $(CURDIR)/portlibs/ntfs/lib/libntfs.a),)
$(error WITH_NTFS=1 needs scripts/build_libntfs.sh to be run first)
endif
endif
# The exFAT libfat has to come before libogc's own.
export LIBPATHS	:=	$(addprefix -L,$(EXFAT_LIB)) -L$(LIBOGC_LIB) $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
export OUTPUT	:=	$(CURDIR)/$(TARGET)

.PHONY: $(BUILD) clean run

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD): libwiiupdate-lib
.PHONY: libwiiupdate-lib
libwiiupdate-lib:
	@$(MAKE) --no-print-directory -C libwiiupdate

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).dol $(OUTPUT)-xz.dol

run:
	wiiload $(TARGET).dol

else

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# Self-extracting xz-compressed copy of the .dol (FIX94's dolxz, see
# scripts/dolxz.sh). It is the default target; NO_DOLXZ=1 skips it.
#---------------------------------------------------------------------------------
ifneq ($(NO_DOLXZ),1)
$(OUTPUT)-xz.dol: $(OUTPUT).dol
	@echo compressing ... $(notdir $@)
	@sh $(dir $(OUTPUT))scripts/dolxz.sh $< $@
endif

$(OUTPUT).dol: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif
