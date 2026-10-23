#ifndef PS2DVDPLAYER_MEMCARD_H
#define PS2DVDPLAYER_MEMCARD_H

/*
 * Memory Card (mc0: / mc1:).
 *
 * Carrega XMCMAN + XMCSERV da ROM, mesma família do
 * XSIO2MAN/XPADMAN carregados por input_init().
 *
 * Deve ser chamado depois de input_init(), que carrega
 * o XSIO2MAN do qual o XMCMAN depende.
 */

/*
 * 0 = mcInit OK; < 0 = erro (detalhes no console).
 */
int memcard_init(void);

/*
 * 1 se memcard_init() concluiu com sucesso.
 */
int memcard_ready(void);

#endif
