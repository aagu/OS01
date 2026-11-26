#ifndef _UAPI_UIO_H
#define _UAPI_UIO_H

#include <stddef.h>

struct iovec {
    void  *iov_base;
    size_t iov_len;
};

#define UIO_MAXIOV 1024

#endif /* _UAPI_UIO_H */
