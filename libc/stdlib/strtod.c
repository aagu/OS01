#include <stdlib.h>
#include <ctype.h>

double strtod(const char *nptr, char **endptr)
{
    double result = 0.0;
    double fraction = 0.0;
    double divisor = 1.0;
    int sign = 1;
    int exp_sign = 1;
    int exponent = 0;
    int has_digits = 0;

    if (nptr == NULL)
        goto done;

    /* Skip leading whitespace */
    while (isspace((unsigned char)*nptr))
        nptr++;

    /* Optional sign */
    if (*nptr == '-') {
        sign = -1;
        nptr++;
    } else if (*nptr == '+') {
        nptr++;
    }

    /* Parse integer part */
    while (*nptr >= '0' && *nptr <= '9') {
        result = result * 10.0 + (*nptr - '0');
        has_digits = 1;
        nptr++;
    }

    /* Parse fractional part */
    if (*nptr == '.') {
        nptr++;
        while (*nptr >= '0' && *nptr <= '9') {
            fraction = fraction * 10.0 + (*nptr - '0');
            divisor *= 10.0;
            has_digits = 1;
            nptr++;
        }
        result += fraction / divisor;
    }

    if (!has_digits) {
        result = 0.0;
        goto done;
    }

    result *= sign;

    /* Parse exponent */
    if (*nptr == 'e' || *nptr == 'E') {
        nptr++;
        if (*nptr == '-') {
            exp_sign = -1;
            nptr++;
        } else if (*nptr == '+') {
            nptr++;
        }
        while (*nptr >= '0' && *nptr <= '9') {
            exponent = exponent * 10 + (*nptr - '0');
            nptr++;
        }
        if (exp_sign == 1) {
            while (exponent-- > 0)
                result *= 10.0;
        } else {
            while (exponent-- > 0)
                result /= 10.0;
        }
    }

done:
    if (endptr)
        *endptr = (char *)nptr;
    return result;
}
