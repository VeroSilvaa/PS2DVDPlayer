#ifndef PS2DVDPLAYER_HDD_H
#define PS2DVDPLAYER_HDD_H

#include <tamtypes.h>

#define HDD_MAX_FILES 64
#define HDD_MAX_NAME  128

typedef struct
{
    char name[HDD_MAX_NAME];
    u64 size;
} HDDFile;

typedef struct
{
    int initialized;
    int mounted;
    int file_count;

    int dev9_result;
    int atad_result;
    int hdd_result;
    int pfs_result;

    HDDFile files[HDD_MAX_FILES];

    char error[128];
} HDDInfo;

/*
 * Inicializa:
 *
 * DEV9
 * ATAD
 * APA/HDD
 * PFS
 */
int hdd_init(void);

/*
 * Monta:
 *
 * hdd0:+PS2DVDPLAYER
 *
 * como:
 *
 * pfs0:
 *
 * em READ/WRITE.
 */
int hdd_mount(void);

/*
 * Garante que:
 *
 * pfs0:/DVD
 *
 * exista.
 *
 * Se não existir, cria automaticamente.
 */
int hdd_ensure_dvd_directory(void);

/*
 * Lista arquivos .iso de:
 *
 * pfs0:/DVD
 */
int hdd_list_dvd(void);

void hdd_unmount(void);
void hdd_shutdown(void);

const HDDInfo *hdd_get_info(void);

#endif