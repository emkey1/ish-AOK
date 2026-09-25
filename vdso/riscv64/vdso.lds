/* The riscv64 vDSO's layout (vdso.S), the same as the arm64 one's
 * (../arm64/vdso.lds) under Linux's riscv64 version name. */

OUTPUT_FORMAT("elf64-littleriscv")
OUTPUT_ARCH(riscv)

SECTIONS {
    . = SIZEOF_HEADERS;

    .hash          : { *(.hash) }                  :text
    .gnu.hash      : { *(.gnu.hash) }
    .dynsym        : { *(.dynsym) }
    .dynstr        : { *(.dynstr) }
    .gnu.version   : { *(.gnu.version) }
    .gnu.version_d : { *(.gnu.version_d) }
    .gnu.version_r : { *(.gnu.version_r) }

    .dynamic       : { *(.dynamic) }               :text :dynamic

    .rodata        : {
        *(.rodata*)
        *(.got) *(.got.plt)
    }                                              :text

    .note          : { *(.note.*) }                :text :note

    .eh_frame_hdr  : { *(.eh_frame_hdr) }          :text :eh_frame_hdr
    .eh_frame      : { KEEP (*(.eh_frame)) }       :text

    . = ALIGN(16);
    .text          : { *(.text*) }                 :text

    /DISCARD/ : {
        *(.data .data.* .sdata*)
        *(.bss .bss.* .sbss .dynbss)
        *(.debug*)
        *(.comment)
    }
}

PHDRS {
    text         PT_LOAD         FLAGS(5) FILEHDR PHDRS; /* PF_R|PF_X */
    dynamic      PT_DYNAMIC      FLAGS(4);               /* PF_R */
    note         PT_NOTE         FLAGS(4);               /* PF_R */
    eh_frame_hdr PT_GNU_EH_FRAME FLAGS(4);               /* PF_R */
}

VERSION {
    LINUX_4.15 {
    global:
        __vdso_rt_sigreturn;
        __vdso_gettimeofday;
        __vdso_clock_gettime;
        __vdso_clock_getres;
    local: *;
    };
}
