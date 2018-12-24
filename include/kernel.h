/*汇编io调用*/
#ifndef IO_H
#define IO_H
void io_hlt(void);
void io_cli(void);
void io_sti(void);
void io_out8(int port, int data);
int io_in8(int port);
int io_load_eflags(void);
void io_store_eflags(int eflags);
load_gdtr(unsigned int *);
load_idtr(unsigned int *);
#endif //KERNEL_H