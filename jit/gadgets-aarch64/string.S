#include "gadgets.h"
#include "math.h"

#define REP_LIST once,rep,repnz

.gadget cmc
    ldrb w8, [_cpu, CPU_cf]
    eor w8, w8, 1
    strb w8, [_cpu, CPU_cf]
    ldr w9, [_cpu, CPU_eflags]
    bic w9, w9, 1
    orr w9, w9, w8
    str w9, [_cpu, CPU_eflags]
    gret

.gadget clc
    strb wzr, [_cpu, CPU_cf]
    ldr w8, [_cpu, CPU_eflags]
    bic w8, w8, 1
    str w8, [_cpu, CPU_eflags]
    gret

.gadget stc
    mov w8, 1
    strb w8, [_cpu, CPU_cf]
    ldr w9, [_cpu, CPU_eflags]
    orr w9, w9, 1
    str w9, [_cpu, CPU_eflags]
    gret

.gadget cld
    ldr w8, [_cpu, CPU_eflags]
    bic w8, w8, DF_FLAG
    str w8, [_cpu, CPU_eflags]
    mov w8, 1
    str w8, [_cpu, CPU_df_offset]
    gret

.gadget std
    ldr w8, [_cpu, CPU_eflags]
    orr w8, w8, DF_FLAG
    str w8, [_cpu, CPU_eflags]
    mov w8, -1
    str w8, [_cpu, CPU_df_offset]
    gret

