// OS01 Init — PID 1 user-space process
//
// Implements a BusyBox-style four-phase boot sequence:
//   1. SYSINIT (blocking)  — one-time system initialization
//   2. WAIT    (blocking)  — one-time tasks that must complete
//   3. ONCE    (fire-and-forget) — one-time tasks, no waiting
//   4. RESPAWN / ASKFIRST   — supervised long-running services
//
// Uses hardcoded fallback actions when /etc/inittab is absent.
// Child reaping via waitpid(WNOHANG) polling — no signal delivery required.

#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <fcntl.h>

// ── Action types ────────────────────────────────────────────
// MUST be powers of 2: run_actions() uses bitmask matching.
#define ACT_SYSINIT     0x01
#define ACT_WAIT        0x02
#define ACT_ONCE        0x04
#define ACT_RESPAWN     0x08
#define ACT_ASKFIRST    0x10
#define ACT_CTRLALTDEL  0x20
#define ACT_SHUTDOWN    0x40
#define ACT_RESTART     0x80

// ── Child process tracking ──────────────────────────────────
#define MAX_PROCS 16

struct child {
    int pid;
    int action;
    char command[128];
    int restart_count;
};

static struct child children[MAX_PROCS];
static int child_count = 0;

// ── Inittab action entry ────────────────────────────────────
#define MAX_ACTIONS 16

struct init_action {
    int action;
    char tty[16];
    char process[128];
};

static struct init_action actions[MAX_ACTIONS];
static int action_count = 0;
static int got_sigchild = 0;
static volatile int shutdown_signal = 0;  // set by signal handler, checked in main loop

// ── Signal handler (best-effort; even without delivery, ─────
// ── waitpid WNOHANG polling handles child reaping)        ───
static void sigchld_handler(int sig __attribute__((unused)))
{
    got_sigchild = 1;
}

// ── Shutdown signal handler ──────────────────────────────────
// BusyBox halt/poweroff/reboot (without -f) send these signals
// to PID 1 instead of calling the reboot syscall directly:
//   halt     → SIGUSR1
//   poweroff → SIGUSR2
//   reboot   → SIGTERM
static void shutdown_handler(int sig)
{
    shutdown_signal = sig;
}

// ── Find a tracked child by PID ─────────────────────────────
static struct child *find_child(int pid)
{
    for (int i = 0; i < child_count; i++) {
        if (children[i].pid == pid)
            return &children[i];
    }
    return NULL;
}

// ── Remove a tracked child ──────────────────────────────────
static void remove_child(int pid)
{
    for (int i = 0; i < child_count; i++) {
        if (children[i].pid == pid) {
            // Compact the array
            if (i < child_count - 1)
                children[i] = children[child_count - 1];
            child_count--;
            return;
        }
    }
}

// ── Track a new child ───────────────────────────────────────
static struct child *add_child(int pid, int action, const char *command)
{
    if (child_count >= MAX_PROCS)
        return NULL;
    struct child *c = &children[child_count++];
    c->pid = pid;
    c->action = action;
    c->restart_count = 0;
    // Truncate command safely
    size_t len = strlen(command);
    if (len >= sizeof(c->command)) len = sizeof(c->command) - 1;
    memcpy(c->command, command, len);
    c->command[len] = '\0';
    return c;
}

// ── Reap all exited children (non-blocking) ─────────────────
// Returns number of children reaped.
static int reap_children(void)
{
    int reaped = 0;
    int status;
    int64_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        struct child *c = find_child((int)pid);
        if (c) {
            printf("init: pid %d (%s) exited, status=%d, restarts=%d\n",
                   (int)pid, c->command, status, c->restart_count);
        } else {
            printf("init: untracked child pid %d exited, status=%d\n",
                   (int)pid, status);
        }
        remove_child((int)pid);
        reaped++;
    }
    got_sigchild = 0;
    return reaped;
}

