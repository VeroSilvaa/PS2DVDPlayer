#ifndef PS2DVDPLAYER_OSK_H
#define PS2DVDPLAYER_OSK_H

/*
 * Teclado virtual (do PS2FileManager, mesmo autor), desenhado com a
 * base gráfica (ui.c) numa janela por cima da tela atual.
 *
 * Direcional move | X digita | QUADRADO apaga | R1 espaço
 * TRIÂNGULO troca maiúsculas/minúsculas/símbolos/acentos
 * START confirma | O cancela
 */

#define OSK_MAX_TEXT  128

typedef enum
{
    OSK_EDITING = 0,
    OSK_DONE,
    OSK_CANCEL
} OskResult;

/* secret = 1: o texto aparece como "••••" (senha). */
void osk_open(const char *title, const char *initial, int max_length, int secret);

/* Sem a página de acentos (campos de rede/SMB), depois do osk_open. */
void osk_ascii_only(void);

/* buttons = botões recém-pressionados (máscara PAD_* da libpad). */
OskResult osk_update(unsigned int buttons);

/* Janela por cima da tela atual (chamar a cada quadro). */
void osk_draw(void);

const char *osk_text(void);

#endif
