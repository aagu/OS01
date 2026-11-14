/* mock_alloc.c — kmalloc/kfree stubs backed by host malloc/free with fault injection. */
#include <stddef.h>
#include <stdlib.h>

extern int _mock_record_call(const char *symbol);

void *kmalloc(size_t size)
{
    if (_mock_record_call("kmalloc")) {
        return NULL;
    }
    return malloc(size);
}

size_t kfree(void *addr)
{
    (void)_mock_record_call("kfree");
    free(addr);
    return 1;
}
