#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include <tamtypes.h>

#define NEWLIB_PORT_AWARE
#include <io_common.h>
#include <fileXio_rpc.h>

#include "cover.h"
#include "pngload.h"
#include "ui.h"

/* PNG maior que isso nem é lido (memória do EE) */
#define COVER_FILE_MAX (4 * 1024 * 1024)


void cover_path(char *out, int size, const char *lib_dir, const char *iso_name,
                char *rel, int rel_size)
{
    char base[96];
    char name[128];
    char *slash;
    char *dot;

    /* pasta-mãe da pasta dos filmes: "usb0:/FILMES" -> "usb0:" */
    snprintf(base, sizeof(base), "%s", lib_dir);
    slash = strrchr(base, '/');

    if (slash)
        *slash = '\0';

    /* nome da ISO sem a extensão */
    snprintf(name, sizeof(name), "%s", iso_name);
    dot = strrchr(name, '.');

    if (dot)
        *dot = '\0';

    snprintf(out, size, "%s/" COVER_DIR "/%s.png", base, name);

    if (rel)
        snprintf(rel, rel_size, COVER_DIR "/%s.png", name);
}


/*
 * Reduz rgba (w x h) para caber em COVER_W x COVER_H, pela média de
 * cada bloco (mantém a proporção). Devolve a imagem nova (ou a mesma,
 * se já cabe) e o tamanho em *ow, *oh.
 */
static unsigned char *fit_cover(unsigned char *rgba, int w, int h, int *ow, int *oh)
{
    unsigned char *out;
    int nw, nh, x, y;

    if (w <= COVER_W && h <= COVER_H)
    {
        *ow = w;
        *oh = h;
        return rgba;
    }

    if ((long)w * COVER_H > (long)h * COVER_W)
    {
        nw = COVER_W;
        nh = (h * COVER_W + w / 2) / w;
    }
    else
    {
        nh = COVER_H;
        nw = (w * COVER_H + h / 2) / h;
    }

    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;

    out = memalign(128, nw * nh * 4);

    if (!out)
        return NULL;

    for (y = 0; y < nh; y++)
    {
        int y0 = y * h / nh, y1 = (y + 1) * h / nh;

        if (y1 <= y0) y1 = y0 + 1;

        for (x = 0; x < nw; x++)
        {
            int x0 = x * w / nw, x1 = (x + 1) * w / nw;
            unsigned sum[4] = { 0, 0, 0, 0 };
            int sx, sy, c, n;

            if (x1 <= x0) x1 = x0 + 1;

            n = (x1 - x0) * (y1 - y0);

            for (sy = y0; sy < y1; sy++)
            {
                const unsigned char *p = rgba + (sy * w + x0) * 4;

                for (sx = x0; sx < x1; sx++, p += 4)
                    for (c = 0; c < 4; c++)
                        sum[c] += p[c];
            }

            for (c = 0; c < 4; c++)
                out[(y * nw + x) * 4 + c] = (unsigned char)(sum[c] / n);
        }
    }

    *ow = nw;
    *oh = nh;
    return out;
}


int cover_load(const char *lib_dir, const char *iso_name)
{
    char path[256];
    unsigned char *file = NULL;
    unsigned char *rgba = NULL;
    unsigned char *fit = NULL;
    int fd, size, got, w = 0, h = 0, fw, fh, result = -1;

    ui_cover_free();

    cover_path(path, sizeof(path), lib_dir, iso_name, NULL, 0);

    fd = fileXioOpen(path, FIO_O_RDONLY);

    if (fd < 0)
    {
        printf("COVER: %s not found (%d)\n", path, fd);
        return 0;
    }

    size = (int)fileXioLseek(fd, 0, FIO_SEEK_END);
    fileXioLseek(fd, 0, FIO_SEEK_SET);

    if (size <= 8 || size > COVER_FILE_MAX)
    {
        printf("COVER: %s: bad size %d\n", path, size);
        goto done;
    }

    file = malloc(size);

    if (!file)
        goto done;

    got = fileXioRead(fd, file, size);

    if (got != size)
    {
        printf("COVER: %s: read %d of %d\n", path, got, size);
        goto done;
    }

    rgba = png_decode(file, size, 4, &w, &h);

    if (!rgba)
    {
        printf("COVER: %s: not a valid PNG\n", path);
        goto done;
    }

    if (w > COVER_MAX_W || h > COVER_MAX_H)
    {
        printf("COVER: %s: %dx%d too large (max %dx%d)\n", path, w, h, COVER_MAX_W, COVER_MAX_H);
        goto done;
    }

    fit = fit_cover(rgba, w, h, &fw, &fh);

    if (!fit)
        goto done;

    result = ui_cover_set(fit, fw, fh) == 0 ? 1 : -2;

    printf("COVER: %s %dx%d -> %dx%d (%d)\n", path, w, h, fw, fh, result);

done:
    fileXioClose(fd);

    if (fit && fit != rgba)
        free(fit);

    free(rgba);
    free(file);

    return result;
}
