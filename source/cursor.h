#ifndef BK_CURSOR_H
#define BK_CURSOR_H

/* Stick cursor overlay, drawn into the game's framebuffer right before each
 * eglSwapBuffers. All GL state it touches is saved and restored. */
void cursor_set(int visible, float x, float y);
void cursor_draw(int surface_w, int surface_h);

#endif
