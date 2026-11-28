# H.264 decode on the F133's video engine, from bare metal (2026-11-27)

A plan for decoding CarPlay's H.264 with the SoC's hardware video engine
(Allwinner "Cedar" VE) in our own firmware (`tools/fel/dram/`): no OS, a
polled loop. It is item #13 of [carplay-host.md](carplay-host.md) ("H.264
decode → YUV video layer, rotated 90° for the panel"), and is needed whichever
way the CarPlay session ends up being hosted.

**Static analysis and web research only. Nothing here has run on hardware.**

Sources:
- **Public:**
  - Linux `drivers/staging/media/sunxi/cedrus/` (GPL-2.0+): `cedrus.c`,
    `cedrus_h264.c`, `cedrus_hw.c`, `cedrus_regs.h`.
  - `drivers/clk/sunxi-ng/ccu-sun20i-d1.c`, `drivers/soc/sunxi/sunxi_sram.c`,
    `drivers/pmdomain/sunxi/sun20i-ppu.c`, `sun8i_mixer.c`, `sun8i_vi_layer.c`.
  - The D1 DTs. Line numbers are from torvalds/linux `master` as fetched
    2026-11-27.
  - Samuel Holland's D1 video-engine series, Dec 2022/Jan 2023, linux-arm-kernel.
  - libvdpau-sunxi `h264.c` (LGPL-2.1+).
  - linux-sunxi wiki "VE Register guide" (archived copy).
  - The F133 datasheet ("The Most Cost-Effective Decoding Platform
    Processor", §2.3–2.4).
  - Allwinner's Tina D1 kernel DT (`sun20iw1p1.dtsi`, rvboards
    `linux_kernel_for_d1`).
- **Stock:**
  - `vdecoder.drv`: libcedarc for Melis, RISC-V, loaded at `0xe5600000`. File
    offset = VA − `0xe5600000` + `0x160` for `.text`; `.rodata` starts at VA
    `0xe5660448`, file offset `0x605b0`.
  - `cedar.mod` (loaded at `0xe9400000`).
  - `kernel.bin`.

Nothing from the Allwinner SDK, libcedarc or stock binaries is copied here.
Our repo is MIT, while cedrus is GPL and libvdpau-sunxi LGPL, so **our driver
has to be written fresh from the register facts below, not ported line by
line.**

"Confirmed" means read directly from a source or the disassembly. "Guess"
means not checked.

## Summary

