#include <stdio.h>
#include <string.h>

#include <tamtypes.h>
#include <timer.h>
#include <libpad.h>

#include "osk.h"
#include "ui.h"

/*
 * Do PS2FileManager (mesmo autor): a lógica é a mesma; o desenho usa
 * a base gráfica deste projeto (ui.c).
 *
 * Páginas do teclado (texto UTF-8). Sem / \ : * ? " < > | (inválidos
 * em nome de arquivo no FAT/SMB).
 */
#define OSK_PAGES  4
#define OSK_ROWS   4

/* Linha das ações, abaixo das letras. */
#define OSK_ACTION_ROW  OSK_ROWS

enum
{
    OSK_ACTION_PAGE = 0,
    OSK_ACTION_SPACE,
    OSK_ACTION_DELETE,
    OSK_ACTION_DONE
};

#define OSK_ACTIONS 4

#define OSK_PAGE_ACCENTS  3

static const char *osk_pages[OSK_PAGES][OSK_ROWS] = {
    { "ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ-_.!", "0123456789" },
    { "abcdefghij", "klmnopqrst", "uvwxyz-_.!", "0123456789" },
    { "#$%&'()+,;", "=@[]^`{}~!", "-_.",        "0123456789" },
    { "áàâãéêíóôõ", "úüçñèìòùäö", "ÁÀÂÃÉÊÍÓÔÕ", "ÚÜÇÑºª-_.!" }
};

static const char *osk_page_names[OSK_PAGES] = { "ABC", "abc", "#+=", "áéç" };

static char osk_title[64];
static char osk_buffer[OSK_MAX_TEXT + 1];
static int osk_length;      /* bytes */
static int osk_max;
static int osk_page;
static int osk_row;
static int osk_col;
static int osk_accents;     /* página de acentos disponível */
static int osk_secret;      /* senha: mostra "••••" */


/* ------------------------------------------------------------
 * UTF-8: as linhas têm caracteres de 1 ou 2 bytes
 * ------------------------------------------------------------ */

static int utf8_bytes(unsigned char c)
{
    if (c < 0x80)
        return 1;

    if ((c & 0xE0) == 0xC0)
        return 2;

    if ((c & 0xF0) == 0xE0)
        return 3;

    return 4;
}


static int row_chars(const char *row)
{
    int count = 0;

    while (*row)
    {
        row += utf8_bytes((unsigned char)*row);
        count++;
    }

    return count;
}


/* Caractere index da linha, copiado (com terminador) para out. */
static void row_char(const char *row, int index, char *out)
{
    int bytes;

    while (*row && index > 0)
    {
        row += utf8_bytes((unsigned char)*row);
        index--;
    }

    bytes = *row ? utf8_bytes((unsigned char)*row) : 0;

    memcpy(out, row, bytes);
    out[bytes] = '\0';
}


static int row_width(int row)
{
    if (row == OSK_ACTION_ROW)
        return OSK_ACTIONS;

    return row_chars(osk_pages[osk_page][row]);
}


static void osk_clamp_col(void)
{
    int width = row_width(osk_row);

    if (osk_col >= width)
        osk_col = width - 1;
}


static void osk_type(const char *text)
{
    int bytes = strlen(text);

    if (osk_length + bytes > osk_max)
        return;

    memcpy(osk_buffer + osk_length, text, bytes);
    osk_length += bytes;
    osk_buffer[osk_length] = '\0';
}


/* Apaga o último caractere (todos os bytes dele). */
static void osk_delete(void)
{
    while (osk_length > 0)
    {
        unsigned char c = (unsigned char)osk_buffer[--osk_length];

        if ((c & 0xC0) != 0x80)
            break;
    }

    osk_buffer[osk_length] = '\0';
}


static void osk_next_page(void)
{
    int pages = osk_accents ? OSK_PAGES : OSK_PAGE_ACCENTS;

    osk_page = (osk_page + 1) % pages;

    if (osk_row != OSK_ACTION_ROW)
        osk_clamp_col();
}


void osk_open(const char *title, const char *initial, int max_length, int secret)
{
    snprintf(osk_title, sizeof(osk_title), "%s", title ? title : "");

    if (max_length <= 0 || max_length > OSK_MAX_TEXT)
        max_length = OSK_MAX_TEXT;

    osk_max = max_length;

    snprintf(osk_buffer, (size_t)osk_max + 1, "%s", initial ? initial : "");
    osk_length = strlen(osk_buffer);

    osk_page = 0;
    osk_row = 0;
    osk_col = 0;
    osk_accents = 1;
    osk_secret = secret;
}


