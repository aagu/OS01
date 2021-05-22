#include <bootinfo.h>

int kernel_main(struct KERNEL_BOOT_PARAMETER_INFORMATION *bootinfo)
{
    return bootinfo->BootFromBIOS;
}