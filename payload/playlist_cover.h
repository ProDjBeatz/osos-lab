/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef CFW_PLAYLIST_COVER_H
#define CFW_PLAYLIST_COVER_H
#include "osos.h"

/* Playlists hold menu: add Set Playlist Photo. */
void cfw_playlist_cover_menu(struct osos_context_menu *menu);
/* Returns 1 when action was Set Playlist Photo (handled or support-queried). */
int cfw_playlist_cover_action(void *controller, const char *action, uint32_t argument);

#endif
