// epoll_event guest-ABI layout: epoll_data must round-trip epoll_ctl ->
// epoll_wait bit-exact (kernel/epoll.c). Linux packs struct epoll_event
// (12 bytes, data at offset 4) ONLY on x86; every other arch uses the
// naturally-aligned 16-byte layout (data at offset 8). AOK marshals the
// two layouts by guest ABI (epoll_event_aligned): arm64 was added for the
// Go netpoll SIGSEGV, but riscv64 was missed, so riscv64 guests read/wrote
// epoll_event with the packed x86 layout -- both the registered data and
// every delivered event's data were garbage. ivykis (syslog-ng) dispatches
// events by that data pointer: it got events it couldn't attribute, never
// serviced the fd, and spun on the level-triggered readiness forever
// (~50-65% CPU, no log throughput, periodic SIGSEGV on the mangled
// pointer). This locks the round-trip -- and the 2-event array stride --
// for whatever ABI it's compiled as.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "test_common.h"

static void check_u64(const char *label, uint64_t got, uint64_t exp) {
    test_log_if(got != exp, "%s: got=%#llx exp=%#llx\n", label,
                (unsigned long long) got, (unsigned long long) exp);
    if (got != exp)
        failf(label, got, 0, 0, exp, 0, 0);
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    int ep = epoll_create1(0);
    int ev1 = eventfd(0, 0);
    int ev2 = eventfd(0, 0);
    if (ep < 0 || ev1 < 0 || ev2 < 0) {
        perror("setup");
        return 1;
    }

    // Distinctive bit patterns: every byte differs, high and low words both
    // nonzero, so any offset/stride mixup shows up as a mismatch.
    const uint64_t data1 = 0xdeadbeefcafef00dULL;
    const uint64_t data2 = 0x0123456789abcdefULL;

    struct epoll_event e = { .events = EPOLLIN, .data.u64 = data1 };
    if (epoll_ctl(ep, EPOLL_CTL_ADD, ev1, &e) != 0) {
        perror("epoll_ctl ADD ev1");
        return 1;
    }
    e.data.u64 = data2;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, ev2, &e) != 0) {
        perror("epoll_ctl ADD ev2");
        return 1;
    }

    // Single ready fd: data must round-trip exactly.
    uint64_t one = 1;
    if (write(ev1, &one, 8) != 8) {
        perror("write ev1");
        return 1;
    }
    struct epoll_event out[4];
    memset(out, 0xaa, sizeof(out));
    int n = epoll_wait(ep, out, 4, 1000);
    check_u64("single.count", (uint64_t) n, 1);
    if (n == 1) {
        check_u64("single.events", out[0].events, EPOLLIN);
        check_u64("single.data", out[0].data.u64, data1);
    }

    // Two ready fds: the second entry exercises the array stride (a packed
    // 12-byte stride misplaces every entry after the first).
    if (write(ev2, &one, 8) != 8) {
        perror("write ev2");
        return 1;
    }
    memset(out, 0xaa, sizeof(out));
    n = epoll_wait(ep, out, 4, 1000);
    check_u64("pair.count", (uint64_t) n, 2);
    if (n == 2) {
        uint64_t got1 = out[0].data.u64, got2 = out[1].data.u64;
        if (got1 > got2) { // order is not guaranteed; normalize
            uint64_t tmp = got1; got1 = got2; got2 = tmp;
        }
        check_u64("pair.data.lo", got1, data2 < data1 ? data2 : data1);
        check_u64("pair.data.hi", got2, data2 < data1 ? data1 : data2);
        check_u64("pair.events0", out[0].events, EPOLLIN);
        check_u64("pair.events1", out[1].events, EPOLLIN);
    }

    // MOD must honor the layout on the read side too. Drain ev2 first so
    // only ev1 (the MOD target) is still level-ready.
    uint64_t drain;
    if (read(ev2, &drain, 8) != 8) {
        perror("drain ev2");
        return 1;
    }
    const uint64_t data3 = 0xfeedfacedeadd00dULL;
    e.events = EPOLLIN;
    e.data.u64 = data3;
    if (epoll_ctl(ep, EPOLL_CTL_MOD, ev1, &e) != 0) {
        perror("epoll_ctl MOD ev1");
        return 1;
    }
    memset(out, 0xaa, sizeof(out));
    n = epoll_wait(ep, out, 1, 1000);
    check_u64("mod.count", (uint64_t) n, 1);
    if (n == 1)
        check_u64("mod.data", out[0].data.u64, data3);

    return finish_suite("epoll_data_layout");
}
