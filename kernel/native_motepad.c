// motepad -- the terminal half of the Workspace MotePad editor.
//
// A native program (kernel/native.c), so it is host code called on the guest
// task's thread: it costs the same under an i386 root as an arm64 one, and
// there is no ELF to match. That is also why it is here rather than a binary
// in /AOK/persist/bin -- the app is built by a Darwin toolchain and cannot
// cross-compile a Linux executable to install there.
//
// Modeless on purpose. MotePad is a plain text editor with a line-number
// gutter, not a modal one, and a terminal counterpart that made you learn vi
// would not be the same program. Keys are the ones a MotePad user already has
// in the GUI: Ctrl-S saves, Ctrl-Q quits, Ctrl-F finds, Ctrl-G goes to a line.
//
// Under Workspace it hands the file to the GUI applet instead, unless told not
// to -- see the /proc/ish/workspace bridge in fs/proc/ish.c.
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#define MP_TAB_STOP 4
#define MP_GUTTER_MIN 3

struct mp_line {
    char *s;
    size_t len, cap;
};

struct mp {
    struct mp_line *line;
    size_t nlines, cap;
    char *path;
    bool dirty;

    size_t cy, cx;        // cursor: line index, byte offset within the line
    size_t rowoff, coloff;
    int rows, cols;       // usable text area, gutter excluded
    int gutter;

    char status[256];
    struct termios saved;
    bool raw;

    char find[128];
};

static struct mp E;

// ------------------------------------------------------------------ utility

static void *mp_realloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (q == NULL && n != 0) {
        // Out of memory mid-edit: restoring the terminal matters more than a
        // tidy unwind, or the user is left with a dead shell.
        if (E.raw)
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.saved);
        write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7);
        fprintf(stderr, "motepad: out of memory\n");
        exit(1);
    }
    return q;
}

static void mp_status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.status, sizeof(E.status), fmt, ap);
    va_end(ap);
}

// UTF-8: a continuation byte is 10xxxxxx. Cursor motion and deletion step over
// whole characters, so a multi-byte character is never split in half -- which
// is the difference between "handles UTF-8" and "corrupts it on the arrow key".
static bool mp_is_cont(unsigned char c) { return (c & 0xc0) == 0x80; }

static size_t mp_left_of(const struct mp_line *l, size_t x) {
    if (x == 0)
        return 0;
    x--;
    while (x > 0 && mp_is_cont((unsigned char) l->s[x]))
        x--;
    return x;
}

static size_t mp_right_of(const struct mp_line *l, size_t x) {
    if (x >= l->len)
        return l->len;
    x++;
    while (x < l->len && mp_is_cont((unsigned char) l->s[x]))
        x++;
    return x;
}

// Display column of a byte offset: tabs expand, continuation bytes take none.
// No wcwidth here -- a double-width CJK glyph counts as one column, which can
// leave the cursor a cell off on such a line. Honest limitation, not a claim.
static int mp_width_to(const struct mp_line *l, size_t x) {
    int w = 0;
    for (size_t i = 0; i < x && i < l->len; i++) {
        if (l->s[i] == '\t')
            w += MP_TAB_STOP - (w % MP_TAB_STOP);
        else if (!mp_is_cont((unsigned char) l->s[i]))
            w++;
    }
    return w;
}

// ------------------------------------------------------------------ document

static void mp_line_init(struct mp_line *l) { l->s = NULL; l->len = l->cap = 0; }

static void mp_line_reserve(struct mp_line *l, size_t need) {
    if (need <= l->cap)
        return;
    size_t cap = l->cap ? l->cap * 2 : 32;
    while (cap < need)
        cap *= 2;
    l->s = mp_realloc(l->s, cap);
    l->cap = cap;
}

