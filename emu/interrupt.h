// Intel standard interrupts
// Any interrupt not handled specially becomes a SIGSEGV
#define INT_NONE -1
#define INT_DIV 0
#define INT_DEBUG 1
#define INT_NMI 2
#define INT_BREAKPOINT 3
#define INT_OVERFLOW 4
#define INT_BOUND 5
#define INT_UNDEFINED 6
#define INT_FPU 7 // do not try to use the fpu. instead, try to realize the truth: there is no fpu.
#define INT_DOUBLE 8 // interrupt during interrupt, i.e. interruptception
#define INT_GPF 13
#define INT_TIMER 32
#define INT_SYSCALL 0x80
// Synthetic interrupt used for privileged instructions that should fault in
// userspace as #GP but are not memory faults.
#define INT_PRIV 0x100
