#ifndef BK_INPUT_H
#define BK_INPUT_H

/* Controller and touchscreen -> Android input events.
 *
 *   A / X / D-pad Up        jump   (KEY_SPACE / KEY_UP)
 *   B / L / ZL / D-pad Down dive   (KEY_DOWN)
 *   Y / R / ZR / D-pad Right dash  (KEY_RIGHT)
 *   +                       back / pause (KEY_ESC, bound to the game's "back")
 *   Left stick              moves an on-screen cursor; while it is shown, A taps
 *   -                       show / hide the cursor
 *   Touchscreen             touches, handheld mode
 *
 * The game also binds ~50 debug and cheat keys (digits, letters, F-keys, TAB).
 * Only the keycodes above are ever generated. */

void input_init(void);
void input_update(void);

#endif