// ── Fork and exec a process ─────────────────────────────────
// Returns child PID, or -1 on error.
static int spawn(const char *command, const char *tty __attribute__((unused)))
{
    // Parse command into argv (simple: split on space)
    char cmd_copy[128];
    size_t clen = strlen(command);
    if (clen >= sizeof(cmd_copy)) clen = sizeof(cmd_copy) - 1;
    memcpy(cmd_copy, command, clen);
    cmd_copy[clen] = '\0';

    char *argv[16];
    int argc = 0;
    char *p = cmd_copy;
    while (*p && argc < 15) {
        // Skip spaces
        while (*p == ' ') p++;
        if (!*p) break;
        argv[argc++] = p;
        // Scan to next space
        while (*p && *p != ' ') p++;
        if (*p) {
            *p = '\0';
            p++;
        }
    }
    argv[argc] = NULL;

    if (argc == 0)
        return -1;

    int64_t pid = fork();
    if (pid < 0) {
        printf("init: fork failed for '%s'\n", command);
        return -1;
    }

    if (pid == 0) {
        // ── Child ──────────────────────────────────────────
        // argv[0] is the executable path, argv[1..] are arguments.
        // This correctly passes the parsed inittab command to exec.
        if (argv[0] == NULL || argv[0][0] == '\0') {
            printf("init: empty command\n");
            exit(1);
        }
        int64_t ret = exec(argv[0], (char *const *)argv, NULL);

        // exec() should not return — if it does, print error and exit
        printf("init: exec '%s' failed, ret=%d\n", argv[0], (int)ret);
        exit(1);
    }

    // ── Parent ────────────────────────────────────────────
    return (int)pid;
}

// ── Run actions of a given type ─────────────────────────────
// For SYSINIT/WAIT: blocks until the spawned process exits.
// For ONCE: spawns without blocking.
// For RESPAWN/ASKFIRST: spawns if not already running.
static void run_actions(int action_mask)
{
    for (int i = 0; i < action_count; i++) {
        struct init_action *a = &actions[i];

        if (!(a->action & action_mask))
            continue;

        if (a->action == ACT_RESPAWN || a->action == ACT_ASKFIRST) {
            // Check if already running
            int running = 0;
            for (int j = 0; j < child_count; j++) {
                if (children[j].action == a->action &&
                    strcmp(children[j].command, a->process) == 0) {
                    running = 1;
                    break;
                }
            }
            if (running)
                continue;

            // ASKFIRST: prompt before first spawn
            if (a->action == ACT_ASKFIRST) {
                printf("\ninit: press Enter to start '%s'", a->process);
                char buf[2];
                read(0, buf, 1);  // wait for any key
            }

            int pid = spawn(a->process, a->tty);
            if (pid > 0) {
                add_child(pid, a->action, a->process);
                printf("init: started pid %d: '%s' (respawn)\n",
                       pid, a->process);
            }
            continue;
        }

        // SYSINIT, WAIT, ONCE
        int pid = spawn(a->process, a->tty);
        if (pid <= 0)
            continue;

        if (a->action == ACT_SYSINIT || a->action == ACT_WAIT) {
            // Block until this child exits
            printf("init: waiting for '%s' (pid %d)...\n", a->process, pid);
            int status;
            waitpid(pid, &status, 0);
            printf("init: '%s' done, status=%d\n", a->process, status);
        }
        // ONCE: fire-and-forget — the child is not tracked;
        // it will be reaped as an untracked orphan.
    }
}

// ── Shutdown sequence ───────────────────────────────────────
// cmd: RB_POWER_OFF for poweroff/halt, RB_AUTOBOOT for reboot
static void do_shutdown(int cmd)
{
    printf("init: shutting down... (cmd=%#x)\n", cmd);

    // 1. Run SHUTDOWN actions
    run_actions(ACT_SHUTDOWN);

    // 2. Send SIGTERM to all tracked children
    for (int i = 0; i < child_count; i++) {
        printf("init: sending SIGTERM to pid %d\n", children[i].pid);
        kill(children[i].pid, SIGTERM);
    }

    // 3. Reap children (with timeout-like behaviour: reap a few times)
    for (int attempt = 0; attempt < 10; attempt++) {
        int reaped = reap_children();
        if (child_count == 0) break;
        if (reaped == 0) {
            // nanosleep not available; just spin-wait a bit
            for (volatile int z = 0; z < 10000000; z++) { }
        }
    }

    // 4. Kill any survivors
    for (int i = 0; i < child_count; i++) {
        printf("init: SIGKILL to pid %d\n", children[i].pid);
        kill(children[i].pid, SIGKILL);
    }

    // 5. Sync filesystems
    sync();

    // 6. Execute the requested action
    printf("init: calling reboot(%#x)...\n", cmd);
    reboot(cmd);
    // If the requested action fails, fall through to reboot
    printf("init: reboot(%#x) failed, falling back to reboot...\n", cmd);
    reboot(RB_AUTOBOOT);
    // unreachable
}

