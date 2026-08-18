// Terminal capabilities for a natively-compiled program, read from the
// GUEST's terminfo database.
//
// WHY THIS FILE EXISTS
// --------------------
// zsh (and any other native program that wants to move a cursor) calls
// tgetent/tgetstr/tgetnum/tgetflag/tgoto/tputs. Those names were not in
// kernel/native_libc.h, and the governing rule of that header is that a name
// it does not rewrite reaches the HOST. Both ways that resolved were wrong:
//
//   * With native bash compiled in, the calls bound to bash's OWN bundled GNU
//     termcap (deps/bash/lib/termcap), which is force-included with the shim
//     and so reads /etc/termcap THROUGH the guest. Routed, but useless:
//     Debian and Alpine both dropped /etc/termcap decades ago and ship
//     terminfo instead, so every capability came back empty.
//   * With bash off, meson put -ltermcap (or -lcurses) on the link line and
//     the calls read the HOST's database -- macOS's on the Mac, and on iOS
//     nothing at all, because the device has no terminfo database.
//
// Either way the guest's own /usr/share/terminfo was never opened, and ZLE
// got no cursor-movement strings. The user-visible symptom was reported from
// a phone: backspace erased the character but the cursor moved FORWARD. With
// no `le` (cursor_left) and no `nd`, zle_refresh has nothing to move the
// cursor with, so an erase emits a bare space -- which advances the cursor --
// and a left-arrow emits nothing. It reproduces on the Mac too, whenever
// native bash is in the build (the default), for the first reason above.
//
// WHAT THIS DOES
// --------------
// Implements the termcap API directly over the guest's terminfo database,
// using the routed I/O in kernel/native_io.h. No host library is consulted
// and no host file is opened, so a device with no terminfo of its own is
// irrelevant -- the database that matters is the one inside the guest
// filesystem, which is where the terminal being driven is described.
//
// The alternative considered was to keep the host library and hand it a
// TERMCAP string built from the guest entry. It needs the same terminfo
// reader and the same capability table to build that string, then depends on
// a host library honouring TERMCAP -- more moving parts for strictly less:
// the host library still owns tputs/tgoto, and the iOS SDK's termcap stub is
// exactly the thing we are trying to stop trusting.
//
// SEARCH ORDER matches ncurses, so a user's setting wins the way it does
// under the guest's own shell: $TERMCAP (an entry, or a file when it begins
// with '/'), then $TERMINFO, $HOME/.terminfo, $TERMINFO_DIRS, then
// /etc/terminfo, /lib/terminfo and /usr/share/terminfo. A guest with no
// database at all makes tgetent return 0, which is what a failed lookup
// returned before -- zsh sets TERM_BAD and degrades exactly as it does today
// rather than crashing.
//
// WHAT IT IS NOT. This is not a curses port and does not try to be: no
// screen model, no soft labels, no extended (user-defined) capabilities from
// the terminfo tail section, and no padding output from tputs (see there).
//
// BOTH native shells come here. bash briefly did not: it kept its bundled GNU
// termcap under NATIVE_LIBC_OWN_TERMCAP, which had exactly the first failure
// above, and its two files are out of the build now (meson.build,
// deps/bash-shim/termcap.h). What stayed on bash's side is PC -- readline
// defines it in lib/readline/terminal.c, thread-local like everything else of
// bash's, and is the only code that writes it. Defining it here instead would
// impose one thread-locality on every native program, and zsh's view of these
// globals is not bash's.

#define NATIVE_LIBC_NO_REDIRECT

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "kernel/native_libc.h"

// The termcap two-letter code for each terminfo capability, in the index
// order the compiled terminfo format uses. Generated from ncurses 6.6's own
// exported boolcodes/numcodes/strcodes arrays rather than transcribed, so the
// pairing cannot drift from what the guest's tic produced. The order is
// append-only across ncurses releases, which is why a table built from 6.6
// reads files compiled by the guest's older 6.x.
//
// EXPORTED under ncurses's names, and not merely for tidiness: these three
// arrays are part of the termcap/terminfo interface, and zsh's zsh/termcap
// module walks them to enumerate $termcap (Src/Modules/termcap.c) when
// HAVE_BOOLCODES and friends are set -- which meson now sets, because the
// arrays exist here. Without them zsh falls back to a bundled ncurses-5-era
// list and $termcap comes up 13 capabilities short of what the guest's own
// zsh reports for the same terminal. One table serving both the parser and
// the enumeration is the point: zsh cannot list a capability this file
// could not resolve, or resolve one it would not list.
/* 44 capabilities, in terminfo bool index order. */
const char *const boolcodes[] = {
    "bw", "am", "xb", "xs", "xn", "eo", "gn", "hc", "km", "hs", "in", "da",
    "db", "mi", "ms", "os", "es", "xt", "hz", "ul", "xo", "nx", "5i", "HC",
    "NR", "NP", "ND", "cc", "ut", "hl", "YA", "YB", "YC", "YD", "YE", "YF",
    "YG", "bs", "ns", "nc", "MT", "NL", "pt", "xr",
    NULL,
};

/* 39 capabilities, in terminfo num index order. */
const char *const numcodes[] = {
    "co", "it", "li", "lm", "sg", "pb", "vt", "ws", "Nl", "lh", "lw", "ma",
    "MW", "Co", "pa", "NC", "Ya", "Yb", "Yc", "Yd", "Ye", "Yf", "Yg", "Yh",
    "Yi", "Yj", "Yk", "Yl", "Ym", "Yn", "BT", "Yo", "Yp", "ug", "dC", "dN",
    "dB", "dT", "kn",
    NULL,
};

