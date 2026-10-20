#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <unistd.h>

#include <tamtypes.h>
#include <timer.h>

#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>
#include <io_common.h>

#include "iso.h"

/*
 * Leitor de ISO por setores (fileXio, offsets de 64 bits)
 * e identificação básica de DVD-Video:
 *
 * - setor 16:  Primary Volume Descriptor ISO9660 ("CD001")
 * - setor 256: Anchor Volume Descriptor UDF (tag 2)
 * - raiz ISO9660 -> VIDEO_TS -> VIDEO_TS.IFO ("DVDVIDEO-VMG")
 *
 * Nada aqui decodifica vídeo; apenas lê e valida estrutura.
 */

static u8 sector[ISO_SECTOR_SIZE] __attribute__((aligned(64)));

/*
 * Buffer do teste de velocidade (maior bloco testado).
 */
#define ISO_SPEED_MAX_BLOCK (128 * 1024)

static u8 speed_buffer[ISO_SPEED_MAX_BLOCK] __attribute__((aligned(64)));


static u16 rd_le16(const u8 *p)
{
    return (u16)(p[0] | (p[1] << 8));
}


static u16 rd_be16(const u8 *p)
{
    return (u16)((p[0] << 8) | p[1]);
}


static u32 rd_le32(const u8 *p)
{
    return
        (u32)p[0] |
        ((u32)p[1] << 8) |
        ((u32)p[2] << 16) |
        ((u32)p[3] << 24);
}


/* ============================================================
 * LEITURA DE SETOR
 * ============================================================ */

static int iso_read_sector(
    int fd,
    u32 lba,
    void *buffer
)
{
    s64 offset = (s64)lba * ISO_SECTOR_SIZE;
    s64 pos;
    int result;

    pos = fileXioLseek64(fd, offset, SEEK_SET);

    if (pos != offset)
    {
        printf(
            "ISO: seek to LBA %u failed (%lld)\n",
            (unsigned)lba,
            (long long)pos
        );

        return -1;
    }

    result = fileXioRead(fd, buffer, ISO_SECTOR_SIZE);

    if (result != ISO_SECTOR_SIZE)
    {
        printf(
            "ISO: read LBA %u returned %d\n",
            (unsigned)lba,
            result
        );

        return -2;
    }

    return 0;
}


/* ============================================================
 * DIRETÓRIO ISO9660
 * ============================================================ */

/*
 * Compara o nome do registro com "name", ignorando
 * maiúsculas/minúsculas e o sufixo ";1".
 */
static int iso_name_equals(
    const u8 *record,
    const char *name
)
{
    int len = record[32];
    const char *id = (const char *)&record[33];
    int i;

    for (i = 0; i < len; i++)
    {
        if (id[i] == ';')
        {
            len = i;
            break;
        }
    }

    if ((int)strlen(name) != len)
        return 0;

    return strncasecmp(id, name, len) == 0;
}


/*
 * Procura "name" no diretório (extent lba, tamanho size).
 * Retorna 1 e preenche lba/size do item se encontrar.
 */
static int iso_find_in_dir(
    int fd,
    u32 dir_lba,
    u32 dir_size,
    const char *name,
    int want_dir,
    u32 *out_lba,
    u32 *out_size
)
{
    u32 sectors = (dir_size + ISO_SECTOR_SIZE - 1) / ISO_SECTOR_SIZE;
    u32 s;

    for (s = 0; s < sectors; s++)
    {
        int pos = 0;

        if (iso_read_sector(fd, dir_lba + s, sector) < 0)
            return -1;

        while (pos < ISO_SECTOR_SIZE)
        {
            const u8 *rec = &sector[pos];
            int len = rec[0];
            int is_dir;

            /*
             * Registro de tamanho 0: resto do setor é padding.
             */
            if (len == 0)
                break;

            if (pos + len > ISO_SECTOR_SIZE || len < 34)
                break;

            is_dir = (rec[25] & 0x02) != 0;

            if (
                is_dir == want_dir &&
                iso_name_equals(rec, name)
            )
            {
                *out_lba = rd_le32(&rec[2]);
                *out_size = rd_le32(&rec[10]);
                return 1;
            }

            pos += len;
        }
    }

    return 0;
}


/* ============================================================
 * DVD-VIDEO (IFO)
 * ============================================================ */

static u8 ifo_buf[256] __attribute__((aligned(64)));


static u32 rd_be32(const u8 *p)
{
    return
        ((u32)p[0] << 24) |
        ((u32)p[1] << 16) |
        ((u32)p[2] << 8) |
        (u32)p[3];
}


static int bcd(u8 v)
{
    return ((v >> 4) * 10) + (v & 0x0F);
}


/*
 * Lê "len" bytes (<= 256) a partir do byte "offset" de um
 * arquivo que começa no setor "base_lba" da ISO.
 */
static int iso_read_bytes(
    int fd,
    u32 base_lba,
    u32 offset,
    int len
)
{
    s64 pos = (s64)base_lba * ISO_SECTOR_SIZE + offset;

    if (len > (int)sizeof(ifo_buf))
        return -1;

    if (fileXioLseek64(fd, pos, SEEK_SET) != pos)
        return -2;

    if (fileXioRead(fd, ifo_buf, len) != len)
        return -3;

    return 0;
}


