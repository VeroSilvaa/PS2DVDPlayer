/*
 * PS2DVDPlayer: funções POSIX que faltam no newlib do PS2 e que a
 * libdvdread referencia. Só são usadas por DVDOpen() com caminho;
 * o player abre o disco por DVDOpenStream() (callbacks), então aqui
 * basta uma versão simples.
 */
#include <stdlib.h>
#include <string.h>

char *realpath(const char *path, char *resolved)
{
    size_t n;

    if (path == NULL)
        return NULL;

    n = strlen(path) + 1;
    if (resolved == NULL)
        resolved = malloc(n);
    if (resolved != NULL)
        memcpy(resolved, path, n);
    return resolved;
}
