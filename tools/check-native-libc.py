#!/usr/bin/env python3
"""Fail the build when a natively-compiled program can reach the host kernel.

Why an ALLOWLIST
----------------
A program compiled into iSH-AOK (kernel/native.h) links against the HOST libc,
so a direct open()/getuid()/socket() resolves against iOS instead of the guest
-- and *succeeds*, returning the wrong answer. Silent wrongness, not a crash.

This began as a denylist of calls to forbid, and that was wrong in a way that
kept repeating. It was written for filesystem calls; then it had to grow for
process control, then host-global state, then system identity when `uname`
reported Darwin, then user identity when `whoami` answered "mobile" -- the iOS
account. Every one of those was found by a person running the thing, never by
this tool, because a denylist only knows the categories someone already
thought of.

So the polarity is inverted. Anything referenced that is not on the allowlist
below fails the build. The allowlist holds only what has no kernel state at
all: arithmetic, memory, strings, formatting, sorting. Adding to it is a
deliberate act asserting "this genuinely cannot observe the host".

The failure mode is the point: a missed call becomes a build error naming the
symbol, instead of a guest quietly reading the device.

Usage: check-native-libc.py [object-or-archive ...]
       check-native-libc.py --report [object-or-archive ...]

With no arguments it checks every archive in DEFAULT_TARGETS below. --report
classifies instead of failing: already routed / pure / needs work. Point it at
a new program's objects and the third list is the porting work, enumerated.
"""
import os
import re
import subprocess
import sys

