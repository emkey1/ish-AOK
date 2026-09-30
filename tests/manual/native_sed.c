// native_sed.c -- SmallCLUE's sed against GNU sed 4.9's answers.
//
// iSH-AOK's native-links.sh puts SmallCLUE's applets ahead of the distro's on
// PATH, so its sed runs every script that says `sed`, dpkg maintainer scripts
// included. The substitution-only sed it replaced rejected `s///p` outright:
// setup-gpu.sh reported "vulkaninfo lists no Venus device" and "zink gave no
// OpenGL context" on a GPU that had both (5th-generation iPad, 2026-09-30).
//
// The table is generated: each case's input and arguments were run through
// GNU sed 4.9 in build/devuan-amd64-test and its stdout and exit status
// recorded, so these expectations are GNU's, not this implementation's. The
// in-place checks at the end are written by hand from the same runs.
//
// Skips wherever the native multi-call binary is missing, real Linux included.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "test_common.h"

#define SMALLCLUE "/AOK/native/smallclue"
#define DIR "/tmp/native_sed_test"
#define SED DIR "/sed"

struct sed_case {
    const char *argv[8];
    const char *input;
    size_t input_len;
    const char *expect;
    size_t expect_len;
    int expect_status;
};

static const struct sed_case cases[] = {
    {{"-n", "s/b/X/p", NULL}, "abc\n", 4, "aXc\n", 4, 0},
    {{"s/a/X/2", NULL}, "aaa\n", 4, "aXa\n", 4, 0},
    {{"s/a/X/2g", NULL}, "aaaa\n", 5, "aXXX\n", 5, 0},
    {{"s/x/Y/ig", NULL}, "aXa\n", 4, "aYa\n", 4, 0},
    {{"s/\\//|/", NULL}, "a/b\n", 4, "a|b\n", 4, 0},
    {{"s|/|\\||", NULL}, "a/b\n", 4, "a|b\n", 4, 0},
    {{"s/[/]/X/", NULL}, "a/b\n", 4, "aXb\n", 4, 0},
    {{"s/\\(a\\)\\(b\\)/\\2\\1/", NULL}, "abc\n", 4, "bac\n", 4, 0},
    {{"-E", "s/(a)(b)/\\2\\1/", NULL}, "abc\n", 4, "bac\n", 4, 0},
    {{"s/\\w\\+/\\u&/g", NULL}, "hello world\n", 12, "Hello World\n", 12, 0},
    {{"s/.*/\\U&/", NULL}, "hello world\n", 12, "HELLO WORLD\n", 12, 0},
    {{"s/\\(.*\\) \\(.*\\)/\\L\\1 \\E\\2/", NULL}, "Hello World\n", 12, "hello World\n", 12, 0},
    {{"s/o/\\n/", NULL}, "foo\n", 4, "f\no\n", 4, 0},
    {{"s/ /\\t/", NULL}, "a b\n", 4, "a\tb\n", 4, 0},
    {{"s/b*/X/g", NULL}, "abc\n", 4, "XaXcX\n", 6, 0},
    {{"s/a*/x/g", NULL}, "baaac\n", 6, "xbxcx\n", 6, 0},
    {{"s/x*/-/g", NULL}, "abc\n", 4, "-a-b-c-\n", 8, 0},
    {{"s/a/b/3", NULL}, "aaa\n", 4, "aab\n", 4, 0},
    {{"s/a/b/4", NULL}, "aaa\n", 4, "aaa\n", 4, 0},
    {{"s/a\\.b/ok/", NULL}, "a.b\n", 4, "ok\n", 3, 0},
    {{"s/a\\|b/X/g", NULL}, "ab\n", 3, "XX\n", 3, 0},
    {{"s/a\\\?b/X/", NULL}, "aab\n", 4, "aX\n", 3, 0},
    {{"s/&/x/", NULL}, "abc\n", 4, "abc\n", 4, 0},
    {{"s/b/[&]/", NULL}, "abc\n", 4, "a[b]c\n", 6, 0},
    {{"s/b/[\\&]/", NULL}, "abc\n", 4, "a[&]c\n", 6, 0},
    {{"s/b/\\\\/", NULL}, "abc\n", 4, "a\\c\n", 4, 0},
    {{"s/b/\\x41/", NULL}, "abc\n", 4, "aAc\n", 4, 0},
    {{"s/\\x62/B/", NULL}, "abc\n", 4, "aBc\n", 4, 0},
    {{"s/\\x2e/!/", NULL}, "a.c\n", 4, "!.c\n", 4, 0},
    {{"-n", "2p", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "two\n", 4, 0},
    {{"-n", "$p", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "five\n", 5, 0},
    {{"2,4d", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "one\nfive\n", 9, 0},
    {{"/two/,/four/d", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "one\nfive\n", 9, 0},
    {{"-n", "/two/,+1p", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "two\nthree\n", 10, 0},
    {{"-n", "1~2p", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "one\nthree\nfive\n", 15, 0},
    {{"-n", "0~2p", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "two\nfour\n", 9, 0},
    {{"-n", "2,~4p", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "two\nthree\nfour\n", 15, 0},
    {{"0,/o/d", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "two\nthree\nfour\nfive\n", 20, 0},
    {{"1,/o/d", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "three\nfour\nfive\n", 16, 0},
    {{"3!d", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "three\n", 6, 0},
    {{"2,3!d", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "two\nthree\n", 10, 0},
    {{"-n", "/t/{p;p}", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "two\ntwo\nthree\nthree\n", 20, 0},
    {{"-n", "/t/{/h/p}", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "three\n", 6, 0},
    {{"=", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "1\none\n2\ntwo\n3\nthree\n4\nfour\n5\nfive\n", 34, 0},
    {{"-n", "$=", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "5\n", 2, 0},
    {{"2q", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "one\ntwo\n", 8, 0},
    {{"2Q", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "one\n", 4, 0},
    {{"2q5", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "one\ntwo\n", 8, 5},
    {{"3a\\\nadded", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "one\ntwo\nthree\nadded\nfour\nfive\n", 30, 0},
    {{"3a added", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "one\ntwo\nthree\nadded\nfour\nfive\n", 30, 0},
    {{"3a\\  indented", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "one\ntwo\nthree\n  indented\nfour\nfive\n", 35, 0},
    {{"3i\\\ninserted", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "one\ntwo\ninserted\nthree\nfour\nfive\n", 33, 0},
    {{"2,3c\\\nchanged", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "one\nchanged\nfour\nfive\n", 22, 0},
    {{"2c\\\nchanged", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "one\nchanged\nthree\nfour\nfive\n", 28, 0},
    {{"$!N;P;D", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "one\ntwo\nthree\nfour\nfive\n", 24, 0},
    {{"N;P;D", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "one\ntwo\nthree\nfour\nfive\n", 24, 0},
    {{":a;N;$!ba;s/\\n/,/g", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "one,two,three,four,five\n", 24, 0},
    {{"-n", "h;n;G;p", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "two\none\nfour\nthree\n", 19, 0},
    {{"1!G;h;$!d", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "five\nfour\nthree\ntwo\none\n", 24, 0},
    {{"-n", "1!G;h;$p", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "five\nfour\nthree\ntwo\none\n", 24, 0},
    {{"x", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "\none\ntwo\nthree\nfour\n", 20, 0},
    {{"n;d", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "one\nthree\nfive\n", 15, 0},
    {{"$!n;s/./X/", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "one\nXwo\nthree\nXour\nXive\n", 24, 0},
    {{"N;N;s/\\n/+/g", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "one+two+three\nfour\nfive\n", 24, 0},
    {{"N;N;N;s/\\n/+/g", NULL}, "one\ntwo\nthree\n", 14, "one\ntwo\nthree\n", 14, 0},
    {{"y/abcdefghijklmnopqrstuvwxyz/ABCDEFGHIJKLMNOPQRSTUVWXYZ/", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "ONE\nTWO\nTHREE\nFOUR\nFIVE\n", 24, 0},
    {{"s/o/0/;ta;s/$/ no/;b;:a;s/$/ yes/", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "0ne yes\ntw0 yes\nthree no\nf0ur yes\nfive no\n", 42, 0},
    {{"s/o/0/;Ta;s/$/ yes/;b;:a;s/$/ no/", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "0ne yes\ntw0 yes\nthree no\nf0ur yes\nfive no\n", 42, 0},
    {{"-n", "l", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "one$\ntwo$\nthree$\nfour$\nfive$\n", 29, 0},
    {{"-n", "l", NULL}, "a\tb\001c\n", 6, "a\\tb\\001c$\n", 11, 0},
    {{"-n", "l", NULL}, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n", 86, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\\\naaaaaaaaaaaaaaaa$\n", 89, 0},
    {{"-n", "l 8", NULL}, "aaaaaaaaaaaaaaaaaaaa\n", 21, "aaaaaaa\\\naaaaaaa\\\naaaaaa$\n", 26, 0},
    {{"-l", "6", "-n", "l", NULL}, "aaaaaaaaaaaaaaaaaaaa\n", 21, "aaaaa\\\naaaaa\\\naaaaa\\\naaaaa$\n", 28, 0},
    {{"z;s/^$/empty/", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "empty\nempty\nempty\nempty\nempty\n", 30, 0},
    {{"-n", "/two/{n;p}", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "three\n", 6, 0},
    {{"/^$/d", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "one\ntwo\nthree\nfour\nfive\n", 24, 0},
    {{"/^$/N;/\\n$/D", NULL}, "a\n\n\n\nb\n\n\nc\n", 11, "a\n\nb\n\nc\n", 8, 0},
    {{"-s", "-n", "$p", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "five\n", 5, 0},
    {{"2{h;d};4G", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "one\nthree\nfour\ntwo\nfive\n", 24, 0},
    {{"/two/I,/FOUR/Id", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "one\nfive\n", 9, 0},
    {{"/one/Id", NULL}, "ONE\ntwo\n", 8, "two\n", 4, 0},
    {{"N;s/^b/X/M", NULL}, "a\nb\n", 4, "a\nX\n", 4, 0},
    {{"N;s/a$/X/M", NULL}, "a\nb\n", 4, "X\nb\n", 4, 0},
    {{"s/b/X/", NULL}, "abc", 3, "aXc", 3, 0},
    {{"p", NULL}, "abc", 3, "abc\nabc", 7, 0},
    {{"$a\\\nend", NULL}, "abc", 3, "abc\nend\n", 8, 0},
    {{"$!d", NULL}, "a\nb", 3, "b", 1, 0},
    {{"-e", "1d", "-e", "2d", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "three\nfour\nfive\n", 16, 0},
    {{"-ne", "2p", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "two\n", 4, 0},
    {{"-n", "-e", "2{", "-e", "p", "-e", "}", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "two\n", 4, 0},
    {{"--expression=3d", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "one\ntwo\nfour\nfive\n", 18, 0},
    {{"--quiet", "-e", "4p", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "four\n", 5, 0},
    {{"#n\n3p", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "three\n", 6, 0},
    {{"-n", "/t/ p", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "two\nthree\n", 10, 0},
    {{"-n", "2,4 !p", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "one\nfive\n", 9, 0},
    {{"2 , 3d", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "one\nfour\nfive\n", 14, 0},
    {{"s/o/O/w /dev/stdout", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "One\nOne\ntwO\ntwO\nthree\nfOur\nfOur\nfive\n", 37, 0},
    {{"-n", "/e/w /dev/stdout", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "one\nthree\nfive\n", 15, 0},
    {{"-E", "s/(a),(b)/\\2,\\1/", NULL}, "a,b\n", 4, "b,a\n", 4, 0},
    {{"-E", "s/x\\+y/ok/", NULL}, "x+y\n", 4, "ok\n", 3, 0},
    {{"s/x+y/ok/", NULL}, "x+y\n", 4, "ok\n", 3, 0},
    {{"s/a\\{2\\}/X/", NULL}, "aaa\n", 4, "Xa\n", 3, 0},
    {{"-E", "s/a{2}/X/", NULL}, "aaa\n", 4, "Xa\n", 3, 0},
    {{"s/[[:space:]]\\+/_/g", NULL}, "a b  c\n", 7, "a_b_c\n", 6, 0},
    {{"s#/#::#g", NULL}, "path/to/file\n", 13, "path::to::file\n", 15, 0},
    {{"s/b/\\\n/", NULL}, "abc\n", 4, "a\nc\n", 4, 0},
    {{"s/\\(.*\\)=\\(.*\\)/\\2=\\1/", NULL}, "k=v\n", 4, "v=k\n", 4, 0},
    {{"-n", "/two/,/NOPE/p", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "two\nthree\nfour\nfive\n", 20, 0},
    {{"-n", "4,2p", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "four\n", 5, 0},
    {{"-n", "2,2p", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "two\n", 4, 0},
    {{"2!{s/^/-/}", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "-one\ntwo\n-three\n-four\n-five\n", 28, 0},
    {{"-n", "/one/,/t/{/t/p}", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "two\n", 4, 0},
    {{"D", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "", 0, 0},
    {{"$d", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "one\ntwo\nthree\nfour\n", 19, 0},
    {{"p", NULL}, "", 0, "", 0, 0},
    {{"p", NULL}, "\n", 1, "\n\n", 2, 0},
    {{"-z", "s/\\n/,/g", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "one,two,three,four,five,", 24, 0},
    {{"-z", "s/^/>/", NULL}, "a\000b\000", 4, ">a\000>b\000", 6, 0},
    {{"k", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "", 0, 1},
    {{"s/a/b/x", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "", 0, 1},
    {{"s/a/b", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "", 0, 1},
    {{"{p", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "", 0, 1},
    {{"p}", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "", 0, 1},
    {{"b nolabel", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "", 0, 4},
    {{"s/\\(a\\)/\\2/", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "", 0, 1},
    {{"/x/,/y/,/z/p", NULL}, "one\ntwo\nthree\nfour\nfive\n", 24, "", 0, 1},
    {{"s/x/a\\tb/", NULL}, "x\n", 2, "a\tb\n", 4, 0},
    {{"a foo\\tbar", NULL}, "x\n", 2, "x\nfoo\tbar\n", 10, 0},
    {{"s/b/x/I", NULL}, "ABC\n", 4, "AxC\n", 4, 0},
    {{"y/B/\\n/", NULL}, "aBc\n", 4, "a\nc\n", 4, 0},
    {{"-n", "s/^[[:space:]]*deviceName[[:space:]]*= //p", NULL}, "  deviceName = Virtio-GPU Venus (Apple A9 GPU)\n  driverName = venus\n", 68, "Virtio-GPU Venus (Apple A9 GPU)\n", 32, 0},
    {{"-n", "s/^OpenGL ES profile version: //p; s/^OpenGL compatibility profile version: //p", NULL}, "OpenGL ES profile version: OpenGL ES 2.0 Mesa\nOpenGL compatibility profile version: 2.1 Mesa\n", 93, "OpenGL ES 2.0 Mesa\n2.1 Mesa\n", 28, 0},
    {{"-n", "/^# Usage:/,/^# Devuan/p", NULL}, "# Usage: x\n# more\n# Devuan\nrest\n", 32, "# Usage: x\n# more\n# Devuan\n", 27, 0},
    {{"-n", "s/^Host Architecture: [^(]*(\\(.*\\))$/\\1/p", NULL}, "Host Architecture: arm64 (aarch64)\n", 35, "aarch64\n", 8, 0},
};

// Runs sed with `args` (NULL-terminated), stdin from `input`; returns the
// exit status and fills `out` (malloc'd) with stdout.
static int run_sed(const char *const *args, const char *input, size_t input_len,
                   char **out, size_t *out_len) {
    const char *in_path = DIR "/in";
    int fd = open(in_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || (input_len && write(fd, input, input_len) != (ssize_t)input_len))
        return -1;
    close(fd);
    int pipefd[2];
    if (pipe(pipefd) != 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        int in = open(in_path, O_RDONLY);
        int null = open("/dev/null", O_WRONLY);
        dup2(in, 0);
        dup2(pipefd[1], 1);
        dup2(null, 2);
        close(pipefd[0]);
        const char *argv[16] = {"sed"};
        int n = 1;
        for (int i = 0; args[i] && n < 15; i++)
            argv[n++] = args[i];
        argv[n] = NULL;
        execv(SED, (char *const *)argv);
        _exit(127);
    }
    close(pipefd[1]);
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    for (;;) {
        if (len == cap) buf = realloc(buf, cap *= 2);
        ssize_t n = read(pipefd[0], buf + len, cap - len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        len += (size_t)n;
    }
    close(pipefd[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    *out = buf;
    *out_len = len;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

static void describe(const struct sed_case *c) {
    printf("  sed");
    for (int i = 0; c->argv[i]; i++)
        printf(" '%s'", c->argv[i]);
    printf("\n");
}

static void print_bytes(const char *label, const char *s, size_t n) {
    printf("  %s (%zu): ", label, n);
    for (size_t i = 0; i < n && i < 120; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == '\n') printf("\\n");
        else if (ch < 32 || ch >= 127) printf("\\%03o", ch);
        else putchar(ch);
    }
    printf("\n");
}

static void write_file(const char *path, const char *text, mode_t mode) {
    unlink(path);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd >= 0) {
        if (write(fd, text, strlen(text)) < 0) {}
        fchmod(fd, mode);
        close(fd);
    }
}

static int file_is(const char *path, const char *text) {
    char buf[256];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 0) return 0;
    buf[n] = '\0';
    return strcmp(buf, text) == 0;
}

static void check(const char *label, int cond) {
    if (cond) {
        test_logf("ok   %s\n", label);
    } else {
        printf("FAIL %s\n", label);
        failures_total++;
    }
}

static void in_place_checks(void) {
    char *out;
    size_t len;
    struct stat st;

    // Mode survives the rewrite (it used to come back 0600 from mkstemp).
    write_file(DIR "/f1", "a1\na2\n", 0640);
    const char *a1[] = {"-i", "s/a/Z/", DIR "/f1", NULL};
    int rc = run_sed(a1, "", 0, &out, &len);
    free(out);
    check("in_place.status", rc == 0);
    check("in_place.content", file_is(DIR "/f1", "Z1\nZ2\n"));
    check("in_place.mode_kept", stat(DIR "/f1", &st) == 0 && (st.st_mode & 07777) == 0640);

    // -iSUFFIX keeps the original beside it.
    write_file(DIR "/f2", "b1\n", 0755);
    const char *a2[] = {"-i.bak", "s/b/Q/", DIR "/f2", NULL};
    rc = run_sed(a2, "", 0, &out, &len);
    free(out);
    check("backup.status", rc == 0);
    check("backup.edited", file_is(DIR "/f2", "Q1\n"));
    check("backup.original", file_is(DIR "/f2.bak", "b1\n"));
    check("backup.mode_kept", stat(DIR "/f2", &st) == 0 && (st.st_mode & 07777) == 0755);

    // -i and -s restart line numbers and $ per file.
    write_file(DIR "/f3", "c1\nc2\n", 0644);
    write_file(DIR "/f4", "d1\nd2\n", 0644);
    const char *a3[] = {"-i", "1d;$s/$/!/", DIR "/f3", DIR "/f4", NULL};
    rc = run_sed(a3, "", 0, &out, &len);
    free(out);
    check("per_file.f3", file_is(DIR "/f3", "c2!\n"));
    check("per_file.f4", file_is(DIR "/f4", "d2!\n"));

    // A missing file is reported, the rest are still processed, status 2.
    const char *a4[] = {"s/c/C/", DIR "/nosuch", DIR "/f3", NULL};
    rc = run_sed(a4, "", 0, &out, &len);
    check("missing_file.status", rc == 2);
    check("missing_file.rest", len == 4 && memcmp(out, "C2!\n", 4) == 0);
    free(out);

    // --follow-symlinks edits the target and leaves the link.
    write_file(DIR "/target", "t1\n", 0644);
    unlink(DIR "/link");
    if (symlink("target", DIR "/link") == 0) {
        const char *a5[] = {"-i", "--follow-symlinks", "s/t/T/", DIR "/link", NULL};
        rc = run_sed(a5, "", 0, &out, &len);
        free(out);
        check("follow_symlinks.target", file_is(DIR "/target", "T1\n"));
        check("follow_symlinks.link_kept", lstat(DIR "/link", &st) == 0 && S_ISLNK(st.st_mode));
    }

    // w FILE, and r FILE appended after the line.
    write_file(DIR "/ins", "INS\n", 0644);
    const char *a6[] = {"-n", "-e", "2r " DIR "/ins", "-e", "p", "-e", "/2/w " DIR "/wout", NULL};
    rc = run_sed(a6, "x1\nx2\nx3\n", 9, &out, &len);
    check("r_file", len == 13 && memcmp(out, "x1\nx2\nINS\nx3\n", 13) == 0);
    free(out);
    check("w_file", file_is(DIR "/wout", "x2\n"));
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    if (access(SMALLCLUE, X_OK) != 0) {
        printf("native_sed: SKIP (no %s)\n", SMALLCLUE);
        return 0;
    }
    alarm(test_watchdog_secs(120));
    mkdir(DIR, 0755);
    unlink(SED);
    if (symlink(SMALLCLUE, SED) != 0) {
        printf("FAIL symlink %s: %s\n", SED, strerror(errno));
        return 1;
    }
    size_t ncases = sizeof(cases) / sizeof(cases[0]);
    for (size_t i = 0; i < ncases; i++) {
        const struct sed_case *c = &cases[i];
        char *out = NULL;
        size_t len = 0;
        int rc = run_sed(c->argv, c->input, c->input_len, &out, &len);
        if (rc == c->expect_status && len == c->expect_len && memcmp(out, c->expect, len) == 0) {
            test_logf("ok   case %zu\n", i);
        } else {
            printf("FAIL case %zu: status %d, expected %d\n", i, rc, c->expect_status);
            describe(c);
            print_bytes("input", c->input, c->input_len);
            print_bytes("expected", c->expect, c->expect_len);
            print_bytes("got", out, len);
            failures_total++;
        }
        free(out);
    }
    in_place_checks();
    return finish_suite("native_sed");
}
