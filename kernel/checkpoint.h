// checkpoint.h -- save a running guest to a file and bring it back.
//
// The design, the scope and the honest limits are at the top of checkpoint.c.
// This is only the seam the rest of the kernel calls through.

#ifndef KERNEL_CHECKPOINT_H
#define KERNEL_CHECKPOINT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Written from the guest's own thread, at the top of task_run_current's loop,
// where the task is at a clean boundary: the syscall that asked for the
// checkpoint has already stored its return value and the program counter names
// the instruction AFTER it. That is what makes a restore continue rather than
// re-run.
//
// Returns 0, or a guest _E* code. The guest keeps running either way -- a
// checkpoint is a copy, not a departure.
int checkpoint_save(const char *host_path);

// Rebuild the guest described by the file onto `current`, which must be a
// freshly constructed init (kernel/init.c's become_first_process). The caller
// then lets it run, exactly as it would after do_execve.
int checkpoint_restore(const char *host_path);

// Requested by a write to /proc/ish/checkpoint; performed at the next loop top.
// Consumed by checkpoint_run_pending, which is the only caller of
// checkpoint_save.
//
// Returns 0 if the request was taken, or a guest _E* code if this guest cannot
// be checkpointed at all. The check is here as WELL as in checkpoint_save
// because some refusals mean the deferred save would never run: a native
// program is dispatched by native_exec_run_pending and never comes back to
// task_run_current's loop, so a request made from one would simply sit there
// -- an image that never appears and no error anywhere, which is worse than
// either outcome.
// `and_halt` makes it a SUSPEND rather than a checkpoint: the image is
// written and the guest then stops, so the next launch resumes it instead of
// booting. A checkpoint is a copy and the guest carries on; a suspend is a
// departure.
int checkpoint_request(const char *host_path, bool and_halt);

// Take a checkpoint from a thread that is NOT a guest task -- the app's
// backgrounding path, which runs on the UI thread and has to know the image is
// on disk before iOS freezes it.
//
// Synchronous, unlike checkpoint_request: there is nothing to defer to,
// because the caller is not a task that will come back round a loop, and
// nothing useful to return to if the save has not happened. Every guest task
// is frozen, including the ones the deferred path would have left running.
//
// Returns 0, or a guest _E* code with /proc/ish/checkpoint's last_refusal
// naming the cause.
int checkpoint_save_external(const char *host_path);
void checkpoint_run_pending(void);

// The session file the entry point was given, if any: restored at startup and
// written at suspend. Empty means neither. kernel/checkpoint.c owns the
// string so both halves name the same file.
// ---- the freezer ---------------------------------------------------------
//
// Stopping the machine, which AOK can do because it owns the scheduler.
//
// A task running guest code parks at the top of task_run_current's loop. A
// task blocked INSIDE a syscall is woken the way a signal wakes it, its wait
// returns EINTR, and the dispatcher turns that into a RESTART -- the program
// counter is rewound over the syscall instruction, so the task arrives at the
// loop top about to re-execute the call it was in. That is what makes a
// blocked read() checkpointable: the image says "about to call read", and the
// restored guest calls it.
//
// checkpoint_freeze_pending is read by kernel/calls.c on every syscall return,
// so it is deliberately one relaxed load of one global in the common case.
bool checkpoint_freeze_pending(void);
// Called at the top of task_run_current's loop, with no lock held.
void checkpoint_park_if_frozen(void);
// The same, for a NATIVE program, from its parking place in
// native_checkpoint(). It never reaches task_run_current's loop -- it is a C
// function on a host thread -- so this is where it stops, and where it is
// asked to describe itself, because its state exists on this thread and
// nowhere else.
void checkpoint_native_park(void);

void checkpoint_set_session(const char *host_path);

// Whether the guest may drive this itself through /proc/ish/checkpoint.
//
// The app publishes its Settings switch here; the CLI has ISH_GUEST_CHECKPOINT
// as well. Writing to a /proc file is how a guest reaches every other AOK
// control (swap_evict, snapshot, the JIT knobs), and there is no reason for
// this one to be the exception -- but it hands a guest process a HOST path to
// write, so it is gated rather than open.
void checkpoint_set_guest_control(bool allowed);
bool checkpoint_guest_control(void);
const char *checkpoint_session(void);

// ---- a session that came back ------------------------------------------
//
// In the app, the terminal a person is looking at is a PSEUDO-terminal whose
// master side is a UI object -- app/Terminal.m's pty_open_fake -- not a guest
// process. It is not re-openable the way /dev/console is: it went with the app
// that was killed, and /dev/pts/1 on the next launch is a different terminal
// belonging to a different window.
//
// So a session on a pty comes back onto a NEW one, and the UI adopts that
// instead of starting a shell of its own. The app installs the factory below
// before restoring; the CLI leaves it NULL and a restored pty session falls
// back to the console, which is the right answer there because the CLI's
// terminal IS the console.
struct tty;
extern struct tty *(*checkpoint_open_session_tty)(void);