static void mp_insert_line(size_t at, const char *s, size_t len) {
    if (E.nlines + 1 > E.cap) {
        E.cap = E.cap ? E.cap * 2 : 64;
        E.line = mp_realloc(E.line, E.cap * sizeof(*E.line));
    }
    memmove(&E.line[at + 1], &E.line[at], (E.nlines - at) * sizeof(*E.line));
    mp_line_init(&E.line[at]);
    mp_line_reserve(&E.line[at], len + 1);
    memcpy(E.line[at].s, s, len);
    E.line[at].len = len;
    E.nlines++;
}

static void mp_free_line(size_t at) {
    free(E.line[at].s);
    memmove(&E.line[at], &E.line[at + 1], (E.nlines - at - 1) * sizeof(*E.line));
    E.nlines--;
}

static int mp_open(const char *path) {
    free(E.path);
    E.path = strdup(path);
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        // A missing file is how you create one, exactly as every editor does.
        // Anything else is worth saying out loud rather than starting empty and
        // silently offering to overwrite whatever is really there.
        if (errno == ENOENT) {
            mp_insert_line(0, "", 0);
            mp_status("%s: new file", path);
            return 0;
        }
        return -1;
    }
    char buf[8192];
    struct mp_line pending;
    mp_line_init(&pending);
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                mp_insert_line(E.nlines, pending.s ? pending.s : "", pending.len);
                pending.len = 0;
            } else if (buf[i] != '\r') {
                mp_line_reserve(&pending, pending.len + 1);
                pending.s[pending.len++] = buf[i];
            }
        }
    }
    int read_err = n < 0 ? errno : 0;
    close(fd);
    if (pending.len > 0)
        mp_insert_line(E.nlines, pending.s, pending.len);
    free(pending.s);
    if (read_err) {
        errno = read_err;
        return -1;
    }
    if (E.nlines == 0)
        mp_insert_line(0, "", 0);
    mp_status("%s: %zu lines", path, E.nlines);
    return 0;
}

// Written to a temp file and renamed, so an interrupted save cannot leave the
// original truncated. MotePad's own store does the same thing.
static int mp_save(void) {
    if (E.path == NULL) {
        mp_status("no filename");
        return -1;
    }
    char tmp[1024];
    if (snprintf(tmp, sizeof(tmp), "%s.motepad.tmp", E.path) >= (int) sizeof(tmp)) {
        mp_status("path too long to save safely");
        return -1;
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        mp_status("cannot write %s: %s", tmp, strerror(errno));
        return -1;
    }
    for (size_t i = 0; i < E.nlines; i++) {
        if ((E.line[i].len > 0 &&
             write(fd, E.line[i].s, E.line[i].len) != (ssize_t) E.line[i].len) ||
                write(fd, "\n", 1) != 1) {
            mp_status("write failed: %s", strerror(errno));
            close(fd);
            unlink(tmp);
            return -1;
        }
    }
    if (close(fd) != 0 || rename(tmp, E.path) != 0) {
        mp_status("save failed: %s", strerror(errno));
        unlink(tmp);
        return -1;
    }
    E.dirty = false;
    mp_status("wrote %s (%zu lines)", E.path, E.nlines);
    return 0;
}

// ------------------------------------------------------------------ terminal

static void mp_size(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        E.cols = ws.ws_col;
        E.rows = ws.ws_row;
    } else {
        // No winsize is not fatal: a pty without one still draws, it just
        // draws for an 80x24 terminal. Refusing to start would be worse.
        E.cols = 80;
        E.rows = 24;
    }
    int digits = 1;
    for (size_t n = E.nlines; n >= 10; n /= 10)
        digits++;
    E.gutter = digits < MP_GUTTER_MIN ? MP_GUTTER_MIN : digits;
    E.gutter += 1;                       // the space after the number
    E.rows -= 2;                         // status line and message line
    E.cols -= E.gutter;
    if (E.rows < 1) E.rows = 1;
    if (E.cols < 1) E.cols = 1;
}

static void mp_on_winch(int sig) {
    (void) sig;
    mp_size();
}

