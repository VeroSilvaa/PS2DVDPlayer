#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include <tamtypes.h>
#include <kernel.h>
#include <timer.h>
#include <loadfile.h>

#define NEWLIB_PORT_AWARE
#include <io_common.h>
#include <fileXio_rpc.h>

#include "bdm.h"
#include "io_modules.h"

/*
 * A detecção do disco é assíncrona: depois de carregar o driver,
 * espera-se a unidade abrir.
 */
#define BDM_WAIT_SECONDS 5

extern unsigned char BDM_irx[];
extern unsigned int size_BDM_irx;

extern unsigned char BDMFS_FATFS_irx[];
extern unsigned int size_BDMFS_FATFS_irx;

extern unsigned char USBD_irx[];
extern unsigned int size_USBD_irx;

extern unsigned char USBMASS_BD_irx[];
extern unsigned int size_USBMASS_BD_irx;

extern unsigned char ATA_BD_irx[];
extern unsigned int size_ATA_BD_irx;

static BDMInfo bdmInfo[BDM_KINDS];
static int coreLoaded = 0;


static void bdm_set_error(int kind, const char *message, int code)
{
    if (code != 0)
        snprintf(bdmInfo[kind].error, sizeof(bdmInfo[kind].error), "%s (%d)", message, code);
    else
        snprintf(bdmInfo[kind].error, sizeof(bdmInfo[kind].error), "%s", message);

    printf("BDM %s: %s\n", bdm_kind_name(kind), bdmInfo[kind].error);
}


const char *bdm_kind_name(int kind)
{
    return kind == BDM_USB ? "USB" : "HDD";
}


static int bdm_load(const char *name, void *buffer, unsigned int size)
{
    int module_id;
    int module_result = 0;

    module_id = SifExecModuleBuffer(buffer, size, 0, NULL, &module_result);

    printf("BDM: %s -> module=%d result=%d\n", name, module_id, module_result);

    if (module_id < 0)
        return module_id;

    /* 1 = NO_RESIDENT_END: o módulo descarregou (ex.: sem disco) */
    if (module_result == 1 || module_result < 0)
        return -1;

    return 0;
}


static int bdm_core_load(void)
{
    if (coreLoaded)
        return 0;

    if (bdm_load("BDM", BDM_irx, size_BDM_irx) < 0)
        return -1;

    if (bdm_load("BDMFS_FATFS", BDMFS_FATFS_irx, size_BDMFS_FATFS_irx) < 0)
        return -2;

    coreLoaded = 1;
    return 0;
}


static int bdm_root_opens(const char *root)
{
    char path[24];
    int fd;

    snprintf(path, sizeof(path), "%s/", root);

    fd = fileXioDopen(path);

    if (fd < 0)
        return 0;

    fileXioDclose(fd);
    return 1;
}


/*
 * Procura a unidade: nome do tipo primeiro, massN: como reserva.
 */
static int bdm_find_root(int kind)
{
    static const char *own[BDM_KINDS] = { "usb", "ata" };
    char root[16];
    int other = kind == BDM_USB ? BDM_HDD : BDM_USB;
    int n;

    for (n = 0; n < 2; n++)
    {
        snprintf(root, sizeof(root), "%s%d:", own[kind], n);

        if (bdm_root_opens(root))
            goto found;
    }

    if (!bdmInfo[other].loaded)
    {
        for (n = 0; n < 2; n++)
        {
            snprintf(root, sizeof(root), "mass%d:", n);

            if (bdm_root_opens(root))
                goto found;
        }
    }

    return 0;

found:
    snprintf(bdmInfo[kind].root, sizeof(bdmInfo[kind].root), "%s", root);
    bdmInfo[kind].mounted = 1;
    return 1;
}


