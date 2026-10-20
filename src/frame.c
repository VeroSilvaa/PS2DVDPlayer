#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>

#include <tamtypes.h>
#include <kernel.h>
#include <timer.h>
#include <dma.h>
#include <libmpeg.h>

#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>
#include <io_common.h>

#include "iso.h"
#include "frame.h"
#include "nav.h"
#include "spu.h"
#include "audio.h"

/*
 * libmpeg local (src/libmpeg): com a CSC em software, o quadro sai
 * já em RGBA 16 bits linear no buffer indicado (largura = stride).
 */
extern void MPEG_SetDirectCT16(unsigned short *dst, int stride);
extern int MPEG_DirectCT16(void);
extern int MPEG_AtSequenceStart(void);
extern void MPEG_StopInput(void);
extern void MPEG_SetSkipCSC(int skip);
extern void MPEG_SetPlanar(unsigned char *y, unsigned char *cb, unsigned char *cr, int stride);

/*
 * Planos Y / Cb / Cr do último quadro (FRAME_GS_CSC): a conversão
 * para RGB é feita pela GS (main.c). Alocados uma vez.
 */
static u8 *plane_y = NULL;
static u8 *plane_cb = NULL;
static u8 *plane_cr = NULL;


static int planes_alloc(void)
{
    if (!FRAME_GS_CSC || plane_y)
        return 0;

    plane_y = memalign(128, FRAME_MAX_WIDTH * FRAME_MAX_HEIGHT);
    plane_cb = memalign(128, (FRAME_MAX_WIDTH / 2) * (FRAME_MAX_HEIGHT / 2));
    plane_cr = memalign(128, (FRAME_MAX_WIDTH / 2) * (FRAME_MAX_HEIGHT / 2));

    if (!plane_y || !plane_cb || !plane_cr)
        return -1;

    memset(plane_y, 16, FRAME_MAX_WIDTH * FRAME_MAX_HEIGHT);
    memset(plane_cb, 128, (FRAME_MAX_WIDTH / 2) * (FRAME_MAX_HEIGHT / 2));
    memset(plane_cr, 128, (FRAME_MAX_WIDTH / 2) * (FRAME_MAX_HEIGHT / 2));
    return 0;
}


void frame_planes(const u8 **y, const u8 **cb, const u8 **cr)
{
    *y = plane_y;
    *cb = plane_cb;
    *cr = plane_cr;
}

/*
 * Leitura: blocos de 128 KB (64 packs de 2048), assíncronos.
 * Buffer circular de vídeo elementar: 4 MB (~4 s a ~1 MB/s).
 */
#define FRAME_READ_BLOCK  (128 * 1024)
#define FRAME_RING_SIZE   (4 * 1024 * 1024)
#define FRAME_DMA_CHUNK   2048

/*
 * Espaço mantido livre atrás da leitura do IPU: um bloco enviado por
 * DMA ainda pode estar em uso e a libmpeg pode reinserir alguns
 * quadwords anteriores ao retomar o fluxo.
 */
#define FRAME_RING_GUARD  (64 * 1024)

/*
 * Mínimo no buffer para decodificar sem esperar a rede
 * (um quadro I de DVD raramente passa de ~150 KB).
 */
#define FRAME_READY_BYTES (384 * 1024)

static int file_fd = -1;
static int read_pending = 0;        /* fileXioRead assíncrono em curso */
static u8 *raw_buf = NULL;          /* bloco bruto do VOB */

static u8 *ring = NULL;             /* vídeo elementar MPEG-2 */
static u32 ring_wr = 0;             /* total escrito (monotônico) */
static u32 ring_rd = 0;             /* total enviado ao IPU */
static int seq_found = 0;           /* já passou do 1º sequence header */
static int tail_done = 0;           /* sequence end + padding no fim */

static u8 *picture = NULL;          /* saída da CSC no IPU (modo IPU) */
static int picture_size = 0;

static MPEGSequenceInfo *seq_info = NULL;
static u16 *direct_dst = NULL;      /* destino do quadro atual */
static s64 cur_pts;
static int decoder_open = 0;

static FrameInfo *cur_info = NULL;  /* para os callbacks */
static u64 read_t0;

