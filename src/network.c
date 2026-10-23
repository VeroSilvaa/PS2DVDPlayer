#include <stdio.h>
#include <string.h>

#include <tamtypes.h>
#include <kernel.h>
#include <timer.h>
#include <iopcontrol.h>
#include <iopheap.h>
#include <netman.h>
#include <ps2ips.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <sbv_patches.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "network.h"

/*
 * Base: versao do network.c em que o DHCP funcionou no PCSX2.
 *
 * IMPORTANTE:
 * - DHCP NAO e iniciado dentro de network_init().
 * - O main/tela SMB chama network_start_dhcp().
 * - A pilha TCP/IP roda no IOP (PS2IP-NM), como no OPL, para
 *   que o SMBMAN use a mesma interface que recebe o DHCP.
 *   O EE acessa essa pilha via RPC (PS2IPS / -lps2ips).
 */

extern unsigned char DEV9_irx[];
extern unsigned int size_DEV9_irx;

extern unsigned char SMAP_irx[];
extern unsigned int size_SMAP_irx;

extern unsigned char NETMAN_irx[];
extern unsigned int size_NETMAN_irx;

extern unsigned char PS2IP_irx[];
extern unsigned int size_PS2IP_irx;

extern unsigned char PS2IPS_irx[];
extern unsigned int size_PS2IPS_irx;

static NetworkInfo networkInfo;
static int networkStarted = 0;


/* ============================================================
 * Utilitarios
 * ============================================================ */

static void network_clear_info(void)
{
    memset(&networkInfo, 0, sizeof(networkInfo));

    networkInfo.mode = NETWORK_DHCP;

    strcpy(networkInfo.ip, "0.0.0.0");
    strcpy(networkInfo.netmask, "0.0.0.0");
    strcpy(networkInfo.gateway, "0.0.0.0");
    strcpy(networkInfo.dns, "0.0.0.0");
}


static void network_copy_ip(
    char *dst,
    const struct ip4_addr *addr
)
{
    snprintf(
        dst,
        16,
        "%d.%d.%d.%d",
        ip4_addr1(addr),
        ip4_addr2(addr),
        ip4_addr3(addr),
        ip4_addr4(addr)
    );
}


/* ============================================================
 * Link
 * ============================================================ */

static int network_link_status(void)
{
    return (
        NetManIoctl(
            NETMAN_NETIF_IOCTL_GET_LINK_STATUS,
            NULL,
            0,
            NULL,
            0
        ) == NETMAN_NETIF_ETH_LINK_STATE_UP
    );
}


/* ============================================================
 * DHCP
 * ============================================================ */

/*
 * Logo depois de ligar o DHCP o estado ainda é OFF (a negociação não
 * começou): contar isso como pronto fazia o primeiro LOGON SMB sair
 * 15 ms depois e cair (-4097) quando o DHCP reconfigurava a interface.
 * OFF só vale como pronto depois que o DHCP passou por outro estado ou
 * depois de DHCP_SETTLE_MS (comportamento antigo, como reserva).
 */
#define DHCP_SETTLE_MS 3000

static u64 dhcp_started_at = 0;
static int dhcp_seen_active = 0;
static int dhcp_last_status = -1;

static int network_dhcp_status(void)
{
    t_ip_info ip_info;
    int result;

    result = ps2ip_getconfig("sm0", &ip_info);

    if (result >= 0)
    {
        if (ip_info.dhcp_enabled)
        {
            int st = ip_info.dhcp_status;

            if (st != dhcp_last_status)
            {
                printf("NETWORK: DHCP state %d -> %d\n", dhcp_last_status, st);
                dhcp_last_status = st;
            }

            if (st == DHCP_STATE_BOUND)
                return 1;

            if (st == DHCP_STATE_OFF)
            {
                u64 settle = (u64)kBUSCLK * DHCP_SETTLE_MS / 1000;

                if (dhcp_seen_active || dhcp_started_at == 0 ||
                    GetTimerSystemTime() - dhcp_started_at > settle)
                    return 1;

                return 0;
            }

            dhcp_seen_active = 1;
            return 0;
        }
    }

    return -1;
}


static int network_apply_dhcp(void)
{
    t_ip_info ip_info;
    int result;

    result = ps2ip_getconfig("sm0", &ip_info);

    if (result < 0)
        return result;

    ip_info.dhcp_enabled = 1;

    result = ps2ip_setconfig(&ip_info);

    if (result < 0)
        return result;

    /* ver network_dhcp_status(): OFF logo agora ainda não é pronto */
    dhcp_started_at = GetTimerSystemTime();
    dhcp_seen_active = 0;

    networkInfo.mode = NETWORK_DHCP;

    return 0;
}


/* ============================================================
 * Leitura da configuracao atual
 * ============================================================ */