static void ifo_lang(
    char *out,
    const u8 *p,
    int has_lang
)
{
    if (has_lang && p[0] >= 'a' && p[0] <= 'z')
    {
        out[0] = p[0];
        out[1] = p[1];
        out[2] = '\0';
    }
    else
    {
        strcpy(out, "--");
    }
}


static const char *audio_format(int coding)
{
    switch (coding)
    {
        case 0: return "AC3";
        case 2: return "MPEG1";
        case 3: return "MPEG2";
        case 4: return "LPCM";
        case 6: return "DTS";
        default: return "?";
    }
}


/*
 * Analisa o VTS "vts" (IFO no setor absoluto vts_lba):
 * maior PGC de título, atributos de vídeo/áudio/legenda.
 * Retorna a duração (s) do maior PGC e o número do título
 * dentro do VTS (VTS_TTN) em *vts_ttn.
 */
static int dvd_parse_vts(
    int fd,
    u32 vts_lba,
    IsoInfo *info,
    int fill_attrs,
    u32 *best_seconds,
    int *vts_ttn,
    int *best_programs
)
{
    u32 pgci_sector;
    int pgc_count;
    int i;
    u8 head[16];

    /*
     * Um título pode ser uma cadeia de PGCs (ex.: um PGC por
     * música/capítulo): somar duração e programas por VTS_TTN.
     */
    u32 ttn_seconds[128];
    int ttn_programs[128];

    memset(ttn_seconds, 0, sizeof(ttn_seconds));
    memset(ttn_programs, 0, sizeof(ttn_programs));

    *best_seconds = 0;
    *vts_ttn = 0;
    *best_programs = 0;

    if (iso_read_sector(fd, vts_lba, sector) < 0)
        return -1;

    if (memcmp(sector, "DVDVIDEO-VTS", 12) != 0)
    {
        printf(
            "ISO: VTS at LBA %u has no DVDVIDEO-VTS signature (got \"%.12s\")\n",
            (unsigned)vts_lba,
            (const char *)sector
        );
        return -2;
    }

    pgci_sector = rd_be32(&sector[0xCC]);

    if (fill_attrs)
    {
        const u8 *v = &sector[0x200];
        int res = (v[1] >> 3) & 0x07;
        int audio_n = rd_be16(&sector[0x202]);
        int sub_n = rd_be16(&sector[0x254]);

        info->video_mpeg2 = ((v[0] >> 6) & 0x03) == 1;
        info->video_pal = ((v[0] >> 4) & 0x03) == 1;
        info->video_wide = ((v[0] >> 2) & 0x03) == 3;

        info->video_width =
            res == 0 ? 720 :
            res == 1 ? 704 : 352;

        info->video_height =
            (res == 3)
                ? (info->video_pal ? 288 : 240)
                : (info->video_pal ? 576 : 480);

        if (audio_n > 8)
            audio_n = 8;

        info->audio_count = audio_n;

        for (i = 0; i < audio_n; i++)
        {
            const u8 *a = &sector[0x204 + i * 8];
            char lang[3];

            ifo_lang(lang, &a[2], ((a[0] >> 2) & 0x03) == 1);

            snprintf(
                info->audio[i],
                sizeof(info->audio[i]),
                "%s %s %dch",
                lang,
                audio_format((a[0] >> 5) & 0x07),
                (a[1] & 0x07) + 1
            );
        }

        if (sub_n > 32)
            sub_n = 32;

        info->sub_count = sub_n;

        for (i = 0; i < sub_n; i++)
        {
            const u8 *sp = &sector[0x256 + i * 6];

            ifo_lang(info->subs[i], &sp[2], (sp[0] & 0x03) == 1);
        }
    }

    /*
     * VTS_PGCI: cabeçalho (nº de PGCs) + tabela de 8 bytes/PGC.
     */
    if (iso_read_bytes(fd, vts_lba + pgci_sector, 0, 8) < 0)
        return -3;

    pgc_count = rd_be16(&ifo_buf[0]);

    for (i = 0; i < pgc_count && i < 255; i++)
    {
        u8 category;
        u32 pgc_offset;
        u32 seconds;

        if (iso_read_bytes(fd, vts_lba + pgci_sector, 8 + i * 8, 8) < 0)
            return -4;

        category = ifo_buf[0];
        pgc_offset = rd_be32(&ifo_buf[4]);

        if (iso_read_bytes(fd, vts_lba + pgci_sector, pgc_offset, 8) < 0)
            return -5;

        memcpy(head, ifo_buf, 8);

        seconds =
            bcd(head[4]) * 3600 +
            bcd(head[5]) * 60 +
            bcd(head[6]);

        printf(
            "ISO:   PGC %d: ttn=%d entry=%d programs=%d cells=%d time=%02d:%02d:%02d\n",
            i + 1,
            category & 0x7F,
            (category & 0x80) ? 1 : 0,
            head[2],
            head[3],
            bcd(head[4]),
            bcd(head[5]),
            bcd(head[6])
        );

        ttn_seconds[category & 0x7F] += seconds;
        ttn_programs[category & 0x7F] += head[2];
    }

    for (i = 1; i < 128; i++)
    {
        if (ttn_seconds[i] > *best_seconds)
        {
            *best_seconds = ttn_seconds[i];
            *vts_ttn = i;
            *best_programs = ttn_programs[i];
        }
    }

    if (*vts_ttn > 0)
        printf(
            "ISO:   VTS title %d total %02u:%02u:%02u\n",
            *vts_ttn,
            (unsigned)(*best_seconds / 3600),
            (unsigned)((*best_seconds / 60) % 60),
            (unsigned)(*best_seconds % 60)
        );

    return 0;
}


