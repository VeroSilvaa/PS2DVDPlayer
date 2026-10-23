#ifndef PS2DVDPLAYER_IO_MODULES_H
#define PS2DVDPLAYER_IO_MODULES_H

/*
 * IOMANX + FILEXIO + fileXioInit, uma única vez por boot.
 *
 * Compartilhado por SMB, USB e HDD: carregar o IOMANX/FILEXIO
 * de novo criaria uma segunda instância no IOP.
 *
 * 0 = pronto; < 0 = erro (detalhes no console).
 */
int io_modules_init(void);

#endif
