/* SPDX-License-Identifier: GPL-3.0-only */

#include "osos.h"

.syntax unified
.cpu arm926ej-s
.arm
.text
.balign 4

/* TextView draw (0x08287218) colour call: r0 = context, r1 = colour, and r4 still
   holds the UTF-8 text being drawn. Pass it on so the rename screen can tell the
   typed name apart from other text. */
.global cfw_text_draw_color_stub
.type cfw_text_draw_color_stub,%function
cfw_text_draw_color_stub:
    mov r2, r4
    b cfw_text_draw_color
