#ifndef BK_APP_H
#define BK_APP_H

/* Services main.c provides to the shims. */
void        app_request_exit(int code);
int         app_exit_requested(void);
int         app_is_main_thread(void);
const char *app_game_dir(void);    /* sdmc:/switch/bananakong            */
const char *app_files_dir(void);   /* /switch/bananakong/files (no device) */
const char *app_assets_dir(void);  /* sdmc:/switch/bananakong/assets     */

#endif
