import hashlib, os, subprocess
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
p = subprocess.Popen(['./hashes_host'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
def ask(s): p.stdin.write(s + '\n'); p.stdin.flush(); return p.stdout.readline().strip()
bad = 0; n = 0
for L in list(range(0, 140)) + [200, 511, 1000]:
    m = os.urandom(L); hx = m.hex() or '-'
    bad += ask(f'1 {hx}') != hashlib.sha1(m).hexdigest(); bad += ask(f'2 {hx}') != hashlib.sha256(m).hexdigest(); n += 2
for L in [0, 1, 15, 16, 17, 64, 100, 300]:
    k, iv, m = os.urandom(16), os.urandom(16), os.urandom(L)
    if L == 300: iv = b'\xff' * 16                       # counter wrap
    e = Cipher(algorithms.AES(k), modes.CTR(iv)).encryptor(); want = (e.update(m) + e.finalize()).hex()
    bad += ask(f'A {k.hex()} {iv.hex()} {m.hex() or "-"}') != want; n += 1
print(f'{n - bad}/{n} OK')
