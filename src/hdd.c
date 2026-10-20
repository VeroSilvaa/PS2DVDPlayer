#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include <tamtypes.h>
#include <sifrpc.h>
#include <loadfile.h>

#include <io_common.h>
#include <fileXio_rpc.h>
#include <fileXio.h>

#include "hdd.h"
#include "iop_boot.h"

/*
 * IRX incorporados ao ELF.
 */

extern unsigned char IOMANX_irx[];
extern unsigned int size_IOMANX_irx;

extern unsigned char FILEXIO_irx[];
extern unsigned int size_FILEXIO_irx;

extern unsigned char DEV9_irx[];
extern unsigned int size_DEV9_irx;

extern unsigned char PS2ATAD_irx[];
extern unsigned int size_PS2ATAD_irx;

extern unsigned char PS2HDD_irx[];
extern unsigned int size_PS2HDD_irx;

extern unsigned char PS2FS_irx[];
extern unsigned int size_PS2FS_irx;


/*
 * Estado do backend HDD.
 */

static HDDInfo hddInfo;


/*
 * ------------------------------------------------------------
 * Utilidades
 * ------------------------------------------------------------
 */

static void hdd_set_error(const char *message)
{
    if (!message)
        message = "Unknown HDD error";

    strncpy(
        hddInfo.error,
        message,
        sizeof(hddInfo.error) - 1
    );

    hddInfo.error[
        sizeof(hddInfo.error) - 1
    ] = '\0';
}


static void hdd_clear_error(void)
{
    hddInfo.error[0] = '\0';
}


static int hdd_is_iso(const char *name)
{
    size_t len;

    if (!name)
        return 0;

    len = strlen(name);

    if (len < 4)
        return 0;

    return (
        name[len - 4] == '.' &&
        tolower((unsigned char)name[len - 3]) == 'i' &&
        tolower((unsigned char)name[len - 2]) == 's' &&
        tolower((unsigned char)name[len - 1]) == 'o'
    );
}


/*
 * ------------------------------------------------------------
 * Carregamento de IRX
 * ------------------------------------------------------------
 */

static int hdd_load_module(
    const char *name,
    unsigned char *module,
    unsigned int size,
    int *module_result
)
{
    int start_result;
    int module_id;

    printf(
        "HDD: loading %s (%u bytes)\n",
        name,
        size
    );

    start_result = 0;

    module_id = SifExecModuleBuffer(
        module,
        size,
        0,
        NULL,
        &start_result
    );

    printf(
        "HDD: %s -> module=%d result=%d\n",
        name,
        module_id,
        start_result
    );

    if (module_result)
        *module_result = module_id;

    if (module_id < 0)
        return module_id;

    if (start_result < 0)
        return start_result;

    return 0;
}


/*
 * ------------------------------------------------------------
 * Inicialização
 * ------------------------------------------------------------
 */

int hdd_init(void)
{
    int result;

    printf("\n");
    printf("=================================\n");
    printf(" PS2DVDPlayer - HDD INIT\n");
    printf("=================================\n");

    if (hddInfo.initialized)
    {
        printf("HDD: already initialized\n");
        return 0;
    }

    memset(&hddInfo, 0, sizeof(hddInfo));

    /*
     * O IOP deve ter sido resetado e preparado pelo main
     * antes de carregar estes módulos.
     */

    if (!iop_boot_is_ready())
    {
        printf("HDD: ERROR: IOP boot not ready\n");

        hdd_set_error(
            "IOP boot not ready"
        );

        return -1;
    }


    /*
     * IOMANX
     */

    result = hdd_load_module(
        "IOMANX",
        IOMANX_irx,
        size_IOMANX_irx,
        NULL
    );

    if (result < 0)
    {
        hdd_set_error("Cannot load IOMANX");
        return result;
    }


    /*
     * FILEXIO
     */

    result = hdd_load_module(
        "FILEXIO",
        FILEXIO_irx,
        size_FILEXIO_irx,
        NULL
    );

    if (result < 0)
    {
        hdd_set_error("Cannot load FILEXIO");
        return result;
    }


    printf("HDD: calling fileXioInit()\n");

    result = fileXioInit();

    printf(
        "HDD: fileXioInit returned %d\n",
        result
    );

    if (result < 0)
    {
        hdd_set_error(
            "fileXioInit failed"
        );

        return result;
    }

    printf("HDD: FILEXIO RPC ready\n");


    /*
     * DEV9
     */

    result = hdd_load_module(
        "DEV9",
        DEV9_irx,
        size_DEV9_irx,
        &hddInfo.dev9_result
    );

    if (result < 0)
    {
        hdd_set_error("Cannot load DEV9");
        return result;
    }


    /*
     * ATAD
     */

    result = hdd_load_module(
        "PS2ATAD",
        PS2ATAD_irx,
        size_PS2ATAD_irx,
        &hddInfo.atad_result
    );

    if (result < 0)
    {
        hdd_set_error("Cannot load PS2ATAD");
        return result;
    }


    /*
     * APA / HDD
     */

    result = hdd_load_module(
        "PS2HDD",
        PS2HDD_irx,
        size_PS2HDD_irx,
        &hddInfo.hdd_result
    );

    if (result < 0)
    {
        hdd_set_error("Cannot load PS2HDD");
        return result;
    }


    /*
     * PFS
     */

    result = hdd_load_module(
        "PS2FS",
        PS2FS_irx,
        size_PS2FS_irx,
        &hddInfo.pfs_result
    );

    if (result < 0)
    {
        hdd_set_error("Cannot load PS2FS");
        return result;
    }


    hddInfo.initialized = 1;

    hdd_clear_error();

    printf("=================================\n");
    printf(" HDD STACK READY\n");
    printf("=================================\n");

    return 0;
}


