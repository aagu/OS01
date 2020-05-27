#ifndef	__MEMORY_H__
#define	__MEMORY_H__

#define MEMMAN_FREES 4096
#define EFLAGS_AC_BIT			0x00040000
#define CR0_CACHE_DISABLE	0x60000000
#define MEMBLOCK_MAGIC 0xf0fe

typedef struct FREEINFO
{
	unsigned int addr, size;
} FREEINFO;

typedef struct MEMMAN
{
	int frees, maxfrees, lostsize, losts;
	struct FREEINFO free[MEMMAN_FREES];
} MEMMAN;

typedef struct MEMBLOCK
{
	unsigned int magic;
	unsigned int size;
} MEMBLOCK;

unsigned int memtest(unsigned int start, unsigned int end);
void memman_init(unsigned int start, unsigned int size);
unsigned int memman_total();
unsigned int memman_alloc(unsigned int size);
unsigned int memman_alloc_4k(unsigned int size);
int memman_free(unsigned int addr);
int memman_free_4k(unsigned int addr);

#endif
