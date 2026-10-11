# user/monitor/app.mk — terminal monitor application (monitor plan Task 8).
#
# The four sources build one program; collect.c/model.c/render.c are the
# same production translations the host fixtures drive.  -I. puts user/ on
# the include path so the "<monitor/...>" public headers resolve from the
# user sub-build's working directory.  APP_LDLIBS carries the consumer=user
# builtins archive (Task 5) which supplies the 128-bit division the delta
# model needs; the directory_program template treats the .a as an ELF
# prerequisite, so an archive-only update relinks monitor.
APP_SOURCES := monitor.c collect.c model.c render.c
APP_CFLAGS := -I.
APP_LDLIBS := $(USER_RUNTIME_ARCHIVE)
