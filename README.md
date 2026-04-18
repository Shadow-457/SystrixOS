# ENGINE OS

> UGH. ME MAKE BIG ROCK THINK. 64-BIT. ME WRITE IN C AND MAGIC SQUIGGLES (x86-64 ASSEMBLY).

ENGINE is cave-tribe hobby rock-brain. It wake up on real rock (and fake rock called QEMU). It have own wake-up magic, it juggle many tasks at once, it remember where things go, it find food in FAT32 cave, it load ELF creature, it poke PS/2 rock-stick, and it make pretty picture cave wall. ME WRITE ALL OF IT. NO STEAL FROM OTHER CAVE.

---

## WHAT ENGINE CAN DO

**Wake Up & Brain Rock**
- Me make tiny 512-rock wake-up spell — it drag brain from dumb-mode to smarter-mode to BIGGEST-mode by hand. Me carve GDT/IDT runes myself
- Brain run many tasks in circle. No task hog brain. Each get turn. Fair like sharing mammoth
- Rock-remember manager (PMM) — use picture-map to track which rocks free, which rocks taken
- Magic address manager (VMM) — each creature get own pretend-rocks so they no fight
- Me also make own `malloc`/`free` — it live at rock-address 0x200000

**Outside World (Userland)**
- ELF64 creature-loader — pick up 64-bit ELF animal and make it run
- Tiny `libc` + `crt0` — me write from scratch so creature have place to stand
- Talk-to-brain hole (`syscall`) — creature poke brain with `write`, `exit`, grunt-words
- Shadow Compiler (`SHC`) — can turn `.shadow` scrolls into running creatures INSIDE ENGINE ITSELF. OOGH.

**Rock-Pokers & Ear-Holes**
- PS/2 rock-poker (keyboard) and tail-pointer (mouse) — both poke and interrupt style
- ATA rock-scratching driver — read and write marks on spinning rock
- VGA word-cave — 80x25 letters, scroll back 200 rows with Shift+PgUp/PgDn
- Picture-wall driver — 1024x768 with 32 colors-per-dot
- Intel e1000 wire-talk driver (basic, ear-address stuck at `52:54:00:12:34:56`)

**Food Storage Cave**
- FAT32 cave at LBA 256 — can store and find food (files). Read AND write. UGH YES.

**Pretty Picture Cave Wall (GUI Desktop)**
- Full cave-drawing at 1024x768
- Move caves around — drag, stretch 8 ways, hide/grow/kill (like apple-tribe buttons)
- Push big button top-left to open more caves
- Top-right shows: sun-count (clock), loud-rock (volume), wire-talk (network)
- Cave-wall has clickable pictures — poke twice to open
- Poke right-hand finger on wall for secret menu
- Bottom has round rock-shelf with dots showing open caves
- Brain-watcher cave shows: how tired brain is, how full memory rock, how full disk rock
- Colors: dark water blue `#0a0e1a` with bright sky `#00e5ff` and magic purple `#7c3aed`

---

## CAVE MAP (Project Structure)

```
ENGINE/
├── boot/
│   └── boot.S          # 16-bit wake-up spell (dumb-mode → smarter-mode → biggest-mode)
├── kernel/
│   ├── entry.S         # brain wake-up spot, stack pile setup
│   ├── isr.S           # interrupt catchers + syscall hole
│   ├── kernel.c        # big brain heart (VGA, PS/2, ATA, FAT32, shell, cave-drawing loop)
│   ├── heap.c          # malloc/free (grab rock, give back rock)
│   ├── pmm.c           # physical rock manager
│   ├── vmm.c           # pretend-rock manager + address paintings
│   ├── tss.c           # task rock slab
│   ├── process.c       # creature management (birth, copy, run)
│   ├── elf.c           # ELF64 creature-loader
│   ├── scheduler.c     # round-circle fair-turn picker
│   ├── syscall.c       # grunt-hole handler
│   ├── input.c         # rock-poker listener
│   ├── net.c           # wire-talk driver
│   ├── fbdev.c         # picture-wall driver
│   └── gui.c           # full cave drawing environment
├── user/
│   ├── crt0.S          # creature birth setup
│   ├── libc.c / libc.h # tiny creature helper scrolls
│   ├── hello.c         # "UGGGH HELLO WORLD" example
│   ├── myprogram.c     # example creature
│   └── shc.c           # Shadow Compiler creature
├── include/
│   ├── kernel.h        # types and grunts
│   ├── font8x8.h       # 8x8 dot-letter pictures for cave wall
│   └── input.h         # rock-poker rune-codes
├── linker.ld           # creature-stitching scroll
└── Makefile
```

