#ifndef PS2DVDPLAYER_UI_H
#define PS2DVDPLAYER_UI_H

#include <tamtypes.h>
#include <gsKit.h>

/*
 * Base gráfica da interface (vinda do PS2FileManager, mesmo autor):
 * fontes proporcionais Noto Sans 18 e 26 px e logo em Noto Sans Bold
 * (OFL, third_party/fonts), botões recortados do
 * "Kit de Botões de Controle PlayStation.png" (tools/make_assets.py)
 * e primitivas desenhadas pelo GS.
 *
 * Tela de 640x480. As texturas vão para a VRAM uma vez em ui_init
 * (fontes em 4 bits com paleta); fundo, painéis, bordas e ícones são
 * desenhados por código (sem VRAM).
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

/*
 * Fontes geradas já no tamanho usado (tools/make_assets.py): ampliar
 * um atlas pequeno serrilhava as letras.
 */
typedef enum
{
    UI_FONT_BODY = 0,       /* Noto Sans 18 px: texto normal */
    UI_FONT_LARGE,          /* Noto Sans 26 px: títulos de opções */
    UI_FONTS
} UiFont;

/* Altura de uma linha da fonte (célula do atlas): 28 / 38 px. */
int ui_font_height(UiFont font);

/* 0 = pronto (texturas na VRAM); < 0 = erro (texto não aparece). */
int ui_init(GSGLOBAL *gs);

/* Cor do GS: componentes 0..255, alfa 0..128 (0x80 = opaco). */
u64 ui_rgba(int r, int g, int b, int a);

void ui_rect(float x, float y, float w, float h, u64 color);
void ui_vgradient(float x, float y, float w, float h, u64 top, u64 bottom);

/* Borda de thick px por dentro do retângulo (use 2: o 1 px cintila). */
void ui_border(float x, float y, float w, float h, int thick, u64 color);

/*
 * Texto (y = topo da linha). Devolve a largura. As versões _f
 * recebem a fonte; as outras usam UI_FONT_BODY.
 */
int ui_text_f(UiFont font, float x, float y, const char *text, u64 color);
int ui_text_width_f(UiFont font, const char *text);
void ui_text_right_f(UiFont font, float right, float y, const char *text, u64 color);
void ui_text_center_f(UiFont font, float cx, float y, const char *text, u64 color);

/* Corta com "..." para caber em max_w. */
void ui_text_fit_f(UiFont font, float x, float y, float max_w, const char *text, u64 color);

int ui_text(float x, float y, const char *text, u64 color);
int ui_text_width(const char *text);
void ui_text_right(float right, float y, const char *text, u64 color);
void ui_text_center(float cx, float y, const char *text, u64 color);
void ui_text_fit(float x, float y, float max_w, const char *text, u64 color);

/* Logo "PS2DVDPlayer" (assets/logo.png, 291x52). Devolve a largura. */
int ui_logo(float x, float y);

/*
 * Ícone do botão + rótulo (rodapé). Devolve a largura usada
 * (incluindo o espaço até o próximo par).
 */
int ui_button_hint(float x, float y, UiButton button, const char *label);

/*
 * Ícones desenhados pelo GS (como os do interface-model.png), num
 * quadrado de size px a partir de (x, y). Sem textura (sem VRAM).
 */
typedef enum
{
    UI_ICON_DISC = 0,       /* disco prateado (logo) */
    UI_ICON_NETWORK,        /* monitor: SMB */
    UI_ICON_USB,            /* pendrive */
    UI_ICON_HDD             /* disco rígido */
} UiIcon;

void ui_icon(UiIcon icon, float x, float y, float size);

/* Círculo cheio com gradiente do centro (inner) para a borda (outer). */
void ui_circle(float cx, float cy, float r, u64 inner, u64 outer);

#endif
