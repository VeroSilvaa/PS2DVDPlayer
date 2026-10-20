libmpeg (decodificador MPEG-1/2 com IPU) copiado do PS2SDK
(ps2dev/ps2sdk, ee/mpeg/src, branch master, 2026-10-17).

Copyright (c) 2006-2007 Eugene Plotnikov <e-plotnikov@operamail.com>
Licença: Academic Free License 2.0 (ver LICENSE do PS2SDK).

Compilado junto com o PS2DVDPlayer (em vez de -lmpeg) para
diagnóstico. Alterações locais: macro MPEG_SPIN e mensagens
"MPEGDBG:" em libmpeg_internal.h e libmpeg_core.c.

Correção local (2026-10-17): em _ipu_suspend(), se a fila de saída
do IPU tiver dados com o canal 3 parado e sem quadwords pendentes,
os dados são lidos de IPU_OUT_FIFO e descartados. Sem isso, no
PCSX2 a decodificação do segundo quadro travava nessa espera.

Alteração local (2026-10-17): PS2DVD_SW_CSC=1 em libmpeg.c troca
_MPEG_CSCImage (CSC no IPU, com suspend/resume do fluxo de bits)
por _sw_csc (YCbCr 4:2:0 -> RGBA32 em software no EE, BT.601).
Motivo: no PCSX2, depois da CSC no IPU a retomada do fluxo perde a
posição e a decodificação do quadro seguinte para (slices=0).

Teste (2026-10-17): CSC no IPU com descarte dos restos da fila de
saída logo após a conversão (PS2DVD_SW_CSC=0) também falhou no
PCSX2: o fluxo para (ES não avança) e os últimos ~204 macroblocos
saem com lixo. Padrão mantido: PS2DVD_SW_CSC=1.

Alteração local (2026-10-17): CSC em software reescrita em MMI
(assembly, 8 pixels por instrução, Q5). Verificada contra a versão
escalar: diferença máxima de 1 nível em 5 bits. PCSX2: 16 ms (I),
25-27 ms (P/B) por quadro, contra 48-65 ms da versão escalar.
PS2DVD_CSC_VERIFY=1 refaz a comparação no primeiro quadro.

Correção local (2026-10-17): no fim da sequência (SEQ_END) o último
quadro de referência pendente (s_RefPending) é entregue. Sem isso um
menu de DVD com imagem parada (um único quadro I) não aparecia.

Alteração local (2026-10-17): MPEG_AtSequenceStart() informa se o
decodificador está no início de uma sequência (antes do 1º quadro ou
depois de um SEQ_END já consumido). frame.c usa para segurar a imagem
num still frame do DVD (navegação pela libdvdnav).

Alteração local (2026-10-17): MPEG_StopInput() (libmpeg_core.c) para
o canal 4 (dados para o IPU). frame.c chama antes de reiniciar o
decodificador num salto do DVD (botão de menu), descartando o vídeo
antigo; MPEG_Initialize reseta o IPU em seguida.

Alteração local (2026-10-17): MPEG_SetSkipCSC(1) faz o próximo quadro
ser decodificado sem a conversão de cor (só com PS2DVD_SW_CSC). O
player usa para quadros atrasados, que não vão para a tela.

Alteração local (2026-10-17): MPEG_SetPlanar(y, cb, cr, stride): com
os planos definidos, _sw_csc só copia os macroblocos para planos Y /
Cb / Cr de 8 bits; a conversão para RGB passa a ser feita pela GS
(main.c, texturas T8 + paletas, 5 passadas com blending).
