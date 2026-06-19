#ifndef _KERNEL_PIC_H
#define _KERNEL_PIC_H

#include <stdint.h>

#define MASTER_ICW1 0x20
#define MASTER_ICW2 0x21
#define MASTER_ICW3 0x21
#define MASTER_ICW4 0x21
#define MASTER_OCW1 0x21
#define MASTER_OCW2 0x20
#define MASTER_OCW3 0x20

#define SLAVE_ICW1 0xa0
#define SLAVE_ICW2 0xa1
#define SLAVE_ICW3 0xa1
#define SLAVE_ICW4 0xa1
#define SLAVE_OCW1 0xa1
#define SLAVE_OCW2 0xa0
#define SLAVE_OCW3 0xa0

void pic_init();

void pic_enable(uint64_t nr);
void pic_disable(uint64_t nr);
uint64_t pic_install(uint64_t nr, void * data);
void pic_uninstall(uint64_t nr);
void pic_ack(uint64_t nr);

#endif