| Question | Answer | Confidence |
|---|---|---|
| VE base | `0x01C0E000` (0x2000 window in Holland's DT; Tina uses 0x1000) | confirmed (DTs + stock `lui a5,0x1c0e` at `0xe560dd60`) |
| Interrupt | PLIC **82** (`SOC_PERIPHERAL_IRQ(66)`) | confirmed (DT + stock installs its ISR on 82 at `0xe560dcf2`) |
| Clocks | CCU `0x02001690` VE_CLK, `0x0200169C` VE_BGR (gate bit 0, reset bit 16), `0x02001804` MBUS gate bit 1, PLL_VE `0x02001058` | confirmed (Linux CCU driver); D1 manual not cross-checked |
| SRAM-C mux | **not needed**: "the usual bits in the SRAM controller register have no effect on the video engine" (Holland) | confirmed (author statement) |
| Power domain | PPU domain 1 (`0x07001080`), **on by default** | confirmed (Holland, `sun20i-ppu.c`) |
| Engines | H.264 BP/MP/HP@L4.2 1080p60, H.265 MP 1080p60, MPEG-1/2/4, H.263, JPEG/MJPEG; no VP8 in cedrus's D1 variant | confirmed (F133 datasheet §2.3, `cedrus.c:620`) |
| Open-source driver | **Linux cedrus supports `allwinner,sun20i-d1-video-engine`** (UNTILED + MPEG2 + H264 + H265, 432 MHz). Holland: "D1 contains a video engine which is supported by the Cedrus driver" and it works without power-domain management | confirmed |
| Register layout | Same H.264 block as cedrus/libvdpau-sunxi (`+0x200`): stock writes VLD at `0x230`–`0x23C`, trigger 7 to `0x224`, SRAM port `0x2E0/0x2E4`; ISR reads `0x220/0x228` | confirmed (stock) |
| VE generation | New-style IP: stock reads `0xF0`, and if it is 0 or `0x16670040` it reads a 64-bit ID from `0xE0:0xE4`. The D1's value is not published | partly confirmed |
| Output format | Linear **NV12** via `0xEC = 4<<4` (cedrus "UNTILED") | confirmed (cedrus); not yet on our unit |
| Display | DE2 mixer 0 channel 0 is a VI layer that takes linear NV12 with CSC and a scaler. **No rotation in the DE**, so the 90° turn for our portrait panel is a separate step | confirmed (Linux mixer cfg + our `display.c`) |

## 1. The hardware

### 1.1 Addresses, clock, reset, interrupt

- **Base:** `0x01C0E000`.
  - Holland's DT patch (`sunxi-d1s-t113.dtsi`, never merged) maps `0x2000`.
  - Tina's `sun20iw1p1.dtsi:1247` maps `0x1000`, plus `0x03000000/0x10` (system
    control) and `0x03001000`.
  - Stock confirms the base: `vdecoder.drv` stores `0x01c0e000` as its
    register base in `VeInitialize` (`0xe560dd60`, `lui a5,0x1c0e`).
- **Interrupt: PLIC 82.**
  - DT: `SOC_PERIPHERAL_IRQ(66)` = 66 + 16 on RISC-V. Tina: `<&plic0 82>`.
  - Stock: `VeInitialize` calls the kernel ISR-install wrapper (`0xe56054fe`)
    with `a0 = 82` and handler `0xe560d136`, then enables 82 (call at `0xe560dd2a`).
  - We poll, so this is only needed later.
- **CCU** (`0x02001000`), from `ccu-sun20i-d1.c`:

| Register | Address | Fields |
|---|---|---|
| PLL_VE | `0x02001058` | enable 27, lock 28, N bits 8–15 (24 MHz × N) |
| VE_CLK | `0x02001690` | M bits 0–4 (÷M+1), mux bit 24 (0 = PLL_VE, 1 = PLL_PERI0_2X), gate bit 31 |
| VE_BGR | `0x0200169C` | bus gate bit 0, **reset deassert bit 16** |
| MBUS_MAT_CLK_GATING | `0x02001804` | VE master gate bit 1 |

  - The bus-gate and reset write is the same pattern `display.c` uses for the
    DE (`0x0200160c`).
  - **Simplest clock:** mux = PLL_PERI0_2X (1200 MHz), M = 2 (÷3) gives
    **400 MHz** without touching PLL_VE. Cedrus asks for 432 MHz on the D1.
  - Stock's version table (below) lists per-IP default frequencies between
    300 and 600 MHz (guess: that field is MHz).
- **SRAM:** nothing to do. The cover letter of Holland's series says there is
  no switchable SRAM section on the D1. `sunxi_sram.c`'s
  `sun20i-d1-system-control` has no VE entry.
- **Power:** the VE power domain (PPU at `0x07001000`, domain 1) is on at
  reset (Holland: "All of the power domains are enabled by default").
- **DRAM addressing (guess):** on the D1 no DT node routes the VE through the
  MBUS `dma-ranges`, so cedrus hands it CPU physical addresses
  (`0x4xxxxxxx`).
  - The H3 is different: there the VE sees DRAM at bus address 0.
  - `display.c` already gives the DE full physical addresses (`0x41000000`)
    and that works.
  - Stock's VE ops have a `getPhyOffset` hook (in the public
    `veInterface.h`), and no `0x40000000` constant appears in stock's VE
    layer (`0xe560c000`–`0xe560e800`).
  - The bitstream address register packs bits 28–31 separately:
    `(a & 0x0ffffff0) | (a >> 28)`.
  - First test, risk 2.

