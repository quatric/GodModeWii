.SUFFIXES:
ifeq ($(strip $(DEVKITPPC)),)
$(error "Please set DEVKITPPC in your environment. export DEVKITPPC=<path to>devkitPPC")
endif

include $(DEVKITPPC)/wii_rules

TARGET		:=	GodModeWii
BUILD		:=	build
UNARR		:=	source/unarr source/unarr/common source/unarr/zip source/unarr/rar source/unarr/_7z source/unarr/lzmasdk
SOURCES		:=	source source/mobiclip source/zstd source/ajpg libwiiinput/source $(UNARR)
DATA		:=	data res
INCLUDES	:=	source source/mobiclip source/zstd source/ajpg $(UNARR)

# libwiiinput: pen tablets / uDraw (drawing) and Wii Speak / USB microphones (voice memos)
CFLAGS	= -g -O2 -Wall $(MACHDEP) $(INCLUDE) -DHAVE_7Z -DHAVE_TABLET -DHAVE_MIC
# make AUTOTEST=1 BUILD=build_test TARGET=GodModeWiiTest NO_DOLXZ=1: a test build that opens the files listed
# in sd:/gmwtest.txt at start and saves each viewer's screen to the SD card (see scripts/autotest.sh).
ifeq ($(AUTOTEST),1)
CFLAGS	+=	-DGMW_AUTOTEST
endif
CXXFLAGS	=	$(CFLAGS)
LDFLAGS	=	-g $(MACHDEP) -Wl,-Map,$(notdir $@).map -Wl,-wrap,__lwp_heap_allocate -Wl,-wrap,__lwp_heap_free -Wl,--wrap=classic_ctrl_handshake,--wrap=parse_event,--wrap=USB_GetDeviceList

LIBS	:=	-lbz2 -lpng16 -ljpeg -lwebp -lsharpyuv -lz -ldi -lfat -lwiikeyboard -lwiiuse -lbte -lasnd -lvorbisidec -logg -logc -lm
LIBS	:=	-lwiiupdate $(LIBS)
LIBDIRS	:=	$(CURDIR)/libwiiinput # headers: <input_wii.h>
LIBDIRS	+=	$(CURDIR)/libwiiupdate
# libpng, libjpeg, libwebp, zlib, libmad, tremor: devkitPro portlibs (ppc-libpng, ppc-libjpeg-turbo, ppc-libwebp, ...)
LIBDIRS	+=	$(PORTLIBS_PATH)/ppc
# Optional controller support (build the libraries first with
# scripts/build_controller_portlibs.sh):
#   make WITH_WIIDRC=1     Wii U GamePad (vWii)
#   make WITH_WUPC=1       Wii U Pro Controller (vWii)
#   make WITH_SICKSAXIS=1  PS3 SIXAXIS / DualShock 3
# exFAT on SD and USB (build libfat first with scripts/build_exfat_libfat.sh):
#   make WITH_EXFAT=1
# NTFS on SD and USB (build libntfs first with scripts/build_libntfs.sh):
#   make WITH_NTFS=1
# ext2/3/4 on SD and USB, read-only (build it first with scripts/build_libext4.sh; needs WITH_EXFAT=1):
#   make WITH_EXT4=1
WITH_WIIDRC ?= 0
WITH_WUPC ?= 0
WITH_SICKSAXIS ?= 0
WITH_EXFAT ?= 0
WITH_NTFS ?= 0
WITH_EXT4 ?= 0

ifeq ($(WITH_EXFAT),1)
EXFAT_LIB	:=	$(CURDIR)/portlibs/exfat/lib
endif

ifeq ($(WITH_EXT4),1)
CFLAGS	+=	-DHAVE_EXT4
LIBDIRS	+=	$(CURDIR)/portlibs/ext4
LIBS	:=	-lext4 $(LIBS)
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
BINFILES	:=	appboot.bin gmsf2.bin sfx.bin pmic_firm.bin tinyload.elf
# MiiKit (source/miikit) builds under its own names: its mii.c would land on mii.o beside ours.
MIIKIT		:=	mii miidraw miibody miifx miinand
export MIIKIT_DIR	:=	$(CURDIR)/source/miikit

export LD	:=	$(CC)
export OFILES	:=	$(addsuffix .o,$(BINFILES)) \
					$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(MIIKIT:%=miikit_%.o) \
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
ifeq ($(WITH_EXT4),1)
ifeq ($(wildcard $(CURDIR)/portlibs/ext4/lib/libext4.a),)
$(error WITH_EXT4=1 needs scripts/build_libext4.sh to be run first)
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

$(BUILD): libwiiupdate-lib appboot-bin
.PHONY: libwiiupdate-lib appboot-bin
libwiiupdate-lib:
	@$(MAKE) --no-print-directory -C libwiiupdate

# The app booter used to launch .dol / .elf files (from YAWM ModMii Edition),
# built on its own and linked in as data/appboot.bin.
appboot-bin:
	@$(MAKE) --no-print-directory -C appboot
	@mkdir -p data
	@cmp -s appboot/appboot.bin data/appboot.bin || cp appboot/appboot.bin data/appboot.bin

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).dol $(OUTPUT)-xz.dol data/appboot.bin
	@$(MAKE) --no-print-directory -C appboot clean

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

# The video decoders run at -O3, as in wii-mobiclip-player.
MOBICLIP_O := $(patsubst %.c,%.o,$(notdir $(wildcard $(OUTPUT:$(TARGET)=)source/mobiclip/*.c)))
$(MOBICLIP_O): CFLAGS += -O3

miikit_%.o: $(MIIKIT_DIR)/%.c
	$(SILENTMSG) miikit/$(notdir $<)
	$(SILENTCMD)$(CC) -MMD -MP -MF $(DEPSDIR)/miikit_$*.d $(CPPFLAGS) $(CFLAGS) -c $< -o $@ $(ERROR_FILTER)

launch.o: appboot_bin.h
midisynth.o: gmsf2_bin.h

%.bin.o %_bin.h : %.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

endif
