#ifndef PS2DVDPLAYER_PNGLOAD_H
#define PS2DVDPLAYER_PNGLOAD_H

/* Do PS2FileManager (mesmo autor). */

/*
 * Decodifica um PNG em memoria para 'channels' canais de 8 bits
 * (1 = cinza, 3 = RGB, 4 = RGBA). Buffer alinhado em 128 (free()).
 * NULL em erro.
 */
unsigned char *png_decode(const unsigned char *data, unsigned int size,
                          int channels, int *width, int *height);

#endif
