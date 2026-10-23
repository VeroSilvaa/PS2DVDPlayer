#ifndef PS2DVDPLAYER_AUDIO_H
#define PS2DVDPLAYER_AUDIO_H

#include <tamtypes.h>

/*
 * Áudio AC3 do DVD (liba52, third_party/liba52, GPL v2).
 *
 * Todas as faixas AC3 ficam guardadas comprimidas (~8 s cada); só a
 * ativa é decodificada para PCM estéreo 16 bits / 48 kHz, ~1 s à
 * frente da imagem. Cada quadro (32 ms) entra numa fila com o seu PTS
 * e só vai para o audsrv (IOP) quando o vídeo na tela alcança esse PTS.
 */

/*
 * Carrega LIBSD + AUDSRV no IOP e configura 48 kHz estéreo.
 * Chamar uma vez no boot. Retorna < 0 se não houver som.
 */
int audio_init_output(void);

/* Descarta a fila de PCM (abertura, salto). */
void audio_reset(void);

/*
 * Faixa decodificada (0..7, substream 0x80 + n). Na troca, o PCM que
 * ainda não começou a tocar é descartado e a faixa nova é decodificada
 * do mesmo ponto (troca ouvida na hora). Sem efeito se já é a ativa.
 */
void audio_select_stream(int stream);

/* Silencia o audsrv (fechar o player). */
void audio_stop(void);

/*
 * stream: n do substream 0x80 + n (todas as faixas são guardadas).
 * data/len: payload do PES 0xBD depois do byte de substream:
 * 3 bytes de cabeçalho do DVD e depois o fluxo AC3.
 * pts: PTS do PES (90 kHz) ou -1 se o PES não tem PTS.
 */
void audio_feed_ac3(int stream, const u8 *data, int len, s64 pts);

/*
 * Envia ao audsrv os quadros cujo PTS o vídeo já alcançou.
 * video_pts: PTS do quadro na tela (<= 0 se desconhecido).
 * Não decodifica (pode ser chamado logo após uma imagem).
 */
void audio_pump(s64 video_pts);

/*
 * Decodifica até budget quadros da faixa ativa (~1 s à frente da
 * imagem). Chamar só no tempo livre entre imagens (frame_pump).
 */
void audio_decode(int budget);

typedef struct
{
    u32 frames;             /* quadros AC3 decodificados */
    u32 errors;
    int sample_rate;
    int bit_rate;
    int channels_flags;     /* acmod/flags do último quadro */
    u32 decode_us_total;    /* tempo total de decodificação */
    u32 decode_us_max;

    u32 played;             /* quadros enviados ao audsrv */
    u32 dropped_late;       /* descartados: atrasados */
    u32 dropped_jump;       /* descartados: PTS muito à frente (salto) */
    u32 dropped_full;       /* descartados: fila de PCM cheia */
    u32 dropped_overlap;    /* descartados: sobrepostos ao trecho seguinte */
    u32 dropped_catchup;    /* descartados: recuperação de atraso pequeno */
    u32 switches;           /* trocas de faixa */
    int queued;             /* quadros esperando na fila */
    u32 silence;            /* bytes de silêncio enviados ao audsrv */
    u32 rpc;                /* chamadas ao audsrv (RPC) */

    /* sincronia medida: som saindo - imagem na tela (90 kHz) */
    s64 av_now;
    s64 av_min;
    s64 av_max;
    s64 av_sum;
    u32 av_count;
    s64 last_diff;          /* PTS áudio - vídeo no último envio */
    int output;             /* 1 se o audsrv está funcionando */
} AudioStats;

const AudioStats *audio_stats(void);

#endif
