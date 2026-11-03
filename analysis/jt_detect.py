"""NBA 2K18 jump-table detector.

Finds   lis rT,hi / addi rT,rT,lo   (any order, interleaved)
        rlwinm rS,rX,2,0,29         (index * 4)
        lwzx rM,rT,rS  (or rS,rT)
        mtctr rM
        bctr
bounded by an earlier  cmplwi crN,rX,N ; bgt crN,...  giving N+1 entries.
Also handles byte/halfword-indexed (computed/offset) tables if present:
        lbzx/lhzx rS,rT,rX ; rlwinm ; lis/addi rB ; add rM,rB,rS ; mtctr rM ; bctr

Every target must land inside the same .pdata function as the bctr.
Output: XenonRecomp switch-table TOML (addresses only).
"""
import struct, sys, bisect, collections

IMG, OUT = sys.argv[1], sys.argv[2]
BASE = 0x82000000
TEXT_START, TEXT_SIZE = 0x82230000, 0x00D80E6C
PDATA_START, PDATA_SIZE = 0x821CAC00, 0x0004E6B0
data = open(IMG, 'rb').read()

def word(a): o = a - BASE; return struct.unpack('>I', data[o:o+4])[0]
def half(a): o = a - BASE; return struct.unpack('>H', data[o:o+2])[0]
def byte(a): return data[a - BASE]
def s16(w): v = w & 0xFFFF; return v - 0x10000 if v & 0x8000 else v

# ---- function bounds from .pdata ----
funcs = []
for off in range(0, PDATA_SIZE - 7, 8):
    begin, info = struct.unpack('>II', data[PDATA_START - BASE + off: PDATA_START - BASE + off + 8])
    if begin == 0: continue
    length = ((info >> 8) & 0x3FFFFF) * 4
    if TEXT_START <= begin < TEXT_START + TEXT_SIZE and length:
        funcs.append((begin, begin + length))
funcs.sort()
starts = [f[0] for f in funcs]
def func_of(a):
    """the .pdata function containing a, or else the gap between two .pdata
    functions (leaf/library code without unwind info)"""
    i = bisect.bisect_right(starts, a) - 1
    if i >= 0 and funcs[i][0] <= a < funcs[i][1]: return funcs[i]
    gap_lo = funcs[i][1] if i >= 0 else TEXT_START
    gap_hi = funcs[i + 1][0] if i + 1 < len(funcs) else TEXT_START + TEXT_SIZE
    return (gap_lo, gap_hi)

# ---- tiny decoder for the instructions we care about ----
def dec(w):
    op = w >> 26; rD, rA, rB = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
    if op == 15 and rA == 0: return ('lis', rD, s16(w))
    if op == 14 and rA != 0: return ('addi', rD, rA, s16(w))
    if op == 21: return ('rlwinm', rA, rD, rB, (w >> 6) & 31, (w >> 1) & 31)
    if op == 10: return ('cmplwi', (w >> 23) & 7, rA, w & 0xFFFF)
    if op == 16: return ('bc', rD, rA)
    if op == 31:
        xo = (w >> 1) & 0x3FF
        if xo == 23: return ('lwzx', rD, rA, rB)
        if xo == 87: return ('lbzx', rD, rA, rB)
        if xo == 279: return ('lhzx', rD, rA, rB)
        if xo == 266: return ('add', rD, rA, rB)
        if xo == 467 and (((w >> 16) & 31) | (((w >> 11) & 31) << 5)) == 9: return ('mtctr', rD)
    return ('?',)

def writes(d):
    """register written by a decoded instruction, if we know it"""
    if d[0] in ('lis', 'addi', 'lwzx', 'lbzx', 'lhzx', 'add'): return d[1]
    if d[0] == 'rlwinm': return d[1]
    return None

LOOKBACK = 24

def find_addr_of(reg, start, lo_limit):
    """walk back from `start` to resolve reg = lis/addi constant. Returns value or None."""
    lo = None
    a = start
    while a >= lo_limit:
        d = dec(word(a))
        if d[0] == 'addi' and d[1] == reg and d[2] == reg and lo is None:
            lo = d[3]
        elif d[0] == 'lis' and d[1] == reg:
            return ((d[2] << 16) + (lo or 0)) & 0xFFFFFFFF
        elif writes(d) == reg:
            return None
        a -= 4
    return None

def find_bound(reg, start, lo_limit):
    """walk back for cmplwi crN,reg,N followed (somewhere after) by a bc on crN."""
    a = start
    while a >= lo_limit:
        d = dec(word(a))
        if d[0] == 'cmplwi' and d[2] == reg:
            return d[3] + 1
        if writes(d) == reg:
            return None
        a -= 4
    return None

def find_def(reg, start, lo_limit, kinds):
    a = start
    while a >= lo_limit:
        d = dec(word(a))
        if writes(d) == reg:
            return (a, d) if d[0] in kinds else (a, None)
        a -= 4
    return (None, None)

