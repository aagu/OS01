#include <time.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

/*
 * strptime: parse `buf` according to `fmt` into `tm`, returning a pointer
 * to the first unparsed character, or NULL on mismatch.  Supports the
 * directives used by coreutils sort -M ("%b") plus the common ones.
 */

static const char *const month_abbr[] = {
    "Jan","Feb","Mar","Apr","May","Jun",
    "Jul","Aug","Sep","Oct","Nov","Dec", NULL
};
static const char *const month_full[] = {
    "January","February","March","April","May","June",
    "July","August","September","October","November","December", NULL
};

static int match_name(const char **buf, const char *const names[])
{
    for (int i = 0; names[i]; i++) {
        size_t len = strlen(names[i]);
        if (strncasecmp(*buf, names[i], len) == 0) {
            *buf += len;
            return i;
        }
    }
    return -1;
}

/* Parse 1..2-digit number, or exactly `width` digits when width > 0. */
static int parse_num(const char **buf, int min, int max, int width)
{
    const char *p = *buf;
    int n = 0, nd = 0;
    if (width > 0) {
        for (; nd < width && isdigit((unsigned char)*p); nd++)
            n = n * 10 + (*p++ - '0');
        if (nd != width)
            return -1;
    } else {
        while (isdigit((unsigned char)*p)) {
            n = n * 10 + (*p++ - '0');
            nd++;
        }
        if (nd == 0 || n < min || n > max)
            return -1;
    }
    *buf = p;
    return n;
}

char *strptime(const char *buf, const char *fmt, struct tm *tm)
{
    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            switch (*fmt++) {
            case '%':
                if (*buf++ != '%')
                    return NULL;
                break;
            case 'b': case 'h': {
                int m = match_name(&buf, month_abbr);
                if (m < 0)
                    return NULL;
                tm->tm_mon = m;
                break;
            }
            case 'B': {
                int m = match_name(&buf, month_full);
                if (m < 0)
                    return NULL;
                tm->tm_mon = m;
                break;
            }
            case 'd': case 'e': {
                int d = parse_num(&buf, 1, 31, 0);
                if (d < 0)
                    return NULL;
                tm->tm_mday = d;
                break;
            }
            case 'm': {
                int m = parse_num(&buf, 1, 12, 0);
                if (m < 0)
                    return NULL;
                tm->tm_mon = m - 1;
                break;
            }
            case 'y': {
                int y = parse_num(&buf, 0, 99, 2);
                if (y < 0)
                    return NULL;
                tm->tm_year = (y >= 69) ? 1900 + y : 2000 + y;
                tm->tm_year -= 1900;
                break;
            }
            case 'Y': {
                int y = parse_num(&buf, 0, 9999, 4);
                if (y < 0)
                    return NULL;
                tm->tm_year = y - 1900;
                break;
            }
            case 'H': {
                int h = parse_num(&buf, 0, 23, 0);
                if (h < 0)
                    return NULL;
                tm->tm_hour = h;
                break;
            }
            case 'I': {
                int h = parse_num(&buf, 1, 12, 0);
                if (h < 0)
                    return NULL;
                tm->tm_hour = h % 12;
                break;
            }
            case 'M': {
                int m = parse_num(&buf, 0, 59, 0);
                if (m < 0)
                    return NULL;
                tm->tm_min = m;
                break;
            }
            case 'S': {
                int s = parse_num(&buf, 0, 61, 0);
                if (s < 0)
                    return NULL;
                tm->tm_sec = s;
                break;
            }
            case 'p': {
                if (strncasecmp(buf, "AM", 2) == 0) {
                    buf += 2;
                } else if (strncasecmp(buf, "PM", 2) == 0) {
                    buf += 2;
                    if (tm->tm_hour < 12)
                        tm->tm_hour += 12;
                } else {
                    return NULL;
                }
                break;
            }
            case 'T': {
                /* %T == %H:%M:%S */
                if (!(buf = strptime(buf, "%H:%M:%S", tm)))
                    return NULL;
                break;
            }
            case 'D': {
                /* %D == %m/%d/%y */
                if (!(buf = strptime(buf, "%m/%d/%y", tm)))
                    return NULL;
                break;
            }
            case 'F': {
                /* %F == %Y-%m-%d */
                if (!(buf = strptime(buf, "%Y-%m-%d", tm)))
                    return NULL;
                break;
            }
            default:
                return NULL;
            }
        } else if (isspace((unsigned char)*fmt)) {
            /* whitespace in format matches any run of whitespace */
            while (isspace((unsigned char)*fmt))
                fmt++;
            while (isspace((unsigned char)*buf))
                buf++;
        } else {
            if (*buf != *fmt)
                return NULL;
            buf++;
            fmt++;
        }
    }
    return (char *)buf;
}
