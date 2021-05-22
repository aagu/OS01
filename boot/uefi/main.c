#include <uefi.h>

int main(int argc, char **argv)
{
    DIR *dirptr = NULL;
    struct dirent *entry;
    if ((dirptr = opendir(".")) == NULL)
    {
        printf("open current dir failed!\n");
        return EFI_ERROR(1);
    }
    while (entry = readdir(dirptr))
    {
        printf("filename %s, filetype %d\n", entry->d_name, entry->d_type);
    }
    closedir(dirptr);
    return 0;
}