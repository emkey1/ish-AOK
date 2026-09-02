// SysV message queues + semaphores (fakeroot's IPC substrate).
//
// AOK stubbed msgget/msgsnd/msgrcv/msgctl and semget/semop/semtimedop/semctl
// to ENOSYS, so fakeroot's faked daemon ("while creating message channels:
// Function not implemented") could never start -- which killed every
// makepkg/dpkg-buildpackage package() stage, i.e. all AUR builds under
// pikaur/yay and all dpkg source builds. Covers: msg roundtrip with type
// selection (0 / >0 / <0 / MSG_EXCEPT), E2BIG vs MSG_NOERROR truncation,
// ENOMSG/EAGAIN with IPC_NOWAIT (qbytes shrunk via IPC_SET), blocking
// msgrcv woken by a child's msgsnd, EIDRM delivered to a receiver blocked
// across IPC_RMID, msgctl IPC_STAT counters; sem SETVAL/GETVAL/SETALL/
// GETALL/GETPID, blocking semop woken by a child's up, wait-for-zero,
// EAGAIN with IPC_NOWAIT, SEM_UNDO applied when a child dies holding the
// semaphore, and EIDRM for a semop blocked across IPC_RMID. Also passes on
// real Linux.

#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

#ifndef MSG_EXCEPT // glibc exposes it only under _GNU_SOURCE; musl always
#define MSG_EXCEPT 020000
#endif

union semun_ {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};

struct msgbuf_ {
    long mtype;
    char mtext[64];
};

static void check(int cond, const char *label) {
    if (cond) {
        test_logf("ok: %s\n", label);
        return;
    }
    printf("FAIL %s (errno=%d %s)\n", label, errno, strerror(errno));
    failures_total++;
}

