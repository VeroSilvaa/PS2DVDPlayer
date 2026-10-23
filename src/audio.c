#include <string.h>
#include <inttypes.h>
#include <malloc.h>

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

/*
 * Todas as faixas AC3 do disco (0x80..0x87) são guardadas ainda
 * comprimidas, cada uma na sua fila (~8 s); só a faixa ativa é
 * decodificada, e só ~1 s à frente da imagem. Trocar de faixa descarta
 * o PCM que ainda não começou a tocar e decodifica a faixa nova do
 * mesmo ponto: a troca é ouvida na hora (antes, o PCM de ~4 s à frente
 * da faixa antiga tocava até o fim).
 *
 * No DVD o AC3 vai até 448 kbps: 1792 bytes por quadro.
 */
#define AUDIO_STREAMS 8
#define RING_SLOTS    256           /* 256 x 32 ms = ~8 s */
#define SLOT_BYTES    1920

/*
 * PCM decodificado à frente da imagem. A decodificação roda só no
 * tempo livre (audio_decode, de frame_pump), nunca junto com a imagem
 * (audio_pump em frame_next): 4 quadros ali (~11 ms em 5.1) faziam o
 * vídeo passar do prazo e pular ~15% dos quadros.
 */
#define DECODE_LEAD   90000         /* 1 s */
#define DECODE_SWITCH 2             /* na troca de faixa, na hora (~5 ms) */

static s64 last_video_pts = -1;     /* último PTS de imagem visto em audio_pump */

static void decode_ahead(s64 video_pts, int budget);

static a52_state_t *a52 = NULL;

/* montagem dos quadros de cada faixa */
typedef struct
{
    u8 buf[AC3_MAX_FRAME];
    int have;                       /* bytes em buf */
    int len;                        /* tamanho do quadro atual (0 = procurando) */
    s64 pts_pending;                /* PTS do PES ainda não usado */
    s64 next_pts;                   /* PTS previsto do próximo quadro */
    s64 frame_pts;                  /* PTS do quadro em montagem */
} Ac3Parser;

/* quadros comprimidos de cada faixa */
typedef struct
{
    u8 *data;                       /* RING_SLOTS x SLOT_BYTES (alocado no 1º quadro) */
    s64 pts[RING_SLOTS];
    u16 len[RING_SLOTS];
    int head;                       /* mais antigo */
    int count;
    int dec;                        /* faixa ativa: próximo a decodificar (a partir de head) */
} Ac3Ring;

static Ac3Parser parsers[AUDIO_STREAMS];
static Ac3Ring rings[AUDIO_STREAMS];
static int active = 0;              /* faixa decodificada */

/*
 * Fila de PCM (~6 s de capacidade; na prática ~1 s, DECODE_LEAD).
 */
#define PCM_FRAME_SAMPLES 1536
#define PCM_FRAME_BYTES   (PCM_FRAME_SAMPLES * 2 * 2)
#define PCM_QUEUE         192

/* 1536 amostras a 48 kHz = 32 ms = 2880 ticks de 90 kHz */
#define AC3_FRAME_TICKS   2880

/* janelas de sincronia (90 kHz) */
#define SYNC_LATE   45000       /* 0,5 s atrasado: descarta */
/*
 * 10 s à frente: salto, descarta. Precisa ser maior que a folga
 * normal entre o áudio decodificado e a imagem na tela; com 3 s, uma
 * fila vazia nunca mais se refazia (todo quadro novo chegava ~4 s à
 * frente).
 */
#define SYNC_JUMP   900000
/*
 * Recuperação: quadro que já sairia mais atrasado que isso (contando
 * a fila do audsrv) é descartado. Depois de um salto o primeiro áudio
 * pode ser anterior à primeira imagem; sem isso o som ficava ~110 ms
 * atrasado até o próximo salto (abaixo de SYNC_LATE). Jitter normal
 * medido: -77 a +54 ms.
 */