/* 414 capabilities, in terminfo str index order. */
const char *const strcodes[] = {
    "bt", "bl", "cr", "cs", "ct", "cl", "ce", "cd", "ch", "CC", "cm", "do",
    "ho", "vi", "le", "CM", "ve", "nd", "ll", "up", "vs", "dc", "dl", "ds",
    "hd", "as", "mb", "md", "ti", "dm", "mh", "im", "mk", "mp", "mr", "so",
    "us", "ec", "ae", "me", "te", "ed", "ei", "se", "ue", "vb", "ff", "fs",
    "i1", "is", "i3", "if", "ic", "al", "ip", "kb", "ka", "kC", "kt", "kD",
    "kL", "kd", "kM", "kE", "kS", "k0", "k1", "k;", "k2", "k3", "k4", "k5",
    "k6", "k7", "k8", "k9", "kh", "kI", "kA", "kl", "kH", "kN", "kP", "kr",
    "kF", "kR", "kT", "ku", "ke", "ks", "l0", "l1", "la", "l2", "l3", "l4",
    "l5", "l6", "l7", "l8", "l9", "mo", "mm", "nw", "pc", "DC", "DL", "DO",
    "IC", "SF", "AL", "LE", "RI", "SR", "UP", "pk", "pl", "px", "ps", "pf",
    "po", "rp", "r1", "r2", "r3", "rf", "rc", "cv", "sc", "sf", "sr", "sa",
    "st", "wi", "ta", "ts", "uc", "hu", "iP", "K1", "K3", "K2", "K4", "K5",
    "pO", "rP", "ac", "pn", "kB", "SX", "RX", "SA", "RA", "XN", "XF", "eA",
    "LO", "LF", "@1", "@2", "@3", "@4", "@5", "@6", "@7", "@8", "@9", "@0",
    "%1", "%2", "%3", "%4", "%5", "%6", "%7", "%8", "%9", "%0", "&1", "&2",
    "&3", "&4", "&5", "&6", "&7", "&8", "&9", "&0", "*1", "*2", "*3", "*4",
    "*5", "*6", "*7", "*8", "*9", "*0", "#1", "#2", "#3", "#4", "%a", "%b",
    "%c", "%d", "%e", "%f", "%g", "%h", "%i", "%j", "!1", "!2", "!3", "RF",
    "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "FA", "FB", "FC",
    "FD", "FE", "FF", "FG", "FH", "FI", "FJ", "FK", "FL", "FM", "FN", "FO",
    "FP", "FQ", "FR", "FS", "FT", "FU", "FV", "FW", "FX", "FY", "FZ", "Fa",
    "Fb", "Fc", "Fd", "Fe", "Ff", "Fg", "Fh", "Fi", "Fj", "Fk", "Fl", "Fm",
    "Fn", "Fo", "Fp", "Fq", "Fr", "cb", "MC", "ML", "MR", "Lf", "SC", "DK",
    "RC", "CW", "WG", "HU", "DI", "QD", "TO", "PU", "fh", "PA", "WA", "u0",
    "u1", "u2", "u3", "u4", "u5", "u6", "u7", "u8", "u9", "op", "oc", "Ic",
    "Ip", "sp", "Sf", "Sb", "ZA", "ZB", "ZC", "ZD", "ZE", "ZF", "ZG", "ZH",
    "ZI", "ZJ", "ZK", "ZL", "ZM", "ZN", "ZO", "ZP", "ZQ", "ZR", "ZS", "ZT",
    "ZU", "ZV", "ZW", "ZX", "ZY", "ZZ", "Za", "Zb", "Zc", "Zd", "Ze", "Zf",
    "Zg", "Zh", "Zi", "Zj", "Zk", "Zl", "Zm", "Zn", "Zo", "Zp", "Zq", "Zr",
    "Zs", "Zt", "Zu", "Zv", "Zw", "Zx", "Zy", "Km", "Mi", "RQ", "Gm", "AF",
    "AB", "xl", "dv", "ci", "s0", "s1", "s2", "s3", "ML", "MT", "Xy", "Zz",
    "Yv", "Yw", "Yx", "Yy", "Yz", "YZ", "S1", "S2", "S3", "S4", "S5", "S6",
    "S7", "S8", "Xh", "Xl", "Xo", "Xr", "Xt", "Xv", "sA", "YI", "i2", "rs",
    "nl", "bc", "ko", "ma", "G2", "G3", "G1", "G4", "GR", "GL", "GU", "GD",
    "GH", "GV", "GC", "ml", "mu", "bx",
    NULL,
};

/* Less the NULL terminator. */
#define TC_NBOOL ((int) (sizeof(boolcodes) / sizeof(boolcodes[0])) - 1)
#define TC_NNUM  ((int) (sizeof(numcodes)  / sizeof(numcodes[0])) - 1)
#define TC_NSTR  ((int) (sizeof(strcodes)  / sizeof(strcodes[0])) - 1)

// A compiled terminfo entry is a few kilobytes; xterm-256color is under 4K.
// The cap is a sanity bound on a file we are about to trust, not a tuning
// knob -- anything larger is not an entry we compiled against.
#define TC_MAX_FILE (64 * 1024)
// Same idea for a termcap-format entry, which is one (continued) line.
#define TC_MAX_TEXT (32 * 1024)

// One capability, whatever it was parsed out of. Terminfo and termcap
// entries are read by different code and reduced to this, so lookup is one
// scan either way and tgetstr cannot behave differently depending on where
// the entry came from.
enum tc_kind { TC_BOOL, TC_NUM, TC_STR };

struct tc_cap {
    char code[2];
    enum tc_kind kind;
    int num;            // TC_NUM
    char *str;          // TC_STR, owned by the entry's string arena
};

// Room for the capabilities tc_postprocess synthesises, which are not in the
// file and so have nowhere in `arena` to live.
#define TC_SYNTH_SLOTS 8
#define TC_SYNTH_BYTES 512

struct tc_entry {
    struct tc_cap *caps;
    int ncaps;
    int maxcaps;
    char *arena;        // every str read from the entry points into this
    char synth[TC_SYNTH_BYTES];
    size_t synth_used;
};

// A bounded output buffer, so every expander below can only ever fill the
// caller's array and never run off it.
struct tc_out {
    char *buf;
    size_t cap;
    size_t len;
};

// PER THREAD, not per process. A native program is a function call on its
// guest task's thread (kernel/native.h), so two shells running at once are
// two threads inside one address space -- the trap kernel/bash_tls_*.py
// exists to close for bash. One shell's TERM must not become another's.
static __thread struct tc_entry *tc_cur;
// tgetstr's return when the caller passes no area, and -- separately, so one
// cannot eat the other -- tgoto's result. Same lifetime rule the host
// functions have: valid until the next call on this thread.
static __thread char tc_scratch[1024];
static __thread char tc_goto_buf[1024];

static void tc_free(struct tc_entry *e) {
    if (e == NULL)
        return;
    free(e->caps);
    free(e->arena);
    free(e);
}

// Codes that name the same capability, because ncurses builds disagree about
// two of them. reset_2string is "r2" in ncurses 6.0 and 6.6 and "rs" in the
// Debian 6.5 the test guest runs; init_3string is "i3" upstream and "i2"
// there. Measured, not guessed: diffing $termcap against the guest's own zsh
// over 2874 terminal descriptions turns up these two spellings and nothing
// else.
//
// The table above stays exactly as ncurses generated it -- hand-editing it to
// chase one distribution's patch is the drift that generating it exists to
// prevent -- and the other spelling is accepted here instead. So every
// capability RESOLVES under either name, and the disagreement survives only
// in which name ${(k)termcap} lists. Nothing in zsh reads an init or reset
// string by code; these are the two capabilities where that is safe to say.
static const char tc_aliases[][2][3] = {
    { "rs", "r2" },     // reset_2string
    { "i2", "i3" },     // init_3string
};

static const struct tc_cap *tc_find_exact(const char *id, enum tc_kind kind) {
    for (int i = 0; i < tc_cur->ncaps; i++) {
        const struct tc_cap *c = &tc_cur->caps[i];
        if (c->kind == kind && c->code[0] == id[0] && c->code[1] == id[1])
            return c;
    }
    return NULL;
}

static const struct tc_cap *tc_find(const char *id, enum tc_kind kind) {
    if (tc_cur == NULL || id == NULL || id[0] == '\0' || id[1] == '\0')
        return NULL;
    const struct tc_cap *c = tc_find_exact(id, kind);
    if (c != NULL)
        return c;
    for (size_t i = 0; i < sizeof(tc_aliases) / sizeof(tc_aliases[0]); i++) {
        for (int d = 0; d < 2; d++) {
            const char *from = tc_aliases[i][d], *to = tc_aliases[i][1 - d];
            if (from[0] == id[0] && from[1] == id[1])
                return tc_find_exact(to, kind);
        }
    }
    return NULL;
}

