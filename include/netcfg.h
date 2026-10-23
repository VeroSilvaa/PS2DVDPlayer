#ifndef PS2DVDPLAYER_NETCFG_H
#define PS2DVDPLAYER_NETCFG_H

/*
 * Preferências de rede/SMB do usuário no Memory Card, em texto
 * ("chave=valor"): mc0:/PS2DVDPLAYER/network.cfg (ou mc1: se não
 * houver cartão no slot 1). A senha fica em texto puro.
 *
 * Usa a libmc (XMCSERV carregado por memcard_init()).
 */

#define NETCFG_TEXT 64

typedef struct
{
    int dhcp;                   /* 1 = DHCP, 0 = estático */

    int ip[4];
    int mask[4];
    int gateway[4];
    int dns[4];
    int server[4];              /* servidor SMB (porta 445) */

    char share[NETCFG_TEXT];
    char user[NETCFG_TEXT];
    char password[NETCFG_TEXT];
} NetConfig;

NetConfig *netcfg_get(void);

/*
 * 1 = lido do Memory Card; 0 = sem arquivo (fica o padrão);
 * < 0 = Memory Card indisponível.
 */
int netcfg_load(void);

/*
 * Grava se mudou desde a última leitura/gravação.
 * 1 = gravado; 0 = nada mudou; < 0 = erro.
 */
int netcfg_save(void);

/* "mc0:/PS2DVDPLAYER/network.cfg" (ou mc1:), vazio se nunca usado. */
const char *netcfg_path(void);

#endif