int bdm_init(int kind)
{
    BDMInfo *info = &bdmInfo[kind];
    u64 t0;
    int result;

    if (kind < 0 || kind >= BDM_KINDS)
        return -1;

    if (info->mounted)
        return 0;

    info->error[0] = '\0';

    printf("\n=================================\n");
    printf(" PS2DVDPlayer - %s (exFAT/FAT32)\n", bdm_kind_name(kind));
    printf("=================================\n");

    if (!info->loaded)
    {
        result = io_modules_init();

        if (result < 0)
        {
            bdm_set_error(kind, "IOMANX/FILEXIO init failed", result);
            return -2;
        }

        result = bdm_core_load();

        if (result < 0)
        {
            bdm_set_error(kind, "BDM/BDMFS_FATFS load failed", result);
            return -3;
        }

        if (kind == BDM_USB)
        {
            result = bdm_load("USBD", USBD_irx, size_USBD_irx);

            if (result == 0)
                result = bdm_load("USBMASS_BD", USBMASS_BD_irx, size_USBMASS_BD_irx);
        }
        else
        {
            /* DEV9 já foi carregado por network_init() */
            result = bdm_load("ATA_BD", ATA_BD_irx, size_ATA_BD_irx);
        }

        if (result < 0)
        {
            bdm_set_error(kind, kind == BDM_USB ? "USB driver load failed" : "ATA driver load failed (no HDD?)", result);
            return -4;
        }

        info->loaded = 1;
    }

    /* espera a unidade montar */
    t0 = GetTimerSystemTime();

    while (!bdm_find_root(kind))
    {
        u64 tick;

        if (GetTimerSystemTime() - t0 > (u64)kBUSCLK * BDM_WAIT_SECONDS)
        {
            bdm_set_error(kind, kind == BDM_USB ? "No USB drive found" : "No exFAT/FAT32 HDD found", 0);
            return -5;
        }

        tick = GetTimerSystemTime();

        while (GetTimerSystemTime() - tick < (u64)kBUSCLK / 4)
        {
        }
    }

    printf("BDM %s: mounted at %s after %u ms\n", bdm_kind_name(kind), info->root,
           (unsigned)(((GetTimerSystemTime() - t0) * 1000ULL) / kBUSCLK));

    return 0;
}


static int bdm_is_iso(const char *name)
{
    size_t len = strlen(name);

    return len >= 4 &&
           name[len - 4] == '.' &&
           tolower((unsigned char)name[len - 3]) == 'i' &&
           tolower((unsigned char)name[len - 2]) == 's' &&
           tolower((unsigned char)name[len - 1]) == 'o';
}


int bdm_list_dvd(int kind)
{
    BDMInfo *info = &bdmInfo[kind];
    iox_dirent_t entry __attribute__((aligned(64)));
    char path[48];
    int dir;
    int result;

    if (kind < 0 || kind >= BDM_KINDS || !info->mounted)
        return -1;

    info->file_count = 0;

    snprintf(path, sizeof(path), "%s" BDM_DVD_DIR, info->root);

    dir = fileXioDopen(path);

    printf("BDM %s: opening %s -> %d\n", bdm_kind_name(kind), path, dir);

    if (dir < 0)
    {
        bdm_set_error(kind, "Folder " BDM_DVD_DIR " not found", dir);
        return -2;
    }

    while ((result = fileXioDread(dir, &entry)) > 0)
    {
        BDMFile *f;

        if (FIO_S_ISDIR(entry.stat.mode) || !bdm_is_iso(entry.name))
            continue;

        if (info->file_count >= BDM_MAX_FILES)
            break;

        f = &info->files[info->file_count++];

        snprintf(f->name, sizeof(f->name), "%s", entry.name);

        /* 64 bits: ISO de DVD passa de 4 GB */
        f->size = ((u64)entry.stat.hisize << 32) | (u64)entry.stat.size;

        printf("BDM %s: ISO: %s (%llu bytes)\n", bdm_kind_name(kind), f->name,
               (unsigned long long)f->size);
    }

    fileXioDclose(dir);

    printf("BDM %s: %d ISO file(s) in %s\n", bdm_kind_name(kind), info->file_count, path);

    return info->file_count;
}


const BDMInfo *bdm_get_info(int kind)
{
    if (kind < 0 || kind >= BDM_KINDS)
        return NULL;

    return &bdmInfo[kind];
}