// --- reading a guest file ------------------------------------------------
//
// nlibc_* throughout: this is the whole point of the file. A plain open()
// here would read the Mac's /usr/share/terminfo, which is the bug.

static char *tc_slurp(const char *path, size_t *len_out) {
    int fd = nlibc_open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    struct stat st;
    if (nlibc_fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) ||
        st.st_size <= 0 || (size_t) st.st_size > TC_MAX_FILE) {
        nlibc_close(fd);
        return NULL;
    }
    size_t size = (size_t) st.st_size;
    char *buf = malloc(size + 1);
    if (buf == NULL) {
        nlibc_close(fd);
        return NULL;
    }
    size_t got = 0;
    while (got < size) {
        ssize_t n = nlibc_read(fd, buf + got, size - got);
        if (n <= 0)
            break;
        got += (size_t) n;
    }
    nlibc_close(fd);
    if (got != size) {
        free(buf);
        return NULL;
    }
    buf[size] = '\0';
    if (len_out != NULL)
        *len_out = size;
    return buf;
}

// --- compiled terminfo ---------------------------------------------------

static int tc_le16(const unsigned char *p) {
    int v = p[0] | (p[1] << 8);
    return v >= 0x8000 ? v - 0x10000 : v;   // stored two's complement
}

static long tc_le32(const unsigned char *p) {
    unsigned long v = (unsigned long) p[0] | ((unsigned long) p[1] << 8) |
                      ((unsigned long) p[2] << 16) | ((unsigned long) p[3] << 24);
    return v >= 0x80000000UL ? (long) v - 0x100000000L : (long) v;
}

// term(5): a six-short header, the name field, one byte per boolean, an
// alignment byte if the two together are odd, the numbers, one 16-bit offset
// per string, then the string table. Magic 0432 is the historical format with
// 16-bit numbers; 01036 is ncurses 6's, which widens the NUMBERS to 32 bits
// and leaves the string offsets 16-bit. An extended section may follow the
// string table; it holds user-defined capabilities, which have no termcap
// code by definition, so it is ignored rather than unsupported.
static struct tc_entry *tc_parse_terminfo(const char *buf, size_t len) {
    const unsigned char *p = (const unsigned char *) buf;
    if (len < 12)
        return NULL;
    int magic = tc_le16(p);
    int numsize;
    if (magic == 0432)
        numsize = 2;
    else if (magic == 01036)
        numsize = 4;
    else
        return NULL;

    int name_sz = tc_le16(p + 2);
    int nbool   = tc_le16(p + 4);
    int nnum    = tc_le16(p + 6);
    int nstr    = tc_le16(p + 8);
    int str_sz  = tc_le16(p + 10);
    if (name_sz < 0 || nbool < 0 || nnum < 0 || nstr < 0 || str_sz < 0)
        return NULL;

    size_t off = 12;
    size_t bools_at = off + (size_t) name_sz;
    size_t nums_at  = bools_at + (size_t) nbool;
    if ((name_sz + nbool) % 2 != 0)
        nums_at++;                                  // align the numbers
    size_t strs_at  = nums_at + (size_t) nnum * (size_t) numsize;
    size_t table_at = strs_at + (size_t) nstr * 2;
    size_t end      = table_at + (size_t) str_sz;
    if (end > len)
        return NULL;

    struct tc_entry *e = calloc(1, sizeof(*e));
    if (e == NULL)
        return NULL;
    e->maxcaps = nbool + nnum + nstr + TC_SYNTH_SLOTS;
    e->caps = calloc((size_t) e->maxcaps, sizeof(*e->caps));
    e->arena = malloc((size_t) str_sz + 1);
    if (e->caps == NULL || e->arena == NULL) {
        tc_free(e);
        return NULL;
    }
    memcpy(e->arena, buf + table_at, (size_t) str_sz);
    e->arena[str_sz] = '\0';

    for (int i = 0; i < nbool && i < TC_NBOOL; i++) {
        // 0 absent, 1 present, 2 cancelled. Only a present flag is a flag.
        if (p[bools_at + (size_t) i] != 1)
            continue;
        struct tc_cap *c = &e->caps[e->ncaps++];
        c->code[0] = boolcodes[i][0];
        c->code[1] = boolcodes[i][1];
        c->kind = TC_BOOL;
    }
    for (int i = 0; i < nnum && i < TC_NNUM; i++) {
        const unsigned char *q = p + nums_at + (size_t) i * (size_t) numsize;
        long v = numsize == 2 ? tc_le16(q) : tc_le32(q);
        if (v < 0)                                  // -1 absent, -2 cancelled
            continue;
        struct tc_cap *c = &e->caps[e->ncaps++];
        c->code[0] = numcodes[i][0];
        c->code[1] = numcodes[i][1];
        c->kind = TC_NUM;
        c->num = (int) v;
    }
    for (int i = 0; i < nstr && i < TC_NSTR; i++) {
        int o = tc_le16(p + strs_at + (size_t) i * 2);
        if (o < 0 || o >= str_sz)                   // -1 absent, -2 cancelled
            continue;
        struct tc_cap *c = &e->caps[e->ncaps++];
        c->code[0] = strcodes[i][0];
        c->code[1] = strcodes[i][1];
        c->kind = TC_STR;
        c->str = e->arena + o;
    }
    return e;
}

// --- ncurses's termcap compatibility layer -------------------------------
//
// A compiled terminfo entry is not quite what the termcap API is meant to
// report, and ncurses adjusts three things on the way out. Skipping them
// would leave the two shells describing the same terminal differently, and
// one of the three is a cursor-movement capability -- which is the whole
// subject of this file. Each was found by diffing $termcap between native zsh
// and the guest's own zsh over every terminal in the guest's database.

static void tc_tparm(struct tc_out *o, const char *fmt, long p1, long p2);

static char *tc_entry_str(struct tc_entry *e, const char *code) {
    for (int i = 0; i < e->ncaps; i++)
        if (e->caps[i].kind == TC_STR &&
            e->caps[i].code[0] == code[0] && e->caps[i].code[1] == code[1])
            return e->caps[i].str;
    return NULL;
}

static struct tc_cap *tc_entry_find(struct tc_entry *e, const char *code,
                                    enum tc_kind kind) {
    for (int i = 0; i < e->ncaps; i++)
        if (e->caps[i].kind == kind &&
            e->caps[i].code[0] == code[0] && e->caps[i].code[1] == code[1])
            return &e->caps[i];
    return NULL;
}

static void tc_entry_del(struct tc_entry *e, const char *code,
                         enum tc_kind kind) {
    struct tc_cap *c = tc_entry_find(e, code, kind);
    if (c == NULL)
        return;
    int i = (int) (c - e->caps);
    memmove(&e->caps[i], &e->caps[i + 1],
            (size_t) (e->ncaps - i - 1) * sizeof(*e->caps));
    e->ncaps--;
}

