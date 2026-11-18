// ── OS01 Shell (xv6-style) ────────────────────────────────
// Tagged-union AST + recursive-descent parser + fork/exec executor.
// Builtins: cd, pwd, echo, exit, help.  Operators: |  <  >  >>
//
// fd 0 = /dev/tty   (inherited from init — canonical ASCII line input)
// fd 1 = /dev/tty   (text output)
// fd 2 = /dev/tty   (error output)

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>      /* exit(3) — POSIX atexit-aware exit (AAGU-4.6) */
#include <stdint.h>
#include <string.h>

// ── Line discipline: fd 0 is /dev/tty ─────────────────────
// The tty layer owns echo and erase (canonical mode); the shell sees
// plain ASCII lines and never touches hardware scancodes.

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
            exec(ecmd->argv[0], ecmd->argv, NULL);
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

// ── Line reader ────────────────────────────────────────────
// fd 0 is /dev/tty (inherited from init).  The tty layer owns echo and
// erase in canonical mode, so this loop only collects the ASCII bytes
// the tty hands over; the same loop also behaves correctly if the tty
// is in raw mode (erase handled locally, no echo).

static void readline(char *buf, int max) {
    int n = 0;

    for (;;) {
        char ch;
        if (read(0, &ch, 1) <= 0) continue;

        if (ch == '\n' || ch == '\r') {
            buf[n] = '\0';
            return;
        }
        if (ch == 0x7f || ch == '\b') {
            // The tty already erased in canonical mode; this is the
            // raw-mode fallback so the buffer never keeps the byte.
            if (n > 0) n--;
            continue;
        }
        if (ch < 0x20) continue;  // ignore other control bytes
        if (n < max - 1)
            buf[n++] = ch;
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