// ── Add an action (from inittab or fallback) ────────────────
static void add_action(int action, const char *tty, const char *process)
{
    if (action_count >= MAX_ACTIONS) {
        printf("init: too many inittab entries (max %lu),"
               " ignoring '%s'\n", (unsigned long)MAX_ACTIONS, process);
        return;
    }
    struct init_action *a = &actions[action_count++];
    a->action = action;

    if (tty) {
        size_t len = strlen(tty);
        if (len >= sizeof(a->tty)) {
            printf("init: tty/id truncated (needs %lu, max %lu): '%s'\n",
                   (unsigned long)len,
                   (unsigned long)(sizeof(a->tty) - 1), tty);
            len = sizeof(a->tty) - 1;
        }
        memcpy(a->tty, tty, len);
        a->tty[len] = '\0';
    } else {
        a->tty[0] = '\0';
    }

    size_t len = strlen(process);
    if (len >= sizeof(a->process)) {
        printf("init: process truncated (needs %lu, max %lu): '%s'\n",
               (unsigned long)len,
               (unsigned long)(sizeof(a->process) - 1), process);
        len = sizeof(a->process) - 1;
    }
    memcpy(a->process, process, len);
    a->process[len] = '\0';
}

// ── Map action name to ACT_* bitmask ────────────────────────
static int parse_action(const char *name)
{
    static const struct { const char *name; int action; } map[] = {
        {"sysinit",   ACT_SYSINIT},
        {"wait",      ACT_WAIT},
        {"once",      ACT_ONCE},
        {"respawn",   ACT_RESPAWN},
        {"askfirst",  ACT_ASKFIRST},
        {"ctrlaltdel",ACT_CTRLALTDEL},
        {"shutdown",  ACT_SHUTDOWN},
        {"restart",   ACT_RESTART},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(name, map[i].name) == 0)
            return map[i].action;
    }
    return -1;
}

// ── Parse /etc/inittab ──────────────────────────────────────
// Format: id:action:process
// Actions: sysinit, wait, once, respawn, askfirst, ctrlaltdel, shutdown, restart
static void parse_inittab(void)
{
    int fd = open("/etc/inittab", O_RDONLY);
    if (fd < 0) {
        printf("init: /etc/inittab not found, using defaults\n");
        return;
    }

    // Inittab is a small build-time file (<1KB); single read() is sufficient.
    char buf[4096];
    int64_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return;
    buf[n] = '\0';

    char *p = buf;
    char *end = buf + n;
    int lineno = 0;

    while (p < end) {
        char *line_start = p;
        lineno++;

        // Find end of line
        char *nl = p;
        while (nl < end && *nl != '\n')
            nl++;
        char *line_end = nl; // points to '\n' or end

        // Trim trailing '\r' (CRLF)
        if (line_end > line_start && line_end[-1] == '\r')
            line_end--;

        // Null-terminate this line in-place for string ops
        char saved = *line_end;
        *line_end = '\0';

        // Skip leading whitespace
        char *s = line_start;
        while (*s == ' ' || *s == '\t')
            s++;

        // Skip blank lines and comments
        if (*s != '\0' && *s != '#') {
            // Split on ':', counting colons to detect excess fields
            char *fields[3] = {NULL, NULL, NULL};
            char *fp = s;
            int colons = 0;
            for (int i = 0; i < 3; i++) {
                fields[i] = fp;
                while (*fp && *fp != ':')
                    fp++;
                if (*fp == ':') {
                    *fp = '\0';
                    fp++;
                    colons++;
                } else {
                    break;
                }
            }
            // colons >= 3 means a 4th field boundary was seen
            int has_extra = (colons >= 3);

            if (!fields[0] || !fields[1] || !fields[2]) {
                printf("init: /etc/inittab:%d: missing fields"
                       " (need id:action:process)\n",
                       lineno);
            } else if (has_extra) {
                printf("init: /etc/inittab:%d: too many fields"
                       " (expected 3 colon-separated fields)\n",
                       lineno);
            } else {
                // Trim whitespace from each field
                for (int i = 0; i < 3; i++) {
                    char *fs = fields[i];
                    while (*fs == ' ' || *fs == '\t')
                        fs++;
                    fields[i] = fs;
                    char *fe = fs + strlen(fs);
                    while (fe > fs
                           && (fe[-1] == ' ' || fe[-1] == '\t')) {
                        fe--;
                        *fe = '\0';
                    }
                }

                if (fields[2][0] == '\0') {
                    printf("init: /etc/inittab:%d: empty process\n",
                           lineno);
                } else if (fields[1][0] == '\0') {
                    printf("init: /etc/inittab:%d: empty action\n",
                           lineno);
                } else {
                    int action = parse_action(fields[1]);
                    if (action < 0) {
                        printf("init: /etc/inittab:%d:"
                               " unknown action '%s'\n",
                               lineno, fields[1]);
                    } else {
                        add_action(action, fields[0], fields[2]);
                    }
                }
            }
        }

        // Restore and advance
        *line_end = saved;
        p = nl;
        if (p < end)
            p++; // skip '\n'
    }
}

