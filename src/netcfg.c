#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tamtypes.h>
#include <libmc.h>

/* só pelos flags FIO_O_* esperados pelo MCSERV */
#define NEWLIB_PORT_AWARE
#include <io_common.h>

#include "dbglog.h"
#include "netcfg.h"
#include "memcard.h"

#define NETCFG_DIR  "/PS2DVDPLAYER"
#define NETCFG_FILE NETCFG_DIR "/network.cfg"
#define NETCFG_MAX  1024

static NetConfig cfg;
static NetConfig saved;             /* conteúdo do cartão (lido/gravado) */
static int saved_port = -1;         /* cartão de onde veio, -1 = nenhum */
static char cfg_path[48];


/* Espera a operação assíncrona da libmc; devolve o resultado. */
static int mc_wait(void)
{
    int cmd = 0;
    int result = 0;

    mcSync(MC_WAIT, &cmd, &result);
    return result;
}


static void netcfg_defaults(NetConfig *c)
{
    static const int ip[4] = { 192, 168, 1, 50 };
    static const int mask[4] = { 255, 255, 255, 0 };
    static const int gateway[4] = { 192, 168, 1, 1 };
    static const int server[4] = { 192, 168, 1, 9 };

    memset(c, 0, sizeof(*c));

    c->dhcp = 1;
    memcpy(c->ip, ip, sizeof(ip));
    memcpy(c->mask, mask, sizeof(mask));
    memcpy(c->gateway, gateway, sizeof(gateway));
    memcpy(c->dns, gateway, sizeof(gateway));
    memcpy(c->server, server, sizeof(server));

    strcpy(c->share, "samba");
    strcpy(c->folder, "FILMES");
    strcpy(c->user, "guest");
}


NetConfig *netcfg_get(void)
{
    return &cfg;
}


const char *netcfg_path(void)
{
    return cfg_path;
}


static void parse_ip(int *out, const char *text)
{
    unsigned int a, b, c, d;

    if (sscanf(text, "%u.%u.%u.%u", &a, &b, &c, &d) == 4 &&
        a <= 255 && b <= 255 && c <= 255 && d <= 255)
    {
        out[0] = a;
        out[1] = b;
        out[2] = c;
        out[3] = d;
    }
}


static void parse_line(NetConfig *c, char *line)
{
    char *value = strchr(line, '=');

    if (line[0] == '#' || !value)
        return;

    *value++ = '\0';

    if (strcmp(line, "dhcp") == 0)
        c->dhcp = atoi(value) != 0;
    else if (strcmp(line, "ip") == 0)
        parse_ip(c->ip, value);
    else if (strcmp(line, "netmask") == 0)
        parse_ip(c->mask, value);
    else if (strcmp(line, "gateway") == 0)
        parse_ip(c->gateway, value);
    else if (strcmp(line, "dns") == 0)
        parse_ip(c->dns, value);
    else if (strcmp(line, "smb_server") == 0)
        parse_ip(c->server, value);
    else if (strcmp(line, "smb_share") == 0)
        snprintf(c->share, sizeof(c->share), "%s", value);
    else if (strcmp(line, "smb_folder") == 0)
        snprintf(c->folder, sizeof(c->folder), "%s", value);
    else if (strcmp(line, "smb_user") == 0)
        snprintf(c->user, sizeof(c->user), "%s", value);
    else if (strcmp(line, "smb_password") == 0)
        snprintf(c->password, sizeof(c->password), "%s", value);
}


static int read_port(int port, char *buf, int size)
{
    int fd;
    int n;

    if (mcOpen(port, 0, NETCFG_FILE, FIO_O_RDONLY) != 0)
        return -1;

    fd = mc_wait();

    if (fd < 0)
        return fd;

    mcRead(fd, buf, size - 1);
    n = mc_wait();

    mcClose(fd);
    mc_wait();

    if (n < 0)
        return n;

    buf[n] = '\0';
    return n;
}