# FIXME non 32 bit
.macro do_strop op, size, rep, s=
    # repnz is only a thing for cmps and scas
    .ifc \rep,repnz
        .ifnc \op,cmps N .ifnc \op,scas
            .exitm
        .endif N .endif
    .endif

    .gadget \op\size\()_\rep
        .ifnc \rep,once
            cbz ecx, 2f
        .endif
        # Forward REP MOVS/STOS a page span at a time: one TLB lookup per
        # span, then math.S's x86_str_copy / x86_str_fill. ECX, ESI and EDI
        # are updated after each span, so a fault or a poke leaves them as
        # x86's restart state. Backward (DF=1) and short counts take the
        # per-element loop below, and so does a MOVS whose destination is
        # ahead of its source inside the span (the byte-smearing overlap); an
        # element straddling a page goes through the per-element access.
        .ifin(\op, movs, stos)
        .ifc \rep,rep
            ldr w8, [_cpu, CPU_eflags]
            tst w8, DF_FLAG
            b.ne 8f                     /* DF=1: backward, per element */
            cmp ecx, 8
            b.lo 8f                     /* a short rep: per element */
          .ifc \op,stos
            .if \size == 8
            and x13, xax, 0xff
            mov x9, 0x0101010101010101
            mul x13, x13, x9
            .elseif \size == 16
            and x13, xax, 0xffff
            mov x9, 0x0001000100010001
            mul x13, x13, x9
            .else
            mov w13, eax
            orr x13, x13, x13, lsl 32
            .endif
          .endif
        .Lspan\@:
            and w8, edi, 0xfff
            mov w9, 0x1000
            sub w9, w9, w8              /* bytes to the destination's page end */
          .ifc \op,movs
            and w8, esi, 0xfff
            mov w10, 0x1000
            sub w10, w10, w8
            cmp w9, w10
            csel w9, w9, w10, lo        /* the nearer page end */
          .endif
            lsr w9, w9, (\size / 16)    /* whole elements (8 -> 0, 16 -> 1, 32 -> 2) */
            cmp w9, ecx
            csel w9, w9, ecx, lo
            cbz w9, .Lone\@             /* an element straddles a page */
            lsl w12, w9, (\size / 16)   /* bytes (w12, x13: kept across a miss's C call) */
          .ifc \op,movs
            cmp edi, esi                /* destination ahead of the source */
            b.ls .Lcopy\@               /* and inside the span: per element */
            sub w8, edi, esi
            cmp w8, w12
            b.lo 8f
        .Lcopy\@:
            mov _addr, esi
            read_prep 8, \op\size\()_rep_bs
            mov x13, _xaddr
            mov _addr, edi
            write_prep 8, \op\size\()_rep_bd
            mov x9, _xaddr
            mov x10, x13
            mov w11, w12
            mov w14, w12                /* the routines leave x14 alone */
            bl NAME(x86_str_copy)
            write_done 8, \op\size\()_rep_bd
            add esi, esi, w14
          .else
            mov _addr, edi
            write_prep 8, \op\size\()_rep_bd
            mov x15, _xaddr
            mov x9, _xaddr
            mov w11, w12
            mov w14, w12
            mov x3, x13
            bl NAME(x86_str_fill)
            mov _xaddr, x15
            write_done 8, \op\size\()_rep_bd
          .endif
            add edi, edi, w14
            sub ecx, ecx, w14, lsr #(\size / 16)
        .Lpoke\@:
            cbz ecx, .Ldone\@
            ldr x8, [_cpu, CPU_poked_ptr]
            ldrb w8, [x8]
            cbz w8, .Lspan\@
            # Poked mid-rep: back to this instruction, ECX/ESI/EDI as they are
            # (jit_exit's save_regs spills them), and out, as the per-element
            # loop does.
            ldr x0, [_ip]
            mov eip, w0
            b jit_ret
        .Lone\@:
          .ifc \op,movs
            mov _addr, esi
            read_prep \size, \op\size\()_rep_os
            ldr\s _tmp, [_xaddr]
            mov _addr, edi
            write_prep \size, \op\size\()_rep_od
            str\s _tmp, [_xaddr]
            write_done \size, \op\size\()_rep_od
            add esi, esi, (\size / 8)
          .else
            mov _addr, edi
            write_prep \size, \op\size\()_rep_od
            str\s eax, [_xaddr]
            write_done \size, \op\size\()_rep_od
          .endif
            add edi, edi, (\size / 8)
            sub ecx, ecx, 1
            b .Lpoke\@
        .Ldone\@:
            gret 1
        8:
        .endif
        .endifin
        # df_offset = w12
        mov w12, (\size/8)
        ldr w8, [_cpu, CPU_eflags]
        tst w8, DF_FLAG
        cneg w12, w12, ne
    1:

        .ifc \op,lods
            mov _addr, esi
            read_prep \size, \op\size\()_\rep
            ldrs eax, [_xaddr], \s

        .else N .ifc \op,stos
            mov _addr, edi
            write_prep \size, \op\size\()_\rep
            str\s eax, [_xaddr]
            write_done \size, \op\size\()_\rep

        .else N .ifc \op,movs
            mov _addr, esi
            read_prep \size, \op\size\()_\rep
            ldr\s _tmp, [_xaddr]
            mov _addr, edi
            write_prep \size, \op\size\()_\rep\()2
            str\s _tmp, [_xaddr]
            write_done \size, \op\size\()_\rep\()2

        .else N .ifc \op, scas
            mov _addr, edi
            read_prep \size, \op\size\()_\rep
            ldr\s w8, [_xaddr]
            mov _tmp, eax
            setf_a src=w8, dst=_tmp, s=\s
            do_add sub, _tmp, w8, \s
            setf_zsp \s

        .else N .ifc \op, cmps
            mov _addr, esi
            read_prep \size, \op\size\()_\rep
            ldr\s _tmp, [_xaddr]
            mov _addr, edi
            read_prep \size, \op\size\()_\rep\()2
            ldr\s w8, [_xaddr]
            setf_a src=w8, dst=_tmp, s=\s
            do_add sub, _tmp, w8, \s
            setf_zsp \s
        .endif N .endif N .endif N .endif N .endif

        .ifin(\op, lods,movs,cmps)
            add esi, esi, w12
        .endifin
        .ifin(\op, movs,stos,cmps,scas)
            add edi, edi, w12
        .endifin

        .ifnc \rep,once
            subs ecx, ecx, 1
            .ifin(\op, scas,cmps)
                .ifc \rep,rep
                    cbnz _tmp, 2f
                .else N .ifc \rep,repnz
                    cbz _tmp, 2f
                .endif N .endif
            .endifin
            cbz ecx, 2f
            # A rep that does NOT take the bulk fast path above -- backward
            # (DF=1), or any scas/cmps/lods -- runs its whole count right here,
            # and this loop used to have no way out. Measured on a 96 MB buffer
            # with a 1ms repeating timer: a backward `rep movsb` took 118ms and
            # delivered ONE signal, `repne scasb` 111ms and one. Those are
            # longer stalls than the forward case, because this path pays a TLB
            # lookup per element instead of a memmove per page-run.
            #
            # Nothing can reach the thread for that whole window: not a signal
            # (no SIGKILL, no SIGSTOP), and not the swap pager's throttle poke
            # -- which this loop is precisely the thing that provokes, since a
            # backward fill takes a write TLB miss every 4 KiB and that is the
            # sensor that calls cpu_poke (emu/tlb.c).
            #
            # ecx doubles as the counter: it decrements every iteration, so
            # testing its low bits costs no register and checks once per 1024
            # elements. Two instructions on the back edge buys a bound on how
            # long this loop can ignore the rest of the system.
            tst ecx, 1023
            b.ne 1b
            ldr x8, [_cpu, CPU_poked_ptr]
            ldrb w8, [x8]
            cbz w8, 1b
            # Poked. eip goes back to this instruction and we leave; jit_exit's
            # save_regs spills ecx/esi/edi, which ARE the x86 restart state, so
            # re-entry resumes the rep exactly where it stopped. _tmp is x0 and
            # is dead here (the scas/cmps compare above has been consumed, and a
            # movs value has already been stored).
            ldr x0, [_ip]
            mov eip, w0
            b jit_ret
    2:
        .endif
        gret 1
        .ifin(\op, lods,movs,scas,cmps)
            read_bullshit \size, \op\size\()_\rep
        .endifin
        .ifc \op,stos
            write_bullshit \size, \op\size\()_\rep
        .endif
        .ifc \op,movs
            write_bullshit \size, \op\size\()_\rep\()2
        .endif
        .ifc \op,cmps
            read_bullshit \size, \op\size\()_\rep\()2
        .endif
        .ifin(\op, movs, stos)
        .ifc \rep,rep
            write_bullshit 8, \op\size\()_rep_bd
            write_bullshit \size, \op\size\()_rep_od
          .ifc \op,movs
            read_bullshit 8, \op\size\()_rep_bs
            read_bullshit \size, \op\size\()_rep_os
          .endif
        .endif
        .endifin
.endm

.irp op, lods,stos,movs,scas,cmps
    .irp size, 8,16,32
        .irp rep, REP_LIST
            .if \size == 8
                do_strop \op, \size, \rep, b
            .elseif \size == 16
                do_strop \op, \size, \rep, h
            .elseif \size == 32
                do_strop \op, \size, \rep
            .endif
        .endr
    .endr
    .gadget_list_size \op, REP_LIST
.endr
# temporary
