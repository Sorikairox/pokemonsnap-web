# Audio: aspMain HLE and the virtual AI (port/audio, session S7)

How the port plays the game's sound. Code: `port/audio/` (`audio.h` interface, `audio_hle.c` the microcode,
`audio_ai.c` the AI and the host's PCM, `test/` offline checks, `tools/wav_report.mjs` WAV analysis). Hooks in the PC
layer: `port/ultra/pc_rcp.c` (audio branch of `osSpTaskStartGo`, `osAi*`, one call per retrace), `port/ultra/pc_io.c`
(`AI_LEN_REG` / `AI_STATUS_REG` reads), `port/tools/port_build.py` (sources, exports).

## What the game uses (survey)

- **libaudio, not n_audio**: splat.yaml links ultralib `src/audio/` (`synthesizer.c`, `load.c`, `resample.c`,
  `env.c`, `reverb.c`, `mainbus.c`, `auxbus.c`, `save.c`, `drvrnew.c`, sequence and sound players); the task's
  ucode is `aspMainTextStart` / `aspMainDataStart` (`src/sys/audio.c` auThreadMain). 32000 Hz asked, the AI gives
  48681812 / 1521 = **32006 Hz**. One audio task per retrace (frames of 528 or 544 samples, see "AI").
- Commands emitted (abi.h numbering): SPNOOP 0, ADPCM 1, CLEARBUFF 2, ENVMIXER 3, LOADBUFF 4, RESAMPLE 5, SAVEBUFF 6,
  SEGMENT 7 (always segment 0 = 0), SETBUFF 8, SETVOL 9, DMEMMOVE 10, LOADADPCM 11, MIXER 12, INTERLEAVE 13,
  POLEF 14 (reverb low-pass), SETLOOP 15. No A_PAN. Unknown opcodes are logged (`[audio] unknown command`; none on
  the Beach route).
- Effects: `AL_FX_CUSTOM` with `auCustomFXParams1/2` (delay lines, chorus through RESAMPLE, low-pass through POLEF).
- `sys/audio.c` sizes each frame from `IO_READ(AI_LEN_REG)` (what the AI still has to play): with the register
  reading 0 (before S7) every frame was 640 samples, **music ran 1.2x too fast** (640 / 533.4).

## Sources and licences

- The **ABI** (command numbers, field layout, flags) from ultralib `include/PR/abi.h`; the **semantics** from how the
  game's libaudio builds the lists and reads DMEM afterwards (e.g. load.c's `*outp += lastsam << 1` fixes where the
  ADPCM output starts, env.c's `_getRate` / `_getVol` the ramp rule, drvrnew.c `_init_lpfilter` the low-pass
  coefficients, reverb.c the buffer moves). ultralib is the libultra decompilation shipped with the decomp (no
  licence file; SGI notices in the files): read as documentation, nothing copied into `port/audio/` beyond ABI facts.
- The resampler's 64-phase 4-tap table is the microcode's own (aspMain data + 0xC0), read from game memory (the
  player's ROM) at run time through the task's `ucode_data`; never stored in the repo.
- **No other HLE code was used or consulted** (in particular not mupen64plus-rsp-hle or GLideN64, GPL). No network
  access in this session (n64brew was not fetched); where the ABI leaves arithmetic open (rounding, ramp granularity,
  state layouts) the choice is ours and written next to the code. Everything else is checked numerically (below).

## Design

- **DMEM** is a private 4 KiB array in the same virtual big-endian layout as game memory (word native, halfword at
  `a ^ 2`, byte at `a ^ 3`), so LOADBUFF / SAVEBUFF are word copies; like the SP DMA engine they round both addresses
  down and the length up to 8 bytes. DMEM addresses wrap at 4 KiB.
- **DRAM addresses** in commands are whatever the game's `osVirtualToPhysical` / `K0_TO_PHYS` give: physical (the
  RDRAM mirror), wasm addresses of C objects or of the mirror (>= 0x10000000; the 0x1FFFFFFF mask leaves them
  unchanged; `auHeapBuffer` is a C array, so most are wasm addresses), or KSEG0. `dram()` resolves all three (and
  segments below 0x10000000), bounds-checked, logged when bad (none on the route).
- **States** (ADPCM_STATE 32 B, RESAMPLE_STATE 32 B, ENVMIX_STATE 80 B, POLEF_STATE 8 B) are the microcode's
  private business; ours use their own layout inside those sizes, except the ADPCM state, which the game also fills
  (a wave's loop state from the bank): the 16 samples of the previous frame.
- **Integer arithmetic only**, no wall time: the same PCM on every host (node, Chrome).

| Command | This HLE |
|---|---|
| ADPCM | writes the previous frame's 16 samples (state, loop state with A_LOOP, zeros with A_INIT), then `ceil(count / 32)` frames: header `scale << 4 | predictor`, 16 signed nibbles `<< scale`; per 8 samples `y[j] = (c0[j] y[-2] + c1[j] y[-1] + 2048 r[j] + sum_{k<j} c1[j-1-k] r[k]) >> 11` (Q11 codebook rows of the predictor), saturated; the last frame becomes the state |
| RESAMPLE | pitch 1.15 (0x8000 = 1); window = 4 history samples + input; output `i` = the table phase `(pos >> 10) & 63` over window taps `idx..idx+3`, `idx = pos >> 16`, `pos = frac + i * 2 * pitch` (Q16), rounded >> 15; the state keeps the 4 window samples at the integer position reached and the fraction, so the next call's input continues exactly after them (libaudio's `inCount = delta + ratio * outCount`) |
| ENVMIXER | sample x volume (Q15) x dry / wet (Q15), saturating adds into dry L/R, wet L/R (SETBUFF A_AUX); volumes 16.16, multiplied by the rate once per 8 samples and clamped at the target (env.c "rate values are actually rate^8") |
| SETVOL | A_AUX: dry (w0), wet (w1 low); A_VOL: start volume L/R; else target L/R and 32-bit rate |
| POLEF | one-pole low-pass in the ADPCM-like 8-sample form, Q14 (libaudio SCALE 16384): coefficients from LOADADPCM (row 0 zeros, row 1 fc^(j+1)), input x gain first |
| MIXER | `out += in * (s16)gain >> 15`, saturated (0xDA83 = -0.293 in reverb.c) |
| INTERLEAVE | L, R from the two inputs to SETBUFF's out, count = bytes per channel |
| CLEARBUFF, DMEMMOVE | exact byte counts |

