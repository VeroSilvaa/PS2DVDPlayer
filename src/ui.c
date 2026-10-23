#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include <tamtypes.h>
#include <kernel.h>
#include <gsKit.h>

#include "ui.h"
#include "pngload.h"
#include "font_sans_14.h"
#include "buttons.h"

/*
 * PNGs embutidos (assets/, gerados por tools/make_assets.py).
 */
extern unsigned char font_sans_png[];
extern unsigned int size_font_sans_png;

extern unsigned char buttons_png[];
extern unsigned int size_buttons_png;

#define FONT_FIRST   32
#define FONT_PER_ROW 16

/* textura sem modulação (cor 0x80 = 1.0) */
#define TEX_WHITE GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0x00)

#define Z_UI 4

static GSGLOBAL *gs = NULL;
static GSTEXTURE sans;
static GSTEXTURE buttons;
static int sans_ok = 0;
static int buttons_ok = 0;


/* ordem da paleta de 256 cores na VRAM (CSM1): troca 8-15 e 16-23 */
static int clut_pos(int i)
{
    return (i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1);
}


/*
 * Atlas da fonte em 8 bits (o cinza é o índice) com paleta branca de
 * alfa crescente: ~72 KB de VRAM em vez de ~290 KB em RGBA. A cor do
 * texto vem da cor do sprite (modulação).
 */
static int load_font(void)
{
    unsigned char *gray;
    u32 *clut;
    int w, h, i;

    gray = png_decode(font_sans_png, size_font_sans_png, 1, &w, &h);

    if (!gray)
        return -1;

    clut = memalign(128, 256 * 4);

    if (!clut)
    {
        free(gray);
        return -2;
    }

    for (i = 0; i < 256; i++)
        clut[clut_pos(i)] = ((u32)((i + 1) >> 1) << 24) | 0x00FFFFFF;

    memset(&sans, 0, sizeof(sans));
    sans.Width = w;
    sans.Height = h;
    sans.PSM = GS_PSM_T8;
    sans.ClutPSM = GS_PSM_CT32;
    sans.Mem = (u32 *)gray;
    sans.Clut = clut;
    sans.Filter = GS_FILTER_NEAREST;
    sans.Vram = gsKit_vram_alloc(gs, gsKit_texture_size(w, h, GS_PSM_T8), GSKIT_ALLOC_USERBUFFER);
    sans.VramClut = gsKit_vram_alloc(gs, gsKit_texture_size(16, 16, GS_PSM_CT32), GSKIT_ALLOC_USERBUFFER);

    if (sans.Vram == GSKIT_ALLOC_ERROR || sans.VramClut == GSKIT_ALLOC_ERROR)
        return -3;

    FlushCache(0);
    gsKit_texture_upload(gs, &sans);

    return 0;
}


/* Folha de botões RGBA (alfa 0..255 -> 0..128 do GS). */
static int load_buttons(void)
{
    unsigned char *px;
    int w, h, i;

    px = png_decode(buttons_png, size_buttons_png, 4, &w, &h);

    if (!px)
        return -1;

    for (i = 0; i < w * h; i++)
        px[i * 4 + 3] = (unsigned char)((px[i * 4 + 3] + 1) >> 1);

    memset(&buttons, 0, sizeof(buttons));
    buttons.Width = w;
    buttons.Height = h;
    buttons.PSM = GS_PSM_CT32;
    buttons.Mem = (u32 *)px;
    buttons.Filter = GS_FILTER_LINEAR;
    buttons.Vram = gsKit_vram_alloc(gs, gsKit_texture_size(w, h, GS_PSM_CT32), GSKIT_ALLOC_USERBUFFER);

    if (buttons.Vram == GSKIT_ALLOC_ERROR)
        return -3;

    FlushCache(0);
    gsKit_texture_upload(gs, &buttons);

    return 0;
}


int ui_init(GSGLOBAL *g)
{
    u32 before;
    int r;

    gs = g;
    before = gs->CurrentPointer;

    r = load_font();
    sans_ok = r == 0;
    printf("UI: font %dx%d T8 -> %d\n", sans.Width, sans.Height, r);

    r = load_buttons();
    buttons_ok = r == 0;
    printf("UI: buttons %dx%d -> %d\n", buttons.Width, buttons.Height, r);

    printf("UI: VRAM %u KB used by UI, pointer 0x%06x of 0x400000\n",
           (unsigned)((gs->CurrentPointer - before) / 1024), (unsigned)gs->CurrentPointer);

    return sans_ok ? 0 : -1;
}