void osk_ascii_only(void)
{
    osk_accents = 0;

    if (osk_page == OSK_PAGE_ACCENTS)
        osk_page = 0;
}


OskResult osk_update(unsigned int buttons)
{
    if (buttons & (PAD_UP | PAD_DOWN))
    {
        int old_width = row_width(osk_row);

        if (buttons & PAD_UP)
            osk_row = osk_row > 0 ? osk_row - 1 : OSK_ACTION_ROW;
        else
            osk_row = osk_row < OSK_ACTION_ROW ? osk_row + 1 : 0;

        /* entre as letras (10) e as ações (4): posição proporcional */
        if (old_width != row_width(osk_row))
            osk_col = osk_col * row_width(osk_row) / old_width;

        osk_clamp_col();
    }

    if (buttons & PAD_LEFT)
    {
        int width = row_width(osk_row);
        osk_col = osk_col > 0 ? osk_col - 1 : width - 1;
    }

    if (buttons & PAD_RIGHT)
    {
        int width = row_width(osk_row);
        osk_col = osk_col < width - 1 ? osk_col + 1 : 0;
    }

    if (buttons & PAD_CROSS)
    {
        if (osk_row == OSK_ACTION_ROW)
        {
            switch (osk_col)
            {
                case OSK_ACTION_PAGE:   osk_next_page(); break;
                case OSK_ACTION_SPACE:  osk_type(" "); break;
                case OSK_ACTION_DELETE: osk_delete(); break;
                default:                return OSK_DONE;
            }
        }
        else
        {
            char key[8];

            row_char(osk_pages[osk_page][osk_row], osk_col, key);
            osk_type(key);
        }
    }

    if (buttons & PAD_R1)
        osk_type(" ");

    if (buttons & PAD_SQUARE)
        osk_delete();

    if (buttons & PAD_TRIANGLE)
        osk_next_page();

    if (buttons & PAD_START)
        return OSK_DONE;

    if (buttons & PAD_CIRCLE)
        return OSK_CANCEL;

    return OSK_EDITING;
}


/* ------------------------------------------------------------
 * Desenho
 * ------------------------------------------------------------ */

#define OSK_X       56
#define OSK_Y       60
#define OSK_W       528
#define OSK_H       370
#define KEY_W       46
#define KEY_H       34
#define KEY_GAP     4

#define C_TEXT      ui_rgba(0xE6, 0xEE, 0xFF, 0x80)
#define C_DIM       ui_rgba(0x9A, 0xA8, 0xC4, 0x80)
#define C_OK        ui_rgba(0x4C, 0xE0, 0x7A, 0x80)
#define C_EDGE      ui_rgba(0x2A, 0x4A, 0x86, 0x80)
#define C_SEL_EDGE  ui_rgba(0x7C, 0xB8, 0xFF, 0x80)

static void draw_key(float x, float y, float w, const char *label, int focused, int highlight)
{
    if (focused)
    {
        ui_vgradient(x, y, w, KEY_H, ui_rgba(0x2A, 0x66, 0xC8, 0x80), ui_rgba(0x16, 0x3C, 0x86, 0x80));
        ui_border(x, y, w, KEY_H, 2, C_SEL_EDGE);
    }
    else
    {
        ui_rect(x, y, w, KEY_H, ui_rgba(0x10, 0x20, 0x42, 0x80));
        ui_border(x, y, w, KEY_H, 2, C_EDGE);
    }

    ui_text_center(x + w / 2, y + 3, label, focused ? C_TEXT : highlight ? C_OK : C_TEXT);
}


/* "••••" com o mesmo número de caracteres (senha) */
static void secret_text(char *out, int size)
{
    const char *p = osk_buffer;
    int n = 0;

    out[0] = '\0';

    while (*p && n + 4 < size)
    {
        p += utf8_bytes((unsigned char)*p);
        memcpy(out + n, "\xE2\x80\xA2", 3);     /* U+2022 */
        n += 3;
        out[n] = '\0';
    }
}


