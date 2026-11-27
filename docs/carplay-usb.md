# Wired CarPlay over USB: the role switch and the gadget stock presents (2026-11-27)

Static analysis of how stock firmware moves an iPhone on the USB-A port (USB0)
from "phone is a device" to "phone is the host", and what USB device the head
unit presents afterwards. It fills in unknown #1 of
[carplay-host.md](carplay-host.md). **Nothing here has been tested on
hardware.**

Sources:
- `init.axf` (VAs `0xe91…`/`0xe92…`; file offset = VA − `0xe9100000` in the
  decompressed `init.axf.bin`);
- the Melis kernel (`kernel.bin`, loaded at `0x40000000`; file offset = VA −
  `0x40000000`).

The public D1s Melis SDK was used **only as a reference** for structure (USB
manager, `udc`, gadget framework). It has no NCM/iAP gadget; that code is
vendor-added and was read from `kernel.bin`. Nothing from the SDK is copied
here.

"Confirmed" means read directly from the disassembly. "Inferred" means a
reading that hasn't been checked on hardware.

## Summary

| Step | What stock does | Confidence |
|---|---|---|
| 1 | Kernel host stack enumerates the phone and sends `SET_CONFIGURATION(1)` (it picks a different configuration only if one has an `FF/2A/FF` interface) | confirmed |
| 2 | **Capability query:** `C0 53 0000 0000 0400`, 4 bytes IN. Byte 0 must be `0x01` | confirmed |
| 3 | **Role switch:** `40 51 0000 0000 0000`, no data stage | confirmed |
| 4 | Immediately: `/dev/udv` ioctl 102 with mode 0. This removes the USB0 host driver (EHCI0/OHCI0), sets gadget mode 0 and loads the device driver (MUSB `udc`) with the NCM + iAP gadget | confirmed (call chain); PHY register steps from the SDK, inferred |
| 5 | Waits up to **2000 ms** for the phone, now the host, to send `SET_CONFIGURATION` to the gadget. On success it starts `carplay.mod`. On timeout it logs `iap connect over time`, goes back to host mode (ioctl 101) and retries | confirmed |
| Gadget | `0525:A4A1`, 1 configuration, 3 interfaces: iAP2 `FF/F0/00` ("iAP Interface", bulk `0x81`/`0x01`) + CDC-NCM (`02/0D/00` comm with interrupt `0x84`, data `0A/00/01` alt 1 with bulk `0x82`/`0x02`), high-speed, 512-byte bulk | confirmed (static bytes) |

No charging-current request, no `0x52` mode request and no string reads are
sent on the CarPlay path. Those belong to the separate "mirror" path (below).

## 1. Who does what

`init.axf` drives the whole USB-host side through `/dev/udv` ioctls. The fd is
at `0xe92fbfec`, and the wrappers sit at `0xe9212cd0`–`0xe9213140`. The
kernel's `udv` driver (`udv_control`, the switch at `0x40099632`) executes
them:

| ioctl | `init.axf` wrapper | Kernel | Meaning |
|---|---|---|---|
| 0 | `0xe9212d5c` | `0x40099e62`… | copy the attached device's 18-byte device descriptor (+ state) to user (`init.axf` keeps it at `0xe9315470`) |
| 5 | `0xe9212cd0` | `0x40099b80` | **control transfer.** Arg → `{u8 bmRequestType; u8 bRequest; u16 wValue; u16 wIndex; u16 wLength; u32 timeout_ms @+8; void *data @+16}`. Timeout 0 becomes `0x7fffffff`. Calls the core `usb_control_msg` at `0x4008024e`; on failure it prints `usb_control_msg fail %d` |
| 6 | `0xe9212d1a` | `0x40099c72` | bulk transfer |
| 12 / 13 | `0xe9212e4c` / `0xe9212e78` | `0x40099b1c` / `0x40099822` | port number the phone is on / port number of the OTG port |
| 100 | `0xe92130ac` | `0x40099120` | current USB0 role: 0 none, 1 host, 2 device (byte `0x4023dfc4`) |
| 101 | `0xe92130e0` | `0x4009922e` | back to host: rmmod device driver, insmod host |
| 102 | `0xe9213112` | `0x40099ebc` | **switch to device:** store gadget mode (`0x4009278c`), then `0x40099280` |

## 2. The CarPlay switch sequence (`init.axf`, thread `libcarplay`, `0xe921619e`)

The loop runs every tick (`esKRNL_TimeDly(1)`, 10 ms). Confirmed from
`0xe9216216`–`0xe92162ca`:

