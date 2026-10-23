#ifndef PS2DVDPLAYER_NAV_H
#define PS2DVDPLAYER_NAV_H

#include <tamtypes.h>

/*
 * Navegação DVD-Video com a libdvdnav (third_party/, GPL v2).
 *
 * A ISO é lida pelos callbacks de stream da libdvdread, com um cache
 * de 128 KB e o bloco seguinte lido em segundo plano (fileXio
 * assíncrono). Enquanto essa leitura está em curso nenhuma outra
 * chamada ao fileXio pode ser feita (inclusive printf): as mensagens
 * vão por frame_logf().
 */

#define NAV_BLOCK_SIZE 2048

enum
{
    NAV_EV_NONE = 0,    /* evento tratado internamente, nada a fazer */
    NAV_EV_BLOCK,       /* buf contém um pack MPEG-PS de 2048 bytes */
    NAV_EV_STILL,       /* still frame: segurar a imagem */
    NAV_EV_STOP,        /* fim da reprodução */
    NAV_EV_ERROR
};

typedef struct
{
    int still_len;          /* NAV_EV_STILL: segundos, 0xff = infinito */
    int domain;             /* 1 First Play, 2 título, 4 VMGM, 8 VTSM */
    int title;
    int buttons;            /* botões no menu atual (pci) */
} NavEvent;

/*
 * prefetch = 1: lê o bloco seguinte em segundo plano (player).
 * prefetch = 0: só leitura síncrona (teste com printf).
 */
int nav_open(const char *path, u64 size, int prefetch);
void nav_close(void);

/*
 * Próximo evento da máquina virtual do DVD. buf: NAV_BLOCK_SIZE
 * bytes, alinhado a 64.
 */
int nav_next(u8 *buf, NavEvent *ev);

/* Encerra o still frame atual (tempo esgotado). */
void nav_still_skip(void);

/*
 * 1 se o próximo nav_next() não precisa esperar a rede (o bloco
 * seguinte já está no cache ou a leitura em segundo plano terminou).
 */
int nav_ready(void);

/* 1 se há leitura assíncrona em curso (fileXio ocupado). */
int nav_io_pending(void);

/* Espera a leitura em segundo plano terminar. */
void nav_wait_io(void);

/* Botões do menu atual (último nav packet), 0 se nenhum. */
int nav_buttons(void);

/*
 * Botões do menu (pci do último nav packet).
 * nav_button_rect: botão atual (1..n) e retângulo em pixels do vídeo;
 * retorna 0 se não há menu.
 */
enum { NAV_UP = 0, NAV_DOWN, NAV_LEFT, NAV_RIGHT };

int nav_button_rect(int *button, int *x0, int *y0, int *x1, int *y1);
int nav_button_move(int dir);
int nav_button_activate(void);

/*
 * Capítulos do título atual (fora do título, ex.: menu, retorna 0).
 * nav_chapter_info: capítulo atual (1..parts) e total.
 * nav_chapter_step: +1 próximo, -1 anterior (no primeiro, recomeça
 * ele); retorna 1 se pulou (depois: frame_nav_flush).
 */
int nav_chapter_info(int *part, int *parts);
int nav_chapter_step(int delta);

/* Vai ao menu do disco; 1 se a VM aceitou (depois: frame_nav_flush). */
int nav_menu(void);

/*
 * Tempo da imagem na tela (pts = PTS do quadro exibido, na linha
 * contínua): posição e duração do título em ms, capítulo e total.
 * Retorna 0 fora do título ou antes do primeiro nav packet.
 */
int nav_play_time(s64 pts, u32 *ms, u32 *total_ms, int *part, int *parts);

/*
 * Paleta de 16 cores do subpicture (0x00YYCrCb), atualizada pelo
 * disco; nav_clut_serial() muda a cada troca.
 */
const u32 *nav_clut(void);
u32 nav_clut_serial(void);

/* Stream de áudio ativo (0..7): o do usuário, se escolheu; senão o da VM. */
int nav_audio_stream(void);

/*
 * Faixas de áudio AC3 do título atual.
 * nav_audio_info: descrição da faixa ativa ("AUDIO 1/2: AC3 2.0 (pt)"),
 * retorna o número de faixas (0 = nenhuma/fora do título).
 * nav_audio_cycle: passa para a próxima faixa; 1 se trocou (depois:
 * audio_resync). label recebe a descrição da faixa nova.
 */
int nav_audio_info(char *label, int size);
int nav_audio_cycle(char *label, int size);

/*
 * Legendas do título atual: quantas faixas existem (0 fora do título).
 * label: "NO SUBTITLES" ou a contagem (exibição ainda não suportada).
 */
int nav_spu_count(char *label, int size);

/*
 * Deslocamento (90 kHz) a somar aos PTS dos packs seguintes ao último
 * nav packet, para que a linha do tempo continue entre PGCs/células
 * cujos PTS recomeçam (vobu_s_ptm != vobu_e_ptm anterior).
 */
s64 nav_pts_offset(void);

/* Stream de subpicture ativo (0..31). */
int nav_spu_stream(void);

/*
 * Cores do botão atual: action 0 = selecionado, 1 = ativado.
 * Formato do DVD: cores em 31..16 (e2 e1 p b), contraste em 15..0.
 * Retorna 0 se o botão não tem cor própria.
 */
int nav_button_palette(int action, u32 *palette);

/* Bytes lidos da rede desde nav_open(). */
u64 nav_bytes_read(void);

/*
 * Teste (R1 na tela DVD INFO): executa a VM a partir do First Play e
 * registra os eventos no log, sem exibir nada.
 */
int nav_test(const char *path, u64 size);

#endif