static int mp_raw_on(void) {
    if (tcgetattr(STDIN_FILENO, &E.saved) != 0)
        return -1;
    struct termios t = E.saved;
    t.c_lflag &= ~(unsigned) (ECHO | ICANON | ISIG | IEXTEN);
    t.c_iflag &= ~(unsigned) (IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    t.c_oflag &= ~(unsigned) OPOST;
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &t) != 0)
        return -1;
    E.raw = true;
    return 0;
}

static void mp_raw_off(void) {
    if (!E.raw)
        return;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.saved);
    E.raw = false;
}

// ------------------------------------------------------------------ drawing

struct mp_buf { char *b; size_t len, cap; };

static void mp_append(struct mp_buf *ab, const char *s, size_t len) {
    if (ab->len + len > ab->cap) {
        ab->cap = ab->cap ? ab->cap * 2 : 4096;
        while (ab->cap < ab->len + len)
            ab->cap *= 2;
        ab->b = mp_realloc(ab->b, ab->cap);
    }
    memcpy(ab->b + ab->len, s, len);
    ab->len += len;
}

static void mp_appends(struct mp_buf *ab, const char *s) { mp_append(ab, s, strlen(s)); }

static void mp_scroll(void) {
    if (E.cy < E.rowoff)
        E.rowoff = E.cy;
    if (E.cy >= E.rowoff + (size_t) E.rows)
        E.rowoff = E.cy - E.rows + 1;
    int cxw = mp_width_to(&E.line[E.cy], E.cx);
    if ((size_t) cxw < E.coloff)
        E.coloff = cxw;
    if ((size_t) cxw >= E.coloff + (size_t) E.cols)
        E.coloff = cxw - E.cols + 1;
}

static void mp_draw(void) {
    mp_scroll();
    struct mp_buf ab = {0};
    mp_appends(&ab, "\x1b[?25l\x1b[H");   // hide cursor, home

    for (int y = 0; y < E.rows; y++) {
        size_t idx = E.rowoff + (size_t) y;
        char num[32];
        if (idx < E.nlines) {
            snprintf(num, sizeof(num), "\x1b[2m%*zu\x1b[0m ", E.gutter - 1, idx + 1);
            mp_appends(&ab, num);
            // Expand tabs and clip to the window by DISPLAY column, not byte
            // offset, or a line with tabs scrolls out of step with the cursor.
            const struct mp_line *l = &E.line[idx];
            int col = 0;
            for (size_t i = 0; i < l->len; i++) {
                if (l->s[i] == '\t') {
                    int stop = MP_TAB_STOP - (col % MP_TAB_STOP);
                    while (stop-- > 0) {
                        if ((size_t) col >= E.coloff && col < (int) (E.coloff + E.cols))
                            mp_append(&ab, " ", 1);
                        col++;
                    }
                    continue;
                }
                bool cont = mp_is_cont((unsigned char) l->s[i]);
                if ((size_t) col >= E.coloff && col < (int) (E.coloff + E.cols))
                    mp_append(&ab, &l->s[i], 1);
                if (!cont)
                    col++;
            }
        } else {
            mp_appends(&ab, "\x1b[2m~\x1b[0m");
        }
        mp_appends(&ab, "\x1b[K\r\n");
    }

    char st[512];
    snprintf(st, sizeof(st), "\x1b[7m %-*s %s  %zu,%d \x1b[0m\x1b[K\r\n",
             E.cols > 40 ? E.cols - 24 : 8,
             E.path ? E.path : "[no name]",
             E.dirty ? "(modified)" : "          ",
             E.cy + 1, mp_width_to(&E.line[E.cy], E.cx) + 1);
    mp_appends(&ab, st);
    mp_appends(&ab, E.status);
    mp_appends(&ab, "\x1b[K");

    char pos[32];
    snprintf(pos, sizeof(pos), "\x1b[%d;%dH",
             (int) (E.cy - E.rowoff) + 1,
             (int) (mp_width_to(&E.line[E.cy], E.cx) - E.coloff) + E.gutter + 1);
    mp_appends(&ab, pos);
    mp_appends(&ab, "\x1b[?25h");
    write(STDOUT_FILENO, ab.b, ab.len);
    free(ab.b);
}