# No kernel state: pure computation over memory the caller already owns.
PURE = {
    # memory and strings
    "memcpy", "memmove", "memset", "memcmp", "memchr", "bcopy", "bzero", "bcmp",
    "strlen", "strnlen", "strcpy", "strncpy", "strcat", "strncat", "strcmp",
    "strncmp", "strcasecmp", "strncasecmp", "strchr", "strrchr", "strstr",
    "strcasestr", "strdup", "strndup", "strspn", "strcspn", "strpbrk", "strtok",
    "strtok_r", "strsep", "strerror", "strsignal", "basename", "dirname",
    "fnmatch", "mbrtowc", "wcwidth", "swab",
    # allocation
    "malloc", "calloc", "realloc", "free", "reallocf", "posix_memalign",
    "open_memstream",   # memory-backed FILE; touches nothing
    # conversion and maths
    "atoi", "atol", "atoll", "atof", "strtol", "strtoll", "strtoul", "strtoull",
    "strtod", "strtof", "abs", "labs", "llabs", "qsort", "qsort_r", "bsearch",
    "sin", "cos", "tan", "atan", "atan2", "exp", "log", "log2", "log10", "pow",
    "sqrt", "fmod", "floor", "ceil", "round", "trunc", "fabs", "ldexp", "frexp",
    # The rest of libm, added when zsh/mathfunc arrived -- that one module is
    # 28 of these on its own, and every one of them is a floating-point
    # function of its arguments. `signgam` is the odd name in the list: it is
    # not a function but the global lgamma writes its sign into, and it is as
    # host-free as the return value beside it.
    "acos", "acosh", "asin", "asinh", "atanh", "cbrt", "cosh", "erf", "erfc",
    "expm1", "hypot", "ilogb", "j0", "j1", "jn", "lgamma", "log1p", "logb",
    "modf", "nextafter", "scalbn", "signgam", "sinh", "tanh", "tgamma",
    "y0", "y1", "yn",
    # The 48-bit PRNG family, on the same terms as rand/random above: erand48
    # draws from a buffer the CALLER owns, and seed48 from libc's own static
    # state. Neither reads a device, a file or a clock -- a seeded sequence is
    # the same sequence on host and guest.
    "erand48", "seed48",
    # Compiler-generated bounds-checked forms of memccpy and strncat. Same
    # function plus a size the compiler knew; clang emits them under
    # _FORTIFY_SOURCE without the source ever naming them.
    "__memccpy_chk", "__strncat_chk", "wmemmove",
    "rand", "rand_r", "srand", "random", "arc4random", "arc4random_uniform",
    # arc4random_buf, and the reasoning written out because this one is a
    # JUDGEMENT rather than a fact -- the shape fileno had.
    #
    # It qualifies on the test that matters: it fills the caller's buffer with
    # CSPRNG output, reads no guest-visible state, and reports nothing about the
    # host. The bytes are indistinguishable from any other source's, so no
    # answer here can "differ between host and guest" in a way a guest could
    # observe. It also belongs to a family: arc4random and arc4random_uniform
    # are already here, and routing one of three would leave the same binary
    # drawing from two pools for no gain.
    #
    # The counter-argument as first written was wrong on its facts and is
    # corrected here rather than quietly dropped. It claimed the routed
    # getentropy meant OpenSSH already drew from two pools. The routing is real
    # and the consequence is not: openbsd-compat/bsd-getentropy.c defines
    # _ssh_compat_getentropy, NOTHING references it across any archive, and
    # arc4random.c/arc4random_uniform.c compile to empty objects. So there is
    # exactly ONE pool here, the host's, and the routed getentropy is
    # compiled-in dead code. That strengthens this entry rather than weakening
    # it -- there is no split to be inconsistent about. Left as written below is
    # the part that still stands: what draws on it, and when to revisit.
    # (Historical: getentropy IS routed
    # (native_libc.h -> NATIVE_SYS_getrandom), which would mean two
    # pools, and everything security-bearing comes from the host's -- session
    # keys, packet padding, private-key shielding, new key material, mux
    # cookies, known_hosts hashing. entropy.c's seed_rng() in this
    # without-OpenSSL build is nothing but an arc4random_buf priming call.
    # Cryptographically the host's is the stronger and non-failing choice, and
    # guest getrandom can fail where arc4random_buf has no way to say so. The
    # reason to revisit would be a guest with a deliberately controlled RNG -- a
    # seeded rootfs for reproducible tests -- which this silently bypasses.
    "arc4random_buf",
    # ctype
    "isalnum", "isalpha", "isascii", "isblank", "iscntrl", "isdigit", "isgraph",
    "islower", "isprint", "ispunct", "isspace", "isupper", "isxdigit",
    "tolower", "toupper",
    # Address text <-> bytes. No kernel state: these parse and format a buffer
    # the caller already owns. The address FAMILY argument stays in the host's
    # numbering throughout (a program passes the AF_INET6 its own headers gave
    # it, and the host libc reads it the same way), so nothing here has to
    # agree with the guest -- only the resulting bytes cross, and those are
    # network order on both sides.
    "inet_ntop", "inet_pton", "inet_ntoa", "inet_addr", "htons", "htonl",
    "ntohs", "ntohl",
    # inet_aton belongs to that same list and is spelled out separately because
    # it arrived beside nineteen symbols that all had to be ROUTED, and "it was
    # in the same batch" is not a reason.
    #
    # It parses a caller-owned NUL-terminated string into four network-order
    # bytes in a caller-owned struct in_addr. No descriptor, path, clock,
    # identity or device is consulted, and the result is a pure function of the
    # input text -- the same standing inet_addr has one line up, which is the
    # same parse with a different return convention.
    #
    # The governing test is not "is it pure" but "can its answer differ between
    # the host and the guest", and for a text-to-bytes parser the only way it
    # could is if Darwin and Linux accepted different input grammars. They do
    # not; both take the classic 4-, 3-, 2- and 1-part forms. That argument did
    # not have to be trusted, though, because zsh's only call site is
    # Src/Modules/tcp.c's `zsh_inet_aton("0.0.0.0", ...)` -- a compile-time
    # string constant, which no grammar difference can reach. (Its other
    # reference, inside tcp.c's own zsh_inet_pton, is compiled out because
    # HAVE_INET_PTON is defined.)
    #
    # One warning for whoever next reads an interposer log: a run of
    # `ztcp 127.0.0.1 12345` shows inet_aton called with BOTH "0.0.0.0" and
    # "127.0.0.1". The second is not zsh -- it is libSystem's own
    # getipnodebyname calling inet_aton internally, visible because dyld
    # interposition rewrites intra-library calls too. That is evidence about
    # getipnodebyname, which is routed, and none about this entry.
    "inet_aton",
    # CommonCrypto's digest transforms, reached through
    # deps/smallclue-shim/openssl/evp.h, which is how md5sum/sha1sum/sha256sum
    # exist at all without AOK linking OpenSSL.
    #
    # These qualify on the same terms as memcpy: Init/Update/Final read and
    # write a context struct and an input buffer the CALLER owns, and consult
    # no descriptor, file, clock or device. The bytes they hash arrive through
    # the redirected fread, so the file being digested is the guest's.
    #
    # Scoped to the digests deliberately. CommonCrypto as a whole is NOT pure
    # -- CCRandomGenerateBytes draws on the host, and the keychain and cipher
    # APIs hold state -- so this is a list of nine functions, not a category.
    "CC_MD5_Init", "CC_MD5_Update", "CC_MD5_Final",
    "CC_SHA1_Init", "CC_SHA1_Update", "CC_SHA1_Final",
    "CC_SHA256_Init", "CC_SHA256_Update", "CC_SHA256_Final",
    # termios helpers that only edit a struct in memory. Not tcgetattr or
    # tcsetattr, which talk to a terminal and are redirected: these set or read
    # fields, and the redirected calls translate the result on its way to the
    # guest's tty.
    "cfmakeraw", "cfgetospeed", "cfgetispeed", "cfsetospeed", "cfsetispeed",
    "cfsetspeed",
    # Formatting, and FILE* operations. Safe only because fopen/fdopen/stdout/
    # stderr/stdin are all redirected, so every FILE* a native program holds is
    # one the shim created over a guest fd.
    #
    # fileno is NOT here, and was, which cost a day. The premise was right --
    # every stream is ours -- and the conclusion backwards: being ours is
    # exactly why fileno must be redirected, because a funopen stream has no
    # descriptor and the host answers -1 with EBADF. bash reads
    # isatty(fileno(stdin)) to decide whether it is interactive.
    "snprintf", "vsnprintf", "sprintf", "vsprintf", "sscanf", "vsscanf",
    "asprintf", "vasprintf", "fprintf", "vfprintf", "fputs", "fputc", "putc",
    "fwrite", "fread", "fgets", "fgetc", "getc", "ungetc", "fclose", "fflush",
    "ferror", "feof", "clearerr", "rewind", "fseek", "fseeko",
    "ftell", "ftello", "setvbuf", "setbuf", "funopen", "fscanf", "getline",
    "getdelim", "getchar",
    # getopt is NOT here, and was, on the reasoning that it "operates on the
    # argv it is handed". That was true of the argv and false of everything
    # else: optind, optarg, opterr, optopt, optreset and getopt's own scanning
    # pointer are one copy per PROCESS, and a native program is a function on a
    # task's thread. Six concurrent `smallclue ssh-keygen -q -t ed25519 -N ""
    # -f /tmp/cN` had three of them lose -f entirely and fall back to prompting
    # for a filename, because their parses were reading each other's state.
    # It is routed through native_libc.h now, per-thread. The lesson generalises
    # past this entry: "touches no file" is not the test -- "holds no state the
    # next program can see" is.
    "regcomp", "regexec", "regerror", "regfree",
    # locale and time FORMATTING. Reading the clock is below; setting it is
    # redirected. setlocale is NOT here: the guest's locale names are Linux's
    # and the host's database is Darwin's, so a name has to be translated
    # rather than passed through -- C.UTF-8 exists on Linux and macOS but not
    # on iOS, which is a warning on every shell start.
    "localeconv", "strftime", "strptime", "mktime", "timegm",
    "gmtime", "gmtime_r", "localtime", "localtime_r", "difftime", "asctime",
    "ctime", "tzset",
    # Thread primitives. Creation is redirected -- a new thread needs the task
    # propagated onto it -- but locking and joining touch nothing the guest can
    # observe.
    "pthread_mutex_init", "pthread_mutex_destroy", "pthread_mutex_lock",
    "pthread_mutex_unlock", "pthread_mutex_trylock", "pthread_cond_init",
    "pthread_cond_destroy", "pthread_cond_wait", "pthread_cond_signal",
    "pthread_cond_broadcast", "pthread_cond_timedwait", "pthread_join",
    "pthread_detach", "pthread_self", "pthread_equal", "pthread_attr_init",
    "pthread_attr_destroy", "pthread_attr_setstacksize",
    "pthread_attr_setdetachstate", "pthread_once", "pthread_key_create",
    "pthread_getspecific", "pthread_setspecific",
    # non-local jumps: control flow within the program
    "setjmp", "longjmp", "sigsetjmp", "siglongjmp", "_setjmp", "_longjmp",
    # Reading the clock. The guest's time IS the host's, so this is not a leak.
    "clock_gettime", "gettimeofday", "time", "clock",
    # Wide characters and multibyte conversion. Every one of these transforms a
    # buffer the caller already owns, against the locale setlocale selected --
    # which is above for the same reason. None can name a file or a process.
    "mblen", "mbrlen", "mbsinit", "mbtowc", "wctomb", "mbstowcs", "wcstombs",
    "mbsrtowcs", "wcsrtombs", "mbsnrtowcs", "wcsnrtombs", "wcrtomb",
    "wcslen", "wcschr", "wcsrchr", "wcscmp", "wcsncmp", "wcscoll", "wcsdup",
    "wcscpy", "wcsncpy", "wcscat", "wcwidth", "wcswidth", "wctob", "btowc",
    "wctype", "iswctype", "wmemchr", "wmemcmp", "wmemcpy", "wmemset",
    "towlower", "towupper", "iswalnum", "iswalpha", "iswblank", "iswcntrl",
    "iswdigit", "iswgraph", "iswlower", "iswprint", "iswpunct", "iswspace",
    "iswupper", "iswxdigit",
    # Locale INFORMATION, as distinct from anything the guest owns: these
    # report the conventions setlocale selected -- decimal point, charset name,
    # month names. iconv converts one buffer to another under those same
    # conventions. Consistent with setlocale and localeconv above.
    "nl_langinfo", "locale_charset", "iconv", "iconv_open", "iconv_close",
    "_DefaultRuneLocale", "___runetype", "__maskrune",
    # More string and integer work over the caller's own memory.
    "strchrnul", "strcoll", "strxfrm", "strtoimax", "strtoumax", "strtold",
    "imaxdiv", "imaxabs", "div", "ldiv", "lldiv", "memset_pattern16",
    "memrchr", "memmem", "stpcpy", "stpncpy",
    # Discards a FILE's buffered data. Every FILE a native program holds is one
    # the shim built over a guest fd, so this reaches no further than fflush.
    "fpurge", "__fpurge",
    # OpenBSD's string and memory helpers, which OpenSSH brings with it. Each
    # one transforms a buffer the caller already owns and consults no
    # descriptor, path, clock or identity -- the same standing memcpy and
    # memcmp have above.
    #
    #  - memset_s writes a byte into the caller's buffer behind an optimization
    #    barrier. (config.h leaves HAVE_EXPLICIT_BZERO undefined and defines
    #    HAVE_MEMSET_S, so this is what explicit_bzero.c compiles to.)
    #  - timingsafe_bcmp compares two caller-owned buffers in constant time.
    #  - strtonum parses text to a long long with a range check, on strings the
    #    caller already holds -- strtol and strtoll are here for the same
    #    reason.
    #  - strmode formats a mode_t as "-rw-r--r--". The only host/guest question
    #    is whether the S_IF* encoding differs, and it does not: Darwin and
    #    Linux agree from S_IFIFO 010000 through S_IFSOCK 0140000, and Darwin's
    #    one extra (S_IFWHT) is a value the guest never produces. The mode
    #    sftp-common.c formats arrives over the SFTP wire, POSIX-numbered on
    #    both sides.
    "memset_s", "timingsafe_bcmp", "strtonum",
    # strmode was here, justified on the grounds that the guest never produces
    # Darwin's extra S_IFWHT. That guard was void -- sftp-common.c takes st_mode
    # off the SFTP WIRE and formats it, so the producer is the remote peer, and
    # Darwin's strmode differs from openbsd's on 4096 of 65536 modes. It is
    # compiled from openbsd-compat now (HAVE_STRMODE undefined) and so is no
    # longer a host symbol at all.
    # abort() raises SIGABRT on the calling thread. It ends the app rather than
    # the task, which is wrong, but it is a crash either way and not a way to
    # observe or change the host -- routing it would be an improvement, not a
    # correctness fix.
    "abort",
}

