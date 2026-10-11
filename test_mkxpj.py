#!/usr/bin/env python3
"""Test helper (host_test.cpp): a copy of a project whose StepRunner-8 tracks hold the given chunk, stored the way MPC
stores a plugin's state: "state": "<size>.<JUCE base64>" of a VST2 bank (CcnK / FBCh, the chunk from byte 160, its
size big-endian at 156). Usage: test_mkxpj.py in.xpj chunkfile out.xpj"""
import gzip, re, struct, sys
T = ".ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+"
def dec(s):
    n, body = s.split('.', 1); b = bytearray(int(n)); pos = 0
    for ch in body:
        v = T.index(ch)
        for k in range(6):
            if v >> k & 1 and (pos + k) // 8 < len(b): b[(pos + k) // 8] |= 1 << ((pos + k) % 8)
        pos += 6
    return bytes(b)
def enc(b):
    out = []; nbits = len(b) * 8
    for pos in range(0, nbits, 6):
        v = 0
        for k in range(6):
            if pos + k < nbits and b[(pos + k) // 8] >> ((pos + k) % 8) & 1: v |= 1 << k
        out.append(T[v])
    return "%d.%s" % (len(b), ''.join(out))
src, chunkf, dst = sys.argv[1:4]
text = gzip.open(src).read().decode('latin-1')
chunk = open(chunkf, 'rb').read().split(b'\0')[0] + b'\0'
def fix(m):
    if '"uid": 1397894456' not in m.group(0): return m.group(0)   # 'SR-8'
    st = re.search(r'"state": "([^"]*)"', m.group(0))
    old = dec(st.group(1)); assert old[:4] == b'CcnK' and old[8:12] == b'FBCh'
    head = bytearray(old[:160]); head[16:20] = b'SR-8'; bank = bytes(head[:156]) + struct.pack('>I', len(chunk)) + chunk
    return m.group(0).replace(st.group(1), enc(bank))
text = re.sub(r'"uid": 1397894456.{0,4000}?"state": "[^"]*"', fix, text, flags=re.S)
with gzip.open(dst, 'wb') as f: f.write(text.encode('latin-1'))