/*
 * ------------------------------------------------------------
 * Mount
 * ------------------------------------------------------------
 */

int hdd_mount(void)
{
    int result;

    printf("\n");
    printf("=================================\n");
    printf(" HDD MOUNT\n");
    printf("=================================\n");

    if (!hddInfo.initialized)
    {
        hdd_set_error(
            "HDD not initialized"
        );

        printf(
            "HDD: ERROR: HDD not initialized\n"
        );

        return -1;
    }

    if (hddInfo.mounted)
    {
        printf("HDD: already mounted\n");
        return 0;
    }

    printf(
        "HDD: block device: hdd0:+PS2DVDPLAYER\n"
    );

    printf(
        "HDD: mount point : pfs0:\n"
    );

    printf(
        "HDD: mode        : READ/WRITE\n"
    );


    /*
     * IMPORTANTE:
     *
     * FIO_MT_RDWR = 0x00
     *
     * Precisamos de escrita neste momento para
     * poder criar pfs0:/DVD.
     */

    result = fileXioMount(
        "pfs0:",
        "hdd0:+PS2DVDPLAYER",
        FIO_MT_RDWR
    );

    printf(
        "HDD: fileXioMount returned %d\n",
        result
    );

    if (result < 0)
    {
        hdd_set_error(
            "Cannot mount +PS2DVDPLAYER"
        );

        printf(
            "HDD: ERROR: %s\n",
            hddInfo.error
        );

        return result;
    }

    hddInfo.mounted = 1;

    printf(
        "HDD: partition mounted successfully\n"
    );

    printf(
        "HDD: pfs0: ready READ/WRITE\n"
    );

    hdd_clear_error();

    return 0;
}


/*
 * ------------------------------------------------------------
 * Criar /DVD
 * ------------------------------------------------------------
 */

int hdd_ensure_dvd_directory(void)
{
    int dd;
    int result;

    printf("\n");
    printf("=================================\n");
    printf(" HDD DVD DIRECTORY\n");
    printf("=================================\n");

    if (!hddInfo.mounted)
    {
        hdd_set_error(
            "HDD partition not mounted"
        );

        printf(
            "HDD: ERROR: partition not mounted\n"
        );

        return -1;
    }


    /*
     * Primeiro verifica se /DVD já existe.
     */

    printf(
        "HDD: checking pfs0:/DVD\n"
    );

    dd = fileXioDopen(
        "pfs0:/DVD"
    );

    printf(
        "HDD: initial fileXioDopen returned %d\n",
        dd
    );

    if (dd >= 0)
    {
        printf(
            "HDD: DVD directory already exists\n"
        );

        fileXioDclose(dd);

        hdd_clear_error();

        return 0;
    }


    /*
     * Não existe.
     */

    printf(
        "HDD: DVD directory not found\n"
    );

    printf(
        "HDD: creating pfs0:/DVD\n"
    );


    /*
     * 0777:
     *
     * leitura/escrita/execução para todos.
     * Para diretório PFS é adequado para nosso
     * diretório de armazenamento.
     */

    result = fileXioMkdir(
        "pfs0:/DVD",
        0777
    );

    printf(
        "HDD: fileXioMkdir returned %d\n",
        result
    );

    if (result < 0)
    {
        hdd_set_error(
            "Cannot create pfs0:/DVD"
        );

        printf(
            "HDD: ERROR: %s\n",
            hddInfo.error
        );

        return result;
    }


    /*
     * Verificação imediata.
     */

    printf(
        "HDD: verifying pfs0:/DVD\n"
    );

    dd = fileXioDopen(
        "pfs0:/DVD"
    );

    printf(
        "HDD: verify fileXioDopen returned %d\n",
        dd
    );

    if (dd < 0)
    {
        hdd_set_error(
            "DVD directory creation verification failed"
        );

        printf(
            "HDD: ERROR: %s\n",
            hddInfo.error
        );

        return dd;
    }

    fileXioDclose(dd);

    printf(
        "HDD: DVD directory created successfully\n"
    );

    printf(
        "HDD: pfs0:/DVD ready\n"
    );

    hdd_clear_error();

    return 0;
}


