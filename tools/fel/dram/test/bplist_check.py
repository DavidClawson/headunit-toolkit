import plistlib, subprocess, random, string
p = subprocess.Popen(['./bplist_host'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
def ask(s): p.stdin.write(s + '\n'); p.stdin.flush(); return p.stdout.readline().strip()
built = plistlib.loads(bytes.fromhex(ask('B')))
want = {'eventPort': 51234, 'timingPort': 0, 'big': 5000000000, 'neg': -7, 'ok': True, 'blob': b'\x01\x02\x03\x00\xff', 'streams': [{'type': 110, 'dataPort': 7100, 'rate': 59.94}]}
print('build:', 'OK' if built == want else f'MISMATCH {built}')
rnd = random.Random(1)
def val(d):
    k = rnd.randrange(8 if d < 3 else 5)
    if k == 0: return rnd.randrange(-2**40, 2**40)
    if k == 1: return rnd.choice([True, False])
    if k == 2: return ''.join(rnd.choice(string.ascii_letters) for _ in range(rnd.randrange(0, 40)))
    if k == 3: return bytes(rnd.randrange(256) for _ in range(rnd.randrange(0, 300)))
    if k == 4: return rnd.random() * 1e6
    if k == 5: return [val(d + 1) for _ in range(rnd.randrange(0, 20))]
    return {''.join(rnd.choice(string.ascii_lowercase) for _ in range(rnd.randrange(1, 12))): val(d + 1) for _ in range(rnd.randrange(0, 18))}
ok = 0
for i in range(200):
    obj = {'k%d' % j: val(0) for j in range(rnd.randrange(1, 10))}
    b = plistlib.dumps(obj, fmt=plistlib.FMT_BINARY)
    r = ask('R ' + b.hex())
    if r != 'ERR' and plistlib.loads(bytes.fromhex(r)) == obj: ok += 1
    elif i < 3: print('fail', i, r[:60])
print(f'round trip: {ok}/200')
# hostile input must not crash
for i in range(300):
    b = bytearray(plistlib.dumps({'a': [1, 'x', b'yy', {'z': 2.5}]}, fmt=plistlib.FMT_BINARY))
    for _ in range(4): b[rnd.randrange(len(b))] = rnd.randrange(256)
    ask('R ' + b.hex())
print('fuzz: survived', p.poll() is None)