---

## HOW TO BUILD FIRE (Building)

**Need these rocks first:**
- `gcc` (64-bit rock-carver)
- `binutils` (`as`, `ld`, `objcopy` — rock tools)
- `mtools` (FAT32 cave digger)
- `qemu-system-x86_64` (fake rock machine)
- `make` (lazy-grunt runner)
- `python3` (boot rock size checker)

**MAKE FIRE:**
```bash
make clean
make all
```

This make `engine.img` — a rock that can wake up and think.

---

## HOW TO RUN (Running)

```bash
make run
```

Start fake rock (QEMU) with disk-rock. Inside ENGINE shell, grunt `gui` to start cave-drawing.

**Other run-grunts:**
```bash
make run-quiet      # GTK picture box, less noise
make run-sdl        # SDL picture box
make run-nographic  # only wire words, no picture cave
```

---

## OUTSIDE CREATURES (User Programs)

Example creatures live in `user/` cave. Build and add to rock-picture:

```bash
make hello        # carve HELLO_C creature
make myprog       # carve MYPROGRAM creature
make shc          # carve Shadow Compiler creature
make programs     # put HELLO_C + MYPROGRAM in rock
make compiler     # put SHC + shadow-scrolls in rock
```

Run creatures inside ENGINE shell:
```
elf HELLO_C
elf MYPROGRAM
elf SHC
```

Add own creature to rock:
```bash
make addprog PROG=./MYPROG
```

---

## WHERE THINGS LIVE IN MEMORY CAVE

| Cave Region         | Rock Address                  |
|---------------------|-------------------------------|
| Wake-up spell       | `0x7C00`                      |
| Brain load spot     | low physical rocks            |
| Heap (grab-rocks)   | `0x200000` (2 MB, 2 MB big)   |
| Outside creatures   | `0x400000`                    |
| Stack pile          | grow down from `0x700000`     |
| Picture-wall        | `0xA0000000` (brain pretend)  |

---

## THINGS ENGINE NOT DO GOOD YET (Known Limitations)

- Only 8 caves open same time (`GUI_MAX_WINDOWS`)
- Only 128 cave-things total (`GUI_MAX_WIDGETS`)
- No magic rock GPU — ALL drawing done by hand into picture-wall. SLOW BUT HONEST
- Sun-count always show `00:00:00` — RTC rock not hooked up yet. TIME IS MYSTERY
- Wire-talk and loud-rock pictures just look pretty, no real work yet
- FAT32 cave work but basic. No fancy tricks

---

## INTERESTING BATTLE SCARS (Notable Implementation Notes)

**The `-fno-pic` flag VERY IMPORTANT. ME LEARN HARD WAY.** Without it, GCC make GOT-hole magic. Calling creatures through pointer go through GOT that not exist. IDT gates load raw code-rocks as addresses instead — brain explode on first timer-poke. Took one whole week of sad grunting to find. UGH.

**binutils sneaks in `.note.gnu.property` rock.** By default, binutils stuff extra section into boot binary, make it grow from 512 to 1064 rocks — BIOS brain triple-fall-down. Fix: link to temp ELF first, then strip to raw rock with `objcopy`. SNEAKY BINUTILS. ME ANGRY.

---

## WHY ME DO THIS?

Me want understand how brain-rocks REALLY work at bottom of everything. Turns out: LOTS of edge cases, magic numbers, and very late nights staring at fire. But when it all click...

UGH. FEEL GOOD LIKE FIRST TIME MAKE FIRE.

---

## SHARING RULES (License)

GPL-3.0 — see [LICENSE](LICENSE). Share with tribe. Give credit. No steal and call own.