/*
 * ------------------------------------------------------------
 * Listagem de ISO
 * ------------------------------------------------------------
 */

int hdd_list_dvd(void)
{
    int result;
    int dd;

    iox_dirent_t dirent;

    printf("\n");
    printf("=================================\n");
    printf(" HDD DVD LIST\n");
    printf("=================================\n");

    hddInfo.file_count = 0;


    /*
     * Antes de listar, garante que /DVD existe.
     */

    result = hdd_ensure_dvd_directory();

    printf(
        "HDD: hdd_ensure_dvd_directory returned %d\n",
        result
    );

    if (result < 0)
        return result;


    printf(
        "HDD: opening pfs0:/DVD\n"
    );

    dd = fileXioDopen(
        "pfs0:/DVD"
    );

    printf(
        "HDD: fileXioDopen returned %d\n",
        dd
    );

    if (dd < 0)
    {
        hdd_set_error(
            "Cannot open pfs0:/DVD"
        );

        printf(
            "HDD: ERROR: %s\n",
            hddInfo.error
        );

        return dd;
    }


    /*
     * Lê todas as entradas.
     */

    while (1)
    {
        u64 file_size;

        memset(
            &dirent,
            0,
            sizeof(dirent)
        );

        result = fileXioDread(
            dd,
            &dirent
        );

        if (result <= 0)
            break;


        /*
         * Ignora entradas especiais.
         */

        if (
            strcmp(dirent.name, ".") == 0 ||
            strcmp(dirent.name, "..") == 0
        )
        {
            continue;
        }


        /*
         * Apenas .iso.
         */

        if (!hdd_is_iso(dirent.name))
            continue;


        /*
         * Limite da lista.
         */

        if (
            hddInfo.file_count >=
            HDD_MAX_FILES
        )
        {
            printf(
                "HDD: file list full (%d entries)\n",
                HDD_MAX_FILES
            );

            break;
        }


        /*
         * Tamanho de 64 bits.
         *
         * Fundamental para DVD ISO > 4 GB.
         */

        file_size =
            ((u64)dirent.stat.hisize << 32) |
            (u64)dirent.stat.size;


        strncpy(
            hddInfo.files[
                hddInfo.file_count
            ].name,
            dirent.name,
            HDD_MAX_NAME - 1
        );

        hddInfo.files[
            hddInfo.file_count
        ].name[
            HDD_MAX_NAME - 1
        ] = '\0';


        hddInfo.files[
            hddInfo.file_count
        ].size = file_size;


        printf(
            "HDD: ISO[%d] %s\n",
            hddInfo.file_count,
            hddInfo.files[
                hddInfo.file_count
            ].name
        );

        printf(
            "HDD: ISO[%d] size = %llu bytes\n",
            hddInfo.file_count,
            (unsigned long long)file_size
        );


        hddInfo.file_count++;
    }


    fileXioDclose(dd);


    /*
     * Dread < 0 representa erro.
     */

    if (result < 0)
    {
        hdd_set_error(
            "Error reading pfs0:/DVD"
        );

        printf(
            "HDD: fileXioDread error %d\n",
            result
        );

        return result;
    }


    printf(
        "HDD: ISO files found: %d\n",
        hddInfo.file_count
    );

    hdd_clear_error();

    return 0;
}


/*
 * ------------------------------------------------------------
 * Unmount
 * ------------------------------------------------------------
 */

void hdd_unmount(void)
{
    int result;

    if (!hddInfo.mounted)
        return;

    printf(
        "HDD: unmounting pfs0:\n"
    );

    result = fileXioUmount(
        "pfs0:"
    );

    printf(
        "HDD: fileXioUmount returned %d\n",
        result
    );

    if (result >= 0)
        hddInfo.mounted = 0;
}


/*
 * ------------------------------------------------------------
 * Shutdown
 * ------------------------------------------------------------
 */

void hdd_shutdown(void)
{
    hdd_unmount();

    hddInfo.initialized = 0;
    hddInfo.file_count = 0;

    printf(
        "HDD: shutdown complete\n"
    );
}


/*
 * ------------------------------------------------------------
 * Estado
 * ------------------------------------------------------------
 */

const HDDInfo *hdd_get_info(void)
{
    return &hddInfo;
}