/* Driver for tools/difftest-native-getopt.sh. See that script for why.
 *
 * One case = an optstring, an argv, and which of the three entry points to
 * use. The case is run twice -- once through the host's getopt, once through
 * the shim's -- and the two traces are compared step by step. A trace records
 * everything a caller can observe: the return value, optarg, optind, optopt,
 * and, because getopt_long permutes, the argv left behind at the end.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>

/* The shim's, from the region lifted out of kernel/native_libc.c. */
int nlibc_getopt(int, char *const [], const char *);
int nlibc_getopt_long(int, char *const [], const char *,
        const struct option *, int *);
int nlibc_getopt_long_only(int, char *const [], const char *,
        const struct option *, int *);
char **nlibc_optargp(void);
int *nlibc_optindp(void);
int *nlibc_opterrp(void);
int *nlibc_optoptp(void);
int *nlibc_optresetp(void);

enum entry { E_SHORT, E_LONG, E_LONG_ONLY };

#define MAXSTEPS 64
#define MAXARGV  24

struct step {
    int ret;
    int optind;
    int optopt;
    int longindex;
    int have_arg;               /* optarg != NULL */
    char arg[128];
};

struct trace {
    int nsteps;
    struct step steps[MAXSTEPS];
    int argc;
    char argv[MAXARGV][64];     /* argv after the parse, permutation and all */
    char err[4096];             /* what opterr printed */
};

struct testcase {
    const char *name;
    const char *optstring;
    const char *argv[MAXARGV];
    enum entry entry;
    const struct option *longopts;
    int reset_at;               /* after this many steps, restart the parse */
    int reset_optind;           /* optind to restart from (0 = "use 1") */
};

/* ------------------------------------------------------------------ running */

static int longflag_host, longflag_shim;

static void run(const struct testcase *tc, int use_shim, int with_err,
        struct trace *tr) {
    char *argv[MAXARGV + 1];
    char storage[MAXARGV][64];
    int argc = 0;
    for (; tc->argv[argc] != NULL; argc++) {
        snprintf(storage[argc], sizeof(storage[argc]), "%s", tc->argv[argc]);
        argv[argc] = storage[argc];
    }
    argv[argc] = NULL;

    int *flag = use_shim ? &longflag_shim : &longflag_host;
    *flag = 0;

    /* Capture whatever opterr prints, on the same fd 2 both sides use. */
    int saved_fd = -1, cap_fd = -1;
    char capname[256];
    tr->err[0] = '\0';
    if (with_err) {
        snprintf(capname, sizeof(capname), "%s/getopt-difftest-err.%d",
                getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int) getpid());
        cap_fd = open(capname, O_RDWR | O_CREAT | O_TRUNC, 0600);
        if (cap_fd >= 0) {
            fflush(stderr);
            saved_fd = dup(2);
            dup2(cap_fd, 2);
        }
    }

    if (use_shim) {
        *nlibc_optresetp() = 1;
        *nlibc_optindp() = 1;
        *nlibc_opterrp() = with_err;
        *nlibc_optoptp() = '?';
        *nlibc_optargp() = NULL;
    } else {
        optreset = 1;
        optind = 1;
        opterr = with_err;
        optopt = '?';
        optarg = NULL;
    }

    tr->nsteps = 0;
    for (int i = 0; i < MAXSTEPS; i++) {
        if (tc->reset_at > 0 && i == tc->reset_at) {
            /* The BSD restart: optreset plus a new optind. */
            if (use_shim) {
                *nlibc_optresetp() = 1;
                *nlibc_optindp() = tc->reset_optind ? tc->reset_optind : 1;
            } else {
                optreset = 1;
                optind = tc->reset_optind ? tc->reset_optind : 1;
            }
        }
        int longindex = -1;
        int r;
        if (use_shim) {
            switch (tc->entry) {
                case E_SHORT:
                    r = nlibc_getopt(argc, argv, tc->optstring); break;
                case E_LONG:
                    r = nlibc_getopt_long(argc, argv, tc->optstring,
                            tc->longopts, &longindex); break;
                default:
                    r = nlibc_getopt_long_only(argc, argv, tc->optstring,
                            tc->longopts, &longindex); break;
            }
        } else {
            switch (tc->entry) {
                case E_SHORT:
                    r = getopt(argc, argv, tc->optstring); break;
                case E_LONG:
                    r = getopt_long(argc, argv, tc->optstring,
                            tc->longopts, &longindex); break;
                default:
                    r = getopt_long_only(argc, argv, tc->optstring,
                            tc->longopts, &longindex); break;
            }
        }
        struct step *st = &tr->steps[tr->nsteps++];
        st->ret = r;
        st->longindex = longindex;
        if (use_shim) {
            st->optind = *nlibc_optindp();
            st->optopt = *nlibc_optoptp();
            char *a = *nlibc_optargp();
            st->have_arg = a != NULL;
            snprintf(st->arg, sizeof(st->arg), "%s", a ? a : "");
        } else {
            st->optind = optind;
            st->optopt = optopt;
            st->have_arg = optarg != NULL;
            snprintf(st->arg, sizeof(st->arg), "%s", optarg ? optarg : "");
        }
        if (r == -1)
            break;
    }

    if (with_err && cap_fd >= 0) {
        fflush(stderr);
        dup2(saved_fd, 2);
        close(saved_fd);
        lseek(cap_fd, 0, SEEK_SET);
        ssize_t n = read(cap_fd, tr->err, sizeof(tr->err) - 1);
        tr->err[n > 0 ? n : 0] = '\0';
        close(cap_fd);
        unlink(capname);
    }

    tr->argc = argc;
    for (int i = 0; i < argc; i++)
        snprintf(tr->argv[i], sizeof(tr->argv[i]), "%s", argv[i]);
    /* The flag pointer a long option may have written. */
    if (tr->nsteps < MAXSTEPS) {
        tr->steps[tr->nsteps].ret = *flag;
        tr->steps[tr->nsteps].optind = -12345;   /* sentinel row */
        tr->steps[tr->nsteps].optopt = -1;
        tr->steps[tr->nsteps].longindex = -1;
        tr->steps[tr->nsteps].have_arg = 0;
        tr->steps[tr->nsteps].arg[0] = '\0';
        tr->nsteps++;
    }
}

