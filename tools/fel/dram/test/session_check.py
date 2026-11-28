"""Checks the extracted session code: /info plist, SETUP replies and keys, screen stream."""
import plistlib, subprocess, struct, sys, os
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.hazmat.primitives import hashes
p = subprocess.Popen(['./session_host'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
def ask(s): p.stdin.write(s + '\n'); p.stdin.flush(); return p.stdout.readline().split()
info = plistlib.loads(bytes.fromhex(ask('I')[0]))
d = info['displays'][0]
print('info: keys', len(info), '| display', d['widthPixels'], 'x', d['heightPixels'], '| viewAreas', len(d['viewAreas']),
      '| hid desc', len(info['hidDevices'][0]['hidDescriptor']), 'B | features', hex(info['features']))
r = ask('S ' + plistlib.dumps({'timingPort': 50000, 'keepAliveLowPower': True, 'features': ['viewAreas', 'hevc', 'altScreen'], 'name': 'x'}, fmt=plistlib.FMT_BINARY).hex())
print('SETUP1 ->', plistlib.loads(bytes.fromhex(r[0])))
sid = 0xF123456789ABCDEF                              # > 2^63
r = ask('S ' + plistlib.dumps({'streams': [{'type': 110, 'streamConnectionID': sid, 'latencyMs': 75, 'uuid': 'x'}, {'type': 100, 'streamConnectionID': 5}]}, fmt=plistlib.FMT_BINARY).hex())
print('SETUP2 ->', plistlib.loads(bytes.fromhex(r[0])), '| log:', ' '.join(r[2:]))
shared = bytes((i * 3 + 7) & 255 for i in range(32))
key = HKDF(hashes.SHA512(), 32, b'DataStream-Salt' + str(sid).encode(), b'DataStream-Output-Encryption-Key').derive(shared)
print('screen key matches unsigned-decimal salt:', r[1] == key.hex())
# screen stream: config (avcC) then encrypted AVCC frames from a real x264 stream
es = open(sys.argv[1], 'rb').read()
nals = [n for n in es.split(b'\x00\x00\x01') if n]
nals = [n.rstrip(b"\x00") for n in nals]; nals = [n for n in nals if n]
sps = next(n for n in nals if n[0] & 31 == 7); pps = next(n for n in nals if n[0] & 31 == 8)
avcc = bytes([1, sps[1], sps[2], sps[3], 0xFF, 0xE1]) + struct.pack('>H', len(sps)) + sps + b'\x01' + struct.pack('>H', len(pps)) + pps
def hdr(size, op): return struct.pack('<IB', size, op) + bytes(123)
stream = hdr(len(avcc) + 8, 1) + b'\0\0\0\0avcC' + avcc
aead = ChaCha20Poly1305(key); ctr = 0; frames = []; cur = b''
for n in nals:
    t = n[0] & 31
    if t in (7, 8): continue
    if t in (1, 5):
        frame = struct.pack('>I', len(n)) + n
        h = hdr(len(frame) + 16, 0)
        stream += h + aead.encrypt(b'\0\0\0\0' + struct.pack('<Q', ctr), frame, h); ctr += 1
print('VIDEO ->', ' '.join(ask('V ' + stream.hex())), '| sent', ctr, 'frames')
# a stream restart: a second config, and the phone's counter back at 0
ctr2 = 0; s2 = hdr(len(avcc) + 8, 1) + b'\0\0\0\0avcC' + avcc
for n in nals:
    if n[0] & 31 in (1, 5):
        frame = struct.pack('>I', len(n)) + n; h = hdr(len(frame) + 16, 0)
        s2 += h + aead.encrypt(b'\0\0\0\0' + struct.pack('<Q', ctr2), frame, h); ctr2 += 1
print('RESTART ->', ' '.join(ask('V ' + s2.hex())), '| total sent', ctr + ctr2)
