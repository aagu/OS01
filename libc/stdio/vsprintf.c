#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include "stdio_internal.h"

int skip_atoi(const char **s)
{
	int i=0;

	while (is_digit(**s))
		i = i*10 + *((*s)++) - '0';
	return i;
}

typedef struct {
	char *dst;
	size_t cap;
	size_t pos;
	size_t total;
} vf_state_t;

/* Checked output primitive: write only when there is room for this char AND
 * the trailing NUL, saturate total at SIZE_MAX, and never write in pure-count
 * mode (cap == 0). */
static void vf_out(vf_state_t *st, char ch)
{
	if (st->total == SIZE_MAX)
		return;
	if (st->cap > 0 && st->pos < st->cap - 1)
		st->dst[st->pos++] = ch;
	++st->total;
}

/* Digit generation for an unsigned magnitude, routed entirely through vf_out.
 * `sign` is the optional leading char ('-','+',' ' or 0). */
static void vf_number(vf_state_t *st, char sign, unsigned long long num, int base,
                      int size, int precision, int type)
{
	char c, tmp[66];
	const char *digits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
	int i;

	if (type & SMALL) { digits = "0123456789abcdefghijklmnopqrstuvwxyz"; }
	if (type & LEFT) { type &= ~ZEROPAD; }
	if (base < 2 || base > 36)
		return;
	c = (type & ZEROPAD) ? '0' : ' ';
	if (sign) size--;
	if (type & SPECIAL) {
		if (base == 16) size -= 2;
		else if (base == 8) size--;
	}
	i = 0;
	if (num == 0)
		tmp[i++] = '0';
	else while (num != 0)
		tmp[i++] = digits[do_div(num, base)];
	if (i > precision) precision = i;
	size -= precision;
	if (!(type & (ZEROPAD + LEFT)))
		while (size-- > 0)
			vf_out(st, ' ');
	if (sign)
		vf_out(st, sign);
	if (type & SPECIAL) {
		if (base == 8) {
			vf_out(st, '0');
		} else if (base == 16) {
			vf_out(st, '0');
			vf_out(st, digits[33]);
		}
	}
	if (!(type & LEFT))
		while (size-- > 0)
			vf_out(st, c);
	while (i < precision--)
		vf_out(st, '0');
	while (i-- > 0)
		vf_out(st, tmp[i]);
	while (size-- > 0)
		vf_out(st, ' ');
}

