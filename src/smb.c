#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <tamtypes.h>
#include <kernel.h>
#include <loadfile.h>

#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>
#include <ps2smb.h>

#include "smb.h"
#include "io_modules.h"

#define SMB_DEVICE   "smb0:"
#define SMB_PORT     445

/*
 * IRX usados pelo cliente SMB.
 *
 * A pilha TCP/IP (PS2IP-NM + PS2IPS) é carregada por network.c.
 * A pilha de rede principal permanece sob responsabilidade de network.c.
 * IOMANX/FILEXIO: io_modules_init() (compartilhado com USB/HDD).
 */
extern unsigned char SMBMAN_irx[];
extern unsigned int size_SMBMAN_irx;

static SMBInfo smbInfo;
static int smbmanLoaded = 0;

/* ============================================================
 * ERRO
 * ============================================================ */

static void smb_set_error(
    const char *message,
    int code
)
{
    if (code != 0)
    {
        snprintf(
            smbInfo.error,
            sizeof(smbInfo.error),
            "%s (%d)",
            message,
            code
        );
    }
    else
    {
        snprintf(
            smbInfo.error,
            sizeof(smbInfo.error),
            "%s",
            message
        );
    }
}


/* ============================================================
 * ISO
 * ============================================================ */

static int smb_is_iso(
    const char *name
)
{
    size_t len;


    if (!name)
        return 0;


    len = strlen(name);


    if (len < 4)
        return 0;


    return
        strcasecmp(
            name + len - 4,
            ".iso"
        ) == 0;
}


/* ============================================================
 * TAMANHO 64-BIT
 * ============================================================ */

static u64 smb_stat_size(
    const iox_stat_t *stat
)
{
    return
        ((u64)stat->hisize << 32) |
        (u64)stat->size;
}


/* ============================================================
 * CARREGADOR IRX
 * ============================================================ */

static int smb_load_module(
    const char *name,
    void *buffer,
    unsigned int size
)
{
    int module_id;
    int module_result = 0;


    printf(
        "SMB: loading %s (%u bytes)\n",
        name,
        size
    );


    module_id =
        SifExecModuleBuffer(
            buffer,
            size,
            0,
            NULL,
            &module_result
        );


    printf(
        "SMB: %s SifExecModuleBuffer=%d start_result=%d\n",
        name,
        module_id,
        module_result
    );


    if (module_id < 0)
    {
        printf(
            "SMB: %s load failed: %d\n",
            name,
            module_id
        );

        return module_id;
    }


    if (module_result < 0)
    {
        printf(
            "SMB: %s _start failed: %d\n",
            name,
            module_result
        );

        return module_result;
    }


    printf(
        "SMB: %s loaded (id=%d)\n",
        name,
        module_id
    );


    return module_id;
}


/* ============================================================
 * INIT
 * ============================================================ */

int smb_init(void)
{
    int result;


    /*
     * Já inicializado.
     */

    if (smbInfo.initialized)
        return 0;


    memset(
        &smbInfo,
        0,
        sizeof(smbInfo)
    );


    printf("\n");
    printf("==============================\n");
    printf(" SMB CLIENT INIT\n");
    printf("==============================\n");


    printf(
        "SMB: initializing SMBMAN/fileXio client\n"
    );


    /*
     * ========================================================
     * MÓDULOS DE FILE I/O (compartilhados com USB/HDD)
     * ========================================================
     */

    result =
        io_modules_init();


    if (result < 0)
    {
        smb_set_error(
            "IOMANX/FILEXIO init failed",
            result
        );

        return -1;
    }


    /*
     * SMBMAN importa a biblioteca IOP "ps2ip", que ja foi
     * carregada por network_init() (PS2IP-NM). Nao carregar
     * uma segunda pilha aqui.
     */

    /*
     * ========================================================
     * SMBMAN
     * ========================================================
     */

    /*
     * smb_shutdown() zera smbInfo, mas o IRX continua residente:
     * nunca carregar uma segunda instancia do SMBMAN.
     */

    if (!smbmanLoaded)
    {
        printf(
            "SMB: loading SMBMAN\n"
        );

        result =
            smb_load_module(
                "SMBMAN",
                SMBMAN_irx,
                size_SMBMAN_irx
            );

        if (result < 0)
        {
            smb_set_error(
                "SMBMAN load failed",
                result
            );

            return -4;
        }

        smbmanLoaded = 1;
    }

    /*
     * Inicializado.
     */

    smbInfo.initialized = 1;

    smbInfo.error[0] = '\0';


    printf("==============================\n");
    printf(" SMB CLIENT READY\n");
    printf("==============================\n");


    return 0;
}


/* ============================================================
 * CONNECT
 * ============================================================ */

