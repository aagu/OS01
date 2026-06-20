HOSTARCH:=$(shell ../target-triplet-to-arch.sh $(shell ../default-host.sh))

ARCHDIR=arch/$(HOSTARCH)

include $(ARCHDIR)/make.config

CFLAGS := $(CFLAGS) -mcmodel=large -ffreestanding -mno-red-zone -Wall -Wextra -D__is_libc -Iinclude -fno-stack-protector
LDFLAGS := $(LDFLAGS) -mcmodel=large -ffreestanding -nostdlib
ASFLAGS :=
ifeq ($(DEBUG),1)
CFLAGS += -DDEBUG=1
endif
LIBK_CFLAGS := $(CFLAGS) -D__is_libk

DESTDIR=$(SYSROOT)

FREEOBJS=\
$(ARCH_FREEOBJS) \
string/memcmp.o \
string/bcmp.o \
string/memcpy.o \
string/memmove.o \
string/memset.o \
string/strlen.o \
string/strcmp.o \
string/strncmp.o \
string/strcpy.o \
string/strdup.o \
string/extras.o \
	string/strchrnul.o \
	string/mempcpy.o \
	string/stpncpy.o \
	string/stpcpy.o \
	string/strsignal.o \
string/strerror.o \
	string/fnmatch.o \
stdio/printf.o \
stdio/putchar.o \
stdio/puts.o \
stdio/vsprintf.o \
stdio/sprintf.o \
stdio/sscanf.o \
	time/time.o \
time/time_r.o \
stdio/getchar_unlocked.o \
	stdio/stdio_file.o \
	stdio/vsnprintf.o \
stdio/vasprintf.o \
stdio/getchar_unlocked.o \
	stdio/stdio_file.o \
	stdio/vsnprintf.o \
stdio/vasprintf.o \
list/list.o \
stdlib/abort.o \
stdlib/calloc.o \
stdlib/malloc.o \
stdlib/free.o \
stdlib/environ.o \
stdlib/strtol.o \
	stdlib/strtoll.o \
stdlib/qsort.o \
stdlib/atexit.o \
ctype/ctype.o \
errno/errno.o \
unistd/read.o \
unistd/write.o \
unistd/open.o \
unistd/close.o \
unistd/dup.o \
unistd/dup2.o \
unistd/waitpid.o \
unistd/pipe.o \
unistd/chdir.o \
unistd/getcwd.o \
unistd/stat.o \
unistd/fstat.o \
unistd/lseek.o \
unistd/fcntl.o \
unistd/ioctl.o \
unistd/getdents.o \
unistd/access.o \
unistd/execve.o \
unistd/link.o \
unistd/symlink.o \
unistd/readlink.o \
unistd/unlink.o \
unistd/mkdir.o \
unistd/rmdir.o \
unistd/rename.o \
unistd/truncate.o \
unistd/ftruncate.o \
unistd/dirent.o \
unistd/time.o \
unistd/gettimeofday.o \
unistd/nanosleep.o \
unistd/chmod.o \
unistd/fchmod.o \
unistd/times.o \
unistd/uname.o \
unistd/getppid.o \
unistd/umask.o \
unistd/kill.o \
unistd/sigaction.o \
unistd/signal.o \
	unistd/raise.o \
	unistd/geteuid.o \
	unistd/uid_stubs.o \
	unistd/sigfillset.o \
	unistd/sigsuspend.o \
	unistd/lstat.o \
	unistd/mknod.o \
	unistd/chown.o \
	unistd/utimes.o \
	unistd/term_stubs.o \
	unistd/sleep.o \
	unistd/getopt.o \
unistd/getgroups.o \
unistd/sysconf.o \
stdlib/random.o \
unistd/busybox_stubs.o \
stdlib/glob.o \
unistd/sigprocmask.o \
stdlib/setjmp.o \
unistd/isatty.o \
unistd/getpid.o \
unistd/exit.o \
stdio/stdio_extras.o \
unistd/misc_stubs.o \
unistd/mkstemp.o \
unistd/prctl.o \
unistd/term_stubs.o \
	unistd/sleep.o \
	unistd/getopt.o \
unistd/getgroups.o \
unistd/sysconf.o \
stdlib/random.o \
unistd/busybox_stubs.o \
stdlib/glob.o \
unistd/sigprocmask.o \
stdlib/setjmp.o \
unistd/isatty.o \
unistd/getpid.o \
unistd/exit.o \
stdio/stdio_extras.o \
unistd/misc_stubs.o \
unistd/mkstemp.o \
unistd/prctl.o \
unistd/term_stubs.o \
	unistd/sleep.o \
	unistd/getopt.o \
unistd/getgroups.o \
unistd/sysconf.o \
stdlib/random.o \
unistd/busybox_stubs.o \
stdlib/glob.o \
unistd/sigprocmask.o \
stdlib/setjmp.o \
unistd/isatty.o \
unistd/getpid.o \
unistd/exit.o \
stdio/stdio_extras.o \
unistd/misc_stubs.o \
unistd/mkstemp.o \
unistd/prctl.o \

HOSTEDOBJS=\
$(ARCH_HOSTEDOBJS) \

OBJS=\
$(FREEOBJS) \
$(HOSTEDOBJS) \

LIBK_OBJS=$(FREEOBJS:.o=.libk.o)

BINARIES=libc.a libk.a

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
