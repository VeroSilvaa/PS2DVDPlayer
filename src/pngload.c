#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include <png.h>

#include "pngload.h"

/*
 * PNG embutido no ELF (bin2c) -> pixels. Usa a libpng do toolchain.
 */

typedef struct {
    const unsigned char *data;
    unsigned int size;
    unsigned int offset;
} PngSource;


static void png_read_mem(png_structp png, png_bytep out, png_size_t length)
{
    PngSource *src = (PngSource *)png_get_io_ptr(png);

    if (src->offset + length > src->size) {
        png_error(png, "fim do PNG");
        return;
    }

    memcpy(out, src->data + src->offset, length);
    src->offset += length;
}


unsigned char *png_decode(const unsigned char *data, unsigned int size,
                          int channels, int *width, int *height)
{
    PngSource src = { data, size, 0 };
    png_structp png;
    png_infop info;
    unsigned char *pixels = NULL;
    png_bytep *rows = NULL;
    int color;
    int h;
    int y;

    if (size < 8 || png_sig_cmp(data, 0, 8) != 0)
        return NULL;

    png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);

    if (!png)
        return NULL;

    info = png_create_info_struct(png);

    if (!info) {
        png_destroy_read_struct(&png, NULL, NULL);
        return NULL;
    }

    if (setjmp(png_jmpbuf(png))) {
        free(rows);
        free(pixels);
        png_destroy_read_struct(&png, &info, NULL);
        return NULL;
    }

    png_set_read_fn(png, &src, png_read_mem);
    png_read_info(png, info);

    color = png_get_color_type(png, info);

    /*
     * Normaliza para 8 bits por canal, no numero de canais pedido
     * (1 = cinza, 3 = RGB, 4 = RGBA).
     */
    png_set_strip_16(png);
    png_set_packing(png);

    if (color == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png);

    if (png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png);

    if (channels >= 3 && (color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA))
        png_set_gray_to_rgb(png);

    if (channels == 1 && (color & PNG_COLOR_MASK_COLOR))
        png_set_rgb_to_gray_fixed(png, 1, -1, -1);

    if (channels == 4)
        png_set_add_alpha(png, 0xFF, PNG_FILLER_AFTER);
    else
        png_set_strip_alpha(png);

    png_read_update_info(png, info);

    *width = png_get_image_width(png, info);
    *height = h = png_get_image_height(png, info);

    if ((int)png_get_rowbytes(png, info) != *width * channels)
        png_error(png, "formato inesperado");

    pixels = memalign(128, *width * h * channels);
    rows = malloc(h * sizeof(png_bytep));

    if (!pixels || !rows)
        png_error(png, "sem memoria");

    for (y = 0; y < h; y++)
        rows[y] = pixels + y * *width * channels;

    png_read_image(png, rows);
    png_read_end(png, NULL);

    free(rows);
    png_destroy_read_struct(&png, &info, NULL);

    return pixels;
}
