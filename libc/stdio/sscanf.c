#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

// Minimal sscanf — handles %d, %s, %c, %x, %u
// Returns number of input items successfully matched.

int sscanf(const char *str, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int count = 0;
    const char *s = str;

    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            int suppress = 0;
            if (*fmt == '*') { suppress = 1; fmt++; }

            switch (*fmt) {
            case 'd': {
                char *end;
                long val = strtol(s, &end, 10);
                if (end == s) goto done; // no match
                if (!suppress) {
                    int *ip = va_arg(args, int *);
                    *ip = (int)val;
                    count++;
                }
                s = end;
                break;
            }
            case 'u': {
                char *end;
                unsigned long val = strtoul(s, &end, 10);
                if (end == s) goto done;
                if (!suppress) {
                    unsigned int *up = va_arg(args, unsigned int *);
                    *up = (unsigned int)val;
                    count++;
                }
                s = end;
                break;
            }
            case 'x':
            case 'X': {
                char *end;
                unsigned long val = strtoul(s, &end, 16);
                if (end == s) goto done;
                if (!suppress) {
                    unsigned int *up = va_arg(args, unsigned int *);
                    *up = (unsigned int)val;
                    count++;
                }
                s = end;
                break;
            }
            case 's': {
                // Skip leading whitespace
                while (isspace(*s)) s++;
                if (*s == '\0') goto done;
                if (!suppress) {
                    char *sp = va_arg(args, char *);
                    while (*s && !isspace(*s))
                        *sp++ = *s++;
                    *sp = '\0';
                    count++;
                } else {
                    while (*s && !isspace(*s)) s++;
                }
                break;
            }
            case 'c': {
                if (*s == '\0') goto done;
                if (!suppress) {
                    char *cp = va_arg(args, char *);
                    *cp = *s;
                    count++;
                }
                s++;
                break;
            }
            case '%':
                if (*s == '%') s++;
                else goto done;
                break;
            default:
                // Unknown format — skip
                break;
            }
        } else if (isspace(*fmt)) {
            // Skip whitespace in both fmt and input
            while (isspace(*fmt)) fmt++;
            while (isspace(*s)) s++;
            continue;  // skip the fmt++ at end of loop
        } else {
            if (*fmt != *s) goto done;
            s++;
        }
        fmt++;
    }

done:
    va_end(args);
    return count;
}