int netcfg_load(void)
{
    static char buf[NETCFG_MAX];
    int port;

    netcfg_defaults(&cfg);
    saved = cfg;
    saved_port = -1;
    cfg_path[0] = '\0';

    if (!memcard_ready())
    {
        printf("NETCFG: memory card not ready, using defaults\n");
        return -1;
    }

    for (port = 0; port < 2; port++)
    {
        char *line;
        int n = read_port(port, buf, sizeof(buf));

        if (n < 0)
            continue;

        for (line = strtok(buf, "\r\n"); line; line = strtok(NULL, "\r\n"))
            parse_line(&cfg, line);

        saved = cfg;
        saved_port = port;
        snprintf(cfg_path, sizeof(cfg_path), "mc%d:" NETCFG_FILE, port);

        printf("NETCFG: loaded %s (%d bytes): %s, server %d.%d.%d.%d, share %s, user %s\n",
               cfg_path, n, cfg.dhcp ? "DHCP" : "STATIC",
               cfg.server[0], cfg.server[1], cfg.server[2], cfg.server[3],
               cfg.share, cfg.user);
        return 1;
    }

    printf("NETCFG: no saved settings, using defaults\n");
    return 0;
}


static int write_port(int port, const char *text, int len)
{
    int fd;
    int written;
    int r;

    /* a pasta pode já existir: erro ignorado */
    mcMkDir(port, 0, NETCFG_DIR);
    mc_wait();

    /* apaga antes: no Memory Card o truncamento não é garantido */
    mcDelete(port, 0, NETCFG_FILE);
    mc_wait();

    if (mcOpen(port, 0, NETCFG_FILE, FIO_O_WRONLY | FIO_O_CREAT) != 0)
        return -1;

    fd = mc_wait();

    if (fd < 0)
        return fd;

    mcWrite(fd, text, len);
    written = mc_wait();

    mcClose(fd);
    r = mc_wait();

    if (written != len)
        return -2;

    return r < 0 ? r : 0;
}


int netcfg_save(void)
{
    static char text[NETCFG_MAX];
    int len;
    int port;
    int i;

    if (memcmp(&cfg, &saved, sizeof(cfg)) == 0 && saved_port >= 0)
        return 0;

    if (!memcard_ready())
    {
        printf("NETCFG: memory card not ready, settings not saved\n");
        return -1;
    }

    len = snprintf(
        text, sizeof(text),
        "# PS2DVDPlayer - rede e SMB\n"
        "dhcp=%d\n"
        "ip=%d.%d.%d.%d\n"
        "netmask=%d.%d.%d.%d\n"
        "gateway=%d.%d.%d.%d\n"
        "dns=%d.%d.%d.%d\n"
        "smb_server=%d.%d.%d.%d\n"
        "smb_share=%s\n"
        "smb_folder=%s\n"
        "smb_user=%s\n"
        "smb_password=%s\n",
        cfg.dhcp,
        cfg.ip[0], cfg.ip[1], cfg.ip[2], cfg.ip[3],
        cfg.mask[0], cfg.mask[1], cfg.mask[2], cfg.mask[3],
        cfg.gateway[0], cfg.gateway[1], cfg.gateway[2], cfg.gateway[3],
        cfg.dns[0], cfg.dns[1], cfg.dns[2], cfg.dns[3],
        cfg.server[0], cfg.server[1], cfg.server[2], cfg.server[3],
        cfg.share, cfg.folder, cfg.user, cfg.password);

    /* mesmo cartão de onde foi lido; senão mc0:, depois mc1: */
    for (i = 0; i < 2; i++)
    {
        int r;

        port = saved_port >= 0 ? (saved_port + i) % 2 : i;
        r = write_port(port, text, len);

        printf("NETCFG: write mc%d:" NETCFG_FILE " -> %d\n", port, r);

        if (r == 0)
        {
            saved = cfg;
            saved_port = port;
            snprintf(cfg_path, sizeof(cfg_path), "mc%d:" NETCFG_FILE, port);
            return 1;
        }
    }

    return -2;
}
