# CarPlay session: from SETUP to the first H.264 frame

This follows on from [carplay-pairing.md](carplay-pairing.md): it covers what comes after
`/auth-setup` on TCP 7000. It is written from two GPL-3.0 receivers, read for reference only.
No code was copied from them.
- **xcertplay** (`shared/src/main/java/com/shilapi/xcertplay/airplay/`): `AirPlaySession.kt`,
  `CarPlayMediaEngine.kt`, `ScreenStream.kt`, `AirPlayInfoPlist.kt`, `AirPlayHid.kt`,
  `ControlCipher.kt`, `AudioStream.kt`. It also ships two captured session logs,
  `common/src/test/resources/handshake-wireless*.txt`.
- **LIVI** (`native/crates/`):
  - `livi-cp/src/stack/{control,setup,events,feedback}.rs`
  - `livi-cp/src/{info,hid,timing,timing_sync,keep_alive,session_keys}.rs`
  - `livi-screen-stream/src/lib.rs`
  - `livi-cp/src/airplay.rs`, which is the plain-AirPlay path; it is cited only where noted.

Marks used below:
- **C** (confirmed): both sources implement it the same way, or a captured log shows it.
- **C1**: only one source shows it.
- **I** (inferred): our reading, not shown by either source. Check it on the bench.

The captures are wireless sessions. Wired differs only in the transport and the iAP2 steps
before TCP 7000; the RTSP layer is the same code path in xcertplay, which also does USB.

## 1. Requests after auth-setup, in order

All requests below go over the encrypted control channel (§4 of carplay-pairing.md). Every one
uses the same URL, `rtsp://<our addr>/<session id>`; the session id is the long decimal number
the bench saw. This order is taken from the captured log `handshake-wireless.txt` **(C)**:

| # | Request (phone → us) | Body | What we answer |
|---|---|---|---|
| 1 | `SETUP` (session) | bplist, about 770 B | 200 + bplist: `timingPort`, `eventPort`, `keepAlivePort`, `enabledFeatures` (§2) |
| — | *(the phone connects to `eventPort` within about 20 ms)* | | |
| 2 | `GET /info` | bplist, about 250 B (`altScreenURLs`, `uiContextURLs`, …) | 200 + bplist info, about 39 KB in the log; ours is much smaller (§4) |
| 3 | `RECORD` | none | 200, no body |
| 4 | `SETUP` (streams: `[{type 130}]`, the iAP channel) | bplist | `{streams:[{type,streamID,dataPort}]}`; only when `iAPChannel` is enabled |
| 5 | `POST /command` `modesChanged` (×2) | bplist `{type, params:{appStates, resources}}` | 200, no body |
| 6 | `POST /command` `disableBluetooth` | `{type, params:{deviceID}}` | 200 |
| 7 | `SETUP` (streams: `[{type 111}]`) | bplist | only when `altScreen` is enabled (cluster) |
| 8 | `SETUP` (streams: `[{type 110}]`) **← main screen** | bplist, 167 B | `{streams:[{type:110, dataPort:N}]}` |
| 9 | `SETUP` 130 (×2, `CarPlayProtocolData`, `…Data2`) | bplist | the same as #4 |
| 10 | `POST /command` `modesChanged` | | 200 |
| 11 | `SETUP` (streams: `[{type 100}]`) audio | bplist | `{streams:[{type,dataPort,controlPort,streamConnectionID}]}` |
| 12 | `POST /feedback` (repeats about every 2 s) | none | 200; optionally a bplist `{streams:[…]}` (§5) |
| — | `TEARDOWN` | optional bplist `{streams:[{type}]}` | 200. A list of streams closes only those; no body closes them all. **(C)** |

**Responses (C).** Every response follows the same rules:
- The status line is `RTSP/1.0 200 OK`, echoing `CSeq` and always sending `Content-Length`.
- A plist body carries `Content-Type: application/x-apple-binary-plist`.
- No other headers are needed.
- Both sources answer `200` to any request they don't recognise.
- `GET_PARAMETER`, `SET_PARAMETER` and `FLUSH`/`FLUSHBUFFERED` belong only to the AirPlay-audio
  path (LIVI `airplay.rs`). They don't appear in the CarPlay captures, so answer them with a
  bare 200. **(C1, I)**

LIVI's stack refuses anything except `OPTIONS`, `/info`, `/pair-*` and `/auth-setup` with
**470** until the control channel is encrypted (`control.rs` `before_pairing`). This is
optional for us.

### SETUP #1 (session): what the phone sends