// Add or replace. Strings are copied into the entry's synth arena, since a
// synthesised value has no home in the file's string table.
static void tc_entry_set(struct tc_entry *e, const char *code,
                         enum tc_kind kind, int num, const char *str) {
    char *copy = NULL;
    if (kind == TC_STR) {
        size_t n = strlen(str) + 1;
        if (e->synth_used + n > sizeof(e->synth))
            return;
        copy = e->synth + e->synth_used;
        memcpy(copy, str, n);
        e->synth_used += n;
    }
    struct tc_cap *c = tc_entry_find(e, code, kind);
    if (c == NULL) {
        if (e->ncaps >= e->maxcaps)
            return;
        c = &e->caps[e->ncaps++];
        c->code[0] = code[0];
        c->code[1] = code[1];
        c->kind = kind;
    }
    c->num = num;
    c->str = copy;
}

// The obsolete termcap-only capabilities. terminfo does not carry them --
// ncurses calls them OTbs, OTbc, OTug and OTNL -- and derives them from the
// modern ones on the way out to a termcap caller. Deriving them here is not
// tidiness: zsh's init_term reads bc, and substitutes a bare "\b" for its
// TCBACKSPACE when it is missing, so a terminal that does NOT backspace with
// ^H would get one anyway.
//
// The rule for each is "derive when the modern capability is there, and
// otherwise leave whatever the file stored", which is what ncurses does and
// what the terminals in the guest's database show. Both halves matter:
// Debian's `ansi` entry stores OTbs set while its cub1 is \E[D, and the
// derived answer (no) is the right one; `8510` stores OTbs with no cub1 at
// all, and the stored answer (yes) is all there is.
static void tc_fix_obsolete(struct tc_entry *e) {
    // bs and bc, the pair that says how this terminal moves left: bs means
    // "^H does it", bc names the sequence when it does not.
    const char *le = tc_entry_str(e, "le");         // cub1
    if (le != NULL) {
        tc_entry_del(e, "bs", TC_BOOL);
        if (strcmp(le, "\b") == 0) {
            tc_entry_set(e, "bs", TC_BOOL, 0, NULL);
            // A stored bc STAYS. z29 and the h29 family back up with ^H and
            // still name a second sequence, and ncurses reports both.
        } else {
            tc_entry_del(e, "bc", TC_STR);
            tc_entry_set(e, "bc", TC_STR, 0, le);
        }
    }

    // ug, the magic-cookie width of an UNDERLINE, which follows the general
    // magic-cookie width sg when it is not given separately -- but only on a
    // terminal that can underline at all. adm5 and vt100-nav have the cookie
    // and no smul, and ncurses reports no ug for them.
    const struct tc_cap *sg = tc_entry_find(e, "sg", TC_NUM);
    if (sg != NULL && tc_entry_str(e, "us") != NULL &&
        tc_entry_find(e, "ug", TC_NUM) == NULL)
        tc_entry_set(e, "ug", TC_NUM, sg->num, NULL);

    // NL, "a linefeed is a newline", i.e. the terminal returns the carriage
    // on \n by itself. nel is the modern capability that says so.
    const char *nw = tc_entry_str(e, "nw");         // nel
    if (nw != NULL) {
        tc_entry_del(e, "NL", TC_BOOL);
        if (strcmp(nw, "\n") == 0)
            tc_entry_set(e, "NL", TC_BOOL, 0, NULL);
    }
}

// me, "turn every attribute off", which ncurses does not report verbatim.
// Two adjustments:
//
//  - it is taken from sgr with all nine parameters zero -- the terminal's own
//    statement of "no attributes" -- rather than from sgr0; and
//  - the part that resets the ALTERNATE CHARACTER SET is removed, because a
//    caller ending boldface does not expect the line-drawing state to change
//    underneath it. That is the entire reason ncurses carries a fixed-up sgr0.
//
// The charset reset takes three shapes, all of them in Debian's database: a
// suffix (vt100's ^O), a prefix (xterm's \E(B), and a PARAMETER inside the
// same CSI sequence (ansi's \E[0;10m against rmacs \E[10m). It is matched
// against rmacs VERBATIM -- vt220's rmacs carries a $<4> delay and so matches
// nothing, and ncurses leaves that terminal's \E(B in place too.
//
// The substitution is refused when the two disagree about more than the
// zeroes and semicolons in their parameters, which is the case where the sgr
// expansion is saying something sgr0 does not (the `linux` entry: sgr(0) is
// \E[0;10m where sgr0 is \E[m^O, and ncurses reports sgr0 unchanged). This
// last rule is a reading of ncurses's behaviour rather than a port of its
// code, so it was checked against every terminal in the guest's database
// rather than against the handful that prompted it.

// Remove "$<...>" delays. tputs drops them on output; a stored capability
// being compared against another must not differ by one.
static void tc_strip_pad(char *s) {
    char *w = s;
    for (char *r = s; *r != '\0'; ) {
        if (r[0] == '$' && r[1] == '<') {
            char *q = r + 2;
            while (*q == '.' || (*q >= '0' && *q <= '9') ||
                   *q == '*' || *q == '/')
                q++;
            if (*q == '>' && q > r + 2) {
                r = q + 1;
                continue;
            }
        }
        *w++ = *r++;
    }
    *w = '\0';
}

static void tc_strip_acs(char *buf, const char *ae) {
    if (ae == NULL || *ae == '\0')
        return;
    size_t n = strlen(ae), m = strlen(buf);
    if (m >= n && memcmp(buf + m - n, ae, n) == 0) {
        buf[m - n] = '\0';
    } else if (m >= n && memcmp(buf, ae, n) == 0) {
        memmove(buf, buf + n, m - n + 1);
    } else if (buf[0] == 033 && buf[1] == '[' &&
               ae[0] == 033 && ae[1] == '[' && n > 3) {
        // ";" followed by rmacs's parameters, e.g. ";10" out of "\E[10m".
        char needle[32];
        needle[0] = ';';
        size_t k = 1;
        for (size_t i = 2; i < n - 1 && k + 1 < sizeof(needle); i++)
            needle[k++] = ae[i];
        needle[k] = '\0';
        char *at = (k > 1) ? strstr(buf, needle) : NULL;
        if (at != NULL)
            memmove(at, at + k, strlen(at + k) + 1);
    }
}

// The comparison the substitution is gated on: equal once the parameter
// noise -- zeroes and separators, which is exactly what differs between
// "\E[m" and "\E[0m" -- is taken out of both.
static int tc_same_shape(const char *a, const char *b) {
    for (;;) {
        while (*a == '0' || *a == ';')
            a++;
        while (*b == '0' || *b == ';')
            b++;
        if (*a != *b)
            return 0;
        if (*a == '\0')
            return 1;
        a++;
        b++;
    }
}

