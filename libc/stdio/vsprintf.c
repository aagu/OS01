#include <stdio.h>
#include <stdarg.h>
#include <string.h>

int skip_atoi(const char **s)
{
	int i=0;

	while (is_digit(**s))
		i = i*10 + *((*s)++) - '0';
	return i;
}

static char * number(char * str, char *end, long num, int base, int size, int precision,	int type)
{
	char c,sign,tmp[50];
	const char *digits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
	int i;

	if (type&SMALL) {digits = "0123456789abcdefghijklmnopqrstuvwxyz";}
	if (type&LEFT) {type &= ~ZEROPAD;}
	if (base < 2 || base > 36)
		return 0;
	c = (type & ZEROPAD) ? '0' : ' ' ;
	sign = 0;
	if (type&SIGN && num < 0) {
		sign='-';
		num = -num;
	} else
		sign=(type & PLUS) ? '+' : ((type & SPACE) ? ' ' : 0);
	if (sign) size--;
	if (type & SPECIAL)
    {
		if (base == 16) size -= 2;
		else if (base == 8) size--;
    }
	i = 0;
	if (num == 0)
		tmp[i++]='0';
	else while (num!=0)
		tmp[i++]=digits[do_div(num,base)];
	if (i > precision) precision=i;
	size -= precision;
	if (!(type & (ZEROPAD + LEFT)))
		while(size-- > 0)
			if(str < end)
				*str++ = ' ';
	if (sign)
		if(str < end)
        	*str++ = sign;
	if (type & SPECIAL)
    {
		if (base == 8)
		{
			if(str < end)
				*str++ = '0';
		}
		else if (base==16) 
		{
			if(str < end)
			{
				*str++ = '0';
				*str++ = digits[33];
			}
		}
    }
	if (!(type & LEFT))
		while(size-- > 0)
			if(str < end)
				*str++ = c;

	while(i < precision--)
		if(str < end)
			*str++ = '0';
	while(i-- > 0)
		if(str < end)
			*str++ = tmp[i];
	while(size-- > 0)
		if(str < end)
			*str++ = ' ';
	return str;
}

int vsprintf(char * buf,const char *fmt, va_list args)
{
	char * str,*s,*end;
	int flags;
	int field_width;
	int precision;
	int len,i;

	int qualifier;		/* 'h', 'l', 'L' or 'Z' for integer fields */
	end = buf + 4096;

	for(str = buf; *fmt; fmt++)
	{

		if(*fmt != '%')
		{
			if(str < end)
				*str++ = *fmt;
			continue;
		}
		flags = 0;
		repeat:
			fmt++;
			switch(*fmt)
			{
				case '-':flags |= LEFT;	
				goto repeat;
				case '+':flags |= PLUS;	
				goto repeat;
				case ' ':flags |= SPACE;	
				goto repeat;
				case '#':flags |= SPECIAL;	
				goto repeat;
				case '0':flags |= ZEROPAD;	
				goto repeat;
			}

			/* get field width */

			field_width = -1;
			if(is_digit(*fmt))
				field_width = skip_atoi(&fmt);
			else if(*fmt == '*')
			{
				fmt++;
				field_width = va_arg(args, int);
				if(field_width < 0)
				{
					field_width = -field_width;
					flags |= LEFT;
				}
			}
			
			/* get the precision */

			precision = -1;
			if(*fmt == '.')
			{
				fmt++;
				if(is_digit(*fmt))
					precision = skip_atoi(&fmt);
				else if(*fmt == '*')
				{	
					fmt++;
					precision = va_arg(args, int);
				}
				if(precision < 0)
					precision = 0;
			}
			
			qualifier = -1;
			if(*fmt == 'h' || *fmt == 'l' || *fmt == 'L' || *fmt == 'Z')
			{	
				qualifier = *fmt;
				fmt++;
			}
							
			switch(*fmt)
			{
				case 'c':

					if(!(flags & LEFT))
						while(--field_width > 0)
							if(str < end)
								*str++ = ' ';
					if(str < end)
						*str++ = (unsigned char)va_arg(args, int);
					while(--field_width > 0)
						if(str < end)
							*str++ = ' ';
					break;

				case 's':
				
					s = va_arg(args,char *);
					if(!s)
						s = '\0';
					len = strlen(s);
					if(precision < 0)
						precision = len;
					else if(len > precision)
						len = precision;
					
					if(!(flags & LEFT))
						while(len < field_width--)
							if(str < end)
								*str++ = ' ';
					for(i = 0;i < len ;i++)
						if(str < end)
							*str++ = *s++;
					while(len < field_width--)
						if(str < end)
							*str++ = ' ';
					break;

				case 'o':
					
					if(qualifier == 'l')
						str = number(str,end,va_arg(args,unsigned long),8,field_width,precision,flags);
					else
						str = number(str,end,va_arg(args,unsigned int),8,field_width,precision,flags);
					break;

				case 'p':

					if(field_width == -1)
					{
						field_width = 2 * sizeof(void *);
						flags |= ZEROPAD;
					}

					str = number(str,end,(unsigned long)va_arg(args,void *),16,field_width,precision,flags);
					break;

				case 'x':

					flags |= SMALL;

				case 'X':

					if(qualifier == 'l')
						str = number(str,end,va_arg(args,unsigned long),16,field_width,precision,flags);
					else
						str = number(str,end,va_arg(args,unsigned int),16,field_width,precision,flags);
					break;

				case 'd':
				case 'i':

					flags |= SIGN;

				case 'u':

					if(qualifier == 'l')
						str = number(str,end,va_arg(args,long),10,field_width,precision,flags);
					else
						str = number(str,end,va_arg(args,int),10,field_width,precision,flags);
					break;

				case 'n':
					
					if(qualifier == 'l')
					{
						long *ip = va_arg(args,long *);
						*ip = (str - buf);
					}
					else
					{
						int *ip = va_arg(args,int *);
						*ip = (str - buf);
					}
					break;

				case '%':
					if (str < end)
						*str++ = '%';
					break;

				default:
					if (str < end)
						*str++ = '%';
					if(*fmt)
						if (str < end)
							*str++ = *fmt;
					else
						fmt--;
					break;
			}

	}
	if (str < end)
		*str = '\0';
	else
		*(end-1) = '\0';
	return str - buf;
}