### 1.2 Which engine generation

`0xF0` (VE_VERSION) holds the old-style ID in bits 31–16. The public table
(linux-sunxi wiki) gives `0x1623` (A10), `0x1625` (A13), `0x1633` (A31),
`0x1639` (A80), `0x1667` (A33), `0x1680` (H3), `0x1681` (V3s/S3), `0x1689`
(A64), `0x1718` (H5).

Stock reads the version as follows (`0xe560dbb2`):

1. Read `0xF0`.
2. If it is non-zero and not `0x16670040`, use `0xF0 >> 16`.
3. Otherwise read a 64-bit ID as `(0xE0 << 32) | 0xE4`, retrying 3× with a
   1 ms delay.

It then looks the ID up in a 21-entry table (`0xe5664058`, 24 B each). The
table holds the old IDs listed above plus `0x1650`, `0x1663`, `0x1673`,
`0x1701`, `0x1707` and five new-style IDs:

| ID | default freq field |
|---|---|
| `0x3101000012010` | 552 |
| `0x3011100020010` | 300 |
| `0x3011100021010` | 600 |
| `0x3011100012010` | 540 |
| `0x3011100021110` | 600 |
| `0x2111000021210` | 300 |
| `0x1301000010210` | 576 |

For comparison, a V851 reports `0x1302000023010` (whycan.com t_10946). That
fits the same scheme.

**The F133's value is unknown.** It is one of these IDs, most likely a
new-style one. Reading `0xF0`, `0xE0` and `0xE4` on the bench settles it
(test step 1).

What matters for us: cedrus's D1 variant drives the **same register set** as
the H3/A64 generation, with linear output. Stock's H.264 code on this chip
writes the same offsets. So the H3/A64 documentation (cedrus,
libvdpau-sunxi, the wiki) applies. The closest documented relatives are
**H3/A64/H5**: same capability set as the D1 apart from VP8.

### 1.3 Limits

- **F133 datasheet §2.3:** H.264 BP/MP/HP@L4.2 up to 1080p@60fps.
  CarPlay's 1280×720 or 800×480 at 30–60 fps is well inside that.
- **Hardware fields:** the SPS register has 8-bit width and height in
  macroblocks. Widths ≤ 2048 use the VE's internal SRAM for deblocking and
  intra prediction (`VE_BUF_CTRL = 0`), so no extra buffers are needed.
- **Format:** 4:2:0 8-bit only (no 10-bit in the D1 variant).

## 2. How stock drives it

### 2.1 Path

The chain is `carplay.mod` → host event 2 (one access unit) → `cedar.mod` in
raw-stream mode → `vdecoder.drv` (libcedarc) → the VE. Decoded frames go to a
`disp` video layer, and `/dev/g2d` does the rotation (see
[carplay-host.md](carplay-host.md)).

`vdecoder.drv` is a full libcedarc with a Melis kernel shim:
`esMEMS_Palloc/VA2PA/CleanFlushDCacheRegion`, `esINT_InsISR`, `esCLK_*`.
Clock, reset and PLL writes go through those kernel calls, not direct CCU
writes.

### 2.2 Function map (`vdecoder.drv`)

Functions are named from their `__func__` log strings. Addresses are log
sites inside each function, not exact entry points, unless marked "entry".

