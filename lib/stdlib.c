#include "stdlib.h"

void *memset8(void *dst, unsigned char value, unsigned int len);
void *memset16(void *dst, unsigned short value, unsigned int len);
void *memset32(void *dst, unsigned int value, unsigned int len);

void *memcpy(void* src, void* dst, unsigned int nbytes) {
    if (NULL == dst || NULL == src) return NULL;

	void *ret = dst;

	if (dst <= src || (char *)dst >= (char *)src + nbytes) {
		while (nbytes--)
		{
			*(char *)dst = *(char *)src;
			dst = (char *)dst + 1;
			src = (char *)src + 1;
		}
		
	} else {
		src = (char *)src + nbytes - 1;
		dst = (char *)dst + nbytes - 1;
		while (nbytes--) {
			*(char *)dst = *(char *)src;
			dst = (char *)dst - 1;
			src = (char *)src - 1;
		}
	}

    return ret;
}

void *memset(void *s, unsigned char value, unsigned int len) {
	unsigned char* su;
	for (su = s; 0 < len; ++su, --len) {
		*su = value;
	}
}