/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Rename Playlist, milestone 1: a "Rename Playlist" item in the Playlists hold
 * menu that opens the stock Search keyboard. Capturing the typed text and
 * applying it (native set-name 0x0805c3d8, persisted by playlist ID) follows
 * once the keyboard plumbing is mapped.
 */
#include "osos.h"
#include "cfw.h"
#include "patch.h"
#include "patches/sites.h"
#include "playlist_rename.h"
#include "playlist_rename_ui.h"

/* Row highlighted when the Playlists hold menu was built; -1 when none. */
static int menu_row = -1;
/* Row chosen for the rename in progress; -1 when idle. */
static int rename_row = -1;

/* TSilverMediaListCntlr_Playlists vtable +0x148: remember the held row. */
PATCH_ARM int cfw_playlists_menu(void *controller, int row)
{
    typedef int (*build)(void *, int);
    menu_row = row;
    return ((build)OSOS_PLAYLISTS_BUILD_MENU)(controller, row);
}

/* Replaces the Cancel append in the Playlists hold menu: Rename, then Cancel. */
PATCH_ARM void cfw_playlists_menu_cancel(struct osos_context_menu *menu,
                                         uint32_t label, const char *action,
                                         uint32_t flags)
{
    osos_context_menu_append(menu, CFW_RENAME_PLAYLIST, "CFW_RenamePlaylist", 1);
    osos_context_menu_append(menu, label, action, flags);
}

int cfw_playlist_rename_action(void *controller, const char *action,
                               uint32_t argument)
{
    (void)controller;
    if (!cfw_string_equal(action, "CFW_RenamePlaylist"))
        return 0;
    if (argument != OSOS_ACTION_SUPPORT_QUERY)
        rename_row = menu_row;
    /* The screen event then pushes Search with its keyboard. */
    return 1;
}
