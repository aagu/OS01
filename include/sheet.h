#ifndef __SHEET_H__
#define __SHEET_H__

#include "mm.h"

/*图层处理*/
#define MAX_SHEETS		256
#define SHEET_USE  1

typedef struct SHEET {
	unsigned char *buf;
	int bxsize, bysize, vx0, vy0, col_inv, height, flags;
} SHEET;

typedef struct SHTCTL {
	unsigned char *vram;
	int xsize, ysize, top;
	struct SHEET *sheets[MAX_SHEETS];
	struct SHEET sheets0[MAX_SHEETS];
} SHTCTL;

//SHTCTL *shtctl_init(MEMMAN *memman, unsigned char *vram, int xsize, int ysize);
SHTCTL *shtctl_init(unsigned char *vram, int xsize, int ysize);
SHEET *sheet_alloc(SHTCTL *ctl);
void sheet_setbuf(SHEET *sht, unsigned char *buf, int xsize, int ysize, int col_inv);
void sheet_updown(SHTCTL *ctl, SHEET *sht, int height);
void sheet_refresh(SHTCTL *ctl, SHEET *sht, int bx0, int by0, int bx1, int by1);
void sheet_slide(SHTCTL *ctl, SHEET *sht, int vx0, int vy0);
void sheet_free(SHTCTL *ctl, SHEET *sht);

#endif