The key list comes from the log **(C)**. The types are as the handlers read them.

| Key | Type | Use |
|---|---|---|
| `deviceID` | string (BT MAC) | log only |
| `macAddress` | string | log only |
| `model`, `name`, `osName`, `osVersion`, `osBuildVersion`, `sourceVersion` | string | log only |
| `features` | array of strings | the features the phone offers. Logged: `uiContext, viewAreas, cornerMasks, focusTransfer, h.264Level5.1, mainBuffered, altScreen, enhancedSiri, hevc, vehicleStateProtocol, sessionManagement, videoPlayback, logTransfer, iAPChannel` |
| `timingPort` | integer | the phone's NTP UDP port (§5) |
| `keepAliveLowPower` | bool | if true, we answer with `keepAlivePort` |
| `sessionUUID`, `sessionCorrelationUUID` | string | ignore |
| `statsCollectionEnabled` | bool | ignore |
| `updateSessionRequest` | | ignore |
| `timingProtocol` | string | **absent** in the CarPlay captures, so NTP. Only LIVI's AirPlay path handles `"PTP"`. |

### SETUP #1 response (C)

| Key | Type | Required | Value |
|---|---|---|---|
| `eventPort` | int | **yes** | our TCP listener (§3) |
| `timingPort` | int | **yes** | our UDP NTP socket (§5) |
| `keepAlivePort` | int | when the phone sent `keepAliveLowPower` true | a UDP socket; we receive and discard |
| `enabledFeatures` | array of strings | yes, as used by both | the subset we accept of the phone's `features`. Minimal choice: `["viewAreas"]` (§4) **(I)** |

Both sources open fresh ephemeral ports, bound on `::`.

### SETUP #2 (streams): what the phone sends

From the logged payloads **(C)**.