/*
 * Modo navegação (libdvdnav): os packs vêm de nav_next() em vez da
 * leitura direta do VOB.
 */
static int nav_mode = 0;
static u8 nav_block[NAV_BLOCK_SIZE] __attribute__((aligned(64)));
static u64 still_t0;
static u32 still_zeros = 0;         /* padding entregue ao IPU no still */

/*
 * Limite de padding num still: se o decodificador continuar pedindo
 * dados depois disso, algo está errado (evita laço infinito).
 */
#define FRAME_STILL_ZERO_MAX (256 * 1024)

/*
 * Botões do menu: o nav packet chega uns segundos antes da imagem
 * (buffer). O número de botões só vale quando o vídeo passa da
 * posição em que ele mudou.
 */
static int btn_pending = 0;
static u32 btn_pending_pos = 0;
static int btn_pending_set = 0;

/*
 * PTS do vídeo: cada PES com PTS marca a posição do buffer onde o
 * seu payload começa; ao enviar essa posição ao IPU, cur_pts recebe
 * o PTS e a libmpeg o associa ao quadro decodificado.
 */
#define VPTS_QUEUE 128

static struct { u32 pos; s64 pts; } vpts_q[VPTS_QUEUE];
static int vpts_head = 0;
static int vpts_count = 0;

/*
 * Packs por chamada de frame_pump: no máximo 256 (512 KB) e no
 * máximo FRAME_NAV_TIME_MS de trabalho. Teste (2026-10-17): 256 sem
 * limite de tempo travava o laço (som falhava); 16 fixos não
 * acompanhavam a leitura (rebuffer).
 */
#define FRAME_NAV_BUDGET  256
#define FRAME_NAV_TIME_MS 8

static char log_buf[8192];          /* mensagens adiadas */
static int log_len = 0;


static u32 ticks_to_ms(u64 ticks)
{
    return (u32)((ticks * 1000ULL) / kBUSCLK);
}


/* ============================================================
 * LOG ADIADO
 * ============================================================ */

int frame_read_idle(void)
{
    return !read_pending && !nav_io_pending();
}


void frame_logf(const char *fmt, ...)
{
    va_list ap;
    int n;

    if (log_len >= (int)sizeof(log_buf) - 1)
        return;

    va_start(ap, fmt);
    n = vsnprintf(log_buf + log_len, sizeof(log_buf) - log_len, fmt, ap);
    va_end(ap);

    if (n > 0)
    {
        log_len += n;

        if (log_len > (int)sizeof(log_buf) - 1)
            log_len = sizeof(log_buf) - 1;
    }

    frame_log_flush();
}


void frame_log_flush(void)
{
    if (read_pending || nav_io_pending() || log_len == 0)
        return;

    printf("%s", log_buf);
    log_len = 0;
    log_buf[0] = '\0';
}


/* ============================================================
 * BUFFER CIRCULAR
 * ============================================================ */

static u32 ring_fill(void)
{
    return ring_wr - ring_rd;
}


static u32 ring_free(void)
{
    return FRAME_RING_SIZE - ring_fill();
}


/*
 * Copia para o buffer (com volta) e grava o cache na RAM, pois o
 * IPU lê por DMA.
 */
static void ring_write(const u8 *data, u32 len)
{
    while (len > 0)
    {
        u32 pos = ring_wr % FRAME_RING_SIZE;
        u32 n = FRAME_RING_SIZE - pos;

        if (n > len)
            n = len;

        memcpy(ring + pos, data, n);
        SyncDCache(ring + (pos & ~63), ring + ((pos + n + 63) & ~63));

        ring_wr += n;
        data += n;
        len -= n;
    }
}


/*
 * Acrescenta payload de vídeo. Antes do primeiro sequence header
 * (00 00 01 B3) os dados são descartados.
 */
static void ring_append_video(const u8 *data, u32 len)
{
    if (!seq_found)
    {
        u32 i;

        for (i = 0; i + 4 <= len; i++)
        {
            if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 && data[i + 3] == 0xB3)
            {
                seq_found = 1;
                data += i;
                len -= i;
                break;
            }
        }

        if (!seq_found)
            return;
    }

    ring_write(data, len);
}


