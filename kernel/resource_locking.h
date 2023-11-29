//#include "util/sync.h"
// Because sometimes we can't #include "kernel/task.h" -mke

extern void task_ref_cnt_mod(struct task *task, int value);
extern void task_ref_cnt_mod_wrapper(int, const char*, int);

extern int task_ref_cnt_val(struct task *task);
extern void mem_ref_cnt_mod(struct mem*, int, char*, int);
extern int mem_ref_cnt_val(struct mem *mem);
extern unsigned locks_held_count(struct task*);
extern void modify_locks_held_count(struct task*, int);
extern bool current_is_valid(void);

