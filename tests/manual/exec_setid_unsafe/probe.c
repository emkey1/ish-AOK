/* Probe: what a traced / no_new_privs exec of a set-id binary runs with.
 *
 *   probe nosudo DIR   as uid 1000: DIR/{plain,suid-own,sgid-grp,sgid-grp-noxgrp}
 *   probe user DIR     as uid 1000: adds the root-owned copies below
 *   sudo probe root DIR
 *
 * DIR (made by the caller):
 *   plain            755  own
 *   suid-own         4755 own
 *   sgid-grp         2755 own:<a supplementary group>
 *   sgid-grp-noxgrp  2745 own:<a supplementary group>
 *   suid-root        4755 root:root
 *   sgid-root        2755 root:root
 *   sgid-root-noxgrp 2745 root:root
 *   suid-sgid-root   6755 root:root
 *   suid-1000        4755 1000:1000
 *   suid-nobody      4755 65534:65534
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/capability.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/fsuid.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PR_SET_NO_NEW_PRIVS
#define PR_SET_NO_NEW_PRIVS 38
#define PR_GET_NO_NEW_PRIVS 39
#endif

struct rep {
    unsigned ruid, euid, suid, fsuid, rgid, egid, sgid, fsgid;
    unsigned long secure, at_uid, at_euid, at_gid, at_egid;
    unsigned long long capeff, capprm;
    long long status_nnp;
    int nnp;
    int seteuid0;
    int pdeath;
};

static long long status_num(const char *key, int base) {
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    long long v = -1;
    size_t kl = strlen(key);
    while (f && fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, kl) == 0) {
            v = (long long) strtoull(line + kl, NULL, base);
            break;
        }
    }
    if (f) fclose(f);
    return v;
}

static int image(int fd) {
    struct rep r;
    uid_t a, b, c;
    gid_t x, y, z;
    getresuid(&a, &b, &c);
    getresgid(&x, &y, &z);
    r.ruid = a; r.euid = b; r.suid = c; r.fsuid = setfsuid(-1);
    r.rgid = x; r.egid = y; r.sgid = z; r.fsgid = setfsgid(-1);
    r.secure = getauxval(AT_SECURE);
    r.at_uid = getauxval(AT_UID);
    r.at_euid = getauxval(AT_EUID);
    r.at_gid = getauxval(AT_GID);
    r.at_egid = getauxval(AT_EGID);
    r.capeff = (unsigned long long) status_num("CapEff:", 16);
    r.capprm = (unsigned long long) status_num("CapPrm:", 16);
    r.status_nnp = status_num("NoNewPrivs:", 10);
    r.nnp = prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0);
    int pd = -1;
    prctl(PR_GET_PDEATHSIG, &pd, 0, 0, 0);
    r.pdeath = pd;
    r.seteuid0 = seteuid(0) == 0 ? 0 : errno;
    return write(fd, &r, sizeof(r)) == sizeof(r) ? 0 : 1;
}

static char dir[200];
static char p_plain[256], p_suid_own[256], p_sgid_grp[256], p_sgid_grp_noxgrp[256];
static char p_suid_root[256], p_sgid_root[256], p_sgid_root_noxgrp[256], p_suid_sgid_root[256];
static char p_suid_1000[256], p_suid_nobody[256];

enum tm { T_NONE, T_TRACEME, T_SEIZE, T_ATTACH, T_SEIZE_DROP, T_ATTACH_DETACH, T_SEIZE_FORK };

static int drop_to_1000(void) {
    if (setgroups(0, NULL) != 0) return -1;
    if (setresgid(1000, 1000, 1000) != 0) return -1;
    if (setresuid(1000, 1000, 1000) != 0) return -1;
    return 0;
}

static int capdrop(unsigned bits_lo) {
    struct __user_cap_header_struct h = {_LINUX_CAPABILITY_VERSION_3, 0};
    struct __user_cap_data_struct d[2];
    if (syscall(SYS_capget, &h, d) != 0) return -1;
    d[0].effective &= ~bits_lo;
    return (int) syscall(SYS_capset, &h, d);
}
static int drop_ptrace_cap(void) { return capdrop(1u << CAP_SYS_PTRACE); }
static int drop_ptrace_setuid_cap(void) { return capdrop((1u << CAP_SYS_PTRACE) | (1u << CAP_SETUID)); }
static int nnp(void) { return prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0); }
static int euid1000(void) { return setresuid(-1, 1000, -1); }
static int nnp_euid1000(void) { return euid1000() || nnp(); }
static int drop_nnp(void) { return drop_to_1000() || nnp(); }
static int pdeath(void) { return prctl(PR_SET_PDEATHSIG, SIGUSR1, 0, 0, 0); }
static int pdeath_nnp(void) { return pdeath() || nnp(); }

struct scen {
    const char *label;
    int (*pre)(void);
    enum tm tm;
    int (*post)(void);
    const char *file;
};

static void show(const char *label, struct rep *r) {
    printf("%-50s ids %u/%u/%u/%u gids %u/%u/%u/%u SEC %lu AT_UID %lu AT_EUID %lu AT_GID %lu AT_EGID %lu eff %llx prm %llx nnp %d/%lld pdeath %d seteuid0 %s\n",
           label, r->ruid, r->euid, r->suid, r->fsuid, r->rgid, r->egid, r->sgid, r->fsgid,
           r->secure, r->at_uid, r->at_euid, r->at_gid, r->at_egid, r->capeff, r->capprm,
           r->nnp, r->status_nnp, r->pdeath, r->seteuid0 == 0 ? "ok" : strerror(r->seteuid0));
}

static void run(const struct scen *s) {
    int rep[2], go[2];
    if (pipe(rep) || pipe(go)) { perror("pipe"); exit(1); }
    fflush(stdout);
    pid_t t = fork();
    if (t == 0) {
        close(rep[0]);
        close(go[1]);
        if (s->pre && s->pre() != 0) { perror("pre"); _exit(3); }
        if (s->tm == T_TRACEME && ptrace(PTRACE_TRACEME, 0, 0, 0) != 0) { perror("traceme"); _exit(4); }
        char c;
        if (s->tm != T_NONE && s->tm != T_TRACEME)
            if (read(go[0], &c, 1) != 1) _exit(5);
        if (s->post && s->post() != 0) { perror("post"); _exit(6); }
        char fd[16];
        snprintf(fd, sizeof(fd), "%d", rep[1]);
        if (s->tm == T_SEIZE_FORK) {
            pid_t g = fork();
            if (g == 0) {
                execl(s->file, s->file, "--image", fd, (char *) NULL);
                perror("exec");
                _exit(127);
            }
            int gs;
            while (waitpid(g, &gs, 0) < 0 && errno == EINTR)
                ;
            _exit(0);
        }
        execl(s->file, s->file, "--image", fd, (char *) NULL);
        perror("exec");
        _exit(127);
    }
    close(rep[1]);
    close(go[0]);
    pid_t tracer = 0;
    if (s->tm == T_SEIZE || s->tm == T_SEIZE_FORK) {
        long opts = s->tm == T_SEIZE_FORK ? PTRACE_O_TRACEFORK : 0;
        if (ptrace(PTRACE_SEIZE, t, 0, opts) != 0) printf("  seize failed: %s\n", strerror(errno));
        if (write(go[1], "g", 1) != 1) perror("go");
    } else if (s->tm == T_ATTACH || s->tm == T_ATTACH_DETACH) {
        if (ptrace(PTRACE_ATTACH, t, 0, 0) != 0) printf("  attach failed: %s\n", strerror(errno));
        int st;
        waitpid(t, &st, __WALL);
        if (s->tm == T_ATTACH_DETACH) {
            if (ptrace(PTRACE_DETACH, t, 0, 0) != 0) printf("  detach failed: %s\n", strerror(errno));
        } else {
            ptrace(PTRACE_CONT, t, 0, 0);
        }
        if (write(go[1], "g", 1) != 1) perror("go");
    } else if (s->tm == T_SEIZE_DROP) {
        tracer = fork();
        if (tracer == 0) {
            if (ptrace(PTRACE_SEIZE, t, 0, 0) != 0) { perror("seize"); _exit(7); }
            if (drop_to_1000() != 0) { perror("drop"); _exit(8); }
            if (write(go[1], "g", 1) != 1) _exit(9);
            int st;
            for (;;) {
                pid_t w = waitpid(t, &st, __WALL);
                if (w < 0) break;
                if (WIFSTOPPED(st)) ptrace(PTRACE_CONT, t, 0, 0);
                else break;
            }
            _exit(0);
        }
    }
    int st = 0;
    bool t_done = false, tracer_done = tracer == 0;
    while (!t_done || !tracer_done) {
        int ws;
        pid_t w = waitpid(-1, &ws, __WALL);
        if (w < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (WIFSTOPPED(ws)) {
            ptrace(PTRACE_CONT, w, 0, 0);
            continue;
        }
        if (w == t) { t_done = true; st = ws; }
        if (w == tracer) tracer_done = true;
    }
    struct rep r;
    ssize_t n = read(rep[0], &r, sizeof(r));
    close(rep[0]);
    close(go[1]);
    if (n != sizeof(r)) {
        printf("%-50s NO REPORT (status %#x)\n", s->label, st);
        return;
    }
    show(s->label, &r);
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--image") == 0)
        return image(atoi(argv[2]));
    if (argc < 3) { fprintf(stderr, "usage: probe nosudo|user|root DIR\n"); return 2; }
    snprintf(dir, sizeof(dir), "%s", argv[2]);
#define P(var, name) snprintf(var, sizeof(var), "%s/" name, dir)
    P(p_plain, "plain");
    P(p_suid_own, "suid-own");
    P(p_sgid_grp, "sgid-grp");
    P(p_sgid_grp_noxgrp, "sgid-grp-noxgrp");
    P(p_suid_root, "suid-root");
    P(p_sgid_root, "sgid-root");
    P(p_sgid_root_noxgrp, "sgid-root-noxgrp");
    P(p_suid_sgid_root, "suid-sgid-root");
    P(p_suid_1000, "suid-1000");
    P(p_suid_nobody, "suid-nobody");
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("probe as uid %d euid %d gid %d, %s\n", getuid(), geteuid(), getgid(),
           sizeof(long) == 8 ? "64-bit" : "32-bit");

    if (strcmp(argv[1], "nosudo") == 0) {
        const struct scen u[] = {
            {"N0 untraced plain", NULL, T_NONE, NULL, p_plain},
            {"N1 untraced sgid-grp", NULL, T_NONE, NULL, p_sgid_grp},
            {"N2 TRACEME sgid-grp", NULL, T_TRACEME, NULL, p_sgid_grp},
            {"N3 parent SEIZE sgid-grp", NULL, T_SEIZE, NULL, p_sgid_grp},
            {"N4 parent ATTACH sgid-grp", NULL, T_ATTACH, NULL, p_sgid_grp},
            {"N5 parent ATTACH+DETACH sgid-grp", NULL, T_ATTACH_DETACH, NULL, p_sgid_grp},
            {"N6 SEIZE+TRACEFORK, grandchild sgid-grp", NULL, T_SEIZE_FORK, NULL, p_sgid_grp},
            {"N7 NNP sgid-grp", nnp, T_NONE, NULL, p_sgid_grp},
            {"N8 NNP plain", nnp, T_NONE, NULL, p_plain},
            {"N9 NNP + TRACEME sgid-grp", nnp, T_TRACEME, NULL, p_sgid_grp},
            {"N10 untraced sgid-grp-noxgrp", NULL, T_NONE, NULL, p_sgid_grp_noxgrp},
            {"N11 TRACEME sgid-grp-noxgrp", NULL, T_TRACEME, NULL, p_sgid_grp_noxgrp},
            {"N12 untraced suid-own", NULL, T_NONE, NULL, p_suid_own},
            {"N13 TRACEME suid-own", NULL, T_TRACEME, NULL, p_suid_own},
            {"N14 NNP suid-own", nnp, T_NONE, NULL, p_suid_own},
            {"P1 PDEATH untraced sgid-grp", pdeath, T_NONE, NULL, p_sgid_grp},
            {"P2 PDEATH TRACEME sgid-grp", pdeath, T_TRACEME, NULL, p_sgid_grp},
            {"P3 PDEATH NNP sgid-grp", pdeath_nnp, T_NONE, NULL, p_sgid_grp},
            {"P4 PDEATH TRACEME plain", pdeath, T_TRACEME, NULL, p_plain},
            {"P5 PDEATH untraced plain", pdeath, T_NONE, NULL, p_plain},
        };
        for (size_t i = 0; i < sizeof(u) / sizeof(u[0]); i++) run(&u[i]);
    } else if (strcmp(argv[1], "user") == 0) {
        const struct scen u[] = {
            {"U1 untraced suid-root", NULL, T_NONE, NULL, p_suid_root},
            {"U2 TRACEME suid-root", NULL, T_TRACEME, NULL, p_suid_root},
            {"U3 untraced sgid-root", NULL, T_NONE, NULL, p_sgid_root},
            {"U4 TRACEME sgid-root", NULL, T_TRACEME, NULL, p_sgid_root},
            {"U5 parent SEIZE suid-root", NULL, T_SEIZE, NULL, p_suid_root},
            {"U6 parent ATTACH suid-root", NULL, T_ATTACH, NULL, p_suid_root},
            {"U7 NNP suid-root", nnp, T_NONE, NULL, p_suid_root},
            {"U8 NNP sgid-root", nnp, T_NONE, NULL, p_sgid_root},
            {"U9 TRACEME suid-nobody", NULL, T_TRACEME, NULL, p_suid_nobody},
            {"U10 untraced suid-nobody", NULL, T_NONE, NULL, p_suid_nobody},
            {"U11 SEIZE+TRACEFORK, grandchild suid-root", NULL, T_SEIZE_FORK, NULL, p_suid_root},
            {"U12 ATTACH+DETACH suid-root", NULL, T_ATTACH_DETACH, NULL, p_suid_root},
            {"U13 untraced sgid-root-noxgrp", NULL, T_NONE, NULL, p_sgid_root_noxgrp},
            {"U14 untraced suid-sgid-root", NULL, T_NONE, NULL, p_suid_sgid_root},
            {"U15 TRACEME suid-sgid-root", NULL, T_TRACEME, NULL, p_suid_sgid_root},
            {"U16 NNP suid-sgid-root", nnp, T_NONE, NULL, p_suid_sgid_root},
        };
        for (size_t i = 0; i < sizeof(u) / sizeof(u[0]); i++) run(&u[i]);
    } else if (strcmp(argv[1], "root") == 0) {
        const struct scen r[] = {
            {"R0 untraced suid-1000", NULL, T_NONE, NULL, p_suid_1000},
            {"R1 TRACEME as root, drop, exec suid-root", NULL, T_TRACEME, drop_to_1000, p_suid_root},
            {"R2 drop, TRACEME, exec suid-root", drop_to_1000, T_TRACEME, NULL, p_suid_root},
            {"R3 drop, root parent SEIZE, suid-root", drop_to_1000, T_SEIZE, NULL, p_suid_root},
            {"R4 drop, root parent ATTACH, suid-root", drop_to_1000, T_ATTACH, NULL, p_suid_root},
            {"R5 drop, root SEIZE then tracer drops", drop_to_1000, T_SEIZE_DROP, NULL, p_suid_root},
            {"R6 root -SYS_PTRACE eff, TRACEME, suid-1000", drop_ptrace_cap, T_TRACEME, NULL, p_suid_1000},
            {"R7 root -SYS_PTRACE-SETUID eff, TRACEME, suid-1000", drop_ptrace_setuid_cap, T_TRACEME, NULL, p_suid_1000},
            {"R8 root euid 1000 then NNP, exec plain", nnp_euid1000, T_NONE, NULL, p_plain},
            {"R9 root NNP, exec suid-1000", nnp, T_NONE, NULL, p_suid_1000},
            {"R10 root euid 1000, TRACEME, exec plain", euid1000, T_TRACEME, NULL, p_plain},
            {"R11 root TRACEME then euid 1000, exec plain", NULL, T_TRACEME, euid1000, p_plain},
            {"R12 root euid 1000 (untraced), exec plain", euid1000, T_NONE, NULL, p_plain},
            {"R13 root TRACEME, exec suid-1000", NULL, T_TRACEME, NULL, p_suid_1000},
            {"R14 drop + TRACEME, sgid-root", drop_to_1000, T_TRACEME, NULL, p_sgid_root},
            {"R15 drop, root SEIZE+TRACEFORK, gchild suid-root", drop_to_1000, T_SEIZE_FORK, NULL, p_suid_root},
            {"R16 drop + NNP, root parent SEIZE, suid-root", drop_nnp, T_SEIZE, NULL, p_suid_root},
            {"R17 root TRACEME, exec plain", NULL, T_TRACEME, NULL, p_plain},
            {"R18 root NNP, exec suid-root", nnp, T_NONE, NULL, p_suid_root},
            {"R19 root TRACEME, exec sgid-grp(own grp)", NULL, T_TRACEME, NULL, p_sgid_grp},
        };
        for (size_t i = 0; i < sizeof(r) / sizeof(r[0]); i++) run(&r[i]);
    }
    return 0;
}