/* ============================================================
 * FIRST PLAY
 * ============================================================ */

/*
 * Posição e tamanho total dos VOBs de conteúdo de um VTS
 * (VTS_xx_1..9.VOB) ou dos menus (VTS_xx_0.VOB / VIDEO_TS.VOB),
 * pela posição real no diretório.
 */
static int dvd_vobs(
    int fd,
    u32 dir_lba,
    u32 dir_size,
    int vts,
    int menu,
    u32 *out_lba,
    u64 *out_bytes
)
{
    char name[16];
    u32 lba;
    u32 size;
    int v;

    *out_lba = 0;
    *out_bytes = 0;

    if (menu)
    {
        if (vts == 0)
            snprintf(name, sizeof(name), "VIDEO_TS.VOB");
        else
            snprintf(name, sizeof(name), "VTS_%02d_0.VOB", vts);

        if (iso_find_in_dir(fd, dir_lba, dir_size, name, 0, &lba, &size) <= 0)
            return -1;

        *out_lba = lba;
        *out_bytes = size;
        return 0;
    }

    for (v = 1; v <= 9; v++)
    {
        snprintf(name, sizeof(name), "VTS_%02d_%d.VOB", vts, v);

        if (iso_find_in_dir(fd, dir_lba, dir_size, name, 0, &lba, &size) <= 0)
            break;

        if (v == 1)
            *out_lba = lba;

        *out_bytes += size;
    }

    return *out_lba ? 0 : -1;
}


/*
 * Lê um PGC (no arquivo IFO que começa em ifo_lba, byte pgc_off) e
 * devolve o setor inicial da célula "entry_cell" (1..n) e o setor
 * final da última célula, relativos ao início do conjunto de VOBs.
 */
static int dvd_pgc_cells(
    int fd,
    u32 ifo_lba,
    u32 pgc_off,
    int entry_cell,
    u32 *first,
    u32 *last,
    int *ncells
)
{
    u32 cell_off;
    int cells;

    if (iso_read_bytes(fd, ifo_lba, pgc_off, 8) < 0)
        return -1;

    cells = ifo_buf[3];
    *ncells = cells;

    if (cells == 0)
        return -2;

    if (entry_cell < 1 || entry_cell > cells)
        entry_cell = 1;

    if (iso_read_bytes(fd, ifo_lba, pgc_off + 0xE8, 2) < 0)
        return -3;

    cell_off = rd_be16(&ifo_buf[0]);

    /*
     * Cell playback info: 24 bytes por célula;
     * 8-11 primeiro setor, 20-23 último setor.
     */
    if (iso_read_bytes(fd, ifo_lba, pgc_off + cell_off + (entry_cell - 1) * 24, 24) < 0)
        return -4;

    *first = rd_be32(&ifo_buf[8]);

    if (iso_read_bytes(fd, ifo_lba, pgc_off + cell_off + (cells - 1) * 24, 24) < 0)
        return -5;

    *last = rd_be32(&ifo_buf[20]);

    return 0;
}


/*
 * PGC de menu na tabela PGCI_UT (ponteiro de setor em ifo_lba +
 * ut_ptr_off): por tipo de menu (menu_id, 2=título, 3=raiz...) ou
 * por número (pgcn > 0). Usa a primeira unidade de idioma.
 * Devolve o byte do PGC dentro do IFO.
 */
static int dvd_menu_pgc(
    int fd,
    u32 ifo_lba,
    u32 ut_ptr_off,
    int menu_id,
    int pgcn,
    u32 *pgc_off
)
{
    u32 ut;
    u32 lu;
    int n;
    int i;

    if (iso_read_bytes(fd, ifo_lba, ut_ptr_off, 4) < 0)
        return -1;

    ut = rd_be32(&ifo_buf[0]);

    if (ut == 0)
        return -2;

    /*
     * PGCI_UT: cabeçalho de 8 bytes; LU de 8 bytes:
     * idioma(2), ext(1), menus(1), offset da LU(4).
     */
    if (iso_read_bytes(fd, ifo_lba + ut, 8, 8) < 0)
        return -3;

    lu = rd_be32(&ifo_buf[4]);

    if (iso_read_bytes(fd, ifo_lba + ut, lu, 8) < 0)
        return -4;

    n = rd_be16(&ifo_buf[0]);

    for (i = 0; i < n && i < 255; i++)
    {
        u8 category;
        u32 off;

        if (iso_read_bytes(fd, ifo_lba + ut, lu + 8 + i * 8, 8) < 0)
            return -5;

        category = ifo_buf[0];
        off = rd_be32(&ifo_buf[4]);

        if ((pgcn > 0 && i + 1 == pgcn) ||
            (pgcn == 0 && (category & 0x80) && (category & 0x0F) == menu_id))
        {
            *pgc_off = ut * ISO_SECTOR_SIZE + lu + off;
            return 0;
        }
    }

    return -6;
}


