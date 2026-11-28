"""Independent HomeKit-style controller (phone side) against pair_host."""
import hashlib, os, subprocess, sys
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.hazmat.primitives import hashes, serialization
N = int(open(sys.argv[1]).read(), 16); g = 5; L = 384
H = lambda *p: hashlib.sha512(b''.join(p)).digest()
i2b = lambda x, n=None: x.to_bytes(n or max(1, (x.bit_length() + 7) // 8), 'big')
raw = lambda k: k.public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
def hkdf(ikm, salt, info): return HKDF(hashes.SHA512(), 32, salt.encode(), info.encode()).derive(ikm)
def tlv(*items):
    o = b''
    for t, v in items:
        while True:
            o += bytes([t, min(len(v), 255)]) + v[:255]; v = v[255:]
            if not v: break
    return o
def untlv(b):
    d, i, last = {}, 0, None
    while i < len(b):
        t, l = b[i], b[i+1]; v = b[i+2:i+2+l]
        d[t] = d.get(t, b'') + v if t == last else v; last = t; i += 2 + l
    return d
nonce = lambda tag: b'\0\0\0\0' + tag
class Acc:
    def __init__(s, pin):
        s.p = subprocess.Popen(['./pair_host', pin], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        s.ltpk = bytes.fromhex(s.p.stdout.readline().strip())
    def ask(s, kind, body):
        s.p.stdin.write(f'{kind} {body.hex()}\n'); s.p.stdin.flush()
        r = s.p.stdout.readline().split()
        if r[0] == 'ERR': return None, int(r[1]), 0
        return untlv(bytes.fromhex(r[0])), int(r[1]), int(r[2])

def pair_setup(acc, pin, ctrl_key, ctrl_id):
    r, _, _ = acc.ask('S', tlv((6, b'\x01'), (0, b'\x00')))
    assert r[6] == b'\x02' and len(r[2]) == 16
    salt, B = r[2], int.from_bytes(r[3], 'big')
    a = int.from_bytes(os.urandom(32), 'big'); A = pow(g, a, N)
    k = int.from_bytes(H(i2b(N), i2b(g, L)), 'big'); I = b'Pair-Setup'
    x = int.from_bytes(H(salt, H(I + b':' + pin.encode())), 'big')
    u = int.from_bytes(H(i2b(A, L), i2b(B, L)), 'big')
    K = H(i2b(pow(B - k * pow(g, x, N), a + u * x, N)))
    hx = bytes(p ^ q for p, q in zip(H(i2b(N)), H(i2b(g))))
    M1 = H(hx, H(I), salt, i2b(A), i2b(B), K)
    r, err, _ = acc.ask('S', tlv((6, b'\x03'), (3, i2b(A)), (4, M1)))
    if 7 in r: return 'M4 error %d' % err
    assert r[4] == H(i2b(A), M1, K), 'M2 proof wrong'
    key = hkdf(K, 'Pair-Setup-Encrypt-Salt', 'Pair-Setup-Encrypt-Info')
    X = hkdf(K, 'Pair-Setup-Controller-Sign-Salt', 'Pair-Setup-Controller-Sign-Info')
    pk = raw(ctrl_key.public_key())
    sub = tlv((1, ctrl_id), (3, pk), (10, ctrl_key.sign(X + ctrl_id + pk)))
    r, err, _ = acc.ask('S', tlv((6, b'\x05'), (5, ChaCha20Poly1305(key).encrypt(nonce(b'PS-Msg05'), sub, None))))
    if 7 in r: return 'M6 error %d' % err
    d = untlv(ChaCha20Poly1305(key).decrypt(nonce(b'PS-Msg06'), r[5], None))
    AX = hkdf(K, 'Pair-Setup-Accessory-Sign-Salt', 'Pair-Setup-Accessory-Sign-Info')
    assert d[3] == acc.ltpk
    Ed25519PublicKey.from_public_bytes(d[3]).verify(d[10], AX + d[1] + d[3])
    return 'ok id=' + d[1].decode()

def pair_verify(acc, ctrl_key, ctrl_id):
    e = X25519PrivateKey.generate(); epub = raw(e.public_key())
    r, err, _ = acc.ask('V', tlv((6, b'\x01'), (3, epub)))
    apub = r[3]; shared = e.exchange(X25519PublicKey.from_public_bytes(apub))
    key = hkdf(shared, 'Pair-Verify-Encrypt-Salt', 'Pair-Verify-Encrypt-Info')
    d = untlv(ChaCha20Poly1305(key).decrypt(nonce(b'PV-Msg02'), r[5], None))
    Ed25519PublicKey.from_public_bytes(acc.ltpk).verify(d[10], apub + d[1] + epub)
    sub = tlv((1, ctrl_id), (10, ctrl_key.sign(epub + ctrl_id + apub)))
    r, err, ver = acc.ask('V', tlv((6, b'\x03'), (5, ChaCha20Poly1305(key).encrypt(nonce(b'PV-Msg03'), sub, None))))
    return 'ok' if (r.get(6) == b'\x04' and 7 not in r and ver == 1) else 'fail err %d' % err

ck = Ed25519PrivateKey.generate(); cid = b'5F1C2A90-0000-4000-8000-0123456789AB'
acc = Acc('3939')
print('pair-setup (PIN 3939):', pair_setup(acc, '3939', ck, cid))
print('pair-verify:', pair_verify(acc, ck, cid))
print('pair-verify again:', pair_verify(acc, ck, cid))
print('pair-verify from an unknown phone:', pair_verify(acc, Ed25519PrivateKey.generate(), b'someone-else'))
print('pair-setup with wrong PIN:', pair_setup(Acc('3939'), '1234', ck, cid))
