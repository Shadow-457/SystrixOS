# ENGINE

A hobby x86-64 bare-metal OS written in C and x86-64 assembly. Boots on real hardware and QEMU. Everything — bootloader, kernel, GUI, libc, compiler — written from scratch.

---

## Features

**Kernel**
- Custom 512-byte bootloader — walks real mode → protected mode → long mode by hand, carves GDT/IDT manually
- Preemptive round-robin scheduler — no task can starve the CPU
- PMM — bitmap-based physical memory manager
- VMM — per-process virtual address spaces, no shared memory conflicts
- `malloc`/`free` — custom heap at `0x200000`

**Userland**
- ELF64 loader — loads and executes 64-bit ELF binaries
- Minimal `libc` + `crt0` — written from scratch
- `syscall` interface — `write`, `exit`, and more
- Shadow Compiler (`SHC`) — compiles `.shadow` scripts to native binaries *inside ENGINE itself*

**Drivers**
- PS/2 keyboard and mouse — both polling and interrupt-driven
- ATA disk driver — read/write to spinning disk
- VGA text mode — 80×25, 200-row scrollback via Shift+PgUp/PgDn
- Framebuffer — 1024×768 @ 32bpp VBE
- Intel e1000 NIC — basic, MAC hardcoded to `52:54:00:12:34:56`

**Storage**
- FAT32 at LBA 256 — read and write support

**GUI Desktop**
- Full desktop at 1024×768
- Draggable, resizable windows (8-direction resize), minimize/maximize/close
- App launcher via dock button
- Right-click context menu on desktop
- Dock with active-window indicators
- System Monitor — CPU, memory, disk usage
- Desktop icons — double-click to open

---

## Project Structure

```
ENGINE/
├── boot/
│   └── boot.S          # 16-bit bootloader (real → protected → long mode)
├── kernel/
│   ├── entry.S         # kernel entry point, stack setup
│   ├── isr.S           # interrupt handlers + syscall gate
│   ├── kernel.c        # kernel main (VGA, PS/2, ATA, FAT32, shell, GUI loop)
│   ├── heap.c          # malloc/free
│   ├── pmm.c           # physical memory manager
│   ├── vmm.c           # virtual memory manager + page tables
│   ├── tss.c           # task state segment
│   ├── process.c       # process lifecycle (create, copy, run)
│   ├── elf.c           # ELF64 loader
│   ├── scheduler.c     # round-robin scheduler
│   ├── syscall.c       # syscall handler
│   ├── input.c         # input event dispatcher
│   ├── net.c           # e1000 network driver
│   ├── fbdev.c         # framebuffer driver
│   └── gui.c           # GUI desktop environment
├── user/
│   ├── crt0.S          # process startup
│   ├── libc.c / libc.h # minimal C library
│   ├── hello.c         # hello world example
│   ├── myprogram.c     # example userland program
│   └── shc.c           # Shadow Compiler
├── include/
│   ├── kernel.h        # types and shared declarations
│   └── font8x8.h       # 8×8 bitmap font
├── linker.ld           # linker script
└── Makefile
```

---

## Building

**Dependencies:**
- `gcc` (x86-64)
- `binutils` (`as`, `ld`, `objcopy`)
- `mtools`
- `qemu-system-x86_64`
- `make`
- `python3`

```bash
make clean
make all
```

Produces `engine.img` — a bootable disk image.

---

## Running

```bash
make run
```

Boots ENGINE in QEMU. Inside the shell, run `gui` to start the desktop.

```bash
make run-quiet      # GTK display, less console noise
make run-sdl        # SDL display
make run-nographic  # serial output only, no display
```

---

## User Programs

Example programs live in `user/`. Build and embed into the disk image:

```bash
make hello        # build HELLO_C
make myprog       # build MYPROGRAM
make shc          # build Shadow Compiler
make programs     # embed HELLO_C + MYPROGRAM
make compiler     # embed SHC + .shadow scripts
```

Run inside ENGINE:
```
elf HELLO_C
elf MYPROGRAM
elf SHC
```

Add your own binary:
```bash
make addprog PROG=./MYPROG
```

---

## Memory Map

| Region              | Address                       |
|---------------------|-------------------------------|
| Bootloader          | `0x7C00`                      |
| Kernel load address | low physical memory           |
| Heap                | `0x200000` (2 MB, 2 MB size)  |
| Userland            | `0x400000`                    |
| Stack               | grows down from `0x700000`    |
| Framebuffer (mapped)| `0xA0000000`                  |

---

## Known Limitations

- Max 8 windows open simultaneously (`GUI_MAX_WINDOWS`)
- Max 128 widgets total (`GUI_MAX_WIDGETS`)
- No GPU acceleration — all rendering is software into the framebuffer
- Clock always shows `00:00:00` — RTC not wired up yet
- Network and audio status icons are decorative — no real backend yet
- FAT32 is functional but basic

---

## Implementation Notes

**`-fno-pic` is mandatory.** Without it, GCC emits GOT-relative calls. IDT gates are loaded as raw code addresses — not function pointers through the GOT. The result is a triple fault on the first timer interrupt. Took an entire week to track down.

**binutils injects `.note.gnu.property` silently.** This bloats the boot binary from 512 to 1064 bytes, causing the BIOS to triple-fault. Fix: link to a temp ELF first, then strip to a raw binary with `objcopy`.

---

## License

GPL-3.0 — see [LICENSE](LICENSE).