// ------------------------------------------------------------------ editing

static void mp_insert_char(int c) {
    struct mp_line *l = &E.line[E.cy];
    mp_line_reserve(l, l->len + 1);
    memmove(&l->s[E.cx + 1], &l->s[E.cx], l->len - E.cx);
    l->s[E.cx] = (char) c;
    l->len++;
    E.cx++;
    E.dirty = true;
}

static void mp_insert_newline(void) {
    struct mp_line *l = &E.line[E.cy];
    mp_insert_line(E.cy + 1, &l->s[E.cx], l->len - E.cx);
    // mp_insert_line may have reallocated the array out from under `l`.
    E.line[E.cy].len = E.cx;
    E.cy++;
    E.cx = 0;
    E.dirty = true;
    mp_size();   // the gutter may have grown a digit
}

static void mp_backspace(void) {
    if (E.cx > 0) {
        struct mp_line *l = &E.line[E.cy];
        size_t from = mp_left_of(l, E.cx);
        memmove(&l->s[from], &l->s[E.cx], l->len - E.cx);
        l->len -= E.cx - from;
        E.cx = from;
        E.dirty = true;
    } else if (E.cy > 0) {
        size_t prev = E.cy - 1;
        size_t at = E.line[prev].len;
        mp_line_reserve(&E.line[prev], E.line[prev].len + E.line[E.cy].len);
        memcpy(&E.line[prev].s[at], E.line[E.cy].s, E.line[E.cy].len);
        E.line[prev].len += E.line[E.cy].len;
        mp_free_line(E.cy);
        E.cy = prev;
        E.cx = at;
        E.dirty = true;
    }
}

static void mp_delete(void) {
    struct mp_line *l = &E.line[E.cy];
    if (E.cx < l->len) {
        size_t to = mp_right_of(l, E.cx);
        memmove(&l->s[E.cx], &l->s[to], l->len - to);
        l->len -= to - E.cx;
        E.dirty = true;
    } else if (E.cy + 1 < E.nlines) {
        size_t next = E.cy + 1;
        mp_line_reserve(l, l->len + E.line[next].len);
        memcpy(&l->s[l->len], E.line[next].s, E.line[next].len);
        l->len += E.line[next].len;
        mp_free_line(next);
        E.dirty = true;
    }
}

static void mp_kill_line(void) {
    if (E.nlines == 1) {
        E.line[0].len = 0;
        E.cx = 0;
    } else {
        mp_free_line(E.cy);
        if (E.cy >= E.nlines)
            E.cy = E.nlines - 1;
        E.cx = 0;
    }
    E.dirty = true;
}

// ------------------------------------------------------------------ input

enum {
    MP_LEFT = 1000, MP_RIGHT, MP_UP, MP_DOWN,
    MP_HOME, MP_END, MP_PGUP, MP_PGDN, MP_DEL,
};

static int mp_read_key(void) {
    char c;
    ssize_t n;
    while ((n = read(STDIN_FILENO, &c, 1)) != 1) {
        // EINTR is normal here: SIGWINCH lands mid-read every time the window
        // is dragged. Anything else, and there is no input source left.
        if (n < 0 && errno == EINTR)
            continue;
        if (n == 0 || n < 0)
            return -1;
    }
    if (c != '\x1b')
        return (unsigned char) c;

    char seq[3];
    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
    if (seq[0] == '[') {
        if (seq[1] >= '0' && seq[1] <= '9') {
            if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
            if (seq[2] == '~') {
                switch (seq[1]) {
                    case '1': case '7': return MP_HOME;
                    case '3': return MP_DEL;
                    case '4': case '8': return MP_END;
                    case '5': return MP_PGUP;
                    case '6': return MP_PGDN;
                }
            }
            return '\x1b';
        }
        switch (seq[1]) {
            case 'A': return MP_UP;
            case 'B': return MP_DOWN;
            case 'C': return MP_RIGHT;
            case 'D': return MP_LEFT;
            case 'H': return MP_HOME;
            case 'F': return MP_END;
        }
    } else if (seq[0] == 'O') {
        switch (seq[1]) {
            case 'H': return MP_HOME;
            case 'F': return MP_END;
        }
    }
    return '\x1b';
}