/* ============================================================
 * Primitivas
 * ============================================================ */

u64 ui_rgba(int r, int g, int b, int a)
{
    return GS_SETREG_RGBAQ(r, g, b, a, 0x00);
}


/* alfa ligado só enquanto a UI desenha */
static void alpha_on(void)
{
    gs->PrimAlphaEnable = GS_SETTING_ON;
    gsKit_set_primalpha(gs, GS_SETREG_ALPHA(0, 1, 0, 1, 0), 0);
}


static void alpha_off(void)
{
    gs->PrimAlphaEnable = GS_SETTING_OFF;
}


void ui_rect(float x, float y, float w, float h, u64 color)
{
    alpha_on();
    gsKit_prim_sprite(gs, x, y, x + w, y + h, Z_UI, color);
    alpha_off();
}


void ui_vgradient(float x, float y, float w, float h, u64 top, u64 bottom)
{
    alpha_on();
    gsKit_prim_quad_gouraud(gs, x, y, x + w, y, x, y + h, x + w, y + h,
                            Z_UI, top, top, bottom, bottom);
    alpha_off();
}


void ui_border(float x, float y, float w, float h, int thick, u64 color)
{
    ui_rect(x, y, w, thick, color);
    ui_rect(x, y + h - thick, w, thick, color);
    ui_rect(x, y + thick, thick, h - 2 * thick, color);
    ui_rect(x + w - thick, y + thick, thick, h - 2 * thick, color);
}


/* ============================================================
 * Texto
 * ============================================================ */

static int char_width(unsigned char c)
{
    if (c < FONT_FIRST)
        return 0;

    return font_sans_width[c - FONT_FIRST];
}


/*
 * Próximo caractere como byte Windows-1252 (o atlas). Textos do
 * programa são UTF-8; nomes de arquivo que não forem UTF-8 válido
 * passam byte a byte. Avança *text.
 */
static unsigned char next_char(const char **text)
{
    const unsigned char *p = (const unsigned char *)*text;
    unsigned int code;

    if (p[0] >= 0xC2 && p[0] <= 0xDF && (p[1] & 0xC0) == 0x80)
    {
        code = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        *text += 2;
        return code <= 0xFF ? (unsigned char)code : '?';
    }

    if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80)
    {
        code = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        *text += 3;

        switch (code)
        {
            case 0x2013: return 0x96;   /* travessão curto */
            case 0x2014: return 0x97;
            case 0x2018: return 0x91;
            case 0x2019: return 0x92;
            case 0x201C: return 0x93;
            case 0x201D: return 0x94;
            case 0x2022: return 0x95;   /* marcador */
            case 0x2026: return 0x85;   /* reticências */
        }

        return '?';
    }

    *text += 1;
    return p[0];
}


int ui_text_width(const char *text)
{
    int width = 0;

    while (*text)
        width += char_width(next_char(&text));

    return width;
}


/*
 * O GS multiplica a textura (branca, 255) pela cor do sprite com 0x80
 * = 1.0: a cor pedida sai certa só pela metade (sem isso, componentes
 * acima de 0x80 saturavam: o azul 5C A8 FF saía ciano B7 FF FF).
 */
static u64 text_color(u64 color)
{
    u32 r = (u32)(color & 0xFF);
    u32 g = (u32)((color >> 8) & 0xFF);
    u32 b = (u32)((color >> 16) & 0xFF);
    u32 a = (u32)((color >> 24) & 0xFF);

    return GS_SETREG_RGBAQ((r + 1) >> 1, (g + 1) >> 1, (b + 1) >> 1, a, 0x00);
}