static const char *menu_name(int id)
{
    switch (id)
    {
        case 2: return "title";
        case 3: return "root";
        case 4: return "subpicture";
        case 5: return "audio";
        case 6: return "angle";
        case 7: return "chapter";
        default: return "?";
    }
}


/*
 * Lê o First Play PGC (VMGI 0x84), registra seus comandos e resolve
 * o primeiro salto: título (JumpTT) ou menu (JumpSS/CallSS). Comandos
 * condicionais e registradores ficam para a navegação completa.
 */
static void dvd_first_play(
    int fd,
    IsoInfo *info,
    u32 vmg_lba,
    u32 dir_lba,
    u32 dir_size,
    u32 tt_srpt,
    const u32 *vts_sector
)
{
    u32 fp_off;
    u32 cmd_off;
    int pre;
    int i;
    int cells;
    u32 first = 0;
    u32 last = 0;
    u32 base_lba = 0;
    u64 base_bytes = 0;
    int resolved = 0;

    strcpy(info->fp_desc, "unknown");

    if (iso_read_bytes(fd, vmg_lba, 0x84, 4) < 0)
        return;

    fp_off = rd_be32(&ifo_buf[0]);

    if (fp_off == 0)
    {
        strcpy(info->fp_desc, "none");
        printf("ISO: First Play PGC: none\n");
        return;
    }

    if (iso_read_bytes(fd, vmg_lba, fp_off, 8) < 0)
        return;

    cells = ifo_buf[3];

    if (iso_read_bytes(fd, vmg_lba, fp_off + 0xE4, 2) < 0)
        return;

    cmd_off = rd_be16(&ifo_buf[0]);
    pre = 0;

    if (cmd_off && iso_read_bytes(fd, vmg_lba, fp_off + cmd_off, 8) >= 0)
        pre = rd_be16(&ifo_buf[0]);

    printf("ISO: First Play PGC at byte %u: %d cells, %d pre commands\n",
           (unsigned)fp_off, cells, pre);

    for (i = 0; i < pre && i < 32; i++)
    {
        u8 c[8];

        if (iso_read_bytes(fd, vmg_lba, fp_off + cmd_off + 8 + i * 8, 8) < 0)
            break;

        memcpy(c, ifo_buf, 8);

        printf("ISO:   FP cmd %d: %02x %02x %02x %02x %02x %02x %02x %02x\n",
               i + 1, c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7]);

        if (resolved || c[0] != 0x30)
            continue;

        /*
         * JumpTT: título em c[5].
         */
        if ((c[1] & 0x0F) == 0x02)
        {
            int title = c[5] & 0x7F;
            int vts;
            int ttn;
            u32 vts_lba;
            u32 ptt;
            u32 off;
            int pgcn;
            int pgn;
            u32 pgci;
            u32 pgc_off;
            u32 map;
            int entry;

            snprintf(info->fp_desc, sizeof(info->fp_desc), "Title %d", title);

            if (title < 1 || iso_read_bytes(fd, vmg_lba + tt_srpt, 8 + (title - 1) * 12, 12) < 0)
                break;

            vts = ifo_buf[6];
            ttn = ifo_buf[7];

            if (vts < 1 || vts > 99)
                break;

            vts_lba = vts_sector[vts];

            /*
             * VTS_PTT_SRPT (0xC8): capítulo 1 do título -> PGCN, PGN.
             */
            if (iso_read_bytes(fd, vts_lba, 0xC8, 4) < 0)
                break;

            ptt = rd_be32(&ifo_buf[0]);

            if (iso_read_bytes(fd, vts_lba + ptt, 8 + (ttn - 1) * 4, 4) < 0)
                break;

            off = rd_be32(&ifo_buf[0]);

            if (iso_read_bytes(fd, vts_lba + ptt, off, 4) < 0)
                break;

            pgcn = rd_be16(&ifo_buf[0]);
            pgn = rd_be16(&ifo_buf[2]);

            /*
             * VTS_PGCI (0xCC) -> PGC; mapa de programas (0xE6) dá a
             * célula de entrada do programa.
             */
            if (iso_read_bytes(fd, vts_lba, 0xCC, 4) < 0)
                break;

            pgci = rd_be32(&ifo_buf[0]);

            if (iso_read_bytes(fd, vts_lba + pgci, 8 + (pgcn - 1) * 8 + 4, 4) < 0)
                break;

            pgc_off = pgci * ISO_SECTOR_SIZE + rd_be32(&ifo_buf[0]);

            if (iso_read_bytes(fd, vts_lba, pgc_off + 0xE6, 2) < 0)
                break;

            map = rd_be16(&ifo_buf[0]);

            if (iso_read_bytes(fd, vts_lba, pgc_off + map + pgn - 1, 1) < 0)
                break;

            entry = ifo_buf[0];

            if (dvd_pgc_cells(fd, vts_lba, pgc_off, entry, &first, &last, &cells) < 0)
                break;

            if (dvd_vobs(fd, dir_lba, dir_size, vts, 0, &base_lba, &base_bytes) < 0)
                break;

            printf("ISO: FP -> JumpTT %d: VTS %d ttn %d, PGC %d, program %d, cell %d, sector %u\n",
                   title, vts, ttn, pgcn, pgn, entry, (unsigned)first);

            /*
             * Toca a partir do capítulo 1 até o fim dos VOBs do VTS
             * (títulos de vários PGCs encadeados, como shows).
             */
            info->fp_menu = 0;
            info->start_lba = base_lba + first;
            info->start_bytes = base_bytes - (u64)first * ISO_SECTOR_SIZE;
            resolved = 1;
        }
        /*
         * JumpSS (06) / CallSS (08) para menu do VMG ou do VTS.
         */
        else if ((c[1] & 0x0F) == 0x06 || (c[1] & 0x0F) == 0x08)
        {
            int call = (c[1] & 0x0F) == 0x08;
            int type = c[5] >> 6;
            int menu_id = c[5] & 0x0F;
            int vmgm = 0;
            int vts = 0;
            int pgcn = 0;
            u32 ifo_lba = vmg_lba;
            u32 ut_ptr = 0xC8;    /* VMGM_PGCI_UT */
            u32 pgc_off;

            if (type == 0)
            {
                snprintf(info->fp_desc, sizeof(info->fp_desc), "First Play (loop)");
                continue;
            }

            /*
             * Tipo (bits 23-22 = c[5] >> 6), como na libdvdnav:
             * JumpSS: 1 = menu VMGM, 2 = menu VTSM (VTS c[4], TTN c[3]),
             *         3 = PGC do VMGM por número (c[2..3], 15 bits).
             * CallSS: 1 = menu VMGM, 2 = menu VTSM, 3 = PGC do VMGM.
             */
            if (!call)
            {
                if (type == 1)
                    vmgm = 1;
                else if (type == 3)
                {
                    vmgm = 1;
                    pgcn = ((c[2] & 0x7F) << 8) | c[3];
                }
                else
                {
                    vts = c[4];
                }
            }
            else
            {
                if (type == 1)
                    vmgm = 1;
                else if (type == 3)
                {
                    vmgm = 1;
                    pgcn = ((c[2] & 0x7F) << 8) | c[3];
                }
                else
                    vts = 1;    /* CallSS VTSM: VTS atual (1 no início) */
            }

            if (!vmgm)
            {
                if (vts < 1 || vts > 99)
                    vts = 1;

                ifo_lba = vts_sector[vts];
                ut_ptr = 0xD0;    /* VTSM_PGCI_UT */
            }

            if (pgcn)
                snprintf(info->fp_desc, sizeof(info->fp_desc), "%s menu PGC %d",
                         vmgm ? "Disc" : "Title set", pgcn);
            else
                snprintf(info->fp_desc, sizeof(info->fp_desc), "%s menu (%s)",
                         vmgm ? "Disc" : "Title set", menu_name(menu_id));

            if (dvd_menu_pgc(fd, ifo_lba, ut_ptr, menu_id, pgcn, &pgc_off) < 0)
            {
                printf("ISO: FP -> menu not found in PGCI_UT\n");
                break;
            }

            if (dvd_pgc_cells(fd, ifo_lba, pgc_off, 1, &first, &last, &cells) < 0)
                break;

            if (dvd_vobs(fd, dir_lba, dir_size, vmgm ? 0 : vts, 1, &base_lba, &base_bytes) < 0)
                break;

            printf("ISO: FP -> %s %s: %d cells, sectors %u-%u\n",
                   call ? "CallSS" : "JumpSS",
                   info->fp_desc, cells, (unsigned)first, (unsigned)last);

            info->fp_menu = 1;
            info->start_lba = base_lba + first;
            info->start_bytes = (u64)(last - first + 1) * ISO_SECTOR_SIZE;
            resolved = 1;
        }
    }

    /*
     * First Play com células próprias (vídeo no VIDEO_TS.VOB).
     */
    if (!resolved && cells > 0 &&
        dvd_pgc_cells(fd, vmg_lba, fp_off, 1, &first, &last, &cells) >= 0 &&
        dvd_vobs(fd, dir_lba, dir_size, 0, 1, &base_lba, &base_bytes) >= 0)
    {
        strcpy(info->fp_desc, "First Play video");
        info->fp_menu = 0;
        info->start_lba = base_lba + first;
        info->start_bytes = (u64)(last - first + 1) * ISO_SECTOR_SIZE;
        resolved = 1;
    }

    if (resolved)
        printf("ISO: start at LBA %u, %llu bytes (%s)\n",
               (unsigned)info->start_lba, (unsigned long long)info->start_bytes, info->fp_desc);
    else
        printf("ISO: First Play not resolved (%s), using main title\n", info->fp_desc);
}