1. A device is attached (`0xe921300e`), and it sits on the OTG port: ioctl 12
   == ioctl 13. Otherwise the thread posts UI event `(1, 16)` and does nothing
   more (`0xe92162d2`).
2. `0xe9315594` is set. That flag is set by the host's CarPlay start wrappers
   (`0xe92166c4`, `0xe92166dc`) and cleared by stop (`0xe92166f4`). The thread
   hasn't switched yet.
3. **Capability query** (`0xe9216244`):
   `bmRequestType 0xC0, bRequest 0x53, wValue 0, wIndex 0, wLength 4`, data to
   the stack, timeout 0 (wait forever).
   SETUP bytes: **`C0 53 00 00 00 00 04 00`**.
4. If the transfer succeeds and **byte 0 == `0x01`** (`0xe921625e`), the thread
   posts UI event `(1, 2)` and sends:
5. **Role switch** (`0xe9216270`):
   `bmRequestType 0x40, bRequest 0x51, wValue 0, wIndex 0, wLength 0`, no data,
   timeout 0. SETUP bytes: **`40 51 00 00 00 00 00 00`**. Its return value is
   ignored.
6. Right away, **ioctl 102 with arg 0** (gadget mode 0 = NCM + iAP,
   `0xe9216288`). The thread records the time (`0xe9216290`).
7. Each tick after that: if ioctl 100 says role 2 (device) and the `/dev/udv`
   callback's link status (`0xe93155a0`, set from "kind 2" messages at
   `0xe9216574`) equals **2**, it starts the CarPlay session (`0xe92162f8`:
   video size, screen size, then `0xe9218ad6`). If 2000 ms pass first:
   `iap connect over time`, ioctl 101, and back to step 1 (`0xe9216324`). If
   the role drops back to non-device (unplug), the state resets.

Before step 3, the kernel's own enumeration has already configured the phone.
`usb_new_device` (`0x40082766`…) reads the strings and then chooses a
configuration. Stock adds a check for **`05AC:12A8`** (the constant
`0x12a805ac` at `0x40082798`): it picks the first configuration with an
interface `FF/2A/FF` and otherwise uses configuration 1
(`config[0].bConfigurationValue`, `0x400827a4`). A normal iPhone has no
`FF/2A/FF` interface (our capture showed 4 configurations without one), so the
phone is in **configuration 1** when `0x53`/`0x51` go out. Whether `0x51`
requires a configured device isn't known; matching stock is the safe choice.

### Public cross-check (not from stock)

