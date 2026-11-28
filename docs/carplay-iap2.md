# Wired CarPlay over iAP2: what stock sends in IdentificationInformation (2026-11-27)

Static analysis of how the stock `carplay.mod` identifies itself to an iPhone
over iAP2, and what happens between `IdentificationAccepted` and the start of
the CarPlay session on the NCM link. It picks up where
[carplay-usb.md](carplay-usb.md) and the devlog's iAP2 entries stop: our
firmware passes authentication, and the phone sends `StartIdentification`
(`0x1D00`). **Nothing here has been tested on hardware.**

Sources:
- `carplay.mod` (VAs `0xe88…`; file offset in the decompressed
  `carplay.mod.bin` = VA − `0xe8800000`). Disassembled with T-Head extensions
  enabled; plain `-M max` decodes some T-Head loads as CORE-V `cv.*`.
- `init.axf` (VAs `0xe91…`/`0xe92…`), for the values the host passes in.

Public references, used only to name parameters and to cross-check (nothing
copied):
- [wiomoc/iap2](https://github.com/wiomoc/iap2) (Python, MIT), especially
  `control_session_message/identification.py`, `car_play.py` and
  `carplay_bonjour.py`;
- [amineross/showcase `carplay_bt.m`](https://github.com/amineross/showcase/blob/main/source/carplay_bt.m)
  (wireless CarPlay accessory), whose rejection decoder names parameters 0–24;
- [rGnng/iap2mediactrl](https://github.com/rGnng/iap2mediactrl) (message IDs
  `0x5000`–`0x5003`, `0xAE00`–`0xAE03`, `0x4E09`–`0x4E0C`);
- a [CSDN post](https://blog.csdn.net/zoosenpin/article/details/87439254)
  (message ID list);
- an [Apple developer forum thread](https://developer.apple.com/forums/thread/811397)
  that mentions a `ProductPlanUID` parameter in `IdentificationInformation`.

"Confirmed" means read directly from the disassembly. "Public" means the name
or meaning comes from the references above, not from stock. "Guess" means a
guess.

## Summary

| Question | Answer | Confidence |
|---|---|---|
| How stock builds `0x1D01` | Fills a static parameter table at `0xe880e15a`, serialises it with a generic TLV writer (`0xe880da30`) and sends it (`0xe880dd4a`) | confirmed |
| Transport component | **`USBHostTransportComponent` (16)**: id 0, name `"host"`, supports iAP2, **CarPlay interface number 1** (the NCM control interface), and flag 4 (supports CarPlay) | layout confirmed; names public; flag 4's name a guess |
| Other components | `VehicleInformationComponent` (20). No HID, Bluetooth, location, EA protocol or app-match team ID | confirmed |
| Parameter 34 | String `"0123456789ABCDEF"` (a placeholder). Probably `ProductPlanUID` | value confirmed; name a guess |
| After `0x1D02` | Stock sends `StartPowerUpdates` (`0xAE00`) and `PowerSourceUpdate` (`0xAE03`, 1000 mA, charge). Nothing CarPlay-specific goes over iAP2 | confirmed |
| What starts CarPlay | iOS: it brings up NCM, and the head unit's mDNS finds the phone's `_carplay-ctrl._tcp`. Stock sends `GET /ctrl-int/1/connect`; the phone then opens the AirPlay session to the head unit's `_airplay._tcp` server | code paths confirmed; ordering partly inferred |

## 1. How stock builds the message

On `0x1D00` the dispatcher (`0xe880e5a2`) calls `0xe880e15a`, which:

1. zeroes four tables of 96-byte entries:
   - the root table (`0xe88d4cf8`, 50 entries);
   - three 10-entry sub-tables (`0xe88d6378`, `0xe88d6738`, `0xe88d5fb8`);
2. fills them with constant stores;
3. tail-calls `0xe880dd4a`. That serialises the root table onto a 2 KB stack
   buffer and sends it as message `0x1D01` through the framer at `0xe880d948`
   (`40 40 <len16> <id16>`, then the parameters).

**Entry layout** (confirmed):
- `+0` u16 parameter ID;
- `+2` u8 type (0 ends the table);
- `+8` value: inline u8/u16, or a pointer to a string or sub-table;
- for arrays: `+8` count, with u16 values from `+10` or string pointers from
  `+16`.

**Serialiser `0xe880da30`** (types via the jump table at `0xe889f928`; all
lengths and IDs big-endian; each length includes the 4-byte header):

| Type | Wire form | Length |
|---|---|---|
| 1 | none (presence flag) | 4 |
| 2 | u8 | 5 |
| 3 | u16 | 6 |
| 4 | UTF-8 string **with its NUL** | strlen + 5 |
| 5 | u16 array | 4 + 2·count |
| 6 | string list: every string with its NUL, **concatenated into one parameter** | 4 + Σ(strlen+1) |
| 7 | group: the sub-table serialised recursively | 4 + contents |

Parameters go out in table order, which is ascending ID order.

## 2. The parameters stock sends

Root table, in wire order:

| ID | Name (public) | Type | Stock value | Source |
|---|---|---|---|---|
| 0 | Name | string | the **car name** (see below) | host, MIoctrl 18 |
| 1 | ModelIdentifier | string | `"Linux"` (`0xe889fa78`) | constant |
| 2 | Manufacturer | string | the same car-name buffer as Name | host, MIoctrl 18 |
| 3 | SerialNumber | string | `"0123456789ABCDEF"` (`0xe889fa80`). A fixed placeholder, the same in every unit, not per-unit | constant |
| 4 | FirmwareVersion | string | `"sw1.1.0"` (`0xe889fa98`) | constant |
| 5 | HardwareVersion | string | `"hw1.1.0"` (`0xe889faa0`) | constant |
| 6 | MessagesSentByAccessory | u16[8] | `EA02 AE00 AE02 AE03 AA06 5000 5002 5003` (data at `0xe889fa60`) | constant |
| 7 | MessagesReceivedFromDevice | u16[3] | `AE01 5001 4E0B` (built inline at `0xe880e30e`) | constant |
| 8 | PowerProvidingCapability | u8 | `2` (Advanced) | constant |
| 9 | MaximumCurrentDrawnFromDevice | u16 | `0` | constant |
| 12 | CurrentLanguage | string | `"en"` (`0xe88d04f0`, the tail of a merged rodata string) | constant |
| 13 | SupportedLanguage | string list | `"en\0zh\0"` in **one** parameter (`"zh"` at `0xe889faa8`) | constant |
| 16 | USBHostTransportComponent | group | see below | constant |
| 20 | VehicleInformationComponent | group | see below | name from host |
| 34 | ProductPlanUID (guess) | string | `"0123456789ABCDEF"`, the same placeholder as the serial | constant |

What stock does **not** send: 10 SupportedExternalAccessoryProtocol,
11 AppMatchTeamID, 14 Serial/UART transport, 15 USBDeviceTransportComponent,
17 BluetoothTransportComponent, 18 iAP2HIDComponent, 21 VehicleStatus,
22 LocationInformation, 23 USBHostHIDComponent and 24
WirelessCarPlayTransportComponent. (Wireless CarPlay lives in the separate
`wireless.mod`.)

The message IDs, named from the public lists:
- `EA02` RequestAppLaunch;
- `AE00`/`AE01`/`AE02`/`AE03` StartPowerUpdates / PowerUpdate /
  StopPowerUpdates / PowerSourceUpdate;
- `5000`/`5001`/`5002`/`5003` StartNowPlayingUpdates / NowPlayingUpdate /
  StopNowPlayingUpdates / SetNowPlayingInformation;
- `4E0B` DeviceTimeUpdate.

`AA06` is the accessory sending the MFi chip's 31-byte serial number (register
`0x4E`; sender `0xe880dc90`). Stock sends it only when `AA00` carries a
parameter 0 (`0xe880dfac`); our phone's `AA00` has none, so we've never needed
it.

### `USBHostTransportComponent` (16), sub-table `0xe88d6378`

| ID | Name (wiomoc) | Type | Value |
|---|---|---|---|
| 0 | TransportComponentIdentifier | u16 | `0` |
| 1 | TransportComponentName | string | `"host"` (`0xe889fa70`) |
| 2 | TransportSupportsiAP2Connection | none | present |
| 3 | **USBHostTransportCarPlayInterfaceNumber** | u8 | **`1`** |
| 4 | TransportSupportsCarPlay (guess; wiomoc uses ID 4 with this meaning in the wireless CarPlay component) | none | present |

This group is what ties CarPlay to the USB link. In Apple's naming, "USB
host" means the *Apple device* is the USB host. That matches wired CarPlay
after the role switch; `USBDeviceTransportComponent` (15, with audio sample
rates) is the opposite role and isn't used. **Interface 1 is the CDC-NCM
control interface** of the gadget in [carplay-usb.md](carplay-usb.md) §4 (iAP
is interface 0, NCM data is interface 2). If our descriptors change, this
number has to follow the NCM control interface.

### `VehicleInformationComponent` (20), sub-table `0xe88d6738`

| ID | Name | Type | Value |
|---|---|---|---|
| 0 | ComponentIdentifier | u16 | `0` |
| 1 | Name | string | the car name |
| 2 | EngineType (wiomoc: 0 gas, 1 diesel, 2 electric, 3 CNG) | u8 | `0` |
| 6 | DisplayName (guess) | string | the car name |

Both components use identifier 0. iOS evidently accepts that.

### A third table that is built but never sent

`0xe88d5fb8` gets: id 0 = u16 0, id 1 = the car name, and flags 17, 18 and
19. Nothing links it into the root table (no other references). It's ignored
here.

### Where the car name comes from

- The module keeps it in a 52-byte `.data` buffer at `0xe88d4168`. Its default
  is `"car"`.
- **MIoctrl 18** (`0xe887ba64` → `0xe880ea88`, a `strcpy`) overwrites it.
- `init.axf` sends cmd 18 at start-up (wrapper `0xe9218c9e`). The argument
  comes from the buffer at `0xe9315198`, filled at `0xe9198dd4` from settings
  offset `+0x1094`.
- That setting is the Config.ini key **`carplayCarName`** (key `0xe9279d00`,
  loaded at `0xe91a9e16`, 50 bytes max). Its default is **`"LTT"`**
  (`0xe9279cf8`).
- The unit's Config.ini has no `carplayCarName` line, so stock identifies as
  Name = Manufacturer = `"LTT"`.

The name is presumably what iOS shows for the car, but that's a guess. We can
choose our own name; keep it under 50 bytes.

## 3. Byte layout to reproduce

Reconstructed from the table above with the name `"LTT"`. The script is
`build.py` in the scratch directory; the bytes below are its output. One
parameter per line, all big-endian, lengths include the header:

```
00 08 00 00  4c 54 54 00                              0  Name "LTT"
00 0a 00 01  4c 69 6e 75 78 00                        1  ModelIdentifier "Linux"
00 08 00 02  4c 54 54 00                              2  Manufacturer "LTT"
00 15 00 03  30 31 … 45 46 00                         3  SerialNumber "0123456789ABCDEF"
00 0c 00 04  73 77 31 2e 31 2e 30 00                  4  FirmwareVersion "sw1.1.0"
00 0c 00 05  68 77 31 2e 31 2e 30 00                  5  HardwareVersion "hw1.1.0"
00 14 00 06  ea 02 ae 00 ae 02 ae 03 aa 06 50 00 50 02 50 03
00 0a 00 07  ae 01 50 01 4e 0b
00 05 00 08  02                                       8  PowerProvidingCapability = Advanced
00 06 00 09  00 00                                    9  MaximumCurrentDrawnFromDevice = 0
00 07 00 0c  65 6e 00                                12  CurrentLanguage "en"
00 0a 00 0d  65 6e 00 7a 68 00                       13  SupportedLanguage "en","zh"
00 20 00 10                                          16  USBHostTransportComponent
      00 06 00 00  00 00                                  0  id 0
      00 09 00 01  68 6f 73 74 00                         1  "host"
      00 04 00 02                                         2  supports iAP2
      00 05 00 03  01                                     3  CarPlay interface 1
      00 04 00 04                                         4  supports CarPlay
00 1f 00 14                                          20  VehicleInformationComponent
      00 06 00 00  00 00                                  0  id 0
      00 08 00 01  4c 54 54 00                            1  "LTT"
      00 05 00 02  00                                     2  engine type 0
      00 08 00 06  4c 54 54 00                            6  "LTT"
00 15 00 22  30 31 … 45 46 00                        34  "0123456789ABCDEF"
```

With the control-message header `40 40 00 db 1d 01`, the whole message is
219 bytes (213 bytes of parameters). It goes on the control session, as our
`AA01`/`AA03` replies already do.

Two stock quirks we could choose not to copy:
- **SupportedLanguage** is one parameter holding `en\0zh\0`. The public
  reading of the spec is one parameter per language (the showcase project
  sends one concatenated list with an extra NUL). Copy stock first. If iOS
  rejects parameter 13, send `00 07 00 0d 65 6e 00` twice (`en`, `zh`) or
  `en` alone.
- The message lists declare `EA02`, `5000`–`5003` and `AA06`, which we don't
  implement yet. Declaring messages we never send is harmless. Declaring
  messages iOS might send that we don't handle (`AE01`, `5001`, `4E0B`) only
  means we must ACK and ignore them at the link layer.

## 4. The flow before and after identification (stock)

Dispatcher `0xe880e5a2`, keyed on the message ID at payload offset 4:

| Phone sends | Stock does | Handler |
|---|---|---|
| `AA00` RequestAuthenticationCertificate | with parameter 0: `AA06` (chip serial). Otherwise: `AA01` with the certificate | `0xe880df8c` |
| `AA02` challenge | `AA03` signature | `0xe880e03c` |
| `AA04` AuthenticationFailed | logs | – |
| `AA05` AuthenticationSucceeded | logs, then **sends `5000` StartNowPlayingUpdates right away** (before identification). Media-item attribute group 0 = {1, 4, 6, 12}, playback attribute group 1 = {0, 1, 7, 16}, all presence flags | `0xe880de44` |
| `1D00` StartIdentification | builds and sends `1D01` (sections 1–3) | `0xe880e15a` |
| `1D02` IdentificationAccepted | sends **`AE00` StartPowerUpdates** (presence flags 0 and 1), then **`AE03` PowerSourceUpdate**: param 0 u16 = **1000** (available current, mA), param 1 u8 = **1** (battery should charge) | `0xe880dd82`, `0xe880dddc` |
| `1D03` IdentificationRejected | logs only, no retry | – |
| `4E0B` DeviceTimeUpdate | parses the date/time, logs `time %d-%d-%d…`, and passes it to the host (event 16, the RTC) | `0xe880e4f6` |
| `5001` NowPlayingUpdate | parses | `0xe880e3aa` |
| `FFFA`, anything else | ignored (`unknow messageID`) | – |

The `AE03` parameter names are public (wiomoc/iap2mediactrl naming); the
values are confirmed.

There is **no CarPlay-specific iAP2 message** on the wired path. Nothing like
`4300`/`4301`/`4E0D`/`4E0E` is declared or handled. Once identification is
accepted with a `USBHostTransportComponent` that names a CarPlay interface,
iOS takes over.

### From `IdentificationAccepted` to a CarPlay session

Stock's side of each step is confirmed in code. The phone's side and the exact
ordering are inferred from stock's code paths and the public wiomoc flow.

1. **iOS selects alt 1 on interface 2** (NCM data). Stock's gadget then
   reports link status 3 (`SET_INTERFACE`; [carplay-usb.md](carplay-usb.md)
   §4) and starts passing Ethernet frames to the module (MIoctrl 13 kind 1).
   Our devlog saw the phone keep NCM down until now, which is consistent.
2. **The module's lwIP interface** (`lib_net_init` `0xe88837bc` →
   `0xe88835f8`):
   - static IPv4 **192.168.31.160/24**, gateway 192.168.31.1;
   - MTU 1500;
   - a **constant firmware MAC** (not per-unit, and different from the
     gadget's iMACAddress string; value not copied here);
   - an IPv6 link-local address from that MAC (`0xe888eaf6`, `fe80::` +
     EUI-64).

   The phone most likely talks to it over IPv6 link-local (guess).
3. **mDNS on the link** (Apple mDNSResponder, `mDNSPosix.c`):
   - the head unit registers its AirPlay receiver as **`_airplay._tcp`**
     (`AirPlayReceiverServer`, `_UpdateBonjourAirPlay`; TXT keys include
     `deviceid`, `features` and `srcvers`);
   - `CarPlayControlClient` browses for the phone's **`_carplay-ctrl._tcp`**
     (`0xe8872032`).
4. **When the phone's `_carplay-ctrl` service appears**, the event callback
   (`0xe8872d9c`, log `Adding CarPlayController`) calls
   `CarPlayControlClientConnect` up to 5 times (`0xe8872e7e`). That sends
   **`GET /ctrl-int/1/connect`** to the phone with `User-Agent:
   AirPlay/320.14` and an `AirPlay-Receiver-Device-ID` header (`0xe887224e`…).
   wiomoc's `carplay_bonjour.py` does the same with `AirPlay/280.33.8`.
5. **The phone then connects to the head unit's AirPlay server** for the
   CarPlay session: RTSP/HTTP, pairing, MFi-SAP (`MFiSAP.c`, the MFi chip
   again), then SETUP of the screen and audio streams. From here on it's the
   module's normal session path (`AirPlay session started: …`), with video
   events to the host as in [carplay-host.md](carplay-host.md).

## 5. mDNS and the network side

Static analysis again, added after the first NCM bring-up on the bench. On
the bench, the phone:
- queries `_airplay._tcp`, `_raop._tcp` and other services;
- announces only `_remotepairing._tcp` and `_apple-mobdev2._tcp`;
- has IPv4 0.0.0.0 and sends DHCP DISCOVERs that nobody answers.

This section is what stock would answer with.

### 5.1 The `_airplay._tcp` registration

Stock makes **exactly one** registration: the only `DNSServiceRegister` call
in the module (`0xe881ac2c`, called from `0xe88654ae` in
`_UpdateBonjourAirPlay`). Its arguments:

| Argument | Value | Evidence |
|---|---|---|
| Instance name | **`"CarPlay"`**: a constant (`0xe88c8758`), copied by `0xe8864f26`. It is not the car name | confirmed |
| Type / domain | `_airplay._tcp.` (`0xe88bdaf0`), default domain | confirmed |
| Interface | index 0 (all interfaces) unless the server's `interfaceName` is set. Nothing sets it, so 0 | confirmed / inferred |
| Flags | `0x800` | confirmed (raw value) |
| Port | the AirPlay HTTP/RTSP server's actual listen port (`server+112 → +40`). That server asks for **7000**, falling back to a dynamic port (see 5.5) | confirmed |

TXT record, built in this order by `0xe886507c`:

| Key | Value | Source |
|---|---|---|
| `deviceid` | `"00:11:22:AA:BB:CC"` | the vendor's copy-property delegate (`0xe88731f8`) returns this constant for `deviceID` (`0xe889e618`). The server parses it into its 6-byte device ID (`0xe8866668`→`0xe884ba26`). It's a **placeholder, the same in every unit**. The interface MAC is used only if parsing fails |
| `features` | **`0x4040280,0x21`** | `0xe8864f4e`: the platform's `features` property, ORed with the constant `0x42080805 << 7` = `0x21_04040280`. The vendor delegate doesn't handle `features`, so the platform part is 0. Printed as `0x%X,0x%X` (low word, high word). For comparison, wiomoc advertises `0x44540380,0x21` |
| `fv` | omitted | only added if `firmwareRevision` is found in `airplay.conf` or the platform. Neither has it: the delegate doesn't handle it, and no `airplay.conf` was found in the NOR dump (not checked exhaustively) |
| `flags` | **`0x4`** | `statusFlags`: the server's own copy-property returns the number 4 for it (`0xe88639e0`–`0xe88639ea`) |
| `model` | **`AirPlayGeneric1,1`** | the default (`0xe88c8d58`), because neither `airplay.conf` nor the delegate supplies `model` |
| `protovers` | omitted | added only if the protocol version isn't `"1.0"`. It is `"1.0"` (`0xe8865246` compares the constant with itself) |
| `pi` | the pairing identity's identifier (a UUID) | `0xe8864fd4`, from the keychain entry "AirPlay Pairing Identity" in `e:\CarPlay\default.keychain`. **Per-unit**, generated and stored on the unit; value not copied here |
| `srcvers` | **`320.14`** | `0xe88cbba0` (the tail of `AirPlay/320.14`) |

**No other keys.** Stock has no `pk`, `pw`, `acl`, `btaddr`, `vv`, `gid` or
`protovers`.

### 5.2 Other registrations

None. `_raop._tcp.`, `_airport._tcp.`, `_mfi-config._tcp.` and `_hap._tcp.`
appear only in `BonjourBrowser.c`'s type table (`0xe8845628`…`0xe8845afc`),
used when browsing. They are never registered. Stock browses for exactly one
service type: `_carplay-ctrl._tcp` (CarPlayControlClient).

### 5.3 Network set-up

- **IPv4:** static **192.168.31.160/24**, gateway .1 (`0xe88835f8`).
  - lwIP's DHCP client and AutoIP are compiled in, but nothing reachable calls
    `dhcp_start`/`autoip_start`. The DHCP functions are reached only from
    lwIP's timer and receive callbacks (inferred).
  - **There is no DHCP server** anywhere (no lwIP or vendor DHCP-server code
    or strings).
  - So stock never answers the phone's DISCOVERs either, and the phone stays
    without a routable IPv4 address on the link. The working path must be
    **IPv6 link-local** (inferred, but consistent with the bench).
- **The MAC on NCM frames is lwIP's netif hardware address.** The netif init
  `0xe8883694` stores it as a code constant: 6 bytes written at `netif+260`,
  the same in every unit.
  - It is **not** the gadget's iMACAddress string (string 7), and not the
    AirPlay `deviceid`.
  - The netif name is `"us"`, MTU 1500, and the flags byte is `0x52`.
- **The IPv6 link-local address** is `fe80::` plus the EUI-64 of that netif
  MAC: first byte XOR `0x02`, `ff:fe` inserted (`0xe888eaf6`, called with
  "from MAC" = 1, right after `netif_add`).
- **mDNS (mDNSResponder/mDNSPosix on lwIP, port 5353):**
  - `mDNSPlatformInit` (`0xe88304e8`…) opens unicast sockets for both AF_INET
    and AF_INET6.
  - Per interface, it also sets up the IPv6 multicast socket
    (`IPV6_JOIN_GROUP`) when the interface reports an AF_INET6 address
    (`0xe883044c`). So it can answer over **IPv4 and IPv6**. Whether lwIP's
    interface list reports the fe80 address isn't traced.
  - **Hostname:** the host label is set to the literal **`"Computer"`**
    (`0xe8830568`–`0xe8830588`; no `gethostname` call). So the records are
    `Computer.local`, with an A record for 192.168.31.160 and an AAAA record
    for the fe80 address. On a name conflict, mDNS renames it to
    `Computer-2` and so on. Confirmed that the label is set; not traced
    whether later code renames it.

### 5.4 Order of events

1. **MIoctrl 1** (module init, `0xe887b970`), before any phone is attached:
   - `lib_net_init`: lwIP plus the netif, which is set up straight away;
   - mDNS init (`0xe88129b6`);
   - `lib_iap_init`;
   - `0xe8873d8a`, which starts **`AirPlayThread`** (`0xe8872f98`). That
     thread:
     - creates the AirPlay receiver server with the vendor delegate;
     - creates `CarPlayControlClient` (browse callback `0xe8872d9c`);
     - sends `startServer`, which opens the HTTP server and **registers
       `_airplay._tcp`**;
     - starts the `_carplay-ctrl._tcp` browse (`0xe8872d06`).
2. So the registration exists **before** USB/NCM link-up. Nothing
   re-registers on link-up. The module has no link-state input; it only gets
   frames via MIoctrl 13, and the host drops outgoing frames until NCM is up.
   Once alt 1 is selected, mDNSResponder's normal query answering and
   periodic announcements reach the phone.
3. **When the phone's `_carplay-ctrl._tcp` appears**, the browse callback
   (`Adding CarPlayController`) calls `CarPlayControlClientConnect` up to 5
   times, which sends `GET /ctrl-int/1/connect` (§4).
4. Stock doesn't depend on `_carplay-ctrl` to be found. Its `_airplay._tcp`
   service is answerable whenever the phone asks, and the phone is already
   asking. The bench phone hasn't advertised `_carplay-ctrl` yet. Whether
   iOS connects as soon as it resolves `_airplay._tcp`, or needs the
   `ctrl-int` request first, is open.

### 5.5 Listening sockets

| Socket | Proto | Port | Evidence |
|---|---|---|---|
| AirPlay HTTP/RTSP server (`/info`, `pair-setup`, `pair-verify`, `auth-setup`, `SETUP`, `RECORD`, …) | TCP, IPv4 + IPv6 | **7000** if free, else dynamic. `server+220 = -7000` (`0xe8864e68`), passed to `ServerSocketPairOpen` (`0xe88514f4`), where negative means "try this port, then any" | confirmed |
| mDNS | UDP, v4 + v6 | 5353 | confirmed |
| Timing | UDP | dynamic | `0xe8869522`, "Timing set up on port %d" |
| Screen (H.264) stream | TCP | dynamic | `0xe886a1b6`, "screen receiver set up on port %d" |
| Audio data / RTCP | UDP | dynamic | `0xe886a5f0`, `0xe886ad6c`, `0xe886adb4` |
| Keep-alive | UDP | dynamic | `0xe886d85e` |
| Events | TCP | dynamic | `0xe886d8d6` |

Only port 7000 is fixed. The phone's first TCP SYN should go to **port 7000**
(the SRV record's port). Every other port is created during `SETUP` and sent
to the phone in the `SETUP` response.

## What's still unknown, and the cheapest bench test

1. **Does iOS accept stock's `1D01` from our firmware?** Cheapest: answer
   `1D00` with the 219 bytes above (our own name is fine) and log what comes
   back. Then check, in order:
   - **`1D02`**. If the phone sends `1D03` instead, its parameters are the IDs
     it disliked: decode them, then fix those first (13, then 34).
   - **`SET_INTERFACE` interface 2 alt 1** within a few seconds. That's the
     proof that the `USBHostTransportComponent` / interface-1 linkage works.
     After it, NCM frames start arriving on EP `0x02` (expect IPv6 ND and
     mDNS from the phone).

   This needs no new code beyond the reply, and `musb.c` already tracks
   `SET_INTERFACE`.
2. **Parameter 34's name** (probably `ProductPlanUID`) and **vehicle
   parameter 6's name** (probably DisplayName). They don't matter if test 1
   passes with stock's values.
3. **Whether the phone needs `AE00`/`AE03` before it brings NCM up.** Stock
   always sends both right after `1D02`. Send them too; dropping them is only
   worth testing if they cause trouble.
4. **The phone's `_carplay-ctrl` advertisement and the address family it
   uses.** Once NCM is up, capture the first few dozen frames from the phone
   (a counter plus a hex dump of the mDNS packets on screen). That shows
   whether the phone uses IPv6 link-local or IPv4, and the port of its
   `_carplay-ctrl` service. That decides what our IP side (or the hosted
   `carplay.mod`'s lwIP) must answer first.
   The bench answered part of this: the phone queries `_airplay._tcp` and
   gets no IPv4 address.
5. **Will the phone connect once `_airplay._tcp` is answered?** Cheapest:
   answer the phone's `_airplay._tcp` PTR query over IPv6 link-local, using
   §5's values:
   - PTR `CarPlay._airplay._tcp.local`;
   - SRV `Computer.local:7000`;
   - TXT `deviceid`/`features=0x4040280,0x21`/`flags=0x4`/`model`/`pi`/`srcvers=320.14`,
     with our own `pi` UUID;
   - AAAA for our fe80 address.

   Then log the first TCP SYN. A SYN to port 7000 means the next step is
   AirPlay `/info` and pairing.
