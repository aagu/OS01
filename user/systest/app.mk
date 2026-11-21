APP_SOURCES := systest.c test_fs.c test_proc.c test_mem.c test_time_poll.c test_libc.c test_net.c test_rbtree.c test_libm.c
# The sixteen math symbols live in the standalone usr/lib/libm.a.
APP_LDLIBS := -lm
