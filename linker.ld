OUTPUT_FORMAT(binary)

SECTIONS {
    . = 0x8000;

    .text : {
        *(.text.start)   /* entry.S must be first */
        *(.text .text.*) /* all other code */
    }

    .rodata : { *(.rodata .rodata.*) }

    .data : {
        *(.data .data.*)

        /*
         * Load canary: the last byte of the image the MBR fetches.
         *
         * kernel_main() verifies it and refuses to continue if the
         * bootloader came up short.  Without it a truncated load is
         * silent: the kernel runs, but .data reads back as zeros and
         * every statically initialised variable in the kernel is 0, so
         * features appear to work while doing nothing.
         *
         * Last is the point.  It is the first thing a too-short loader
         * fails to fetch, which is exactly the case worth catching.
         * tools/check_kernel_fit.py asserts that this word still falls
         * inside the window the MBR reads.
         *
         * It lives at the end of .data rather than in a section of its
         * own: .bss is NOBITS, and a new section after it would make ld
         * emit the whole megabyte of .bss padding into a binary-format
         * image.
         */
        . = ALIGN(4);
        kernel_load_canary = .;
        LONG(0x4C4F4144)   /* "LOAD" */
    }

    _bss_start = .;
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    }
    _bss_end = .;

    _end = .;
}