size_t vformatter(char *dst, size_t cap, const char *fmt, va_list ap, int perform_assign)
{
	vf_state_t st;
	st.dst = dst;
	st.cap = cap;
	st.pos = 0;
	st.total = 0;

	int flags;
	int field_width;
	int precision;
	int i;

	int qualifier;          /* 'h','l','L','Z' or -1 */
	int is_ll;              /* 'll' (two l) or 'L' -> long long */

	for (; *fmt; fmt++) {
		if (*fmt != '%') {
			vf_out(&st, *fmt);
			continue;
		}

		flags = 0;
	repeat:
		fmt++;
		switch (*fmt) {
			case '-': flags |= LEFT;   goto repeat;
			case '+': flags |= PLUS;   goto repeat;
			case ' ': flags |= SPACE;  goto repeat;
			case '#': flags |= SPECIAL; goto repeat;
			case '0': flags |= ZEROPAD; goto repeat;
		}

		/* field width */
		field_width = -1;
		if (is_digit(*fmt))
			field_width = skip_atoi(&fmt);
		else if (*fmt == '*') {
			fmt++;
			field_width = va_arg(ap, int);
			if (field_width < 0) {
				field_width = -field_width;
				flags |= LEFT;
			}
		}

		/* precision */
		precision = -1;
		if (*fmt == '.') {
			fmt++;
			if (is_digit(*fmt))
				precision = skip_atoi(&fmt);
			else if (*fmt == '*') {
				fmt++;
				precision = va_arg(ap, int);
			}
			if (precision < 0)
				precision = 0;
		}

		/* qualifier */
		qualifier = -1;
		is_ll = 0;
		if (*fmt == 'h' || *fmt == 'l' || *fmt == 'L' || *fmt == 'Z') {
			qualifier = *fmt;
			fmt++;
			if (qualifier == 'l' && *fmt == 'l') {
				is_ll = 1;
				fmt++;
			}
		}

		switch (*fmt) {
			case 'c': {
				if (!(flags & LEFT))
					while (--field_width > 0)
						vf_out(&st, ' ');
				vf_out(&st, (unsigned char)va_arg(ap, int));
				while (--field_width > 0)
					vf_out(&st, ' ');
				break;
			}

			case 's': {
				const char *s = va_arg(ap, char *);
				if (!s)
					s = "";
				int len = (int)strlen(s);
				if (precision < 0)
					precision = len;
				else if (len > precision)
					len = precision;
				if (!(flags & LEFT))
					while (len < field_width--)
						vf_out(&st, ' ');
				for (i = 0; i < len; i++)
					vf_out(&st, *s++);
				while (len < field_width--)
					vf_out(&st, ' ');
				break;
			}

			case 'o':
				flags &= ~SMALL;
				goto do_unsigned;

			case 'p': {
				if (field_width == -1) {
					field_width = 2 * (int)sizeof(void *);
					flags |= ZEROPAD;
				}
				vf_number(&st, 0,
				          (unsigned long long)(unsigned long)va_arg(ap, void *),
				          16, field_width, precision, flags);
				break;
			}

			case 'x':
				flags |= SMALL;
				/* fall through */
			case 'X':
			do_unsigned:
				{
					unsigned long long uval;
					if (is_ll || qualifier == 'L' || qualifier == 'Z')
						uval = va_arg(ap, unsigned long long);
					else if (qualifier == 'l')
						uval = va_arg(ap, unsigned long);
					else
						uval = va_arg(ap, unsigned int);
					vf_number(&st, 0, uval,
					          (*fmt == 'o') ? 8 : 16,
					          field_width, precision, flags);
				}
				break;

			case 'd':
			case 'i':
				flags |= SIGN;
				/* fall through */
			case 'u':
				{
					int is_signed = (flags & SIGN);
					char sign = 0;
					unsigned long long mag;
					if (is_signed) {
						long long sval;
						if (is_ll || qualifier == 'L' || qualifier == 'Z')
							sval = va_arg(ap, long long);
						else if (qualifier == 'l')
							sval = va_arg(ap, long);
						else
							sval = va_arg(ap, int);
						if (sval < 0) {
							sign = '-';
							mag = 0 - (unsigned long long)sval;
						} else {
							sign = (flags & PLUS) ? '+' : ((flags & SPACE) ? ' ' : 0);
							mag = (unsigned long long)sval;
						}
					} else {
						if (is_ll || qualifier == 'L' || qualifier == 'Z')
							mag = va_arg(ap, unsigned long long);
						else if (qualifier == 'l')
							mag = va_arg(ap, unsigned long);
						else
							mag = va_arg(ap, unsigned int);
					}
					vf_number(&st, sign, mag, 10, field_width, precision, flags);
				}
				break;

			case 'n': {
				if (is_ll || qualifier == 'L' || qualifier == 'Z') {
					long long *ip = va_arg(ap, long long *);
					if (perform_assign)
						*ip = (long long)st.total;
				} else if (qualifier == 'l') {
					long *ip = va_arg(ap, long *);
					if (perform_assign)
						*ip = (long)st.total;
				} else {
					int *ip = va_arg(ap, int *);
					if (perform_assign)
						*ip = (int)st.total;
				}
				break;
			}

			case 'f': case 'F':
			case 'e': case 'E':
			case 'g': case 'G':
			case 'a': case 'A': {
				/* Float rendering is implemented in a later task. For now
				 * consume the double (keeping va_list alignment) and emit the
				 * literal conversion text as a stub. %L* does NOT consume. */
				char conv = *fmt;
				if (qualifier != 'L')
					(void)va_arg(ap, double);
				vf_out(&st, '%');
				if (qualifier == 'L')
					vf_out(&st, 'L');
				vf_out(&st, conv);
				break;
			}

			case '%':
				vf_out(&st, '%');
				break;

			default:
				vf_out(&st, '%');
				if (*fmt)
					vf_out(&st, *fmt);
				else
					fmt--;
				break;
		}
	}

	if (cap > 0)
		st.dst[st.pos] = '\0';

	if (st.total == SIZE_MAX)
		return SIZE_MAX;
	return st.total;
}

int vsprintf(char * buf, const char *fmt, va_list args)
{
	if (!buf)
		return -1;
	size_t r = vformatter(buf, 4096, fmt, args, 1);
	if (r == SIZE_MAX || r > INT_MAX)
		return -1;
	return (int)r;
}