/*
 * Fim da leitura: sequence end code e padding até múltiplo de 16
 * (o DMA trabalha em quadwords).
 */
static void ring_finish(void)
{
    static const u8 end_code[4] = { 0x00, 0x00, 0x01, 0xB7 };
    static const u8 zeros[16] = { 0 };

    if (tail_done)
        return;

    ring_write(end_code, 4);

    if (ring_wr & 15)
        ring_write(zeros, 16 - (ring_wr & 15));

    tail_done = 1;
}


/* ============================================================
 * DEMUX MPEG-PS (DVD)
 * ============================================================ */

static s64 pes_pts(const u8 *p, int hdr)
{
    const u8 *b = p + hdr + 3;

    if (!(p[hdr + 1] & 0x80) || p[hdr + 2] < 5)
        return -1;

    return ((s64)((b[0] >> 1) & 7) << 30) |
           ((s64)b[1] << 22) |
           ((s64)(b[2] >> 1) << 15) |
           ((s64)b[3] << 7) |
           (s64)(b[4] >> 1);
}


static void vpts_push(u32 pos, s64 pts)
{
    if (vpts_count == VPTS_QUEUE)
    {
        vpts_head = (vpts_head + 1) % VPTS_QUEUE;
        vpts_count--;
    }

    vpts_q[(vpts_head + vpts_count) % VPTS_QUEUE].pos = pos;
    vpts_q[(vpts_head + vpts_count) % VPTS_QUEUE].pts = pts;
    vpts_count++;
}


/* Atualiza cur_pts com o PTS do trecho que vai para o IPU. */
static void vpts_apply(u32 pos)
{
    while (vpts_count > 0 && (s32)(pos - vpts_q[vpts_head].pos) >= 0)
    {
        cur_pts = vpts_q[vpts_head].pts;
        vpts_head = (vpts_head + 1) % VPTS_QUEUE;
        vpts_count--;
    }
}


/*
 * Envia ao buffer o payload dos PES de vídeo (0xE0) de um pack.
 */
static void demux_pack(const u8 *p)
{
    int pos;

    if (p[0] != 0 || p[1] != 0 || p[2] != 1 || p[3] != 0xBA)
        return;

    /*
     * Pack header MPEG-2: 14 bytes + stuffing.
     */
    pos = 14 + (p[13] & 0x07);

    while (pos + 6 <= ISO_SECTOR_SIZE)
    {
        int id;
        int len;
        int hdr;

        if (p[pos] != 0 || p[pos + 1] != 0 || p[pos + 2] != 1)
            break;

        id = p[pos + 3];
        len = (p[pos + 4] << 8) | p[pos + 5];
        hdr = pos + 6;

        if (hdr + len > ISO_SECTOR_SIZE)
            len = ISO_SECTOR_SIZE - hdr;

        if (id == 0xE0 && len > 3)
        {
            int header_len = p[hdr + 2];
            int payload = hdr + 3 + header_len;
            int payload_len = len - 3 - header_len;

            if (payload_len > 0)
            {
                s64 pts = nav_mode ? pes_pts(p, hdr) : -1;

                if (pts >= 0 && seq_found)
                    vpts_push(ring_wr, pts);

                ring_append_video(p + payload, payload_len);
            }
        }
        else if (id == 0xBD && len > 3 && nav_mode)
        {
            /* private stream 1: subpicture 0x20 + n */
            int header_len = p[hdr + 2];
            int payload = hdr + 3 + header_len;
            int payload_len = len - 3 - header_len;

            if (payload_len > 1 && payload < ISO_SECTOR_SIZE &&
                p[payload] == 0x20 + nav_spu_stream())
            {
                spu_feed(p + payload + 1, payload_len - 1,
                         (p[hdr + 1] & 0x80) != 0, ring_wr);
            }
            else if (payload_len > 1 && payload < ISO_SECTOR_SIZE &&
                     p[payload] == 0x80 + nav_audio_stream())
            {
                /* áudio AC3 */
                audio_feed_ac3(p + payload + 1, payload_len - 1, pes_pts(p, hdr));
            }
        }

        pos = hdr + len;
    }
}


/* ============================================================
 * LEITURA ASSÍNCRONA
 * ============================================================ */

