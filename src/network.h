#ifndef PS2DVDPLAYER_NETWORK_H
#define PS2DVDPLAYER_NETWORK_H

#include <tamtypes.h>

typedef enum
{
    NETWORK_DHCP = 0,
    NETWORK_STATIC
} NetworkMode;

typedef struct
{
    NetworkMode mode;

    int initialized;
    int link_up;
    int dhcp_bound;

    char ip[16];
    char netmask[16];
    char gateway[16];
    char dns[16];

} NetworkInfo;

int network_init(void);

int network_start_dhcp(void);

int network_apply_static(
    const char *ip,
    const char *netmask,
    const char *gateway,
    const char *dns
);
int network_test_tcp(
    const char *ip,
    int port,
    int timeout_ms
);

void network_update(void);

void network_shutdown(void);

const NetworkInfo *network_get_info(void);

int network_test_tcp(
    const char *ip,
    int port,
    int timeout_ms
);

#endif