int smb_connect(
    const char *server,
    const char *share,
    const char *user,
    const char *password
)
{
    smbLogOn_in_t logon;
    smbEcho_in_t echo;
    smbOpenShare_in_t openShare;

    int result;


    if (!smbInfo.initialized)
    {
        smb_set_error(
            "SMB not initialized",
            0
        );

        return -1;
    }


    if (
        !server ||
        !share ||
        !user ||
        !password
    )
    {
        smb_set_error(
            "Invalid SMB parameters",
            0
        );

        return -2;
    }


    if (smbInfo.connected)
    {
        smb_disconnect();
    }


    memset(
        &logon,
        0,
        sizeof(logon)
    );


    memset(
        &echo,
        0,
        sizeof(echo)
    );


    memset(
        &openShare,
        0,
        sizeof(openShare)
    );


    /*
     * Servidor.
     */

    snprintf(
        logon.serverIP,
        sizeof(logon.serverIP),
        "%s",
        server
    );


    logon.serverPort =
        SMB_PORT;


    /*
     * Usuário.
     */

    snprintf(
        logon.User,
        sizeof(logon.User),
        "%s",
        user
    );


    /*
     * Senha.
     */

    if (password[0] != '\0')
    {
        snprintf(
            logon.Password,
            sizeof(logon.Password),
            "%s",
            password
        );


        logon.PasswordType =
            PLAINTEXT_PASSWORD;


        snprintf(
            openShare.Password,
            sizeof(openShare.Password),
            "%s",
            password
        );


        openShare.PasswordType =
            PLAINTEXT_PASSWORD;
    }
    else
    {
        /*
         * Nosso servidor atual:
         *
         * USER     = guest
         * PASSWORD = vazio
         */

        logon.PasswordType =
            NO_PASSWORD;


        openShare.PasswordType =
            NO_PASSWORD;
    }


    printf("\n");
    printf("==============================\n");
    printf(" SMB CONNECT\n");
    printf("==============================\n");


    printf(
        "SMB: server=%s port=%d\n",
        server,
        SMB_PORT
    );


    printf(
        "SMB: share=%s\n",
        share
    );


    printf(
        "SMB: user=%s\n",
        user
    );


    /*
     * ========================================================
     * LOGON
     * ========================================================
     */

    printf("SMB: [1/3] LOGON -> %s:%d\n", server, SMB_PORT);


    result =
        fileXioDevctl(
            SMB_DEVICE,
            SMB_DEVCTL_LOGON,
            &logon,
            sizeof(logon),
            NULL,
            0
        );


    printf(
        "SMB: LOGON returned %d\n",
        result
    );


    if (result < 0)
    {
        if (
            result ==
            -SMB_DEVCTL_LOGON_ERR_CONN
        )
        {
            smb_set_error(
                "LOGON: server connection failed",
                result
            );
        }
        else if (
            result ==
            -SMB_DEVCTL_LOGON_ERR_PROT
        )
        {
            smb_set_error(
                "LOGON: protocol error",
                result
            );
        }
        else if (
            result ==
            -SMB_DEVCTL_LOGON_ERR_LOGON
        )
        {
            smb_set_error(
                "LOGON: authentication failed",
                result
            );
        }
        else
        {
            smb_set_error(
                "SMB LOGON failed",
                result
            );
        }


        printf(
            "SMB: %s\n",
            smbInfo.error
        );


        return -3;
    }


    smbInfo.logged_on = 1;


    printf(
        "SMB: LOGON OK\n"
    );


    /*
     * ========================================================
     * ECHO
     * ========================================================
     */

    snprintf(
        echo.echo,
        sizeof(echo.echo),
        "PS2DVDPlayer ECHO"
    );


    echo.len =
        strlen(echo.echo);


    printf("SMB: [2/3] ECHO\n");


    result =
        fileXioDevctl(
            SMB_DEVICE,
            SMB_DEVCTL_ECHO,
            &echo,
            sizeof(echo),
            NULL,
            0
        );


    printf(
        "SMB: ECHO returned %d\n",
        result
    );


    if (result < 0)
    {
        smb_set_error(
            "SMB ECHO failed",
            result
        );


        printf(
            "SMB: %s\n",
            smbInfo.error
        );


        smb_disconnect();


        return -4;
    }


    printf(
        "SMB: ECHO OK\n"
    );


    /*
     * ========================================================
     * OPEN SHARE
     * ========================================================
     */

    snprintf(
        openShare.ShareName,
        sizeof(openShare.ShareName),
        "%s",
        share
    );


    printf("SMB: [3/3] OPENSHARE %s\n", share);


    result =
        fileXioDevctl(
            SMB_DEVICE,
            SMB_DEVCTL_OPENSHARE,
            &openShare,
            sizeof(openShare),
            NULL,
            0
        );


    printf(
        "SMB: OPENSHARE returned %d\n",
        result
    );


    if (result < 0)
    {
        smb_set_error(
            "SMB OPENSHARE failed",
            result
        );


        printf(
            "SMB: %s\n",
            smbInfo.error
        );


        smb_disconnect();


        return -5;
    }


    smbInfo.share_open = 1;
    smbInfo.connected = 1;


    snprintf(
        smbInfo.server,
        sizeof(smbInfo.server),
        "%s",
        server
    );


    snprintf(
        smbInfo.share,
        sizeof(smbInfo.share),
        "%s",
        share
    );


    snprintf(
        smbInfo.user,
        sizeof(smbInfo.user),
        "%s",
        user
    );


    smbInfo.error[0] =
        '\0';


    printf(
        "SMB: OPENSHARE OK\n"
    );


    printf(
        "SMB: connection ready at %s\n",
        SMB_DEVICE
    );


    return 0;
}