# Compiler and runtime plumbing rather than calls the program made.
#
# Note what is NOT a prefix here. This list began ("__", "_tlv_", ...), and
# that first entry is why __progname shipped: OpenSSH read the host's
# __progname, so every applet introduced itself as the app and sftp printed
# "usage: ish [...]". The symbol was an undefined external sitting in
# libopenssh.a in plain view of this allowlist, and the prefix waved it
# through. Darwin keeps compiler plumbing and real host state in the SAME
# namespace, so matching the shape of a name instead of the name itself handed
# back exactly the denylist blindness the top of this file describes -- and
# handed it back wholesale, since one prefix exempts a whole namespace at once.
#
# The lesson is the docstring's, one level down: it is not enough to invert the
# polarity if an escape hatch is left that is matched on form. Anything
# exempted has to be exempted BY NAME, which is a sentence someone has to
# write. So a new __-name fails the build until it is classified.
#
# (A second reading of "the gate cannot see this": it can. It reads undefined
# symbols, which covers data as readily as calls -- __progname is a variable
# and would have been caught. Nothing here needs to learn about variables.)
INTERNAL_PREFIXES = ("_tlv_", "_os_", "_platform_")
INTERNAL = {
    "dyld_stub_binder",
    # Stack protector and stack probes.
    "__stack_chk_fail", "__stack_chk_guard", "__chkstk_darwin",
    # _FORTIFY_SOURCE wrappers around calls already in PURE, plus the fd_set
    # bounds check the compiler inserts. They compute over the caller's own
    # memory and trap; nothing observes the host.
    "__memcpy_chk", "__memmove_chk", "__memset_chk",
    "__snprintf_chk", "__sprintf_chk", "__vsnprintf_chk",
    "__strcat_chk", "__strcpy_chk", "__strncpy_chk",
    "__strlcat_chk", "__strlcpy_chk",
    "__darwin_check_fd_set_overflow",
    # errno. Darwin expands errno to (*__error()), which returns a pointer to
    # the CALLING THREAD's copy -- and a native program runs on its guest
    # task's own thread, the same thread the routed nlibc_* calls set errno on.
    # So this already carries the guest's error, and routing it could only
    # invent a second errno for the first one to disagree with.
    "__error",
    # MB_CUR_MAX, and the one entry here that is a real divergence rather than
    # plumbing. It reports the HOST locale's maximum multibyte length, because
    # the app's LC_CTYPE is what libSystem has; bash and OpenSSH's utf8.c read
    # it. Left unrouted because the blast radius is bounded -- how many bytes a
    # character may occupy, not which file gets opened -- and because the
    # guest's own locale is not plumbed anywhere a native program could ask
    # yet. It is written down so that it is a known gap with somewhere to hang
    # the fix, which is precisely what it was not while "__" hid it.
    "__mb_cur_max",
}

