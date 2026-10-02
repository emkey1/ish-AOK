// native_coreutils.c -- SmallCLUE's ls, rm, wc, head, tail, sort, cp, mv,
// date, chmod, xargs, find and grep against the answers of GNU coreutils 9.4,
// findutils 4.9 and grep 3.11.
//
// iSH-AOK's native-links.sh puts SmallCLUE's applets ahead of the distro's on
// PATH, so they run every script that names them. The versions these
// replaced lacked head -c, tail -c and -F, rm -i/-I/-d/-v, sort -k F,F and -o,
// and printed wc's counts in their own widths (2026-10-01).
//
// The table is generated: each case was run through GNU coreutils in
// build/devuan-amd64-test from the same fixture, and its stdout, exit status
// and the resulting tree recorded (tree NULL: unchanged). These expectations
// are GNU's, not this implementation's.
//
// Skips wherever the native multi-call binary is missing, real Linux included.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_common.h"

#define SMALLCLUE "/AOK/native/smallclue"
#define TDIR "/tmp/native_coreutils_test"
#define WORK TDIR "/w"

struct cu_case {
    const char *applet;
    const char *args;   // split on spaces
    const char *input;  // NULL: /dev/null
    size_t input_len;
    const char *expect;
    size_t expect_len;
    int expect_status;
    const char *tree;   // `find . | sort` afterwards; NULL: the fixture's
};

