libdvdread 6.1.3 e libdvdnav 6.1.1 (VideoLAN), licença GPL v2 ou
posterior (ver COPYING em cada pasta). Baixados de
https://download.videolan.org/pub/videolan/ em 2026-10-17.

Usados pelo PS2DVDPlayer para a navegação DVD-Video (máquina virtual,
menus, botões). Por causa da GPL, o PS2DVDPlayer distribuído deve
acompanhar o código-fonte sob licença compatível com a GPL.

Alterações locais para o PS2:
- ps2/config.h: configuração (UNUSED, sem dlopen/libdvdcss).
- libdvdread/src/bswap.h: ramo #if defined(_EE) com __builtin_bswap*.
- Compilados com -Wno-incompatible-pointer-types: no newlib do PS2
  int32_t é "long" (32 bits) e o código passa int* em alguns lugares.
- ps2/compat.c: realpath() simples (não existe no newlib do PS2).
- libdvdread/src/dvd_input.c: ramo _EE no backend file_* que lê pelos
  callbacks de DVDOpenStream (na versão original o modo stream só existe
  através da libdvdcss).