// A one-line prompt on the message row. Returns a malloc'd answer, or NULL if
// the user pressed Escape -- which every caller treats as "do nothing", so
// there is always a way out of a prompt entered by accident.
static char *mp_prompt(const char *label) {
    size_t cap = 64, len = 0;
    char *buf = mp_realloc(NULL, cap);
    buf[0] = '\0';
    for (;;) {
        mp_status("%s%s", label, buf);
        mp_draw();
        int c = mp_read_key();
        if (c == '\x1b' || c == -1) {
            free(buf);
            mp_status("");
            return NULL;
        }
        if (c == '\r' || c == '\n') {
            mp_status("");
            return buf;
        }
        if (c == 127 || c == 8) {
            if (len > 0)
                buf[--len] = '\0';
            continue;
        }
        if (c < 32 || c >= 1000)
            continue;
        if (len + 2 > cap) {
            cap *= 2;
            buf = mp_realloc(buf, cap);
        }
        buf[len++] = (char) c;
        buf[len] = '\0';
    }
}

static void mp_find(void) {
    char *q = mp_prompt("find: ");
    if (q == NULL)
        return;
    if (*q != '\0')
        snprintf(E.find, sizeof(E.find), "%s", q);
    free(q);
    if (E.find[0] == '\0')
        return;
    // Search forward from just after the cursor, then wrap. Wrapping is what
    // makes a repeat-find key usable at all.
    for (size_t n = 0; n <= E.nlines; n++) {
        size_t i = (E.cy + n) % E.nlines;
        const char *from = E.line[i].s ? E.line[i].s : "";
        size_t start = (n == 0 && E.cx + 1 <= E.line[i].len) ? E.cx + 1 : 0;
        if (n == 0 && start > E.line[i].len)
            continue;
        // The line is not NUL-terminated; give strstr a terminated copy.
        char *tmp = mp_realloc(NULL, E.line[i].len + 1);
        memcpy(tmp, from, E.line[i].len);
        tmp[E.line[i].len] = '\0';
        char *hit = strstr(tmp + start, E.find);
        if (hit != NULL) {
            E.cy = i;
            E.cx = (size_t) (hit - tmp);
            free(tmp);
            mp_status("found \"%s\"", E.find);
            return;
        }
        free(tmp);
    }
    mp_status("not found: %s", E.find);
}

static void mp_goto_line(void) {
    char *a = mp_prompt("go to line: ");
    if (a == NULL)
        return;
    long n = strtol(a, NULL, 10);
    free(a);
    if (n < 1)
        n = 1;
    if ((size_t) n > E.nlines)
        n = (long) E.nlines;
    E.cy = (size_t) n - 1;
    E.cx = 0;
}

// ------------------------------------------------------------ workspace handoff