# Provided by AOK itself, by the shim, or by the program.
OURS = re.compile(r"^(nlibc_|native_|task_|do_|f_get|f_install|f_close|"
                  r"generic_|mount_|fd_|lock$|unlock$|current$|smallclue|"
                  r"pscal|awk|nextvi|micro_|dvtm_|"
                  # The terminfo capability-code tables. They carry ncurses's
                  # names because that is the interface -- zsh's zsh/termcap
                  # module walks them to enumerate $termcap -- but the
                  # definitions are AOK's, in kernel/native_termcap.c, and
                  # they are three arrays of two-letter string constants.
                  # There is no host in them; the file that indexes a guest
                  # terminfo entry with them is the same file that defines
                  # them, which is the point.
                  r"boolcodes$|numcodes$|strcodes$)")


def _symbols(path, args):
    out = subprocess.run(["nm"] + args + [path], capture_output=True, text=True)
    syms = set()
    for line in out.stdout.splitlines():
        s = line.strip()
        if not s or s.endswith(":"):     # nm prints a header per object
            continue
        s = re.sub(r"^_", "", s)
        s = re.sub(r"\$.*$", "", s)      # realpath$DARWIN_EXTSN and friends
        syms.add(s)
    return syms


# Every archive holding natively-compiled code, so that running this with no
# arguments checks all of it. Named rather than globbed: a new native program
# gets a new archive, and the failure this avoids is checking half the native
# code and reading "clean" -- which is exactly what splitting Nextvi out of
# libsmallclue.a to give it its own -D made possible.
# The ssh family is a native program like the rest, and was missing here while
# it was being built -- so nothing checked it. It cost a real bug: config.h said
# HAVE_READPASSPHRASE, so OpenSSH called the HOST's readpassphrase(), which
# opens the Mac's /dev/tty rather than the guest's. ssh never prompted, sent an
# empty password, and reported "Too many authentication failures". A gate that
# does not look at an archive cannot report anything about it.
#
# libzsh.a is here on the same terms, and is only built when -Dnative_zsh is
# on. Missing targets are skipped rather than failed (see main below), so a
# default build -- which has no zsh -- is unaffected.
DEFAULT_TARGETS = ("build/libsmallclue.a", "build/libnextvi.a",
                   "build/libbash.a", "build/libzsh.a", "build/libopenssh.a",
                   "build/libopenssh_scp.a", "build/libopenssh_stubs.a",
                   "build/libopenssh_smult_curve25519_ref.a")