| Function | Where | What matters |
|---|---|---|
| version read | `0xe560dbb2` (entry) | `0xF0`, then `0xE0:0xE4` |
| `VeInitialize` | `0xe560dc4a`–`0xe560dfd0` | IRQ 82, base `0x01c0e000`, table lookup, DDR mode, clock |
| DDR-mode/reset helper | `0xe560cfda` (entry) | read-modify-write of VE reg `0x00` bits 16–17 and 20, chosen per IC ID |
| `VeSetDdrMode` | `0xe560d0fe` (entry) | clamps the mode to ≤ 10 (default 5), stores it, calls the helper |
| ISR | `0xe560d136` (entry) | reads VE reg `0x00` low nibble (engine). For H.264 (1): status `0x228 & 0xF`, ctrl `0x220`; clears the ctrl enable bits, posts a semaphore |
| `VeGetGroupRegAddr` | `0xe560d2b6` | per-engine register base |
| `H264MallocBuffer` | `0xe561b4c0`, `0xe561b6b6`, `0xe561bb2c` | MV-col buffers per picture |
| `H264ConfigureBitstreamRegister` | `0xe561dc28`–`0xe561df38` | VLD_OFFSET `0x234`, VLD_LEN `0x238`, VLD_END `0x23C`, VLD_ADDR `0x230`, then **TRIGGER `0x224` = 7** (`0xe561dd36`), then SRAM port writes `0x2E0/0x2E4` |
| `H264DecodeSps` | `0xe5621486` | software SPS parse |
| `H264DecodeSliceHeader` | `0xe5622a6e`–`0xe5622fda` | slice header |
| `H264JudgeFrameEnd` | `0xe561cb66` | |
| `H264DecoderDecodeSbmFrame` | `0xe561a3cc` | per-frame entry from the stream buffer |

The full ordered register sequence for one frame has not been traced.
Everything stock writes that has been checked matches cedrus's layout, so
cedrus is used as the sequence (§3).

### 2.3 Output and display

- libcedarc's pixel-format names in `vdecoder.drv` include `YUV_MB32_420`
  (32×32-tiled), `YV12` and `YUV_PLANER_420`. Which one stock picks for
  CarPlay hasn't been traced.
- `cedar.mod` logs `lbc_mode[%d],lbc_lossy[%d],vdecode_scaledown[%d]`
  (string at `0xe940dc58`, decision just before at `~0xe94060f0`, compared
  against 384000 = 800×480 px).
  - The F133 DE "supports LBC buffer decoder" (datasheet §2.4.1).
  - **Guess:** for larger streams, stock has the VE write LBC-compressed
    frames that the DE reads directly, to save DRAM bandwidth.
- Rotation for the portrait panel is done with G2D (`/dev/g2d`, strings
  `g2d hw_scale_yuv`, `rotateAngle` in `init.axf`).
- We don't need either LBC or G2D for a first version.

## 3. The open-source reference: cedrus `cedrus_h264.c`

Linux cedrus is the reference to follow: it supports this exact SoC and
does what we need, which is that software parses and the driver only
programs registers. libvdpau-sunxi `h264.c` is the second reference. It
reads the slice header **through the VE's own bit reader**, which is handy
on bare metal (below).

### 3.1 One-time setup per stream (`cedrus_h264_start`, `cedrus_hw.c`)

- **pic_info buffer:** `max(130 KiB, 18 × 0x1000 + height × 2 × 64)`. At
  720p that is 165,888 B.
- **Neighbor-info buffer:** 32 KiB, **16 KiB-aligned**.
- **Per output frame, an MV-col buffer:**
  - field = ⌈w/16⌉ × ⌈h/16⌉ × 16, ×2 if not `direct_8x8_inference`, ×2 if
    not `frame_mbs_only`;
  - buffer = 2 × field. At 720p with the usual flags that is 115,200 B.
- **Output format** (`cedrus_dst_format_set`, `cedrus_hw.c:79–129`):
  - `0xEC` = `4<<4` (NV12 linear);
  - `0xC4` = chroma size / 2;
  - `0xC8` = luma stride ALIGN(w,16) in bits 0–15, chroma stride in bits
    16–31.
  - Tiled MB32 would be `0xEC` = 0, which needs a de-tiler. Avoid it.

