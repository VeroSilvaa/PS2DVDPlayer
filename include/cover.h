#ifndef PS2DVDPLAYER_COVER_H
#define PS2DVDPLAYER_COVER_H

/*
 * Capa do filme: PNG com o mesmo nome da ISO na pasta ART, ao lado da
 * pasta dos filmes, em cada mídia:
 *
 *   usb0:/FILMES/Filme.iso  ->  usb0:/ART/Filme.png
 *   smb0:/FILMES/Filme.iso  ->  smb0:/ART/Filme.png
 *
 * Tamanho da tela: COVER_W x COVER_H (proporção da capa de DVD).
 * Maiores (até COVER_MAX_W x COVER_MAX_H) são reduzidos no PS2.
 */

#define COVER_W      160
#define COVER_H      224
#define COVER_MAX_W  1024
#define COVER_MAX_H  1536

#define COVER_DIR    "ART"

/*
 * "usb0:/ART/Filme.png" para a ISO iso_name da pasta lib_dir
 * (ex. "usb0:/FILMES"). rel (opcional): só "ART/Filme.png".
 */
void cover_path(char *out, int size, const char *lib_dir, const char *iso_name,
                char *rel, int rel_size);

/* Carrega a capa (ui_cover_set). 1 = carregada, 0 = não há, < 0 = erro. */
int cover_load(const char *lib_dir, const char *iso_name);

#endif