# The libc names kernel/native_libc.h already rewrites. Read from the header
# rather than listed here, so this cannot disagree with what the build does.
def _routed(root):
    header = os.path.join(root, "kernel", "native_libc.h")
    try:
        text = open(header).read()
    except OSError:
        return set()
    names = set()
    for m in re.finditer(r"^#define\s+(\w+)\s*(?:\([^)]*\))?\s+.*nlibc_",
                         text, re.M):
        names.add(m.group(1))
    return names


# Porting a NEW native program starts here: build its objects however its own
# build system does, point this at them, and the report is the work list --
# what is already routed, what is pure, and what has to be dealt with before it
# can link against AOK without reaching the host. That last list is the whole
# job, enumerated, rather than discovered one runtime surprise at a time.
def report(targets, root):
    referenced, defined = set(), set()
    for path in targets:
        referenced |= _symbols(path, ["-ju"])
        defined |= _symbols(path, ["-jUg"])
    external = {
        s for s in referenced - defined
        if not s.startswith(INTERNAL_PREFIXES) and s not in INTERNAL
        and not OURS.match(s)
    }
    routed = _routed(root)
    already = sorted(external & routed)
    pure = sorted((external & PURE) - routed)
    todo = sorted(external - routed - PURE)

    print(f"referenced from outside: {len(external)}")
    print(f"\n  already routed by native_libc.h ({len(already)}):")
    print("    " + " ".join(already))
    print(f"\n  pure, no kernel state ({len(pure)}):")
    print("    " + " ".join(pure))
    print(f"\n  NEEDS WORK ({len(todo)}) -- route through native_libc.h, or add")
    print(f"  to PURE here if it genuinely cannot observe the host:")
    print("    " + " ".join(todo))
    return 0