static int dvd_parse(
    int fd,
    IsoInfo *info,
    u32 vmg_lba,
    u32 vts_dir_lba,
    u32 vts_dir_size
)
{
    u32 tt_srpt;
    u8 vts_seen[100];
    u32 vts_sector[100];
    int t;
    int v;
    int n;

    memset(vts_seen, 0, sizeof(vts_seen));

    /*
     * VMGI_MAT 0xC4: setor do TT_SRPT (relativo ao IFO).
     */
    if (iso_read_bytes(fd, vmg_lba, 0xC4, 4) < 0)
        return -1;

    tt_srpt = rd_be32(&ifo_buf[0]);

    if (iso_read_bytes(fd, vmg_lba + tt_srpt, 0, 8) < 0)
        return -2;

    n = rd_be16(&ifo_buf[0]);

    if (n > 99)
        n = 99;

    info->titles = n;

    printf("ISO: TT_SRPT at sector %u: %d titles\n", (unsigned)tt_srpt, n);

    /*
     * Entradas de 12 bytes:
     * 2-3 capítulos, 6 VTS, 7 VTS_TTN, 8-11 setor do VTS.
     */
    for (t = 0; t < n; t++)
    {
        int vts;

        if (iso_read_bytes(fd, vmg_lba + tt_srpt, 8 + t * 12, 12) < 0)
            return -3;

        vts = ifo_buf[6];

        printf(
            "ISO: title %d: VTS %d ttn %d chapters %d sector %u\n",
            t + 1,
            vts,
            ifo_buf[7],
            rd_be16(&ifo_buf[2]),
            (unsigned)rd_be32(&ifo_buf[8])
        );

        if (vts > 0 && vts < 100)
        {
            vts_seen[vts] = 1;
            vts_sector[vts] = rd_be32(&ifo_buf[8]);
        }
    }

    /*
     * Maior PGC entre todos os VTS = título principal.
     */
    {
        u32 best = 0;
        int best_vts = 0;
        int best_ttn = 0;
        int best_programs = 0;

        for (v = 1; v < 100; v++)
        {
            u32 seconds;
            int ttn;
            int programs;

            if (!vts_seen[v])
                continue;

            /*
             * O setor do TT_SRPT pode estar desatualizado em ISOs
             * regravadas: usar a posição real de VTS_xx_0.IFO no
             * diretório VIDEO_TS quando existir.
             */
            {
                char name[16];
                u32 lba;
                u32 size;

                snprintf(name, sizeof(name), "VTS_%02d_0.IFO", v);

                if (iso_find_in_dir(fd, vts_dir_lba, vts_dir_size, name, 0, &lba, &size) > 0)
                {
                    if (lba != vts_sector[v])
                        printf(
                            "ISO: %s at LBA %u (TT_SRPT says %u)\n",
                            name,
                            (unsigned)lba,
                            (unsigned)vts_sector[v]
                        );

                    vts_sector[v] = lba;
                }
                else
                {
                    printf("ISO: %s not found in VIDEO_TS, using TT_SRPT\n", name);
                }
            }

            printf("ISO: VTS %d at LBA %u\n", v, (unsigned)vts_sector[v]);

            if (dvd_parse_vts(fd, vts_sector[v], info, 0, &seconds, &ttn, &programs) < 0)
                continue;

            if (seconds > best)
            {
                best = seconds;
                best_vts = v;
                best_ttn = ttn;
                best_programs = programs;
            }
        }

        if (best_vts == 0)
            return -4;

        /*
         * Atributos do VTS principal.
         */
        dvd_parse_vts(fd, vts_sector[best_vts], info, 1, &best, &best_ttn, &best_programs);

        info->main_vts = best_vts;
        info->main_seconds = best;
        info->main_chapters = best_programs;

        /*
         * Número global do título e capítulos via TT_SRPT.
         */
        for (t = 0; t < n; t++)
        {
            if (iso_read_bytes(fd, vmg_lba + tt_srpt, 8 + t * 12, 12) < 0)
                break;

            if (ifo_buf[6] == best_vts && ifo_buf[7] == best_ttn)
            {
                info->main_title = t + 1;
                info->main_chapters = rd_be16(&ifo_buf[2]);
                break;
            }
        }
    }

    /*
     * Tamanho dos VOBs do título: VTS_xx_1.VOB .. VTS_xx_9.VOB.
     */
    for (v = 1; v <= 9; v++)
    {
        static u32 next_lba;
        char name[16];
        u32 lba;
        u32 size;

        snprintf(name, sizeof(name), "VTS_%02d_%d.VOB", info->main_vts, v);

        if (iso_find_in_dir(fd, vts_dir_lba, vts_dir_size, name, 0, &lba, &size) <= 0)
            break;

        /*
         * O player lê os VOBs do título como um trecho contínuo da
         * ISO a partir do VTS_xx_1.VOB: avisar se não forem contíguos.
         */
        if (v > 1 && lba != next_lba)
            printf("ISO: WARNING %s at LBA %u, expected %u (VOBs not contiguous)\n",
                   name, (unsigned)lba, (unsigned)next_lba);

        next_lba = lba + (size + ISO_SECTOR_SIZE - 1) / ISO_SECTOR_SIZE;

        info->vob_bytes += size;

        if (v == 1)
        {
            info->vob1_lba = lba;
            info->vob1_size = size;
        }

        printf("ISO: %s LBA %u, %u bytes\n", name, (unsigned)lba, (unsigned)size);
    }

    if (info->main_seconds > 0)
        info->avg_kbps = (u32)(info->vob_bytes / 1024ULL / info->main_seconds);

    info->dvd_parsed = 1;

    dvd_first_play(fd, info, vmg_lba, vts_dir_lba, vts_dir_size, tt_srpt, vts_sector);

    printf(
        "ISO: main title %d (VTS %d): %02u:%02u:%02u, %d chapters\n",
        info->main_title,
        info->main_vts,
        (unsigned)(info->main_seconds / 3600),
        (unsigned)((info->main_seconds / 60) % 60),
        (unsigned)(info->main_seconds % 60),
        info->main_chapters
    );

    printf(
        "ISO: video %s %s %s %dx%d\n",
        info->video_mpeg2 ? "MPEG-2" : "MPEG-1",
        info->video_pal ? "PAL" : "NTSC",
        info->video_wide ? "16:9" : "4:3",
        info->video_width,
        info->video_height
    );

    for (t = 0; t < info->audio_count; t++)
        printf("ISO: audio %d: %s\n", t + 1, info->audio[t]);

    for (t = 0; t < info->sub_count; t++)
        printf("ISO: subtitle %d: %s\n", t + 1, info->subs[t]);

    printf(
        "ISO: title VOBs %llu bytes -> average %u KB/s\n",
        (unsigned long long)info->vob_bytes,
        (unsigned)info->avg_kbps
    );

    return 0;
}