### AI (audio_ai.c)

- The AI's FIFO: one playing DMA + one queued; `osAiSetNextBuffer` returns -1 when both are taken (as libultra on
  `AI_STATUS_FIFO_FULL`); `AI_LEN_REG` = bytes left in the playing DMA; `AI_STATUS_REG` full / busy bits.
- **DAC time advances with the retraces only**: `port_vi_retrace` first consumes `rate / 60` stereo frames (exact
  integer accumulator: 32006 / 60 = 533.43), reading the samples from game memory as the AI's DMA would, then the
  threads run. So `AI_LEN_REG`, which sizes the game's next frame, depends on the retrace count only. Measured: the
  game settles at 528 / 544-sample frames (mean 533.4), **0 FIFO-full refusals and 2 underruns** (at boot, before
  the first buffer) over 13000 retraces.
- An empty FIFO plays silence (frames still counted), so the stream stays continuous in time.
- **Host interface** (retrowser standard 4.6, exported now; S9 wires the module): `rtw_audio_rate()` = the AI rate
  (0 before `osAiSetFrequency`), `rtw_audio_drain()` = stereo frames since the last call, int16 L,R at
  `rtw_audio_buffer()` (16384 frames kept; if the host stops draining, the oldest half is dropped).
- Tests: `port_audio_capture(1)` keeps every frame (`port_audio_capture_ptr/frames`), `port_audio_pcm_hash()` an
  FNV-1a of everything played; `[audio] retrace N: ... PCM hash` every 600 retraces in the log.

## Checks (how to run, what they showed)

1. `port/audio/test/run.sh` (node in the emsdk image, the ROM read at run time): command lists written by the test,
   run through `port_audio_task`:
   - `adpcm_loopstate`: **70 of 70** looped ADPCM waves of both banks, decoded in 64-frame tasks (state saved and
     restored between them), end exactly on the loop state the encoder stored in the bank (bit-exact).
   - `resample_sine`: a 440 Hz sine at pitches 0x2000..0xFFFF, 160 samples per task with libaudio's input count:
     frequency exact, SNR 52-69 dB; a mutant whose input slips one sample every second task drops to 24 dB.
   - `envmixer_ramp`, `polef_dc` (unity DC gain, y0 5000 / y1 7500 for fc 0.5), `mix_interleave`.
2. Game route, node: `port/docker/run.sh port-test --out port_work/s7/route_a --wav port_work/s7/route_a/audio.wav
   --frames 13000 --tap 120 0x8000 --input 2940:0x4000:3,4000:0x2000:400` (title, intro, menus, Beach start to end).
   `node port/audio/tools/wav_report.mjs FILE.wav [--png OUT --from S --to S] [--tempo S0,S1 --bpm LO,HI]
   [--tuning S0,S1]`:
   - level: peak -6.1 dBFS (browser run with photos -4.3), RMS -26.5 dBFS, **0 clipped samples** in 216 s;
   - **tempo** (onset autocorrelation) vs the sequence player's own tempo (`alCSPGetTempo`, logged once from a
     temporary debug build): song 23 140.1 BPM -> measured 139-140; song 12 120.08 -> 60 (half); song 8 114.05 ->
     114.0; Beach (song 0) 96.0 -> **96.0** over 65..200 s;
   - **pitch**: the strongest spectral peaks sit on the equal-tempered grid around A4 = 440 Hz: mean offset +0.9 cents
     (Beach), -1.7 / +3.1 (intro / title), histograms centred on 0;
   - spectrograms looked at: harmonic notes, percussion, no periodic clicks at task (60 Hz) or 160-sample
     boundaries. The Beach section has a broadband noise floor; not from the reverb (identical with the wet path
     cut, temporary build), it starts with the Beach song (song 0 at retrace 3723): taken as the song's sea sound,
     not verified against a console.
3. Determinism: two node runs: WAV and `tasks.txt` identical (PCM hash cc8c4486). Browser
   (`node port/web/run.mjs ... --audio`): identical to node up to the first Pokemon in focus (retrace ~3831, where
   node without GL diverges, as before S7); node with the browser's read-back journal (`--readback`) gives a
   **bit-identical WAV** and identical `[task]` lines over 13000 retraces.
4. Cost: **0.04-0.08 ms per audio task** on average (max 0.4 ms) in node and Chrome (frames of ~533 samples).

## Open / not verified

- No console or emulator reference recording: absolute loudness (the Q15 volume chain), the ramp's 8-sample steps,
  the resampler's rounding and the exact reverb sound are ours. Bit-exactness with the real microcode is not claimed
  (it is not needed for determinism: the game never reads synthesized samples back except `auSoundQuality == 0`'s
  averaging, which is the game's own C).
- `auSoundQuality` 0 (the game's "low quality" averaging) not exercised.
- The real VI rate (~59.94 / 60.1 Hz on hardware depending on mode) is taken as exactly 60, like the port's retrace.
