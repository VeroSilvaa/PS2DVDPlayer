#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <math.h>

#include <tamtypes.h>
#include <kernel.h>
#include <gsKit.h>

#include "ui.h"
#include "pngload.h"
#include "font_body.h"
#include "font_large.h"
#include "buttons.h"

/*
 * PNGs embutidos (assets/, gerados por tools/make_assets.py).
 */
extern unsigned char font_body_png[];
extern unsigned int size_font_body_png;

extern unsigned char font_large_png[];
extern unsigned int size_font_large_png;

extern unsigned char logo_png[];
extern unsigned int size_logo_png;

extern unsigned char buttons_png[];
extern unsigned int size_buttons_png;

#define FONT_FIRST   32
#define FONT_PER_ROW 16

/* textura sem modulação (cor 0x80 = 1.0) */
#define TEX_WHITE GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0x00)

#define Z_UI 4

typedef struct
{
    GSTEXTURE tex;
    const unsigned char *width;     /* avanço de cada caractere (32..255) */
    int cell_w;
    int cell_h;
    int ok;
} UiFontData;

static GSGLOBAL *gs = NULL;
static UiFontData fonts[UI_FONTS];
static GSTEXTURE buttons;
static GSTEXTURE logo;
static int buttons_ok = 0;
static int logo_ok = 0;


/*
 * Atlas da fonte em 4 bits (16 níveis de cinza = índice) com paleta
 * branca de alfa crescente: 1/8 da VRAM do RGBA. Gerado já no tamanho
 * usado (ampliar um atlas pequeno serrilhava as letras). A cor do
 * texto vem da cor do sprite (modulação).
 */
static int load_font(UiFontData *f, const unsigned char *png, unsigned int size,
                     const unsigned char *widths, int cell_w, int cell_h)
{
    unsigned char *gray;
    unsigned char *packed;
    u32 *clut;
    int w, h, i;

    gray = png_decode(png, size, 1, &w, &h);

    if (!gray)
        return -1;

    packed = memalign(128, (w * h + 1) / 2);
    clut = memalign(128, 8 * 2 * 4);

    if (!packed || !clut)
    {
        free(gray);
        return -2;
    }

    /* 2 pixels por byte: o da esquerda no nibble baixo */
    memset(packed, 0, (w * h + 1) / 2);

    for (i = 0; i < w * h; i++)
        packed[i >> 1] |= (unsigned char)((gray[i] >> 4) << ((i & 1) * 4));

    free(gray);

    for (i = 0; i < 16; i++)
        clut[i] = ((u32)((i * 0x80 + 7) / 15) << 24) | 0x00FFFFFF;

    memset(&f->tex, 0, sizeof(f->tex));
    f->tex.Width = w;
    f->tex.Height = h;
    f->tex.PSM = GS_PSM_T4;
    f->tex.ClutPSM = GS_PSM_CT32;
    f->tex.Mem = (u32 *)packed;
    f->tex.Clut = clut;
    f->tex.Filter = GS_FILTER_NEAREST;
    f->tex.Vram = gsKit_vram_alloc(gs, gsKit_texture_size(w, h, GS_PSM_T4), GSKIT_ALLOC_USERBUFFER);
    f->tex.VramClut = gsKit_vram_alloc(gs, gsKit_texture_size(8, 2, GS_PSM_CT32), GSKIT_ALLOC_USERBUFFER);

    if (f->tex.Vram == GSKIT_ALLOC_ERROR || f->tex.VramClut == GSKIT_ALLOC_ERROR)
        return -3;

    FlushCache(0);
    gsKit_texture_upload(gs, &f->tex);

    f->width = widths;
    f->cell_w = cell_w;
    f->cell_h = cell_h;
    f->ok = 1;

    return 0;
}


