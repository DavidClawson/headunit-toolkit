"""Independent SRP-6a client (Python ints + hashlib) against srp_host."""
import hashlib, os, subprocess, sys
N = int(open(sys.argv[1]).read(), 16); g = 5; L = 384
H = lambda *p: hashlib.sha512(b''.join(p)).digest()
i2b = lambda x, n=None: x.to_bytes(n or max(1, (x.bit_length() + 7) // 8), 'big')
I, P = b'Pair-Setup', b'3939'
bad = 0
for trial in range(20):
    salt, bsec, a = os.urandom(16), os.urandom(32), int.from_bytes(os.urandom(32), 'big')
    if trial == 0: bsec = b'\x00' * 31 + b'\x01'
    B = int(subprocess.run(['./srp_host', 'B', salt.hex(), bsec.hex()], capture_output=True, text=True).stdout, 16)
    k = int.from_bytes(H(i2b(N), i2b(g, L)), 'big')
    x = int.from_bytes(H(salt, H(I + b':' + P)), 'big'); v = pow(g, x, N)
    b = int.from_bytes(bsec, 'big')
    assert B == (k * v + pow(g, b, N)) % N, "B mismatch"
    A = pow(g, a, N)
    u = int.from_bytes(H(i2b(A, L), i2b(B, L)), 'big')
    S = pow(B - k * pow(g, x, N), a + u * x, N)            # client side
    K = H(i2b(S))
    hx = bytes(p ^ q for p, q in zip(H(i2b(N)), H(i2b(g))))
    M1 = H(hx, H(I), salt, i2b(A), i2b(B), K)
    M2 = H(i2b(A), M1, K)
    out = subprocess.run(['./srp_host', 'F', salt.hex(), bsec.hex(), i2b(A).hex(), M1.hex()], capture_output=True, text=True).stdout.split()
    ok = out[0] == '0' and out[1] == K.hex() and out[2] == M2.hex()
    # a wrong proof must be rejected
    wrong = subprocess.run(['./srp_host', 'F', salt.hex(), bsec.hex(), i2b(A).hex(), bytes(64).hex()], capture_output=True, text=True).stdout.split()[0]
    if not ok or wrong != '-2': bad += 1; print('trial', trial, 'FAIL', out[0], wrong)
print(f'{20 - bad}/20 trials OK')
