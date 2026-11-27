# Strategy: goals and the CarPlay question (2026-11-27)

## Two tracks

**Track 1: mod stock, for everyday users.** This covers the browser tool, boot
logo, wallpapers, menu text and translations, and safe `Config.ini` settings.
CarPlay and Android Auto stay untouched because the vendor's firmware still
runs. This is where the reach is. Typo fixes turned out to matter little (most
typos sit behind factory screens). What owners actually ask for is a custom
look, settings, and logo changes without a USB stick.

**Track 2: our own platform.** This is clean-sheet firmware with full freedom,
already booting on the bench unit ([display.md](display.md),
[boot-and-recovery.md](boot-and-recovery.md)). The open question is how to keep
**CarPlay and Android Auto**, the reason people buy these units.

## One firmware, many boards

The vendor ships one app for a family of boards and picks drivers from
`Config.ini` / `sys_config`. It carries six tuner drivers, two sound
processors, and support for a microSD slot, DAB and TPMS that a given board may
not have. A lot of what the code suggests belongs to another variant: the SD
slot, the `ANT` tuner module, and the Si4730 this board's own config selects.
**Rule: confirm a part exists on the board (probe it or photograph it) before
building on what the code says.** Our hardware layer is exactly that ground
truth, board by board.

## Three architectures for keeping CarPlay/AA

| | Approach | CarPlay/AA | Cost |
|---|---|---|---|
| **A** | Keep Melis as the base layer (kernel, drivers, CarPlay/AA modules, video decoder) and replace or extend the vendor's UI app (`init.axf`) with ours. | Runs on its real runtime | Medium: redo the app's CarPlay wiring (start/stop, video, audio, touch) |
| **B** | Our own OS plus a Melis compatibility layer that hosts the user's own `carplay.mod` / `auto.mod` | Works if the layer covers what they use | Large, bounded by how much of Melis they call |
| **C** | Linux plus open-source projection stacks | AA has open implementations; an open CarPlay receiver barely exists | Very large; the F133 has 64 MB of RAM |

**How we decide between A and B:** an exact inventory of what `carplay.mod`,
`auto.mod` and `wireless.mod` need from Melis: syscalls, devices, other
modules, and direct hardware access. A few dozen mostly basic services
(threads, memory, files) make B realistic. Hundreds of calls, or deep driver
dependencies (USB, video decode, display layers), make A the practical route.
Result ([carplay-inventory.md](carplay-inventory.md)): the modules use only 52
basic kernel services and no hardware directly; the heavy parts (USB, H.264
decode, display, audio, WiFi) are services their host provides through
callbacks. **Decision: Option A now** (our app replaces `init.axf` on Melis and
reuses its drivers), **Option B kept open** (the narrow interface lets us swap
the layers underneath one subsystem at a time later).

Either way, the hardware layer we're building (display, touch, MCU, audio, USB)
is needed, so Track 2 work isn't wasted.

**Stepping stone available now:** the boot selector lets stock and ours coexist.
Ours boots by default; stock is one gesture away for CarPlay.

## Legal position

This doesn't change. The repo ships code and knowledge, never vendor firmware.
Any build that includes the CarPlay/AA modules is assembled on the user's
machine from the user's own vendor image, as the browser tool does today. See
[legal-and-scope.md](legal-and-scope.md).