/* Imagem RGBA (alfa 0..255 -> 0..128 do GS). */
static int load_rgba(const unsigned char *png, unsigned int size, GSTEXTURE *tex)
{
    unsigned char *px;
    int w, h, i;

    px = png_decode(png, size, 4, &w, &h);

    if (!px)
        return -1;

    for (i = 0; i < w * h; i++)
        px[i * 4 + 3] = (unsigned char)((px[i * 4 + 3] + 1) >> 1);

    memset(tex, 0, sizeof(*tex));
    tex->Width = w;
    tex->Height = h;
    tex->PSM = GS_PSM_CT32;
    tex->Mem = (u32 *)px;
    tex->Filter = GS_FILTER_LINEAR;
    tex->Vram = gsKit_vram_alloc(gs, gsKit_texture_size(w, h, GS_PSM_CT32), GSKIT_ALLOC_USERBUFFER);

    if (tex->Vram == GSKIT_ALLOC_ERROR)
        return -3;

    FlushCache(0);
    gsKit_texture_upload(gs, tex);

    return 0;
}


int ui_init(GSGLOBAL *g)
{
    u32 before;
    int r;

    gs = g;
    before = gs->CurrentPointer;

    r = load_font(&fonts[UI_FONT_BODY], font_body_png, size_font_body_png,
                  font_body_width, FONT_BODY_CELL_W, FONT_BODY_CELL_H);
    printf("UI: font body %dx%d T4 -> %d\n", fonts[UI_FONT_BODY].tex.Width, fonts[UI_FONT_BODY].tex.Height, r);

    r = load_font(&fonts[UI_FONT_LARGE], font_large_png, size_font_large_png,
                  font_large_width, FONT_LARGE_CELL_W, FONT_LARGE_CELL_H);
    printf("UI: font large %dx%d T4 -> %d\n", fonts[UI_FONT_LARGE].tex.Width, fonts[UI_FONT_LARGE].tex.Height, r);

    r = load_rgba(logo_png, size_logo_png, &logo);
    logo_ok = r == 0;
    printf("UI: logo %dx%d -> %d\n", logo.Width, logo.Height, r);

    r = load_rgba(buttons_png, size_buttons_png, &buttons);
    buttons_ok = r == 0;
    printf("UI: buttons %dx%d -> %d\n", buttons.Width, buttons.Height, r);

    printf("UI: VRAM %u KB used by UI, pointer 0x%06x of 0x400000\n",
           (unsigned)((gs->CurrentPointer - before) / 1024), (unsigned)gs->CurrentPointer);

    return fonts[UI_FONT_BODY].ok ? 0 : -1;
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


void ui_triangle(float x1, float y1, float x2, float y2, float x3, float y3, u64 color)
{
    alpha_on();
    gsKit_prim_triangle(gs, x1, y1, x2, y2, x3, y3, Z_UI, color);
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

static int char_width(const UiFontData *f, unsigned char c)
{
    if (c < FONT_FIRST || !f->width)
        return 0;

    return f->width[c - FONT_FIRST];
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


int ui_font_height(UiFont font)
{
    return fonts[font].cell_h;
}


int ui_text_width_f(UiFont font, const char *text)
{
    const UiFontData *f = &fonts[font];
    int width = 0;

    while (*text)
        width += char_width(f, next_char(&text));

    return width;
}


int ui_text_width(const char *text)
{
    return ui_text_width_f(UI_FONT_BODY, text);
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


/* Até length bytes de text (-1 = tudo). */
static int draw_text(UiFont font, float x, float y, const char *text, int length, u64 color)
{
    UiFontData *f = &fonts[font];
    const char *end = length < 0 ? NULL : text + length;
    float start = x;

    if (!f->ok)
        return 0;

    color = text_color(color);

    alpha_on();

    while (*text && (!end || text < end))
    {
        unsigned char c = next_char(&text);
        int width = char_width(f, c);

        if (c > ' ')
        {
            int index = c - FONT_FIRST;
            float u = (float)((index % FONT_PER_ROW) * f->cell_w);
            float v = (float)((index / FONT_PER_ROW) * f->cell_h);

            /*
             * A célula tem 1 px de margem à esquerda (make_assets.py) e
             * a última coluna/linha vazias: o sprite vai até antes
             * delas, senão o GS lia 1 texel da célula vizinha (um ponto
             * aparecia depois do "P").
             */
            gsKit_prim_sprite_texture(gs, &f->tex,
                                      x - 1, y, u, v,
                                      x - 2 + f->cell_w, y + f->cell_h - 1,
                                      u + f->cell_w - 1, v + f->cell_h - 1,
                                      Z_UI, color);
        }

        x += width;
    }

    alpha_off();

    return (int)(x - start);
}


int ui_text_f(UiFont font, float x, float y, const char *text, u64 color)
{
    return draw_text(font, x, y, text, -1, color);
}


int ui_text(float x, float y, const char *text, u64 color)
{
    return draw_text(UI_FONT_BODY, x, y, text, -1, color);
}


void ui_text_right_f(UiFont font, float right, float y, const char *text, u64 color)
{
    draw_text(font, right - ui_text_width_f(font, text), y, text, -1, color);
}


void ui_text_right(float right, float y, const char *text, u64 color)
{
    ui_text_right_f(UI_FONT_BODY, right, y, text, color);
}


void ui_text_center_f(UiFont font, float cx, float y, const char *text, u64 color)
{
    draw_text(font, cx - ui_text_width_f(font, text) / 2.0f, y, text, -1, color);
}


void ui_text_center(float cx, float y, const char *text, u64 color)
{
    ui_text_center_f(UI_FONT_BODY, cx, y, text, color);
}


void ui_text_fit_f(UiFont font, float x, float y, float max_w, const char *text, u64 color)
{
    static const char ellipsis[] = "\x85";      /* "..." em Windows-1252 */
    const UiFontData *f = &fonts[font];
    int ellipsis_w = char_width(f, 0x85);
    const char *p = text;
    int width = 0;

    if (ui_text_width_f(font, text) <= max_w)
    {
        draw_text(font, x, y, text, -1, color);
        return;
    }

    while (*p)
    {
        const char *next = p;
        int w = char_width(f, next_char(&next));

        if (width + w + ellipsis_w > max_w)
            break;

        width += w;
        p = next;
    }

    draw_text(font, x, y, text, (int)(p - text), color);
    draw_text(font, x + width, y, ellipsis, -1, color);
}


void ui_text_fit(float x, float y, float max_w, const char *text, u64 color)
{
    ui_text_fit_f(UI_FONT_BODY, x, y, max_w, text, color);
}


/* ============================================================
 * Logo
 * ============================================================ */

int ui_logo(float x, float y)
{
    if (!logo_ok)
        return 0;

    alpha_on();
    gsKit_prim_sprite_texture(gs, &logo,
                              x, y, 0, 0,
                              x + logo.Width, y + logo.Height, logo.Width, logo.Height,
                              Z_UI, TEX_WHITE);
    alpha_off();

    return logo.Width;
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
        float top = y + fonts[UI_FONT_BODY].cell_h / 2.0f - r[3] / 2.0f;

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

        ui_rect(x, y + 3, icon_w, 22, ui_rgba(0x0E, 0x10, 0x14, 0x80));
        ui_vgradient(x + 1, y + 4, icon_w - 2, 20,
                     ui_rgba(0x5A, 0x5E, 0x66, 0x80), ui_rgba(0x2C, 0x2F, 0x35, 0x80));
        ui_text(x + 5, y, names[button], ui_rgba(0xD8, 0xDE, 0xEA, 0x80));

        return (int)(icon_w + 5 + ui_text(x + icon_w + 5, y, label, text) + 18);
    }

    return ui_text(x, y, label, text) + 18;
}


/* ============================================================
 * Ícones (desenhados pelo GS)
 * ============================================================ */

#define CIRCLE_SEGMENTS 32

void ui_circle(float cx, float cy, float r, u64 inner, u64 outer)
{
    static float cs[CIRCLE_SEGMENTS + 1], sn[CIRCLE_SEGMENTS + 1];
    static int table = 0;
    int i;

    if (!table)
    {
        for (i = 0; i <= CIRCLE_SEGMENTS; i++)
        {
            float a = (float)i * 6.2831853f / CIRCLE_SEGMENTS;

            cs[i] = cosf(a);
            sn[i] = sinf(a);
        }

        table = 1;
    }

    alpha_on();

    for (i = 0; i < CIRCLE_SEGMENTS; i++)
    {
        gsKit_prim_triangle_gouraud(gs,
                                    cx, cy,
                                    cx + r * cs[i], cy + r * sn[i],
                                    cx + r * cs[i + 1], cy + r * sn[i + 1],
                                    Z_UI, inner, outer, outer);
    }

    alpha_off();
}


/* disco prateado com o furo do centro (logo do modelo) */
static void icon_disc(float x, float y, float s)
{
    float c = s / 2;

    ui_circle(x + c, y + c, c, ui_rgba(0xE8, 0xEC, 0xF2, 0x80), ui_rgba(0x7A, 0x82, 0x90, 0x80));
    ui_circle(x + c, y + c, c * 0.92f, ui_rgba(0xF4, 0xF6, 0xFA, 0x80), ui_rgba(0x9A, 0xA2, 0xB0, 0x80));
    ui_circle(x + c, y + c, c * 0.34f, ui_rgba(0x60, 0x66, 0x70, 0x80), ui_rgba(0x8A, 0x90, 0x9A, 0x80));
    ui_circle(x + c, y + c, c * 0.24f, ui_rgba(0xC8, 0xCC, 0xD4, 0x80), ui_rgba(0xA0, 0xA6, 0xB0, 0x80));
    ui_circle(x + c, y + c, c * 0.10f, ui_rgba(0x10, 0x14, 0x1C, 0x80), ui_rgba(0x24, 0x28, 0x30, 0x80));
}


/* monitor: moldura clara, tela azul, pé */
static void icon_network(float x, float y, float s)
{
    float u = s / 40;
    u64 frame = ui_rgba(0xE8, 0xF0, 0xFF, 0x80);

    ui_rect(x + 2 * u, y + 5 * u, 36 * u, 24 * u, frame);
    ui_vgradient(x + 5 * u, y + 8 * u, 30 * u, 18 * u,
                 ui_rgba(0x4C, 0x9C, 0xFF, 0x80), ui_rgba(0x16, 0x4C, 0xC0, 0x80));
    ui_rect(x + 17 * u, y + 29 * u, 6 * u, 5 * u, frame);
    ui_rect(x + 10 * u, y + 34 * u, 20 * u, 3 * u, frame);
}


/* pendrive: conector com os dois furos e corpo prateado */
static void icon_usb(float x, float y, float s)
{
    float u = s / 40;
    u64 dark = ui_rgba(0x3A, 0x40, 0x4C, 0x80);

    ui_rect(x + 14 * u, y + 2 * u, 12 * u, 11 * u, ui_rgba(0xD8, 0xDE, 0xE8, 0x80));
    ui_rect(x + 16 * u, y + 5 * u, 3 * u, 3 * u, dark);
    ui_rect(x + 21 * u, y + 5 * u, 3 * u, 3 * u, dark);
    ui_vgradient(x + 11 * u, y + 13 * u, 18 * u, 25 * u,
                 ui_rgba(0xC8, 0xCE, 0xD8, 0x80), ui_rgba(0x8A, 0x92, 0xA0, 0x80));
    ui_rect(x + 19 * u, y + 18 * u, 2 * u, 15 * u, dark);
}


/* disco rígido: caixa prateada, fenda e três pontos */
static void icon_hdd(float x, float y, float s)
{
    float u = s / 40;
    u64 dark = ui_rgba(0x3A, 0x40, 0x4C, 0x80);

    ui_vgradient(x + 4 * u, y + 6 * u, 32 * u, 28 * u,
                 ui_rgba(0xD4, 0xDA, 0xE4, 0x80), ui_rgba(0x8C, 0x94, 0xA2, 0x80));
    ui_border(x + 4 * u, y + 6 * u, 32 * u, 28 * u, 2, ui_rgba(0x5A, 0x62, 0x70, 0x80));
    ui_rect(x + 9 * u, y + 11 * u, 22 * u, 3 * u, dark);
    ui_rect(x + 10 * u, y + 26 * u, 4 * u, 4 * u, dark);
    ui_rect(x + 18 * u, y + 26 * u, 4 * u, 4 * u, dark);
    ui_rect(x + 26 * u, y + 26 * u, 4 * u, 4 * u, dark);
}


void ui_icon(UiIcon icon, float x, float y, float size)
{
    switch (icon)
    {
        case UI_ICON_DISC:    icon_disc(x, y, size); break;
        case UI_ICON_NETWORK: icon_network(x, y, size); break;
        case UI_ICON_USB:     icon_usb(x, y, size); break;
        case UI_ICON_HDD:     icon_hdd(x, y, size); break;
    }
}