- A 2018 MediaTek Linux patch (not merged, as far as I can tell) defines
  `0x51` as the "Apple Device to Host Mode Switch" vendor request. It says
  VBUS must stay powered through the switch and cites "Chapter 46. USB Role
  Switch" of Apple's MFi Accessory Interface Specification (not public).
  [lore.kernel.org](https://lore.kernel.org/lkml/1521015535.3717.26.camel@mhfsdcap03/t/)
- A CSDN blog post describes the same `C0 53 … 0004` capability query (1 =
  supported) and a `0x51` switch, but gives **wValue = 1**.
  [blog.csdn.net](https://blog.csdn.net/zoosenpin/article/details/87439254)
  **Stock sends wValue = 0** (`0xe9216278`: `a2 = 0`). Copy stock first. If the
  phone stalls, try wValue 1.
- Apple's forum points people to MFi licensing for the details.
  [developer.apple.com](https://developer.apple.com/forums/thread/768993)

## 3. Host → device on the head-unit side

`0x40099280` (ioctl 102), confirmed:

1. If the role is 1 (host): clear the role and call `0x4008f39a(0)`, which tears
   down host controller 0. This is the kernel counterpart of the SDK's
   `hal_usb_hcd_deinit(0)`. On stock, **USB0 in host mode runs on EHCI0/OHCI0,
   not the MUSB** (`[ehci-usb%d] rmmod host driver!` strings).
2. Set the role to 2, print `[usb0] insmod device driver!`, and call
   `sunxi_gadget_init(mode)` (`0x40092796`). Mode 0 → `ncm_main`
   (`0x40091920`) → `ncm_desc_init` + `usb_gadget_function_enable("ncm")`.
   Mode 1 → mirror gadget, mode 2 → Android gadget.

There's no delay between the `0x51` request and this teardown: it's the next
call.

**PHY/MUSB register steps (inferred from the SDK's `udc_init`; not traced in
`kernel.bin`):**
- enable D+/D− and ID pull-ups;
- ISCR force ID **high** (device) and force VBUS valid **high**;
- route the PHY to the OTG controller (`usbc_phy_otg_sel`);
- PHY init, then the MUSB soft-connect.

Our `musb.c` device mode already does the equivalent. Because our host mode
runs on the MUSB rather than EHCI, our switch is: end the session
(`DEVCTL = 0`), force ID high, keep VBUS-valid forced, and soft-connect the
gadget.

**VBUS:** nothing in the ioctl 102 path touches a GPIO, so PD18 (USB-A 5 V)
appears to **stay on** (inferred). The MediaTek patch says VBUS must stay
alive.

## 4. The gadget stock presents (mode 0, "NCM Gadget")

`ncm_desc_init` (`0x40091292`) concatenates descriptors from a NULL-terminated
pointer list at `0x40194738` (file `0x194738`). It sets `wTotalLength` at run
time (`0x400913c0`), registers the device descriptor at `0x40210a50`
(`0x4009143e`), and registers 8 string descriptors from the pointer table at
`0x40194848`. Unless `fd->strings[i]` overrides an entry, these are the static
ones. I found no code that overrides them, but didn't search exhaustively
(inferred).

### Device descriptor (VA `0x40210a50`, file `0x210a50`)

```
12 01 00 02 00 00 00 40 25 05 a1 a4 16 03 01 02 03 01
```

- USB 2.0, class/subclass/protocol `00/00/00` (per interface), EP0 64 bytes;
- **VID `0x0525`, PID `0xA4A1`** (the NetChip/Linux-USB "Ethernet Gadget" IDs,
  not Apple's or the vendor's own), bcdDevice `0x0316`;
- iManufacturer 1, iProduct 2, iSerial 3, 1 configuration.

CarPlay evidently doesn't care about the VID:PID.

### Configuration descriptor, assembled (117 = `0x75` bytes)

```
09 02 75 00 03 01 00 c0 00                      config: 3 interfaces, value 1, self-powered, 0 mA
09 04 00 00 02 ff f0 00 04                      if 0: FF/F0/00, 2 EPs, iInterface 4 "iAP Interface"
07 05 81 02 00 02 00                            EP 0x81 bulk IN 512
07 05 01 02 00 02 00                            EP 0x01 bulk OUT 512
08 0b 01 02 02 0d 00 08                         IAD: first if 1, 2 ifs, 02/0D/00, iFunction 8 (*)
09 04 01 00 01 02 0d 00 05                      if 1: CDC comm 02/0D/00 (NCM), 1 EP, iInterface 5
05 24 00 10 01                                  CDC header, bcdCDC 1.10
05 24 06 01 02                                  union: master 1, slave 2
0d 24 0f 07 00 00 00 00 50 2f 00 00 00          ethernet: iMACAddress 7, stats 0, wMaxSegmentSize 0x2F50 (**), 0 MC filters, 0 power filters
06 24 1a 00 01 11                               NCM functional: bcdNcmVersion 1.00, bmNetworkCapabilities 0x11
07 05 84 03 40 00 01                            EP 0x84 interrupt IN 64, bInterval 1
09 04 02 00 00 0a 00 01 06                      if 2 alt 0: CDC data 0A/00/01, 0 EPs, iInterface 6
09 04 02 01 02 0a 00 01 06                      if 2 alt 1: 2 EPs
07 05 82 02 00 02 00                            EP 0x82 bulk IN 512
07 05 02 02 00 02 00                            EP 0x02 bulk OUT 512
```

Where each piece lives (VA = file offset + `0x40000000`). The list order is the
order above:

| Piece | VA | Piece | VA |
|---|---|---|---|
| config | `0x40210a08` | ethernet func | `0x40210968` |
| if 0 | `0x402109e8` | NCM func | `0x40210a48` |
| EP 81 | `0x40210978` | EP 84 | `0x402109c8` |
| EP 01 | `0x40210998` | if 2 alt 0 | `0x40210a38` |
| IAD | `0x40210a70` | if 2 alt 1 | `0x40210a28` |
| if 1 | `0x40210a18` | EP 82 | `0x402109b8` |
| CDC header | `0x40210a68` | EP 02 | `0x402109d8` |
| union | `0x40210a78` | | |

Notes:
- (*) The IAD's iFunction 8 points past the 8-entry string table (indices
  0–7). It's a stock quirk; the phone evidently tolerates it.
- (**) `wMaxSegmentSize` reads `0x2F50` (12112), not the usual 1514. That's
  what's stored; whether it's intentional isn't known.
- Only high-speed descriptors exist (512-byte bulk); no full-speed set was
  found.
- The iAP interface is first (interface 0); NCM is interfaces 1–2.

### Strings (table `0x40194848`)

| Index | VA | Value |
|---|---|---|
| 0 | `0x401945b0` | LANGID `0x0409` |
| 1 | `0x40194500` | `arkmicro` |
| 2 | `0x401945c8` | `NCM Gadget` |
| 3 | `0x40194610` | `0123456789ABCDEF` (placeholder serial, same in every unit) |
| 4 | `0x40194440` | **`iAP Interface`** |
| 5 | `0x40194548` | `CDC Network Control Model (NCM)` |
| 6 | `0x40194588` | `CDC Network Data` |
| 7 | `0x401944a0` | iMACAddress: a 12-hex-digit firmware constant |

### NCM parameters and gadget behaviour (confirmed)

- **`GET_NTB_PARAMETERS`** reply (`0x40210bd8`):
  ```
  1c 00 03 00 00 08 00 00 04 00 00 00 04 00 00 00 00 40 00 00 04 00 00 00 04 00 00 00
  ```
  This means NTB-16 and NTB-32, `dwNtbInMaxSize` 2048, IN divisor/alignment
  4, `dwNtbOutMaxSize` 16384, OUT divisor/alignment 4, `wNtbOutMaxDatagrams`
  0. The signatures are `NCMH`/`NCM0` (`0x40194888`), and a CRC-32 table is
  built at init.
- **Standard-request hook** (`0x400910d6`):
  - `SET_CONFIGURATION` enables all five endpoints and reports link status
    **2** to `init.axf`. That's the status the CarPlay thread waits for.
  - `SET_INTERFACE` on interface 2 marks NCM up (`0x4023dac8`/`+0x59`) and
    reports status **3**.
  - Init reports 0.
- **Threads:**
  - `receive_iap_thread` reads EP index 1 (the iAP bulk OUT) in 4 KB chunks
    and hands them to the `/dev/udv` callback as kind 0;
  - `receive_ncm_thread` unwraps OUT NTBs (16 KB buffer) into kind 1 frames;
  - `ncm_notify_thread` sends the interrupt notifications.

## 5. The other USB paths (for completeness)

**"Mirror" (`libmirror` thread, `0xe921260e`; not CarPlay).** On the OTG port:
- If the phone shows ≤ 4 configurations: `40 52 0000 0002 0000` ("get
  config"), wait 1 s, and re-check. This is the vendor request public
  QuickTime-capture tools use to expose the hidden `FF/2A/FF` configuration;
  the kernel then selects it (see §2).
- With 5 configurations: log `switch otg bcd %x`, then `40 40 0960 076C 0000`
  and `40 40 0960 0960 0000`. These look like Apple's charging-current vendor
  request (2400/1900 mA); the meaning is inferred.
- Then `40 51 0000 0000 0000` and ioctl 102 with mode **1** (mirror gadget).

**Android Auto (AOA, `0xe921557a`):**
1. `C0 33 0000 0000 0002` (protocol);
2. `40 34 0000 idx len` six times: `Android`, `Android Auto`, `Android Auto`,
   `1.0`, `http://www.android.com/auto`, `41524B4D4943524F`;
3. `40 35 0001 0000 0000` (start; note **wValue 1**).
4. The thread waits for the phone to re-appear as `18D1:2D00`/`2D01` with a
   3000 ms timeout, then talks bulk with the head unit **as host**. ioctl 102
   mode 2 (Android gadget) exists, but the AOA thread doesn't call it.

## What's still unknown, and the cheapest way to settle it

1. **Does `0x51` with wValue 0 work on current iOS?** Cheapest: add the two
   requests to `iphonetest.c` (after `SET_CONFIGURATION 1`), log the 4 bytes
   from `0x53` and the `0x51` status, and watch the phone disconnect. Then
   bring up `musb.c` in device mode with the descriptors above and see whether
   the phone sends `SET_CONFIGURATION`.
2. **Exact MUSB/PHY steps stock takes** (ISCR values, any delay before
   soft-connect). Cheapest: trace `sunxi_gadget_init` → `udc` init in
   `kernel.bin` (`0x40092538` onward), or capture stock's debug UART during a
   plug-in (it prints `[usb0] insmod device driver!`, `ncm_main`,
   `sunxi_gadget_init 0`).
3. **Whether stock toggles VBUS (PD18) during the switch.** Statically no GPIO
   is touched in this path. A meter on the USB-A 5 V pin during a stock
   CarPlay plug-in would confirm.
4. **What the iPhone sends to the gadget** (class requests on the iAP
   interface, NCM `SET_NTB_INPUT_SIZE`, etc.) is not covered here. The function pointer
   at `0x40090d14` (stored next to the standard-request hook; probably the
   class-request handler) is the next thing to read.
