/* SPDX-License-Identifier: GPL-3.0-only */

#include "osos.h"
#include "patches/sites.h"

.syntax unified
.cpu arm926ej-s
.arm
.text
.balign 4

/* Replay the two instructions replaced at the playlist name getter entry. movs sets
   the flags the native body tests right after (popeq), and ldr pc keeps them. */
.global cfw_playlist_get_name_original
.type cfw_playlist_get_name_original,%function
cfw_playlist_get_name_original:
    movs r2, r1
    mov r3, r0
    ldr pc, =OSOS_PLAYLIST_GET_NAME_RESUME

/* Rename keyboard: the instructions each PATCH_JUMP replaced, then resume. */
.global cfw_search_key_original
.type cfw_search_key_original,%function
cfw_search_key_original:
    push {r4, r5, r6, r7, r8, r9, r10, lr}
    mov r7, r0
    ldr pc, =OSOS_SEARCH_KEY_RESUME

.global cfw_key_label_original
.type cfw_key_label_original,%function
cfw_key_label_original:
    push {r4, r5, r6, r7, r8, lr}
    mov r6, r0
    ldr pc, =OSOS_KEY_LABEL_RESUME

.global cfw_key_text_original
.type cfw_key_text_original,%function
cfw_key_text_original:
    sub r0, r0, #0x6f00
    sub r0, r0, #0x81
    ldr pc, =OSOS_KEY_TEXT_RESUME
.ltorg