/* ---------------------------------------------------------------- compare */

static int trace_diff(const struct trace *a, const struct trace *b,
        char *why, size_t whysz) {
    if (a->nsteps != b->nsteps) {
        snprintf(why, whysz, "step count %d vs %d", a->nsteps, b->nsteps);
        return 1;
    }
    for (int i = 0; i < a->nsteps; i++) {
        const struct step *x = &a->steps[i], *y = &b->steps[i];
        if (x->ret != y->ret || x->optind != y->optind ||
                x->optopt != y->optopt || x->longindex != y->longindex ||
                x->have_arg != y->have_arg || strcmp(x->arg, y->arg) != 0) {
            snprintf(why, whysz,
                    "step %d: host ret=%d(%c) optind=%d optopt=%d li=%d "
                    "optarg=%s | shim ret=%d(%c) optind=%d optopt=%d li=%d "
                    "optarg=%s",
                    i, x->ret, x->ret > 31 && x->ret < 127 ? x->ret : '.',
                    x->optind, x->optopt, x->longindex,
                    x->have_arg ? x->arg : "(null)",
                    y->ret, y->ret > 31 && y->ret < 127 ? y->ret : '.',
                    y->optind, y->optopt, y->longindex,
                    y->have_arg ? y->arg : "(null)");
            return 1;
        }
    }
    if (a->argc != b->argc) {
        snprintf(why, whysz, "argc %d vs %d", a->argc, b->argc);
        return 1;
    }
    for (int i = 0; i < a->argc; i++) {
        if (strcmp(a->argv[i], b->argv[i]) != 0) {
            snprintf(why, whysz, "argv[%d] after parse: host %s | shim %s",
                    i, a->argv[i], b->argv[i]);
            return 1;
        }
    }
    return 0;
}

