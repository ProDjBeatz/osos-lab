/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef CFW_PLAYLIST_RENAME_H
#define CFW_PLAYLIST_RENAME_H
#include <stdint.h>

/* Returns 1 when action was a rename action (handled or support-queried). */
int cfw_playlist_rename_action(void *controller, const char *action,
                               uint32_t argument);

#endif