/* ============================================================
 * IDENTIFICAÇÃO
 * ============================================================ */

int iso_identify(
    const char *path,
    u64 size,
    IsoInfo *info
)
{
    int fd;
    u32 root_lba;
    u32 root_size;
    u32 vts_lba;
    u32 vts_size;
    u32 ifo_lba;
    u32 ifo_size;
    int found;
    int i;

    memset(info, 0, sizeof(*info));

    info->total_sectors = (u32)(size / ISO_SECTOR_SIZE);

    printf("\n");
    printf("==============================\n");
    printf(" ISO IDENTIFY\n");
    printf("==============================\n");
    printf("ISO: path=%s\n", path);
    printf(
        "ISO: size=%llu bytes, sectors=%u\n",
        (unsigned long long)size,
        (unsigned)info->total_sectors
    );


    /*
     * fileXioOpen() repassa as flags direto ao iomanX do IOP:
     * usar FIO_O_RDONLY (0x0001). O O_RDONLY do newlib vale 0
     * e abre o arquivo sem permissao de leitura (read retorna 0).
     */
    fd = fileXioOpen(path, FIO_O_RDONLY);

    printf("ISO: fileXioOpen returned %d\n", fd);

    if (fd < 0)
    {
        info->open_error = fd;

        snprintf(
            info->error,
            sizeof(info->error),
            "OPEN failed (%d)",
            fd
        );

        return -1;
    }

    info->opened = 1;


    /*
     * Último setor: valida seek/leitura perto do fim do arquivo.
     */
    if (info->total_sectors > 0)
    {
        if (iso_read_sector(fd, info->total_sectors - 1, sector) == 0)
            info->last_read_ok = 1;

        printf(
            "ISO: last sector (LBA %u) read %s\n",
            (unsigned)(info->total_sectors - 1),
            info->last_read_ok ? "OK" : "FAILED"
        );
    }


    /*
     * UDF: Anchor Volume Descriptor Pointer no setor 256.
     */
    if (iso_read_sector(fd, 256, sector) == 0)
    {
        if (rd_le16(&sector[0]) == 2)
            info->udf_anchor = 1;
    }

    printf("ISO: UDF anchor %s\n", info->udf_anchor ? "found" : "not found");


    /*
     * ISO9660: Primary Volume Descriptor no setor 16.
     */
    if (iso_read_sector(fd, 16, sector) < 0)
    {
        snprintf(
            info->error,
            sizeof(info->error),
            "READ sector 16 failed"
        );

        fileXioClose(fd);
        return -2;
    }

    info->first_read_ok = 1;

    if (
        sector[0] != 1 ||
        memcmp(&sector[1], "CD001", 5) != 0
    )
    {
        printf("ISO: no ISO9660 PVD at sector 16\n");

        snprintf(
            info->error,
            sizeof(info->error),
            info->udf_anchor
                ? "UDF only (no ISO9660)"
                : "Not an ISO9660/UDF image"
        );

        fileXioClose(fd);
        return -3;
    }

    info->iso9660 = 1;

    memcpy(info->volume_id, &sector[40], 32);
    info->volume_id[32] = '\0';

    for (i = 31; i >= 0 && info->volume_id[i] == ' '; i--)
        info->volume_id[i] = '\0';

    root_lba = rd_le32(&sector[156 + 2]);
    root_size = rd_le32(&sector[156 + 10]);

    printf("ISO: volume id \"%s\"\n", info->volume_id);
    printf(
        "ISO: root dir LBA %u size %u\n",
        (unsigned)root_lba,
        (unsigned)root_size
    );


    /*
     * VIDEO_TS
     */
    found = iso_find_in_dir(
        fd,
        root_lba,
        root_size,
        "VIDEO_TS",
        1,
        &vts_lba,
        &vts_size
    );

    if (found <= 0)
    {
        printf("ISO: VIDEO_TS not found\n");

        snprintf(
            info->error,
            sizeof(info->error),
            "VIDEO_TS not found"
        );

        fileXioClose(fd);
        return -4;
    }

    info->video_ts = 1;
    info->video_ts_lba = vts_lba;

    printf("ISO: VIDEO_TS at LBA %u\n", (unsigned)vts_lba);


    /*
     * VIDEO_TS.IFO
     */
    found = iso_find_in_dir(
        fd,
        vts_lba,
        vts_size,
        "VIDEO_TS.IFO",
        0,
        &ifo_lba,
        &ifo_size
    );

    if (found <= 0)
    {
        printf("ISO: VIDEO_TS.IFO not found\n");

        snprintf(
            info->error,
            sizeof(info->error),
            "VIDEO_TS.IFO not found"
        );

        fileXioClose(fd);
        return -5;
    }

    if (iso_read_sector(fd, ifo_lba, sector) < 0)
    {
        snprintf(
            info->error,
            sizeof(info->error),
            "READ VIDEO_TS.IFO failed"
        );

        fileXioClose(fd);
        return -6;
    }

    if (memcmp(sector, "DVDVIDEO-VMG", 12) != 0)
    {
        printf("ISO: VIDEO_TS.IFO has no DVDVIDEO-VMG signature\n");

        snprintf(
            info->error,
            sizeof(info->error),
            "Invalid VIDEO_TS.IFO"
        );

        fileXioClose(fd);
        return -7;
    }

    info->vmg_ifo = 1;

    /*
     * VMGI_MAT: número de title sets em 0x3E (big-endian).
     */
    info->title_sets = rd_be16(&sector[0x3E]);

    printf(
        "ISO: DVDVIDEO-VMG OK, title sets=%d\n",
        info->title_sets
    );


    /*
     * Títulos, duração, áudio, legendas, taxa média.
     * Falha aqui não invalida a identificação.
     */
    if (dvd_parse(fd, info, ifo_lba, vts_lba, vts_size) < 0)
        printf("ISO: DVD-Video details not available\n");


    fileXioClose(fd);

    printf("ISO: DVD-Video identified\n");

    return 0;
}


