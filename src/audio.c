#include <string.h>
#include <inttypes.h>

#include <stdio.h>
#include <tamtypes.h>
#include <timer.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <audsrv.h>

#include <a52.h>

#include "audio.h"
#include "frame.h"

/*
 * Um quadro AC3 tem no máximo 3840 bytes; 6 blocos de 256 amostras
 * por canal (1536 amostras = 32 ms a 48 kHz).
 */
#define AC3_HEADER    7
#define AC3_MAX_FRAME 3840

static a52_state_t *a52 = NULL;

static u8 frame_buf[AC3_MAX_FRAME];
static int frame_have = 0;          /* bytes em frame_buf */
static int frame_len = 0;           /* tamanho do quadro atual (0 = procurando) */

/*
 * Fila de PCM: ~6 s (o buffer de vídeo segura ~4 s à frente).
 */
#define PCM_FRAME_SAMPLES 1536
#define PCM_FRAME_BYTES   (PCM_FRAME_SAMPLES * 2 * 2)
#define PCM_QUEUE         192

/* 1536 amostras a 48 kHz = 32 ms = 2880 ticks de 90 kHz */
#define AC3_FRAME_TICKS   2880

/* janelas de sincronia (90 kHz) */
#define SYNC_LATE   45000       /* 0,5 s atrasado: descarta */
#define SYNC_JUMP   270000      /* 3 s à frente: salto, descarta */
/* ajuste fino da sincronia (som adiantado > 0) */
#define SYNC_LEAD   7200        /* 80 ms: medido -66 ms (lead 0) e -20 ms (lead 60) */

typedef struct
{
    s64 pts;
    s16 pcm[PCM_FRAME_SAMPLES * 2];
} PcmFrame;

static PcmFrame pcm_q[PCM_QUEUE] __attribute__((aligned(64)));
static int q_head = 0;
static int q_count = 0;

static s64 pts_pending = -1;        /* PTS do PES ainda não usado */
static s64 next_pts = -1;           /* PTS previsto do próximo quadro */
static s64 frame_pts = -1;          /* PTS do quadro em montagem */

static int output_ok = 0;
static int head_sent = 0;           /* bytes do quadro da frente já enviados */

/*
 * Medida da sincronia (só observa, não altera o envio): PTS do fim
 * do que já foi entregue ao audsrv; o som que sai agora é esse PTS
 * menos o tempo ainda na fila do audsrv.
 */
static s64 end_pts = -1;

static AudioStats stats;

extern unsigned char LIBSD_irx[];
extern unsigned int size_LIBSD_irx;
extern unsigned char AUDSRV_irx[];
extern unsigned int size_AUDSRV_irx;


static u32 ticks_to_us(u64 t)
{
    return (u32)((t * 1000000ULL) / kBUSCLK);
}


int audio_init_output(void)
{
    struct audsrv_fmt_t fmt;
    int ret = 0;
    int id;

    id = SifExecModuleBuffer(LIBSD_irx, size_LIBSD_irx, 0, NULL, &ret);
    printf("AUDIO: libsd.irx -> module=%d result=%d\n", id, ret);
    if (id < 0)
        return -1;

    id = SifExecModuleBuffer(AUDSRV_irx, size_AUDSRV_irx, 0, NULL, &ret);
    printf("AUDIO: audsrv.irx -> module=%d result=%d\n", id, ret);
    if (id < 0)
        return -2;

    ret = audsrv_init();
    if (ret != 0)
    {
        printf("AUDIO: audsrv_init failed (%d)\n", ret);
        return -3;
    }

    fmt.bits = 16;
    fmt.freq = 48000;
    fmt.channels = 2;

    ret = audsrv_set_format(&fmt);
    if (ret != 0)
    {
        printf("AUDIO: audsrv_set_format failed (%d)\n", ret);
        return -4;
    }

    audsrv_set_volume(MAX_VOLUME);

    output_ok = 1;
    printf("AUDIO: output ready (48000 Hz, 16 bits, stereo), audsrv free %d bytes, queued %d\n",
           audsrv_available(), audsrv_queued());
    return 0;
}


void audio_reset(void)
{
    if (!a52)
        a52 = a52_init(0);

    frame_have = 0;
    frame_len = 0;
    q_head = 0;
    q_count = 0;
    pts_pending = -1;
    next_pts = -1;
    frame_pts = -1;

    head_sent = 0;
    end_pts = -1;

    /*
     * audsrv_stop_audio congela a leitura do anel: só ao fechar
     * (audio_stop). Num salto, a fila do audsrv toca até acabar e
     * audio_pump completa com silêncio.
     */

    memset(&stats, 0, sizeof(stats));
    stats.output = output_ok;
}