void osk_draw(void)
{
    static const char *action_names[] = { "", "Espaço", "Apagar", "OK" };
    float grid_w = 10 * KEY_W + 9 * KEY_GAP;
    float grid_x = OSK_X + (OSK_W - grid_w) / 2;
    int pages = osk_accents ? OSK_PAGES : OSK_PAGE_ACCENTS;
    char line[OSK_MAX_TEXT * 3 + 1];
    char key[8];
    float x, y;
    int r, c, p;

    /* tela de trás escurecida + janela */
    ui_rect(0, 0, 640, 480, ui_rgba(0x00, 0x00, 0x00, 0x50));
    ui_rect(OSK_X, OSK_Y, OSK_W, OSK_H, ui_rgba(0x08, 0x12, 0x2C, 0x80));
    ui_border(OSK_X, OSK_Y, OSK_W, OSK_H, 2, C_SEL_EDGE);

    ui_text_f(UI_FONT_LARGE, grid_x, OSK_Y + 6, osk_title, C_TEXT);
    y = OSK_Y + 50;

    /* campo de texto com cursor piscando; mostra o fim se não couber */
    ui_rect(grid_x, y, grid_w, 34, ui_rgba(0x05, 0x0B, 0x1E, 0x80));
    ui_border(grid_x, y, grid_w, 34, 2, C_SEL_EDGE);

    {
        const char *shown;
        float max_w = grid_w - 24;
        float text_w;

        if (osk_secret)
            secret_text(line, sizeof(line));
        else
            snprintf(line, sizeof(line), "%s", osk_buffer);

        shown = line;

        while (*shown && ui_text_width(shown) > max_w)
            shown += utf8_bytes((unsigned char)*shown);

        text_w = ui_text(grid_x + 10, y + 3, shown, C_TEXT);

        /* pisca a cada ~0,5 s */
        if ((GetTimerSystemTime() / (kBUSCLK / 2)) % 2 == 0)
            ui_rect(grid_x + 11 + text_w, y + 6, 2, 22, C_SEL_EDGE);
    }

    y += 40;

    snprintf(line, sizeof(line), "%d de %d", osk_length, osk_max);
    ui_text_right(grid_x + grid_w, y, line, osk_length >= osk_max ? ui_rgba(0xFF, 0xC8, 0x4C, 0x80) : C_DIM);

    /* páginas (a atual em destaque) */
    x = grid_x;

    for (p = 0; p < pages; p++)
    {
        float w = ui_text_width(osk_page_names[p]) + 16;

        if (p == osk_page)
            ui_rect(x, y + 2, w, 24, ui_rgba(0x16, 0x3C, 0x86, 0x80));

        ui_text(x + 8, y, osk_page_names[p], p == osk_page ? C_TEXT : C_DIM);
        x += w + 4;
    }

    y += 32;

    /* letras */
    for (r = 0; r < OSK_ROWS; r++)
    {
        const char *row = osk_pages[osk_page][r];

        for (c = 0; c < row_chars(row); c++)
        {
            row_char(row, c, key);

            draw_key(grid_x + c * (KEY_W + KEY_GAP), y + r * (KEY_H + KEY_GAP), KEY_W, key,
                     osk_row == r && osk_col == c, 0);
        }
    }

    y += OSK_ROWS * (KEY_H + KEY_GAP) + 2;

    /* ações: a primeira mostra a próxima página */
    {
        float action_w = (grid_w - (OSK_ACTIONS - 1) * KEY_GAP) / OSK_ACTIONS;

        for (c = 0; c < OSK_ACTIONS; c++)
        {
            const char *label = action_names[c];

            if (c == OSK_ACTION_PAGE)
            {
                snprintf(line, sizeof(line), "Teclas: %s", osk_page_names[(osk_page + 1) % pages]);
                label = line;
            }

            draw_key(grid_x + c * (action_w + KEY_GAP), y, action_w, label,
                     osk_row == OSK_ACTION_ROW && osk_col == c, c == OSK_ACTION_DONE);
        }
    }

    /* atalhos (abaixo da janela, sobre o rodapé da tela) */
    y = OSK_Y + OSK_H + 8;
    ui_rect(0, y - 4, 640, 480 - y + 4, ui_rgba(0x02, 0x05, 0x10, 0x80));

    x = 32;
    x += ui_button_hint(x, y, UI_BTN_CROSS, "Digitar");
    x += ui_button_hint(x, y, UI_BTN_SQUARE, "Apagar");
    x += ui_button_hint(x, y, UI_BTN_R1, "Espaço");
    x += ui_button_hint(x, y, UI_BTN_TRIANGLE, "Teclas");
    ui_button_hint(x, y, UI_BTN_CIRCLE, "Cancelar");     /* START = OK (tecla na tela) */
}


const char *osk_text(void)
{
    return osk_buffer;
}
