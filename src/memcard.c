#include <stdio.h>

#include <tamtypes.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <libmc.h>

#include "dbglog.h"
#include "memcard.h"

static int memcardReady;


/* ============================================================
 * INFO DE UM SLOT (apenas log)
 * ============================================================ */

static void memcard_log_port(int port)
{
    int type = 0;
    int free = 0;
    int format = 0;
    int ret = 0;

    mcGetInfo(port, 0, &type, &free, &format);
    mcSync(0, NULL, &ret);

    /*
     * ret: 0 = mesmo cartão, -1 = cartão novo (formatado),
     * -2 = cartão novo não formatado, < -2 = sem cartão.
     */
    printf(
        "MEMCARD: mc%d: ret=%d type=%d free=%d KB formatted=%d\n",
        port,
        ret,
        type,
        free,
        format
    );
}


/* ============================================================
 * INIT
 * ============================================================ */

int memcard_init(void)
{
    int ret;

    if (memcardReady)
        return 0;

    printf("MEMCARD: loading XMCMAN\n");

    ret = SifLoadModule(
        "rom0:XMCMAN",
        0,
        NULL
    );

    printf("MEMCARD: XMCMAN returned %d\n", ret);

    if (ret < 0)
        return -1;

    printf("MEMCARD: loading XMCSERV\n");

    ret = SifLoadModule(
        "rom0:XMCSERV",
        0,
        NULL
    );

    printf("MEMCARD: XMCSERV returned %d\n", ret);

    if (ret < 0)
        return -2;

    ret = mcInit(MC_TYPE_XMC);

    printf("MEMCARD: mcInit(MC_TYPE_XMC) returned %d\n", ret);

    if (ret < 0)
        return -3;

    memcardReady = 1;

    memcard_log_port(0);
    memcard_log_port(1);

    return 0;
}


int memcard_ready(void)
{
    return memcardReady;
}