static int network_read_config(void)
{
    t_ip_info ip_info;
    const ip_addr_t *dns;

    if (ps2ip_getconfig("sm0", &ip_info) < 0)
        return -1;

    dns = dns_getserver(0);

    network_copy_ip(
        networkInfo.ip,
        (const struct ip4_addr *)&ip_info.ipaddr
    );

    network_copy_ip(
        networkInfo.netmask,
        (const struct ip4_addr *)&ip_info.netmask
    );

    network_copy_ip(
        networkInfo.gateway,
        (const struct ip4_addr *)&ip_info.gw
    );

    if (dns != NULL)
    {
        network_copy_ip(
            networkInfo.dns,
            (const struct ip4_addr *)dns
        );
    }
    else
    {
        strcpy(networkInfo.dns, "0.0.0.0");
    }

    networkInfo.dhcp_bound =
        (ip_info.dhcp_status == DHCP_STATE_BOUND);

    return 0;
}


/* ============================================================
 * Inicializacao
 * ============================================================ */

int network_init(void)
{
    int result;

    if (networkStarted)
    {
        printf("NETWORK: already initialized\n");
        return 0;
    }

    network_clear_info();

    printf("NETWORK: initializing\n");

    /*
     * O iop_boot_reset() permanece responsabilidade do main.
     * Aqui apenas reinicializamos os servicos EE necessarios
     * depois do reset do IOP.
     */
    sceSifInitRpc(0);
    SifLoadFileInit();
    SifInitIopHeap();
    sbv_patch_enable_lmb();

    printf("NETWORK: loading DEV9\n");

    result = SifExecModuleBuffer(
        DEV9_irx,
        size_DEV9_irx,
        0,
        NULL,
        NULL
    );

    if (result < 0)
    {
        printf("NETWORK: DEV9 failed: %d\n", result);
        return -1;
    }


    printf("NETWORK: loading NETMAN\n");

    result = SifExecModuleBuffer(
        NETMAN_irx,
        size_NETMAN_irx,
        0,
        NULL,
        NULL
    );

    if (result < 0)
    {
        printf("NETWORK: NETMAN failed: %d\n", result);
        return -1;
    }


    printf("NETWORK: loading SMAP\n");

    result = SifExecModuleBuffer(
        SMAP_irx,
        size_SMAP_irx,
        0,
        NULL,
        NULL
    );

    if (result < 0)
    {
        printf("NETWORK: SMAP failed: %d\n", result);
        return -1;
    }


    printf("NETWORK: NetManInit\n");

    result = NetManInit();

    if (result < 0)
    {
        printf("NETWORK: NetManInit failed: %d\n", result);
        return -1;
    }


    /*
     * Pilha TCP/IP no IOP, na mesma ordem do OPL:
     * SMAP -> PS2IP-NM -> PS2IPS -> ps2ip_init() no EE.
     * Nao alteramos o link mode aqui.
     */
    printf("NETWORK: loading PS2IP-NM\n");

    result = SifExecModuleBuffer(
        PS2IP_irx,
        size_PS2IP_irx,
        0,
        NULL,
        NULL
    );

    if (result < 0)
    {
        printf("NETWORK: PS2IP-NM failed: %d\n", result);

        NetManDeinit();

        return -1;
    }


    printf("NETWORK: loading PS2IPS\n");

    result = SifExecModuleBuffer(
        PS2IPS_irx,
        size_PS2IPS_irx,
        0,
        NULL,
        NULL
    );

    if (result < 0)
    {
        printf("NETWORK: PS2IPS failed: %d\n", result);

        NetManDeinit();

        return -1;
    }


    printf("NETWORK: ps2ip_init\n");

    result = ps2ip_init();

    if (result < 0)
    {
        printf("NETWORK: ps2ip_init failed: %d\n", result);

        NetManDeinit();

        return -1;
    }

    networkStarted = 1;
    networkInfo.initialized = 1;
    networkInfo.mode = NETWORK_DHCP;

    printf("NETWORK: initialized\n");

    return 0;
}


/* ============================================================
 * Iniciar DHCP
 * ============================================================ */

int network_start_dhcp(void)
{
    int result;

    if (!networkStarted)
    {
        printf("NETWORK: not initialized\n");
        return -1;
    }

    printf("NETWORK: enabling DHCP\n");

    result = network_apply_dhcp();

    if (result < 0)
    {
        printf(
            "NETWORK: DHCP configuration failed: %d\n",
            result
        );

        return -1;
    }

    printf("NETWORK: DHCP enabled\n");

    return 0;
}


/* ============================================================
 * Configuracao estatica
 * ============================================================ */