struct checkpoint_restored_session {
    int leader_pid;   // the session leader, so the UI knows whose exit ends it
    int tty_num;      // the pts number it came back on
    void *terminal;   // the tty's driver data: the app's Terminal object
};

// Take the next restored session nothing is showing yet, in the order the
// image had them. Returns 1 and fills `out`, or 0 when there are none left;
// each is handed out exactly once.
int checkpoint_take_restored_session(struct checkpoint_restored_session *out);
// Take the restored session whose session leader is `leader_pid`, falling back
// to the next one in the queue when there is no such session. A window that
// was showing a particular shell before the suspend asks for that shell back:
// the pts NUMBER is not stable across a restore (the restore makes fresh
// ptys), but the leader pid is, because the checkpoint restores pids.
int checkpoint_take_restored_session_for_pid(int leader_pid,
                                            struct checkpoint_restored_session *out);
// Whether a restored session led by `leader_pid` is still waiting to be shown.
// A peek, not a take: the workspace uses it to let every window that can name
// its own shell claim it BEFORE any window falls back to queue order, so a
// window with a stale pid cannot take a shell that another window is about to
// ask for by name.
int checkpoint_restored_session_pending(int leader_pid);

// Traces a restored task's first few syscalls when ISH_CHECKPOINT_DEBUG is on.
void checkpoint_trace_syscall(unsigned long nr);

// Traces one process exit when ISH_CHECKPOINT_DEBUG is on; nothing otherwise.
void checkpoint_trace_exit(int pid, const char *comm, int status);

// What /proc/ish/checkpoint reports. `restored` is how a guest program tells
// the two sides of a checkpoint apart: the write that took it returns
// normally, and so does the same write in the restored guest, because it is
// the SAME instruction stream continuing.
struct checkpoint_status {
    bool restored;            // this guest came back from a file
    unsigned long generation; // how many restores this image has been through
    unsigned long saves;      // checkpoints taken since boot
    int last_err;             // guest _E* code of the last save, 0 if fine
    char last_path[256];
    char last_refusal[256];   // why the last save refused, if it did
    unsigned long long bytes; // guest memory in the last image, in bytes
    unsigned long pages;      // guest pages in it
    unsigned long fds;        // descriptors in it
    unsigned long tasks;      // processes in it
    // Native programs in the last image that could not describe themselves.
    // They are SAVED and re-launched, not refused -- see ckpt_check_scope --
    // so the session comes back with these programs started again from their
    // command line rather than from where they were. Reported because a
    // restart is a real difference the person should hear about, not because
    // it is an error.
    unsigned long natives_restarted;
    char natives_note[192];   // their names, comma separated
};
void checkpoint_get_status(struct checkpoint_status *out);

// What the last successful restore could not put back as it was -- a socket
// whose rebuild failed and came back hung up -- as "pid P fd N: why", joined by
// "; ". Empty when everything came back, or when nothing was restored.
//
// A separate call rather than a field of struct checkpoint_status, and that is
// deliberate: callers keep that struct on their stack, and the Xcode build does
// not reliably recompile every one of them when this header changes. Growing
// the struct overran the stack of a stale caller and aborted the app at launch.
void checkpoint_get_restore_note(char *out, size_t size);

// ---- reading an image WITHOUT loading it --------------------------------
//
// What a session picker has to show: which machine, how big, and whether this
// build can still load it. Answered from the header alone -- the first ~200
// bytes -- so listing ten slots costs ten small reads rather than ten restores.
//
// Deliberately not a sidecar file written alongside the image: an image can be
// written by the app OR by the guest itself (/AOK/tools/suspend.sh), and a
// description that only one of those two updates is a description that lies.
// The image is the only thing that always knows.
#define CKPT_PEEK_HOSTNAME 65
#define CKPT_PEEK_ROOT_NAME 64
struct checkpoint_image_info {
    bool loadable;          // magic, version and page size match THIS build
    uint32_t version;       // what it actually is, for "saved by an older build"
    uint32_t abi;
    uint32_t tasks;
    uint64_t pages;
    char hostname[CKPT_PEEK_HOSTNAME];
    // The root it was saved on, as checkpoint_root_identity names it, and that
    // root's name at the time. 0 when the image does not say.
    uint64_t root;
    char root_name[CKPT_PEEK_ROOT_NAME];
};

// 0 and fills `out`, or a guest _E* code if the file cannot be read at all.
// A file that is readable but not loadable is NOT an error: out->loadable is
// false and the rest is filled in as far as it could be, so the picker can say
// "saved by a different build" rather than showing nothing.
int checkpoint_peek(const char *host_path, struct checkpoint_image_info *out);

// Which root a session belongs to: the identity of the root whose data
// directory (roots/<name>/data, or a CLI root's <dir>/data) is at this path,
// the same value a save records. It survives a rename and differs for any copy
// of the directory. 0 if the directory cannot be read. A restore on a root
// other than the image's is refused with _EXDEV before anything is changed,
// so the image is still good for its own root.
uint64_t checkpoint_root_identity(const char *root_data_dir);

#endif
