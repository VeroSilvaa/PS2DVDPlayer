#ifndef PS2DVDPLAYER_FRAME_H
#define PS2DVDPLAYER_FRAME_H

#include <tamtypes.h>

/*
 * Reprodução de vídeo de um VOB (sem áudio ainda):
 * leitura assíncrona da ISO por SMB em blocos, demux do vídeo
 * MPEG-2 (PES 0xE0) para um buffer circular e decodificação de
 * quadros com a libmpeg local (IPU + conversão de cor MMI).
 */

#define FRAME_MAX_WIDTH  720
#define FRAME_MAX_HEIGHT 576

/*
 * 1: conversão de cor pela GS (o quadro sai em planos Y/Cb/Cr, ver
 * frame_planes); 0: conversão em software para CT16 em dst.
 */
#define FRAME_GS_CSC 1

typedef struct
{
    int width;
    int height;
    int frames;            /* quadros decodificados */
    u32 frame_us;          /* duração de um quadro (sequence header) */

    u32 decode_ms;         /* tempo do último quadro */

    u32 buffered;          /* bytes de vídeo prontos no buffer */
    u32 buffer_size;
    u64 read_total;        /* bytes do VOB já lidos */
    u64 read_limit;        /* bytes a ler (VOBs do título) */
    u32 read_kbps;         /* vazão média da leitura */
    int eof;               /* leitura terminou */

    /* navegação (frame_open_nav) */
    int nav;
    int still;             /* still frame do DVD em curso */
    int still_len;         /* segundos, 0xff = infinito */
    u32 still_left_ms;
    int buttons;           /* botões do menu atual (na tela) */
    u32 spu_serial;        /* muda quando o subpicture na tela muda */
    s64 pts;               /* PTS do quadro na tela (90 kHz), -1 se não há */

    char error[96];
} FrameInfo;

/*
 * Abre o vídeo a partir do setor vob_lba da ISO (vob_bytes bytes,
 * VOBs contíguos) e começa a ler em segundo plano.
 */
int frame_open(
    const char *path,
    u32 vob_lba,
    u64 vob_bytes,
    FrameInfo *info
);

/*
 * Abre a ISO com a navegação do DVD (libdvdnav): começa no First
 * Play e segue os comandos do disco (avisos, menus, stills).
 */
int frame_open_nav(
    const char *path,
    u64 iso_size,
    FrameInfo *info
);

/*
 * Salto do DVD (ex.: botão de menu ativado): descarta o vídeo no
 * buffer e reinicia o decodificador.
 */
void frame_nav_flush(FrameInfo *info);

/*
 * 1 se um still frame está na tela e não se deve chamar frame_next
 * (a imagem fica parada até o tempo do disco acabar).
 */
int frame_still_hold(const FrameInfo *info);

/*
 * Atende a leitura assíncrona (chamar a cada volta do laço).
 */
void frame_pump(FrameInfo *info);

/*
 * 1 se há vídeo suficiente no buffer para decodificar um quadro
 * sem esperar a rede (ou se a leitura já terminou).
 */
int frame_ready(const FrameInfo *info);

/*
 * Decodifica o próximo quadro em dst (RGBA 16 bits linear,
 * largura info->width). Retorna 1 com quadro, 0 no fim, < 0 erro,
 * 2 sem quadro novo (navegação: fim de sequência, o disco continua).
 */
int frame_next(
    u16 *dst,
    FrameInfo *info
);

/*
 * Como frame_next, mas sem converter a imagem (quadro atrasado que
 * não vai para a tela): dst não é alterado.
 */
int frame_next_skip(
    u16 *dst,
    FrameInfo *info
);

void frame_close(void);

/*
 * Planos do último quadro (FRAME_GS_CSC): Y com largura info->width,
 * Cb e Cr com metade da largura e da altura.
 */
void frame_planes(const u8 **y, const u8 **cb, const u8 **cr);

/*
 * O printf do PS2SDK passa pelo fileXio (tty: no IOP). Com uma
 * leitura assíncrona em curso, qualquer chamada ao fileXio (inclusive
 * printf) se perde ou corrompe o resultado da leitura.
 *
 * frame_read_idle(): 1 se não há leitura em curso (pode usar printf).
 * frame_logf(): guarda a mensagem; é escrita quando a leitura parar.
 * frame_log_flush(): escreve as mensagens guardadas, se possível.
 */
int frame_read_idle(void);
void frame_logf(const char *fmt, ...);
void frame_log_flush(void);

#endif