/* The message BODIES, with the "name: " each line is prefixed by removed.
 *
 * The prefix is the one deliberate divergence: the host's getopt_long names
 * the process (__progname), which inside AOK would make every applet
 * introduce itself as "ish". The shim names argv[0]'s basename instead -- what
 * the host's plain getopt already does, and what the guest expects. Compared
 * separately below so the divergence is asserted rather than waved through. */
static void strip_prognames(const char *in, char *out, size_t outsz) {
    size_t o = 0;
    int at_line_start = 1;
    for (const char *p = in; *p && o + 1 < outsz; ) {
        if (at_line_start) {
            const char *colon = strstr(p, ": ");
            const char *nl = strchr(p, '\n');
            if (colon != NULL && (nl == NULL || colon < nl)) {
                p = colon + 2;
                at_line_start = 0;
                continue;
            }
            at_line_start = 0;
        }
        if (*p == '\n')
            at_line_start = 1;
        out[o++] = *p++;
    }
    out[o] = '\0';
}

/* Every line of the shim's output must start with argv[0]'s basename. */
static int prognames_are(const char *text, const char *want) {
    size_t wl = strlen(want);
    for (const char *p = text; *p; ) {
        if (strncmp(p, want, wl) != 0 || strncmp(p + wl, ": ", 2) != 0)
            return 0;
        const char *nl = strchr(p, '\n');
        if (nl == NULL)
            break;
        p = nl + 1;
    }
    return 1;
}

/* ------------------------------------------------------------- the corpus */

static struct option lo_basic[] = {
    {"verbose", no_argument,       NULL, 'v'},
    {"file",    required_argument, NULL, 'f'},
    {"level",   optional_argument, NULL, 'l'},
    {"quiet",   no_argument,       NULL, 'q'},
    {"version", no_argument,       NULL, 'V'},
    {NULL, 0, NULL, 0}
};
static struct option lo_flag[] = {
    {"set",  no_argument, &longflag_host, 42},   /* patched per side below */
    {"file", required_argument, NULL, 'f'},
    {NULL, 0, NULL, 0}
};
static struct option lo_ambig[] = {
    {"verbose", no_argument, NULL, 'v'},
    {"version", no_argument, NULL, 'V'},
    {"vex",     no_argument, NULL, 'x'},
    {NULL, 0, NULL, 0}
};
/* An exact match sitting AFTER two partial ones: the older BSD getopt_long
 * reports "ambiguous" on reaching the second partial and never sees the exact
 * match; the one macOS ships returns it. */
static struct option lo_exact_last[] = {
    {"veal", no_argument, NULL, 'a'},
    {"vest", no_argument, NULL, 'b'},
    {"ve",   no_argument, NULL, 'c'},
    {NULL, 0, NULL, 0}
};
/* Two partial matches that agree on has_arg/flag/val are not ambiguous; a
 * third that disagrees makes them so. */
static struct option lo_dup[] = {
    {"foo",  no_argument, NULL, 'f'},
    {"foo",  no_argument, NULL, 'f'},
    {NULL, 0, NULL, 0}
};
static struct option lo_dup_then_diff[] = {
    {"foo",  no_argument, NULL, 'f'},
    {"foo",  no_argument, NULL, 'f'},
    {"fool", no_argument, NULL, 'g'},
    {NULL, 0, NULL, 0}
};
static struct option lo_hasarg_differs[] = {
    {"aa", required_argument, NULL, 'a'},
    {"ab", no_argument,       NULL, 'b'},
    {NULL, 0, NULL, 0}
};

