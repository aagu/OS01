extern void io_cli_hlt();

void main()
{
  int i;
    char*p = 0;

    for (i = 0xa0000; i <= 0xaffff; i++) {
        p = i;
        *p = i & 0x0f;  
    }

    io_cli_hlt();
}