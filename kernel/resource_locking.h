//#include "util/sync.h"
// Because sometimes we can't #include "kernel/task.h" -mke

extern unsigned task_reference_count(struct task*);
extern void task_ref_count(struct task*, int, char*, int);
extern unsigned locks_held_count(struct task*);
extern void modify_locks_held_count(struct task*, int);
extern bool current_is_valid(void);