### 3.2 Per slice (`cedrus_h264_setup` `:519`, `cedrus_set_params` `:344`, trigger `:698`)

1. VE reg `0x00` (VE_MODE) = H.264 (1) | DDR mode `3<<16` | `1<<20`.
   Cedrus's FIXME says the DDR-mode value is only valid for 32-bit DRAM.
   Stock instead picks bits 16–17 per IC ID. See risk 3.
2. `0x240` SDROT_CTRL = 0. `0x250` = pic_info. `0x254` = neighbor_info.
3. Scaling lists to VE SRAM, only if the PPS has them.
4. **Frame list** to SRAM at word `0x100`: 18 × 32 B entries
   `{top POC, bottom POC, pic_type<<8, luma addr, chroma addr, mvcol top, mvcol bottom, 0}`.
   Then `0x24C` OUTPUT_FRAME_IDX = the output slot.
5. Bitstream:
   - `0x238` = bytes × 8;
   - `0x234` = 0 (bit offset);
   - `0x23C` = end address;
   - `0x230` = `(a & 0x0ffffff0) | (a >> 28)` | FIRST (bit 30) | LAST
     (bit 29) | VALID (bit 28).
6. `0x50` BUF_CTRL = 0 (internal SRAM, width ≤ 2048).
7. `0x224` = 7 (INIT_SWDEC).
8. Skip the slice header: `0x224` = `3 | n<<8` (n ≤ 32) repeatedly, polling
   `0x228` bit 8 (VLD busy). Cedrus notes that skipping via `0x234` is
   unreliable.
9. Prediction weight table (SRAM word `0x000`, via `0x210`) if needed.
10. Ref list 0 (word `0x190`) for P and B slices, list 1 (word `0x199`) for B
    slices. One byte per entry: `slot<<1 | bottom`.
11. `0x204` PPS, `0x200` SPS, `0x208` SHS, `0x20C` SHS2, `0x21C` SHS_QP. Field
    packing is in `cedrus_set_params` `:405–:478`.
12. Clear `0x228` by writing back the value read. Set `0x220` = 7 (slice
    done, error and data-request interrupts enabled; we poll the status).
13. `0x224` = 8 (AVC_SLICE_DECODE). Wait for `0x228` bit 0 (done) or bits 1–2
    (error), then write the status back to clear it.

SRAM access: write the word address << 2 to `0x2E0`, then stream words to
`0x2E4`.

### 3.3 libvdpau-sunxi's bare-metal-friendly variant

libvdpau-sunxi `h264.c` (`:820–:950`) handles the slice header differently:

1. It points the VLD at the slice with `0x234` = bit offset just past the NAL
   header.
2. It enables start-code detection in `0x220` (`1<<25 | 1<<10`) and triggers
   7.
3. It then **parses the slice header with the VE**:
   - `0x224 = 2 | n<<8` reads n bits;
   - 5 reads ue(v);
   - 4 reads se(v);
   - the result is read from `0x2DC` (BASIC_BITS).
4. After the slice decodes, it takes the next slice's start from `0x234`.

The VE strips emulation-prevention bytes itself. This removes the trickiest
part of a software slice-header parser: keeping the header bit count
consistent between raw and unescaped data.

## 4. Proposed bare-metal design

### 4.1 Split of work

| Part | Who | Notes |
|---|---|---|
| NAL split (Annex-B or AVCC length prefix) | CPU | CarPlay framing to be confirmed (`carplay-host.md` assumes Annex-B) |
| SPS / PPS | CPU, software (exp-Golomb on the RBSP) | Small. Keep up to 32 PPS / 1 SPS |
| Slice header | **VE bit reader** (libvdpau style) or software + flush-bits (cedrus style) | Start with the VE reader |
| POC (types 0 and 2), frame_num, DPB, sliding-window and MMCO marking, ref-list init and modification | CPU | The bulk of the code. CarPlay's encoder is low-latency, so expect **no B-frames** (guess). P-only DPB logic is enough at first |
| CABAC/CAVLC, IDCT, MC, deblock, output | VE | |
| Rotation 90° + scale + YUV→RGB | see 4.4 | |