/* ============================================================
 * LISTA SMB_DVD_PATH
 * ============================================================ */

int smb_list_dvd(void)
{
    int dir;
    int result;

    iox_dirent_t entry
        __attribute__((aligned(64)));


    if (
        !smbInfo.connected ||
        !smbInfo.share_open
    )
    {
        smb_set_error(
            "SMB share is not open",
            0
        );

        return -1;
    }


    smbInfo.file_count = 0;


    printf("SMB: LIST: opening %s\n", SMB_DVD_PATH);


    dir =
        fileXioDopen(
            SMB_DVD_PATH
        );


    printf(
        "SMB: fileXioDopen returned %d\n",
        dir
    );


    if (dir < 0)
    {
        smb_set_error(
            "Cannot open " SMB_DVD_PATH,
            dir
        );


        printf(
            "SMB: %s\n",
            smbInfo.error
        );


        return -2;
    }


    while (1)
    {
        memset(
            &entry,
            0,
            sizeof(entry)
        );


        result =
            fileXioDread(
                dir,
                &entry
            );


        /*
         * O SMBMAN nao retorna 0 no fim do diretorio: quando o
         * servidor responde FIND_NEXT2 com SearchCount == 0,
         * smb_FindFirstNext2() devolve -EINVAL (-22).
         */

        if (result == -22)
            break;


        if (result < 0)
        {
            smb_set_error(
                "fileXioDread failed",
                result
            );


            printf(
                "SMB: %s\n",
                smbInfo.error
            );


            fileXioDclose(
                dir
            );


            return -3;
        }


        if (result == 0)
            break;


        /*
         * Ignora entradas inválidas.
         */

        if (
            entry.name[0] == '\0' ||
            strcmp(
                entry.name,
                "."
            ) == 0 ||
            strcmp(
                entry.name,
                ".."
            ) == 0
        )
        {
            continue;
        }


        /*
         * Somente ISO.
         */

        if (!smb_is_iso(entry.name))
            continue;


        /*
         * Limite.
         */

        if (
            smbInfo.file_count >=
            SMB_MAX_FILES
        )
        {
            printf(
                "SMB: maximum file count reached\n"
            );

            break;
        }


        snprintf(
            smbInfo.files[
                smbInfo.file_count
            ].name,

            sizeof(
                smbInfo.files[
                    smbInfo.file_count
                ].name
            ),

            "%s",
            entry.name
        );


        smbInfo.files[
            smbInfo.file_count
        ].size =
            smb_stat_size(
                &entry.stat
            );


        smbInfo.files[
            smbInfo.file_count
        ].is_iso = 1;


        printf(
            "SMB: ISO: %s (%llu bytes)\n",
            smbInfo.files[
                smbInfo.file_count
            ].name,

            (unsigned long long)
                smbInfo.files[
                    smbInfo.file_count
                ].size
        );


        smbInfo.file_count++;
    }


    fileXioDclose(
        dir
    );


    smbInfo.error[0] =
        '\0';


    printf(
        "SMB: %d ISO files found\n",
        smbInfo.file_count
    );


    return smbInfo.file_count;
}


/* ============================================================
 * DISCONNECT
 * ============================================================ */

void smb_disconnect(void)
{
    int result;


    if (!smbInfo.initialized)
        return;


    /*
     * Fecha share.
     */

    if (smbInfo.share_open)
    {
        printf(
            "SMB: CLOSESHARE\n"
        );


        result =
            fileXioDevctl(
                SMB_DEVICE,
                SMB_DEVCTL_CLOSESHARE,
                NULL,
                0,
                NULL,
                0
            );


        printf(
            "SMB: CLOSESHARE returned %d\n",
            result
        );


        smbInfo.share_open = 0;
    }


    /*
     * Logoff.
     */

    if (smbInfo.logged_on)
    {
        printf(
            "SMB: LOGOFF\n"
        );


        result =
            fileXioDevctl(
                SMB_DEVICE,
                SMB_DEVCTL_LOGOFF,
                NULL,
                0,
                NULL,
                0
            );


        printf(
            "SMB: LOGOFF returned %d\n",
            result
        );


        smbInfo.logged_on = 0;
    }


    smbInfo.connected = 0;
}


/* ============================================================
 * SHUTDOWN
 * ============================================================ */

void smb_shutdown(void)
{
    smb_disconnect();


    /*
     * Os IRX continuam residentes.
     */

    smbInfo.initialized = 0;


    printf(
        "SMB: shutdown\n"
    );
}


/* ============================================================
 * INFO
 * ============================================================ */

const SMBInfo *smb_get_info(void)
{
    return &smbInfo;
}