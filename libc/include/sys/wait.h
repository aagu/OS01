#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H
#define WEXITSTATUS(s) (((s) & 0xff00) >> 8)
#define WTERMSIG(s) ((s) & 0x7f)
#define WIFEXITED(s) (WTERMSIG(s) == 0)
#define WIFSIGNALED(s) (!WIFEXITED(s) && !WIFSTOPPED(s))
#define WIFSTOPPED(s) (((s) & 0xff) == 0x7f)
#define WCOREDUMP(s) ((s) & 0x80)
#endif

/* Wait macros */
#define WUNTRACED 4

#define WIFEXITED(s)  (((s) & 0xff) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WIFSIGNALED(s) (((s) & 0xff) != 0 && ((s) & 0xff) != 0x7f)
#define WTERMSIG(s)    ((s) & 0x7f)
#define WIFSTOPPED(s)  (((s) & 0xff) == 0x7f)
#define WSTOPSIG(s)    (((s) >> 8) & 0xff)
#define WIFCONTINUED(s) (0)