// ── Set up hardcoded fallback actions ───────────────────────
// Called when /etc/inittab is absent or empty.
static void setup_fallback_actions(void)
{
    printf("init: no /etc/inittab, using built-in defaults\n");
    add_action(ACT_RESPAWN, "", "/bin/terminal");
}

// ── Main ────────────────────────────────────────────────────
int main(void)
{
    printf("\n");
    printf("+--------------------------------+\n");
    printf("|  OS01 Init v1.0 (PID 1)        |\n");
    printf("+--------------------------------+\n");
    printf("\n");

    // 1. Verify we are PID 1
    int my_pid = (int)syscall(SYS_getpid, 0, 0, 0);
    printf("init: running as PID %d\n", my_pid);

    // 2. Set up signal handling
    // SIGCHLD: reap children
    signal(SIGCHLD, sigchld_handler);
    // SIGINT (Ctrl-C): ignore (no tty job control on PID 1)
    signal(SIGINT, SIG_IGN);
    // SIGHUP: reload inittab (stub for now)
    signal(SIGHUP, SIG_IGN);
    // Shutdown signals: BusyBox halt/poweroff/reboot send these to PID 1
    // SIGUSR1 → halt, SIGUSR2 → poweroff, SIGTERM → reboot
    signal(SIGUSR1, shutdown_handler);
    signal(SIGUSR2, shutdown_handler);
    signal(SIGTERM, shutdown_handler);

    // 3. Ensure stdin/stdout/stderr are open
    // They should already be inherited from the kernel init thread.

    // 4. Load inittab actions
    parse_inittab();
    if (action_count == 0)
        setup_fallback_actions();

    // 5. SYSINIT phase (blocking)
    printf("init: phase SYSINIT\n");
    run_actions(ACT_SYSINIT);

    // 6. WAIT phase (blocking)
    printf("init: phase WAIT\n");
    run_actions(ACT_WAIT);

    // 7. ONCE phase (fire-and-forget)
    printf("init: phase ONCE\n");
    run_actions(ACT_ONCE);

    // 8. Main supervision loop
    printf("init: entering supervision loop\n");
    while (1) {
        // Check for shutdown signals (set by shutdown_handler)
        if (shutdown_signal) {
            int cmd = RB_POWER_OFF;  // default: power off
            if (shutdown_signal == SIGTERM)
                cmd = RB_AUTOBOOT;   // reboot → restart
            // SIGUSR1 (halt) and SIGUSR2 (poweroff) both → power off
            printf("init: received signal %d, shutting down...\n", shutdown_signal);
            do_shutdown(cmd);
            // do_shutdown does not return
        }

        // Reap any exited children
        reap_children();

        // Check if any RESPAWN/ASKFIRST actions need starting
        run_actions(ACT_RESPAWN | ACT_ASKFIRST);

        // Sleep briefly to avoid busy-waiting.
        {
            struct timespec req = { .tv_sec = 0, .tv_nsec = 100000000 };
            nanosleep(&req, NULL);
        }
    }

    return 0;
}