static void read_start(FrameInfo *info)
{
    u64 left = info->read_limit - info->read_total;
    u32 block = left > FRAME_READ_BLOCK ? FRAME_READ_BLOCK : (u32)left;
    int r;

    block &= ~(ISO_SECTOR_SIZE - 1);

    if (block == 0)
    {
        info->eof = 1;
        return;
    }

    fileXioSetBlockMode(FXIO_NOWAIT);

    r = fileXioRead(file_fd, raw_buf, block);

    if (r < 0)
    {
        fileXioSetBlockMode(FXIO_WAIT);
        frame_logf("FRAME: async read start failed: %d\n", r);
        info->eof = 1;
        return;
    }

    read_pending = 1;
}


/*
 * Conclui uma leitura pendente (sem bloquear se wait == 0).
 * Retorna 1 se concluiu.
 */
static int read_complete(FrameInfo *info, int wait)
{
    int result = 0;
    int i;
    u64 elapsed;

    if (!read_pending)
        return 0;

    if (fileXioWaitAsync(wait ? FXIO_WAIT : FXIO_NOWAIT, &result) != FXIO_COMPLETE)
        return 0;

    read_pending = 0;
    fileXioSetBlockMode(FXIO_WAIT);

    if (result <= 0)
    {
        frame_logf("FRAME: read returned %d at %llu\n", result, (unsigned long long)info->read_total);
        info->eof = 1;
        return 1;
    }

    info->read_total += result;

    elapsed = GetTimerSystemTime() - read_t0;

    if (elapsed > 0)
        info->read_kbps = (u32)((info->read_total * (u64)kBUSCLK) / (elapsed * 1024ULL));

    for (i = 0; i + ISO_SECTOR_SIZE <= result; i += ISO_SECTOR_SIZE)
        demux_pack(raw_buf + i);

    if (info->read_total >= info->read_limit)
        info->eof = 1;

    return 1;
}


/* ============================================================
 * MODO NAVEGAÇÃO (LIBDVDNAV)
 * ============================================================ */

/*
 * Still frame: sequence end code depois do último quadro, para o
 * decodificador entregar a imagem que fica na tela.
 */
static void ring_seq_end(void)
{
    static const u8 end_code[4] = { 0x00, 0x00, 0x01, 0xB7 };
    static const u8 zeros[16] = { 0 };

    if (!seq_found)
        return;

    ring_write(end_code, 4);

    if (ring_wr & 15)
        ring_write(zeros, 16 - (ring_wr & 15));
}


/*
 * Aplica o que já foi alcançado pelo vídeo: subpicture e número de
 * botões.
 */
static void nav_sync(FrameInfo *info)
{
    if (spu_update(ring_rd))
        info->spu_serial = spu_info()->serial;

    if (btn_pending_set && (s32)(ring_rd - btn_pending_pos) >= 0)
    {
        info->buttons = btn_pending;
        btn_pending_set = 0;
    }
}