static void tc_fix_sgr0(struct tc_entry *e) {
    const char *me = tc_entry_str(e, "me");
    const char *sgr = tc_entry_str(e, "sa");
    const char *ae = tc_entry_str(e, "ae");
    // No alternate character set means nothing to strip, and the whole point
    // of the substitution is the stripping -- so sgr0 stands. `cons25` and
    // `sun` are the terminals in the guest's database that land here, and
    // ncurses reports their sgr0 verbatim too.
    if (me == NULL || sgr == NULL || ae == NULL || *ae == '\0')
        return;

    char cand[256], ref[256];
    struct tc_out o = { cand, sizeof(cand), 0 };
    tc_tparm(&o, sgr, 0, 0);
    cand[o.len] = '\0';
    // A capability we mis-expanded must never replace one that works.
    if (o.len == 0 || cand[0] != 033)
        return;
    if (snprintf(ref, sizeof(ref), "%s", me) >= (int) sizeof(ref))
        return;

    tc_strip_pad(cand);
    tc_strip_pad(ref);
    tc_strip_acs(cand, ae);
    tc_strip_acs(ref, ae);
    if (tc_same_shape(cand, ref))
        tc_entry_set(e, "me", TC_STR, 0, cand);
}

// co and li. ncurses resolves the screen size at setupterm time rather than
// reporting what the entry says, in this order: the terminal's actual window
// size, then $LINES and $COLUMNS, then the entry, then 24x80. Terminals whose
// size is not fixed say nothing in the entry -- `linux` and `dumb` both leave
// out lines -- so without this a shell asking li gets no answer at all where
// the guest's own shell gets 24.
static void tc_fix_screensize(struct tc_entry *e) {
    int rows = 0, cols = 0;
    static const int fds[3] = { 1, 0, 2 };
    for (int i = 0; i < 3 && (rows == 0 || cols == 0); i++) {
        struct winsize ws;
        memset(&ws, 0, sizeof(ws));
        if (nlibc_ioctl(fds[i], TIOCGWINSZ, &ws) == 0) {
            if (ws.ws_row > 0 && ws.ws_col > 0) {
                rows = ws.ws_row;
                cols = ws.ws_col;
            }
        }
    }
    if (rows == 0 || cols == 0) {
        const char *l = nlibc_getenv("LINES"), *c = nlibc_getenv("COLUMNS");
        if (l != NULL && c != NULL) {
            int lv = (int) strtol(l, NULL, 10), cv = (int) strtol(c, NULL, 10);
            if (lv > 0 && cv > 0) {
                rows = lv;
                cols = cv;
            }
        }
    }
    if (rows > 0 && cols > 0) {
        tc_entry_set(e, "li", TC_NUM, rows, NULL);
        tc_entry_set(e, "co", TC_NUM, cols, NULL);
        return;
    }
    if (tc_entry_find(e, "li", TC_NUM) == NULL)
        tc_entry_set(e, "li", TC_NUM, 24, NULL);
    if (tc_entry_find(e, "co", TC_NUM) == NULL)
        tc_entry_set(e, "co", TC_NUM, 80, NULL);
}

// Only for entries read from terminfo. A $TERMCAP entry is already written in
// termcap's own terms -- it says bs and bc itself -- so translating it again
// would be second-guessing the user, which is the one thing an override must
// never do.
static void tc_postprocess(struct tc_entry *e) {
    tc_fix_obsolete(e);
    tc_fix_sgr0(e);
    tc_fix_screensize(e);
}

// --- termcap-format text -------------------------------------------------
//
// Only reached through $TERMCAP or a termcap FILE. It exists because the
// classic override has to keep working: a user who sets TERMCAP by hand is
// telling us about a terminal the database does not describe, and that has to
// beat anything we find on disk.

static int tc_unescape(const char *src, size_t n, char *dst) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        char ch = src[i];
        if (ch == '^' && i + 1 < n) {
            char c = src[++i];
            dst[o++] = (char) (c == '?' ? 0177 : (c & 0x1f));
        } else if (ch == '\\' && i + 1 < n) {
            char c = src[++i];
            switch (c) {
                case 'E': case 'e': dst[o++] = 033; break;
                case 'n': dst[o++] = '\n'; break;
                case 'r': dst[o++] = '\r'; break;
                case 't': dst[o++] = '\t'; break;
                case 'b': dst[o++] = '\b'; break;
                case 'f': dst[o++] = '\f'; break;
                case 's': dst[o++] = ' '; break;
                case '0': case '1': case '2': case '3':
                case '4': case '5': case '6': case '7': {
                    int v = c - '0';
                    for (int k = 0; k < 2 && i + 1 < n &&
                                    src[i + 1] >= '0' && src[i + 1] <= '7'; k++)
                        v = v * 8 + (src[++i] - '0');
                    dst[o++] = (char) (v == 0 ? 0200 : v);  // \0 means \200
                    break;
                }
                default: dst[o++] = c; break;
            }
        } else {
            dst[o++] = ch;
        }
    }
    dst[o] = '\0';
    return (int) o;
}

// True when `entry`'s name field (everything before the first ':', split on
// '|') contains `name`.
static int tc_names_match(const char *entry, const char *name) {
    size_t want = strlen(name);
    const char *p = entry;
    while (*p != '\0' && *p != ':') {
        const char *q = p;
        while (*q != '\0' && *q != ':' && *q != '|')
            q++;
        if ((size_t) (q - p) == want && strncmp(p, name, want) == 0)
            return 1;
        p = (*q == '|') ? q + 1 : q;
    }
    return 0;
}

static struct tc_entry *tc_lookup_file(const char *path, const char *name,
                                       int depth);