/* Até length bytes de text (-1 = tudo), na escala scale. */
static int draw_text(float x, float y, float scale, const char *text, int length, u64 color)
{
    const char *end = length < 0 ? NULL : text + length;
    float start = x;

    if (!sans_ok)
        return 0;

    color = text_color(color);

    alpha_on();

    while (*text && (!end || text < end))
    {
        unsigned char c = next_char(&text);
        int width = char_width(c);

        if (c > ' ')
        {
            int index = c - FONT_FIRST;
            float u = (float)((index % FONT_PER_ROW) * FONT_SANS_CELL_W);
            float v = (float)((index / FONT_PER_ROW) * FONT_SANS_CELL_H);

            /* a célula tem 1 px de margem à esquerda (make_assets.py) */
            gsKit_prim_sprite_texture(gs, &sans,
                                      x - scale, y, u, v,
                                      x - scale + FONT_SANS_CELL_W * scale, y + FONT_SANS_CELL_H * scale,
                                      u + FONT_SANS_CELL_W, v + FONT_SANS_CELL_H,
                                      Z_UI, color);
        }

        x += width * scale;
    }

    alpha_off();

    return (int)(x - start);
}


int ui_text(float x, float y, const char *text, u64 color)
{
    return draw_text(x, y, 1.0f, text, -1, color);
}


int ui_text_scaled(float x, float y, float scale, const char *text, u64 color)
{
    return draw_text(x, y, scale, text, -1, color);
}


void ui_text_right(float right, float y, const char *text, u64 color)
{
    draw_text(right - ui_text_width(text), y, 1.0f, text, -1, color);
}


void ui_text_center(float cx, float y, const char *text, u64 color)
{
    draw_text(cx - ui_text_width(text) / 2.0f, y, 1.0f, text, -1, color);
}


void ui_text_fit(float x, float y, float max_w, const char *text, u64 color)
{
    static const char ellipsis[] = "\x85";      /* "..." em Windows-1252 */
    int ellipsis_w = char_width(0x85);
    const char *p = text;
    int width = 0;

    if (ui_text_width(text) <= max_w)
    {
        draw_text(x, y, 1.0f, text, -1, color);
        return;
    }

    while (*p)
    {
        const char *next = p;
        int w = char_width(next_char(&next));

        if (width + w + ellipsis_w > max_w)
            break;

        width += w;
        p = next;
    }

    draw_text(x, y, 1.0f, text, (int)(p - text), color);
    draw_text(x + width, y, 1.0f, ellipsis, -1, color);
}


/* ============================================================
 * Botões do controle (rodapé)
 * ============================================================ */

int ui_button_hint(float x, float y, UiButton button, const char *label)
{
    static const char *names[] = { "", "", "", "", "L1", "R1", "SELECT", "START" };
    u64 text = ui_rgba(0xE6, 0xEE, 0xFF, 0x80);
    float icon_w;

    /* botão recortado do kit (assets/buttons.png) */
    if (buttons_ok && button < (int)(sizeof(button_rects) / sizeof(button_rects[0])) &&
        button_rects[button][2] > 0)
    {
        const unsigned short *r = button_rects[button];
        float top = y + 10 - r[3] / 2.0f;

        alpha_on();
        gsKit_prim_sprite_texture(gs, &buttons,
                                  x, top, r[0], r[1],
                                  x + r[2], top + r[3], r[0] + r[2], r[1] + r[3],
                                  Z_UI, TEX_WHITE);
        alpha_off();

        icon_w = r[2];
        return (int)(icon_w + 5 + ui_text(x + icon_w + 5, y, label, text) + 18);
    }

    /* SELECT/START: tecla escura com o nome, como as do kit */
    if (button == UI_BTN_SELECT || button == UI_BTN_START)
    {
        icon_w = ui_text_width(names[button]) + 10;

        ui_rect(x, y + 1, icon_w, 18, ui_rgba(0x0E, 0x10, 0x14, 0x80));
        ui_vgradient(x + 1, y + 2, icon_w - 2, 16,
                     ui_rgba(0x5A, 0x5E, 0x66, 0x80), ui_rgba(0x2C, 0x2F, 0x35, 0x80));
        ui_text(x + 5, y, names[button], ui_rgba(0xD8, 0xDE, 0xEA, 0x80));

        return (int)(icon_w + 5 + ui_text(x + icon_w + 5, y, label, text) + 18);
    }

    return ui_text(x, y, label, text) + 18;
}