// Read /proc/ish/workspace and, if this session is Workspace-hosted, ask the
// app to open `path` in the GUI MotePad instead. Returns true when the handoff
// happened and there is nothing left to do here.
static bool mp_handoff(const char *path) {
    int fd = open("/proc/ish/workspace", O_RDONLY);
    if (fd < 0)
        return false;
    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return false;
    buf[n] = '\0';
    if (strncmp(buf, "hosted=1", 8) != 0)
        return false;

    // The bridge takes absolute paths only -- the app has no idea what this
    // process's cwd is, and may look at the request after the process is gone.
    char abs[1024];
    if (path[0] == '/') {
        snprintf(abs, sizeof(abs), "%s", path);
    } else {
        char cwd[768];
        if (getcwd(cwd, sizeof(cwd)) == NULL)
            return false;
        if (snprintf(abs, sizeof(abs), "%s/%s", cwd, path) >= (int) sizeof(abs))
            return false;
    }

    char req[1100];
    int len = snprintf(req, sizeof(req), "open motepad %s\n", abs);
    if (len < 0 || len >= (int) sizeof(req))
        return false;
    fd = open("/proc/ish/workspace", O_WRONLY);
    if (fd < 0)
        return false;
    bool ok = write(fd, req, (size_t) len) == len;
    close(fd);
    if (ok) {
        printf("motepad: opened %s in the Workspace applet (-t edits here instead)\n", abs);
        fflush(stdout);   // see mp_usage
    }
    return ok;
}

// ------------------------------------------------------------------ main

static void mp_usage(void) {
    printf("usage: motepad [-t] [file]\n"
           "\n"
           "  A plain text editor. Ctrl-S save, Ctrl-Q quit, Ctrl-F find,\n"
           "  Ctrl-G go to line, Ctrl-K delete line, Ctrl-A/Ctrl-E line ends.\n"
           "\n"
           "  Under Workspace, `motepad file` hands the file to the GUI applet.\n"
           "  -t edits in the terminal instead.\n");
    // A native program is a function call into a process that keeps running
    // afterwards -- nothing drains stdio on the way out, so an unflushed
    // buffer is simply lost. This cost the first --help its entire output.
    fflush(stdout);
}

