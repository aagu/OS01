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
MEMMAN *mem;

static char closebtn[14][16] = {
		"OOOOOOOOOOOOOOO@",
		"OQQQQQQQQQQQQQ$@",
		"OQQQQQQQQQQQQQ$@",
		"OQQQ@@QQQQ@@QQ$@",
		"OQQQQ@@QQ@@QQQ$@",
		"OQQQQQ@@@@QQQQ$@",
		"OQQQQQQ@@QQQQQ$@",
		"OQQQQQ@@@@QQQQ$@",
		"OQQQQ@@QQ@@QQQ$@",
		"OQQQ@@QQQQ@@QQ$@",
		"OQQQQQQQQQQQQQ$@",
		"OQQQQQQQQQQQQQ$@",
		"O$$$$$$$$$$$$$$@",
		"@@@@@@@@@@@@@@@@"
	};

SHTCTL *compositor_init(struct MEMMAN  *memman, struct BOOTINFO *bootinfo);

window *create_window(int x0, int y0, int xsize, int ysize, char *title);