#define SYNC_CATCHUP 8100       /* 90 ms */
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
    int s;

    if (!a52)
        a52 = a52_init(0);

    for (s = 0; s < AUDIO_STREAMS; s++)
    {
        Ac3Parser *p = &parsers[s];

        p->have = 0;
        p->len = 0;
        p->pts_pending = -1;
        p->next_pts = -1;
        p->frame_pts = -1;

        /* a memória da fila fica alocada */
        rings[s].head = 0;
        rings[s].count = 0;
        rings[s].dec = 0;
    }

    q_head = 0;
    q_count = 0;

    head_sent = 0;
    end_pts = -1;
    last_video_pts = -1;

    /*
     * audsrv_stop_audio congela a leitura do anel: só ao fechar
     * (audio_stop). Num salto, a fila do audsrv toca até acabar e
     * audio_pump completa com silêncio.
     */

    memset(&stats, 0, sizeof(stats));
    stats.output = output_ok;
}


void audio_select_stream(int stream)
{
    Ac3Ring *r;
    s64 cut;
    int keep;
    int i;

    stream &= AUDIO_STREAMS - 1;

    if (stream == active)
        return;

    /*
     * Corte: o primeiro quadro de PCM que ainda não começou a tocar.
     * O que já está no audsrv (~64 ms) e o quadro pela metade ficam.
     */
    keep = head_sent > 0 ? 1 : 0;

    if (q_count > keep)
        cut = pcm_q[(q_head + keep) % PCM_QUEUE].pts;
    else if (q_count > 0 && pcm_q[q_head].pts >= 0)
        cut = pcm_q[q_head].pts + AC3_FRAME_TICKS;
    else
        cut = end_pts;

    q_count = keep;

    rings[active].dec = 0;
    active = stream;

    /* faixa nova: decodifica do corte em diante */
    r = &rings[active];
    r->dec = 0;

    if (cut >= 0)
    {
        for (i = 0; i < r->count; i++)
        {
            s64 pts = r->pts[(r->head + i) % RING_SLOTS];

            if (pts >= 0 && pts >= cut)
                break;
        }

        r->dec = i;
    }

    stats.switches++;

    frame_logf("AUDIO: stream -> %d, cut %d ms, %d frames ready (%d skipped)\n",
               active, cut >= 0 ? (int)(cut / 90) : -1, r->count - r->dec, r->dec);

    /* os primeiros já, para não faltar som; o resto em audio_decode */
    decode_ahead(last_video_pts, DECODE_SWITCH);
}


