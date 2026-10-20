#include <stdio.h>
#include <malloc.h>
#include <unistd.h>

#include "memlog.h"

/*
 * Layout do EE (linkfile do PS2SDK):
 *   0x00100000 ............ programa (.text/.data/.bss) até _end
 *   _end .................. heap (malloc, cresce com sbrk)
 *   ... livre ...
 *   topo - _stack_size .... pilha (128 KB), topo em 32 MB
 */
#define EE_RAM_TOP     0x02000000u

extern char _end[];
extern char _stack_size[];      /* símbolo do linker: o endereço é o valor */


static unsigned int stack_limit(void)
{
    return EE_RAM_TOP - (unsigned int)_stack_size;
}


unsigned int mem_free_bytes(void)
{
    struct mallinfo mi = mallinfo();
    unsigned int brk = (unsigned int)sbrk(0);
    unsigned int above = brk < stack_limit() ? stack_limit() - brk : 0;

    return above + (unsigned int)mi.fordblks;
}


void mem_log(const char *where)
{
    struct mallinfo mi = mallinfo();
    unsigned int prog = (unsigned int)_end - 0x00100000u;
    unsigned int brk = (unsigned int)sbrk(0);
    unsigned int above = brk < stack_limit() ? stack_limit() - brk : 0;
    unsigned int sp;

    __asm__ __volatile__("move %0, $sp" : "=r"(sp));

    printf(
        "MEM [%s]: program %u KB, heap used %u KB (free in heap %u KB), "
        "unused %u KB, FREE %u KB, sp=0x%08x\n",
        where,
        prog / 1024,
        (unsigned int)mi.uordblks / 1024,
        (unsigned int)mi.fordblks / 1024,
        above / 1024,
        (above + (unsigned int)mi.fordblks) / 1024,
        sp
    );
}
