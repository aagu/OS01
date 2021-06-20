HOSTARCH:=$(shell ../target-triplet-to-arch.sh $(shell ../default-host.sh))

ARCHDIR=arch/$(HOSTARCH)

include $(ARCHDIR)/make.config

CFLAGS := $(CFLAGS) -mcmodel=large -ffreestanding -mno-red-zone -Wall -Wextra -D__is_libc -Iinclude
LDFLAGS := $(LDFLAGS) -mcmodel=large -ffreestanding -O2 -nostdlib
ASFLAGS :=
# ifeq ($(DEBUG),1)
# CFLAGS += -g
# endif
LIBK_CFLAGS := $(CFLAGS) -D__is_libk

DESTDIR=$(SYSROOT)

FREEOBJS=\
$(ARCH_FREEOBJS) \
string/memcmp.o \
string/memcpy.o \
string/memmove.o \
string/memset.o \
string/strlen.o \
string/strcpy.o \
stdio/printf.o \
stdio/putchar.o \
stdio/puts.o \
stdio/vsprintf.o \
list/list.o \
stdlib/abort.o \
stdlib/calloc.o \
stdlib/malloc.o \
stdlib/free.o

HOSTEDOBJS=\
$(ARCH_HOSTEDOBJS) \

OBJS=\
$(FREEOBJS) \
$(HOSTEDOBJS) \

LIBK_OBJS=$(FREEOBJS:.o=.libk.o)

BINARIES=libk.a

.PHONY: all clean install install-headers install-libs
.SUFFIXES: .o .libk.o .c .S

all: $(BINARIES)

libc.a: $(OBJS)
	$(AR) rcs $@ $(OBJS)

libk.a: $(LIBK_OBJS)
	$(AR) rcs $@ $(LIBK_OBJS)

.c.o:
	$(CC) $(CFLAGS) -c $< -o $@

.c.libk.o:
	$(CC) $(LIBK_CFLAGS) -c $< -o $@

clean:
	rm -f $(BINARIES) *.a
	rm -f $(OBJS) $(LIBK_OBJS) *.o */*.o */*/*.o
	rm -f $(OBJS:.o=.d) $(LIBK_OBJS:.o=.d) *.d */*.d */*/*.d

install: install-headers install-libs

install-headers:
	mkdir -p $(DESTDIR)$(INCLUDEDIR)
	cp -R --preserve=timestamps include/. $(DESTDIR)$(INCLUDEDIR)/.

install-libs: $(BINARIES)
	mkdir -p $(DESTDIR)$(LIBDIR)
	cp $(BINARIES) $(DESTDIR)$(LIBDIR)

-include $(OBJS:.o=.d)
-include $(LIBK_OBJS:.o=.d)