static void nav_pump(FrameInfo *info)
{
    int budget = FRAME_NAV_BUDGET;
    u64 t_end = GetTimerSystemTime() + (u64)kBUSCLK * FRAME_NAV_TIME_MS / 1000;
    int n = 0;

    nav_sync(info);

    if (info->still)
    {
        u32 ms = ticks_to_ms(GetTimerSystemTime() - still_t0);

        /*
         * Conclui a leitura em segundo plano durante o still: libera
         * o fileXio para o log adiado.
         */
        nav_ready();

        if (info->still_len == 0xff)
        {
            info->still_left_ms = 0;
            return;
        }

        info->still_left_ms = ms < (u32)info->still_len * 1000 ? (u32)info->still_len * 1000 - ms : 0;

        /* tempo esgotado e imagem já entregue: segue o disco */
        if (info->still_left_ms == 0 && frame_still_hold(info))
        {
            frame_logf("FRAME: still %d s done (%d pictures)\n", info->still_len, info->frames);
            nav_still_skip();
            info->still = 0;
            still_zeros = 0;
        }
        else
            return;
    }

    while (budget-- > 0 && !info->eof &&
           ring_free() >= NAV_BLOCK_SIZE + FRAME_RING_GUARD && nav_ready())
    {
        NavEvent ev;
        int r = nav_next(nav_block, &ev);

        if (r == NAV_EV_BLOCK)
        {
            demux_pack(nav_block);

            /* para ao estourar o tempo */
            if ((++n & 7) == 0 && GetTimerSystemTime() > t_end)
                break;
        }
        else if (r == NAV_EV_STILL)
        {
            ring_seq_end();

            info->still = 1;
            info->still_len = ev.still_len;
            info->still_left_ms = ev.still_len == 0xff ? 0 : (u32)ev.still_len * 1000;
            still_t0 = GetTimerSystemTime();
            still_zeros = 0;
            break;
        }
        else if (r == NAV_EV_STOP || r == NAV_EV_ERROR)
        {
            if (r == NAV_EV_ERROR)
                snprintf(info->error, sizeof(info->error), "DVD navigation error");
            info->eof = 1;
        }
    }

    {
        int b = nav_buttons();

        if (b != (btn_pending_set ? btn_pending : info->buttons))
        {
            btn_pending = b;
            btn_pending_pos = ring_wr;
            btn_pending_set = 1;
        }
    }

    info->read_total = nav_bytes_read();

    {
        u64 elapsed = GetTimerSystemTime() - read_t0;

        if (elapsed > 0)
            info->read_kbps = (u32)((info->read_total * (u64)kBUSCLK) / (elapsed * 1024ULL));
    }

    if (info->eof)
        ring_finish();
}


/*
 * Still frame com a imagem já entregue: a tela deve ficar parada
 * (não chamar frame_next).
 */
int frame_still_hold(const FrameInfo *info)
{
    return nav_mode && info->still && ring_fill() < 16 && MPEG_AtSequenceStart();
}


void frame_pump(FrameInfo *info)
{
    if (nav_mode)
    {
        frame_log_flush();
        audio_pump(info->pts);
        nav_pump(info);
        info->buffered = ring_fill();
        frame_log_flush();
        return;
    }

    if (file_fd < 0)
        return;

    read_complete(info, 0);

    frame_log_flush();

    if (!read_pending && !info->eof &&
        ring_free() >= FRAME_READ_BLOCK + FRAME_RING_GUARD)
    {
        read_start(info);
    }

    if (info->eof && !read_pending)
        ring_finish();

    info->buffered = ring_fill();

    /*
     * Escreve mensagens adiadas antes de iniciar a próxima leitura.
     */
    frame_log_flush();
}


int frame_ready(const FrameInfo *info)
{
    return info->eof || info->still || ring_fill() >= FRAME_READY_BYTES;
}


/* ============================================================
 * CALLBACKS DA LIBMPEG
 * ============================================================ */

/*
 * Envia o próximo bloco de vídeo ao IPU. Se o buffer secar (rede
 * atrasada), espera a leitura aqui mesmo.
 */
static int frame_dma_cb(void *param)
{
    FrameInfo *info = cur_info;
    u32 chunk;

    (void)param;

    while (1)
    {
        u32 avail = ring_fill();
        u32 pos = ring_rd % FRAME_RING_SIZE;

        chunk = avail;

        if (chunk > FRAME_DMA_CHUNK)
            chunk = FRAME_DMA_CHUNK;

        if (chunk > FRAME_RING_SIZE - pos)
            chunk = FRAME_RING_SIZE - pos;

        chunk &= ~15;

        if (chunk > 0)
            break;

        if (nav_mode)
        {
            if (info->still)
            {
                /*
                 * Still: o decodificador ainda quer bits depois do
                 * sequence end; entrega padding (zeros).
                 */
                static const u8 zeros[16] = { 0 };

                if (still_zeros >= FRAME_STILL_ZERO_MAX)
                {
                    frame_logf("FRAME: decoder kept reading during still - stop\n");
                    return 0;
                }

                ring_write(zeros, 16);
                still_zeros += 16;
                continue;
            }

            if (info->eof)
            {
                ring_finish();

                if (ring_fill() < 16)
                    return 0;

                continue;
            }

            if (!nav_ready())
                nav_wait_io();

            nav_pump(info);
            continue;
        }

        if (info->eof && !read_pending)
        {
            ring_finish();

            if (ring_fill() < 16)
                return 0;

            continue;
        }

        /*
         * Sem dados: concluir (esperando) a leitura em curso ou
         * iniciar outra.
         */
        if (read_pending)
            read_complete(info, 1);
        else
            frame_pump(info);
    }

    if (nav_mode)
        vpts_apply(ring_rd + chunk);

    dma_channel_wait(DMA_CHANNEL_toIPU, 0);
    dma_channel_send_normal(DMA_CHANNEL_toIPU, ring + (ring_rd % FRAME_RING_SIZE), chunk >> 4, 0, 0);

    ring_rd += chunk;

    return 1;
}