const AudioStats *audio_stats(void)
{
    return &stats;
}


static inline s16 to_s16(float v)
{
    int i = (int)v;

    if (i > 32767)
        return 32767;
    if (i < -32768)
        return -32768;
    return (s16)i;
}


static void decode_frame(void)
{
    u64 t0 = GetTimerSystemTime();
    int flags = A52_STEREO | A52_ADJUST_LEVEL;
    sample_t level = 32767.0f;
    int b;
    u32 us;
    PcmFrame *f;
    s16 *pcm;

    if (q_count == PCM_QUEUE)
    {
        /* fila cheia: descarta o mais antigo */
        q_head = (q_head + 1) % PCM_QUEUE;
        q_count--;
        head_sent = 0;
        stats.dropped_full++;
    }

    f = &pcm_q[(q_head + q_count) % PCM_QUEUE];
    pcm = f->pcm;

    if (a52_frame(a52, frame_buf, &flags, &level, 0.0f) != 0)
    {
        stats.errors++;
        return;
    }

    a52_dynrng(a52, NULL, NULL);    /* sem compressão de dinâmica */

    for (b = 0; b < 6; b++)
    {
        sample_t *s;
        s16 *out = pcm + b * 256 * 2;
        int i;

        if (a52_block(a52) != 0)
        {
            stats.errors++;
            return;
        }

        /* canal esquerdo em s[0..255], direito em s[256..511] */
        s = a52_samples(a52);

        for (i = 0; i < 256; i++)
        {
            out[i * 2] = to_s16(s[i]);
            out[i * 2 + 1] = to_s16(s[i + 256]);
        }
    }

    us = ticks_to_us(GetTimerSystemTime() - t0);

    f->pts = frame_pts;
    q_count++;
    stats.queued = q_count;

    stats.frames++;
    stats.decode_us_total += us;
    if (us > stats.decode_us_max)
        stats.decode_us_max = us;

    if (stats.frames == 1)
        frame_logf("AUDIO: first AC3 frame: %d Hz, %d kbps, flags 0x%x, %u us\n",
                   stats.sample_rate, stats.bit_rate / 1000, stats.channels_flags, us);
}


void audio_feed_ac3(const u8 *data, int len, s64 pts)
{
    if (!a52)
        return;

    if (pts >= 0)
        pts_pending = pts;

    /* cabeçalho do DVD: nº de quadros (1) + ponteiro (2) */
    if (len <= 3)
        return;

    data += 3;
    len -= 3;

    while (len > 0)
    {
        int n;

        if (frame_len == 0)
        {
            /*
             * Começo de um quadro: o PTS do PES vale para o primeiro
             * quadro que começa nele; os seguintes somam 32 ms.
             */
            if (frame_have == 0)
            {
                if (pts_pending >= 0)
                {
                    frame_pts = pts_pending;
                    pts_pending = -1;
                }
                else
                    frame_pts = next_pts;
            }

            /* procurando o sync (0x0B77) + cabeçalho */
            n = AC3_HEADER - frame_have;
            if (n > len)
                n = len;

            memcpy(frame_buf + frame_have, data, n);
            frame_have += n;
            data += n;
            len -= n;

            if (frame_have < AC3_HEADER)
                break;

            {
                int flags, sr, br;
                int size = a52_syncinfo(frame_buf, &flags, &sr, &br);

                if (size <= 0 || size > AC3_MAX_FRAME)
                {
                    /* não sincronizado: avança 1 byte */
                    memmove(frame_buf, frame_buf + 1, AC3_HEADER - 1);
                    frame_have = AC3_HEADER - 1;
                    continue;
                }

                frame_len = size;
                next_pts = frame_pts >= 0 ? frame_pts + AC3_FRAME_TICKS : -1;
                stats.sample_rate = sr;
                stats.bit_rate = br;
                stats.channels_flags = flags;
            }
        }

        n = frame_len - frame_have;
        if (n > len)
            n = len;

        memcpy(frame_buf + frame_have, data, n);
        frame_have += n;
        data += n;
        len -= n;

        if (frame_have == frame_len)
        {
            decode_frame();
            frame_have = 0;
            frame_len = 0;
        }
    }
}


/* ============================================================
 * SAÍDA (SINCRONIA COM O VÍDEO)
 * ============================================================ */