| Stream | Keys from the phone |
|---|---|
| 110/111 screen | `type` int; `streamConnectionID` (unsigned 64-bit int, often > 2^63); `latencyMs` int (75 logged); `uuid` string (the display uuid we declared in `/info`) |
| 100/101/102 audio | `type`, `streamConnectionID`, `audioFormat` (bitmask), `audioType` ("default"/"media"/"telephony"…), `audioLatencyMs`, `framesPerPacket`, `dataPort` (when present on 100, the phone's microphone port) |
| 130 data | `controlType`, `channelID`, `seed`, `clientUUID`, `clientTypeUUID` (E9459FD0-… = iAP channel), `wantsDedicatedSocket`, `sendMessageAsIs` |

### SETUP #2 response (C)

The response is `{streams: [ one dict per stream we accept ]}`. A stream we don't support is
simply left out of the array. Both sources do this, and the phone carried on in the logs.

| Stream | Response entry |
|---|---|
| 110 / 111 | `{type, dataPort}`. Nothing else. |
| 100 / 101 / 102 | `{type, dataPort, controlPort, streamConnectionID}`; the id is echoed, unsigned |
| 130 | `{type: 130, streamID, dataPort}` |

## 2. Ports we must listen on

| Port | Proto | Who connects | What flows |
|---|---|---|---|
| 7000 | TCP | phone | control RTSP (already working) |
| `eventPort` | TCP | **phone connects to us** right after the SETUP #1 response | encrypted RTSP in both directions (§3) |
| `timingPort` | UDP | both | NTP-style timing, 32-byte packets (§5) |
| `keepAlivePort` | UDP | phone → us | beacons; LIVI only notes when it last heard one; xcertplay ignores them |
| screen `dataPort` | TCP | **phone connects to us** after the SETUP 110 response | 128-byte header + body messages (§3b) |
| audio `dataPort` / `controlPort` | UDP | phone → us | RTP + RTCP (§6) |

## 3. The event channel (C)

- **Transport.** We listen on TCP; the phone connects. Its framing and cipher are the same as
  the control channel's:
  - Each frame is a u16 LE length, used as the AAD, then the ChaCha20-Poly1305 ciphertext,
    then a 16-byte tag.
  - The nonce is 4 zero bytes followed by a 64-bit LE counter.
  - The read and write counters are separate, start at 0 and are independent of the
    control channel's.
- **Keys.** HKDF-SHA512 over the **pair-verify X25519 shared secret** (the same one used for the
  control channel), salt `"Events-Salt"`, 32-byte output:
  - We **decrypt** with info `"Events-Read-Encryption-Key"`.
  - We **encrypt** with info `"Events-Write-Encryption-Key"`.
  - **These are not swapped, unlike control.** For the control channel we decrypt with
    `Control-Write-…`. xcertplay (`acceptEvent`) and LIVI (`session_keys::events`, which both
    paths pass straight to `ControlCipher::new(read, write)`) agree on this.
- **Content.** RTSP-shaped messages travel in both directions:
  - **Us → phone:** `POST /command RTSP/1.0`, with `Content-Type: application/x-apple-binary-plist`,
    `Content-Length` and our own `CSeq` counting from 1. The body is a bplist
    `{type: <string>, params: {…}}`.
  - **Phone → us:** it answers `RTSP/1.0 200 OK`; we ignore that. It may also send requests on
    this socket. Both sources answer `POST …/command` like control `/command`, and anything
    else with a bare 200, encrypted on the same channel.
- **What we send there:** HID reports (§7), `setNightMode`, `forceKeyFrame` (`params` empty, or
  `{uuid}`), `updateViewArea`, `requestSiri`, `iAPSendMessage`. **None is needed for first video.** **(C)**

### 3b. The screen stream (type 110) (C)

- **Key.** HKDF-SHA512, 32-byte output, where:
  - IKM = the pair-verify shared secret.
  - salt = the ASCII string `"DataStream-Salt"` followed by the **unsigned decimal** of
    `streamConnectionID`. Example: `DataStream-Salt1586959786098771707`.
  - info = `"DataStream-Output-Encryption-Key"`.

  Source: xcertplay `dataStreamKey`, LIVI `setup_screen`/`session_keys::data_stream`.
  - A `streamConnectionID` above 2^63 comes over as a 16-byte bplist int, or as a negative
    int64 in a naive parser. It must be printed as **unsigned**.
  - Our `bplist.c` keeps the low 8 bytes in an `int64_t`, so use a `%llu`-style conversion of
    `(uint64_t)i`.
- **Transport.** TCP, a single connection. Messages repeat:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | body size, u32 **LE** |
| 4 | 1 | opcode: **0** = video frame, **1** = video config |
| 5 | 123 | the rest of the 128-byte header (timestamp etc.; neither source reads it) **(C)**; contents not decoded **(I)** |
| 128 | size | body |

- **Opcode 1 (config).** The body is **clear text**:
  - It carries an `avcC` box, or `hvcC` for HEVC.
  - Both sources search the body for the 4CC `avcC` or `hvcC` and take the bytes that follow
    it as the AVCDecoderConfigurationRecord.
  - If there is no 4CC, xcertplay checks whether the body itself looks like an avcC record.
  - The NAL length size is `(record[4] & 3) + 1`; it is 4 in practice. The SPS and PPS live
    in this record.
  - The config comes first, and again whenever the stream restarts.
- **Opcode 0 (frame):**
  - `body = ChaCha20-Poly1305(ciphertext ‖ 16-byte tag)`.
  - **The AAD is the whole 128-byte header.**
  - The nonce is 4 zero bytes followed by a u64 LE frame counter. The counter starts at 0 per
    connection and goes up only on frames that decrypt successfully. Config messages don't use
    a counter value.
  - A body shorter than 16 bytes is passed through clear.
  - The plaintext is **AVCC**: length-prefixed NAL units with 4-byte big-endian lengths, **not
    Annex-B**. For a decoder that wants Annex-B, replace each length with `00 00 00 01` and
    prepend the SPS and PPS from the avcC record.
- **Logged sizes.** The config was 139–154 B. The first frame was 0.5–1.1 KB, about 150 ms
  after SETUP 110 (an HEVC session).
- **Codec.** We get H.264 when we neither enable `hevc` in `enabledFeatures` nor put `hevcInfo`
  in `/info`. **(C)** (Both sources gate HEVC on exactly these two.)

## 4. `GET /info` (C: the phone asks for it, also on our wired path; I: minimal content)

- **When the phone asks.** It sends `GET /info` right after SETUP #1, with a small bplist
  request body (`altScreenURLs`, `uiContextURLs`, …), which we may ignore.
- **What happens without it.** xcertplay notes that without a complete declaration (display,
  audio formats, modes, HID devices) "the phone aborts the session".
- **The response** is a bplist dict, `Content-Type` plist.

The fields both sources send, with what we'd send for 1280×720 and no audio:

| Key | Type | Ours | Note |
|---|---|---|---|
| `sourceVersion` | string | the same as 0x4301 param 5 | |
| `features` | int (u64) | `CARPLAY_FEATURES & ~CARPLAY_AUDIO_FEATURES`. These are xcertplay's constants: 0x615653AEE2 and 0x10004540A00. | Both sources clear the audio bits when audio output is off. |
| `statusFlags` | int | 4 | both sources |
| `model`, `manufacturer`, `name` | string | ours | |
| `deviceID` | string | our MAC string, the same as 0x4301 param 3 | |
| `bluetoothIDs` | array of strings | `[ our BT MAC ]` | both send it; a dummy is probably fine **(I)** |
| `keepAliveLowPower` | bool | true (LIVI) / false (xcertplay) | either |
| `keepAliveSendStatsAsBody` | bool | false | |
| `rightHandDrive`, `nightMode` | bool | false | |
| `modes` | dict | `resources`: screen (id 1) and audio (id 2), each `{transferType 1, transferPriority 100, take/borrow/unborrowConstraint 100}`; `appStates`: `[{appStateID 2, state false}, {appStateID 1, speechMode -1}, {appStateID 3, state false}]` | Both sources send identical modes. The phone then takes the screen itself (`modesChanged` puts resource 1 under owner 1). **(C)** |
| `extendedFeatures` | array of strings | `["vocoderInfo","enhancedRequestCarUI"]` | probably optional **(I)** |
| `displays` | array | one entry, below | **required** |
| `hidDevices` | array | the touch device (§7) | needed for touch |
| `audioLatencies`, `audioFormats` | array | leave both out when audio is off | sent only when audio is enabled |
| `hevcInfo` | dict | **leave out** | keeps us on H.264 |

### The display entry (C): 1280×720 landscape

| Key | Value |
|---|---|
| `uuid` | our fixed UUID string; the phone echoes it as `uuid` in SETUP 110 |
| `type` | 110 |
| `widthPixels` / `heightPixels` | 1280 / 720 |
| `widthPixelsMax` / `heightPixelsMax` | 1280 / 720 (xcertplay only) |
| `widthPhysical` / `heightPhysical` | in mm; xcertplay clamps to 1..2000 and defaults the width to 200. Use the panel's real size, about 155×87 for 7". |
| `maxFPS` | 30 to 60 (xcertplay snaps to steps of 5). **Use 30** for our decoder. **(I)** |
| `features` | `0x08 \| 0x02` = 10 (high-fidelity touch \| knobs); use 0x08 alone if we have no knob **(I)** |
| `primaryInputDevice` | an int; LIVI defaults to 3 (knobs). With touch only, 1 is probably right. **(I)** |
| `viewAreas` | `[ {viewAreaIndex 0, widthPixels 1280, heightPixels 720, originXPixels 0, originYPixels 0, viewAreaTransitionControl 0/false, viewAreaStatusBarEdge 0, viewAreaSupportsFocusTransfer false, safeArea {widthPixels 1280, heightPixels 720, originXPixels 0, originYPixels 0, drawUIOutsideSafeArea true}} ]` |
| `initialViewArea` | 0 |
| `adjacentViewAreas` | `[]` |

- **`viewAreas` and `enabledFeatures`.** xcertplay notes that since the SETUP #1 response
  enables `viewAreas`, `/info` must describe at least one view area. If `viewAreas` is left out
  of `enabledFeatures`, the `viewAreas` dict might be optional too; that is untested **(I)**. The
  safe choice is to enable it and describe one full-screen area.
- **Features not to enable for first video:** `altScreen`/`uiContext` (cluster), `hevc`, and
  `iAPChannel`. We already have iAP2 on USB; enabling `iAPChannel` adds three type-130 SETUPs
  for an iAP2 tunnel. **(I)**

## 5. Timing (C: NTP, not PTP)

- **The protocol.** CarPlay SETUP carries `timingPort`, with no `timingProtocol` key, so it is
  AirPlay-1-style NTP over UDP. Packets are 32 bytes:

  | Offset | Request (payload type **210**) | Response (payload type **211**) |
  |---|---|---|
  | 0 | 0x80 | 0x80 |
  | 1 | 210 | 211 |
  | 2..3 | u16 BE 7 | u16 BE 7 |
  | 8..15 | — | the requester's transmit time, copied from bytes 24..31 of the request |
  | 16..23 | — | the time we received it |
  | 24..31 | the transmit time (NTP 64-bit BE: seconds since 1900 · 2^32 + fraction) | the time we sent it |

- **What both sources do.** They answer every 210 with a 211 on our `timingPort`. They also
  send a 210 to the phone's `timingPort` once a second and filter the offset from the replies
  (LIVI `timing.rs`, `timing_sync.rs`; xcertplay `NtpClock`).
- **Minimum for video (I).** Video framing uses neither clock, so answering 210 with 211 should
  be enough, and might not even be needed. The NTP sync feeds only audio `/feedback`
  timestamps.
- **`/feedback`.** With no audio streams, both sources reply to `POST /feedback` with a bare 200.

## 6. Audio, for completeness (C, not needed first)

- **Wire layout.** UDP RTP, one packet per datagram: a 12-byte RTP header, then the ciphertext,
  the 16-byte tag, and an 8-byte LE nonce tail.
  - The full nonce is 4 zero bytes followed by that tail.
  - The AAD is RTP header bytes 4..11.
  - The key is `DataStream-Output-Encryption-Key` with that stream's `streamConnectionID` salt.
  - The microphone uplink uses `DataStream-Input-Encryption-Key`.
- **Sources.** xcertplay `AudioStream.kt`, LIVI `setup_audio`.
- **Avoiding audio (I).** Leaving `audioFormats` and `audioLatencies` out of `/info` and
  clearing the audio feature bits should stop the type-100 SETUP from coming. Both sources
  support exactly this mode (`disableAudioOutput`).

## 7. Touch (C)

- **Declaring the device.** In `/info` `hidDevices`, add one dict:
  - `uuid`: a hex string, for example `"2a2a2a2a"`. Both sources use these exact uids.
  - `name`: a string.
  - `hidVendorID` 2, `hidProductID` 1, `hidCountryCode` 0.
  - `displayUUID`: the display's uuid.
  - `hidDescriptor`: data.
- **The descriptor.** It is a Digitizer/Touch Screen application collection with **2 finger
  logical collections**. Each finger carries:
  - Contact Identifier: 8 bits.
  - Tip Switch: 1 bit, plus 7 bits of padding.
  - X: Generic Desktop, 16 bits, logical 0..`widthPixels`.
  - Y: 16 bits, logical 0..`heightPixels`.

  Write it ourselves from the HID usage tables using this layout.
- **The report.** 12 bytes, with 6 per finger slot:
  - `id u8` (the slot number)
  - `tip u8` (bit 0 = down)
  - `x u16 LE`, in **pixels**
  - `y u16 LE`

  Unused slots carry only their id. A lift is a report with tip = 0.
- **Sending.** Send it on the **event channel** as `POST /command`, body bplist
  `{type: "hidSendReport", uuid: "2a2a2a2a", hidReport: <data 12 B>}`. These are top-level keys,
  with no `params`. Both sources do this.

## 8. Minimal path to "first H.264 frame received"

| Step | Must do | May stub |
|---|---|---|
| SETUP #1 | Parse the bplist; reply with `eventPort` (TCP listen), `timingPort` (UDP socket), `keepAlivePort` if asked, and `enabledFeatures ["viewAreas"]` | the device-info keys |
| Event channel | Accept the connection; set up the Events cipher (§3). Read and decrypt; answer any request with an encrypted 200. | Sending anything. Even accepting and draining might be enough: LIVI's AirPlay path just drains when it has no keys. **(I)** |
| Timing | Answer 210 with 211 | Sending our own 210s **(I)** |
| `GET /info` | `displays` (one 1280×720 entry with a view area), `modes`, `features` without the audio bits, `statusFlags` 4, ids, `hidDevices` (touch can wait, but keep the key) | audio formats, icons, `extendedFeatures` **(I)** |
| `RECORD` | 200 | |
| `POST /command` | 200 to everything (`modesChanged`, `disableBluetooth`, `requestUI`, …) | parsing them |
| SETUP 110 | Derive the key from `streamConnectionID` (unsigned decimal!); TCP-listen; reply `{streams:[{type:110,dataPort}]}` | |
| SETUP 100/101/130 | Leave them out of the `streams` array | |
| `/feedback` | 200 | |
| Screen TCP | Read 128 + size bytes. Opcode 1: keep avcC. Opcode 0: AEAD-open with AAD = header and nonce = counter++. You now have AVCC NALs. | Decoding |

**Bench checks, in order of risk:**
1. Does the phone connect to `eventPort`? If it doesn't, check that the port is reachable over
   IPv6 link-local on NCM.
2. Does it send `GET /info`?
3. Does the `modesChanged` with screen owner 1 arrive?
4. Does SETUP 110 arrive?
5. Does frame 0 decrypt? If it fails, check the unsigned-decimal salt and that the AAD is the
   whole header.
