liba52 0.7.4 (a52dec, http://liba52.sourceforge.net/), licença GPL v2
ou posterior (ver COPYING). Origem do arquivo: Debian,
a52dec_0.7.4.orig.tar.gz, SHA-256
a21d724ab3b3933330194353687df82c475b5dfb997513eef4c25de6c865ec33
(conferido com a52dec_0.7.4-22.dsc), em 2026-10-17.

Copiados só liba52/*.c, *.h e include/a52.h, attributes.h, mm_accel.h.

Alterações locais para o PS2:
- config.h novo (HAVE_MEMALIGN, ATTRIBUTE_ALIGNED_MAX).
- Compilada com -fsingle-precision-constant: o EE só tem FPU de
  precisão simples; constantes double seriam emuladas em software.
- Compilada com -Wno-array-bounds: imdct.c usa ponteiros como roots16 - 4
  de propósito (ifft_pass compensa); o aviso do GCC é falso positivo.
- imdct.c: variáveis temporárias das borboletas da FFT (ifft2/4/8,
  ifft_pass) trocadas de double para sample_t (float). Em double cada
  operação era emulada em software no EE (~26 ms por quadro AC3).
