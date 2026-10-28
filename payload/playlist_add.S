/* SPDX-License-Identifier: GPL-3.0-only */

#include "osos.h"
#include "patches/sites.h"

.syntax unified
.cpu arm926ej-s
.arm
.text
.balign 4

/* TSilverMediaListCntlr_Playlists appear: the library browser call (vtable +0x5c)
   that puts the browser on the Playlists category. Replays the two instructions
   PATCH_JUMP replaced, then skips the call while the Add to Playlist chooser is
   open (once, at its first appear), so the song list it came from keeps its album, artist or genre. */
.global cfw_playlists_appear_category
.type cfw_playlists_appear_category,%function
cfw_playlists_appear_category:
    ldr r12, =cfw_playlist_chooser_open
    ldr r2, [r12]
    cmp r2, #0
    beq 1f
    /* Once per chooser: clear the flag, skip the call (r2 and r3 are reloaded
       before they are used again). */
    mov r2, #0
    str r2, [r12]
    ldr pc, =OSOS_PLAYLISTS_APPEAR_CATEGORY_SKIP
1:
    mov r2, #0
    ldr r3, [r1, #0x5c]
    ldr pc, =OSOS_PLAYLISTS_APPEAR_CATEGORY_RESUME
.ltorg

/* Replays the two instructions PATCH_JUMP replaced at OSOS_SONGS_APPEAR. */
.global cfw_songs_appear_original
.type cfw_songs_appear_original,%function
cfw_songs_appear_original:
    push {r4, r5, r6, r7, r8, r9, r10, lr}
    sub sp, sp, #24
    ldr pc, =OSOS_SONGS_APPEAR_RESUME
.ltorg