static const struct cu_case cases[] = {
    {"head", "lines.txt", NULL, 0,
     "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\n", 49, 0, NULL},
    {"head", "-n 3 lines.txt", NULL, 0,
     "one\ntwo\nthree\n", 14, 0, NULL},
    {"head", "-3 lines.txt", NULL, 0,
     "one\ntwo\nthree\n", 14, 0, NULL},
    {"head", "-n -3 lines.txt", NULL, 0,
     "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\n", 45, 0, NULL},
    {"head", "-c 5 lines.txt", NULL, 0,
     "one\nt", 5, 0, NULL},
    {"head", "-c -5 lines.txt", NULL, 0,
     "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\neleven\ntw", 58, 0, NULL},
    {"head", "-c 1K lines.txt", NULL, 0,
     "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\neleven\ntwelve\n", 63, 0, NULL},
    {"head", "-c 1kB nonl.txt", NULL, 0,
     "no newline", 10, 0, NULL},
    {"head", "-q lines.txt data.txt", NULL, 0,
     "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\nalpha 3 x\nbeta 1 y\ngamma 2 z\nalpha 3 x\ndelta 10 w\n", 99, 0, NULL},
    {"head", "-v nonl.txt", NULL, 0,
     "==> nonl.txt <==\nno newline", 27, 0, NULL},
    {"head", "-n 2 lines.txt data.txt", NULL, 0,
     "==> lines.txt <==\none\ntwo\n\n==> data.txt <==\nalpha 3 x\nbeta 1 y\n", 63, 0, NULL},
    {"head", "-n 2 lines.txt nosuch data.txt", NULL, 0,
     "==> lines.txt <==\none\ntwo\n\n==> data.txt <==\nalpha 3 x\nbeta 1 y\n", 63, 1, NULL},
    {"head", "-n -1 nonl.txt", NULL, 0,
     "", 0, 0, NULL},
    {"head", "-n 0 lines.txt", NULL, 0,
     "", 0, 0, NULL},
    {"head", "-x lines.txt", NULL, 0,
     "", 0, 1, NULL},
    {"head", "-n abc lines.txt", NULL, 0,
     "", 0, 1, NULL},
    {"head", "-c 99999999999999999999999 nonl.txt", NULL, 0,
     "no newline", 10, 0, NULL},
    {"head", "-n 2", "a\nb\nc\nd\n", 8,
     "a\nb\n", 4, 0, NULL},
    {"head", "-n -1", "a\nb\nc\nd\n", 8,
     "a\nb\nc\n", 6, 0, NULL},
    {"head", "-z -n 2", "a\000b\000c\000", 6,
     "a\000b\000", 4, 0, NULL},
    {"head", "dir", NULL, 0,
     "", 0, 1, NULL},
    {"tail", "lines.txt", NULL, 0,
     "three\nfour\nfive\nsix\nseven\neight\nnine\nten\neleven\ntwelve\n", 55, 0, NULL},
    {"tail", "-n 3 lines.txt", NULL, 0,
     "ten\neleven\ntwelve\n", 18, 0, NULL},
    {"tail", "-3 lines.txt", NULL, 0,
     "ten\neleven\ntwelve\n", 18, 0, NULL},
    {"tail", "+10 lines.txt", NULL, 0,
     "ten\neleven\ntwelve\n", 18, 0, NULL},
    {"tail", "-n +10 lines.txt", NULL, 0,
     "ten\neleven\ntwelve\n", 18, 0, NULL},
    {"tail", "-n +0 nonl.txt", NULL, 0,
     "no newline", 10, 0, NULL},
    {"tail", "-c 5 lines.txt", NULL, 0,
     "elve\n", 5, 0, NULL},
    {"tail", "-c +60 lines.txt", NULL, 0,
     "lve\n", 4, 0, NULL},
    {"tail", "-n 1 nonl.txt", NULL, 0,
     "no newline", 10, 0, NULL},
    {"tail", "-n 0 lines.txt", NULL, 0,
     "", 0, 0, NULL},
    {"tail", "-q lines.txt data.txt", NULL, 0,
     "three\nfour\nfive\nsix\nseven\neight\nnine\nten\neleven\ntwelve\nalpha 3 x\nbeta 1 y\ngamma 2 z\nalpha 3 x\ndelta 10 w\n", 105, 0, NULL},
    {"tail", "-v nonl.txt", NULL, 0,
     "==> nonl.txt <==\nno newline", 27, 0, NULL},
    {"tail", "-n 1 lines.txt nosuch data.txt", NULL, 0,
     "==> lines.txt <==\ntwelve\n\n==> data.txt <==\ndelta 10 w\n", 54, 1, NULL},
    {"tail", "dir", NULL, 0,
     "", 0, 1, NULL},
    {"tail", "-n abc lines.txt", NULL, 0,
     "", 0, 1, NULL},
    {"tail", "-n 2", "a\nb\nc\nd\n", 8,
     "c\nd\n", 4, 0, NULL},
    {"tail", "-n +3", "a\nb\nc\nd\n", 8,
     "c\nd\n", 4, 0, NULL},
    {"tail", "-c 4", "a\nb\nc\nd\n", 8,
     "c\nd\n", 4, 0, NULL},
    {"tail", "-z -n 1", "a\000b\000c", 5,
     "c", 1, 0, NULL},
    {"tail", "-f -n 1", "a\nb\nc\nd\n", 8,
     "d\n", 2, 0, NULL},
    {"tail", "-5c lines.txt", NULL, 0,
     "elve\n", 5, 0, NULL},
    {"wc", "data.txt", NULL, 0,
     " 5 15 50 data.txt\n", 18, 0, NULL},
    {"wc", "-l lines.txt", NULL, 0,
     "12 lines.txt\n", 13, 0, NULL},
    {"wc", "-w text.txt", NULL, 0,
     "7 text.txt\n", 11, 0, NULL},
    {"wc", "-c data.txt", NULL, 0,
     "50 data.txt\n", 12, 0, NULL},
    {"wc", "-m text.txt", NULL, 0,
     "34 text.txt\n", 12, 0, NULL},
    {"wc", "-lw data.txt", NULL, 0,
     " 5 15 data.txt\n", 15, 0, NULL},
    {"wc", "data.txt lines.txt", NULL, 0,
     "  5  15  50 data.txt\n 12  12  63 lines.txt\n 17  27 113 total\n", 61, 0, NULL},
    {"wc", "-l data.txt lines.txt", NULL, 0,
     "  5 data.txt\n 12 lines.txt\n 17 total\n", 37, 0, NULL},
    {"wc", "--total=only data.txt lines.txt", NULL, 0,
     "17 27 113\n", 10, 0, NULL},
    {"wc", "--total=never data.txt lines.txt", NULL, 0,
     "  5  15  50 data.txt\n 12  12  63 lines.txt\n", 43, 0, NULL},
    {"wc", "--total=always data.txt", NULL, 0,
     " 5 15 50 data.txt\n 5 15 50 total\n", 33, 0, NULL},
    {"wc", "dir", NULL, 0,
     "      0       0       0 dir\n", 28, 1, NULL},
    {"wc", "data.txt nosuch", NULL, 0,
     " 5 15 50 data.txt\n 5 15 50 total\n", 33, 1, NULL},
    {"wc", "", "one two\nthree\n", 14,
     "      2       3      14\n", 24, 0, NULL},
    {"wc", "-l", "one two\nthree\n", 14,
     "2\n", 2, 0, NULL},
    {"wc", "-c -", "one two\nthree\n", 14,
     "14 -\n", 5, 0, NULL},
    {"rm", "-v nonl.txt", NULL, 0,
     "removed 'nonl.txt'\n", 19, 0, ".\n./data.txt\n./dir\n./dir/f1\n./dir/sub\n./dir/sub/f2\n./empty\n./empty.txt\n./lines.txt\n./text.txt\n"},
    {"rm", "nosuch", NULL, 0,
     "", 0, 1, NULL},
    {"rm", "-f nosuch", NULL, 0,
     "", 0, 0, NULL},
    {"rm", "dir", NULL, 0,
     "", 0, 1, NULL},
    {"rm", "-d empty", NULL, 0,
     "", 0, 0, ".\n./data.txt\n./dir\n./dir/f1\n./dir/sub\n./dir/sub/f2\n./empty.txt\n./lines.txt\n./nonl.txt\n./text.txt\n"},
    {"rm", "-r .", NULL, 0,
     "", 0, 1, NULL},
    {"rm", "-rf dir", NULL, 0,
     "", 0, 0, ".\n./data.txt\n./empty\n./empty.txt\n./lines.txt\n./nonl.txt\n./text.txt\n"},
    {"rm", "-x nonl.txt", NULL, 0,
     "", 0, 1, NULL},
    {"rm", "-v nonl.txt nosuch empty.txt", NULL, 0,
     "removed 'nonl.txt'\nremoved 'empty.txt'\n", 39, 1, ".\n./data.txt\n./dir\n./dir/f1\n./dir/sub\n./dir/sub/f2\n./empty\n./lines.txt\n./text.txt\n"},
    {"sort", "data.txt", NULL, 0,
     "alpha 3 x\nalpha 3 x\nbeta 1 y\ndelta 10 w\ngamma 2 z\n", 50, 0, NULL},
    {"sort", "-r data.txt", NULL, 0,
     "gamma 2 z\ndelta 10 w\nbeta 1 y\nalpha 3 x\nalpha 3 x\n", 50, 0, NULL},
    {"sort", "-k2,2n data.txt", NULL, 0,
     "beta 1 y\ngamma 2 z\nalpha 3 x\nalpha 3 x\ndelta 10 w\n", 50, 0, NULL},
    {"sort", "-k2n data.txt", NULL, 0,
     "beta 1 y\ngamma 2 z\nalpha 3 x\nalpha 3 x\ndelta 10 w\n", 50, 0, NULL},
    {"sort", "-k1,1 -k2,2nr data.txt", NULL, 0,
     "alpha 3 x\nalpha 3 x\nbeta 1 y\ndelta 10 w\ngamma 2 z\n", 50, 0, NULL},
    {"sort", "-u -k1,1 data.txt", NULL, 0,
     "alpha 3 x\nbeta 1 y\ndelta 10 w\ngamma 2 z\n", 40, 0, NULL},
    {"sort", "-s -k1,1 data.txt", NULL, 0,
     "alpha 3 x\nalpha 3 x\nbeta 1 y\ndelta 10 w\ngamma 2 z\n", 50, 0, NULL},
    {"sort", "-f text.txt", NULL, 0,
     "foo bar baz\nHello World\nHELLO \303\251t\303\251\n", 36, 0, NULL},
    {"sort", "-m lines.txt data.txt", NULL, 0,
     "alpha 3 x\nbeta 1 y\ngamma 2 z\nalpha 3 x\ndelta 10 w\none\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\neleven\ntwelve\n", 113, 0, NULL},
    {"sort", "-c data.txt", NULL, 0,
     "", 0, 1, NULL},
    {"sort", "-C data.txt", NULL, 0,
     "", 0, 1, NULL},
    {"sort", "-o out.txt data.txt", NULL, 0,
     "", 0, 0, ".\n./data.txt\n./dir\n./dir/f1\n./dir/sub\n./dir/sub/f2\n./empty\n./empty.txt\n./lines.txt\n./nonl.txt\n./out.txt\n./text.txt\n"},
    {"sort", "nosuch", NULL, 0,
     "", 0, 2, NULL},
    {"sort", "-k0 data.txt", NULL, 0,
     "", 0, 2, NULL},
    {"sort", "-gn data.txt", NULL, 0,
     "", 0, 2, NULL},
    {"sort", "-x data.txt", NULL, 0,
     "", 0, 2, NULL},
    {"sort", "--sort=bogus data.txt", NULL, 0,
     "", 0, 1, NULL},
    {"sort", "-n", "10\n9\n-3\n2.5\n-0\n0\nabc\n007\n.5\n-.5\n\n 8\n", 36,
     "-3\n-.5\n\n-0\n0\nabc\n.5\n2.5\n007\n 8\n9\n10\n", 36, 0, NULL},
    {"sort", "-un", "10\n9\n-3\n2.5\n-0\n0\nabc\n007\n.5\n-.5\n\n 8\n", 36,
     "-3\n-.5\n-0\n.5\n2.5\n007\n 8\n9\n10\n", 29, 0, NULL},
    {"sort", "-h", "2K\n1G\n500\n1M\n3k\n-1K\n0K\n1.5K\n10\n", 31,
     "-1K\n0K\n10\n500\n1.5K\n2K\n3k\n1M\n1G\n", 31, 0, NULL},
    {"sort", "-V", "a10\na2\na1.10\na1.9\nfile-1.0.tar.gz\nfile-1.0~rc1.tar.gz\nfile-1.0a.tar.gz\n.hidden\n..\n.\n1.0-2\nx~\nx\n", 95,
     ".\n..\n.hidden\n1.0-2\na1.9\na1.10\na2\na10\nfile-1.0~rc1.tar.gz\nfile-1.0.tar.gz\nfile-1.0a.tar.gz\nx~\nx\n", 95, 0, NULL},
    {"sort", "-M", "Mar 3\njan 1\nDEC 9\nfoo\n feb 2\nmaybe\n", 35,
     "foo\njan 1\n feb 2\nMar 3\nmaybe\nDEC 9\n", 35, 0, NULL},
    {"sort", "-g", "1e3\n-inf\nnan\nabc\n0x10\n2.5e-1\ninf\n-5\n", 36,
     "abc\nnan\n-inf\n-5\n2.5e-1\n0x10\n1e3\ninf\n", 36, 0, NULL},
    {"sort", "-k2,2n -k1,1r", "b 2 x\na 10 y\nc 2 a\na 2 z\nB 1 q\nb  3 r\n", 38,
     "B 1 q\nc 2 a\nb 2 x\na 2 z\nb  3 r\na 10 y\n", 38, 0, NULL},
    {"sort", "-k1.2,1.2 -k2b,2", "b 2 x\na 10 y\nc 2 a\na 2 z\nB 1 q\nb  3 r\n", 38,
     "B 1 q\na 10 y\na 2 z\nb 2 x\nc 2 a\nb  3 r\n", 38, 0, NULL},
    {"sort", "-fu -k1,1", "b 2 x\na 10 y\nc 2 a\na 2 z\nB 1 q\nb  3 r\n", 38,
     "a 10 y\nb 2 x\nc 2 a\n", 19, 0, NULL},
    {"sort", "-t: -k2,2n -k3,3r", "x:3:b\ny:1:a\nz:2:c\nw:1:b\nv::d\n", 29,
     "v::d\nw:1:b\ny:1:a\nz:2:c\nx:3:b\n", 29, 0, NULL},
    {"sort", "-d", "a,b\n!a\n_b\nA\nab\n", 15,
     "A\n!a\na,b\nab\n_b\n", 15, 0, NULL},
    {"sort", "-z", "b\000a\000c\000", 6,
     "a\000b\000c\000", 6, 0, NULL},
    {"sort", "-c", "a\nb\na\n", 6,
     "", 0, 1, NULL},
    {"sort", "-cu", "a\na\nb\n", 6,
     "", 0, 1, NULL},
    {"xargs", "", "a b c\nd e\n", 10,
     "a b c d e\n", 10, 0, NULL},
    {"xargs", "-n 2 echo", "a b c\nd e\n", 10,
     "a b\nc d\ne\n", 10, 0, NULL},
    {"xargs", "-L 1 echo", "a b c\nd e\n\nf\n", 13,
     "a b c\nd e\nf\n", 12, 0, NULL},
    {"xargs", "-L 1 echo", "a b \nc\nd\n", 9,
     "a b c\nd\n", 8, 0, NULL},
    {"xargs", "-I {} echo [{}]", "a b c\nd e\n", 10,
     "[a b c]\n[d e]\n", 14, 0, NULL},
    {"xargs", "-I % echo <%>", "  lead\ntrail  \n\n", 16,
     "<lead>\n<trail  >\n", 17, 0, NULL},
    {"xargs", "-n1 echo", "'a b' c\n", 8,
     "a b\nc\n", 6, 0, NULL},
    {"xargs", "-n1 echo", "a\\ b c\n", 7,
     "a b\nc\n", 6, 0, NULL},
    {"xargs", "echo", "a 'b\n", 5,
     "a\n", 2, 1, NULL},
    {"xargs", "-0 -n1 echo", "a\000b c\000", 6,
     "a\nb c\n", 6, 0, NULL},
    {"xargs", "-d: -n1 echo", "a:b:c", 5,
     "a\nb\nc\n", 6, 0, NULL},
    {"xargs", "-E STOP echo", "a b STOP c d\n", 13,
     "a b\n", 4, 0, NULL},
    {"xargs", "echo hi", NULL, 0,
     "hi\n", 3, 0, NULL},
    {"xargs", "-r echo hi", NULL, 0,
     "", 0, 0, NULL},
    {"xargs", "-n1 false", "1 2\n", 4,
     "", 0, 123, NULL},
    {"xargs", "nosuchcommand", "a\n", 2,
     "", 0, 127, NULL},
    {"xargs", "-s 12 echo", "a b c d e f\n", 12,
     "a b c\nd e f\n", 12, 0, NULL},
    {"xargs", "-n 0 echo", "a b\n", 4,
     "", 0, 1, NULL},
    {"xargs", "-n 1 -I {} echo {}", "a b\n", 4,
     "a b\n", 4, 0, NULL},
    {"xargs", "-a lines.txt -n 5 echo", "a b\n", 4,
     "one two three four five\nsix seven eight nine ten\neleven twelve\n", 63, 0, NULL},
    {"find", "dir/sub", NULL, 0,
     "dir/sub\ndir/sub/f2\n", 19, 0, NULL},
    {"find", "-depth dir/sub", NULL, 0,
     "", 0, 1, NULL},
    {"find", ". -name f1", NULL, 0,
     "./dir/f1\n", 9, 0, NULL},
    {"find", ". -name f? -path *sub*", NULL, 0,
     "./dir/sub/f2\n", 13, 0, NULL},
    {"find", ". -iname DATA*", NULL, 0,
     "./data.txt\n", 11, 0, NULL},
    {"find", ". -regex .*/f\\(1\\|x\\)", NULL, 0,
     "./dir/f1\n", 9, 0, NULL},
    {"find", ". -regextype posix-extended -regex .*/(f2|zz)", NULL, 0,
     "./dir/sub/f2\n", 13, 0, NULL},
    {"find", ". -type f -size 0 -name e*", NULL, 0,
     "./empty.txt\n", 12, 0, NULL},
    {"find", ". -empty -type d", NULL, 0,
     "./empty\n", 8, 0, NULL},
    {"find", ". -size +10c -name *s.txt", NULL, 0,
     "./lines.txt\n", 12, 0, NULL},
    {"find", ". -perm 644 -name nonl.txt", NULL, 0,
     "./nonl.txt\n", 11, 0, NULL},
    {"find", ". -name sub -prune", NULL, 0,
     "./dir/sub\n", 10, 0, NULL},
    {"find", "dir -name sub -prune -o -type f -print", NULL, 0,
     "dir/f1\n", 7, 0, NULL},
    {"find", ". -maxdepth 0", NULL, 0,
     ".\n", 2, 0, NULL},
    {"find", ". -mindepth 3", NULL, 0,
     "./dir/sub/f2\n", 13, 0, NULL},
    {"find", ". -name f1 -printf %p|%f|%h|%P|%d|%y|%s|%m\\n", NULL, 0,
     "./dir/f1|f1|./dir|dir/f1|2|f|2|644\n", 35, 0, NULL},
    {"find", "dir -maxdepth 0 -printf %-6f|%6f|%.2f\\n", NULL, 0,
     "dir   |   dir|di\n", 17, 0, NULL},
    {"find", ". -name f1 -printf a\\tb\\\\c%%d\\101\\n", NULL, 0,
     "a\tb\\c%dA\n", 9, 0, NULL},
    {"find", ". -name f1 -exec echo X {} ;", NULL, 0,
     "X ./dir/f1\n", 11, 0, NULL},
    {"find", ". -name f1 -exec echo X {} +", NULL, 0,
     "X ./dir/f1\n", 11, 0, NULL},
    {"find", ". -name f1 -exec false ; -print", NULL, 0,
     "", 0, 0, NULL},
    {"find", ". -name f2 -execdir echo {} ;", NULL, 0,
     "./f2\n", 5, 0, NULL},
    {"find", ". -name f1 -print -quit", NULL, 0,
     "./dir/f1\n", 9, 0, NULL},
    {"find", "dir/sub -delete", NULL, 0,
     "", 0, 0, ".\n./data.txt\n./dir\n./dir/f1\n./empty\n./empty.txt\n./lines.txt\n./nonl.txt\n./text.txt\n"},
    {"find", ". -name nonl.txt -delete", NULL, 0,
     "", 0, 0, ".\n./data.txt\n./dir\n./dir/f1\n./dir/sub\n./dir/sub/f2\n./empty\n./empty.txt\n./lines.txt\n./text.txt\n"},
    {"find", "nosuch", NULL, 0,
     "", 0, 1, NULL},
    {"find", ". -foo", NULL, 0,
     "", 0, 1, NULL},
    {"find", ". -name", NULL, 0,
     "", 0, 1, NULL},
    {"find", ". -type q", NULL, 0,
     "", 0, 1, NULL},
    {"find", ". -size 3x", NULL, 0,
     "", 0, 1, NULL},
    {"find", ". -perm 999", NULL, 0,
     "", 0, 1, NULL},
    {"find", ". -o -print", NULL, 0,
     "", 0, 1, NULL},
    {"find", ". ( -name a", NULL, 0,
     "", 0, 1, NULL},
    {"find", ". -delete -prune", NULL, 0,
     "", 0, 1, NULL},
    {"find", ". -mtime x", NULL, 0,
     "", 0, 1, NULL},
    {"find", ". -newer nosuch", NULL, 0,
     "", 0, 1, NULL},
    {"grep", "-n alpha data.txt", NULL, 0,
     "1:alpha 3 x\n4:alpha 3 x\n", 24, 0, NULL},
    {"grep", "-c alpha data.txt", NULL, 0,
     "2\n", 2, 0, NULL},
    {"grep", "-vi ALPHA data.txt", NULL, 0,
     "beta 1 y\ngamma 2 z\ndelta 10 w\n", 30, 0, NULL},
    {"grep", "-w o text.txt", NULL, 0,
     "", 0, 1, NULL},
    {"grep", "-x foo text.txt", NULL, 0,
     "", 0, 1, NULL},
    {"grep", "-ob o text.txt", NULL, 0,
     "4:o\n7:o\n13:o\n14:o\n", 18, 0, NULL},
    {"grep", "alpha\\|beta data.txt", NULL, 0,
     "alpha 3 x\nbeta 1 y\nalpha 3 x\n", 29, 0, NULL},
    {"grep", "-E (a)\\1 data.txt", NULL, 0,
     "", 0, 1, NULL},
    {"grep", "-F a.b data.txt", NULL, 0,
     "", 0, 1, NULL},
    {"grep", "-e alpha -e delta data.txt", NULL, 0,
     "alpha 3 x\nalpha 3 x\ndelta 10 w\n", 31, 0, NULL},
    {"grep", "-l a data.txt lines.txt text.txt", NULL, 0,
     "data.txt\ntext.txt\n", 18, 0, NULL},
    {"grep", "-L a data.txt lines.txt text.txt", NULL, 0,
     "lines.txt\n", 10, 0, NULL},
    {"grep", "-q alpha data.txt", NULL, 0,
     "", 0, 0, NULL},
    {"grep", "zzz nosuch", NULL, 0,
     "", 0, 2, NULL},
    {"grep", "-q alpha nosuch data.txt", NULL, 0,
     "", 0, 0, NULL},
    {"grep", "-m1 -A2 a data.txt", NULL, 0,
     "alpha 3 x\nbeta 1 y\ngamma 2 z\n", 29, 0, NULL},
    {"grep", "-C1 -n one\\|six\\|ten lines.txt", NULL, 0,
     "1:one\n2-two\n--\n5-five\n6:six\n7-seven\n--\n9-nine\n10:ten\n11-eleven\n", 63, 0, NULL},
    {"grep", "-A1 --group-separator=XX two\\|nine lines.txt", NULL, 0,
     "two\nthree\nXX\nnine\nten\n", 22, 0, NULL},
    {"grep", "-T -n alpha data.txt", NULL, 0,
     " 1:\talpha 3 x\n 4:\talpha 3 x\n", 28, 0, NULL},
    {"grep", "-r x dir", NULL, 0,
     "dir/f1:x\n", 9, 0, NULL},
    {"grep", "-r --include=*.txt -l alpha .", NULL, 0,
     "./data.txt\n", 11, 0, NULL},
    {"grep", "-r --exclude-dir=sub -l . dir", NULL, 0,
     "dir/f1\n", 7, 0, NULL},
    {"grep", "x dir", NULL, 0,
     "", 0, 2, NULL},
    {"grep", "\\w\\+.1 data.txt", NULL, 0,
     "beta 1 y\ndelta 10 w\n", 20, 0, NULL},
    {"grep", "\\d data.txt", NULL, 0,
     "delta 10 w\n", 11, 0, NULL},
    {"grep", "-oP alpha.\\K\\d data.txt", NULL, 0,
     "3\n3\n", 4, 0, NULL},
    {"grep", "-oP (?<=beta.)\\d data.txt", NULL, 0,
     "1\n", 2, 0, NULL},
    {"grep", "-oP \\w+(?=.2) data.txt", NULL, 0,
     "gamma\n", 6, 0, NULL},
    {"grep", "--color=always -n -H -C1 gamma data.txt", NULL, 0,
     "\033[35m\033[Kdata.txt\033[m\033[K\033[36m\033[K-\033[m\033[K\033[32m\033[K2\033[m\033[K\033[36m\033[K-\033[m\033[Kbeta 1 y\n\033[35m\033[Kdata.txt\033[m\033[K\033[36m\033[K:\033[m\033[K\033[32m\033[K3\033[m\033[K\033[36m\033[K:\033[m\033[K\033[01;31m\033[Kgamma\033[m\033[K 2 z\n\033[35m\033[Kdata.txt\033[m\033[K\033[36m\033[K-\033[m\033[K\033[32m\033[K4\033[m\033[K\033[36m\033[K-\033[m\033[Kalpha 3 x\n", 247, 0, NULL},
    {"grep", "-k x data.txt", NULL, 0,
     "", 0, 2, NULL},
    {"grep", "-A x a data.txt", NULL, 0,
     "", 0, 2, NULL},
    {"grep", "alpha", "alpha\nzz\000alpha\n", 15,
     "", 0, 0, NULL},
    {"grep", "-c alpha", "alpha\nzz\000alpha\n", 15,
     "2\n", 2, 0, NULL},
    {"grep", "-I alpha", "alpha\nzz\000alpha\n", 15,
     "", 0, 1, NULL},
    {"grep", "-z a", "a\000b\000ab\000", 7,
     "a\000ab\000", 5, 0, NULL},
    {"cp", "data.txt new.txt", NULL, 0,
     "", 0, 0, ".\n./data.txt\n./dir\n./dir/f1\n./dir/sub\n./dir/sub/f2\n./empty\n./empty.txt\n./lines.txt\n./new.txt\n./nonl.txt\n./text.txt\n"},
    {"cp", "data.txt lines.txt nonl.txt", NULL, 0,
     "", 0, 1, NULL},
    {"cp", "dir newdir", NULL, 0,
     "", 0, 1, NULL},
    {"cp", "-r dir newdir", NULL, 0,
     "", 0, 0, ".\n./data.txt\n./dir\n./dir/f1\n./dir/sub\n./dir/sub/f2\n./empty\n./empty.txt\n./lines.txt\n./newdir\n./newdir/f1\n./newdir/sub\n./newdir/sub/f2\n./nonl.txt\n./text.txt\n"},
    {"cp", "-rv dir dir3", NULL, 0,
     "'dir' -> 'dir3'\n'dir/sub' -> 'dir3/sub'\n'dir/sub/f2' -> 'dir3/sub/f2'\n'dir/f1' -> 'dir3/f1'\n", 92, 0, ".\n./data.txt\n./dir\n./dir/f1\n./dir/sub\n./dir/sub/f2\n./dir3\n./dir3/f1\n./dir3/sub\n./dir3/sub/f2\n./empty\n./empty.txt\n./lines.txt\n./nonl.txt\n./text.txt\n"},
    {"cp", "-r dir dir", NULL, 0,
     "", 0, 1, ".\n./data.txt\n./dir\n./dir/dir\n./dir/dir/f1\n./dir/dir/sub\n./dir/dir/sub/f2\n./dir/f1\n./dir/sub\n./dir/sub/f2\n./empty\n./empty.txt\n./lines.txt\n./nonl.txt\n./text.txt\n"},
    {"cp", "data.txt data.txt", NULL, 0,
     "", 0, 1, NULL},
    {"cp", "-n data.txt lines.txt", NULL, 0,
     "", 0, 0, NULL},
    {"cp", "-vb data.txt lines.txt", NULL, 0,
     "'data.txt' -> 'lines.txt' (backup: 'lines.txt~')\n", 49, 0, ".\n./data.txt\n./dir\n./dir/f1\n./dir/sub\n./dir/sub/f2\n./empty\n./empty.txt\n./lines.txt\n./lines.txt~\n./nonl.txt\n./text.txt\n"},
    {"cp", "--backup=numbered data.txt lines.txt", NULL, 0,
     "", 0, 0, ".\n./data.txt\n./dir\n./dir/f1\n./dir/sub\n./dir/sub/f2\n./empty\n./empty.txt\n./lines.txt\n./lines.txt.~1~\n./nonl.txt\n./text.txt\n"},
    {"cp", "--backup=bad data.txt lines.txt", NULL, 0,
     "", 0, 1, NULL},
    {"cp", "-t dir data.txt lines.txt", NULL, 0,
     "", 0, 0, ".\n./data.txt\n./dir\n./dir/data.txt\n./dir/f1\n./dir/lines.txt\n./dir/sub\n./dir/sub/f2\n./empty\n./empty.txt\n./lines.txt\n./nonl.txt\n./text.txt\n"},
    {"cp", "-T data.txt dir", NULL, 0,
     "", 0, 1, NULL},
    {"cp", "-s data.txt sym", NULL, 0,
     "", 0, 0, ".\n./data.txt\n./dir\n./dir/f1\n./dir/sub\n./dir/sub/f2\n./empty\n./empty.txt\n./lines.txt\n./nonl.txt\n./sym\n./text.txt\n"},
    {"cp", "--parents -v dir/f1 empty", NULL, 0,
     "dir -> empty/dir\n'dir/f1' -> 'empty/dir/f1'\n", 44, 0, ".\n./data.txt\n./dir\n./dir/f1\n./dir/sub\n./dir/sub/f2\n./empty\n./empty.txt\n./empty/dir\n./empty/dir/f1\n./lines.txt\n./nonl.txt\n./text.txt\n"},
    {"cp", "-u lines.txt nonl.txt", NULL, 0,
     "", 0, 0, NULL},
    {"cp", "--update=none-fail data.txt lines.txt", NULL, 0,
     "", 0, 1, NULL},
    {"mv", "data.txt new.txt", NULL, 0,
     "", 0, 0, ".\n./dir\n./dir/f1\n./dir/sub\n./dir/sub/f2\n./empty\n./empty.txt\n./lines.txt\n./new.txt\n./nonl.txt\n./text.txt\n"},
    {"mv", "-v data.txt lines.txt dir", NULL, 0,
     "renamed 'data.txt' -> 'dir/data.txt'\nrenamed 'lines.txt' -> 'dir/lines.txt'\n", 76, 0, ".\n./dir\n./dir/data.txt\n./dir/f1\n./dir/lines.txt\n./dir/sub\n./dir/sub/f2\n./empty\n./empty.txt\n./nonl.txt\n./text.txt\n"},
    {"mv", "dir dir/sub/x", NULL, 0,
     "", 0, 1, NULL},
    {"mv", "-b data.txt lines.txt", NULL, 0,
     "", 0, 0, ".\n./dir\n./dir/f1\n./dir/sub\n./dir/sub/f2\n./empty\n./empty.txt\n./lines.txt\n./lines.txt~\n./nonl.txt\n./text.txt\n"},
    {"mv", "-n data.txt lines.txt", NULL, 0,
     "", 0, 0, NULL},
    {"mv", "-T data.txt dir", NULL, 0,
     "", 0, 1, NULL},
    {"mv", "nosuch x", NULL, 0,
     "", 0, 1, NULL},
    {"mv", "data.txt data.txt", NULL, 0,
     "", 0, 1, NULL},
    {"date", "-u -d @1700000000.123456789 +%F.%T.%N.%Z", NULL, 0,
     "2023-11-14.22:13:20.123456789.UTC\n", 34, 0, NULL},
    {"date", "-u -d 2024-03-13T10:20:30+0230 +%F.%T", NULL, 0,
     "2024-03-13.07:50:30\n", 20, 0, NULL},
    {"date", "-u -d @0 -R", NULL, 0,
     "Thu, 01 Jan 1970 00:00:00 +0000\n", 32, 0, NULL},
    {"date", "-u -d @0 -Ins", NULL, 0,
     "1970-01-01T00:00:00,000000000+00:00\n", 36, 0, NULL},
    {"date", "-u -d @0 --rfc-3339=seconds", NULL, 0,
     "1970-01-01 00:00:00+00:00\n", 26, 0, NULL},
    {"date", "-u -d @1700000000 +%-d|%_m|%^a|%#Z|%:z|%q|%P|%10Y|%e|%j|%U|%W|%V|%G|%s", NULL, 0,
     "14|11|TUE|utc|+00:00|4|pm|0000002023|14|318|46|46|46|2023|1700000000\n", 69, 0, NULL},
    {"date", "-u -d 2024-01-31+1month +%F", NULL, 0,
     "2024-03-02\n", 11, 0, NULL},
    {"date", "-u -d 20240313 +%F", NULL, 0,
     "2024-03-13\n", 11, 0, NULL},
    {"date", "-u -d 3/5/2024 +%F", NULL, 0,
     "2024-03-05\n", 11, 0, NULL},
    {"date", "-u -d 5-Mar-2024 +%F", NULL, 0,
     "2024-03-05\n", 11, 0, NULL},
    {"date", "-u -d foo", NULL, 0,
     "", 0, 1, NULL},
    {"date", "-u -d 2024-02-30", NULL, 0,
     "", 0, 1, NULL},
    {"date", "+%Y +%m", NULL, 0,
     "", 0, 1, NULL},
    {"date", "-I -R", NULL, 0,
     "", 0, 1, NULL},
    {"date", "-u +%Y -d @0", NULL, 0,
     "1970\n", 5, 0, NULL},
    {"chmod", "-v 755 data.txt", NULL, 0,
     "mode of 'data.txt' changed from 0644 (rw-r--r--) to 0755 (rwxr-xr-x)\n", 69, 0, NULL},
    {"chmod", "-v u+x,g-w,o= data.txt", NULL, 0,
     "mode of 'data.txt' changed from 0644 (rw-r--r--) to 0740 (rwxr-----)\n", 69, 0, NULL},
    {"chmod", "-v g=u,o=g data.txt", NULL, 0,
     "mode of 'data.txt' changed from 0644 (rw-r--r--) to 0666 (rw-rw-rw-)\n", 69, 0, NULL},
    {"chmod", "-v a+X data.txt", NULL, 0,
     "mode of 'data.txt' retained as 0644 (rw-r--r--)\n", 48, 0, NULL},
    {"chmod", "-v a+X dir", NULL, 0,
     "mode of 'dir' retained as 0755 (rwxr-xr-x)\n", 43, 0, NULL},
    {"chmod", "-v =x data.txt", NULL, 0,
     "mode of 'data.txt' changed from 0644 (rw-r--r--) to 0111 (--x--x--x)\n", 69, 0, NULL},
    {"chmod", "-v +t dir", NULL, 0,
     "mode of 'dir' changed from 0755 (rwxr-xr-x) to 1755 (rwxr-xr-t)\n", 64, 0, NULL},
    {"chmod", "-v -6000 dir", NULL, 0,
     "mode of 'dir' retained as 0755 (rwxr-xr-x)\n", 43, 0, NULL},
    {"chmod", "-c 644 data.txt", NULL, 0,
     "", 0, 0, NULL},
    {"chmod", "-Rv 700 dir", NULL, 0,
     "mode of 'dir' changed from 0755 (rwxr-xr-x) to 0700 (rwx------)\nmode of 'dir/sub' changed from 0755 (rwxr-xr-x) to 0700 (rwx------)\nmode of 'dir/sub/f2' changed from 0644 (rw-r--r--) to 0700 (rwx------)\nmode of 'dir/f1' changed from 0644 (rw-r--r--) to 0700 (rwx------)\n", 270, 0, NULL},
    {"chmod", "--reference=dir data.txt", NULL, 0,
     "", 0, 0, NULL},
    {"chmod", "xyz data.txt", NULL, 0,
     "", 0, 1, NULL},
    {"chmod", "755 nosuch", NULL, 0,
     "", 0, 1, NULL},
    {"chmod", "755", NULL, 0,
     "", 0, 1, NULL},
    {"ls", "", NULL, 0,
     "data.txt\ndir\nempty\nempty.txt\nlines.txt\nnonl.txt\ntext.txt\n", 57, 0, NULL},
    {"ls", "-a", NULL, 0,
     ".\n..\ndata.txt\ndir\nempty\nempty.txt\nlines.txt\nnonl.txt\ntext.txt\n", 62, 0, NULL},
    {"ls", "-A dir", NULL, 0,
     "f1\nsub\n", 7, 0, NULL},
    {"ls", "-C -w 30", NULL, 0,
     "data.txt  empty.txt  text.txt\ndir\t  lines.txt\nempty\t  nonl.txt\n", 63, 0, NULL},
    {"ls", "-x -w 30", NULL, 0,
     "data.txt   dir\nempty\t   empty.txt\nlines.txt  nonl.txt\ntext.txt\n", 63, 0, NULL},
    {"ls", "-m -w 30", NULL, 0,
     "data.txt, dir, empty,\nempty.txt, lines.txt,\nnonl.txt, text.txt\n", 63, 0, NULL},
    {"ls", "-F", NULL, 0,
     "data.txt\ndir/\nempty/\nempty.txt\nlines.txt\nnonl.txt\ntext.txt\n", 59, 0, NULL},
    {"ls", "-p dir", NULL, 0,
     "f1\nsub/\n", 8, 0, NULL},
    {"ls", "-d dir data.txt", NULL, 0,
     "data.txt\ndir\n", 13, 0, NULL},
    {"ls", "data.txt dir lines.txt", NULL, 0,
     "data.txt\nlines.txt\n\ndir:\nf1\nsub\n", 32, 0, NULL},
    {"ls", "-R dir", NULL, 0,
     "dir:\nf1\nsub\n\ndir/sub:\nf2\n", 25, 0, NULL},
    {"ls", "-r -X", NULL, 0,
     "text.txt\nnonl.txt\nlines.txt\nempty.txt\ndata.txt\nempty\ndir\n", 57, 0, NULL},
    {"ls", "-S", NULL, 0,
     "dir\nempty\nlines.txt\ndata.txt\ntext.txt\nnonl.txt\nempty.txt\n", 57, 0, NULL},
    {"ls", "-v", NULL, 0,
     "data.txt\ndir\nempty\nempty.txt\nlines.txt\nnonl.txt\ntext.txt\n", 57, 0, NULL},
    {"ls", "-Q", NULL, 0,
     "\"data.txt\"\n\"dir\"\n\"empty\"\n\"empty.txt\"\n\"lines.txt\"\n\"nonl.txt\"\n\"text.txt\"\n", 71, 0, NULL},
    {"ls", "-I *.txt", NULL, 0,
     "dir\nempty\n", 10, 0, NULL},
    {"ls", "--group-directories-first", NULL, 0,
     "dir\nempty\ndata.txt\nempty.txt\nlines.txt\nnonl.txt\ntext.txt\n", 57, 0, NULL},
    {"ls", "-l --time-style=+ data.txt lines.txt", NULL, 0,
     "-rw-r--r-- 1 root root 50  data.txt\n-rw-r--r-- 1 root root 63  lines.txt\n", 73, 0, NULL},
    {"ls", "-n --time-style=+ data.txt", NULL, 0,
     "-rw-r--r-- 1 0 0 50  data.txt\n", 30, 0, NULL},
    {"ls", "-go --time-style=+ lines.txt", NULL, 0,
     "-rw-r--r-- 1 63  lines.txt\n", 27, 0, NULL},
    {"ls", "-l --si --time-style=+ lines.txt", NULL, 0,
     "-rw-r--r-- 1 root root 63  lines.txt\n", 37, 0, NULL},
    {"ls", "nosuch", NULL, 0,
     "", 0, 2, NULL},
    {"ls", "nosuch data.txt", NULL, 0,
     "data.txt\n", 9, 2, NULL},
    {"ls", "--sort=bogus", NULL, 0,
     "", 0, 1, NULL},
    {"ls", "-z", NULL, 0,
     "", 0, 2, NULL},
    {"diff", "data.txt data.txt", NULL, 0,
     "", 0, 0, NULL},
    {"diff", "data.txt lines.txt", NULL, 0,
     "1,5c1,12\n< alpha 3 x\n< beta 1 y\n< gamma 2 z\n< alpha 3 x\n< delta 10 w\n---\n> one\n> two\n> three\n> four\n> five\n> six\n> seven\n> eight\n> nine\n> ten\n> eleven\n> twelve\n", 160, 1, NULL},
    {"diff", "-q data.txt lines.txt", NULL, 0,
     "Files data.txt and lines.txt differ\n", 36, 1, NULL},
    {"diff", "-s data.txt data.txt", NULL, 0,
     "Files data.txt and data.txt are identical\n", 42, 0, NULL},
    {"diff", "nonl.txt empty.txt", NULL, 0,
     "1d0\n< no newline\n\\ No newline at end of file\n", 45, 1, NULL},
    {"diff", "empty.txt nonl.txt", NULL, 0,
     "0a1\n> no newline\n\\ No newline at end of file\n", 45, 1, NULL},
    {"diff", "-u --label A --label B data.txt lines.txt", NULL, 0,
     "--- A\n+++ B\n@@ -1,5 +1,12 @@\n-alpha 3 x\n-beta 1 y\n-gamma 2 z\n-alpha 3 x\n-delta 10 w\n+one\n+two\n+three\n+four\n+five\n+six\n+seven\n+eight\n+nine\n+ten\n+eleven\n+twelve\n", 159, 1, NULL},
    {"diff", "-U1 --label A --label B data.txt text.txt", NULL, 0,
     "--- A\n+++ B\n@@ -1,5 +1,3 @@\n-alpha 3 x\n-beta 1 y\n-gamma 2 z\n-alpha 3 x\n-delta 10 w\n+Hello World\n+foo bar baz\n+HELLO \303\251t\303\251\n", 122, 1, NULL},
    {"diff", "-c --label A --label B lines.txt data.txt", NULL, 0,
     "*** A\n--- B\n***************\n*** 1,12 ****\n! one\n! two\n! three\n! four\n! five\n! six\n! seven\n! eight\n! nine\n! ten\n! eleven\n! twelve\n--- 1,5 ----\n! alpha 3 x\n! beta 1 y\n! gamma 2 z\n! alpha 3 x\n! delta 10 w\n", 202, 1, NULL},
    {"diff", "-u --label A --label B nonl.txt data.txt", NULL, 0,
     "--- A\n+++ B\n@@ -1 +1,5 @@\n-no newline\n\\ No newline at end of file\n+alpha 3 x\n+beta 1 y\n+gamma 2 z\n+alpha 3 x\n+delta 10 w\n", 121, 1, NULL},
    {"diff", "-e data.txt lines.txt", NULL, 0,
     "1,5c\none\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\neleven\ntwelve\n.\n", 70, 1, NULL},
    {"diff", "-n data.txt lines.txt", NULL, 0,
     "d1 5\na5 12\none\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\neleven\ntwelve\n", 74, 1, NULL},
    {"diff", "-i text.txt text.txt", NULL, 0,
     "", 0, 0, NULL},
    {"diff", "-y -W 50 data.txt lines.txt", NULL, 0,
     "alpha 3 x\t      |\tone\nbeta 1 y\t      |\ttwo\ngamma 2 z\t      |\tthree\nalpha 3 x\t      |\tfour\ndelta 10 w\t      |\tfive\n\t\t      >\tsix\n\t\t      >\tseven\n\t\t      >\teight\n\t\t      >\tnine\n\t\t      >\tten\n\t\t      >\televen\n\t\t      >\ttwelve\n", 223, 1, NULL},
    {"diff", "-y --suppress-common-lines data.txt text.txt", NULL, 0,
     "alpha 3 x\t\t\t\t\t\t      |\tHello World\nbeta 1 y\t\t\t\t\t\t      |\tfoo bar baz\ngamma 2 z\t\t\t\t\t\t      |\tHELLO \303\251t\303\251\nalpha 3 x\t\t\t\t\t\t      <\ndelta 10 w\t\t\t\t\t\t      <\n", 151, 1, NULL},
    {"diff", "-r dir empty", NULL, 0,
     "Only in dir: f1\nOnly in dir: sub\n", 33, 1, NULL},
    {"diff", "-rN dir empty", NULL, 0,
     "diff -rN dir/f1 empty/f1\n1d0\n< x\ndiff -rN dir/sub/f2 empty/sub/f2\n1d0\n< yy\n", 75, 1, NULL},
    {"diff", "-u --label A --label B - data.txt", NULL, 0,
     "--- A\n+++ B\n@@ -0,0 +1,5 @@\n+alpha 3 x\n+beta 1 y\n+gamma 2 z\n+alpha 3 x\n+delta 10 w\n", 83, 1, NULL},
    {"diff", "-i - data.txt", "alpha 3 x\nBETA 1 y\ngamma 2 z\n", 29,
     "3a4,5\n> alpha 3 x\n> delta 10 w\n", 31, 1, NULL},
    {"diff", "nosuch data.txt", NULL, 0,
     "", 0, 2, NULL},
    {"diff", "-C x data.txt lines.txt", NULL, 0,
     "", 0, 2, NULL},
    {"diff", "data.txt", NULL, 0,
     "", 0, 2, NULL},
    {"diff", "-u -c data.txt lines.txt", NULL, 0,
     "", 0, 2, NULL},
    {"diff", "--bogus data.txt lines.txt", NULL, 0,
     "", 0, 2, NULL},
    {"cmp", "data.txt data.txt", NULL, 0,
     "", 0, 0, NULL},
    {"cmp", "data.txt text.txt", NULL, 0,
     "data.txt text.txt differ: byte 1, line 1\n", 41, 1, NULL},
    {"cmp", "-s data.txt text.txt", NULL, 0,
     "", 0, 1, NULL},
    {"cmp", "-b data.txt text.txt", NULL, 0,
     "data.txt text.txt differ: byte 1, line 1 is 141 a 110 H\n", 56, 1, NULL},
    {"cmp", "-l data.txt text.txt", NULL, 0,
     " 1 141 110\n 2 154 145\n 3 160 154\n 4 150 154\n 5 141 157\n 7  63 127\n 8  40 157\n 9 170 162\n10  12 154\n11 142 144\n12 145  12\n13 164 146\n14 141 157\n15  40 157\n16  61  40\n17  40 142\n18 171 141\n19  12 162\n20 147  40\n21 141 142\n22 155 141\n23 155 172\n24 141  12\n25  40 110\n26  62 105\n27  40 114\n28 172 114\n29  12 117\n30 141  40\n31 154 303\n32 160 251\n33 150 164\n34 141 303\n35  40 251\n36  63  12\n", 385, 1, NULL},
    {"cmp", "-lb data.txt lines.txt", NULL, 0,
     " 1 141 a    157 o\n 2 154 l    156 n\n 3 160 p    145 e\n 4 150 h     12 ^J\n 5 141 a    164 t\n 6  40      167 w\n 7  63 3    157 o\n 8  40       12 ^J\n 9 170 x    164 t\n10  12 ^J   150 h\n11 142 b    162 r\n13 164 t    145 e\n14 141 a     12 ^J\n15  40      146 f\n16  61 1    157 o\n17  40      165 u\n18 171 y    162 r\n20 147 g    146 f\n21 141 a    151 i\n22 155 m    166 v\n23 155 m    145 e\n24 141 a     12 ^J\n25  40      163 s\n26  62 2    151 i\n27  40      170 x\n28 172 z     12 ^J\n29  12 ^J   163 s\n30 141 a    145 e\n31 154 l    166 v\n32 160 p    145 e\n33 150 h    156 n\n34 141 a     12 ^J\n35  40      145 e\n36  63 3    151 i\n37  40      147 g\n38 170 x    150 h\n39  12 ^J   164 t\n40 144 d     12 ^J\n41 145 e    156 n\n42 154 l    151 i\n43 164 t    156 n\n44 141 a    145 e\n45  40       12 ^J\n46  61 1    164 t\n47  60 0    145 e\n48  40      156 n\n49 167 w     12 ^J\n50  12 ^J   145 e\n", 873, 1, NULL},
    {"cmp", "data.txt lines.txt", NULL, 0,
     "data.txt lines.txt differ: byte 1, line 1\n", 42, 1, NULL},
    {"cmp", "-n 3 data.txt text.txt", NULL, 0,
     "data.txt text.txt differ: byte 1, line 1\n", 41, 1, NULL},
    {"cmp", "-i 2:3 data.txt lines.txt", NULL, 0,
     "data.txt lines.txt differ: byte 1, line 1\n", 42, 1, NULL},
    {"cmp", "-i 0x10 data.txt lines.txt", NULL, 0,
     "data.txt lines.txt differ: byte 1, line 1\n", 42, 1, NULL},
    {"cmp", "data.txt lines.txt 2 3", NULL, 0,
     "data.txt lines.txt differ: byte 1, line 1\n", 42, 1, NULL},
    {"cmp", "empty.txt data.txt", NULL, 0,
     "", 0, 1, NULL},
    {"cmp", "nonl.txt text.txt", NULL, 0,
     "nonl.txt text.txt differ: byte 1, line 1\n", 41, 1, NULL},
    {"cmp", "- data.txt", "alpha 3 x\nBETA", 14,
     "- data.txt differ: byte 11, line 2\n", 35, 1, NULL},
    {"cmp", "-l - data.txt", "alpha 3 x\n", 10,
     "", 0, 1, NULL},
    {"cmp", "-i 1m data.txt lines.txt", NULL, 0,
     "", 0, 2, NULL},
    {"cmp", "-n -1 data.txt lines.txt", NULL, 0,
     "", 0, 2, NULL},
    {"cmp", "-s -l data.txt text.txt", NULL, 0,
     "", 0, 2, NULL},
    {"cmp", "nosuch data.txt", NULL, 0,
     "", 0, 2, NULL},
    {"cmp", "data.txt lines.txt 1 2 3", NULL, 0,
     "", 0, 2, NULL},
    {"cmp", "-i 3:4 data.txt data.txt", NULL, 0,
     "data.txt data.txt differ: byte 1, line 1\n", 41, 1, NULL},
    {"cmp", "data.txt dir", NULL, 0,
     "", 0, 2, NULL},
    {"sed", "1e", "echo hi\nb\n", 10,
     "hi\nb\n", 5, 0, NULL},
    {"sed", "s/hi/yo/e", "echo hi\nb\n", 10,
     "yo\nb\n", 5, 0, NULL},
    {"sed", "-n p;1eecho", "a\nb\n", 4,
     "a\n\nb\n", 5, 0, NULL},
    {"sed", "s/a/b/gk", "aa\n", 3,
     "", 0, 1, NULL},
    {"sed", "s/a/b/k", "aa\n", 3,
     "", 0, 1, NULL},
    {"sed", "pk", "aa\n", 3,
     "", 0, 1, NULL},
    {"uniq", "-", "a\na\nb\nB\nc\nc\nc\nd", 15,
     "a\nb\nB\nc\nd\n", 10, 0, NULL},
    {"uniq", "-c", "a\na\nb\nB\nc\nc\nc\nd", 15,
     "      2 a\n      1 b\n      1 B\n      3 c\n      1 d\n", 50, 0, NULL},
    {"uniq", "-d", "a\na\nb\nB\nc\nc\nc\nd", 15,
     "a\nc\n", 4, 0, NULL},
    {"uniq", "-u", "a\na\nb\nB\nc\nc\nc\nd", 15,
     "b\nB\nd\n", 6, 0, NULL},
    {"uniq", "-D", "a\na\nb\nB\nc\nc\nc\nd", 15,
     "a\na\nc\nc\nc\n", 10, 0, NULL},
    {"uniq", "--all-repeated=separate", "a\na\nb\nB\nc\nc\nc\nd", 15,
     "a\na\n\nc\nc\nc\n", 11, 0, NULL},
    {"uniq", "--all-repeated=prepend", "a\na\nb\nB\nc\nc\nc\nd", 15,
     "\na\na\n\nc\nc\nc\n", 12, 0, NULL},
    {"uniq", "--group", "a\na\nb\nB\nc\nc\nc\nd", 15,
     "a\na\n\nb\n\nB\n\nc\nc\nc\n\nd\n", 20, 0, NULL},
    {"uniq", "--group=both", "a\na\nb\nB\nc\nc\nc\nd", 15,
     "\na\na\n\nb\n\nB\n\nc\nc\nc\n\nd\n\n", 22, 0, NULL},
    {"uniq", "-icd", "a\na\nb\nB\nc\nc\nc\nd", 15,
     "      2 a\n      2 b\n      3 c\n", 30, 0, NULL},
    {"uniq", "-cD", "a\na\nb\n", 6,
     "", 0, 1, NULL},
    {"uniq", "--group -c", "a\na\nb\n", 6,
     "", 0, 1, NULL},
    {"uniq", "--group=x", "a\na\nb\n", 6,
     "", 0, 1, NULL},
    {"uniq", "-1 -1", "1 2 a\n3 4 a\n5 6 b\n", 18,
     "1 2 a\n", 6, 0, NULL},
    {"uniq", "-f 2 -c", "1 2 a\n3 4 a\n5 6 b\n", 18,
     "      2 1 2 a\n      1 5 6 b\n", 28, 0, NULL},
    {"uniq", "-s 4 +1", "1 2 a\n3 4 a\n5 6 b\n", 18,
     "1 2 a\n3 4 a\n5 6 b\n", 18, 0, NULL},
    {"uniq", "+1 -s 4", "1 2 a\n3 4 a\n5 6 b\n", 18,
     "1 2 a\n5 6 b\n", 12, 0, NULL},
    {"uniq", "-w1 --group", "aa\nab\nba\nbb\n", 12,
     "aa\nab\n\nba\nbb\n", 13, 0, NULL},
    {"uniq", "-z", "a\000a\000b\000", 6,
     "a\000b\000", 4, 0, NULL},
    {"uniq", "-c data.txt", NULL, 0,
     "      1 alpha 3 x\n      1 beta 1 y\n      1 gamma 2 z\n      1 alpha 3 x\n      1 delta 10 w\n", 90, 0, NULL},
    {"uniq", "-f 1 -c data.txt", NULL, 0,
     "      1 alpha 3 x\n      1 beta 1 y\n      1 gamma 2 z\n      1 alpha 3 x\n      1 delta 10 w\n", 90, 0, NULL},
    {"uniq", "data.txt out.txt", NULL, 0,
     "", 0, 0, ".\n./data.txt\n./dir\n./dir/f1\n./dir/sub\n./dir/sub/f2\n./empty\n./empty.txt\n./lines.txt\n./nonl.txt\n./out.txt\n./text.txt\n"},
    {"uniq", "data.txt out.txt extra", NULL, 0,
     "", 0, 1, NULL},
    {"uniq", "-f x data.txt", NULL, 0,
     "", 0, 1, NULL},
    {"uniq", "nosuch", NULL, 0,
     "", 0, 1, NULL},
    {"tr", "a-z A-Z", "Hello World\n", 12,
     "HELLO WORLD\n", 12, 0, NULL},
    {"tr", "-d lo", "Hello World\n", 12,
     "He Wrd\n", 7, 0, NULL},
    {"tr", "-s a-z", "aa  bb\n", 7,
     "a  b\n", 5, 0, NULL},
    {"tr", "-cs A-Za-z \\n", "Hello, World\n", 13,
     "Hello\nWorld\n", 12, 0, NULL},
    {"tr", "[:lower:] [:upper:]", "Hello World\n", 12,
     "HELLO WORLD\n", 12, 0, NULL},
    {"tr", "\\141-\\143 A-C", "abc\n", 4,
     "ABC\n", 4, 0, NULL},
    {"tr", "-t abcdef xy", "abcdef\n", 7,
     "xycdef\n", 7, 0, NULL},
    {"tr", "abcdef xy", "abcdef\n", 7,
     "xyyyyy\n", 7, 0, NULL},
    {"tr", "a [b*]c", "abc\n", 4,
     "cbc\n", 4, 0, NULL},
    {"tr", "a -d", "abc\n", 4,
     "-bc\n", 4, 0, NULL},
    {"tr", "z-a x", "abc\n", 4,
     "", 0, 1, NULL},
    {"tr", "a [:upper:]", "abc\n", 4,
     "", 0, 1, NULL},
    {"tr", "-d a b", "abc\n", 4,
     "", 0, 1, NULL},
    {"tr", "-cd [:lower:]", "aXb\n", 4,
     "ab", 2, 0, NULL},
    {"nl", "-ba -v0", "a\nb\n", 4,
     "     0\ta\n     1\tb\n", 18, 0, NULL},
    {"nl", "-", "a\n\nb\n", 5,
     "     1\ta\n       \n     2\tb\n", 26, 0, NULL},
    {"nl", "-ba -n rz -w3", "a\n\nb\n", 5,
     "001\ta\n002\t\n003\tb\n", 17, 0, NULL},
    {"nl", "-n ln -s :", "a\n\nb\n", 5,
     "1     :a\n       \n2     :b\n", 26, 0, NULL},
    {"nl", "-ba -l2", "\n\n\n\na\n", 6,
     "       \n     1\t\n       \n     2\t\n     3\ta\n", 41, 0, NULL},
    {"nl", "-ha -fa", "h1\n\\:\\:\\:\nH\n\\:\\:\nB1\n\nB2\n\\:\nF\n", 29,
     "     1\th1\n\n     1\tH\n\n     1\tB1\n       \n     2\tB2\n\n     1\tF\n", 59, 0, NULL},
    {"nl", "-p -ha", "h1\n\\:\\:\\:\nH\n\\:\\:\nB1\n", 20,
     "     1\th1\n\n     2\tH\n\n     3\tB1\n", 31, 0, NULL},
    {"nl", "-i 5 -v -3", "a\nb\n", 4,
     "    -3\ta\n     2\tb\n", 18, 0, NULL},
    {"nl", "-bpb", "a\nb\n", 4,
     "       a\n     1\tb\n", 18, 0, NULL},
    {"nl", "-b x", "a\nb\n", 4,
     "", 0, 1, NULL},
    {"nl", "-w 0", "a\nb\n", 4,
     "", 0, 1, NULL},
    {"nl", "data.txt lines.txt", NULL, 0,
     "     1\talpha 3 x\n     2\tbeta 1 y\n     3\tgamma 2 z\n     4\talpha 3 x\n     5\tdelta 10 w\n     6\tone\n     7\ttwo\n     8\tthree\n     9\tfour\n    10\tfive\n    11\tsix\n    12\tseven\n    13\teight\n    14\tnine\n    15\tten\n    16\televen\n    17\ttwelve\n", 232, 0, NULL},
    {"seq", "5", NULL, 0,
     "1\n2\n3\n4\n5\n", 10, 0, NULL},
    {"seq", "-s, 1 2 9", NULL, 0,
     "1,3,5,7,9\n", 10, 0, NULL},
    {"seq", "-w 8 11", NULL, 0,
     "08\n09\n10\n11\n", 12, 0, NULL},
    {"seq", "0 0.1 0.5", NULL, 0,
     "0.0\n0.1\n0.2\n0.3\n0.4\n0.5\n", 24, 0, NULL},
    {"seq", "-f %03g 3", NULL, 0,
     "001\n002\n003\n", 12, 0, NULL},
    {"seq", "1e2 1e2 3e2", NULL, 0,
     "100\n200\n300\n", 12, 0, NULL},
    {"seq", "0x10 0x12", NULL, 0,
     "16\n17\n18\n", 9, 0, NULL},
    {"seq", "9999999999999999999 10000000000000000001", NULL, 0,
     "9999999999999999999\n10000000000000000000\n10000000000000000001\n", 62, 0, NULL},
    {"seq", "-w -.5 .5 1", NULL, 0,
     "-0.5\n00.0\n00.5\n01.0\n", 20, 0, NULL},
    {"seq", "-0 2", NULL, 0,
     "-0\n1\n2\n", 7, 0, NULL},
    {"seq", "3 1", NULL, 0,
     "", 0, 0, NULL},
    {"seq", "1 0 3", NULL, 0,
     "", 0, 1, NULL},
    {"seq", "-f %d 3", NULL, 0,
     "", 0, 1, NULL},
    {"seq", "a", NULL, 0,
     "", 0, 1, NULL},
    {"seq", "-f %.0f 0 1.2 1", NULL, 0,
     "0\n1\n", 4, 0, NULL},
    {"seq", "10 -2.5 0", NULL, 0,
     "10.0\n7.5\n5.0\n2.5\n0.0\n", 21, 0, NULL},
    {"touch", "-d @1000 data.txt", NULL, 0,
     "", 0, 0, NULL},
    {"touch", "-t 200102300405 data.txt", NULL, 0,
     "", 0, 1, NULL},
    {"touch", "-d garbage data.txt", NULL, 0,
     "", 0, 1, NULL},
    {"touch", "-c nope", NULL, 0,
     "", 0, 0, NULL},
    {"touch", "nodir/x", NULL, 0,
     "", 0, 1, NULL},
    {"touch", "--time=bogus data.txt", NULL, 0,
     "", 0, 1, NULL},
    {"touch", "-d @1 -t 200001010000 data.txt", NULL, 0,
     "", 0, 1, NULL},
    {"stat", "-c %a|%A|%F|%s|%h|%n|%N empty.txt", NULL, 0,
     "644|-rw-r--r--|regular empty file|0|1|empty.txt|'empty.txt'\n", 60, 0, NULL},
    {"stat", "-c %F|%a dir", NULL, 0,
     "directory|755\n", 14, 0, NULL},
    {"stat", "--printf %n:%s\\n data.txt lines.txt", NULL, 0,
     "data.txt:50\nlines.txt:63\n", 25, 0, NULL},
    {"stat", "-c %5s|%-5s|%05s data.txt", NULL, 0,
     "   50|50   |00050\n", 18, 0, NULL},
    {"stat", "-L -c %F nosuch", NULL, 0,
     "", 0, 1, NULL},
    {"stat", "-c %5% data.txt", NULL, 0,
     "", 0, 1, NULL},
    {"stat", "-f -c %l /", NULL, 0,
     "255\n", 4, 0, NULL},
    {"realpath", "--relative-to=dir dir/sub/f2", NULL, 0,
     "sub/f2\n", 7, 0, NULL},
    {"realpath", "--relative-to=dir/sub data.txt", NULL, 0,
     "../../data.txt\n", 15, 0, NULL},
    {"realpath", "--relative-base=. data.txt dir/f1 /", NULL, 0,
     "data.txt\ndir/f1\n/\n", 18, 0, NULL},
    {"realpath", "--relative-to=/usr /etc", NULL, 0,
     "../etc\n", 7, 0, NULL},
    {"realpath", "-s --relative-to=/a/b /a/c/d", NULL, 0,
     "../c/d\n", 7, 0, NULL},
    {"realpath", "-e nosuch", NULL, 0,
     "", 0, 1, NULL},
    {"realpath", "nosuch/x", NULL, 0,
     "", 0, 1, NULL},
    {"realpath", "data.txt/", NULL, 0,
     "", 0, 1, NULL},
    {"realpath", "-m --relative-to=. a/b/../c", NULL, 0,
     "a/c\n", 4, 0, NULL},
    {"readlink", "-n dir", NULL, 0,
     "", 0, 1, NULL},
    {"readlink", "data.txt", NULL, 0,
     "", 0, 1, NULL},
    {"readlink", "-f nosuch/x", NULL, 0,
     "", 0, 1, NULL},
    {"readlink", "-m --relative-to=x a", NULL, 0,
     "", 0, 1, NULL},
    {"env", "-i A=1 B=2 env", NULL, 0,
     "A=1\nB=2\n", 8, 0, NULL},
    {"env", "-i - C=4 env", NULL, 0,
     "C=4\n", 4, 0, NULL},
    {"env", "-u HOME -i X=1 env", NULL, 0,
     "X=1\n", 4, 0, NULL},
    {"env", "-S -i\\_Y=2\\_env", NULL, 0,
     "Y=2\n", 4, 0, NULL},
    {"env", "-i -0 A=1 B=2", NULL, 0,
     "A=1\000B=2\000", 8, 0, NULL},
    {"env", "nosuchcmd", NULL, 0,
     "", 0, 127, NULL},
    {"env", "-u A=B true", NULL, 0,
     "", 0, 125, NULL},
    {"env", "-C", NULL, 0,
     "", 0, 125, NULL},
    {"env", "--ignore-signal=FOO true", NULL, 0,
     "", 0, 125, NULL},
    {"sum", "data.txt", NULL, 0,
     "24576     1 data.txt\n", 21, 0, NULL},
    {"sum", "-s data.txt lines.txt", NULL, 0,
     "3755 1 data.txt\n5663 1 lines.txt\n", 33, 0, NULL},
    {"sum", "-", "alpha\n", 6,
     "18540     1 -\n", 14, 0, NULL},
    {"sum", "- data.txt", "alpha\n", 6,
     "18540     1 -\n24576     1 data.txt\n", 35, 0, NULL},
    {"sum", "nosuch", NULL, 0,
     "", 0, 1, NULL},
    {"sum", "dir", NULL, 0,
     "", 0, 1, NULL},
    {"rmdir", "empty", NULL, 0,
     "", 0, 0, ".\n./data.txt\n./dir\n./dir/f1\n./dir/sub\n./dir/sub/f2\n./empty.txt\n./lines.txt\n./nonl.txt\n./text.txt\n"},
    {"rmdir", "dir", NULL, 0,
     "", 0, 1, NULL},
    {"rmdir", "--ignore-fail-on-non-empty dir", NULL, 0,
     "", 0, 0, NULL},
    {"rmdir", "-v empty", NULL, 0,
     "rmdir: removing directory, 'empty'\n", 35, 0, ".\n./data.txt\n./dir\n./dir/f1\n./dir/sub\n./dir/sub/f2\n./empty.txt\n./lines.txt\n./nonl.txt\n./text.txt\n"},
    {"rmdir", "nosuch", NULL, 0,
     "", 0, 1, NULL},
    {"rmdir", "data.txt", NULL, 0,
     "", 0, 1, NULL},
    {"cat", "-A nonl.txt", NULL, 0,
     "no newline", 10, 0, NULL},
    {"cat", "-A", "a\tb\015\n\001\377x\n", 9,
     "a^Ib^M$\n^AM-^?x$\n", 17, 0, NULL},
    {"cat", "-n nonl.txt data.txt", NULL, 0,
     "     1\tno newlinealpha 3 x\n     2\tbeta 1 y\n     3\tgamma 2 z\n     4\talpha 3 x\n     5\tdelta 10 w\n", 95, 0, NULL},
    {"cat", "-s", "a\n\n\n\nb\n", 7,
     "a\n\nb\n", 5, 0, NULL},
    {"cat", "-nE", "a\n\nb", 4,
     "     1\ta$\n     2\t$\n     3\tb", 27, 0, NULL},
    {"cat", "-b text.txt lines.txt", NULL, 0,
     "     1\tHello World\n     2\tfoo bar baz\n     3\tHELLO \303\251t\303\251\n     4\tone\n     5\ttwo\n     6\tthree\n     7\tfour\n     8\tfive\n     9\tsix\n    10\tseven\n    11\teight\n    12\tnine\n    13\tten\n    14\televen\n    15\ttwelve\n", 204, 0, NULL},
    {"cat", "-E", "x\015\ny\015", 5,
     "x^M$\ny\015", 7, 0, NULL},
    {"cat", "dir", NULL, 0,
     "", 0, 1, NULL},
    {"cat", "-z", NULL, 0,
     "", 0, 1, NULL},
    {"fold", "-w 5 text.txt", NULL, 0,
     "Hello\n Worl\nd\nfoo b\nar ba\nz\nHELLO\n \303\251t\303\n\251\n", 42, 0, NULL},
    {"fold", "-b -w 3 text.txt", NULL, 0,
     "Hel\nlo \nWor\nld\nfoo\n ba\nr b\naz\nHEL\nLO \n\303\251t\n\303\251\n", 45, 0, NULL},
    {"fold", "-s -w 5", "aaa bbb ccc ddd\n", 16,
     "aaa \nbbb \nccc \nddd\n", 19, 0, NULL},
    {"fold", "-w 4", "ab\tcd\n", 6,
     "ab\n\t\ncd\n", 8, 0, NULL},
    {"fold", "-w 0 text.txt", NULL, 0,
     "", 0, 1, NULL},
    {"tac", "lines.txt", NULL, 0,
     "twelve\neleven\nten\nnine\neight\nseven\nsix\nfive\nfour\nthree\ntwo\none\n", 63, 0, NULL},
    {"tac", "-s :", "a:b:c", 5,
     "cb:a:", 5, 0, NULL},
    {"tac", "-b -s :", "a:b:c", 5,
     ":c:ba", 5, 0, NULL},
    {"tac", "-r -s [0-9]+", "a1b22c", 6,
     "c2b2a1", 6, 0, NULL},
    {"tac", "-rb -s [0-9][0-9]*", "a1b22c", 6,
     "2c21ba", 6, 0, NULL},
    {"tac", "nosuch", NULL, 0,
     "", 0, 1, NULL},
    {"dd", "if=colon.txt conv=ucase status=none", NULL, 0,
     "", 0, 1, NULL},
    {"dd", "if=text.txt conv=swab status=none", NULL, 0,
     "eHll ooWlr\ndof oab rab\nzEHLL O\251\303\303t\n\251", 36, 0, NULL},
    {"dd", "cbs=4 conv=block status=none", "ab\ncdefgh\n\nx", 12,
     "ab  cdef    x   ", 16, 0, NULL},
    {"dd", "cbs=4 conv=unblock status=none", "ab  cdefgh  ", 12,
     "ab\ncdef\ngh\n", 11, 0, NULL},
    {"dd", "conv=ebcdic status=none", "hello", 5,
     "\210\205\223\223\226", 5, 0, NULL},
    {"dd", "if=lines.txt bs=1 skip=4 count=3 status=none", NULL, 0,
     "two", 3, 0, NULL},
    {"dd", "if=data.txt iflag=count_bytes count=3 status=none", NULL, 0,
     "alp", 3, 0, NULL},
    {"dd", "bs=0", NULL, 0,
     "", 0, 1, NULL},
    {"dd", "conv=foo", NULL, 0,
     "", 0, 1, NULL},
    {"od", "-c nonl.txt", NULL, 0,
     "0000000   n   o       n   e   w   l   i   n   e\n0000012\n", 56, 0, NULL},
    {"od", "-tx1z data.txt", NULL, 0,
     "0000000 61 6c 70 68 61 20 33 20 78 0a 62 65 74 61 20 31  >alpha 3 x.beta 1<\n0000020 20 79 0a 67 61 6d 6d 61 20 32 20 7a 0a 61 6c 70  > y.gamma 2 z.alp<\n0000040 68 61 20 33 20 78 0a 64 65 6c 74 61 20 31 30 20  >ha 3 x.delta 10 <\n0000060 77 0a                                            >w.<\n0000062\n", 298, 0, NULL},
    {"od", "-td1 -tx1 -tc nonl.txt", NULL, 0,
     "0000000  110  111   32  110  101  119  108  105  110  101\n          6e   6f   20   6e   65   77   6c   69   6e   65\n           n    o         n    e    w    l    i    n    e\n0000012\n", 182, 0, NULL},
    {"od", "-to2 -tx1 nonl.txt", NULL, 0,
     "0000000 067556 067040 073545 064554 062556\n         6e 6f  20 6e  65 77  6c 69  6e 65\n0000012\n", 94, 0, NULL},
    {"od", "-An -w8 -tx1 lines.txt", NULL, 0,
     " 6f 6e 65 0a 74 77 6f 0a\n 74 68 72 65 65 0a 66 6f\n 75 72 0a 66 69 76 65 0a\n 73 69 78 0a 73 65 76 65\n 6e 0a 65 69 67 68 74 0a\n 6e 69 6e 65 0a 74 65 6e\n 0a 65 6c 65 76 65 6e 0a\n 74 77 65 6c 76 65 0a\n", 197, 0, NULL},
    {"od", "-Ad -j3 -N4 -c data.txt", NULL, 0,
     "0000003   h   a       3\n0000007\n", 32, 0, NULL},
    {"od", "nonl.txt +2", NULL, 0,
     "0000002 067040 073545 064554 062556\n0000012\n", 44, 0, NULL},
    {"od", "-s nonl.txt", NULL, 0,
     "0000000  28526  28192  30565  26988  25966\n0000012\n", 51, 0, NULL},
    {"od", "-c", "\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000x", 49,
     "0000000  \\0  \\0  \\0  \\0  \\0  \\0  \\0  \\0  \\0  \\0  \\0  \\0  \\0  \\0  \\0  \\0\n*\n0000060   x\n0000061\n", 94, 0, NULL},
    {"od", "-tx3 data.txt", NULL, 0,
     "", 0, 1, NULL},
    {"split", "-n 2/3 lines.txt", NULL, 0,
     "ve\nsix\nseven\neight\nni", 21, 0, NULL},
    {"split", "-n l/2/3 lines.txt", NULL, 0,
     "six\nseven\neight\nnine\n", 21, 0, NULL},
    {"split", "-n r/2/3 lines.txt", NULL, 0,
     "two\nfive\neight\neleven\n", 22, 0, NULL},
    {"split", "-l 0 lines.txt", NULL, 0,
     "", 0, 1, NULL},
    {"split", "-n 3/2 lines.txt", NULL, 0,
     "", 0, 1, NULL},
    {"split", "-l 5 -d lines.txt sd_", NULL, 0,
     "", 0, 0, ".\n./data.txt\n./dir\n./dir/f1\n./dir/sub\n./dir/sub/f2\n./empty\n./empty.txt\n./lines.txt\n./nonl.txt\n./sd_00\n./sd_01\n./sd_02\n./text.txt\n"},
    {"split", "-C 20 lines.txt sc_", NULL, 0,
     "", 0, 0, ".\n./data.txt\n./dir\n./dir/f1\n./dir/sub\n./dir/sub/f2\n./empty\n./empty.txt\n./lines.txt\n./nonl.txt\n./sc_aa\n./sc_ab\n./sc_ac\n./sc_ad\n./text.txt\n"},
    {"split", "-n l/3 lines.txt sl_", NULL, 0,
     "", 0, 0, ".\n./data.txt\n./dir\n./dir/f1\n./dir/sub\n./dir/sub/f2\n./empty\n./empty.txt\n./lines.txt\n./nonl.txt\n./sl_aa\n./sl_ab\n./sl_ac\n./text.txt\n"},
    {"split", "--verbose -b 30 lines.txt v_", NULL, 0,
     "creating file 'v_aa'\ncreating file 'v_ab'\ncreating file 'v_ac'\n", 63, 0, ".\n./data.txt\n./dir\n./dir/f1\n./dir/sub\n./dir/sub/f2\n./empty\n./empty.txt\n./lines.txt\n./nonl.txt\n./text.txt\n./v_aa\n./v_ab\n./v_ac\n"},
    {"du", "-ab dir", NULL, 0,
     "3\tdir/sub/f2\n3\tdir/sub\n2\tdir/f1\n5\tdir\n", 38, 0, NULL},
    {"du", "-sb dir", NULL, 0,
     "5\tdir\n", 6, 0, NULL},
    {"du", "-b data.txt lines.txt", NULL, 0,
     "50\tdata.txt\n63\tlines.txt\n", 25, 0, NULL},
    {"du", "--inodes -s dir", NULL, 0,
     "4\tdir\n", 6, 0, NULL},
    {"du", "-s -a dir", NULL, 0,
     "", 0, 1, NULL},
    {"du", "nosuch", NULL, 0,
     "", 0, 1, NULL},
    {"gzip", "-c -n data.txt", NULL, 0,
     "\037\213\010\000\000\000\000\000\000\003K\314)\310HT0V\250\340JJ-IT0T\250\344JO\314\315MT0R\250\342J\204K\246\244\346\200d\015\024\312\271\000\273Q\356\2612\000\000\000", 60, 0, NULL},
    {"gzip", "-9 -c -n lines.txt", NULL, 0,
     "\037\213\010\000\000\000\000\000\002\003\035\312I\n\0000\010\004\301\373\374s\022\205\240\020\315\362\374,\307n\312\215\310\345H\351$\212\217\216\242\223\010\335\010N\032\250U\022\246\017\276l\377\346b\273\354\000V\026!\206?\000\000\000", 70, 0, NULL},
    {"gzip", "-dc", "\037\213\010\000\000\000\000\000\000\003\313\310\344\002\000zzo\355\003\000\000\000", 23,
     "hi\n", 3, 0, NULL},
    {"gzip", "-dc", "junk\n", 5,
     "", 0, 1, NULL},
    {"gzip", "-dcf", "junk\n", 5,
     "junk\n", 5, 0, NULL},
    {"gzip", "-k data.txt", NULL, 0,
     "", 0, 0, ".\n./data.txt\n./data.txt.gz\n./dir\n./dir/f1\n./dir/sub\n./dir/sub/f2\n./empty\n./empty.txt\n./lines.txt\n./nonl.txt\n./text.txt\n"},
    {"gzip", "dir", NULL, 0,
     "", 0, 2, NULL},
    {"gzip", "nosuch", NULL, 0,
     "", 0, 1, NULL},
    {"gunzip", "data.txt", NULL, 0,
     "", 0, 2, NULL},
    {"zcat", "-", "\037\213\010\000\000\000\000\000\000\003\313\310\344\002\000zzo\355\003\000\000\000", 23,
     "hi\n", 3, 0, NULL},
    {"tar", "cf a.tar dir data.txt", NULL, 0,
     "", 0, 0, ".\n./a.tar\n./data.txt\n./dir\n./dir/f1\n./dir/sub\n./dir/sub/f2\n./empty\n./empty.txt\n./lines.txt\n./nonl.txt\n./text.txt\n"},
    {"tar", "czf a.tgz dir", NULL, 0,
     "", 0, 0, ".\n./a.tgz\n./data.txt\n./dir\n./dir/f1\n./dir/sub\n./dir/sub/f2\n./empty\n./empty.txt\n./lines.txt\n./nonl.txt\n./text.txt\n"},
    {"tar", "xf nosuch.tar", NULL, 0,
     "", 0, 2, NULL},
    {"tar", "tf data.txt", NULL, 0,
     "", 0, 2, NULL},
    {"tar", "-c", NULL, 0,
     "", 0, 2, NULL},
    {"tar", "cf a.tar --exclude=sub dir", NULL, 0,
     "", 0, 0, ".\n./a.tar\n./data.txt\n./dir\n./dir/f1\n./dir/sub\n./dir/sub/f2\n./empty\n./empty.txt\n./lines.txt\n./nonl.txt\n./text.txt\n"},
    {"tar", "-cf a.tar -C dir f1 sub", NULL, 0,
     "", 0, 0, ".\n./a.tar\n./data.txt\n./dir\n./dir/f1\n./dir/sub\n./dir/sub/f2\n./empty\n./empty.txt\n./lines.txt\n./nonl.txt\n./text.txt\n"},
    /* awk: mawk 1.3.4 (Devuan's awk) is the reference */
    {"awk", "BEGIN{x=1;x+=x+++(++x);print(x)}", NULL, 0,
     "7\n", 2, 0, NULL},
    {"awk", "BEGIN{i=1;a[i++]+=5;print(i,a[1])}", NULL, 0,
     "2 5\n", 4, 0, NULL},
    {"awk", "BEGIN{print(-2^2,!0+1,2^-1,1<2<3,!0&&0)}", NULL, 0,
     "-4 2 0.5 1 0\n", 13, 0, NULL},
    {"awk", "BEGIN{printf(\"[%*d][%-*s][%.*f]\\n\",5,42,4,\"ab\",2,3.14159)}", NULL, 0,
     "[   42][ab  ][3.14]\n", 20, 0, NULL},
    {"awk", "BEGIN{printf(\"%d|%d\\n\",1)}END{print(\"e\")}", NULL, 0,
     "1|", 2, 2, NULL},
    {"awk", "BEGIN{print(substr(\"x\"))}", NULL, 0,
     "", 0, 2, NULL},
    {"awk", "BEGIN{print(length(1,2))}", NULL, 0,
     "", 0, 2, NULL},
    {"awk", "BEGIN{s=\"aaa\";gsub(/x*/,\"-\",s);t=\"hello\";gsub(/l*/,\"X\",t);print(s,t)}", NULL, 0,
     "-a-a-a- XhXeXoX\n", 16, 0, NULL},
    {"awk", "BEGIN{print(\"0x10\"+0,\"+inf\"+0,\"1e3\"+0,\".5\"+0)}", NULL, 0,
     "0 0 1000 0.5\n", 13, 0, NULL},
    {"awk", "BEGIN{print(strftime(\"%Y-%m-%d.%H\",86400*365,1),mktime(\"x\"),length(strftime()))}", NULL, 0,
     "1971-01-01.00 -1 24\n", 20, 0, NULL},
    {"awk", "{while((getline<\"data.txt\")>0)n++;print(NR,n,$1)}", "a\nb\n", 4,
     "1 5 delta\n2 5 b\n", 16, 0, NULL},
    {"awk", "BEGIN{print(\"x\")>\"/dev/stderr\";print(\"y\")>\"/dev/stdout\"}", NULL, 0,
     "y\n", 2, 0, NULL},
    {"awk", "BEGIN{print(\"a\")>\"/nonexistent/x\"}", NULL, 0,
     "", 0, 2, NULL},
    {"awk", "BEGIN{print((getline<\"/nonexist\"))}", NULL, 0,
     "-1\n", 3, 0, NULL},
    {"awk", "{", NULL, 0,
     "", 0, 2, NULL},
    {"awk", "BEGIN{ARGV[1]=\"data.txt\";ARGC=2}{print($1)}", NULL, 0,
     "alpha\nbeta\ngamma\nalpha\ndelta\n", 29, 0, NULL},
    {"awk", "-v s=a\\tb\\\\n BEGIN{print(s)}", NULL, 0,
     "a\tb\\n\n", 6, 0, NULL},
    {"awk", "BEGIN{RS=\"[0-9]+\"}{print(NR,$0)}", "x1y22z333", 9,
     "1 x\n2 y\n3 z\n", 12, 0, NULL},
    {"awk", "BEGIN{CONVFMT=\"%.2g\";a[0.1234567]=1;print(a[\"0.12\"],a[0.1234567])}", NULL, 0,
     "1 1\n", 4, 0, NULL},
    {"awk", "-F\\t {print(NF,$4)}", "a\tb\t\tc\n", 7,
     "4 c\n", 4, 0, NULL},
    {"awk", "{printf(\"%c%c\\n\",$1,$2)}", "65 x\n", 5,
     "Ax\n", 3, 0, NULL},
    {"awk", "1 nosuchfile", NULL, 0,
     "", 0, 2, NULL},
    {"awk", "NR==2{printf(\"%d%d\\n\",1)}{print}", "l1\nl2\n", 6,
     "l1\n1", 4, 2, NULL},
    {"awk", "BEGIN{x[\"a\"];print(length(x))}", NULL, 0,
     "1\n", 2, 0, NULL},
    {"awk", "BEGIN{print(1/0,-1/0)}", NULL, 0,
     "+inf -inf\n", 10, 0, NULL},
};