results, reasons = [], collections.Counter()
for off in range(0, TEXT_SIZE - 4, 4):
    bctr = TEXT_START + off
    if word(bctr) != 0x4E800420: continue
    f = func_of(bctr)
    if not f: reasons['no pdata function'] += 1; continue
    lim = max(f[0], bctr - 4 * LOOKBACK)
    mt = dec(word(bctr - 4))
    if mt[0] != 'mtctr': reasons['no mtctr before bctr'] += 1; continue
    rM = mt[1]
    a_def, d = find_def(rM, bctr - 8, lim, ('lwzx', 'add'))
    if d is None: reasons['ctr not from table load (indirect call)'] += 1; continue

    kind, index_reg, labels, table_base = None, None, None, None
    if d[0] == 'lwzx':
        # one of rA/rB is the table base (lis/addi), the other is index*4 (rlwinm)
        for rT, rS in ((d[2], d[3]), (d[3], d[2])):
            base = find_addr_of(rT, a_def - 4, lim)
            if base is None: continue
            _, sh = find_def(rS, a_def - 4, lim, ('rlwinm',))
            if sh is None or sh[3] != 2 or sh[5] != 29: continue
            index_reg, table_base = sh[2], base
            count = find_bound(index_reg, a_def - 4, max(f[0], bctr - 4 * 48))
            if count and count <= 4096:
                labels = [word(base + 4 * i) for i in range(count)]
                kind = 'absolute'
            else:
                # No bounds check (compiler knew all values are covered):
                # take entries while they are code addresses inside this function.
                labels = []
                while len(labels) < 4096:
                    t = word(base + 4 * len(labels))
                    if not (f[0] <= t < f[1] and t % 4 == 0): break
                    labels.append(t)
                if len(labels) < 2: labels = None; continue
                kind = 'absolute-unbounded'
            break
    elif d[0] == 'add':
        # rM = base + rS, rS = (byte/half table)[rX] << 2 (or not shifted)
        for rB_, rS in ((d[2], d[3]), (d[3], d[2])):
            code_base = find_addr_of(rB_, a_def - 4, lim)
            if code_base is None: continue
            a2, sh = find_def(rS, a_def - 4, lim, ('rlwinm', 'lbzx', 'lhzx'))
            shift, src = 0, sh
            if sh and sh[0] == 'rlwinm':
                if sh[5] != 31 - sh[3] and not (sh[4] == 0): pass
                shift = sh[3]
                a2, src = find_def(sh[2], a2 - 4, lim, ('lbzx', 'lhzx'))
            if not src: continue
            for rT, rX in ((src[2], src[3]), (src[3], src[2])):
                tbase = find_addr_of(rT, a2 - 4, lim)
                if tbase is None: continue
                count = find_bound(rX, a2 - 4, max(f[0], bctr - 4 * 48))
                if not count or count > 4096: continue
                rd = byte if src[0] == 'lbzx' else half
                step = 1 if src[0] == 'lbzx' else 2
                labels = [(code_base + (rd(tbase + step * i) << shift)) & 0xFFFFFFFF for i in range(count)]
                index_reg, kind, table_base = rX, ('computed' if shift else 'offset'), tbase
                break
            if labels: break

    if not labels: reasons['pattern not recognised'] += 1; continue
    if not all(f[0] <= t < f[1] and t % 4 == 0 for t in labels):
        reasons['target outside its function'] += 1; continue
    results.append((bctr, index_reg, labels, kind, table_base))
    reasons['table: ' + kind] += 1

# Unbounded tables: stop where the next table in memory begins.
all_bases = sorted({r[4] for r in results})
trimmed = []
for bctr, r, labels, kind, tb in results:
    if kind == 'absolute-unbounded':
        i = bisect.bisect_right(all_bases, tb)
        if i < len(all_bases):
            limit = (all_bases[i] - tb) // 4
            if limit < len(labels):
                labels = labels[:limit]
                reasons['unbounded tables trimmed at next table'] += 1
        if len(labels) < 2:
            reasons['table: absolute-unbounded'] -= 1; reasons['dropped (too short after trim)'] += 1
            continue
    trimmed.append((bctr, r, labels, kind, tb))
results = trimmed

with open(OUT, 'w') as fo:
    fo.write('# NBA 2K18 jump tables, generated by analysis/jt_detect.py\n')
    fo.write('# Addresses only. Valid for default.xex SHA-256 27B6EF7D...CA8BA5A\n\n')
    for bctr, r, labels, kind, _ in results:
        fo.write(f'[[switch]]\n# {kind}, {len(labels)} cases\nbase = 0x{bctr:08X}\nr = {r}\nlabels = [\n')
        for i in range(0, len(labels), 6):
            fo.write('    ' + ', '.join(f'0x{t:08X}' for t in labels[i:i+6]) + ',\n')
        fo.write(']\n\n')

print('functions in .pdata:', len(funcs))
for k, v in sorted(reasons.items(), key=lambda kv: -kv[1]): print(f'{v:6d}  {k}')
print('cases total:', sum(len(r[2]) for r in results))
