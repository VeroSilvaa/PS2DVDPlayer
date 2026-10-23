#ifndef PS2DVDPLAYER_DBGLOG_H
#define PS2DVDPLAYER_DBGLOG_H

/*
 * Log de depuração no compartilhamento SMB (no PS2 físico não há o
 * console do PCSX2): tudo o que o programa escreve com printf também
 * vai para um buffer na RAM, gravado em smb0:/LOGS/ps2dvdplayer.log.
 *
 * Incluir depois de <stdio.h>: printf passa a ser dbglog_printf (escreve
 * no console como antes e guarda uma cópia). dbglog.c não usa o macro.
 */

#include <stdio.h>

/*
 * Desligado por enquanto (será usado no PS2 físico): com 0, printf é o
 * normal, o buffer de 512 KB não é compilado e nada é gravado no SMB.
 * Para ativar: 1.
 */
#define DBGLOG_ENABLED 0

int dbglog_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/*
 * Grava no SMB o que ainda não foi gravado. Só chamar fora da
 * reprodução de vídeo (o fileXio e a rede ficam com o filme). Sem SMB
 * conectado, não faz nada (o texto continua no buffer).
 */
void dbglog_flush(void);

#define DBGLOG_DIR  "smb0:/LOGS"
#define DBGLOG_FILE DBGLOG_DIR "/ps2dvdplayer.log"

#if DBGLOG_ENABLED && !defined(DBGLOG_NO_MACRO)
#define printf dbglog_printf
#endif

#endif
