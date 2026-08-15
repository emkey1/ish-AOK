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
    "rand", "rand_r", "srand", "random", "arc4random", "arc4random_uniform",
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
    # option parsing: operates on the argv it is handed
    "getopt", "getopt_long", "optarg", "optind", "opterr", "optopt", "optreset",
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
    # abort() raises SIGABRT on the calling thread. It ends the app rather than
    # the task, which is wrong, but it is a crash either way and not a way to
    # observe or change the host -- routing it would be an improvement, not a
    # correctness fix.
    "abort",
}

# Compiler and runtime plumbing rather than calls the program made.
INTERNAL_PREFIXES = ("__", "_tlv_", "_os_", "_platform_")
INTERNAL = {"dyld_stub_binder"}

# Provided by AOK itself, by the shim, or by the program.
OURS = re.compile(r"^(nlibc_|native_|task_|do_|f_get|f_install|f_close|"
                  r"generic_|mount_|fd_|lock$|unlock$|current$|smallclue|"
                  r"pscal|awk|nextvi|micro_|dvtm_)")


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
DEFAULT_TARGETS = ("build/libsmallclue.a", "build/libnextvi.a",
                   "build/libbash.a")


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
