// native_dash_patterns.c -- native dash's pattern matching against Debian's
// dash 0.5.12 (camd, glibc).
//
// A `[` that begins no bracket expression matches itself (POSIX 2.13.1).
// Native dash (/AOK/native/dash, which native-links.sh puts first on PATH as
// `sh`) matched with Darwin's fnmatch, which refuses such a pattern outright:
// `t='socket:[686]'; echo ${t#socket:[}` printed `socket:[686]`, not `686]`,
// and start-wayland.sh -- run by the app under this `sh` -- never found its X
// display. It uses dash's own matcher now (kernel/dash_config_aok.h). The rest
// of the table is the matching that must not move with it: negation, classes,
// escapes, quoted metacharacters, pathname expansion. The expectations are
// Debian dash's output for the same script.
//
// Skips wherever the native shell is missing, real Linux included.
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define DASH "/AOK/native/dash"

static const char script[] =
    "t='socket:[686]'\n"
    "echo \"1 ${t#socket:[}\"\n"
    "echo \"2 ${t%[686]}\"\n"
    "case 'a[b' in a[b) echo '3 case literal';; *) echo '3 case nomatch';; esac\n"
    "case 'x' in [!a]) echo '4 bracket ok';; *) echo '4 bracket broken';; esac\n"
    "case 'b' in [[:alpha:]]) echo '5 class ok';; *) echo '5 class broken';; esac\n"
    "case '[' in \\[) echo '6 escaped ok';; *) echo '6 escaped broken';; esac\n"
    "case 'a]' in a]) echo '7 lone ] ok';; *) echo '7 lone ] broken';; esac\n"
    "case 'x' in [^a]) echo '8 [^a] negates';; *) echo '8 [^a] does not';; esac\n"
    "v='a*b'; case 'a*b' in \"$v\") echo '9 quoted star literal';; *) echo '9 broken';; esac\n"
    "case 'axb' in \"$v\") echo '10 quoted star globbed';; *) echo '10 quoted star not globbed';; esac\n"
    "u='x[y'; echo \"11 ${u%[y}\" \"${u#*[}\"\n"
    "d=/tmp/native_dash_patterns.$$; mkdir -p $d && cd $d && touch 'a[b' abc &&\n"
    "  echo \"12 $(echo a[b)\" && echo \"13 $(echo a[!x]c)\"; cd /; rm -rf $d\n";

static const char expect[] =
    "1 686]\n"
    "2 socket:[686]\n"
    "3 case literal\n"
    "4 bracket ok\n"
    "5 class ok\n"
    "6 escaped ok\n"
    "7 lone ] ok\n"
    "8 [^a] does not\n"
    "9 quoted star literal\n"
    "10 quoted star not globbed\n"
    "11 x y\n"
    "12 a[b\n"
    "13 abc\n";

int main(void) {
    if (access(DASH, X_OK) != 0) {
        printf("native_dash_patterns: SKIP (no %s)\n", DASH);
        return 0;
    }
    int out[2];
    pipe(out);
    pid_t c = fork();
    if (c == 0) {
        dup2(out[1], 1);
        dup2(out[1], 2);
        close(out[0]);
        execl(DASH, "dash", "-c", script, (char *) NULL);
        _exit(127);
    }
    close(out[1]);
    char got[4096];
    size_t len = 0;
    ssize_t n;
    while (len < sizeof got - 1 && (n = read(out[0], got + len, sizeof got - 1 - len)) > 0)
        len += (size_t) n;
    got[len] = '\0';
    int st = 0;
    waitpid(c, &st, 0);
    int failures = 0;
    if (strcmp(got, expect) != 0) {
        failures++;
        // line by line, so the report names what moved
        const char *g = got, *e = expect;
        while (*g || *e) {
            size_t gl = strcspn(g, "\n"), el = strcspn(e, "\n");
            if (gl != el || strncmp(g, e, gl) != 0)
                printf("FAIL got \"%.*s\", want \"%.*s\"\n", (int) gl, g, (int) el, e);
            g += gl + (g[gl] == '\n');
            e += el + (e[el] == '\n');
        }
    }
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        failures++;
        printf("FAIL dash ended with status %#x\n", st);
    }
    printf("native_dash_patterns: %s\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