/* ============================================================
 * TESTE DE VELOCIDADE
 * ============================================================ */

/*
 * Cada bloco: lê até "limit" bytes ou até ~8 s, o que vier
 * primeiro. Cada teste começa num offset diferente para não
 * reaproveitar cache do servidor entre os testes.
 */

static const int speed_blocks[ISO_SPEED_TESTS] =
{
    2 * 1024,
    16 * 1024,
    32 * 1024,
    64 * 1024,
    128 * 1024
};

static const u32 speed_limits[ISO_SPEED_TESTS] =
{
    512 * 1024,
    2 * 1024 * 1024,
    2 * 1024 * 1024,
    4 * 1024 * 1024,
    4 * 1024 * 1024
};

#define ISO_SPEED_MAX_MS 8000


static u32 ticks_to_ms(u64 ticks)
{
    return (u32)((ticks * 1000ULL) / kBUSCLK);
}


int iso_speed_test(
    const char *path,
    u64 size,
    IsoSpeedResult results[ISO_SPEED_TESTS]
)
{
    int fd;
    int t;

    memset(results, 0, sizeof(IsoSpeedResult) * ISO_SPEED_TESTS);

    printf("\n");
    printf("==============================\n");
    printf(" ISO SPEED TEST\n");
    printf("==============================\n");
    printf("ISO: path=%s\n", path);

    /*
     * Hipotese: o fileXio no IOP quebra cada leitura em pedacos
     * do seu buffer RW (16 KB por padrao), limitando a vazao
     * a ~720 KB/s acima de 16 KB. Aumenta o buffer para o maior
     * bloco testado.
     */
    {
        int rw = fileXioSetRWBufferSize(ISO_SPEED_MAX_BLOCK);

        printf(
            "ISO: fileXioSetRWBufferSize(%d) returned %d\n",
            ISO_SPEED_MAX_BLOCK,
            rw
        );
    }

    fd = fileXioOpen(path, FIO_O_RDONLY);

    printf("ISO: fileXioOpen returned %d\n", fd);

    if (fd < 0)
        return fd;

    for (t = 0; t < ISO_SPEED_TESTS; t++)
    {
        IsoSpeedResult *r = &results[t];
        int block = speed_blocks[t];
        u32 limit = speed_limits[t];
        s64 offset;
        u64 start;
        u64 now;

        r->block_size = block;

        /*
         * Offsets a partir de 256 MiB, 256 MiB entre testes.
         */
        offset = (s64)(256 + t * 256) * 1024 * 1024;

        if ((u64)offset + limit > size)
            offset = 0;

        if (fileXioLseek64(fd, offset, SEEK_SET) != offset)
        {
            r->error = -1;
            printf("ISO: speed %d KB: seek failed\n", block / 1024);
            continue;
        }

        start = GetTimerSystemTime();
        now = start;

        while (r->bytes < limit)
        {
            int result = fileXioRead(fd, speed_buffer, block);

            if (result != block)
            {
                r->error = result < 0 ? result : -2;
                printf(
                    "ISO: speed %d KB: read returned %d\n",
                    block / 1024,
                    result
                );
                break;
            }

            r->bytes += block;

            now = GetTimerSystemTime();

            if (ticks_to_ms(now - start) >= ISO_SPEED_MAX_MS)
                break;
        }

        r->ms = ticks_to_ms(now - start);

        if (r->ms > 0)
            r->kbps = (u32)(((u64)r->bytes * 1000ULL) / ((u64)r->ms * 1024ULL));

        printf(
            "ISO: speed block=%3d KB  bytes=%u  ms=%u  -> %u KB/s%s\n",
            block / 1024,
            (unsigned)r->bytes,
            (unsigned)r->ms,
            (unsigned)r->kbps,
            r->error ? "  (ERROR)" : ""
        );
    }

    fileXioClose(fd);

    printf("ISO: speed test done\n");

    return 0;
}