/*
 * Chamado no sequence header: reserva a área do quadro (modo IPU)
 * e configura o destino direto (modo software).
 */
static void *frame_init_cb(void *param, MPEGSequenceInfo *si)
{
    FrameInfo *info = (FrameInfo *)param;
    int size = si->m_Width * si->m_Height * 4;

    if (info->width != si->m_Width || info->height != si->m_Height)
        frame_logf(
            "FRAME: sequence %dx%d profile=%d level=%d fmt=%d ms/frame=%d\n",
            si->m_Width,
            si->m_Height,
            si->m_Profile,
            si->m_Level,
            si->m_VideoFmt,
            si->m_MSPerFrame
        );

    seq_info = si;
    info->width = si->m_Width;
    info->height = si->m_Height;

    /*
     * NTSC 29.97 (33 ms) ou PAL 25 (40 ms).
     */
    info->frame_us = si->m_MSPerFrame >= 40 ? 40000 : 33367;

    if (FRAME_GS_CSC)
    {
        if (plane_y && si->m_Width <= FRAME_MAX_WIDTH && si->m_Height <= FRAME_MAX_HEIGHT)
            MPEG_SetPlanar(plane_y, plane_cb, plane_cr, si->m_Width);
        else
            MPEG_SetPlanar(NULL, NULL, NULL, 0);
    }
    else if (direct_dst && si->m_Width <= FRAME_MAX_WIDTH && si->m_Height <= FRAME_MAX_HEIGHT)
        MPEG_SetDirectCT16(direct_dst, si->m_Width);

    if (MPEG_DirectCT16())
    {
        /*
         * A CSC em software ignora este buffer; basta um válido.
         */
        if (!picture)
        {
            picture = memalign(64, 64);
            picture_size = 64;
        }

        return picture;
    }

    if (!picture || picture_size < size)
    {
        if (picture)
            free(picture);

        picture = memalign(64, size);
        picture_size = picture ? size : 0;
    }

    if (picture)
        SyncDCache(picture, picture + size);

    return picture;
}


/* ============================================================
 * API
 * ============================================================ */

static void frame_free(void)
{
    if (raw_buf)
        free(raw_buf);

    if (ring)
        free(ring);

    if (picture)
        free(picture);

    raw_buf = NULL;
    ring = NULL;
    picture = NULL;
    picture_size = 0;
    seq_info = NULL;
    direct_dst = NULL;
}


int frame_open(
    const char *path,
    u32 vob_lba,
    u64 vob_bytes,
    FrameInfo *info
)
{
    s64 offset;

    frame_close();

    memset(info, 0, sizeof(*info));

    printf("\n");
    printf("==============================\n");
    printf(" VIDEO OPEN\n");
    printf("==============================\n");
    printf(
        "FRAME: %s VOB LBA %u, %llu bytes\n",
        path,
        (unsigned)vob_lba,
        (unsigned long long)vob_bytes
    );

    raw_buf = memalign(64, FRAME_READ_BLOCK);
    ring = memalign(64, FRAME_RING_SIZE);

    if (!raw_buf || !ring)
    {
        snprintf(info->error, sizeof(info->error), "Out of memory");
        frame_free();
        return -1;
    }

    if (planes_alloc() < 0)
    {
        snprintf(info->error, sizeof(info->error), "Out of memory (planes)");
        frame_free();
        return -1;
    }

    ring_wr = 0;
    ring_rd = 0;
    seq_found = 0;
    tail_done = 0;
    read_pending = 0;

    info->buffer_size = FRAME_RING_SIZE;
    info->read_limit = vob_bytes;

    fileXioSetBlockMode(FXIO_WAIT);
    fileXioSetRWBufferSize(FRAME_READ_BLOCK);

    file_fd = fileXioOpen(path, FIO_O_RDONLY);

    if (file_fd < 0)
    {
        snprintf(info->error, sizeof(info->error), "OPEN failed (%d)", file_fd);
        file_fd = -1;
        frame_free();
        return -2;
    }

    offset = (s64)vob_lba * ISO_SECTOR_SIZE;

    if (fileXioLseek64(file_fd, offset, SEEK_SET) != offset)
    {
        snprintf(info->error, sizeof(info->error), "SEEK to VOB failed");
        fileXioClose(file_fd);
        file_fd = -1;
        frame_free();
        return -3;
    }

    cur_info = info;
    read_t0 = GetTimerSystemTime();

    read_start(info);

    /*
     * Decodificador. Usa DMA toIPU/fromIPU e scratchpad
     * 0x0000-0x3C00 durante a decodificação.
     */
    dma_channel_initialize(DMA_CHANNEL_toIPU, NULL, 0);

    MPEG_Initialize(frame_dma_cb, NULL, frame_init_cb, info, &cur_pts);

    decoder_open = 1;

    return 0;
}


