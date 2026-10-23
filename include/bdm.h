#ifndef PS2DVDPLAYER_BDM_H
#define PS2DVDPLAYER_BDM_H

#include <tamtypes.h>

/*
 * USB e HDD em FAT32/exFAT pelo BDM do PS2SDK (mesma pilha do OPL e
 * do PS2FileManager):
 *
 *   núcleo: bdm.irx + bdmfs_fatfs.irx (uma vez, compartilhado)
 *   USB:    usbd.irx + usbmass_bd.irx   -> "usb0:"
 *   HDD:    ata_bd.irx (DEV9 já carregado pela rede) -> "ata0:"
 *
 * O bdmfs_fatfs registra um nome por tipo de disco (usbN:, ataN:) e
 * também massN: para todos; usa-se o nome do tipo e, se não abrir,
 * massN: (só quando o outro tipo não foi carregado).
 *
 * HDD em APA/PFS não é suportado (decisão: só exFAT).
 */

enum { BDM_USB = 0, BDM_HDD, BDM_KINDS };

#define BDM_MAX_FILES 64
#define BDM_MAX_NAME  128

/* Pasta das ISOs de filmes na raiz da unidade. */
#define BDM_DVD_DIR "/FILMES"

typedef struct
{
    char name[BDM_MAX_NAME];
    u64 size;
} BDMFile;

typedef struct
{
    int loaded;             /* módulos carregados */
    int mounted;            /* unidade encontrada */
    char root[16];          /* ex.: "usb0:" */

    int file_count;
    BDMFile files[BDM_MAX_FILES];

    char error[128];
} BDMInfo;

/*
 * Carrega os módulos (uma vez) e espera a unidade montar
 * (até BDM_WAIT_SECONDS). 0 = unidade pronta.
 */
int bdm_init(int kind);

/*
 * Lista os .iso de <root>/FILMES. Número de ISOs ou < 0.
 */
int bdm_list_dvd(int kind);

const BDMInfo *bdm_get_info(int kind);

const char *bdm_kind_name(int kind);

#endif