### 4.2 Memory

Everything is physically contiguous by construction: no allocator, just a
fixed carve-out. Current map: code `0x40000000`/`0x40100000`, FB0/FB1
`0x41000000`/`0x41400000` (3.5 MiB each), stacks `0x43E00000`–`0x43FF0000`,
log `0x43FF0000`.

Proposal: **a 16 MiB VE region at `0x42000000`–`0x43000000`**. This has to be
coordinated with the carplay.mod guest window and heap
([carplay-host.md](carplay-host.md)).

| Buffer | Size at 1280×720 | Align |
|---|---|---|
| Bitstream ring (VLD) | 1 MiB | 1 KiB (guess; 16 B is required by the address packing) |
| pic_info | 166 KiB (use 192 KiB) | 4 KiB |
| neighbor_info | 32 KiB | 16 KiB |
| Frames: NV12 1280×720 = 1,382,400 B each (luma 921,600 + chroma 460,800) | 6 slots ≈ 7.9 MiB (`max_num_ref_frames` + 1 output + 1 on display; CarPlay likely uses 1–4 refs, guess) | 4 KiB |
| MV-col per frame slot | 6 × 115,200 B ≈ 675 KiB | 1 KiB |
| Rotated NV12 for the display (4.4) | 2 × 1.32 MiB | 4 KiB |

That totals about 13 MiB. At 800×480 it is about 5 MiB.

### 4.3 Cache maintenance

- Under FEL the D-cache is off (`mhcr = 0x109`), so the first test needs
  nothing.
- With `dcache_enable()`, `lib.h` has only the **whole-cache**
  `dcache_flush()` (`th.dcache.ciall`, clean + invalidate):
  1. **Before each trigger:** flush. The bitstream, SRAM-bound data and
     pic_info contents the CPU wrote are then in DRAM. No CPU-dirty lines may
     remain in frame buffers the VE is about to write.
  2. **After decode:** flush or invalidate again **before the CPU reads** the
     output (rotation, 4.4), so it doesn't see stale lines.
  3. **Rule:** the CPU must never write into a frame slot while the VE owns
     it. A late write-back of a dirty line would corrupt VE output.
- A ranged clean/invalidate (`th.dcache.cpa`/`ipa` by address) would be
  cheaper than `ciall` on a 32 KiB cache, but `ciall` is fine at first.
- VE registers are MMIO and uncached by the PMA, as with every other
  peripheral we drive.

### 4.4 Getting frames on screen

`display.c` drives DE2 mixer 0 **channel 1 (UI)** with 720×1280 XRGB8888.
The panel is portrait in memory and viewed landscape ([display.md](display.md)
"Orientation"): screen (sx, sy) = buffer (x = 719 − sy, y = sx). The DE2
mixer cannot rotate. Options, in build order:

1. **First test: CPU convert + rotate into the existing UI framebuffer.**
   NV12 → XRGB with the 90° turn, written into FB0. It is about 30 lines and
   reuses `display_flip()`.
   - At 1280×720 that is 921,600 px per frame. Guess: ~10–15 cycles/px on
     the C906 at 1008 MHz, so ~10–14 ms per frame, which is fine for a still
     test but marginal at 30 fps.
2. **Real path: VE → NV12 → rotate NV12 on the CPU → VI layer.**
   - Bring up mixer 0 **channel 0 (VI)** at `0x05102000`. Linux's D1 mixer 0
     config is 1 VI + 1 UI with `CCSC_D1_MIXER0_LAYOUT` for the colour-space
     converter.
   - The layer gets NV12 at 720×1280 portrait and is placed under the UI
     layer, so our own UI can be drawn on top with alpha.
   - The CPU transposes only 1.5 B/px in 32×32 tiles (~1.4 MB in/out per
     frame). The DE does YUV→RGB, and scaling if we ask CarPlay for 800×480
     (its VI scaler).
   - Code: ~150 lines (VI layer + CSC coefficients + scaler).