// Parse one termcap entry (the whole ':'-separated line) into an entry.
// `path` and `depth` carry the tc= continuation, which can only be followed
// when the entry came from a file.
static struct tc_entry *tc_parse_termcap(const char *text, const char *path,
                                         int depth) {
    size_t len = strlen(text);
    struct tc_entry *e = calloc(1, sizeof(*e));
    if (e == NULL)
        return NULL;
    // One cap per ':' is a ceiling, and no unescaped value is longer than its
    // source, so both allocations are bounds rather than estimates.
    size_t maxcaps = 2;
    for (size_t i = 0; i < len; i++)
        if (text[i] == ':')
            maxcaps++;
    e->maxcaps = (int) maxcaps;
    e->caps = calloc(maxcaps, sizeof(*e->caps));
    e->arena = malloc(len + 1);
    if (e->caps == NULL || e->arena == NULL) {
        tc_free(e);
        return NULL;
    }
    char *arena = e->arena;

    const char *p = strchr(text, ':');
    while (p != NULL) {
        p++;
        const char *q = strchr(p, ':');
        size_t flen = (q == NULL) ? strlen(p) : (size_t) (q - p);
        if (flen >= 2) {
            char code[2] = { p[0], p[1] };
            if (flen >= 3 && p[2] == '=') {
                if (code[0] == 't' && code[1] == 'c') {
                    // Continuation: everything the referenced entry has that
                    // this one has not already said. Bounded, because a cycle
                    // in a hand-written /etc/termcap must not hang a shell.
                    char ref[128];
                    size_t rlen = flen - 3;
                    if (path != NULL && depth < 8 && rlen < sizeof(ref)) {
                        memcpy(ref, p + 3, rlen);
                        ref[rlen] = '\0';
                        struct tc_entry *base = tc_lookup_file(path, ref, depth + 1);
                        if (base != NULL) {
                            for (int i = 0; i < base->ncaps &&
                                            (size_t) e->ncaps < maxcaps; i++) {
                                struct tc_cap *b = &base->caps[i];
                                int seen = 0;
                                for (int j = 0; j < e->ncaps; j++)
                                    if (e->caps[j].kind == b->kind &&
                                        e->caps[j].code[0] == b->code[0] &&
                                        e->caps[j].code[1] == b->code[1])
                                        seen = 1;
                                if (seen)
                                    continue;
                                struct tc_cap *c = &e->caps[e->ncaps++];
                                *c = *b;
                                if (b->kind == TC_STR) {
                                    // Copy out of the base entry's arena,
                                    // which is about to be freed.
                                    size_t n = strlen(b->str);
                                    if ((size_t) (arena - e->arena) + n + 1 <= len + 1) {
                                        memcpy(arena, b->str, n + 1);
                                        c->str = arena;
                                        arena += n + 1;
                                    } else {
                                        e->ncaps--;
                                    }
                                }
                            }
                            tc_free(base);
                        }
                    }
                } else {
                    struct tc_cap *c = &e->caps[e->ncaps++];
                    c->code[0] = code[0];
                    c->code[1] = code[1];
                    c->kind = TC_STR;
                    c->str = arena;
                    arena += tc_unescape(p + 3, flen - 3, arena) + 1;
                }
            } else if (flen >= 3 && p[2] == '#') {
                char numbuf[32];
                size_t n = flen - 3;
                if (n < sizeof(numbuf)) {
                    memcpy(numbuf, p + 3, n);
                    numbuf[n] = '\0';
                    struct tc_cap *c = &e->caps[e->ncaps++];
                    c->code[0] = code[0];
                    c->code[1] = code[1];
                    c->kind = TC_NUM;
                    c->num = (int) strtol(numbuf, NULL, 0);
                }
            } else if (flen == 2) {
                struct tc_cap *c = &e->caps[e->ncaps++];
                c->code[0] = code[0];
                c->code[1] = code[1];
                c->kind = TC_BOOL;
            }
            // "xx@" cancels a capability. Recording nothing is the right
            // answer for a standalone entry; for a tc= base it would need to
            // shadow, which the loop above already does by insertion order.
        }
        p = q;
    }
    return e;
}

// Find `name` in a termcap-format file, joining backslash continuations.
static struct tc_entry *tc_lookup_file(const char *path, const char *name,
                                       int depth) {
    size_t len = 0;
    char *text = tc_slurp(path, &len);
    if (text == NULL)
        return NULL;
    struct tc_entry *found = NULL;
    char *line = text;
    while (line != NULL && *line != '\0') {
        char *nl = strchr(line, '\n');
        if (nl != NULL)
            *nl = '\0';
        // A continued line ends in a backslash; splice the next one on.
        char *end = line + strlen(line);
        while (end > line && end[-1] == '\\' && nl != NULL) {
            char *next = nl + 1;
            char *nnl = strchr(next, '\n');
            if (nnl != NULL)
                *nnl = '\0';
            memmove(end - 1, next, strlen(next) + 1);
            end = end - 1 + strlen(end - 1);
            nl = nnl;
            if (nl == NULL)
                break;
        }
        if (*line != '#' && *line != '\0' && *line != ' ' && *line != '\t' &&
            tc_names_match(line, name)) {
            found = tc_parse_termcap(line, path, depth);
            break;
        }
        line = (nl == NULL) ? NULL : nl + 1;
    }
    free(text);
    return found;
}

// --- finding the entry ---------------------------------------------------

static struct tc_entry *tc_try_terminfo_dir(const char *dir, const char *name) {
    if (dir == NULL || *dir == '\0')
        return NULL;
    char path[PATH_MAX];
    // Two layouts, both produced by tic: a directory named for the first
    // letter (Linux) and one named for its hex value (what macOS ships, and
    // what tic writes when the filesystem is case-insensitive).
    const char *forms[2] = { "%s/%c/%s", "%s/%02x/%s" };
    for (int f = 0; f < 2; f++) {
        int n = f == 0
            ? snprintf(path, sizeof(path), forms[0], dir, name[0], name)
            : snprintf(path, sizeof(path), forms[1], dir,
                       (unsigned char) name[0], name);
        if (n <= 0 || (size_t) n >= sizeof(path))
            continue;
        size_t len = 0;
        char *buf = tc_slurp(path, &len);
        if (buf == NULL)
            continue;
        struct tc_entry *e = tc_parse_terminfo(buf, len);
        free(buf);
        if (e != NULL) {
            tc_postprocess(e);
            return e;
        }
    }
    return NULL;
}

static struct tc_entry *tc_load(const char *name) {
    if (name == NULL || *name == '\0' || strchr(name, '/') != NULL)
        return NULL;

    // $TERMCAP first, so a hand-set entry beats the database -- the classic
    // override, and the only way to describe a terminal the guest has no
    // entry for. A value starting with '/' names a termcap FILE instead.
    const char *tcenv = nlibc_getenv("TERMCAP");
    if (tcenv != NULL && *tcenv != '\0' && strlen(tcenv) < TC_MAX_TEXT) {
        if (*tcenv == '/') {
            struct tc_entry *e = tc_lookup_file(tcenv, name, 0);
            if (e != NULL)
                return e;
        } else if (tc_names_match(tcenv, name)) {
            struct tc_entry *e = tc_parse_termcap(tcenv, NULL, 0);
            if (e != NULL)
                return e;
        }
    }

    struct tc_entry *e = tc_try_terminfo_dir(nlibc_getenv("TERMINFO"), name);
    if (e != NULL)
        return e;

    const char *home = nlibc_getenv("HOME");
    if (home != NULL && *home != '\0') {
        char dir[PATH_MAX];
        int n = snprintf(dir, sizeof(dir), "%s/.terminfo", home);
        if (n > 0 && (size_t) n < sizeof(dir)) {
            e = tc_try_terminfo_dir(dir, name);
            if (e != NULL)
                return e;
        }
    }

    static const char *const defaults[] = {
        "/etc/terminfo", "/lib/terminfo", "/usr/share/terminfo", NULL
    };

    // $TERMINFO_DIRS is a colon list in which an EMPTY element means "the
    // compiled-in default list here", which is how a user prepends a
    // directory without losing the system one.
    const char *dirs = nlibc_getenv("TERMINFO_DIRS");
    if (dirs != NULL && *dirs != '\0') {
        const char *p = dirs;
        for (;;) {
            const char *q = strchr(p, ':');
            size_t n = (q == NULL) ? strlen(p) : (size_t) (q - p);
            if (n == 0) {
                for (int i = 0; defaults[i] != NULL; i++) {
                    e = tc_try_terminfo_dir(defaults[i], name);
                    if (e != NULL)
                        return e;
                }
            } else if (n < PATH_MAX) {
                char dir[PATH_MAX];
                memcpy(dir, p, n);
                dir[n] = '\0';
                e = tc_try_terminfo_dir(dir, name);
                if (e != NULL)
                    return e;
            }
            if (q == NULL)
                break;
            p = q + 1;
        }
    }