static void msleep_(int ms) {
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void test_msg_basic(void) {
    int id = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
    check(id >= 0, "msgget IPC_PRIVATE");
    if (id < 0)
        return;

    struct msgbuf_ m = { .mtype = 1 };
    strcpy(m.mtext, "one");
    check(msgsnd(id, &m, 4, 0) == 0, "msgsnd type1");
    m.mtype = 2; strcpy(m.mtext, "two");
    check(msgsnd(id, &m, 4, 0) == 0, "msgsnd type2");
    m.mtype = 3; strcpy(m.mtext, "three");
    check(msgsnd(id, &m, 6, 0) == 0, "msgsnd type3");

    struct msqid_ds st;
    check(msgctl(id, IPC_STAT, &st) == 0, "msgctl IPC_STAT");
    check(st.msg_qnum == 3, "IPC_STAT qnum==3");

    // mtype < 1 is EINVAL.
    m.mtype = 0;
    check(msgsnd(id, &m, 1, 0) < 0 && errno == EINVAL, "msgsnd mtype 0 EINVAL");

    // Type selection: exact type, then FIFO, then lowest <= |type|.
    struct msgbuf_ r;
    memset(&r, 0, sizeof(r));
    check(msgrcv(id, &r, sizeof(r.mtext), 2, 0) == 4 && r.mtype == 2 &&
          strcmp(r.mtext, "two") == 0, "msgrcv exact type 2");
    memset(&r, 0, sizeof(r));
    check(msgrcv(id, &r, sizeof(r.mtext), -3, 0) == 4 && r.mtype == 1,
          "msgrcv -3 picks lowest type");
    // MSG_EXCEPT: only type 3 remains; asking for "not 3" must ENOMSG.
    check(msgrcv(id, &r, sizeof(r.mtext), 3, MSG_EXCEPT | IPC_NOWAIT) < 0 &&
          errno == ENOMSG, "msgrcv MSG_EXCEPT skips type 3");
    // E2BIG without MSG_NOERROR, truncation with.
    check(msgrcv(id, &r, 2, 0, 0) < 0 && errno == E2BIG, "msgrcv short E2BIG");
    memset(&r, 0, sizeof(r));
    check(msgrcv(id, &r, 2, 0, MSG_NOERROR) == 2 && r.mtype == 3,
          "msgrcv MSG_NOERROR truncates");
    check(msgrcv(id, &r, sizeof(r.mtext), 0, IPC_NOWAIT) < 0 && errno == ENOMSG,
          "msgrcv empty ENOMSG");

    check(msgctl(id, IPC_RMID, NULL) == 0, "msgctl IPC_RMID");
    check(msgsnd(id, &m, 1, 0) < 0 && (errno == EINVAL || errno == EIDRM),
          "msgsnd on removed id");
}

static void test_msg_limits(void) {
    int id = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
    check(id >= 0, "msgget for limits");
    if (id < 0)
        return;
    struct msqid_ds st;
    check(msgctl(id, IPC_STAT, &st) == 0, "IPC_STAT for qbytes");
    st.msg_qbytes = 64;
    check(msgctl(id, IPC_SET, &st) == 0, "IPC_SET qbytes=64");

    struct msgbuf_ m = { .mtype = 1 };
    int sent = 0;
    for (int i = 0; i < 100; i++) {
        if (msgsnd(id, &m, 32, IPC_NOWAIT) != 0)
            break;
        sent++;
    }
    check(sent == 2 && errno == EAGAIN, "qbytes cap: 2x32 fits, 3rd EAGAIN");
    msgctl(id, IPC_RMID, NULL);
}

static void test_msg_blocking(void) {
    int id = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
    check(id >= 0, "msgget for blocking");
    if (id < 0)
        return;
    pid_t pid = fork();
    if (pid == 0) {
        msleep_(150);
        struct msgbuf_ m = { .mtype = 7 };
        strcpy(m.mtext, "wake");
        msgsnd(id, &m, 5, 0);
        _exit(0);
    }
    struct msgbuf_ r;
    memset(&r, 0, sizeof(r));
    int n = (int) msgrcv(id, &r, sizeof(r.mtext), 0, 0);
    check(n == 5 && r.mtype == 7 && strcmp(r.mtext, "wake") == 0,
          "blocked msgrcv woken by child msgsnd");
    waitpid(pid, NULL, 0);

    // Receiver blocked across IPC_RMID gets EIDRM.
    pid = fork();
    if (pid == 0) {
        msleep_(150);
        msgctl(id, IPC_RMID, NULL);
        _exit(0);
    }
    errno = 0;
    check(msgrcv(id, &r, sizeof(r.mtext), 0, 0) < 0 && errno == EIDRM,
          "blocked msgrcv sees EIDRM on RMID");
    waitpid(pid, NULL, 0);
}

static void test_sem_basic(void) {
    int id = semget(IPC_PRIVATE, 2, IPC_CREAT | 0600);
    check(id >= 0, "semget 2 sems");
    if (id < 0)
        return;

    union semun_ arg = { .val = 3 };
    check(semctl(id, 0, SETVAL, arg) == 0, "SETVAL sem0=3");
    check(semctl(id, 0, GETVAL) == 3, "GETVAL sem0==3");
    unsigned short vals[2] = { 5, 9 };
    arg.array = vals;
    check(semctl(id, 0, SETALL, arg) == 0, "SETALL {5,9}");
    unsigned short got[2] = { 0, 0 };
    arg.array = got;
    check(semctl(id, 0, GETALL, arg) == 0 && got[0] == 5 && got[1] == 9,
          "GETALL {5,9}");

    struct sembuf op = { .sem_num = 0, .sem_op = -2, .sem_flg = 0 };
    check(semop(id, &op, 1) == 0, "semop down 2");
    check(semctl(id, 0, GETVAL) == 3, "GETVAL after down==3");
    check(semctl(id, 0, GETPID) == getpid(), "GETPID is us");

    op.sem_op = -4;
    op.sem_flg = IPC_NOWAIT;
    check(semop(id, &op, 1) < 0 && errno == EAGAIN, "down 4 of 3 EAGAIN");

    check(semctl(id, 0, IPC_RMID) == 0, "sem IPC_RMID");
}

static void test_sem_blocking(void) {
    int id = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
    check(id >= 0, "semget for blocking");
    if (id < 0)
        return;
    union semun_ arg = { .val = 0 };
    check(semctl(id, 0, SETVAL, arg) == 0, "SETVAL 0");

    pid_t pid = fork();
    if (pid == 0) {
        msleep_(150);
        struct sembuf up = { .sem_num = 0, .sem_op = 1, .sem_flg = 0 };
        semop(id, &up, 1);
        _exit(0);
    }
    struct sembuf down = { .sem_num = 0, .sem_op = -1, .sem_flg = 0 };
    check(semop(id, &down, 1) == 0, "blocked down woken by child's up");
    waitpid(pid, NULL, 0);

    // Wait-for-zero: value is 0 now; push it up, child drops it to 0.
    struct sembuf up = { .sem_num = 0, .sem_op = 2, .sem_flg = 0 };
    check(semop(id, &up, 1) == 0, "up 2");
    pid = fork();
    if (pid == 0) {
        msleep_(150);
        struct sembuf d = { .sem_num = 0, .sem_op = -2, .sem_flg = 0 };
        semop(id, &d, 1);
        _exit(0);
    }
    struct sembuf zero = { .sem_num = 0, .sem_op = 0, .sem_flg = 0 };
    check(semop(id, &zero, 1) == 0, "wait-for-zero woken");
    waitpid(pid, NULL, 0);

    // SEM_UNDO: child takes the semaphore with SEM_UNDO and dies without
    // releasing; the kernel's exit undo must give it back.
    arg.val = 1;
    check(semctl(id, 0, SETVAL, arg) == 0, "SETVAL 1 for undo");
    pid = fork();
    if (pid == 0) {
        struct sembuf d = { .sem_num = 0, .sem_op = -1, .sem_flg = SEM_UNDO };
        if (semop(id, &d, 1) != 0)
            _exit(1);
        _exit(0); // dies "holding" it
    }
    int status;
    waitpid(pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "undo child took sem");
    struct sembuf down_nb = { .sem_num = 0, .sem_op = -1, .sem_flg = IPC_NOWAIT };
    check(semop(id, &down_nb, 1) == 0, "SEM_UNDO restored value at exit");

    // Blocked semop across RMID gets EIDRM.
    pid = fork();
    if (pid == 0) {
        msleep_(150);
        semctl(id, 0, IPC_RMID);
        _exit(0);
    }
    errno = 0;
    struct sembuf d = { .sem_num = 0, .sem_op = -1, .sem_flg = 0 };
    check(semop(id, &d, 1) < 0 && (errno == EIDRM || errno == EINVAL),
          "blocked semop sees EIDRM on RMID");
    waitpid(pid, NULL, 0);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(30));

    test_msg_basic();
    test_msg_limits();
    test_msg_blocking();
    test_sem_basic();
    test_sem_blocking();

    return finish_suite("sysv_ipc");
}
