/*
 * date — print the current date and time.
 *
 * The built-in consumer of the libc's time API: the wall clock comes
 * from the kernel, the broken-down time from localtime()/gmtime() and
 * the text from strftime().  The zone is the POSIX TZ of the
 * environment, which is what the libc implements.
 *
 *   date            the local time
 *   date -u         the UTC time
 *   date +FORMAT    the local time in a strftime format
 *   date -u +FORMAT the UTC time in a strftime format
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

/* OS01's kernel has no RTC yet: time() answers the epoch, while
 * clock_gettime(CLOCK_REALTIME) reports the time since boot.  The
 * applet uses the latter so that it prints a clock that moves. */
static time_t wall_clock(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        return (time_t)ts.tv_sec;
    }
    return time(NULL);
}

int main(int argc, char **argv)
{
    bool utc = false;
    const char *format = NULL;
    time_t now;
    struct tm tm;
    struct tm *broken;
    char text[256];

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-u") == 0) {
            utc = true;
        } else if (argv[i][0] == '+') {
            format = argv[i] + 1;
        } else {
            fprintf(stderr, "date: unknown option '%s'\n", argv[i]);
            return 1;
        }
    }

    if (format == NULL) {
        format = "%a %b %e %H:%M:%S %Z %Y";
    }

    now = wall_clock();
    broken = utc ? gmtime_r(&now, &tm) : localtime_r(&now, &tm);
    if (broken == NULL) {
        fprintf(stderr, "date: the clock is not available\n");
        return 1;
    }

    if (strftime(text, sizeof(text), format, broken) == 0) {
        fprintf(stderr, "date: the format does not fit\n");
        return 1;
    }

    printf("%s\n", text);
    return 0;
}
