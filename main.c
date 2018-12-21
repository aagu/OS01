void io_hlt();

void main()
{
  int i;
    char*p = 0;

    for (i = 0xa0000; i <= 0xaffff; i++) {
        p = i;
        *p = i & 0x0f;  
    }

    for(;;) {
      io_hlt();
    }
}

void io_hlt() {
  asm volatile("hlt");
}