int native_motepad_main(int argc, char *const argv[], char *const envp[]) {
    (void) envp;
    bool force_terminal = false;
    const char *path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--terminal") == 0) {
            force_terminal = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            mp_usage();
            return 0;
        } else if (strcmp(argv[i], "--selftest") == 0) {
            // Exercises the document model and the save path with no terminal
            // involved, so a failure here is the editor's and not the tty's.
            // Kept in the program rather than a separate test because a native
            // program cannot be linked into the tier0 harness -- it is host
            // code inside the emulator, not a guest ELF.
            for (size_t k = 0; k < E.nlines; k++)
                free(E.line[k].s);
            free(E.line);
            free(E.path);
            memset(&E, 0, sizeof(E));
            mp_insert_line(0, "", 0);
            E.path = strdup("/realmnt/motepad-selftest.txt");
            const char *first = "hello", *second = "world";
            for (const char *q = first; *q; q++)
                mp_insert_char(*q);
            mp_insert_newline();
            for (const char *q = second; *q; q++)
                mp_insert_char(*q);
            // Backspace over the 'd', then retype it: exercises deletion and
            // the cursor bookkeeping, not just append.
            mp_backspace();
            mp_insert_char('d');
            // Home, then delete the leading 'w', then put it back.
            E.cx = 0;
            mp_delete();
            mp_insert_char('w');
            int rc = mp_save();
            printf("selftest: lines=%zu save=%s (%s) l1=\"%.*s\" l2=\"%.*s\"\n",
                   E.nlines, rc == 0 ? "ok" : "FAILED", E.status,
                   (int) E.line[0].len, E.line[0].s,
                   (int) E.line[1].len, E.line[1].s);
            fflush(stdout);
            return rc == 0 && E.nlines == 2 &&
                   E.line[0].len == 5 && memcmp(E.line[0].s, "hello", 5) == 0 &&
                   E.line[1].len == 5 && memcmp(E.line[1].s, "world", 5) == 0 ? 0 : 1;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "motepad: unknown option %s\n", argv[i]);
            return 2;
        } else {
            path = argv[i];
        }
    }

    if (path != NULL && !force_terminal && mp_handoff(path))
        return 0;

    // Every run starts from a clean slate: a native program is a function call
    // into a process that may have run it before, so these globals survive
    // between runs and must not be assumed zero.
    for (size_t i = 0; i < E.nlines; i++)
        free(E.line[i].s);
    free(E.line);
    free(E.path);
    memset(&E, 0, sizeof(E));

    if (path != NULL) {
        if (mp_open(path) != 0) {
            fprintf(stderr, "motepad: %s: %s\n", path, strerror(errno));
            return 1;
        }
    } else {
        mp_insert_line(0, "", 0);
        mp_status("new file -- Ctrl-S saves, Ctrl-Q quits");
    }

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "motepad: not a terminal (use the Workspace applet, or redirect nothing)\n");
        return 1;
    }
    if (mp_raw_on() != 0) {
        fprintf(stderr, "motepad: cannot set raw mode: %s\n", strerror(errno));
        return 1;
    }
    signal(SIGWINCH, mp_on_winch);
    mp_size();

    int quit_confirm = 0;
    for (;;) {
        mp_draw();
        int c = mp_read_key();
        if (c == -1)
            break;

        // Any key that is not Ctrl-Q cancels a pending quit confirmation, so a
        // stray Ctrl-Q followed by typing cannot discard the buffer later.
        if (c != 17 && quit_confirm > 0) {
            quit_confirm = 0;
            mp_status("");
        }

        switch (c) {
            case 17:   // Ctrl-Q
                if (E.dirty && quit_confirm == 0) {
                    quit_confirm = 1;
                    mp_status("unsaved changes -- Ctrl-Q again to discard, Ctrl-S to save");
                    continue;
                }
                goto done;
            case 19:   // Ctrl-S
                mp_save();
                break;
            case 6:    // Ctrl-F
                mp_find();
                break;
            case 7:    // Ctrl-G
                mp_goto_line();
                break;
            case 11:   // Ctrl-K
                mp_kill_line();
                mp_size();
                break;
            case 1:  case MP_HOME: E.cx = 0; break;
            case 5:  case MP_END:  E.cx = E.line[E.cy].len; break;
            case MP_LEFT:
                if (E.cx > 0) {
                    E.cx = mp_left_of(&E.line[E.cy], E.cx);
                } else if (E.cy > 0) {
                    E.cy--;
                    E.cx = E.line[E.cy].len;
                }
                break;
            case MP_RIGHT:
                if (E.cx < E.line[E.cy].len) {
                    E.cx = mp_right_of(&E.line[E.cy], E.cx);
                } else if (E.cy + 1 < E.nlines) {
                    E.cy++;
                    E.cx = 0;
                }
                break;
            case MP_UP:
                if (E.cy > 0) E.cy--;
                break;
            case MP_DOWN:
                if (E.cy + 1 < E.nlines) E.cy++;
                break;
            case MP_PGUP:
                E.cy = E.cy > (size_t) E.rows ? E.cy - (size_t) E.rows : 0;
                break;
            case MP_PGDN:
                E.cy += (size_t) E.rows;
                if (E.cy >= E.nlines) E.cy = E.nlines - 1;
                break;
            case MP_DEL:
                mp_delete();
                break;
            case 127: case 8:
                mp_backspace();
                mp_size();
                break;
            case '\r': case '\n':
                mp_insert_newline();
                break;
            case '\t':
                mp_insert_char('\t');
                break;
            default:
                // Printable ASCII and every UTF-8 byte. Control characters
                // other than the ones handled above are dropped rather than
                // inserted -- a stray escape sequence should not end up in the
                // file.
                if (c >= 32 && c < 1000)
                    mp_insert_char(c);
                break;
        }
        // Clamp after any movement: a shorter line must not leave the cursor
        // past its end, and the byte offset must land on a character boundary.
        if (E.cy >= E.nlines)
            E.cy = E.nlines - 1;
        if (E.cx > E.line[E.cy].len)
            E.cx = E.line[E.cy].len;
        while (E.cx > 0 && E.cx < E.line[E.cy].len &&
                mp_is_cont((unsigned char) E.line[E.cy].s[E.cx]))
            E.cx--;
    }

done:
    mp_raw_off();
    write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7);
    return 0;
}
