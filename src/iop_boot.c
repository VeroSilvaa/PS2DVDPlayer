#include <stdio.h>

#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <iopcontrol.h>
#include <sbv_patches.h>

#include "iop_boot.h"


static int iop_ready = 0;


/* ============================================================
 * RESET LIMPO DO IOP
 * ============================================================ */

int iop_boot_reset(void)
{
    int result;


    printf("\n");
    printf("=================================\n");
    printf(" PS2DVDPlayer - IOP RESET\n");
    printf("=================================\n");


    /*
     * Inicializa SIF RPC antes de solicitar
     * o reset do IOP.
     */

    printf("IOP: initializing SIF RPC\n");

    sceSifInitRpc(0);


    /*
     * Reset do IOP.
     */

    printf("IOP: resetting...\n");


    while (!SifIopReset(NULL, 0))
    {
        /*
         * Aguarda o IOP aceitar o reset.
         */
    }


    printf("IOP: reset accepted\n");


    /*
     * Espera o IOP terminar de reiniciar.
     */

    printf("IOP: waiting synchronization...\n");


    while (!SifIopSync())
    {
        /*
         * Aguarda sincronização.
         */
    }


    printf("IOP: synchronized\n");


    /*
     * O reset destruiu o estado RPC anterior.
     * Inicializamos novamente.
     */

    printf("IOP: reinitializing SIF RPC\n");

    sceSifInitRpc(0);


    /*
     * Permite carregar módulos IRX diretamente
     * da memória EE.
     */

    printf("IOP: applying LMB patch\n");

    result = sbv_patch_enable_lmb();

    printf(
        "IOP: sbv_patch_enable_lmb -> %d\n",
        result
    );


    if (result < 0)
    {
        printf("IOP: LMB patch failed\n");

        return result;
    }


    /*
     * Remove a restrição de prefixo usada pelo
     * carregador de módulos.
     */

    printf("IOP: applying prefix patch\n");

    result = sbv_patch_disable_prefix_check();

    printf(
        "IOP: sbv_patch_disable_prefix_check -> %d\n",
        result
    );


    if (result < 0)
    {
        printf("IOP: prefix patch failed\n");

        return result;
    }


    iop_ready = 1;


    printf("=================================\n");
    printf(" IOP READY\n");
    printf("=================================\n");


    return 0;
}


int iop_boot_is_ready(void)
{
    return iop_ready;
}