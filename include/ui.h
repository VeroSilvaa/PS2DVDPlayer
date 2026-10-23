#ifndef PS2DVDPLAYER_UI_H
#define PS2DVDPLAYER_UI_H

#include <tamtypes.h>
#include <gsKit.h>

/*
 * Base gráfica da interface (vinda do PS2FileManager, mesmo autor):
 * fonte proporcional Noto Sans 14 (OFL, third_party/fonts), botões
 * recortados do "Kit de Botões de Controle PlayStation.png"
 * (tools/make_assets.py) e primitivas desenhadas pelo GS.
 *
 * Tela de 640x480. As texturas vão para a VRAM uma vez em ui_init
 * (a fonte em 8 bits com paleta: ~72 KB); fundo, painéis e bordas
 * são desenhados por código (sem VRAM).
 *
 * Texto em UTF-8 (acentos do português), convertido para o atlas
 * Windows-1252.
 */

/* Ordem do assets/buttons.h */
typedef enum
{
    UI_BTN_CROSS = 0,
    UI_BTN_CIRCLE,
    UI_BTN_SQUARE,
    UI_BTN_TRIANGLE,
    UI_BTN_L1,
    UI_BTN_R1,
    UI_BTN_SELECT,
    UI_BTN_START,
    UI_BTN_UP,
    UI_BTN_DOWN,
    UI_BTN_LEFT,
    UI_BTN_RIGHT,
    UI_BTN_L2,
    UI_BTN_R2
} UiButton;

/* Altura de uma linha de texto (célula do atlas). */
#define UI_LINE_H 20

/* 0 = pronto (texturas na VRAM); < 0 = erro (texto não aparece). */
int ui_init(GSGLOBAL *gs);

/* Cor do GS: componentes 0..255, alfa 0..128 (0x80 = opaco). */
u64 ui_rgba(int r, int g, int b, int a);

void ui_rect(float x, float y, float w, float h, u64 color);
void ui_vgradient(float x, float y, float w, float h, u64 top, u64 bottom);

/* Borda de thick px por dentro do retângulo (use 2: o 1 px cintila). */
void ui_border(float x, float y, float w, float h, int thick, u64 color);

/*
 * Texto (y = topo da linha de UI_LINE_H px). Devolve a largura.
 * scale: 1.0 = 14 px; 2.0 para títulos.
 */
int ui_text(float x, float y, const char *text, u64 color);
int ui_text_scaled(float x, float y, float scale, const char *text, u64 color);
int ui_text_width(const char *text);
void ui_text_right(float right, float y, const char *text, u64 color);
void ui_text_center(float cx, float y, const char *text, u64 color);

/* Corta com "..." para caber em max_w. */
void ui_text_fit(float x, float y, float max_w, const char *text, u64 color);

/*
 * Ícone do botão + rótulo (rodapé). Devolve a largura usada
 * (incluindo o espaço até o próximo par).
 */
int ui_button_hint(float x, float y, UiButton button, const char *label);

#endif
