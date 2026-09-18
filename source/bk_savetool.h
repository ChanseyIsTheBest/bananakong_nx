/* bk_savetool.h -- edit the game's own saves at boot. MIT licensed.
 *
 * Reads saves.txt next to the .nro and writes the values it names into the
 * game's save files, in the game's own format, before the engine loads them.
 * Generates a fully commented saves.txt on first run listing every profile
 * field the game knows, so the file itself documents what can be changed.
 *
 * Call once from main(), after the paths are known and before the module is
 * loaded.
 */
#ifndef BK_SAVETOOL_H
#define BK_SAVETOOL_H

void bk_savetool_apply(void);

#endif
