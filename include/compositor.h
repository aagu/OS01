#include "sheet.h"
#include "kernel.h"

typedef struct WINDOW
{
    char *name;
    int x0, y0;
    int width, height;
    int depth;
    unsigned char *win_buf;
    struct SHEET *sht;
} window;

SHTCTL *shtctl;
SHEET *sht_mouse;
SHEET *sht_back;
MEMMAN *mem;
struct BOOTINFO *binfo;

extern const char closebtn[14][16];

void compositor_init(struct BOOTINFO *bootinfo);

window *create_window(int x0, int y0, int xsize, int ysize, char *title);

void create_background();

void create_mouse();

void make_textbox(struct SHEET *sht, int x0, int y0, int sx, int sy, int c);