    for (int i = 0; defaults[i] != NULL; i++) {
        e = tc_try_terminfo_dir(defaults[i], name);
        if (e != NULL)
            return e;
    }

    // Last: a guest that really does ship /etc/termcap and no terminfo. Rare
    // now, free to support given the parser above already exists.
    static const char *const tcfiles[] = {
        "/etc/termcap", "/usr/share/misc/termcap", NULL
    };
    for (int i = 0; tcfiles[i] != NULL; i++) {
        e = tc_lookup_file(tcfiles[i], name, 0);
        if (e != NULL)
            return e;
    }
    return NULL;
}

// --- the termcap API -----------------------------------------------------

// Returns 1 for a terminal we found, 0 for one we did not. `bp`, the caller's
// 2048-byte buffer for the raw entry text, is ignored exactly as ncurses
// ignores it: nothing in-tree reads it back, and a terminfo entry has no
// termcap text to put there. zsh's config.h has TGETENT_ACCEPTS_NULL, so it
// passes NULL and never looks.
int nlibc_tgetent(char *bp, char *name) {
    (void) bp;
    struct tc_entry *e = tc_load(name);
    tc_free(tc_cur);
    tc_cur = e;
    return e != NULL ? 1 : 0;
}

int nlibc_tgetnum(char *id) {
    const struct tc_cap *c = tc_find(id, TC_NUM);
    return c != NULL ? c->num : -1;
}

int nlibc_tgetflag(char *id) {
    return tc_find(id, TC_BOOL) != NULL ? 1 : 0;
}

// The historical contract: when `area` is given, the value is copied there
// and *area is advanced past it, so a caller can pack several capabilities
// into one buffer. zsh (Src/init.c) passes a pointer to a 1024-byte local and
// then reads the RETURN value, which both behaviours satisfy.
char *nlibc_tgetstr(char *id, char **area) {
    const struct tc_cap *c = tc_find(id, TC_STR);
    if (c == NULL)
        return NULL;
    size_t n = strlen(c->str);
    // The caller's area has no length with it -- that is the interface, and
    // callers size it by the historical 1024-byte entry limit (zsh's
    // init_term passes a 1024-byte local). A capability longer than that is a
    // corrupt or hostile terminfo file rather than a terminal, and the guest
    // owns the file, so refusing is the only answer that is not a write past
    // somebody's buffer.
    if (n >= sizeof(tc_scratch))
        return NULL;
    if (area != NULL && *area != NULL) {
        char *dst = *area;
        memcpy(dst, c->str, n + 1);
        *area = dst + n + 1;
        return dst;
    }
    memcpy(tc_scratch, c->str, n + 1);
    return tc_scratch;
}

// --- parameter expansion -------------------------------------------------

// tgetstr hands back the TERMINFO string, unconverted -- so a parameterised
// capability arrives in terminfo's stack language (`\E[%p1%dD`), not
// termcap's (`\E[%dD`). ncurses's tgoto has the same problem and solves it
// the same way: look at the string, and run whichever expander it is written
// in. A $TERMCAP-sourced entry is the case that still needs the old one.
static int tc_is_termcap_style(const char *s) {
    for (const char *p = s; *p != '\0'; p++) {
        if (*p != '%')
            continue;
        switch (*++p) {
            case '\0': return 1;
            // termcap's own set. Anything else -- p, {, ?, ', g, P -- is
            // terminfo.
            case '%': case 'd': case '2': case '3': case '.': case '+':
            case '>': case 'r': case 'i': case 'n': case 'B': case 'D':
                break;
            default:
                return 0;
        }
    }
    return 1;
}

static void tc_put(struct tc_out *o, int ch) {
    if (o->len + 1 < o->cap)
        o->buf[o->len++] = (char) ch;
}

static void tc_puts_fmt(struct tc_out *o, const char *fmt, long v) {
    char tmp[64];
    int n = snprintf(tmp, sizeof(tmp), fmt, v);
    for (int i = 0; i < n && i < (int) sizeof(tmp); i++)
        tc_put(o, (unsigned char) tmp[i]);
}

