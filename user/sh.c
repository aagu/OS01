// ── OS01 Shell (xv6-style) ────────────────────────────────
// Tagged-union AST + recursive-descent parser + fork/exec executor.
// Builtins: cd, pwd, echo, exit, help.  Operators: |  <  >  >>
//
// fd 0 = /dev/keyboard  (inherited from init task — raw scancodes)
// fd 1 = /dev/fb         (framebuffer text output)
// fd 2 = /dev/serial      (COM1 error output)

#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>

// ── PS/2 scancode set 1 → ASCII mapping ──────────────────
// Identical to init.c — the keyboard returns raw scancodes.

#define SC_MAX 0x53

static const char ascii_tbl[SC_MAX + 1] = {
    [0x01] = 0x1B, [0x02] = '1', [0x03] = '2', [0x04] = '3',
    [0x05] = '4', [0x06] = '5', [0x07] = '6', [0x08] = '7',
    [0x09] = '8', [0x0A] = '9', [0x0B] = '0', [0x0C] = '-',
    [0x0D] = '=', [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r',
    [0x14] = 't', [0x15] = 'y', [0x16] = 'u', [0x17] = 'i',
    [0x18] = 'o', [0x19] = 'p', [0x1A] = '[', [0x1B] = ']',
    [0x1C] = '\n', [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd',
    [0x21] = 'f', [0x22] = 'g', [0x23] = 'h', [0x24] = 'j',
    [0x25] = 'k', [0x26] = 'l', [0x27] = ';', [0x28] = '\'',
    [0x29] = '`', [0x2B] = '\\', [0x2C] = 'z', [0x2D] = 'x',
    [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b', [0x31] = 'n',
    [0x32] = 'm', [0x33] = ',', [0x34] = '.', [0x35] = '/',
    [0x39] = ' ',
};

static const char shifted_tbl[SC_MAX + 1] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$',
    [0x06] = '%', [0x07] = '^', [0x08] = '&', [0x09] = '*',
    [0x0A] = '(', [0x0B] = ')', [0x0C] = '_', [0x0D] = '+',
    [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R',
    [0x14] = 'T', [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I',
    [0x18] = 'O', [0x19] = 'P', [0x1A] = '{', [0x1B] = '}',
    [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F',
    [0x22] = 'G', [0x23] = 'H', [0x24] = 'J', [0x25] = 'K',
    [0x26] = 'L', [0x27] = ':', [0x28] = '"', [0x29] = '~',
    [0x2B] = '|', [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C',
    [0x2F] = 'V', [0x30] = 'B', [0x31] = 'N', [0x32] = 'M',
    [0x33] = '<', [0x34] = '>', [0x35] = '?', [0x39] = ' ',
};

#define SC_LSHIFT   0x2A
#define SC_RSHIFT   0x36
#define SC_CAPSLOCK 0x3A

// ── AST node types ────────────────────────────────────────

#define EXEC  1
#define REDIR 2
#define PIPE  3

#define MAXARGS  16
#define MAXLINE  256

struct cmd { int type; };

struct execcmd {
    int type;
    char *argv[MAXARGS];
    char  buf[MAXLINE];
};

struct redircmd {
    int type;
    struct cmd *cmd;
    char *file;
    int mode;
    int fd;
};

struct pipecmd {
    int type;
    struct cmd *left;
    struct cmd *right;
};

// ── Parser globals ─────────────────────────────────────────

static char g_line[MAXLINE];
static int  g_pos;
static int  g_tok_type;
static char g_tok_word[MAXLINE];
static int  g_tok_ready;

// Pool allocators
static struct execcmd execcmd_pool[8];
static struct redircmd redircmd_pool[8];
static struct pipecmd pipecmd_pool[8];
static int execcmd_n, redircmd_n, pipecmd_n;

// ── Forward declarations ──────────────────────────────────

void runcmd(struct cmd *cmd);
static struct cmd *parsecmd(char *s);

// ── Tokenizer ──────────────────────────────────────────────

static int isspace_char(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static void skip_ws(void) {
    while (g_line[g_pos] && isspace_char(g_line[g_pos]))
        g_pos++;
}

static void next_token(void) {
    skip_ws();
    char c = g_line[g_pos];
    if (c == '\0') { g_tok_type = 0; g_tok_word[0] = '\0'; return; }
    if (c == '|') { g_tok_type = '|'; g_tok_word[0] = '|'; g_tok_word[1] = '\0'; g_pos++; return; }
    if (c == '<') { g_tok_type = '<'; g_tok_word[0] = '<'; g_tok_word[1] = '\0'; g_pos++; return; }
    if (c == '>') {
        g_pos++;
        if (g_line[g_pos] == '>') {
            g_tok_type = '+';
            g_tok_word[0] = '>'; g_tok_word[1] = '>'; g_tok_word[2] = '\0';
            g_pos++;
        } else {
            g_tok_type = '>'; g_tok_word[0] = '>'; g_tok_word[1] = '\0';
        }
        return;
    }
    int n = 0;
    while (c && !isspace_char(c) && c != '|' && c != '<' && c != '>') {
        if (n < MAXLINE - 1) g_tok_word[n++] = c;
        c = g_line[++g_pos];
    }
    g_tok_word[n] = '\0';
    g_tok_type = 'a';
}

static int peek_type(void) {
    if (!g_tok_ready) { next_token(); g_tok_ready = 1; }
    return g_tok_type;
}

static void advance(void) {
    if (g_tok_ready) g_tok_ready = 0;
    else next_token();
}

// ── AST allocators ─────────────────────────────────────────

static struct execcmd *alloc_execcmd(void) {
    if (execcmd_n >= 8) return NULL;
    struct execcmd *p = &execcmd_pool[execcmd_n++];
    memset(p, 0, sizeof(*p));
    p->type = EXEC;
    return p;
}
static struct redircmd *alloc_redircmd(void) {
    if (redircmd_n >= 8) return NULL;
    struct redircmd *p = &redircmd_pool[redircmd_n++];
    memset(p, 0, sizeof(*p));
    p->type = REDIR;
    return p;
}
static struct pipecmd *alloc_pipecmd(void) {
    if (pipecmd_n >= 8) return NULL;
    struct pipecmd *p = &pipecmd_pool[pipecmd_n++];
    memset(p, 0, sizeof(*p));
    p->type = PIPE;
    return p;
}

static char *copy_str(struct execcmd *ecmd, const char *s) {
    int len = strlen(s) + 1;
    // Find the current end of the used region in buf by scanning
    // all existing argv strings and computing their end addresses.
    char *p = ecmd->buf;
    for (int i = 0; i < MAXARGS && ecmd->argv[i]; i++) {
        char *s_end = ecmd->argv[i] + strlen(ecmd->argv[i]) + 1;
        if (s_end > p) p = s_end;
    }
    if (p + len > ecmd->buf + sizeof(ecmd->buf)) return NULL;
    memcpy(p, s, len);
    return p;
}

// ── Parser ─────────────────────────────────────────────────

static struct cmd *parseexec(void);

static struct cmd *parseredirs(struct cmd *cmd) {
    int t;
    while ((t = peek_type()) == '<' || t == '>' || t == '+') {
        advance();
        if (peek_type() != 'a') return NULL;
        struct redircmd *rcmd = alloc_redircmd();
        if (!rcmd) return NULL;
        rcmd->cmd = cmd;
        rcmd->file = copy_str((struct execcmd *)cmd, g_tok_word);
        advance();
        if (t == '<') { rcmd->mode = 0; rcmd->fd = 0; }
        else if (t == '>') { rcmd->mode = 1 | 0100; rcmd->fd = 1; }
        else { rcmd->mode = 1 | 0100 | 02000; rcmd->fd = 1; }
        cmd = (struct cmd *)rcmd;
    }
    return cmd;
}

static struct cmd *parseexec(void) {
    struct execcmd *ecmd = alloc_execcmd();
    if (!ecmd) return NULL;
    int argc = 0;
    while (peek_type() == 'a') {
        if (argc >= MAXARGS - 1) break;
        ecmd->argv[argc] = copy_str(ecmd, g_tok_word);
        if (!ecmd->argv[argc]) break;
        argc++; advance();
    }
    ecmd->argv[argc] = NULL;
    return parseredirs((struct cmd *)ecmd);
}

static struct cmd *parsepipe(void) {
    struct cmd *cmd = parseexec();
    if (!cmd) return NULL;
    if (peek_type() == '|') {
        advance();
        struct pipecmd *pcmd = alloc_pipecmd();
        if (!pcmd) return cmd;
        pcmd->left = cmd;
        pcmd->right = parsepipe();
        if (!pcmd->right) return cmd;
        return (struct cmd *)pcmd;
    }
    return cmd;
}

static struct cmd *parsecmd(char *s) {
    execcmd_n = redircmd_n = pipecmd_n = 0;
    int i;
    for (i = 0; s[i]; i++) g_line[i] = s[i];
    g_line[i] = '\0';
    g_pos = 0; g_tok_ready = 0;
    if (peek_type() == 0) return NULL;
    return parsepipe();
}

// ── Builtins ───────────────────────────────────────────────

static int builtin(struct execcmd *ecmd) {
    if (!ecmd->argv[0]) return 0;
    if (strcmp(ecmd->argv[0], "cd") == 0) {
        const char *dir = ecmd->argv[1] ? ecmd->argv[1] : "/";
        if (chdir(dir) < 0) printf("cd: %s: no such directory\n", dir);
        return 1;
    }
    if (strcmp(ecmd->argv[0], "pwd") == 0) {
        char cwd[256];
        if (getcwd(cwd, sizeof(cwd)) == 0) printf("%s\n", cwd);
        return 1;
    }
    if (strcmp(ecmd->argv[0], "echo") == 0) {
        for (int i = 1; ecmd->argv[i]; i++) {
            if (i > 1) printf(" ");
            printf("%s", ecmd->argv[i]);
        }
        printf("\n");
        return 1;
    }
    if (strcmp(ecmd->argv[0], "exit") == 0) exit(0);
    if (strcmp(ecmd->argv[0], "help") == 0) {
        printf("builtins: cd pwd echo exit help\n");
        printf("ops: | < > >>\n");
        printf("progs: /spin.elf\n");
        return 1;
    }
    return 0;
}

// ── Runner ─────────────────────────────────────────────────

void runcmd(struct cmd *cmd) {
    if (!cmd) return;
    switch (cmd->type) {
    case EXEC: {
        struct execcmd *ecmd = (struct execcmd *)cmd;
        if (!ecmd->argv[0]) return;
        if (builtin(ecmd)) return;
        int64_t pid = fork();
        if (pid < 0) { printf("fork failed\n"); return; }
        if (pid == 0) {
            exec(ecmd->argv[0]);
            printf("exec: %s: failed\n", ecmd->argv[0]);
            exit(1);
        }
        waitpid(pid, NULL, 0);
        break;
    }
    case REDIR: {
        struct redircmd *rcmd = (struct redircmd *)cmd;
        close(rcmd->fd);
        if (open(rcmd->file, rcmd->mode) < 0) {
            printf("open: %s: failed\n", rcmd->file);
            break;
        }
        runcmd(rcmd->cmd);
        break;
    }
    case PIPE: {
        struct pipecmd *pcmd = (struct pipecmd *)cmd;
        int p[2];
        if (pipe(p) < 0) { printf("pipe failed\n"); return; }
        int64_t left_pid = fork();
        if (left_pid < 0) { printf("fork failed\n"); close(p[0]); close(p[1]); return; }
        if (left_pid == 0) {
            close(1); dup(p[1]); close(p[0]); close(p[1]);
            runcmd(pcmd->left); exit(0);
        }
        int64_t right_pid = fork();
        if (right_pid < 0) { printf("fork failed\n"); close(p[0]); close(p[1]); waitpid(left_pid, NULL, 0); return; }
        if (right_pid == 0) {
            close(0); dup(p[0]); close(p[0]); close(p[1]);
            runcmd(pcmd->right); exit(0);
        }
        close(p[0]); close(p[1]);
        waitpid(left_pid, NULL, 0);
        waitpid(right_pid, NULL, 0);
        break;
    }
    }
}

// ── Line reader with scancode→ASCII translation ────────────
// Reads raw scancodes from fd 0 (/dev/keyboard) and translates
// to ASCII using the PS/2 set-1 tables. Tracks shift/capslock state.

static void readline(char *buf, int max) {
    int n = 0;
    int shift = 0;
    int capslock = 0;

    for (;;) {
        uint8_t sc;
        if (read(0, &sc, 1) <= 0) continue;

        uint8_t code = sc & 0x7F;
        int released = (sc >> 7) & 1;

        // Modifier tracking
        if (code == SC_LSHIFT || code == SC_RSHIFT) {
            shift = released ? 0 : 1;
            continue;
        }
        if (code == SC_CAPSLOCK) {
            if (!released) capslock = !capslock;
            continue;
        }
        if (released) continue;  // ignore key-release for non-modifiers

        if (code > SC_MAX) continue;

        int use_shift = shift ^ capslock;
        char ch = use_shift ? shifted_tbl[code] : ascii_tbl[code];
        if (ch == 0) continue;

        if (ch == '\n' || ch == '\r') {
            buf[n] = '\0';
            write(1, "\n", 1);
            return;
        }
        if (ch == '\b') {
            if (n > 0) { n--; write(1, "\b \b", 3); }
            continue;
        }
        if (n < max - 1) {
            buf[n++] = ch;
            write(1, &ch, 1);
        }
    }
}

// ── Main ───────────────────────────────────────────────────

int main(void) {
    char buf[MAXLINE];
    printf("OS01 Shell\n");

    for (;;) {
        printf("$ ");
        readline(buf, MAXLINE);
        if (buf[0] == '\0') continue;
        struct cmd *cmd = parsecmd(buf);
        if (cmd) runcmd(cmd);
    }
    return 0;
}