int frame_open_nav(
    const char *path,
    u64 iso_size,
    FrameInfo *info
)
{
    frame_close();

    memset(info, 0, sizeof(*info));

    printf("\n");
    printf("==============================\n");
    printf(" VIDEO OPEN (DVD navigation)\n");
    printf("==============================\n");
    printf("FRAME: %s, %llu bytes\n", path, (unsigned long long)iso_size);

    ring = memalign(64, FRAME_RING_SIZE);

    if (!ring)
    {
        snprintf(info->error, sizeof(info->error), "Out of memory");
        frame_free();
        return -1;
    }

    if (planes_alloc() < 0)
    {
        snprintf(info->error, sizeof(info->error), "Out of memory (planes)");
        frame_free();
        return -1;
    }

    ring_wr = 0;
    ring_rd = 0;
    seq_found = 0;
    tail_done = 0;
    read_pending = 0;
    still_zeros = 0;

    info->nav = 1;
    info->buffer_size = FRAME_RING_SIZE;

    btn_pending_set = 0;
    spu_reset();
    info->spu_serial = spu_info()->serial;
    audio_reset();
    vpts_head = 0;
    vpts_count = 0;
    cur_pts = -1;
    info->pts = -1;

    if (nav_open(path, iso_size, 1) < 0)
    {
        nav_close();
        frame_log_flush();
        snprintf(info->error, sizeof(info->error), "DVD navigation open failed");
        frame_free();
        return -2;
    }

    /* nada de fileXio em curso ao voltar (o chamador usa printf) */
    nav_wait_io();
    frame_log_flush();

    nav_mode = 1;
    cur_info = info;
    read_t0 = GetTimerSystemTime();

    dma_channel_initialize(DMA_CHANNEL_toIPU, NULL, 0);

    MPEG_Initialize(frame_dma_cb, NULL, frame_init_cb, info, &cur_pts);

    decoder_open = 1;

    return 0;
}


/*
 * Salto do DVD (botão de menu): descarta o vídeo antigo do buffer e
 * reinicia o decodificador; o vídeo novo começa no próximo sequence
 * header.
 */
void frame_nav_flush(FrameInfo *info)
{
    if (!nav_mode || !decoder_open)
        return;

    MPEG_StopInput();
    MPEG_Destroy();

    ring_wr = 0;
    ring_rd = 0;
    seq_found = 0;
    tail_done = 0;
    still_zeros = 0;

    info->still = 0;
    info->still_left_ms = 0;
    info->eof = 0;
    info->buttons = 0;

    btn_pending_set = 0;
    spu_reset();
    info->spu_serial = spu_info()->serial;
    audio_reset();
    vpts_head = 0;
    vpts_count = 0;
    cur_pts = -1;
    info->pts = -1;

    dma_channel_initialize(DMA_CHANNEL_toIPU, NULL, 0);
    MPEG_Initialize(frame_dma_cb, NULL, frame_init_cb, info, &cur_pts);

    frame_logf("FRAME: flush (jump)\n");
}


