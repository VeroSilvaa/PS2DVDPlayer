#ifndef PS2DVDPLAYER_MEMLOG_H
#define PS2DVDPLAYER_MEMLOG_H

/*
 * Registra no log o uso de memória do EE (32 MB):
 * programa, heap (malloc) e espaço livre até a pilha.
 * "where" identifica o ponto da chamada.
 */
void mem_log(const char *where);

/*
 * Memória livre estimada do EE, em bytes (heap livre + espaço
 * entre o topo do heap e a pilha).
 */
unsigned int mem_free_bytes(void);

#endif
