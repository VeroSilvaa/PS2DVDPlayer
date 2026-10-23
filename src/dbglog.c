#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include <tamtypes.h>

#define NEWLIB_PORT_AWARE
#include <io_common.h>
#include <fileXio_rpc.h>

#define DBGLOG_NO_MACRO
#include "dbglog.h"
#include "smb.h"

#if DBGLOG_ENABLED

/*
 * Buffer circular do texto do console. total_in conta tudo o que já
 * entrou; total_out, o que já foi gravado no SMB. Se o buffer enche
 * (ex.: um vídeo longo), o mais antigo é perdido e o arquivo avisa.
 */
#define DBGLOG_RING (512 * 1024)

static char ring[DBGLOG_RING];
static u64 total_in = 0;
static u64 total_out = 0;

static int file_started = 0;    /* arquivo já criado nesta sessão */
static int disabled = 0;        /* gravação falhou: não tenta de novo */


static void ring_put(const char *text, int len)
{
    while (len > 0)
    {
        int pos = (int)(total_in % DBGLOG_RING);
        int n = DBGLOG_RING - pos;

        if (n > len)
            n = len;

        memcpy(ring + pos, text, n);
        total_in += n;
        text += n;
        len -= n;
    }
}


int dbglog_printf(const char *fmt, ...)
{
    /* frame_log_flush manda até 8 KB de uma vez (um só thread: static) */
    static char line[9216];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    if (n < 0)
        return n;

    if (n >= (int)sizeof(line))
        n = sizeof(line) - 1;

    ring_put(line, n);

    return printf("%s", line);
}


static int write_all(int fd, const char *data, int len)
{
    while (len > 0)
    {
        int n = fileXioWrite(fd, (void *)data, len > 16384 ? 16384 : len);

        if (n <= 0)
            return -1;

        data += n;
        len -= n;
    }

    return 0;
}


void dbglog_flush(void)
{
    const SMBInfo *smb = smb_get_info();
    char marker[96];
    int fd;

    if (disabled || total_out == total_in || !smb || !smb->connected)
        return;

    /* primeira gravação da sessão: pasta LOGS e arquivo novo */
    if (!file_started)
    {
        fileXioMkdir(DBGLOG_DIR, 0777);
        fd = fileXioOpen(DBGLOG_FILE, FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC);
    }
    else
    {
        fd = fileXioOpen(DBGLOG_FILE, FIO_O_WRONLY | FIO_O_CREAT);

        if (fd >= 0)
            fileXioLseek(fd, 0, FIO_SEEK_END);
    }

    if (fd < 0)
    {
        disabled = 1;
        printf("DBGLOG: cannot open " DBGLOG_FILE " (%d), SMB log disabled\n", fd);
        return;
    }

    if (!file_started)
    {
        const char *header = "PS2DVDPlayer - log de depuracao (" __DATE__ " " __TIME__ ")\n\n";

        write_all(fd, header, strlen(header));
        file_started = 1;
    }

    /* o buffer deu a volta: parte do texto foi perdida */
    if (total_in - total_out > DBGLOG_RING)
    {
        u64 lost = total_in - total_out - DBGLOG_RING;

        snprintf(marker, sizeof(marker), "\n[... %llu bytes perdidos (buffer cheio) ...]\n\n",
                 (unsigned long long)lost);
        write_all(fd, marker, strlen(marker));
        total_out = total_in - DBGLOG_RING;
    }

    while (total_out < total_in)
    {
        int pos = (int)(total_out % DBGLOG_RING);
        int n = DBGLOG_RING - pos;

        if ((u64)n > total_in - total_out)
            n = (int)(total_in - total_out);

        if (write_all(fd, ring + pos, n) < 0)
        {
            disabled = 1;
            break;
        }

        total_out += n;
    }

    fileXioClose(fd);

    if (disabled)
        printf("DBGLOG: write to " DBGLOG_FILE " failed, SMB log disabled\n");
}

#else /* DBGLOG_ENABLED == 0: só os símbolos, sem buffer nem gravação */

int dbglog_printf(const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vprintf(fmt, ap);
    va_end(ap);

    return n;
}


void dbglog_flush(void)
{
}

#endif