// terminfo's %-language, the arithmetic-and-conditionals subset that real
// entries use. Everything on the stack is an integer: the string operands
// (%s, %l) only appear in capabilities that take a string parameter -- the
// printer and status-line ones -- and nothing here calls those.
#define TC_STACK 32
static void tc_tparm(struct tc_out *o, const char *fmt, long p1, long p2) {
    long params[9] = { p1, p2, 0, 0, 0, 0, 0, 0, 0 };
    long stack[TC_STACK];
    long vars[26];
    int sp = 0;
    memset(vars, 0, sizeof(vars));

    for (const char *p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            tc_put(o, (unsigned char) *p);
            continue;
        }
        p++;
        switch (*p) {
            case '\0':
                return;
            case '%':
                tc_put(o, '%');
                break;
            case 'p': {
                int idx = p[1] - '1';
                if (idx >= 0 && idx < 9 && sp < TC_STACK)
                    stack[sp++] = params[idx];
                p++;
                break;
            }
            case '\'':                       // %'c' -- a character constant
                if (p[1] != '\0' && sp < TC_STACK) {
                    stack[sp++] = (unsigned char) p[1];
                    p += (p[2] == '\'') ? 2 : 1;
                }
                break;
            case '{': {                      // %{nn} -- an integer constant
                long v = 0;
                int neg = 0;
                p++;
                if (*p == '-') { neg = 1; p++; }
                while (*p >= '0' && *p <= '9')
                    v = v * 10 + (*p++ - '0');
                if (*p != '}')
                    p--;
                if (sp < TC_STACK)
                    stack[sp++] = neg ? -v : v;
                break;
            }
            case 'P':                        // %Pa .. %Pz, %PA .. %PZ
                if (p[1] >= 'a' && p[1] <= 'z' && sp > 0)
                    vars[p[1] - 'a'] = stack[--sp];
                else if (p[1] >= 'A' && p[1] <= 'Z' && sp > 0)
                    vars[p[1] - 'A'] = stack[--sp];
                p++;
                break;
            case 'g':
                if (p[1] >= 'a' && p[1] <= 'z' && sp < TC_STACK)
                    stack[sp++] = vars[p[1] - 'a'];
                else if (p[1] >= 'A' && p[1] <= 'Z' && sp < TC_STACK)
                    stack[sp++] = vars[p[1] - 'A'];
                p++;
                break;
            case 'i':                        // one-based row and column
                params[0]++;
                params[1]++;
                break;
            case 'c':
                if (sp > 0)
                    tc_put(o, (int) (stack[--sp] & 0xff));
                break;
            case 'l':                        // length of a string parameter
                if (sp > 0)
                    stack[sp - 1] = 0;       // no string parameters here
                break;
            case 's':                        // ditto: nothing to print
                if (sp > 0)
                    sp--;
                break;
            case 'd': case 'o': case 'x': case 'X': case ':':
            case '0': case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8': case '9':
            case '-': case '+': case '#': case ' ': case '.': {
                // %[[:]flags][width[.precision]]conv, and the ambiguity that
                // makes the ':' flag exist: %+ and %- are also arithmetic.
                // Arithmetic is what they are unless a conversion follows.
                const char *q = p;
                char spec[24];
                size_t n = 0;
                spec[n++] = '%';
                if (*q == ':')
                    q++;
                while (*q != '\0' && n + 2 < sizeof(spec) &&
                       (strchr("-+# .0123456789", *q) != NULL))
                    spec[n++] = *q++;
                if (*q == 'd' || *q == 'o' || *q == 'x' || *q == 'X') {
                    spec[n++] = 'l';
                    spec[n++] = *q;
                    spec[n] = '\0';
                    if (sp > 0)
                        tc_puts_fmt(o, spec, stack[--sp]);
                    else
                        tc_puts_fmt(o, spec, 0L);
                    p = q;
                    break;
                }
                // Not a conversion after all: fall through to arithmetic.
                if (*p == '+' || *p == '-') {
                    if (sp >= 2) {
                        long b = stack[--sp], a = stack[--sp];
                        stack[sp++] = (*p == '+') ? a + b : a - b;
                    }
                    break;
                }
                tc_put(o, (unsigned char) *p);
                break;
            }
            case '*': case '/': case 'm': case '&': case '|': case '^':
            case '=': case '<': case '>': case 'A': case 'O': {
                if (sp < 2)
                    break;
                long b = stack[--sp], a = stack[--sp];
                long r = 0;
                switch (*p) {
                    case '*': r = a * b; break;
                    case '/': r = (b != 0) ? a / b : 0; break;
                    case 'm': r = (b != 0) ? a % b : 0; break;
                    case '&': r = a & b; break;
                    case '|': r = a | b; break;
                    case '^': r = a ^ b; break;
                    case '=': r = (a == b); break;
                    case '<': r = (a < b); break;
                    case '>': r = (a > b); break;
                    case 'A': r = (a && b); break;
                    case 'O': r = (a || b); break;
                }
                stack[sp++] = r;
                break;
            }
            case '!':
                if (sp > 0)
                    stack[sp - 1] = !stack[sp - 1];
                break;
            case '~':
                if (sp > 0)
                    stack[sp - 1] = ~stack[sp - 1];
                break;
            case '?':                        // if
                break;
            case 't': {                      // then
                long cond = (sp > 0) ? stack[--sp] : 0;
                if (cond)
                    break;
                // Skip to the matching %e or %;, minding nested conditionals.
                int depth = 0;
                for (p++; *p != '\0'; p++) {
                    if (*p != '%')
                        continue;
                    if (p[1] == '?')
                        depth++;
                    else if (p[1] == ';') {
                        if (depth == 0) { p++; break; }
                        depth--;
                    } else if (p[1] == 'e' && depth == 0) {
                        p++;
                        break;
                    }
                    if (p[1] != '\0')
                        p++;
                }
                break;
            }
            case 'e': {                      // else, reached only after a
                int depth = 0;               // taken then-branch: skip it
                for (p++; *p != '\0'; p++) {
                    if (*p != '%')
                        continue;
                    if (p[1] == '?')
                        depth++;
                    else if (p[1] == ';') {
                        if (depth == 0) { p++; break; }
                        depth--;
                    }
                    if (p[1] != '\0')
                        p++;
                }
                break;
            }
            case ';':                        // endif
                break;
            default:
                tc_put(o, (unsigned char) *p);
                break;
        }
    }
}

// termcap's older, positional language, for entries that came from $TERMCAP.
static void tc_tgoto_termcap(struct tc_out *o, const char *fmt,
                             int col, int row) {
    int args[2] = { row, col };              // termcap emits row first
    int argi = 0;
    for (const char *p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            tc_put(o, (unsigned char) *p);
            continue;
        }
        p++;
        int v = (argi < 2) ? args[argi] : 0;
        switch (*p) {
            case '\0': return;
            case '%': tc_put(o, '%'); break;
            case 'd': tc_puts_fmt(o, "%ld", v); argi++; break;
            case '2': tc_puts_fmt(o, "%02ld", v % 100); argi++; break;
            case '3': tc_puts_fmt(o, "%03ld", v % 1000); argi++; break;
            case '.': tc_put(o, v ? v : 0200); argi++; break;
            case '+':
                if (p[1] != '\0') {
                    tc_put(o, (v + (unsigned char) p[1]) & 0xff);
                    p++;
                    argi++;
                }
                break;
            case '>':                        // %>xy: if arg > x, add y
                if (p[1] != '\0' && p[2] != '\0') {
                    if (v > (unsigned char) p[1])
                        args[argi] = v + (unsigned char) p[2];
                    p += 2;
                }
                break;
            case 'i': args[0]++; args[1]++; break;
            case 'r': {                      // column first after all
                int t = args[0]; args[0] = args[1]; args[1] = t;
                break;
            }
            case 'n': args[0] ^= 0140; args[1] ^= 0140; break;
            case 'B':
                args[argi] = (v / 10) * 16 + (v % 10);
                break;
            case 'D':
                args[argi] = v - 2 * (v % 16);
                break;
            default:
                tc_put(o, (unsigned char) *p);
                break;
        }
    }
}

// tgoto(cap, col, row). The terminfo order is (row, col), which is why the
// tparm call below looks reversed -- ncurses does exactly this.
char *nlibc_tgoto(char *cap, int col, int row) {
    if (cap == NULL)
        return NULL;
    struct tc_out o = { tc_goto_buf, sizeof(tc_goto_buf), 0 };
    if (tc_is_termcap_style(cap))
        tc_tgoto_termcap(&o, cap, col, row);
    else
        tc_tparm(&o, cap, row, col);
    o.buf[o.len] = '\0';
    return o.buf;
}

// Send a capability. `affcnt` is the number of lines affected, which only
// matters for padding.
//
// PADDING IS DROPPED, deliberately. A `$<5>` delay exists to give a physical
// terminal time to finish scrolling before more bytes arrive at 9600 baud;
// AOK's terminal is a pty read by a view in the same process, so the delay
// buys nothing and the pad characters would be written into the user's
// output. ncurses itself skips padding when it has no ospeed, and PC/ospeed
// are not plumbed here: zsh declares them and never assigns them
// (deps/zsh-shim/termcap.h), and bash's readline assigns PC and never reads
// it back (deps/bash-shim/termcap.h), which is only harmless because this
// function ignores it.
int nlibc_tputs(char *str, int affcnt, int (*putc_fn)(int)) {
    (void) affcnt;
    if (str == NULL || putc_fn == NULL)
        return -1;
    for (const char *p = str; *p != '\0'; p++) {
        if (p[0] == '$' && p[1] == '<') {
            const char *q = p + 2;
            while (*q == '.' || (*q >= '0' && *q <= '9') ||
                   *q == '*' || *q == '/')
                q++;
            if (*q == '>' && q > p + 2) {    // a real delay, not literal "$<"
                p = q;
                continue;
            }
        }
        putc_fn((unsigned char) *p);
    }
    return 0;
}
