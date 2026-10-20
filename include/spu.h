#ifndef PS2DVDPLAYER_SPU_H
#define PS2DVDPLAYER_SPU_H

#include <tamtypes.h>

/*
 * Subpicture do DVD (SPU): legendas e gráficos dos botões de menu.
 *
 * Os pacotes chegam no PES 0xBD (substream 0x20 + n), são remontados,
 * e cada pacote completo fica numa fila marcado com a posição do
 * buffer de vídeo; quando o decodificador de vídeo passa dessa
 * posição o pacote é decodificado num bitmap de índices:
 *   0..3  cor/contraste do próprio SPU (fundo, padrão, ênfase 1 e 2)
 *   0xFF  transparente (fora da área)
 */

#define SPU_MAX_W 720
#define SPU_MAX_H 576
#define SPU_NONE  0xFF

typedef struct
{
    int visible;            /* start/forced display recebido */
    int forced;             /* forced start (menus) */
    int x0, y0, x1, y1;     /* área desenhada */
    u8 color[4];            /* índice na paleta de 16 cores */
    u8 alpha[4];            /* contraste 0..15 */
    u32 serial;             /* muda a cada SPU novo */
} SpuInfo;

void spu_reset(void);

/*
 * data/len: payload do PES depois do byte de substream; has_pts: o
 * PES traz PTS (início de pacote SPU); pos: posição do buffer de
 * vídeo no momento (para sincronizar a exibição).
 */
void spu_feed(const u8 *data, int len, int has_pts, u32 pos);

/*
 * Decodifica os pacotes cuja posição já foi consumida pelo vídeo.
 * Retorna 1 se o SPU exibido mudou.
 */
int spu_update(u32 consumed);

const SpuInfo *spu_info(void);
const u8 *spu_bitmap(void);     /* SPU_MAX_W x SPU_MAX_H */

#endif
