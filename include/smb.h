#ifndef PS2DVDPLAYER_SMB_H
#define PS2DVDPLAYER_SMB_H

#include <tamtypes.h>

#define SMB_MAX_FILES 64
#define SMB_MAX_NAME  128

/*
 * Pasta das ISOs de filmes no share.
 * Separada de /DVD e /CD, que são usadas pelo OPL para jogos.
 */
#define SMB_DVD_PATH "smb0:/FILMES"

typedef struct
{
    char name[SMB_MAX_NAME];
    u64 size;
    int is_iso;
} SMBFile;

typedef struct
{
    int initialized;
    int logged_on;
    int share_open;
    int connected;

    char server[64];
    char share[64];
    char user[64];

    int file_count;
    SMBFile files[SMB_MAX_FILES];

    char error[128];
} SMBInfo;

int smb_init(void);
int smb_connect(
    const char *server,
    const char *share,
    const char *user,
    const char *password
);
int smb_list_dvd(void);
void smb_disconnect(void);
void smb_shutdown(void);
const SMBInfo *smb_get_info(void);

#endif
