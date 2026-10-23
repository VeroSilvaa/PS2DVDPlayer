#include <stdio.h>

#include <tamtypes.h>
#include <kernel.h>
#include <loadfile.h>

#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>

#include "dbglog.h"
#include "io_modules.h"

extern unsigned char IOMANX_irx[];
extern unsigned int size_IOMANX_irx;

extern unsigned char FILEXIO_irx[];
extern unsigned int size_FILEXIO_irx;

static int ioModulesReady = 0;


static int io_modules_load(const char *name, void *buffer, unsigned int size)
{
    int module_id;
    int module_result = 0;

    module_id = SifExecModuleBuffer(buffer, size, 0, NULL, &module_result);

    printf("IO_MODULES: %s -> module=%d result=%d\n", name, module_id, module_result);

    if (module_id < 0)
        return module_id;

    if (module_result < 0)
        return module_result;

    return 0;
}


int io_modules_init(void)
{
    int result;

    if (ioModulesReady)
        return 0;

    result = io_modules_load("IOMANX", IOMANX_irx, size_IOMANX_irx);

    if (result < 0)
        return -1;

    result = io_modules_load("FILEXIO", FILEXIO_irx, size_FILEXIO_irx);

    if (result < 0)
        return -2;

    result = fileXioInit();

    printf("IO_MODULES: fileXioInit returned %d\n", result);

    if (result < 0)
        return -3;

    ioModulesReady = 1;
    return 0;
}