static const struct testcase corpus[] = {
 /* --- plain short options ------------------------------------------------ */
 {"simple",            "abc",   {"p","-a","-b","x",NULL}, E_SHORT, NULL, 0, 0},
 {"clustered",         "abc",   {"p","-abc",NULL},        E_SHORT, NULL, 0, 0},
 {"cluster+unknown",   "abc",   {"p","-abz","-c",NULL},   E_SHORT, NULL, 0, 0},
 {"attached arg",      "f:v",   {"p","-fFILE","-v",NULL}, E_SHORT, NULL, 0, 0},
 {"detached arg",      "f:v",   {"p","-f","FILE","-v",NULL}, E_SHORT, NULL, 0, 0},
 {"cluster then arg",  "vf:",   {"p","-vfFILE",NULL},     E_SHORT, NULL, 0, 0},
 {"cluster then sep",  "vf:",   {"p","-vf","FILE",NULL},  E_SHORT, NULL, 0, 0},
 {"missing arg",       "f:",    {"p","-f",NULL},          E_SHORT, NULL, 0, 0},
 {"missing arg colon", ":f:",   {"p","-f",NULL},          E_SHORT, NULL, 0, 0},
 {"unknown",           "ab",    {"p","-z",NULL},          E_SHORT, NULL, 0, 0},
 {"unknown colon",     ":ab",   {"p","-z",NULL},          E_SHORT, NULL, 0, 0},
 {"literal colon opt", "a:b",   {"p","-:",NULL},          E_SHORT, NULL, 0, 0},
 {"double dash",       "ab",    {"p","-a","--","-b","x",NULL}, E_SHORT, NULL, 0, 0},
 {"dash alone",        "ab",    {"p","-a","-","-b",NULL}, E_SHORT, NULL, 0, 0},
 {"dash in optstring", "a-b",   {"p","-a","-","-b",NULL}, E_SHORT, NULL, 0, 0},
 {"nonopt first",      "ab",    {"p","x","-a",NULL},      E_SHORT, NULL, 0, 0},
 {"empty optstring",   "",      {"p","-a","x",NULL},      E_SHORT, NULL, 0, 0},
 {"no args",           "ab",    {"p",NULL},               E_SHORT, NULL, 0, 0},
 {"empty arg",         "ab",    {"p","","-a",NULL},       E_SHORT, NULL, 0, 0},
 {"just dashdash",     "ab",    {"p","--",NULL},          E_SHORT, NULL, 0, 0},
 {"dashdashfoo",       "ab",    {"p","--foo",NULL},       E_SHORT, NULL, 0, 0},
 /* optional arguments, the `::` GNU extension BSD getopt also honours */
 {"optarg attached",   "l::v",  {"p","-l3","-v",NULL},    E_SHORT, NULL, 0, 0},
 {"optarg detached",   "l::v",  {"p","-l","3","-v",NULL}, E_SHORT, NULL, 0, 0},
 {"optarg absent",     "l::v",  {"p","-l","-v",NULL},     E_SHORT, NULL, 0, 0},
 {"optarg last",       "l::",   {"p","-l",NULL},          E_SHORT, NULL, 0, 0},
 /* optreset: restart a parse over the same argv */
 {"optreset restart",  "ab",    {"p","-a","-b",NULL},     E_SHORT, NULL, 2, 1},
 {"optreset midcluster","abc",  {"p","-abc",NULL},        E_SHORT, NULL, 1, 1},
 {"optreset new index","af:",   {"p","-a","-f","X",NULL}, E_SHORT, NULL, 1, 2},

 /* --- getopt_long -------------------------------------------------------- */
 {"long exact",        "vf:",   {"p","--verbose","--file=X",NULL}, E_LONG, lo_basic, 0, 0},
 {"long sep arg",      "vf:",   {"p","--file","X","-v",NULL},      E_LONG, lo_basic, 0, 0},
 {"long abbrev",       "vf:",   {"p","--fil","X",NULL},            E_LONG, lo_basic, 0, 0},
 {"long ambiguous",    "vVx",   {"p","--ve",NULL},                 E_LONG, lo_ambig, 0, 0},
 {"long unambig pfx",  "vVx",   {"p","--vex",NULL},                E_LONG, lo_ambig, 0, 0},
 {"long unknown",      "vf:",   {"p","--nope",NULL},               E_LONG, lo_basic, 0, 0},
 {"long noarg =arg",   "vf:",   {"p","--verbose=1",NULL},          E_LONG, lo_basic, 0, 0},
 {"long missing arg",  "vf:",   {"p","--file",NULL},               E_LONG, lo_basic, 0, 0},
 {"long missing colon",":vf:",  {"p","--file",NULL},               E_LONG, lo_basic, 0, 0},
 {"long optional =",   "vf:l::",{"p","--level=9",NULL},            E_LONG, lo_basic, 0, 0},
 {"long optional bare","vf:l::",{"p","--level","9",NULL},          E_LONG, lo_basic, 0, 0},
 {"long permute",      "vf:",   {"p","one","--verbose","two","--file=X","three",NULL}, E_LONG, lo_basic, 0, 0},
 {"long permute short","vf:",   {"p","one","-v","two","-f","X",NULL}, E_LONG, lo_basic, 0, 0},
 {"long plus prefix",  "+vf:",  {"p","-v","one","-f","X",NULL},    E_LONG, lo_basic, 0, 0},
 {"long dash prefix",  "-vf:",  {"p","one","-v","two",NULL},       E_LONG, lo_basic, 0, 0},
 {"long dashdash",     "vf:",   {"p","-v","--","one","-f",NULL},   E_LONG, lo_basic, 0, 0},
 {"long dashdash perm","vf:",   {"p","one","-v","--","two","-f",NULL}, E_LONG, lo_basic, 0, 0},
 {"long W;",           "W;vf:", {"p","-W","file=X","-v",NULL},     E_LONG, lo_basic, 0, 0},
 {"long W; attached",  "W;vf:", {"p","-Wverbose",NULL},            E_LONG, lo_basic, 0, 0},
 {"long W; no arg",    "W;vf:", {"p","-W",NULL},                   E_LONG, lo_basic, 0, 0},
 {"long flag ptr",     "f:",    {"p","--set","--file","X",NULL},   E_LONG, lo_flag, 0, 0},
 {"long optreset",     "vf:",   {"p","--verbose","--file=X",NULL}, E_LONG, lo_basic, 1, 1},
 {"long many nonopts", "vf:",   {"p","a","b","-v","c","d","-f","X","e",NULL}, E_LONG, lo_basic, 0, 0},

 /* the corners that separate the getopt_long macOS ships from older BSD ones */
 {"exact after partial","abc",  {"p","--ve",NULL},                 E_LONG, lo_exact_last, 0, 0},
 {"dup partials ok",   "f",     {"p","--fo",NULL},                 E_LONG, lo_dup, 0, 0},
 {"dup then differing","fg",    {"p","--fo",NULL},                 E_LONG, lo_dup_then_diff, 0, 0},
 {"partial hasarg diff","ab",   {"p","--a",NULL},                  E_LONG, lo_hasarg_differs, 0, 0},
 {"long lone dash opt","v-",    {"p","-","-v",NULL},               E_LONG, lo_basic, 0, 0},
 {"long noarg= colon", ":vf:",  {"p","--verbose=1",NULL},          E_LONG, lo_basic, 0, 0},
 {"long unknown short","vf:",   {"p","-z",NULL},                   E_LONG, lo_basic, 0, 0},
 {"long unknown colon",":vf:",  {"p","-z",NULL},                   E_LONG, lo_basic, 0, 0},
 {"long inorder dash", "-vf:",  {"p","a","-v","b",NULL},           E_LONG, lo_basic, 0, 0},
 {"only ambiguous",    "vf:",   {"p","-fo",NULL},                  E_LONG_ONLY, lo_dup, 0, 0},

 /* --- getopt_long_only --------------------------------------------------- */
 {"only exact",        "vf:",   {"p","-verbose","-f","X",NULL},    E_LONG_ONLY, lo_basic, 0, 0},
 {"only short wins",   "vf:",   {"p","-v","-file=X",NULL},         E_LONG_ONLY, lo_basic, 0, 0},
 {"only abbrev",       "vf:",   {"p","-fil","X",NULL},             E_LONG_ONLY, lo_basic, 0, 0},
 {"only unknown",      "vf:",   {"p","-zzz",NULL},                 E_LONG_ONLY, lo_basic, 0, 0},
 {"only cluster",      "vqf:",  {"p","-vq",NULL},                  E_LONG_ONLY, lo_basic, 0, 0},

 /* --- real command lines from the applets that hit this ------------------ */
 {"ls -la",            "1AaCcdFfhiklmnpqRrSstuwx", {"ls","-la",NULL}, E_SHORT, NULL, 0, 0},
 {"grep -q pat file",  "EFGHIPRSViclnqsvwxyabde:f:m:", {"grep","-q","pat","file",NULL}, E_SHORT, NULL, 0, 0},
 {"sort -r",           "bcdfghik:mno:rst:uz", {"sort","-r","file",NULL}, E_SHORT, NULL, 0, 0},
 {"head -2",           "n:c:qv",  {"head","-2","file",NULL},       E_SHORT, NULL, 0, 0},
 {"wc -l",             "clmwL",   {"wc","-l","file",NULL},         E_SHORT, NULL, 0, 0},
 {"ssh-keygen ed25519","Aa:b:Ccdefghij:klm:N:N:N:", {"ssh-keygen","-q","-t","ed25519","-N","","-f","/tmp/c1",NULL}, E_SHORT, NULL, 0, 0},
 {"ssh -V",            "1246ab:c:e:fgi:kl:m:no:p:qstvx", {"ssh","-V",NULL}, E_SHORT, NULL, 0, 0},
 {"scp -r a b",        "12346ABCTdfpqrtvO", {"scp","-r","a","b",NULL}, E_SHORT, NULL, 0, 0},
 {"sftp -P 22",        "1246afhpqrvCc:D:i:l:o:s:S:b:B:F:J:P:R:", {"sftp","-P","22","host",NULL}, E_SHORT, NULL, 0, 0},
 {NULL, NULL, {NULL}, E_SHORT, NULL, 0, 0}
};

