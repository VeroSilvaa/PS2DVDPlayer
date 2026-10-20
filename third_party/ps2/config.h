/*
 * config.h para compilar libdvdread/libdvdnav no PS2 (EE, newlib).
 * PS2DVDPlayer: leitura sempre por callbacks de stream (sem libdvdcss,
 * sem acesso a dispositivo), little-endian.
 */
#ifndef PS2DVD_DVD_CONFIG_H
#define PS2DVD_DVD_CONFIG_H

#define HAVE_GETTIMEOFDAY 1
#define HAVE_SYS_PARAM_H 1
#define HAVE_STRERROR_R 0

#define PACKAGE_VERSION "ps2"
#define VERSION "ps2"

#define UNUSED __attribute__((unused))

/*
 * Sem carregamento dinâmico no PS2: libdvdcss nunca é encontrada e a
 * libdvdread usa a leitura direta (discos sem CSS / ISOs).
 */
#define RTLD_LAZY 0
#define dlopen(name, flags) ((void *)0)
#define dlsym(lib, sym) ((void *)0)
#define dlclose(lib) ((void)0)

#endif
