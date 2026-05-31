#include <stdio.h>

int main(int argc __attribute__((unused)),
         char **argv __attribute__((unused)))
{
    printf("Hello from user space!\n");
    return 42;
}