int frame_next_skip(
    u16 *dst,
    FrameInfo *info
)
{
    int r;

    MPEG_SetSkipCSC(1);
    r = frame_next(dst, info);
    MPEG_SetSkipCSC(0);

    return r;
}


int frame_next(
    u16 *dst,
    FrameInfo *info
)
{
    u64 t0;
    s64 pts;

    if (!decoder_open)
        return -1;

    cur_info = info;

    /*
     * Destino do quadro: se a largura ainda não é conhecida (antes
     * do primeiro sequence header), frame_init_cb configura.
     */
    direct_dst = dst;

    if (!FRAME_GS_CSC && info->width > 0)
        MPEG_SetDirectCT16(dst, info->width);

    t0 = GetTimerSystemTime();

    if (picture && !MPEG_DirectCT16())
        SyncDCache(picture, picture + picture_size);

    if (!MPEG_Picture(picture, &pts))
    {
        /*
         * Navegação: fim de uma sequência (ex.: still, troca de
         * menu) não é o fim do disco.
         */
        if (nav_mode && !info->eof)
            return 2;

        frame_logf(
            "FRAME: end of video after %d pictures (eof=%d)\n",
            info->frames,
            seq_info ? seq_info->m_fEOF : -1
        );

        return 0;
    }

    info->frames++;

    if (info->width <= 0 || info->height <= 0 ||
        info->width > FRAME_MAX_WIDTH || info->height > FRAME_MAX_HEIGHT)
    {
        snprintf(info->error, sizeof(info->error), "Bad picture size %dx%d", info->width, info->height);
        return -2;
    }

    if (FRAME_GS_CSC)
    {
        /* planos escritos pela CPU: gravar na RAM antes do DMA ao GS */
        SyncDCache(plane_y, plane_y + info->width * info->height);
        SyncDCache(plane_cb, plane_cb + (info->width / 2) * (info->height / 2));
        SyncDCache(plane_cr, plane_cr + (info->width / 2) * (info->height / 2));
    }
    else if (MPEG_DirectCT16())
    {
        /*
         * CSC em software: quadro já em dst (CT16 linear), escrito
         * pela CPU; gravar na RAM antes do envio por DMA ao GS.
         */
        SyncDCache(dst, dst + info->width * info->height);
    }
    else
    {
        /*
         * CSC no IPU: blocos 16x16 RGBA32 em "picture" (escritos por
         * DMA) -> CT16 linear em dst.
         */
        int mbw = info->width >> 4;
        int mbh = info->height >> 4;
        int mx;
        int my;

        InvalidDCache(picture, picture + picture_size);

        for (my = 0; my < mbh; my++)
        {
            for (mx = 0; mx < mbw; mx++)
            {
                const u32 *mb = (const u32 *)(picture + ((my * mbw + mx) << 10));
                int y;

                for (y = 0; y < 16; y++)
                {
                    u16 *out = dst + (my * 16 + y) * info->width + mx * 16;
                    int x;

                    for (x = 0; x < 16; x++)
                    {
                        u32 c = mb[y * 16 + x];

                        out[x] =
                            ((c >> 3) & 0x1F) |
                            (((c >> 11) & 0x1F) << 5) |
                            (((c >> 19) & 0x1F) << 10) |
                            0x8000;
                    }
                }
            }
        }
    }

    info->decode_ms = ticks_to_ms(GetTimerSystemTime() - t0);
    info->buffered = ring_fill();

    if (nav_mode)
    {
        info->pts = pts;
        nav_sync(info);
        audio_pump(pts);
    }

    return 1;
}


void frame_close(void)
{
    if (decoder_open)
    {
        MPEG_Destroy();
        decoder_open = 0;
    }

    if (nav_mode)
    {
        audio_reset();
        audio_stop();
        nav_close();
        nav_mode = 0;
    }

    if (file_fd >= 0)
    {
        if (read_pending)
        {
            int result;

            fileXioWaitAsync(FXIO_WAIT, &result);
            read_pending = 0;
        }

        fileXioSetBlockMode(FXIO_WAIT);
        fileXioClose(file_fd);
        file_fd = -1;
    }

    frame_log_flush();

    frame_free();
    cur_info = NULL;
}
