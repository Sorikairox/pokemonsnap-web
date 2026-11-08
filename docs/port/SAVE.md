# Save: FlashRAM (session S8)

Pokémon Snap saves to a **1 Mbit FlashRAM** (128 KiB) on the cartridge. The port keeps the console's model: the game's
own flash driver runs on a model of the chip, and the save is the chip's plain 131072-byte image in console byte
order. retrowser: `save: { kind: "flash", size: 131072 }`, exports `rtw_save_ptr` / `rtw_save_size` /
`rtw_save_generation`.

## Layers

| Layer | Where | What |
|---|---|---|
| save logic | `src/more_funcs/5BF20.c` | one struct `UnkBigBoy` (0x1F2A4 bytes) in bss, 16-aligned; read at boot, written by "Save" |
| flash helpers | `src/more_funcs/5D500.c` | read all pages; write = per 16 KiB sector: compare, if different erase + program 128 pages + verify (3 tries) |
| libultra driver | `src/os/osFlash.c` + ultralib `flashreadstatus.c` `flashclearstatus.c` `flashwritebuffer.c` `flashreinit.c` | **compiled as game code** (vbe) since S8; before, the PC layer replaced it |
| the chip | `port/save/flashram.c` | command / status registers behind `osEPiWriteIo` / `osEPiReadIo`, PI DMA on the flash handle (routed from `pc_pi.c osEPiStartDma`) |
| host interface | `port/save/save.c` | `port_save_load`, `port_save_poll`, `port_flash_image`, `port_flash_generation`, `rtw_save_*` |
| tools | `port/save/snap_save.py` | decode + check an image (the game's acceptance test, reimplemented) |

## The chip model (flashram.c)

Written from libultra's driver (what it writes, reads and tests) and the documented FlashRAM protocol. Commands are
32-bit words written to `base | 0x10000` (base = KSEG1 0xA8000000, PI domain 2):

| Command | Effect |
|---|---|
| `3C000000` | chip erase mode |
| `4B000000 \| page` | sector erase mode: the 16 KiB sector (128 pages) holding `page` |
| `78000000` | execute the erase: bytes -> 0xFF, status 0x08 |
| `B4000000` | page program mode: the next PI **write** DMA fills the 128-byte page buffer |
| `A5000000 \| page` | program the page: `page &= buffer` (programming only clears bits), status 0x04 |
| `D2000000` | status mode: reading base gives the status word; writing base sets the status (`osFlashClearStatus` writes 0) |
| `E1000000` | silicon id mode: a PI read DMA returns `11118001 00C2001E` (FLASH_TYPE_MAGIC, FLASH_VERSION_MX_C) |
| `F0000000` | read array mode: a PI read DMA returns the array at `devAddr * 2` |

- **Id and addressing.** The id is a Macronix MX29L1100 (`MX_C`), so the game's `osFlashInit` sets
  `__osFlashVersion = 0x40` and `osFlashReadArray` passes `devAddr = page * 0x40`; MX parts address the array in
  16-bit units, so the chip reads at `devAddr * 2` = `page * 128`. Same convention as emulators that report MX_C.
- **Status.** Word `0x11118000 | status`; the driver only tests the low byte: busy bits (0x02 erase, 0x01 program)
  in its poll loops, done bits (0x08, 0x04) for the result. Reads of base in erase / program mode also return the
  status (the driver polls there without selecting status mode).
- **Timing.** Operations finish at once: each poll loop ends at its first read. On the console an erase takes tens
  of ms and the driver busy-polls (no yield) the whole time; the game shows "Saving..." and waits fixed frame counts
  (`ohWait(48)` / `ohWait(33)`, oaks_lab 8A8A50.c) around the call, so nothing it measures depends on the chip's
  time. A whole save runs within one retrace in the port (the save thread never blocks: its DMA messages are posted
  before it waits).
- **Generation.** `port_flash_gen` +1 after every completed erase or page program. A first save is 8 erases + 998
  programs (generation 1006); a re-save that only changes the header sector is 1 erase + 128 programs.
- Not modelled: erase/program failures (the game retries 3 times, then shows an error), the other chip ids (MX_A,
  MEI...), `osFlashReInit`/`osFlashChange` (unused by Snap).

## The image

- **Byte order: console (big-endian) bytes**, offset 0 = flash address 0: the format of a cartridge dump. The chip
  keeps native bytes; every DMA goes byte by byte through the vbe accessors (`vbe_st8` / `vbe_ld8`), so the game's
  BE words land as BE bytes. Some emulators store FlashRAM as 32-bit words in host (little-endian) order;
  `snap_save.py` recognises such a file and says so (convert by swapping each 4-byte group).
- Erased = 0xFF. Before any save the flash is blank; the game then starts a new file (checks below fail).

### Layout (`UnkBigBoy`, src/more_funcs/more_funcs.h; offsets in the image)

| Offset | Field | Notes |
|---|---|---|
| 0x00 | MD4 a, b, c, d | over `data` = 0x10..0x1F2A4, the game's MD4 variant (md4.c: bit counter as 8 LE bytes, data words read BE) |
| 0x10 | "HAL_SNAP_V1.0-1\0" | version string |
| 0x20..0x2C | 0x6C, 0x180, 0xFBA0, 0x10A20 | self offsets of the arrays below |
| 0x30 | osGetTime() low word | at save time (deterministic clock in the port) |
| 0x34 | s32[7] | per-course values (`func_800C0224_5D0C4`: HI-SCOREs shown in the lab once PFID_11 is set) |
| 0x50 | s32 | clamped 0..6 (`func_800C02A0_5D140`) |
| 0x54 | player name | 16 bytes, EUC-JP (full-width letters `A3C1`...) |
| 0x64 | u32 bitfields | `unk_64_29:3`, `unk_64_22:7` (default 0x4C), `unk_64_16:6` (default 0x24), then 16 player flags: hasApple (bit 15) ... unk_64_00 (bit 0) |
| 0x68 | u32 bitfields | unk_68_31..27 (1 bit each), unk_68_26:27 |
| 0x6C | s32[69] | per-Pokemon report scores (index from `func_8009BB4C`; the route's Pidgey: index 8) |
| 0x180 | PhotoData[69] | report photos, one per Pokemon (`unk_04 == -1` = empty) |
| 0xFBA0 | PhotoData[4] | |
| 0x10A20 | AlbumPhotoData[60] | album: PhotoData + 64-byte comment (`photoData.unk_04 == -1` = empty) |
| 0x1F2A4.. | | rest of the last written page comes from the RAM buffer (0xFF if read from a blank flash); pages after 0x1F300 never written; pages 0x3FE-0x3FF never read or written by the game |

The game accepts a save when the version string and the MD4 match (5BF20.c `func_800BF178_5C018`), else it creates a
new file (`func_800C042C_5D2CC`) and flags it.

### Bitfields

IDO allocates bitfields from the MSB, clang from the LSB (MEMORY.md "Bitfields"). The two flag words at 0x64 / 0x68
were **not converted before S8**: the port stored e.g. `unk_64_22 = 0x4C` in other bits than a console save would,
and an image from (or for) a console or emulator would read wrong flags. S8 declares them LSB first under
`#ifdef PORT` (more_funcs.h). Check: a port save holds word 0x64 = `13240120`, i.e. 0x4C at bits 28..22 and 0x24 at
bits 21..16 as on the console. `PhotoData` / `PokemonPhotoData` (photos in the save) were converted in S3a.
Other single-bit structs checked in S8 do **not** reach the save: `camera_check.h UnkIndigoHalibut.unk_18_*`
(camera-check bss, C bitfield access and whole-struct `qsort2` copies only) — left unconverted.

## Hosts

- retrowser (S9): copy the stored image to `rtw_save_ptr()` before `main`; persist when `rtw_save_generation()`
  changes. The game reads the flash once at boot, so a save loaded later is seen at the next boot.
- node: `node port/tools/node_run.js ... --save IN.fla --save-out OUT.fla` (via `port/docker/run.sh port-test`).
- browser harness: `node port/web/run.mjs ... --save IN.fla`; the worker POSTs the image after every retrace whose
  `port_save_poll()` reports a change -> `OUT/flash.fla`.
- `[save]` log lines (deterministic, in node's tasks.txt): host load, each silicon-id read (boot, start of each
  save), each generation change with the erase / program counts.
- Save images are personal, ROM-derived data: keep them in `port_work/`, never commit.

## Verification (S8)

Route: the S6 Beach route then Oak's dialogue and **Save** (`port_work/route_s8.txt`; docs/port/notes/S8.md):
save at retrace 17961 (8 erases, 998 programs), `snap_save.py`: version OK, MD4 OK (independent reimplementation),
offsets OK, name "ＡＡＡＡＡＡＡ", Pidgey report score 2810, report photo slot 8 (Beach), flags 0x4C / 0x24 at the console
bits. Second boot with that image: the title shows **New Game / Continue / Options**, Continue goes to the lab
("I have been waiting for you, AAAAAAA."), PKMN Report: 1 kind, 2810 points. Re-save from there: 1 sector rewritten.
Browser runs repeat byte-identically (image and screenshots); node with the browser's read-back journal writes the
same image.
