#ifndef BLIKACHU_UF2_DISK_H
#define BLIKACHU_UF2_DISK_H
int blikachu_uf2_init(void);
void blikachu_uf2_complete(void);
void dfu_screen_message(const char *title, const char *subtitle);
void dfu_screen_progress(unsigned percent);
#endif