/*
 * O audsrv (IOP) toca um anel de 20480 bytes (~107 ms) e avança a
 * leitura sem parar, mesmo sem dado novo: se a escrita atrasa, ele
 * repete som velho. Por isso a fila dele é mantida sempre em
 * AUDSRV_TARGET bytes, com o próximo quadro na hora certa ou com
 * silêncio.
 */
#define AUDSRV_TARGET   12288       /* ~64 ms */

/*
 * Cada chamada ao audsrv é um RPC (o EE espera o IOP): no máximo um
 * ciclo a cada AUDIO_PUMP_MS, com poucos envios grandes.
 */
#define AUDIO_PUMP_MS   8

static const s16 silence[AUDSRV_TARGET / 2] __attribute__((aligned(64)));
static u64 last_pump = 0;



/* bytes na fila do audsrv -> ticks de 90 kHz (48 kHz, 4 bytes/amostra) */
static s64 bytes_to_ticks(int bytes)
{
    return (s64)bytes * 90000 / (48000 * 4);
}


void audio_pump(s64 video_pts)
{
    int queued;
    int guard;

    if (!output_ok)
        return;

    {
        u64 now = GetTimerSystemTime();

        if (now - last_pump < (u64)kBUSCLK * AUDIO_PUMP_MS / 1000)
            return;

        last_pump = now;
    }

    queued = audsrv_queued();
    stats.rpc++;
    if (queued < 0 || queued > 20480)
        queued = 0;

    for (guard = 0; guard < 16 && queued < AUDSRV_TARGET; guard++)
    {
        int room = AUDSRV_TARGET - queued;
        int sent;

        /* descarta o que já passou ou ficou longe demais (salto) */
        while (q_count > 0 && video_pts > 0 && pcm_q[q_head].pts >= 0 && head_sent == 0)
        {
            s64 d = pcm_q[q_head].pts - video_pts;

            if (d < -SYNC_LATE)
                stats.dropped_late++;
            else if (d > SYNC_JUMP)
                stats.dropped_jump++;
            else
                break;

            q_head = (q_head + 1) % PCM_QUEUE;
            q_count--;
        }

        if (q_count > 0 && video_pts > 0 &&
            (head_sent > 0 || pcm_q[q_head].pts < 0 ||
             pcm_q[q_head].pts - video_pts <= SYNC_LEAD + bytes_to_ticks(queued)))
        {
            /* quadro na hora (contando a espera na fila do audsrv) */
            PcmFrame *f = &pcm_q[q_head];
            int n = PCM_FRAME_BYTES - head_sent;

            if (n > room)
                n = room;

            if (head_sent == 0)
                stats.last_diff = f->pts >= 0 ? f->pts - video_pts - bytes_to_ticks(queued) : 0;

            sent = audsrv_play_audio((const char *)f->pcm + head_sent, n);
            stats.rpc++;
            if (sent <= 0)
                break;

            head_sent += sent;

            if (f->pts >= 0)
                end_pts = f->pts + bytes_to_ticks(head_sent);

            if (head_sent >= PCM_FRAME_BYTES)
            {
                head_sent = 0;
                q_head = (q_head + 1) % PCM_QUEUE;
                q_count--;
                stats.played++;
            }
        }
        else
        {
            /*
             * Nada na hora: silêncio (evita repetir som velho). Se um
             * quadro vem logo, só até a hora dele.
             */
            int n = room;

            if (q_count > 0 && video_pts > 0 && pcm_q[q_head].pts >= 0)
            {
                s64 wait = pcm_q[q_head].pts - video_pts - SYNC_LEAD - bytes_to_ticks(queued);
                int wbytes = (int)(wait * (48000 * 4) / 90000) & ~3;

                if (wbytes > 0 && wbytes < n)
                    n = wbytes;
            }

            if (n < 4)
                break;

            sent = audsrv_play_audio((const char *)silence, n);
            stats.rpc++;
            if (sent <= 0)
                break;

            stats.silence += sent;

            if (end_pts >= 0)
                end_pts += bytes_to_ticks(sent);
        }

        queued += sent;
    }

    stats.queued = q_count;

    /* som saindo agora - imagem na tela agora */
    if (end_pts >= 0 && video_pts > 0)
    {
        s64 av = end_pts - bytes_to_ticks(queued) - video_pts;

        stats.av_now = av;

        if (av > -SYNC_JUMP && av < SYNC_JUMP)
        {
            if (stats.av_count == 0 || av < stats.av_min)
                stats.av_min = av;
            if (stats.av_count == 0 || av > stats.av_max)
                stats.av_max = av;

            stats.av_sum += av;
            stats.av_count++;
        }
    }
}


void audio_stop(void)
{
    if (output_ok)
        audsrv_stop_audio();
}
