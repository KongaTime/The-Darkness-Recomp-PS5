/*
 * The Darkness on PS5 - the title's PIE link layout.
 *
 * Keeps code, immutable data, RELRO and writable data in separate, page-aligned
 * address ranges. The console's loader and the native tool that converts this
 * link into a module need that separation: the tool writes the PS5 process
 * parameters after the last read-only section, and only a page-aligned gap
 * between the read-only and writable ranges leaves it somewhere mapped.
 *
 * Derived from ps5-native-app-boilerplate's ps5-pie.ld and ps5-pie-unwind.ld
 * (PS5_Vulkan/tooling), Copyright (C) 2026 BlackBearReloaded,
 * SPDX-License-Identifier: GPL-3.0-or-later, with this title's own boundary
 * symbols: the C runtime and libunwind look for the bss and unwind-table ends
 * rather than walking the loaded modules, which a title cannot do.
 */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)

PHDRS {
    text PT_LOAD FLAGS (0x5);
    ro PT_LOAD FLAGS (0x4);
    relro PT_LOAD FLAGS (0x6);
    data PT_LOAD FLAGS (0x6);
    dynamic PT_DYNAMIC FLAGS (0x6);
}

SECTIONS {
    .text 0 : ALIGN(CONSTANT(MAXPAGESIZE)) {
        QUAD(0xcccccccccccccccc)
        QUAD(0xcccccccccccccccc)
        *(.text .text.*)
        *(.plt .plt.*)
    } : text

    .eh_frame_hdr : ALIGN(CONSTANT(MAXPAGESIZE)) {
        KEEP(*(.eh_frame_hdr))
    } : ro
    .eh_frame : { KEEP(*(.eh_frame)) } : ro
    .gcc_except_table : { *(.gcc_except_table .gcc_except_table.*) } : ro
    .rodata : { *(.rodata .rodata.*) } : ro

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .data.rel.ro : {
        *(.data.rel.ro .data.rel.ro.*)
    } : relro
    .got : { *(.got) } : relro
    .got.plt : { *(.got.plt) } : relro
    .preinit_array : {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN(__preinit_array_end = .);
    } : relro
    .init_array : {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.init_array .ctors))
        PROVIDE_HIDDEN(__init_array_end = .);
    } : relro
    .fini_array : {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
        KEEP(*(.fini_array .dtors))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } : relro

    /* Kept only long enough for the converter to read and rewrite them. */
    .dynsym : { *(.dynsym) } : relro
    .gnu.hash : { *(.gnu.hash) } : relro
    .dynstr : { *(.dynstr) } : relro
    .rela.dyn : { *(.rela.dyn) } : relro
    .rela.plt : { *(.rela.plt) } : relro

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .dynamic : {
        *(.dynamic)
    } : data : dynamic
    .data : { *(.data .data.*) } : data
    .tdata : { *(.tdata .tdata.*) } : data
    .tbss : { *(.tbss .tbss.*) } : data
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(8);
        PROVIDE(__bss_start = ADDR(.bss));
        PROVIDE(__bss_end = ADDR(.bss) + SIZEOF(.bss));
        PROVIDE(_end = ADDR(.bss) + SIZEOF(.bss));
    } : data
}

/* The C runtime clears bss through the __bss pair and bounds the image with the
 * __image pair, and libunwind finds the unwind tables through the *_start/*_end
 * pairs. crt1.o refers to all of them weakly, so without these the link leaves
 * them imported and finds no module that exports them. */
/* Anchored to a section, not to the constant 0: an absolute 0 does not move
 * with the image, and these bound the loaded module. A weak reference does not
 * make PROVIDE define a symbol, so these are plain assignments. */
__executable_start = ADDR(.text);
__image_start = ADDR(.text);
__image_end = ADDR(.bss) + SIZEOF(.bss);
PROVIDE(__eh_frame_start = ADDR(.eh_frame));
PROVIDE(__eh_frame_end = ADDR(.eh_frame) + SIZEOF(.eh_frame));
PROVIDE(__eh_frame_hdr_start = ADDR(.eh_frame_hdr));
PROVIDE(__eh_frame_hdr_end = ADDR(.eh_frame_hdr) + SIZEOF(.eh_frame_hdr));
