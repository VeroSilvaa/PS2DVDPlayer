EE_BIN = build/PS2DVDPlayer.elf

EE_OBJS = \
	build/main.o \
	build/input.o \
	build/memcard.o \
	build/netcfg.o \
	build/iop_boot.o \
	build/network.o \
	build/smb.o \
	build/iso.o \
	build/frame.o \
	build/memlog.o \
	build/nav.o \
	build/spu.o \
	build/audio.o \
	build/libmpeg.o \
	build/libmpeg_core.o \
	build/libmpeg_op.o \
	build/io_modules.o \
	build/bdm.o \
	build/DEV9_irx.o \
	build/NETMAN_irx.o \
	build/SMAP_irx.o \
	build/IOMANX_irx.o \
	build/FILEXIO_irx.o \
	build/PS2IP_irx.o \
	build/PS2IPS_irx.o \
	build/BDM_irx.o \
	build/BDMFS_FATFS_irx.o \
	build/USBD_irx.o \
	build/USBMASS_BD_irx.o \
	build/ATA_BD_irx.o \
	build/SMBMAN_irx.o \
	build/LIBSD_irx.o \
	build/AUDSRV_irx.o

# libdvdread + libdvdnav (VideoLAN, GPL v2), ver third_party/ps2/README.txt
DVDREAD_SRCS := $(wildcard third_party/libdvdread/src/*.c)
DVDNAV_SRCS := $(wildcard third_party/libdvdnav/src/*.c)
DVDVM_SRCS := $(wildcard third_party/libdvdnav/src/vm/*.c)

DVD_OBJS := \
	$(patsubst third_party/libdvdread/src/%.c,build/dvdread/%.o,$(DVDREAD_SRCS)) \
	$(patsubst third_party/libdvdnav/src/%.c,build/dvdnav/%.o,$(DVDNAV_SRCS)) \
	$(patsubst third_party/libdvdnav/src/vm/%.c,build/dvdnav/vm/%.o,$(DVDVM_SRCS)) \
	build/dvdnav/ps2_compat.o

EE_OBJS += $(DVD_OBJS)

# liba52 0.7.4 (GPL v2), ver third_party/liba52/PS2_README.txt
A52_SRCS := $(wildcard third_party/liba52/*.c)
A52_OBJS := $(patsubst third_party/liba52/%.c,build/a52/%.o,$(A52_SRCS))

EE_OBJS += $(A52_OBJS)

A52_CFLAGS = \
	-fsingle-precision-constant \
	-Wno-array-bounds \
	-Ithird_party/liba52 \
	-Ithird_party/liba52/include

DVD_INCS = \
	-Ithird_party/libdvdnav/src \
	-Ithird_party/libdvdread/src

DVD_CFLAGS = \
	-DHAVE_CONFIG_H \
	-Ithird_party/ps2 \
	-Ithird_party/libdvdread/src/dvdread \
	-Ithird_party/libdvdnav/src/dvdnav \
	-Ithird_party/libdvdnav/src/vm \
	$(DVD_INCS) \
	-Wno-incompatible-pointer-types \
	-w

EE_LIBS = \
	-L$(GSKIT)/lib \
	-L$(PS2SDK)/ports/lib \
	-lgskit_toolkit \
	-lgskit \
	-ldmakit \
	-ldma \
	-lnetman \
	-lps2ips \
	-lpatches \
	-lpad \
	-lmc \
	-lfileXio \
	-laudsrv \
	-lm \
	-lkernel

EE_INCS += \
	-I$(PS2SDK)/common/include \
	-I$(PS2SDK)/ports/include \
	-Iinclude

.PHONY: all clean

all: $(EE_BIN)


# ============================================================
# EE SOURCES
# ============================================================

build/main.o: src/main.c
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) \
		-I$(GSKIT)/include \
		-I$(GSKIT)/ee/dma/include \
		-I$(GSKIT)/ee/toolkit/include \
		-c $< -o $@

build/input.o: src/input.c
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) \
		-c $< -o $@

build/memcard.o: src/memcard.c
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) \
		-c $< -o $@

build/netcfg.o: src/netcfg.c
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) \
		-c $< -o $@

build/iop_boot.o: src/iop_boot.c
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) \
		-c $< -o $@

build/network.o: src/network.c
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) \
		-c $< -o $@

build/smb.o: src/smb.c
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) \
		-c $< -o $@

# libmpeg do PS2SDK compilada no projeto (diagnóstico), ver src/libmpeg/README.txt
build/libmpeg.o: src/libmpeg/libmpeg.c
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -Isrc/libmpeg \
		-c $< -o $@

build/libmpeg_core.o: src/libmpeg/libmpeg_core.c
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -Isrc/libmpeg \
		-c $< -o $@

build/libmpeg_op.o: src/libmpeg/libmpeg_op.s
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) -c $< -o $@

build/memlog.o: src/memlog.c
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) \
		-c $< -o $@

build/nav.o: src/nav.c
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) $(DVD_INCS) \
		-Wno-attributes \
		-c $< -o $@

# Código de terceiros: avisos desligados (-w), ver third_party/ps2/README.txt
build/dvdread/%.o: third_party/libdvdread/src/%.c
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) $(DVD_CFLAGS) $(EE_INCS) -c $< -o $@

build/dvdnav/vm/%.o: third_party/libdvdnav/src/vm/%.c
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) $(DVD_CFLAGS) $(EE_INCS) -c $< -o $@

build/dvdnav/%.o: third_party/libdvdnav/src/%.c
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) $(DVD_CFLAGS) $(EE_INCS) -c $< -o $@

build/dvdnav/ps2_compat.o: third_party/ps2/compat.c
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) $(DVD_CFLAGS) $(EE_INCS) -c $< -o $@

build/a52/%.o: third_party/liba52/%.c
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) $(A52_CFLAGS) $(EE_INCS) -c $< -o $@

build/audio.o: src/audio.c
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -fsingle-precision-constant \
		-Ithird_party/liba52/include \
		-c $< -o $@

build/spu.o: src/spu.c
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) \
		-c $< -o $@

build/frame.o: src/frame.c
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) \
		-c $< -o $@

build/iso.o: src/iso.c
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) \
		-c $< -o $@

build/io_modules.o: src/io_modules.c
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) \
		-c $< -o $@

build/bdm.o: src/bdm.c
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) \
		-c $< -o $@


# ============================================================
# IRX -> C
# ============================================================

build/DEV9_irx.c:
	@mkdir -p $(@D)
	bin2c $(PS2SDK)/iop/irx/ps2dev9.irx $@ DEV9_irx

build/NETMAN_irx.c:
	@mkdir -p $(@D)
	bin2c $(PS2SDK)/iop/irx/netman.irx $@ NETMAN_irx

build/SMAP_irx.c:
	@mkdir -p $(@D)
	bin2c $(PS2SDK)/iop/irx/smap.irx $@ SMAP_irx

build/IOMANX_irx.c:
	@mkdir -p $(@D)
	bin2c $(PS2SDK)/iop/irx/iomanX.irx $@ IOMANX_irx

build/FILEXIO_irx.c:
	@mkdir -p $(@D)
	bin2c $(PS2SDK)/iop/irx/fileXio.irx $@ FILEXIO_irx

# Pilha TCP/IP no IOP (mesmo modelo do OPL):
# ps2ip-nm.irx usa o NETMAN/SMAP e atende o SMBMAN;
# ps2ips.irx expoe essa pilha ao EE via RPC (-lps2ips).
build/PS2IP_irx.c:
	@mkdir -p $(@D)
	bin2c $(PS2SDK)/iop/irx/ps2ip-nm.irx $@ PS2IP_irx

build/PS2IPS_irx.c:
	@mkdir -p $(@D)
	bin2c $(PS2SDK)/iop/irx/ps2ips.irx $@ PS2IPS_irx

build/SMBMAN_irx.c:
	@mkdir -p $(@D)
	bin2c $(PS2SDK)/iop/irx/smbman.irx $@ SMBMAN_irx


# ============================================================
# IRX C -> EE OBJECT
# ============================================================

build/DEV9_irx.o: build/DEV9_irx.c
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) -c $< -o $@

build/NETMAN_irx.o: build/NETMAN_irx.c
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) -c $< -o $@

build/SMAP_irx.o: build/SMAP_irx.c
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) -c $< -o $@

build/IOMANX_irx.o: build/IOMANX_irx.c
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) -c $< -o $@

build/FILEXIO_irx.o: build/FILEXIO_irx.c
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) -c $< -o $@

build/PS2IP_irx.o: build/PS2IP_irx.c
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) -c $< -o $@

build/PS2IPS_irx.o: build/PS2IPS_irx.c
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) -c $< -o $@

# USB e HDD em FAT32/exFAT pelo BDM (mesma pilha do OPL/PS2FileManager),
# usados em bdm.c. HDD em APA/PFS não é suportado.
build/BDM_irx.c:
	@mkdir -p $(@D)
	bin2c $(PS2SDK)/iop/irx/bdm.irx $@ BDM_irx

build/BDMFS_FATFS_irx.c:
	@mkdir -p $(@D)
	bin2c $(PS2SDK)/iop/irx/bdmfs_fatfs.irx $@ BDMFS_FATFS_irx

build/USBD_irx.c:
	@mkdir -p $(@D)
	bin2c $(PS2SDK)/iop/irx/usbd.irx $@ USBD_irx

build/USBMASS_BD_irx.c:
	@mkdir -p $(@D)
	bin2c $(PS2SDK)/iop/irx/usbmass_bd.irx $@ USBMASS_BD_irx

build/ATA_BD_irx.c:
	@mkdir -p $(@D)
	bin2c $(PS2SDK)/iop/irx/ata_bd.irx $@ ATA_BD_irx

build/BDM_irx.o: build/BDM_irx.c
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) -c $< -o $@

build/BDMFS_FATFS_irx.o: build/BDMFS_FATFS_irx.c
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) -c $< -o $@

build/USBD_irx.o: build/USBD_irx.c
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) -c $< -o $@

build/USBMASS_BD_irx.o: build/USBMASS_BD_irx.c
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) -c $< -o $@

build/ATA_BD_irx.o: build/ATA_BD_irx.c
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) -c $< -o $@

# Som: libsd + audsrv (PS2SDK)
build/LIBSD_irx.c:
	@mkdir -p $(@D)
	bin2c $(PS2SDK)/iop/irx/libsd.irx $@ LIBSD_irx

build/AUDSRV_irx.c:
	@mkdir -p $(@D)
	bin2c $(PS2SDK)/iop/irx/audsrv.irx $@ AUDSRV_irx

build/LIBSD_irx.o: build/LIBSD_irx.c
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) -c $< -o $@

build/AUDSRV_irx.o: build/AUDSRV_irx.c
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) -c $< -o $@

build/SMBMAN_irx.o: build/SMBMAN_irx.c
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) -c $< -o $@


# ============================================================
# CLEAN
# ============================================================

clean:
	rm -f $(EE_BIN) $(EE_OBJS)
	rm -f \
		build/DEV9_irx.c \
		build/NETMAN_irx.c \
		build/SMAP_irx.c \
		build/IOMANX_irx.c \
		build/FILEXIO_irx.c \
		build/PS2IP_irx.c \
		build/PS2IPS_irx.c \
		build/BDM_irx.c \
		build/BDMFS_FATFS_irx.c \
		build/USBD_irx.c \
		build/USBMASS_BD_irx.c \
		build/ATA_BD_irx.c \
		build/SMBMAN_irx.c


# ============================================================
# PS2SDK
# ============================================================

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal