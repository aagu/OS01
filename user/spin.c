#include <unistd.h>

int main(void)
{
    return 42;  // exit immediately — no printf, no syscall except exit
}
