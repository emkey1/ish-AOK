// Linux native AIO (the io_* family), enough of it that programs which assume
// it exists on Linux run. See docs/aio_plan.md for the design and, more
// usefully, for the two decisions that shaped it.
#ifndef KERNEL_AIO_H
#define KERNEL_AIO_H

#include "misc.h"

struct tgroup;

// The full-width forms, dispatched natively by the 64-bit ABIs. An
// aio_context_t is the address of the context's ring page, so on amd64 it is
// a real 64-bit address -- pushing these through the legacy 32-bit marshaller
// truncates the handle, and the marshaller rightly SIGSYS-kills the task
// rather than do it.
int_t sys_io_setup_guest(uint_t nr_events, guest_addr_t ctx_idp);
int_t sys_io_destroy_guest(guest_addr_t ctx_id);
int_t sys_io_submit_guest(guest_addr_t ctx_id, sqword_t nr, guest_addr_t iocbpp);
int_t sys_io_getevents_guest(guest_addr_t ctx_id, sqword_t min_nr, sqword_t nr,
                             guest_addr_t events_addr, guest_addr_t timeout_addr);
int_t sys_io_cancel_guest(guest_addr_t ctx_id, guest_addr_t iocb_addr,
                          guest_addr_t result_addr);

// ...and the dword forms for i386's table, where the whole address space fits
// in a dword and widening is lossless.
int_t sys_io_setup(uint_t nr_events, addr_t ctx_idp);
int_t sys_io_destroy(addr_t ctx_id);
int_t sys_io_submit(addr_t ctx_id, sdword_t nr, addr_t iocbpp);
int_t sys_io_getevents(addr_t ctx_id, sdword_t min_nr, sdword_t nr,
                       addr_t events_addr, addr_t timeout_addr);
int_t sys_io_cancel(addr_t ctx_id, addr_t iocb_addr, addr_t result_addr);

// Every context belonging to this thread group, released. Called from
// task_free_final when the group leader goes; a context outliving its address
// space would be holding a guest address that no longer means anything.
void aio_discard_tgroup(struct tgroup *group);

#endif
