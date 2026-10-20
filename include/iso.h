#ifndef PS2DVDPLAYER_ISO_H
#define PS2DVDPLAYER_ISO_H

#include <tamtypes.h>

#define ISO_SECTOR_SIZE 2048

/*
 * Resultado da identificação de uma ISO.
 * Cada campo só é preenchido quando lido de fato do arquivo.
 */
typedef struct
{
    int opened;            /* fileXioOpen OK */
    int open_error;        /* código de erro do open, se falhou */

    int first_read_ok;     /* leitura do setor 16 OK */
    int last_read_ok;      /* leitura do último setor OK */
    u32 total_sectors;     /* tamanho / 2048 */

    int iso9660;           /* PVD "CD001" no setor 16 */
    char volume_id[33];    /* Volume Identifier do PVD */

    int udf_anchor;        /* Anchor Volume Descriptor no setor 256 */

    int video_ts;          /* diretório VIDEO_TS encontrado */
    u32 video_ts_lba;

    int vmg_ifo;           /* VIDEO_TS.IFO com assinatura DVDVIDEO-VMG */
    int title_sets;        /* número de title sets (VMGI) */

    /*
     * DVD-Video (IFO). Preenchido só se dvd_parsed == 1.
     */
    int dvd_parsed;
    int titles;            /* títulos no TT_SRPT */
    int main_title;        /* título principal (1..n), maior duração */
    int main_vts;          /* title set do título principal */
    u32 main_seconds;      /* duração do PGC principal */
    int main_chapters;     /* capítulos (PTTs) do título principal */

    int video_mpeg2;       /* 1 = MPEG-2, 0 = MPEG-1 */
    int video_pal;         /* 1 = PAL, 0 = NTSC */
    int video_wide;        /* 1 = 16:9, 0 = 4:3 */
    int video_width;       /* 720, 704, 352 */
    int video_height;      /* 480/576, 240/288 */

    int audio_count;
    char audio[8][20];     /* ex.: "pt AC3 2ch" */

    int sub_count;
    char subs[32][4];      /* código de idioma, ex.: "pt" */

    u64 vob_bytes;         /* soma de VTS_xx_1..9.VOB do VTS principal */
    u32 vob1_lba;          /* VTS_xx_1.VOB: setor inicial na ISO */
    u32 vob1_size;
    u32 avg_kbps;          /* vob_bytes / main_seconds, em KB/s */

    /*
     * First Play: o que o disco manda fazer ao iniciar, e de onde a
     * reprodução começa (start_bytes == 0: usar vob1/vob_bytes).
     */
    char fp_desc[48];      /* ex.: "Title 1", "Disc menu (title)" */
    int fp_menu;           /* 1 se o destino é um menu */
    u32 start_lba;         /* setor inicial na ISO */
    u64 start_bytes;       /* bytes a tocar a partir dali */

    char error[96];
} IsoInfo;

int iso_identify(
    const char *path,
    u64 size,
    IsoInfo *info
);


/*
 * Teste de velocidade de leitura sequencial.
 */

#define ISO_SPEED_TESTS 5

typedef struct
{
    int block_size;        /* bytes por fileXioRead */
    u32 bytes;             /* bytes lidos de fato */
    u32 ms;                /* tempo total */
    u32 kbps;              /* KB/s */
    int error;             /* < 0 se a leitura falhou */
} IsoSpeedResult;

int iso_speed_test(
    const char *path,
    u64 size,
    IsoSpeedResult results[ISO_SPEED_TESTS]
);

#endif