int network_apply_static(
    const char *ip,
    const char *netmask,
    const char *gateway,
    const char *dns
)
{
    struct ip4_addr ip_addr;
    struct ip4_addr netmask_addr;
    struct ip4_addr gateway_addr;
    struct ip4_addr dns_addr;

    t_ip_info ip_info;

    int result;
    unsigned int a;
    unsigned int b;
    unsigned int c;
    unsigned int d;

    if (sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
        return -1;

    if (a > 255 || b > 255 || c > 255 || d > 255)
        return -1;

    ip_addr.addr =
        ((u32)a) |
        ((u32)b << 8) |
        ((u32)c << 16) |
        ((u32)d << 24);


    if (sscanf(netmask, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
        return -1;

    if (a > 255 || b > 255 || c > 255 || d > 255)
        return -1;

    netmask_addr.addr =
        ((u32)a) |
        ((u32)b << 8) |
        ((u32)c << 16) |
        ((u32)d << 24);


    if (sscanf(gateway, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
        return -1;

    if (a > 255 || b > 255 || c > 255 || d > 255)
        return -1;

    gateway_addr.addr =
        ((u32)a) |
        ((u32)b << 8) |
        ((u32)c << 16) |
        ((u32)d << 24);


    if (sscanf(dns, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
        return -1;

    if (a > 255 || b > 255 || c > 255 || d > 255)
        return -1;

    dns_addr.addr =
        ((u32)a) |
        ((u32)b << 8) |
        ((u32)c << 16) |
        ((u32)d << 24);


    result = ps2ip_getconfig("sm0", &ip_info);

    if (result < 0)
    {
        printf(
            "NETWORK: ps2ip_getconfig failed: %d\n",
            result
        );

        return -1;
    }

    ip_info.ipaddr.s_addr = ip_addr.addr;
    ip_info.netmask.s_addr = netmask_addr.addr;
    ip_info.gw.s_addr = gateway_addr.addr;
    ip_info.dhcp_enabled = 0;

    result = ps2ip_setconfig(&ip_info);

    if (result < 0)
    {
        printf(
            "NETWORK: ps2ip_setconfig failed: %d\n",
            result
        );

        return -1;
    }

    dns_setserver(0, &dns_addr);

    networkInfo.mode = NETWORK_STATIC;
    networkInfo.initialized = 1;
    networkInfo.link_up = network_link_status();
    networkInfo.dhcp_bound = 0;

    network_copy_ip(networkInfo.ip, &ip_addr);
    network_copy_ip(networkInfo.netmask, &netmask_addr);
    network_copy_ip(networkInfo.gateway, &gateway_addr);
    network_copy_ip(networkInfo.dns, &dns_addr);

    printf("NETWORK: static configuration applied\n");
    printf("NETWORK: IP      %s\n", networkInfo.ip);
    printf("NETWORK: NETMASK %s\n", networkInfo.netmask);
    printf("NETWORK: GATEWAY %s\n", networkInfo.gateway);
    printf("NETWORK: DNS     %s\n", networkInfo.dns);

    return 0;
}


/* ============================================================
 * Atualizacao
 * ============================================================ */

void network_update(void)
{
    int link;
    int dhcp;

    if (!networkStarted)
        return;

    link = network_link_status();

    networkInfo.link_up = link ? 1 : 0;

    dhcp = network_dhcp_status();

    if (dhcp > 0)
        networkInfo.dhcp_bound = 1;
    else
        networkInfo.dhcp_bound = 0;

    network_read_config();
}


/* ============================================================
 * Shutdown
 * ============================================================ */

void network_shutdown(void)
{
    if (!networkStarted)
        return;

    printf("NETWORK: shutdown\n");

    ps2ip_deinit();
    NetManDeinit();

    networkStarted = 0;

    network_clear_info();
}


/* ============================================================
 * Teste TCP
 * ============================================================ */

int network_test_tcp(
    const char *ip,
    int port,
    int timeout_ms
)
{
    int sock;
    struct sockaddr_in addr;
    int result;

    (void)timeout_ms;

    printf("\n");
    printf("NETWORK: TCP TEST\n");
    printf("NETWORK: target=%s:%d\n", ip, port);

    sock = socket(
        AF_INET,
        SOCK_STREAM,
        0
    );

    if (sock < 0)
    {
        printf("NETWORK: socket failed: %d\n", sock);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    result = inet_aton(
        ip,
        &addr.sin_addr
    );

    if (!result)
    {
        printf("NETWORK: invalid IP: %s\n", ip);

        close(sock);

        return -2;
    }

    printf("NETWORK: connecting...\n");

    result = connect(
        sock,
        (struct sockaddr *)&addr,
        sizeof(addr)
    );

    if (result < 0)
    {
        printf("NETWORK: TCP connection failed\n");

        close(sock);

        return -3;
    }

    printf("NETWORK: TCP connection OK\n");

    close(sock);

    return 0;
}


/* ============================================================
 * Get Info
 * ============================================================ */

const NetworkInfo *network_get_info(void)
{
    return &networkInfo;
}