3. **Later: no CPU pass at all.** Either:
   - the VE's own **scale-down/rotate output** (`0x240` SDROT_CTRL,
     `0x244`/`0x248` luma/chroma addresses; named in the wiki, bit layout
     undocumented; stock has `VDRV_INTERNAL_CMD_SET_ROTATE`); or
   - G2D rotation (stock's choice; no public driver for the D1's G2D).

   Both need RE first.

Also: **ask the phone for a size we like.** Host cmd 9 sets the stream size
([carplay-host.md](carplay-host.md)). 800×480 halves the decode and rotation
cost, and the VI scaler brings it back to full screen.

### 4.5 Code size (estimate)

| Part | Lines of C |
|---|---|
| `ve.c`: clocks, reset, version probe, SRAM port, slice program, poll | ~350 |
| `h264.c`: NAL split, SPS/PPS, slice header (VE reader), POC, DPB and marking, ref lists | ~900–1200 |
| `display.c` additions: VI layer, CSC, scaler | ~150 |
| `yuv.c`: NV12 rotate (tile transpose) + NV12→XRGB fallback | ~120 |
| Total | **~1.5–1.8 k lines, ~20–30 KB of code** |

That is small next to stock's 394 KB `vdecoder.drv` `.text`, which has every
codec.

### 4.6 First bench test (`vdectest.c`, FEL run, D-cache off)

Test stream: one 1280×720 IDR frame generated on the Mac and embedded as a C
array.

- Command: `ffmpeg -f lavfi -i testsrc2=size=1280x720 -frames:v 1 -c:v
  libx264 -profile:v baseline -bsf:v h264_mp4toannexb out.h264`.
- About 20–60 KB.
- Single slice: add `-x264-params slices=1`.
- The stream is our own, so there is no licensing issue.

Steps, each logged:

1. **Probe:**
   - VE_BGR: bus gate on, reset deasserted. MBUS gate bit 1. VE_CLK =
     PERI0_2X/3 = 400 MHz, gate on.
   - Log `0x01C0E0F0`, `0x01C0E0E0`, `0x01C0E0E4`.
   - This one step settles the IC ID and proves the VE is clocked. Reads of
     an unclocked block return 0 or hang, so do this first, alone.
2. **Parse** SPS, PPS and the IDR slice header in software, and log the
   fields: 80×45 MBs, `frame_mbs_only`, POC type, `header_bit_size`.
3. **Program** per §3.2:
   - frame list with one entry (slot 0, POC 0);
   - output NV12 at `0x42100000` (luma) / `+0xE1000` (chroma);
   - no ref lists;
   - trigger 8;
   - poll `0x228` with a 100 ms timeout. Log status and cycles.
4. **Show:** CPU NV12→XRGB with rotation into FB0, then `display_up` /
   `display_flip`.
   - Pass: testsrc2's colour bars and counter appear upright on the screen.
   - Also dump 64 KiB of luma over the log/FEL for a byte compare against
     `ffmpeg -pix_fmt nv12` output.
5. **If the result is all zeros or garbage,** retry with:
   - DRAM-offset addresses (`a − 0x40000000`);
   - DDR mode `2<<16`;
   - VLD start at the NAL header byte instead of after it.

Then: a 2-second P-frame clip (`-g 60 -bf 0`) for the DPB, timing per frame,
then the VI-layer path.

## 5. Risks and unknowns, and the cheapest way to settle each

