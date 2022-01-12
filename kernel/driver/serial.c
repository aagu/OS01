#include <driver/serial.h>
#include <hw.h>

void init_serial() {
    outb(SERIAL_COM1 + 1, 0x00);    // Disable all interrupts
    outb(SERIAL_COM1 + 3, 0x80);    // Enable DLAB (set baud rate divisor)
    outb(SERIAL_COM1 + 0, 0x03);    // Set divisor to 3 (lo byte) 38400 baud
    outb(SERIAL_COM1 + 1, 0x00);    //                  (hi byte)
    outb(SERIAL_COM1 + 3, 0x03);    // 8 bits, no parity, one stop bit
    outb(SERIAL_COM1 + 2, 0xC7);    // Enable FIFO, clear them, with 14-byte threshold
    outb(SERIAL_COM1 + 4, 0x0B);    // IRQs enabled, RTS/DSR set
}

// receiving data
bool serial_received() {
    return inb(SERIAL_COM1 + 5) & 1;
}

char read_serial() {
    while (serial_received() == 0);
    
    return inb(SERIAL_COM1);
}

// sending data
bool is_transmit_empty() {
    return inb(SERIAL_COM1+5) & 0x20;
}

void write_serial(char c) {
    while (is_transmit_empty() == 0);

    outb(SERIAL_COM1, c);
}