/* ------------------------------------------------------------- random cases */

static unsigned rng_state = 0x2545F491u;
static unsigned rnd(unsigned n) {
    rng_state = rng_state * 1103515245u + 12345u;
    return (rng_state >> 8) % n;
}

static const char letters[] = "abcfglnovxz";

static void make_random(struct testcase *tc, char *optbuf, size_t optsz,
        char argbuf[MAXARGV][32], const char *argv[MAXARGV]) {
    size_t o = 0;
    if (rnd(6) == 0) optbuf[o++] = ':';
    else if (rnd(6) == 0) optbuf[o++] = '+';
    else if (rnd(8) == 0) optbuf[o++] = '-';
    if (rnd(7) == 0) { optbuf[o++] = 'W'; optbuf[o++] = ';'; }
    if (rnd(9) == 0) optbuf[o++] = '-';
    int nletters = 1 + (int) rnd(5);
    for (int i = 0; i < nletters && o + 3 < optsz; i++) {
        optbuf[o++] = letters[rnd(sizeof(letters) - 1)];
        unsigned k = rnd(4);
        if (k == 1) optbuf[o++] = ':';
        else if (k == 2) { optbuf[o++] = ':'; optbuf[o++] = ':'; }
    }
    optbuf[o] = '\0';
    tc->optstring = optbuf;

    int n = 1;
    snprintf(argbuf[0], 32, "prog");
    argv[0] = argbuf[0];
    static const char *longish[] = {
        "--file", "--fil", "--f", "--file=X", "--verbose", "--verb",
        "--verbose=1", "--level", "--level=9", "--lev", "--quiet", "--nope",
        "--v", "--=", "--file=", "-verbose", "-file", "-fil", "-level=2",
    };
    int want = 1 + (int) rnd(6);
    for (int i = 0; i < want && n < MAXARGV - 1; i++) {
        unsigned k = rnd(14);
        if (k < 4) {
            int m = 1 + (int) rnd(3);
            char t[16];
            t[0] = '-';
            for (int j = 0; j < m; j++)
                t[1 + j] = letters[rnd(sizeof(letters) - 1)];
            t[1 + m] = '\0';
            snprintf(argbuf[n], 32, "%s", t);
        } else if (k < 5) {
            snprintf(argbuf[n], 32, "--");
        } else if (k < 6) {
            snprintf(argbuf[n], 32, "-");
        } else if (k < 7) {
            snprintf(argbuf[n], 32, "-%cVAL", letters[rnd(sizeof(letters) - 1)]);
        } else if (k < 10) {
            snprintf(argbuf[n], 32, "%s",
                    longish[rnd(sizeof(longish) / sizeof(longish[0]))]);
        } else if (k < 11) {
            snprintf(argbuf[n], 32, "-W");
        } else if (k < 12) {
            snprintf(argbuf[n], 32, "-W%s", rnd(2) ? "file=X" : "verbose");
        } else {
            snprintf(argbuf[n], 32, "word%d", i);
        }
        argv[n] = argbuf[n];
        n++;
    }
    argv[n] = NULL;
    for (int i = 0; i <= n; i++)
        tc->argv[i] = argv[i];
    tc->name = "random";
    unsigned e = rnd(3);
    tc->entry = e == 0 ? E_SHORT : (e == 1 ? E_LONG : E_LONG_ONLY);
    tc->longopts = tc->entry == E_SHORT ? NULL : lo_basic;
    tc->reset_at = rnd(8) == 0 ? 1 + (int) rnd(2) : 0;
    tc->reset_optind = rnd(3);
}