void audio_decode(int budget)
{
    decode_ahead(last_video_pts, budget);
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


static void decode_frame(u8 *buf, s64 frame_pts)
{
    u64 t0 = GetTimerSystemTime();
    int flags = A52_STEREO | A52_ADJUST_LEVEL;
    sample_t level = 32767.0f;
    int b;
    u32 us;
    PcmFrame *f;
    s16 *pcm;

    /*
     * Troca de PGC: o fim do áudio do clipe anterior pode passar do
     * fim do vídeo dele e se sobrepor ao começo do seguinte (o tempo
     * volta). Os quadros do fim da fila que ficaram à frente deste
     * são descartados; senão o som do clipe novo sai atrasado pelo
     * tamanho da sobra (medido: ~450 ms, para sempre).
     */
    if (frame_pts >= 0)
    {
        while (q_count > (head_sent > 0 ? 1 : 0))
        {
            PcmFrame *tail = &pcm_q[(q_head + q_count - 1) % PCM_QUEUE];

            if (tail->pts < frame_pts)
                break;

            q_count--;
            stats.dropped_overlap++;
        }
    }

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

    if (a52_frame(a52, buf, &flags, &level, 0.0f) != 0)
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


/*
 * Quadro completo de uma faixa: vai para a fila comprimida dela.
 */
static void ring_push(int stream, const u8 *buf, int len, s64 pts)
{
    Ac3Ring *r = &rings[stream];
    int i;

    if (len > SLOT_BYTES)
    {
        stats.errors++;
        return;
    }

    if (!r->data)
    {
        r->data = memalign(64, RING_SLOTS * SLOT_BYTES);

        frame_logf("AUDIO: stream %d buffer %s (%d KB)\n", stream,
                   r->data ? "allocated" : "ALLOCATION FAILED", RING_SLOTS * SLOT_BYTES / 1024);

        if (!r->data)
            return;
    }

    /*
     * Tempo voltou (troca de PGC): descarta o fim ainda não decodificado
     * que ficou à frente deste quadro (o já decodificado é tratado em
     * decode_frame).
     */
    if (pts >= 0)
    {
        int min = stream == active ? r->dec : 0;

        while (r->count > min)
        {
            s64 last = r->pts[(r->head + r->count - 1) % RING_SLOTS];

            if (last < 0 || last < pts)
                break;

            r->count--;

            if (stream == active)
                stats.dropped_overlap++;
        }
    }

    if (r->count == RING_SLOTS)
    {
        /* cheia: descarta o mais antigo */
        r->head = (r->head + 1) % RING_SLOTS;
        r->count--;

        if (r->dec > 0)
            r->dec--;
        else if (stream == active)
            stats.dropped_full++;
    }

    i = (r->head + r->count) % RING_SLOTS;

    memcpy(r->data + i * SLOT_BYTES, buf, len);
    r->len[i] = (u16)len;
    r->pts[i] = pts;
    r->count++;
}


void audio_feed_ac3(int stream, const u8 *data, int len, s64 pts)
{
    Ac3Parser *p;

    if (!a52)
        return;

    stream &= AUDIO_STREAMS - 1;
    p = &parsers[stream];

    if (pts >= 0)
        p->pts_pending = pts;

    /* cabeçalho do DVD: nº de quadros (1) + ponteiro (2) */
    if (len <= 3)
        return;

    data += 3;
    len -= 3;

    while (len > 0)
    {
        int n;

        if (p->len == 0)
        {
            /*
             * Começo de um quadro: o PTS do PES vale para o primeiro
             * quadro que começa nele; os seguintes somam 32 ms.
             */
            if (p->have == 0)
            {
                if (p->pts_pending >= 0)
                {
                    p->frame_pts = p->pts_pending;
                    p->pts_pending = -1;
                }
                else
                    p->frame_pts = p->next_pts;
            }

            /* procurando o sync (0x0B77) + cabeçalho */
            n = AC3_HEADER - p->have;
            if (n > len)
                n = len;

            memcpy(p->buf + p->have, data, n);
            p->have += n;
            data += n;
            len -= n;

            if (p->have < AC3_HEADER)
                break;

            {
                int flags, sr, br;
                int size = a52_syncinfo(p->buf, &flags, &sr, &br);

                if (size <= 0 || size > AC3_MAX_FRAME)
                {
                    /* não sincronizado: avança 1 byte */
                    memmove(p->buf, p->buf + 1, AC3_HEADER - 1);
                    p->have = AC3_HEADER - 1;
                    continue;
                }

                p->len = size;
                p->next_pts = p->frame_pts >= 0 ? p->frame_pts + AC3_FRAME_TICKS : -1;

                if (stream == active)
                {
                    stats.sample_rate = sr;
                    stats.bit_rate = br;
                    stats.channels_flags = flags;
                }
            }
        }

        n = p->len - p->have;
        if (n > len)
            n = len;

        memcpy(p->buf + p->have, data, n);
        p->have += n;
        data += n;
        len -= n;

        if (p->have == p->len)
        {
            ring_push(stream, p->buf, p->len, p->frame_pts);
            p->have = 0;
            p->len = 0;
        }
    }
}


/*
 * Decodifica a faixa ativa até ~DECODE_LEAD à frente da imagem
 * (no máximo budget quadros por chamada).
 */
static void decode_ahead(s64 video_pts, int budget)
{
    Ac3Ring *r = &rings[active];

    if (!r->data)
        return;

    while (budget > 0 && r->dec < r->count && q_count < PCM_QUEUE - 1)
    {
        int i = (r->head + r->dec) % RING_SLOTS;
        s64 pts = r->pts[i];

        if (q_count > 0)
        {
            s64 last = pcm_q[(q_head + q_count - 1) % PCM_QUEUE].pts;

            if (video_pts > 0 && last >= 0 && last > video_pts + DECODE_LEAD)
                break;
        }

        /* sem imagem ainda (abertura, salto): só um pouco adiantado */
        if (video_pts <= 0 && q_count >= 32)
            break;

        r->dec++;

        /* já passou: nem decodifica */
        if (video_pts > 0 && pts >= 0 && pts - video_pts < -SYNC_LATE)
        {
            stats.dropped_late++;
            continue;
        }

        decode_frame(r->data + i * SLOT_BYTES, pts);
        budget--;
    }

    /*
     * Os já decodificados ficam na fila (saem só quando ela enche):
     * voltar para esta faixa logo depois não deixa buraco.
     */
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

    if (video_pts > 0)
        last_video_pts = video_pts;

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
            else if (d - bytes_to_ticks(queued) < -SYNC_CATCHUP)
                stats.dropped_catchup++;
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