static void write_file(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        if (write(fd, text, strlen(text)) < 0) {}
        close(fd);
    }
}

static void rm_rf(const char *path) {
    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/rm", "rm", "-rf", path, (char *)NULL);
        _exit(127);
    }
    int st;
    waitpid(pid, &st, 0);
}

// The fixture every case starts from, as the generator made it.
static void fixture(void) {
    rm_rf(WORK);
    mkdir(WORK, 0755);
    if (chdir(WORK) != 0) return;
    write_file("data.txt", "alpha 3 x\nbeta 1 y\ngamma 2 z\nalpha 3 x\ndelta 10 w\n");
    write_file("lines.txt", "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\neleven\ntwelve\n");
    write_file("text.txt", "Hello World\nfoo bar baz\nHELLO \303\251t\303\251\n");
    write_file("nonl.txt", "no newline");
    write_file("empty.txt", "");
    mkdir("dir", 0755);
    mkdir("dir/sub", 0755);
    mkdir("empty", 0755);
    write_file("dir/f1", "x\n");
    write_file("dir/sub/f2", "yy\n");
}

struct names { char **v; size_t n, cap; };

static void walk(const char *rel, struct names *out) {
    if (out->n == out->cap) {
        out->cap = out->cap ? out->cap * 2 : 32;
        out->v = realloc(out->v, out->cap * sizeof(char *));
    }
    out->v[out->n++] = strdup(rel);
    DIR *d = opendir(rel);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", rel, e->d_name);
        walk(path, out);
    }
    closedir(d);
}