/* ---------------------------------------------------------------- main */

int main(int argc, char **argv) {
    int nrandom = argc > 1 ? atoi(argv[1]) : 20000;
    unsetenv("POSIXLY_CORRECT");    /* both sides consult it; keep it steady */

    /* lo_flag's flag pointer has to be the right side's each run. */
    static struct trace th, ts;
    int ran = 0, diff = 0, errdiff = 0;
    char why[1024];

    for (int witherr = 0; witherr < 2; witherr++) {
        for (const struct testcase *tc = corpus; tc->name; tc++) {
            struct testcase local = *tc;
            static struct option lf_host[3], lf_shim[3];
            if (tc->longopts == lo_flag) {
                memcpy(lf_host, lo_flag, sizeof(lf_host));
                memcpy(lf_shim, lo_flag, sizeof(lf_shim));
                lf_host[0].flag = &longflag_host;
                lf_shim[0].flag = &longflag_shim;
            }
            local.longopts = tc->longopts == lo_flag ? lf_host : tc->longopts;
            run(&local, 0, witherr, &th);
            local.longopts = tc->longopts == lo_flag ? lf_shim : tc->longopts;
            run(&local, 1, witherr, &ts);
            ran++;
            if (trace_diff(&th, &ts, why, sizeof(why))) {
                diff++;
                printf("DIFF [%s%s] optstring=\"%s\": %s\n", tc->name,
                        witherr ? " +opterr" : "", tc->optstring, why);
            }
            if (witherr) {
                char hb[4096], sb[4096];
                strip_prognames(th.err, hb, sizeof(hb));
                strip_prognames(ts.err, sb, sizeof(sb));
                if (strcmp(hb, sb) != 0) {
                    errdiff++;
                    printf("ERRTEXT [%s] optstring=\"%s\":\n"
                           "  host: %s  shim: %s",
                            tc->name, tc->optstring, th.err, ts.err);
                }
                if (!prognames_are(ts.err, tc->argv[0])) {
                    errdiff++;
                    printf("PROGNAME [%s]: shim did not prefix with '%s':\n%s",
                            tc->name, tc->argv[0], ts.err);
                }
            }
        }
    }
    int handwritten = ran;

    for (int i = 0; i < nrandom; i++) {
        struct testcase tc;
        char optbuf[64];
        static char argbuf[MAXARGV][32];
        static const char *av[MAXARGV];
        memset(&tc, 0, sizeof(tc));
        make_random(&tc, optbuf, sizeof(optbuf), argbuf, av);
        run(&tc, 0, 0, &th);
        run(&tc, 1, 0, &ts);
        ran++;
        if (trace_diff(&th, &ts, why, sizeof(why))) {
            diff++;
            if (diff < 25) {
                printf("DIFF [random] optstring=\"%s\" argv:", tc.optstring);
                for (int j = 0; tc.argv[j]; j++) printf(" '%s'", tc.argv[j]);
                printf("\n  %s\n", why);
            }
        }
    }

    printf("difftest-native-getopt: %d cases (%d hand-written, %d random), "
           "%d parse differences, %d error-text differences\n",
           ran, handwritten, nrandom, diff, errdiff);
    return diff == 0 && errdiff == 0 ? 0 : 1;
}