| # | Risk | Cheapest check |
|---|---|---|
| 1 | The F133's VE is a new-style IP (`0xE0:0xE4` ID) whose H.264 block differs from the H3 layout | Low risk: cedrus has a D1 variant, Holland reports it working, and stock writes the same offsets. Bench step 1 reads the ID; step 3 decodes |
| 2 | Bus address form: CPU physical (`0x4…`) vs DRAM offset | Decode with physical first, then offset (bench step 5). Statically: trace stock's `CdcVeGetPhyOffset` implementation in `vdecoder.drv` |
| 3 | DDR-mode bits (VE reg 0 bits 16–17) for the F133's 16-bit DDR2. Cedrus's value is marked "32-bit DRAM only" | Trace stock's helper at `0xe560cfda` with the mode stock sets on this chip (default 5 → jump table at `0xe56639bc`), or try 0–3 on the bench |
| 4 | Rotation cost on the CPU at 30–60 fps | A CPU-only FEL benchmark of an NV12 32×32-tile transpose on a 1280×720 buffer, no VE needed. If it's too slow: ask the phone for 800×480 (risk shrinks 2.4×), or RE `SDROT_CTRL` from stock's rotate path |
| 5 | VI layer + CSC not yet brought up in our DE code | A test pattern: draw an NV12 colour ramp in RAM and show it on channel 0. No VE needed, so it can run in parallel with step 1 |
| 6 | Slice-header bit accounting (emulation-prevention bytes, NAL header) | Use the VE bit reader (libvdpau style); compare against software parse on the test stream |
| 7 | What CarPlay actually sends: profile, CABAC, slices per frame, B-frames, AVCC or Annex-B, SPS changes on size switch | Dump the first access units once the session runs. Until then, test with x264 Baseline/Main/High, 1–4 slices, CABAC on and off |
| 8 | VE reset on a new-style IP: stock has `wait ve mem sync idle bit too long, reset anyway after 10 ms` | Use the CCU reset (VE_BGR bit 16) only, as cedrus does. Revisit if a second decode after an error hangs |
| 9 | Clock: 400 MHz from PERI0_2X vs cedrus's 432 or stock's table value | Time a decode. 1080p60 is rated at roughly these clocks, and 720p30 needs well under a third of that |
| 10 | Memory map clash with the carplay.mod guest | Agree the `0x42000000` carve-out in [carplay-host.md](carplay-host.md)'s memory plan before writing `ve.c` |

## References

- Linux cedrus: `drivers/staging/media/sunxi/cedrus/cedrus.c` (D1 variant
  `:620`, compatibles `:657–700`), `cedrus_h264.c`, `cedrus_hw.c`,
  `cedrus_regs.h`.
- D1 video-engine series: https://lists.infradead.org/pipermail/linux-arm-kernel/2023-January/804983.html
  (cover letter); DT patch and Holland's reply on power domains:
  https://lists.infradead.org/pipermail/linux-arm-kernel/2023-February/811673.html
- `drivers/clk/sunxi-ng/ccu-sun20i-d1.c` (PLL_VE `:149`, VE `:333–343`, MBUS
  VE `:393`, reset `:1247`); `arch/riscv/boot/dts/allwinner/sun20i-d1s.dtsi`
  (`SOC_PERIPHERAL_IRQ`), `sunxi-d1s-t113.dtsi` (MBUS `dma-ranges` `:547`).
- `drivers/gpu/drm/sun4i/sun8i_mixer.c` (`sun20i_d1_mixer0_cfg` `:796`),
  `sun8i_vi_layer.c` (formats `:323–336`).
- libvdpau-sunxi `h264.c`: https://github.com/linux-sunxi/libvdpau-sunxi
- linux-sunxi "VE Register guide": https://web.archive.org/web/2024/https://linux-sunxi.org/VE_Register_guide
- Tina D1 DT: https://github.com/rvboards/linux_kernel_for_d1 `arch/riscv/boot/dts/sunxi/sun20iw1p1.dtsi:1247`
- F133 datasheet / product brief: https://www.allwinnertech.com/uploads/pdf/20211016141154e7.pdf
