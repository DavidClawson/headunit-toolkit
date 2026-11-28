# Wired CarPlay pairing: the wire spec for pair-setup, pair-verify and auth-setup (2026-11-27)

This picks up from the bench result that, after our iAP2 `0x4301`
CarPlayStartSession, the phone connects to TCP 7000 and sends
`POST /pair-setup RTSP/1.0`. It covers what the receiver must do from there
until the control channel is encrypted and MFi-authenticated.

**Sources.** Read on 2026-11-27, used as protocol references only. Both are
GPL-3.0; nothing is copied, and the C implementation here is ours (MIT).
- **xcertplay** (https://github.com/shilapi/xcertplay, Kotlin). Mainly
  `shared/.../airplay/`: `PairSetup.kt`, `Srp6a.kt`, `PairVerify.kt`,
  `ControlCipher.kt`, `AirPlayCrypto.kt`, `Tlv8Codec.kt`, `RtspMessage.kt`,
  `AirPlaySession.kt` (`handle`, the read loop), `MfiSapAuthSetup.kt`,
  `AirPlayIdentity.kt`; and `shared/.../mfi/MfiAuthenticationClient.kt`
  (`signChallenge`).
- **LIVI** (https://github.com/f-io/LIVI, Rust; xcertplay credits it as its
  reference). `native/crates/livi-cp/src/`: `pair_setup.rs`, `srp.rs`,
  `pair_verify.rs`, `session_keys.rs`, `control_cipher.rs`, `rtsp.rs`,
  `tlv8.rs`, `auth_setup.rs`, `identity.rs`, `airplay.rs` (`Conn::on_bytes`,
  `Conn::handle`, `pair_setup`, `auth_setup`, `info`), `mfi4/`.

**Labels.**
- **C:** confirmed from source. Both projects agree unless a row says
  otherwise.
- **I:** inferred. Not checked against a phone capture.

Neither project contains a captured request from a phone. So the exact
request headers the phone sends (item 1) aren't known from these sources.

## 1. RTSP framing on TCP 7000

| Item | Rule | |
|---|---|---|
| Request line | `METHOD path PROTO`; PROTO is usually `RTSP/1.0`. Requests are dispatched on the **path suffix** (`/pair-setup`, `/pair-verify`, `/auth-setup`, `/info`, `/command`, `/feedback`; `SETUP`, `RECORD`, `TEARDOWN` by method). Neither project checks the request's `Content-Type` | C |
| Request headers | Header names are case-insensitive (both lower-case them). Body length comes from `Content-Length`; a missing one means 0. Several requests may arrive back to back in one read, so parse in a loop and keep the partial remainder | C |
| Response status line | **Echo the request's protocol** (`RTSP/1.0 200 OK`). Status texts: 200 OK, 400 Bad Request, 404 Not Found, 470 Connection Authorization Required (LIVI), 500 Internal Server Error | C |
| Response headers | Only these three: `Content-Type` (when there's a body), **`CSeq` echoed from the request**, and `Content-Length` (always, including `0`). Neither sends `Server`, `Date` or `Audio-Jack-Status` | C |
| Content types sent | `/pair-setup` and `/pair-verify`: `application/pairing+tlv8`. `/auth-setup`: `application/octet-stream`. `/info` and others: `application/x-apple-binary-plist` | C |
| What the phone sends | Not in either source. Our bench saw `Content-Type: application/x-…` on `/pair-setup`, 278 bytes including headers. For comparison, pyatv's AirPlay 2 *client* (`pyatv/protocols/airplay/auth/hap.py`, `hap_transient.py`) sends `Content-Type: application/octet-stream` with `X-Apple-HKP: 3` for normal pairing and `4` for transient. The phone may use different values; **log every header and the body in hex** | I |
| `/fp-setup` (FairPlay) | LIVI answers **404**. xcertplay has no handler and falls through to a bare 200. CarPlay uses MFi-SAP instead | C |
| `/info` | `GET` or `POST`, with an optional bplist body. Reply with a bplist. LIVI also answers a `qualifier` of `txtAirPlay` with the TXT record as data. Keys LIVI sends include `deviceID`, `macAddress`, `features`, `statusFlags` 68, `model`, `manufacturer`, `name`, `sourceVersion`, `protocolVersion` `"1.1"`, **`pk`** (32-byte data), **`pi`** (pairing ID string), `vv` 2, `keepAliveLowPower`, `displays`, `supportedFormats`. xcertplay builds its own in `AirPlayInfoPlist.kt` | C |
| Connection | **Persistent.** One TCP connection carries the whole exchange: pair-setup, then pair-verify, then (encrypted) auth-setup, `/info`, SETUP and so on. Close only on EOF or an error. Each response goes out before the next request is parsed | C |
| Encryption switch | After the response to a **successful pair-verify M3/M4** (its control keys exist), every later byte in both directions uses the control cipher (§4). The M4 response itself goes out in clear. LIVI also switches after **transient** pair-setup M4 (§2.4) | C |

## 2. pair-setup

### 2.1 TLV8 (`Tlv8Codec.kt`, `tlv8.rs`)

- **Items:** `type u8, len u8, value[len]`.
- **Values longer than 255 bytes** split into consecutive items of the same
  type: 255, 255, …, rest. The decoder joins a same-type item onto the
  previous one when the previous item had length 255.
- **Two different items of the same type in a row** are separated by
  `FF 00`.
- **Multi-byte values** (e.g. the 384-byte SRP B) are carried as raw
  big-endian byte strings.

| Type | Name | Used in |
|---|---|---|
| 0x00 | Method | M1 (0 = pair-setup); ignored by both |
| 0x01 | Identifier | M5/M6 sub-TLV; pair-verify M2/M3 sub-TLV |
| 0x02 | Salt | M2 (16 bytes) |
| 0x03 | PublicKey | M2 (B, 384 bytes), M3 (A), M5/M6 sub-TLV (Ed25519 LTPK, 32), pair-verify M1/M2 (X25519, 32) |
| 0x04 | Proof | M3 (client M, 64), M4 (server proof, 64) |
| 0x05 | EncryptedData | ciphertext ‖ 16-byte Poly1305 tag |
| 0x06 | State | every message: 1..6 |
| 0x07 | Error | 2 = Authentication |
| 0x0A | Signature | Ed25519, 64 bytes |
| 0x13 | Flags | M1, optional; bit `0x10` = transient (LIVI only) |

**Errors:** reply `{State = expected state, Error = 2}` in TLV8, with HTTP
status 200 (C).

### 2.2 SRP-6a (`Srp6a.kt`, `srp.rs`; both identical, C)

| Symbol | Definition |
|---|---|
| Group | RFC 5054 3072-bit N, g = 5. `|N|` = 384 bytes |
| H | SHA-512 |
| `pad(x)` | big-endian, left-padded with zeros to 384 bytes |
| `bytes(x)` | minimal big-endian (no leading zero bytes) |
| I | `"Pair-Setup"` (ASCII, 10 bytes) |
| P | `"3939"` (ASCII); the fixed setup code, never shown to the user |
| s | 16 random bytes |
| b | 32 random bytes |
| k | `H(bytes(N) ‖ pad(g))` |
| x | `H(s ‖ H(I ‖ ":" ‖ P))` |
| v | `g^x mod N` |
| B | `(k·v + g^b) mod N`, sent as `pad(B)` |
| A | from M3. Reject if `A mod N == 0` |
| u | `H(pad(A) ‖ pad(B))` |
| S | `(A · v^u)^b mod N` |
| **K** | `H(bytes(S))`: 64 bytes. **Note `bytes`, not `pad`.** If S has a leading zero byte, it's hashed without it |
| M (client proof, check) | `H( (H(bytes(N)) XOR H(bytes(g))) ‖ H(I) ‖ s ‖ pad(A) ‖ pad(B) ‖ K )`. `H(bytes(g))` = SHA-512 of the single byte `0x05` |
| M2 (server proof) | `H(pad(A) ‖ M ‖ K)` |

Comparing against a fresh SRP client implementation, as LIVI's `srp.rs`
tests do, is the quickest unit test.

### 2.3 Messages (normal, non-transient)

| Message | Direction | TLV contents | |
|---|---|---|---|
| M1 | phone → us | State = 1 (Method = 0; Flags optional) | C |
| M2 | us → phone | State = 2, PublicKey = pad(B), Salt = s | C |
| M3 | phone → us | State = 3, PublicKey = A, Proof = M | C |
| M4 | us → phone | State = 4, Proof = M2. Or, if M doesn't match: State = 4, Error = 2 | C |
| M5 | phone → us | State = 5, EncryptedData. Decrypt with key **SK**, nonce `"PS-Msg05"` → sub-TLV {Identifier = controller ID (UTF-8), PublicKey = controller Ed25519 LTPK, Signature} | C |
| M5 check | | Verify `Ed25519(LTPK_c, CSK ‖ ID_c ‖ LTPK_c)`. Then **store (ID_c → LTPK_c) persistently**, because pair-verify needs it | C |
| M6 | us → phone | State = 6, EncryptedData = Seal(SK, `"PS-Msg06"`, sub-TLV {Identifier = **our pairing ID**, PublicKey = **our Ed25519 LTPK**, Signature = `Ed25519_sign(LTSK_a, ASK ‖ ID_a ‖ LTPK_a)`}) | C |

**Keys.** All are HKDF-SHA-512 with IKM = K (64 bytes) and **32-byte**
output (C):

| Key | Salt | Info |
|---|---|---|
| SK (M5/M6 encryption) | `Pair-Setup-Encrypt-Salt` | `Pair-Setup-Encrypt-Info` |
| CSK (controller sign) | `Pair-Setup-Controller-Sign-Salt` | `Pair-Setup-Controller-Sign-Info` |
| ASK (accessory sign) | `Pair-Setup-Accessory-Sign-Salt` | `Pair-Setup-Accessory-Sign-Info` |

**ChaCha20-Poly1305** (RFC 8439, IETF, 12-byte nonce) (C):
- **Label nonces:** 4 zero bytes, then the 8-byte ASCII label (`"PS-Msg05"`,
  `"PS-Msg06"`, `"PV-Msg02"`, `"PV-Msg03"`).
- **No AAD.**
- **EncryptedData** = ciphertext ‖ 16-byte tag.

### 2.4 The transient variant

LIVI only (C), via `Conn::pair_setup` and `on_bytes` in `airplay.rs`:
- **The trigger:** M1 carries Flags (0x13) with bit `0x10`.
- **What happens:** pair-setup stops after **M3/M4**. The control keys
  (§4.1) come from **K** instead of the pair-verify secret, and encryption
  starts after the M4 response.
- **xcertplay doesn't handle transient** and always runs M1–M6. Since
  xcertplay works with current phones, wired CarPlay normally uses the full
  M1–M6 plus pair-verify (I).

**On the bench:** check whether our M1 contains `13 01 10`.

### 2.5 The accessory identity

| Item | Value | |
|---|---|---|
| Long-term key | Ed25519 key pair, generated once and **persisted**: LTSK (secret) and LTPK (32-byte public key) | C |
| Pairing ID (`Identifier` in M6/PV-M2) | a random **UUID v4 string**, lower-case with hyphens (e.g. `xxxxxxxx-xxxx-4xxx-…`). Generated once with the key, and also sent as `pi` in `/info` | C (xcertplay `AirPlayIdentity.generate`, LIVI `identity.rs`) |
| Persist | the identity (LTSK, LTPK, pairing ID), plus **each controller's (ID → LTPK)** from M5. Losing the controller entry makes pair-verify fail ("unknown controller"), and the phone then has to pair again | C / I |

## 3. pair-verify

`PairVerify.kt`, `pair_verify.rs`. TLV8 over `/pair-verify`.

| Message | Direction | Contents | |
|---|---|---|---|
| M1 | phone → us | State = 1, PublicKey = the phone's X25519 ephemeral key (32 bytes) | C |
| M2 | us → phone | Generate X25519 ephemeral (e, E). shared = X25519(e, E_c), rejecting all-zero. PVK = HKDF-SHA512(shared, `Pair-Verify-Encrypt-Salt`, `Pair-Verify-Encrypt-Info`, 32). Reply: State = 2, PublicKey = E, EncryptedData = Seal(PVK, `"PV-Msg02"`, sub-TLV {Identifier = our pairing ID, Signature = `Ed25519_sign(LTSK_a, E ‖ ID_a ‖ E_c)`}) | C |
| M3 | phone → us | State = 3, EncryptedData. Open(PVK, `"PV-Msg03"`) → {Identifier = ID_c, Signature} | C |
| M3 check | | Look up LTPK_c by ID_c; if unknown, error. Verify `Ed25519(LTPK_c, E_c ‖ ID_c ‖ E)` | C |
| M4 | us → phone | State = 4 only. Errors: State = 4 (or 2), Error = 2 | C |

**Signature order:**
- **Ours:** our ephemeral, our ID, theirs.
- **Theirs:** their ephemeral, their ID, ours.

## 4. The encrypted control channel

### 4.1 Keys

HKDF-SHA-512, 32-byte output, with IKM = the pair-verify X25519 shared
secret (32 bytes). For transient pair-setup, IKM = K instead (LIVI) (C):

| Our key | Salt | Info |
|---|---|---|
| **read** (decrypt phone → us) | `Control-Salt` | **`Control-Write-Encryption-Key`** |
| **write** (encrypt us → phone) | `Control-Salt` | **`Control-Read-Encryption-Key`** |

The names are from the controller's point of view, which is why they look
swapped. Both projects do the same.

**Later keys** (same IKM, from LIVI `session_keys.rs`; C, for when they're
needed):
- **Events channel:** `Events-Salt`, with `Events-Read-Encryption-Key` and
  `Events-Write-Encryption-Key`;
- **Data streams:** salt `"DataStream-Salt" + <streamConnectionID>`, with
  info `DataStream-Output-Encryption-Key` or
  `DataStream-Input-Encryption-Key`. The screen stream uses the output key
  (`ScreenStream.kt`).

### 4.2 Framing (`ControlCipher.kt`, `control_cipher.rs`; C)

| Field | Size | Notes |
|---|---|---|
| length L | 2 bytes, **little-endian** | plaintext length; **also the AAD** (both bytes) |
| ciphertext | L | ChaCha20-Poly1305 |
| tag | 16 | |

- **Max L is 0x4000** (16,384). Longer plaintext is split into several
  frames.
- **Nonce:** 4 zero bytes, then a 64-bit **little-endian counter**. There's
  one counter per direction, each starting at 0 and incremented per frame.
- **Plaintext** is the ordinary RTSP request/response bytes; the frames
  don't line up with message boundaries. Buffer partial frames.

## 5. MFi-SAP: `/auth-setup`

`MfiSapAuthSetup.kt`, `auth_setup.rs`. It arrives **after** pair-verify, so
it runs over the encrypted channel (C, per xcertplay's doc comment).

| Item | Content | |
|---|---|---|
| Request body | 33 bytes: `0x01` (version), then the phone's X25519 public key (32). Anything else → 400 | C |
| Key exchange | our ephemeral X25519 (e, E); shared = X25519(e, peer); reject all-zero | C |
| AES key | `SHA-1("AES-KEY" ‖ shared)[0:16]` (ASCII `AES-KEY`, 7 bytes, no NUL) | C |
| AES IV | `SHA-1("AES-IV" ‖ shared)[0:16]` | C |
| What the chip signs | **MFi 3.0 (protocol major ≠ 2):** `SHA-256(E ‖ peer)`, 32 bytes. **MFi 2.0:** `SHA-1(E ‖ peer)`, 20 bytes. Order: **our key first** | C |
| Chip sequence | Write challenge length (u16 BE) to **0x20**, the digest to **0x21**, then `0x01` to **0x10** (start). Poll **0x10** until it reads **0x10** (status bits 4–6 = 1), every 10 ms, timeout 3 s. Read the signature length (u16 BE) from **0x11**, then the signature from **0x12** (64 bytes for 3.0). This is **the same sequence as our iAP2 `AA02`/`AA03` path**, just with a different 32-byte input | C (xcertplay `signChallenge`); our bench matches |
| Certificate | the same chip certificate as iAP2 `AA01` (length at 0x30, data from 0x31 in 128-byte windows; 607 bytes on our chip) | C |
| Response body | `E (32) ‖ u32 BE cert_len ‖ cert ‖ u32 BE sig_len ‖ AES-128-CTR(key, IV, signature)`, with `Content-Type: application/octet-stream` | C |
| Variants not needed | **BAA** certificate type (xcertplay), and LIVI's **MFi4** path, selected by request header `X-Apple-AT: 0x20` (32), with `X-Apple-PairingTranscript` and HKDF `MFi4-KT1/KT2` transcript proofs (`mfi4/mod.rs`). Neither applies to a physical MFi 3.0 chip unless the phone sends `X-Apple-AT` | C |

## 6. Our key in `0x4301` (param 4) and in `/info`

| Point | | |
|---|---|---|
| 1 | Both put **the same long-term Ed25519 public key** in three places: `0x4301` param 4 (hex string), `/info` `pk` (32-byte data), and the M6 sub-TLV PublicKey | C |
| 2 | **Hex form:** lower-case, 64 characters, no separators (Kotlin `toHexString`, LIVI `hex::encode`) | C |
| 3 | `0x4301` param 3 (device ID) is the MAC string. It's also used as `/info` `deviceID`/`macAddress` (LIVI) | C |
| 4 | Whether the phone **rejects** a pairing whose M6 LTPK differs from `0x4301`'s key isn't shown in either source. Keep them identical | I |
| 5 | The key must be **stable across sessions**. A new key forces a new pair-setup, and probably a new per-car entry on the phone | I |

## Checklist for our C implementation

1. **The RTSP loop:** parse, dispatch on path, echo the protocol and `CSeq`,
   always send `Content-Length`, keep the connection open.
2. **TLV8** with 255-byte fragmentation (M2's 384-byte B is two fragments,
   255 + 129).
3. **SRP:**
   - a 3072-bit modexp on our bignum; exponents are 512-bit (x) and 256-bit
     (b), and u is 512-bit;
   - K = SHA-512 of the minimal-bytes S.
4. **Monocypher** (the vendored copy in `tools/fel/dram/third_party/monocypher`
   has all of these):
   - ChaCha20-Poly1305 in IETF mode (12-byte nonce, RFC 8439) via
     `crypto_aead_init_ietf`. Not the default XChaCha `crypto_aead_lock`;
   - Ed25519 via `crypto_ed25519_key_pair`, `crypto_ed25519_sign` and
     `crypto_ed25519_check` from `monocypher-ed25519.c` (SHA-512, RFC 8032).
     Not the Blake2b `crypto_eddsa_*`;
   - X25519 via `crypto_x25519`;
   - SHA-512 and HKDF-SHA-512 via `crypto_sha512` and `crypto_sha512_hkdf`
     (salt, info, 32-byte output).
5. **Also needed:** SHA-1, SHA-256 and AES-128-CTR, for auth-setup only.
6. **Persistent storage:** our LTSK, LTPK and pairing ID, plus each
   controller's ID → LTPK.
7. **Switch to the control cipher** right after sending the pair-verify M4
   (or the transient pair-setup M4).
