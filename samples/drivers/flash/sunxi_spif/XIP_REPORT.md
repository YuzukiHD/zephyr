# SPIF XIP test report

Board: yuzukineko, F101 (C907, 600 MHz), SPI NOR JEDEC `852018` (16 MiB),
controller version 0x00010003, run from RAM with xfel, console COM4.
Sample: `samples/drivers/flash/sunxi_spif` (last line `PASS (0 failures)`).

## Configuration

| | |
|---|---|
| SCK | 100 MHz, 1-4-4 DTR read (`0xed`, 7 dummy cycles, mode byte), quad program |
| Sample point | mode 0, delay 31, window 31 points (SDR had mode 1, delay 24, window 49) |
| XIP | prefetch mapping at 0x0e000000, one command at a time, interrupts masked while a command runs |

The DTR read was off before: the default `dtr-read-opcode` was `0xee`
(4 byte address form) while this flash uses 3 byte addresses (`0xed`). The
probe now picks the opcode from the address width.

## Correctness (all passed)

- XIP code and data in the window run and read back right; a sector rewritten
  while mapped shows up in the window at once (code returns 26, then 40).
- Concurrent: a 1 ms timer interrupt and a low priority thread execute XIP
  code while the main thread erases, programs and reads other sectors:
  1266 timer runs, 4130 thread runs, 0 wrong results.
- Edges: first and last sector and byte of the window, 15 read lengths from
  1 to 4097 bytes at odd offsets, programs crossing a 256 byte page, erase
  visible in the window, 300 command/window alternations, interrupt reading
  the window and another thread reading the partition during 40 erase and
  program rounds: 2053 + 421 reads, 0 wrong.

## Read

| size | unmapped | mapped |
|---|---|---|
| 4 / 16 / 64 B | 40.7 / 41.1 / 40.6 us per call | 50.9 / 51.1 / 50.7 us |
| 256 B | 42.4 us (5.9 MB/s) | 52.7 us (4.7 MB/s) |
| 4 KiB | 82.4 us (48.5 MB/s) | 92.8 us (43.1 MB/s) |
| 64 KiB | 714 us (89.6 MB/s) | 726 us (88.2 MB/s) |
| 1 MiB in 512 KiB pieces | 94.2 MB/s | |
| same in SDR | 47 MB/s | |

- Large reads are at the DTR limit (about 100 MB/s at 100 MHz); mapping costs
  1.6 %.
- A call has a fixed cost of about 40 us (command, DMA descriptor, polling,
  bounce copy); mapping adds about 10 us (window down and up, cache
  invalidate). Small reads are bound by that, not by the bus.

## Write

| operation | unmapped | mapped |
|---|---|---|
| program 1 / 16 B | 103 / 104 us | 201 / 202 us |
| program 256 B | 298 us (838 KB/s) | 333 us (750 KB/s) |
| program 4 KiB | 4.84 ms (826 KB/s) | 5.40 ms (740 KB/s) |
| program 64 KiB | 77.2 ms (829 KB/s) | 84.9 ms (754 KB/s) |
| erase 4 KiB | 47.8 ms | 47.8 ms |
| erase 64 KiB | 131.6 ms | 131.7 ms |

- Program speed is the flash page program time; mapping costs about 9 %
  (one window down/up per page, about 100 us each for the small ones).
- Erase time is the flash's; mapping changes nothing in the duration.

## Window access (XIP data and code)

| access | window | RAM |
|---|---|---|
| word reads in order, 128 KiB | 1.84 ms, 70 MB/s | 0.27 ms |
| random word reads, 128 KiB | 1521 ns/word | 353 ns/word |
| random word reads, 16 KiB, 3 passes | 233 / 220 / 241 ns/word | 71 to 74 ns/word |
| stride 1 / 4 / 16 / 64 words | 61 / 170 / 896 / 1193 ns | |
| 60 KiB copy | 69.6 MB/s (SDR: 38 MB/s) | |
| 2M iteration loop | 23.364 ms | 23.338 ms |

- The window is not served from the data cache: reading the same 16 KiB
  again does not get faster. A read that is not next to the previous one is
  a new flash transaction, about 0.9 to 1.2 us.
- Code loops that fit the instruction cache run as fast as from RAM. Cold or
  large code pays about 1 us per cache line miss (not measured separately).
- Unexplained: 128 KiB random is 1521 ns/word but 16 KiB random is about
  230 ns/word.

## Cost of the mapping on the rest of the system

Each command takes the window down with interrupts masked:

| command | longest interrupt latency unmapped | mapped |
|---|---|---|
| read 64 KiB | 0.7 ms | 0.74 ms |
| program 64 KiB | 1.0 ms | 1.3 ms |
| erase 64 KiB | 1.0 ms | **122 ms** |

An erase waits for the flash with interrupts off while mapped, so for the
whole erase (48 ms per 4 KiB, 132 ms per 64 KiB) no interrupt, thread switch
or DMA completion is served. Code in the window cannot run during the erase
either, because the flash is busy (a NOR part cannot be read while it erases).

## Problems found and fixed

1. DTR was never used: wrong default opcode for 3 byte addresses (fixed in the
   driver, automatic by address width).
2. Tuning and saved parameters failed with a blank tune partition (`-61`,
   `-11`): the sample writes a reference pattern when the partition is blank.
3. An earlier interrupt latency measurement showed 2 us for the mapped case
   because the first late interrupt was not counted; fixed in the test.

## Open points

- Erase while mapped blocks everything for 48 to 132 ms. Options: erase in
  4 KiB steps with interrupts enabled between steps (only helps code that is
  not in the window), or run erase code from RAM and suspend the erase
  (erase suspend command) to let XIP reads through.
- Small reads cost 40 us per call; the command path (DMA descriptor, polling)
  could be shortened for reads of a few bytes.
- Window data is uncached; hot code and data should be placed in RAM with the
  existing `__xip_text` split, window data read in order.
- Not tested: a clock above 100 MHz, continuous read mode, other flash parts,
  the EVB (GD25Q128), long soak runs.