static int by_name(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

// `find . | sort` of the work directory.
static char *tree(void) {
    struct names n = {0};
    walk(".", &n);
    qsort(n.v, n.n, sizeof(char *), by_name);
    size_t len = 1;
    for (size_t i = 0; i < n.n; i++) len += strlen(n.v[i]) + 1;
    char *s = malloc(len), *p = s;
    for (size_t i = 0; i < n.n; i++) {
        p += sprintf(p, "%s\n", n.v[i]);
        free(n.v[i]);
    }
    *p = '\0';
    free(n.v);
    return s;
}

// Runs one case in WORK; status returned, stdout in out/out_len.
static int run_case(const struct cu_case *c, char *out, size_t out_size, size_t *out_len) {
    char argbuf[256], applet[256];
    const char *argv[24];
    int n = 0;
    snprintf(applet, sizeof(applet), TDIR "/bin/%s", c->applet);
    argv[n++] = c->applet;
    snprintf(argbuf, sizeof(argbuf), "%s", c->args);
    for (char *t = strtok(argbuf, " "); t && n < 23; t = strtok(NULL, " "))
        argv[n++] = t;
    argv[n] = NULL;
    int outp[2], inp[2];
    if (pipe(outp) != 0 || pipe(inp) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        dup2(c->input ? inp[0] : null, 0);
        dup2(outp[1], 1);
        dup2(null, 2);
        close(inp[1]);
        close(outp[0]);
        setenv("LC_ALL", "C.UTF-8", 1);
        execv(applet, (char *const *)argv);
        _exit(127);
    }
    close(inp[0]);
    close(outp[1]);
    if (c->input && write(inp[1], c->input, c->input_len) < 0) {}
    close(inp[1]);
    size_t len = 0;
    for (;;) {
        ssize_t r = read(outp[0], out + len, out_size - 1 - len);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        len += (size_t)r;
    }
    out[len] = '\0';
    *out_len = len;
    close(outp[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

static void print_bytes(const char *label, const char *s, size_t n) {
    printf("  %s: \"", label);
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == '\n') printf("\\n");
        else if (ch < 32 || ch >= 127) printf("\\%03o", ch);
        else putchar(ch);
    }
    printf("\"\n");
}

// chmod -R and du -a walk a directory in readdir order, as GNU's fts does,
// and that order is the filesystem's: the table has the order of the root it
// was recorded on, and a 5th-gen iPad's root returned dir/f1 before dir/sub.
// Those cases compare their lines as a set.
static int cmp_line(const void *a, const void *b) {
    return strcmp(*(char *const *) a, *(char *const *) b);
}

static char *sorted_lines(const char *text, size_t len) {
    char *copy = strndup(text, len);
    size_t n = 0, cap = 16;
    char **v = malloc(cap * sizeof(*v));
    for (char *save = NULL, *l = strtok_r(copy, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
        if (n == cap) v = realloc(v, (cap *= 2) * sizeof(*v));
        v[n++] = l;
    }
    qsort(v, n, sizeof(*v), cmp_line);
    char *out = malloc(len + 2);
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        size_t k = strlen(v[i]);
        memcpy(out + o, v[i], k);
        o += k;
        out[o++] = '\n';
    }
    out[o] = 0;
    free(v);
    free(copy);
    return out;
}

static int walk_order_free(const struct cu_case *c) {
    return strcmp(c->applet, "du") == 0 || (strcmp(c->applet, "chmod") == 0 && strstr(c->args, "-R"));
}

// The table was recorded as root, so ls -l/-n name root and 0. Run as anyone
// else (a device leg runs as uid 1000) the fixture belongs to the caller, and
// GNU prints that owner instead: rewrite the expectation to data.txt's owner.
static const char *owner_expect(const char *expect, size_t *len) {
    static char buf[4096];
    struct stat st;
    if (stat("data.txt", &st) != 0 || (st.st_uid == 0 && st.st_gid == 0))
        return expect;
    struct passwd *pw = getpwuid(st.st_uid);
    struct group *gr = getgrgid(st.st_gid);
    char names[256], ids[64];
    snprintf(names, sizeof(names), " %s %s ", pw ? pw->pw_name : "?", gr ? gr->gr_name : "?");
    snprintf(ids, sizeof(ids), " %u %u ", (unsigned) st.st_uid, (unsigned) st.st_gid);
    size_t o = 0;
    for (const char *p = expect; *p && o < sizeof(buf) - 256;) {
        if (strncmp(p, " root root ", 11) == 0) {
            o += (size_t) snprintf(buf + o, sizeof(buf) - o, "%s", names);
            p += 11;
        } else if (strncmp(p, " 1 0 0 ", 7) == 0) {
            o += (size_t) snprintf(buf + o, sizeof(buf) - o, " 1%s", ids);
            p += 7;
        } else {
            buf[o++] = *p++;
        }
    }
    buf[o] = 0;
    *len = o;
    return buf;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    if (access(SMALLCLUE, X_OK) != 0) {
        printf("native_coreutils: SKIP (no %s)\n", SMALLCLUE);
        return 0;
    }
    alarm(test_watchdog_secs(180));
    mkdir(TDIR, 0755);
    mkdir(TDIR "/bin", 0755);
    static const char *const applets[] = {"head", "tail", "wc", "rm", "sort", "xargs", "find", "grep", "cp", "mv", "date", "sudo", "chmod", "ls", "diff", "cmp", "sed", "uniq", "tr", "nl", "seq", "touch", "stat", "realpath", "readlink", "env", "sum", "rmdir", "cat", "fold", "tac", "dd", "od", "split", "du", "gzip", "gunzip", "zcat", "tar", "awk"};
    for (size_t i = 0; i < sizeof(applets) / sizeof(applets[0]); i++) {
        char link[256];
        snprintf(link, sizeof(link), TDIR "/bin/%s", applets[i]);
        unlink(link);
        if (symlink(SMALLCLUE, link) != 0) {
            printf("FAIL symlink %s: %s\n", link, strerror(errno));
            return 1;
        }
    }
    fixture();
    char *fixture_tree = tree();
    static char out[65536];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const struct cu_case *c = &cases[i];
        size_t len = 0;
        fixture();
        int status = run_case(c, out, sizeof(out), &len);
        char *after = tree();
        const char *want_tree = c->tree ? c->tree : fixture_tree;
        size_t expect_len = c->expect_len;
        const char *expect = strcmp(c->applet, "ls") == 0
            ? owner_expect(c->expect, &expect_len) : c->expect;
        int ok_out = len == expect_len && memcmp(out, expect, len) == 0;
        if (!ok_out && walk_order_free(c) && len == expect_len) {
            char *a = sorted_lines(out, len), *b = sorted_lines(expect, expect_len);
            ok_out = strcmp(a, b) == 0;
            free(a);
            free(b);
        }
        int ok_status = status == c->expect_status;
        int ok_tree = strcmp(after, want_tree) == 0;
        if (ok_out && ok_status && ok_tree) {
            test_logf("ok   %s %s\n", c->applet, c->args);
        } else {
            printf("FAIL %s %s\n", c->applet, c->args);
            if (!ok_out) {
                print_bytes("want", expect, expect_len);
                print_bytes("got ", out, len);
            }
            if (!ok_status) printf("  status want %d got %d\n", c->expect_status, status);
            if (!ok_tree) {
                print_bytes("tree want", want_tree, strlen(want_tree));
                print_bytes("tree got ", after, strlen(after));
            }
            failures_total++;
        }
        free(after);
    }
    free(fixture_tree);

    // Native stat carried whole seconds only (fixed in kernel/native_libc.c,
    // 2026-10-01), so a file 100 ns newer than its reference was "not newer".
    fixture();
    write_file("old", "");
    write_file("new", "");
    struct timespec times[2] = {{1700000000, 100}, {1700000000, 100}};
    utimensat(AT_FDCWD, "old", times, 0);
    times[0].tv_nsec = times[1].tv_nsec = 200;
    utimensat(AT_FDCWD, "new", times, 0);
    {
        static const struct cu_case newer = {"find", "new old -newer old", NULL, 0, "new\n", 4, 0, NULL};
        size_t len = 0;
        int status = run_case(&newer, out, sizeof(out), &len);
        if (status == 0 && len == 4 && !memcmp(out, "new\n", 4)) {
            test_logf("ok   find -newer by nanoseconds\n");
        } else {
            printf("FAIL find -newer by nanoseconds\n");
            print_bytes("got ", out, len);
            failures_total++;
        }
    }

    // cp -p keeps nanoseconds: native utimensat reached the host's libc,
    // with a guest path, until it was routed through the shim (2026-10-01).
    fixture();
    times[0].tv_nsec = times[1].tv_nsec = 123456789;
    utimensat(AT_FDCWD, "data.txt", times, 0);
    {
        static const struct cu_case keep = {"cp", "-p data.txt kept", NULL, 0, "", 0, 0, NULL};
        size_t len = 0;
        int status = run_case(&keep, out, sizeof(out), &len);
        struct stat st;
        if (status == 0 && stat("kept", &st) == 0 && st.st_mtim.tv_sec == 1700000000 &&
            st.st_mtim.tv_nsec == 123456789) {
            test_logf("ok   cp -p keeps nanosecond times\n");
        } else {
            printf("FAIL cp -p keeps nanosecond times (status %d)\n", status);
            failures_total++;
        }
    }

    // touch sets what it is asked to: -d with fractions, -m alone, the -t
    // leap second (GNU: 23:59:60 is the next minute), -r plus a relative -d.
    {
        static const struct { struct cu_case c; long long atime, mtime; long mnsec; } touched[] = {
            {{"touch", "-d @1000.25 data.txt", NULL, 0, "", 0, 0, NULL}, 1000, 1000, 250000000},
            {{"touch", "-m -d @1000 data.txt", NULL, 0, "", 0, 0, NULL}, 1700000000, 1000, 0},
            {{"touch", "-t 197001010000.60 data.txt", NULL, 0, "", 0, 0, NULL}, -1, 60, 0},
            {{"touch", "-r text.txt -d +1hour data.txt", NULL, 0, "", 0, 0, NULL}, -2, 1700003600, 0},
        };
        setenv("TZ", "UTC0", 1);   /* -t is local time */
        for (size_t i = 0; i < sizeof(touched) / sizeof(touched[0]); i++) {
            fixture();
            struct timespec base[2] = {{1700000000, 0}, {1700000000, 0}};
            utimensat(AT_FDCWD, "data.txt", base, 0);
            utimensat(AT_FDCWD, "text.txt", base, 0);
            size_t len = 0;
            int status = run_case(&touched[i].c, out, sizeof(out), &len);
            struct stat st;
            int ok = status == 0 && stat("data.txt", &st) == 0 && st.st_mtim.tv_sec == touched[i].mtime &&
                      st.st_mtim.tv_nsec == touched[i].mnsec &&
                      (touched[i].atime < 0 || st.st_atim.tv_sec == touched[i].atime);
            if (ok) {
                test_logf("ok   touch %s\n", touched[i].c.args);
            } else {
                printf("FAIL touch %s (status %d, mtime %lld.%09ld)\n", touched[i].c.args, status,
                       (long long)st.st_mtim.tv_sec, (long)st.st_mtim.tv_nsec);
                failures_total++;
            }
        }
        unsetenv("TZ");
    }

    // Native local time is the GUEST's zone (kernel/native_tz.c), not the
    // host's. POSIX rule strings need no zoneinfo files, so these hold on
    // every root; the answers are GNU date's.
    static const struct { const char *tz; struct cu_case c; } zoned[] = {
        {"EST5EDT,M3.2.0,M11.1.0", {"date", "-d @1719835200 +%F_%T_%Z_%z", NULL, 0, "2024-07-01_08:00:00_EDT_-0400\n", 30, 0, NULL}},
        {"EST5EDT,M3.2.0,M11.1.0", {"date", "-d @1704067200 +%F_%T_%Z_%z", NULL, 0, "2023-12-31_19:00:00_EST_-0500\n", 30, 0, NULL}},
        {"<+0530>-5:30", {"date", "-d @0 +%F_%T_%Z_%z", NULL, 0, "1970-01-01_05:30:00_+0530_+0530\n", 32, 0, NULL}},
        {"AEST-10AEDT,M10.1.0,M4.1.0/3", {"date", "-d @1719835200 +%F_%T_%Z", NULL, 0, "2024-07-01_22:00:00_AEST\n", 25, 0, NULL}},
        {"CET-1CEST,M3.5.0,M10.5.0/3", {"date", "-d 2024-07-01T12:00 +%s_%Z", NULL, 0, "1719828000_CEST\n", 16, 0, NULL}},
        {"CET-1CEST,M3.5.0,M10.5.0/3", {"date", "-d 2024-10-27T02:30 +%s_%Z", NULL, 0, "1729992600_CET\n", 15, 0, NULL}},
        {"EST5EDT,M3.2.0,M11.1.0", {"date", "-d 2024-03-10T02:30 +%T", NULL, 0, "", 0, 1, NULL}},
    };
    for (size_t i = 0; i < sizeof(zoned) / sizeof(zoned[0]); i++) {
        size_t len = 0;
        setenv("TZ", zoned[i].tz, 1);
        int status = run_case(&zoned[i].c, out, sizeof(out), &len);
        unsetenv("TZ");
        const struct cu_case *c = &zoned[i].c;
        if (status == c->expect_status && len == c->expect_len && !memcmp(out, c->expect, len)) {
            test_logf("ok   TZ=%s date %s\n", zoned[i].tz, c->args);
        } else {
            printf("FAIL TZ=%s date %s\n", zoned[i].tz, c->args);
            print_bytes("want", c->expect, c->expect_len);
            print_bytes("got ", out, len);
            failures_total++;
        }
    }

    // Native sudo: the target's groups (initgroups was a no-op in the shim,
    // so the invoker's stayed), and leading NAME=value arguments as the
    // command's environment, not as the command. Root only: sudo checks
    // policy for anyone else.
    if (geteuid() == 0) {
        /* nobody's own group alone, when the root has a nobody at all. */
        char nobodyGroups[32] = "";
        struct passwd *pw = getpwnam("nobody");
        if (pw) snprintf(nobodyGroups, sizeof(nobodyGroups), "%u\n", (unsigned)pw->pw_gid);
        struct cu_case sudoCases[] = {
            {"sudo", "-u nobody id -G", NULL, 0, nobodyGroups, strlen(nobodyGroups), 0, NULL},
            {"sudo", "FOO=bar printenv FOO", NULL, 0, "bar\n", 4, 0, NULL},
            {"sudo", "LD_PRELOAD=x printenv FOO", NULL, 0, "", 0, 1, NULL},
        };
        for (size_t i = pw ? 0 : 1; i < sizeof(sudoCases) / sizeof(sudoCases[0]); i++) {
            size_t len = 0;
            int status = run_case(&sudoCases[i], out, sizeof(out), &len);
            const struct cu_case *c = &sudoCases[i];
            if (status == c->expect_status && len == c->expect_len && !memcmp(out, c->expect, len)) {
                test_logf("ok   sudo %s\n", c->args);
            } else {
                printf("FAIL sudo %s (status %d)\n", c->args, status);
                print_bytes("got ", out, len);
                failures_total++;
            }
        }
    }
    if (chdir("/") != 0) {}
    rm_rf(TDIR);
    return finish_suite("native_coreutils");
}
