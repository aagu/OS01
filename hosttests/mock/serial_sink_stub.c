/*
 * hosttests/mock/serial_sink_stub.c — the host-side console sink.
 *
 * The production console path (kernel/fs/devfs.c serial_write) emits a
 * WHOLE buffer under the console lock (serial_lock) through
 * write_serial_unlocked(), so that a console write reaches the wire
 * contiguously and another console writer cannot inject bytes in the
 * middle of a line or a protocol record.
 *
 * The host build has no UART, so the two symbols the production object
 * references must exist here.  The host spinlock is a no-op (the host
 * tests drive this path single-threaded), so the sink simply discards
 * the bytes — exactly like the write_serial() stub that each host test
 * TU already provides.
 */
#include <stddef.h>
#include <driver/serial.h>

spinlock_T serial_lock = { 1 };

void write_serial_unlocked(char c) { (void)c; }