def main():
    targets = sys.argv[1:]
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if targets and targets[0] == "--report":
        targets = targets[1:]
        if not targets:
            targets = [os.path.join(root, t) for t in DEFAULT_TARGETS]
            targets = [t for t in targets if os.path.exists(t)]
        return report(targets, root)
    if not targets:
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        targets = [os.path.join(root, t) for t in DEFAULT_TARGETS]
        targets = [t for t in targets if os.path.exists(t)]
        if not targets:
            print("check-native-libc: nothing built yet -- run ninja -C build, "
                  "or name an object or archive", file=sys.stderr)
            return 2

    referenced, defined = set(), set()
    for path in targets:
        referenced |= _symbols(path, ["-ju"])
        defined |= _symbols(path, ["-jUg"])

    external = {
        s for s in referenced - defined
        if not s.startswith(INTERNAL_PREFIXES) and s not in INTERNAL
        and not OURS.match(s)
    }
    offenders = sorted(external - PURE)

    if not offenders:
        print(f"check-native-libc: clean "
              f"({len(external)} host symbols referenced, all pure)")
        return 0

    print("check-native-libc: FAIL -- these reach the HOST, not the guest:",
          file=sys.stderr)
    for sym in offenders:
        print(f"  {sym}", file=sys.stderr)
    print(f"\n{len(offenders)} symbol(s). Route each through "
          f"kernel/native_libc.h, or -- only if it genuinely cannot observe "
          f"the host -- add it to PURE in this file, deliberately.",
          file=sys.stderr)
    return 1


sys.exit(main())
