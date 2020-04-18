/*
 *
 *       Filename:  string.h
 *
 *    Description:  字符串处理函数
 *
 */

#ifndef __STRING_H__
#define __STRING_H__

#include "stdlib.h"

static inline void bzero(void *dest, unsigned int len)
{
	memset(dest, 0, len);
}

/*
 * 描述：比较两个字符串
 * 如果相等：返回0
 * 如果不等：返回正数，则str1大
 *	     返回负数，则str2大
 */
static inline int strcmp(const char *str1, const char *str2)
{
    while (*str1 && *str2 && *str1 == *str2) {
        str1++;
        str2++;
    }

    return *str1 - *str2;
}

static inline char *strcpy(char *dest, const char *src)
{
	char *tmp = dest;

	while (*src) {
	      *dest++ = *src++;
	}

	*dest = '\0';

	return tmp;
}
/*
 * 描述：将src代表的字符串加到dest代表的字符串后面
 */
static inline char *strcat(char *dest, const char *src)
{
	char *cp = dest;

	while(*cp){
	     cp++;
	}

	while ((*cp++ = *src++))
	      ;

	return dest;
}

static inline int strlen(const char *src)
{
	const char *eos = src;

        while (*eos++);

	return (eos - src - 1);
}

static inline char *itoa(int number)
{
	char str[10];
	int i = 0;
	while(number > 10)
	{
		str[i] = number%10+'0';
		number = number/10;
		i++;
	}
	str[i] = number+'0';
	str[i+1] = '\0';
	return str;
}

//#define sizeof(char *type) ((type+1)-type)


#endif//STRING_H_

