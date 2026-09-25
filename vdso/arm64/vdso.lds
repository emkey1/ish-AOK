/* The aarch64 vDSO's layout (vdso.S): one read+execute PT_LOAD from address
 * 0, headers included, the way Linux lays out its own (arch/arm64/kernel/
 * vdso/vdso.lds.S) -- the loader takes the base from where the page is
 * mapped, and nothing in it is written. */

OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)

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
    LINUX_2.6.39 {
    global:
        __kernel_rt_sigreturn;
        __kernel_gettimeofday;
        __kernel_clock_gettime;
        __kernel_clock_getres;
    local: *;
    };
}
