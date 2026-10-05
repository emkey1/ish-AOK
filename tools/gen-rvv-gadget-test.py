#!/usr/bin/env python3
"""Generate tests/manual/riscv64/riscv64_rvv_gadgets.c.

AOK's typed vector gadgets (jit/guest-riscv64/vector.S, chosen in jit/gen.c
gen_riscv64_vector) are emitted for the vtype a vsetvli earlier in the block
set; after a vsetvl (register vtype) that is unknown, a guess is emitted and
the gadget re-dispatches to the SEW in force at run time. Each case runs one
instruction both ways -- typed, and re-dispatched -- from the same random
register file (v0 a random mask) and checks v0, v8-v31 and any scalar result
against a model of the instruction written here from the spec, independent
of the C core (jit/riscv64_vector.c; that one is checked against gcc's
scalar code by riscv64_rvv.c).

Every case runs under each SEW/LMUL its encoding allows, at vl 0, VLMAX and
random values past it, unmasked and (where it has the encoding) masked, so
the gadgets' tails, partial chunks, masks and register groups are reached.

    python3 tools/gen-rvv-gadget-test.py > tests/manual/riscv64/riscv64_rvv_gadgets.c
"""

SEWS = [8, 16, 32, 64]
LMULS = ['mf2', 'm1', 'm2', 'm4', 'm8']


def lmul8(l):
    return {'mf8': 1, 'mf4': 2, 'mf2': 4, 'm1': 8, 'm2': 16, 'm4': 32, 'm8': 64}[l]


def vtypes():
    for sew in SEWS:
        for l in LMULS:
            if l == 'mf2' and sew > 32:  # SEW <= ELEN * LMUL
                continue
            yield sew, l


def vlmax(sew, l):
    return 128 * lmul8(l) // 8 // sew


def vtcode(sew, l):  # tu, mu
    return {8: 0, 16: 1, 32: 2, 64: 3}[sew] << 3 | {'mf2': 7, 'm1': 0, 'm2': 1, 'm4': 2, 'm8': 3}[l]


IMMS = [-16, -1, 0, 1, 5, 15]
UIMMS = [0, 1, 3, 7, 13, 31]
UIMM6 = [0, 1, 7, 13, 31, 45]

# model kinds and ops (the C model's enums below)
(K_BIN, K_CMP, K_RED, K_VID, K_EXT, K_MSF, K_CPOP, K_FIRST, K_MVXS, K_MVSX, K_LX, K_W,
 K_SLIDE, K_GATHER, K_COMPRESS, K_IOTA, K_CARRY, K_UNARY, K_FB, K_FC, K_FMVFS, K_FMVSF,
 K_FCVT, K_FUN1, K_FRED, K_FW) = range(26)
BINOPS = ['add', 'sub', 'rsub', 'and', 'or', 'xor', 'andn', 'mv', 'minu', 'min', 'maxu', 'max',
          'sll', 'srl', 'sra', 'ror', 'rol', 'mul', 'macc', 'nmsac', 'madd', 'nmsub', 'merge',
          'divu', 'div', 'remu', 'rem', 'mulhu', 'mulhsu', 'mulh']
CMPOPS = ['seq', 'sne', 'sltu', 'slt', 'sleu', 'sle', 'sgtu', 'sgt']
REDOPS = ['sum', 'and', 'or', 'xor', 'minu', 'min', 'maxu', 'max']
FORMS = {'vv': 0, 'vx': 1, 'vi': 2}

# (name, text, model fields, needs) -- operands v8 (vd), v16 (vs2), v24 (vs1),
# %[x] (rs1); model fields: kind, op, form, masked, vd, vs2, vs1, imm, aux, sign
cases = []


def add(name, text, kind, op=0, form=0, vd=8, vs2=16, vs1=24, imm=0, aux=0, sign=0,
        ok=lambda sew, l: True, masked=True, xmask='~0ull'):
    cases.append((name, text, (kind, op, form, 0, vd, vs2, vs1, imm, aux, sign, xmask), ok))
    if masked:
        cases.append((name + ' v0.t', text + ', v0.t', (kind, op, form, 1, vd, vs2, vs1, imm, aux, sign, xmask), ok))


imm_i = 0


def nextimm(lst):
    global imm_i
    imm_i += 1
    return lst[imm_i % 6]


binops = {
    'vadd': 'vv vx vi', 'vsub': 'vv vx', 'vrsub': 'vx vi',
    'vand': 'vv vx vi', 'vor': 'vv vx vi', 'vxor': 'vv vx vi', 'vandn': 'vv vx',
    'vminu': 'vv vx', 'vmin': 'vv vx', 'vmaxu': 'vv vx', 'vmax': 'vv vx',
    'vsll': 'vv vx vi', 'vsrl': 'vv vx vi', 'vsra': 'vv vx vi', 'vror': 'vv vx vi', 'vrol': 'vv vx',
}
for op, forms in binops.items():
    for f in forms.split():
        lst = UIMM6 if op == 'vror' else UIMMS if op in ('vsll', 'vsrl', 'vsra') else IMMS
        for k in range(2 if f == 'vi' else 1):
            imm = nextimm(lst) if f == 'vi' else 0
            src = {'vv': 'v24', 'vx': '%[x]', 'vi': str(imm)}[f]
            add(f'{op}.{f} {src}', f'{op}.{f} v8, v16, {src}', K_BIN, BINOPS.index(op[1:]), FORMS[f], imm=imm)
add('vadd.vv vd=vs2', 'vadd.vv v16, v16, v24', K_BIN, 0, 0, vd=16)
for op in ('mul', 'divu', 'div', 'remu', 'rem', 'mulhu', 'mulhsu', 'mulh'):
    add(f'v{op}.vv', f'v{op}.vv v8, v16, v24', K_BIN, BINOPS.index(op), 0)
    add(f'v{op}.vx', f'v{op}.vx v8, v16, %[x]', K_BIN, BINOPS.index(op), 1)
for op in ('macc', 'nmsac', 'madd', 'nmsub'):  # vd, vs1, vs2
    add(f'v{op}.vv', f'v{op}.vv v8, v24, v16', K_BIN, BINOPS.index(op), 0)
    add(f'v{op}.vx', f'v{op}.vx v8, %[x], v16', K_BIN, BINOPS.index(op), 1)
add('vmv.v.v', 'vmv.v.v v8, v24', K_BIN, BINOPS.index('mv'), 0, masked=False)
add('vmv.v.x', 'vmv.v.x v8, %[x]', K_BIN, BINOPS.index('mv'), 1, masked=False)
for i in (-16, 3):
    add(f'vmv.v.i {i}', f'vmv.v.i v8, {i}', K_BIN, BINOPS.index('mv'), 2, imm=i, masked=False)
mg = BINOPS.index('merge')
add('vmerge.vvm', 'vmerge.vvm v8, v16, v24, v0', K_BIN, mg, 0, masked=False)
add('vmerge.vxm', 'vmerge.vxm v8, v16, %[x], v0', K_BIN, mg, 1, masked=False)
for i in (-1, 1):
    add(f'vmerge.vim {i}', f'vmerge.vim v8, v16, {i}, v0', K_BIN, mg, 2, imm=i, masked=False)

cmps = {'vmseq': 'vv vx vi', 'vmsne': 'vv vx vi', 'vmsltu': 'vv vx', 'vmslt': 'vv vx',
        'vmsleu': 'vv vx vi', 'vmsle': 'vv vx vi', 'vmsgtu': 'vx vi', 'vmsgt': 'vx vi'}
for op, forms in cmps.items():
    for f in forms.split():
        for k in range(2 if f == 'vi' else 1):
            imm = nextimm(IMMS) if f == 'vi' else 0
            src = {'vv': 'v24', 'vx': '%[x]', 'vi': str(imm)}[f]
            add(f'{op}.{f} {src}', f'{op}.{f} v8, v16, {src}', K_CMP, CMPOPS.index(op[2:]), FORMS[f], imm=imm)
add('vmseq.vv vd=vs2', 'vmseq.vv v16, v16, v24', K_CMP, 0, 0, vd=16)
add('vmseq.vv vd=v0', 'vmseq.vv v0, v16, v24', K_CMP, 0, 0, vd=0)

for op in REDOPS:
    add(f'vred{op}.vs', f'vred{op}.vs v8, v16, v24', K_RED, REDOPS.index(op))
add('vredsum.vs vd=vs2', 'vredsum.vs v16, v16, v24', K_RED, 0, vd=16)

# widening and narrowing: model op 0 add, 1 sub, 2 mul, 3 macc, 4 srl, 5 sra, 6 sll;
# aux = vs2 wide | vd wide << 1, sign = vs2 signed | vs1/scalar signed << 1
WIDE_OK = lambda sew, l: sew <= 32 and lmul8(l) < 64
WOPS = [  # mnemonic, model op, vs2 wide, vs2 signed, vs1 signed, vd wide, forms (vs1 forms named per op)
    ('vwaddu', 0, 0, 0, 0, 1, 'vv vx'), ('vwadd', 0, 0, 1, 1, 1, 'vv vx'),
    ('vwsubu', 1, 0, 0, 0, 1, 'vv vx'), ('vwsub', 1, 0, 1, 1, 1, 'vv vx'),
    ('vwaddu', 0, 1, 0, 0, 1, 'wv wx'), ('vwadd', 0, 1, 1, 1, 1, 'wv wx'),
    ('vwsubu', 1, 1, 0, 0, 1, 'wv wx'), ('vwsub', 1, 1, 1, 1, 1, 'wv wx'),
    ('vwmulu', 2, 0, 0, 0, 1, 'vv vx'), ('vwmulsu', 2, 0, 1, 0, 1, 'vv vx'), ('vwmul', 2, 0, 1, 1, 1, 'vv vx'),
    ('vwmaccu', 3, 0, 0, 0, 1, 'vv vx'), ('vwmacc', 3, 0, 1, 1, 1, 'vv vx'),
    ('vwmaccus', 3, 0, 1, 0, 1, 'vx'), ('vwmaccsu', 3, 0, 0, 1, 1, 'vv vx'),
    ('vnsrl', 4, 1, 0, 0, 0, 'wv wx wi'), ('vnsra', 5, 1, 1, 0, 0, 'wv wx wi'),
    ('vwsll', 6, 0, 0, 0, 1, 'vv vx vi'),
]
for mn, op, aw, asg, bsg, dw, forms in WOPS:
    for f in forms.split():
        form = {'v': 0, 'x': 1, 'i': 2}[f[1]]
        imm = nextimm(UIMMS) if form == 2 else 0
        src = {0: 'v24', 1: '%[x]', 2: str(imm)}[form]
        if op == 3:  # vd, vs1/rs1, vs2
            text = f'{mn}.{f} v8, {src}, v16'
        else:
            text = f'{mn}.{f} v8, v16, {src}'
        add(f'{mn}.{f} {src}', text, K_W, op, form, imm=imm, aux=aw | dw << 1, sign=asg | bsg << 1, ok=WIDE_OK)

# permutations: K_SLIDE op 0 up, 1 down, 2 1up, 3 1down (aux 1: an f
# register, 2: one boxed by fmv.w.x); K_GATHER aux = index bytes (0: SEW)
FP_OK = lambda sew, l: sew >= 32
for op, mn in ((0, 'vslideup'), (1, 'vslidedown')):
    add(f'{mn}.vx', f'{mn}.vx v8, v16, %[x]', K_SLIDE, op, 1, xmask='0x3f')
    add(f'{mn}.vx big', f'{mn}.vx v8, v16, %[x]', K_SLIDE, op, 1)
    for imm in (0, 3, 31):
        add(f'{mn}.vi {imm}', f'{mn}.vi v8, v16, {imm}', K_SLIDE, op, 2, imm=imm)
add('vslideup.vi vd=vs2', 'vslideup.vi v16, v16, 2', K_SLIDE, 0, 2, vd=16, imm=2)
add('vslidedown.vi vd=vs2', 'vslidedown.vi v16, v16, 2', K_SLIDE, 1, 2, vd=16, imm=2)
add('vslide1up.vx', 'vslide1up.vx v8, v16, %[x]', K_SLIDE, 2, 1)
add('vslide1down.vx', 'vslide1down.vx v8, v16, %[x]', K_SLIDE, 3, 1)
for op, mn in ((2, 'vfslide1up'), (3, 'vfslide1down')):
    add(f'{mn}.vf d', f'fmv.d.x ft0, %[x]\\n{mn}.vf v8, v16, ft0', K_SLIDE, op, 1, aux=1, ok=FP_OK)
    add(f'{mn}.vf w', f'fmv.w.x ft0, %[x]\\n{mn}.vf v8, v16, ft0', K_SLIDE, op, 1, aux=2,
        ok=lambda sew, l: sew == 32)
add('vrgather.vv', 'vrgather.vv v8, v16, v24', K_GATHER, 0, 0)
add('vrgather.vx', 'vrgather.vx v8, v16, %[x]', K_GATHER, 0, 1, xmask='0xff')
add('vrgather.vx big', 'vrgather.vx v8, v16, %[x]', K_GATHER, 0, 1)
for imm in (0, 5, 31):
    add(f'vrgather.vi {imm}', f'vrgather.vi v8, v16, {imm}', K_GATHER, 0, 2, imm=imm)
add('vrgatherei16.vv', 'vrgatherei16.vv v8, v16, v24', K_GATHER, 0, 0, aux=2,
    ok=lambda sew, l: lmul8(l) * 16 // sew <= 64 and lmul8(l) * 16 >= sew)
add('vcompress.vm', 'vcompress.vm v8, v16, v24', K_COMPRESS, masked=False)
add('viota.m', 'viota.m v8, v16', K_IOTA)
# carries: K_CARRY op 0 add 1 sub, aux = mask out | carry in << 1
for op, mn, mmn in ((0, 'vadc', 'vmadc'), (1, 'vsbc', 'vmsbc')):
    forms = ('vv', 'vx', 'vi') if op == 0 else ('vv', 'vx')
    for f in forms:
        imm = -5 if f == 'vi' else 0
        src = {'vv': 'v24', 'vx': '%[x]', 'vi': '-5'}[f]
        fm = FORMS[f]
        add(f'{mn}.{f}m', f'{mn}.{f}m v8, v16, {src}, v0', K_CARRY, op, fm, imm=imm, aux=2, masked=False)
        add(f'{mmn}.{f}m', f'{mmn}.{f}m v8, v16, {src}, v0', K_CARRY, op, fm, imm=imm, aux=3, masked=False)
        add(f'{mmn}.{f}', f'{mmn}.{f} v8, v16, {src}', K_CARRY, op, fm, imm=imm, aux=1, masked=False)
for i, mn in enumerate(('vbrev8', 'vrev8', 'vbrev', 'vclz', 'vctz', 'vcpop')):
    add(f'{mn}.v', f'{mn}.v v8, v16', K_UNARY, i)

# floating point: SEW 32/64. The text clears fflags first and reads them
# into r last; aux 1: the f scalar from fmv.d.x (NaN-box checked at SEW 32),
# 2: from fmv.w.x (boxed)
FBOPS = ['add', 'sub', 'rsub', 'mul', 'div', 'rdiv', 'min', 'max', 'sgnj', 'sgnjn', 'sgnjx', 'mv', 'sqrt',
         'macc', 'nmacc', 'msac', 'nmsac', 'madd', 'nmadd', 'msub', 'nmsub', 'merge']
FMA = ('macc', 'nmacc', 'msac', 'nmsac', 'madd', 'nmadd', 'msub', 'nmsub')
def fpcase(name, body, kind, op, form, aux=0, masked=True, ok=FP_OK, vs1=24):
    wrap = lambda b: 'csrwi fflags, 0\\n' + b + '\\nfrflags %[r]'
    add(name, wrap(body), kind, op, form, aux=aux, masked=False, ok=ok, vs1=vs1)
    if masked:
        cases.append((name + ' v0.t', wrap(body + ', v0.t'), (kind, op, form, 1, 8, 16, vs1, 0, aux, 0, '~0ull'), ok))
for op in FBOPS[:-1]:
    i = FBOPS.index(op)
    if op == 'mv':
        fpcase('vfmv.v.f', 'fmv.d.x ft0, %[x]\\nvfmv.v.f v8, ft0', K_FB, i, 1, aux=1, masked=False)
        continue
    if op == 'sqrt':
        fpcase('vfsqrt.v', 'vfsqrt.v v8, v16', K_FB, i, 0)
        continue
    mn = 'vf' + op
    if op not in ('rsub', 'rdiv'):
        fpcase(f'{mn}.vv', f'{mn}.vv v8, v24, v16' if op in FMA else f'{mn}.vv v8, v16, v24', K_FB, i, 0)
    fpcase(f'{mn}.vf', f'fmv.d.x ft0, %[x]\\n' + (f'{mn}.vf v8, ft0, v16' if op in FMA else f'{mn}.vf v8, v16, ft0'),
           K_FB, i, 1, aux=1)
fpcase('vfadd.vf boxed', 'fmv.w.x ft0, %[x]\\nvfadd.vf v8, v16, ft0', K_FB, 0, 1, aux=2, ok=lambda sew, l: sew == 32)
fpcase('vfmerge.vfm', 'fmv.d.x ft0, %[x]\\nvfmerge.vfm v8, v16, ft0, v0', K_FB, FBOPS.index('merge'), 1, aux=1, masked=False)
FCOPS = ['eq', 'ne', 'lt', 'le', 'gt', 'ge']
for i, op in enumerate(('eq', 'ne', 'lt', 'le')):  # equal operands: the boundary of each compare
    fpcase(f'vmf{op}.vv same', f'vmf{op}.vv v8, v16, v16', K_FC, i, 0, vs1=16)
for i, op in enumerate(FCOPS):
    if op not in ('gt', 'ge'):
        fpcase(f'vmf{op}.vv', f'vmf{op}.vv v8, v16, v24', K_FC, i, 0)
    fpcase(f'vmf{op}.vf', f'fmv.d.x ft0, %[x]\\nvmf{op}.vf v8, v16, ft0', K_FC, i, 1, aux=1)
# conversions: K_FCVT, op = the VFUNARY0 vs1 code
WN = lambda sews: (lambda sew, l: sew in sews and lmul8(l) < 64)
CVTS = [(0, 'vfcvt.xu.f.v', FP_OK), (1, 'vfcvt.x.f.v', FP_OK), (2, 'vfcvt.f.xu.v', FP_OK),
        (3, 'vfcvt.f.x.v', FP_OK), (6, 'vfcvt.rtz.xu.f.v', FP_OK), (7, 'vfcvt.rtz.x.f.v', FP_OK),
        (8, 'vfwcvt.xu.f.v', WN((32,))), (9, 'vfwcvt.x.f.v', WN((32,))), (10, 'vfwcvt.f.xu.v', WN((16, 32))),
        (11, 'vfwcvt.f.x.v', WN((16, 32))), (12, 'vfwcvt.f.f.v', WN((16, 32))),
        (14, 'vfwcvt.rtz.xu.f.v', WN((32,))), (15, 'vfwcvt.rtz.x.f.v', WN((32,))),
        (16, 'vfncvt.xu.f.w', WN((16, 32))), (17, 'vfncvt.x.f.w', WN((16, 32))), (18, 'vfncvt.f.xu.w', WN((32,))),
        (19, 'vfncvt.f.x.w', WN((32,))), (20, 'vfncvt.f.f.w', WN((16, 32))), (21, 'vfncvt.rod.f.f.w', WN((32,))),
        (22, 'vfncvt.rtz.xu.f.w', WN((16, 32))), (23, 'vfncvt.rtz.x.f.w', WN((16, 32)))]
for code, mn, okf in CVTS:
    fpcase(mn, f'{mn} v8, v16', K_FCVT, code, 0, ok=okf)
for i, mn in enumerate(('vfclass.v', 'vfrsqrt7.v', 'vfrec7.v')):
    fpcase(mn, f'{mn} v8, v16', K_FUN1, i, 0)
for i, mn in enumerate(('vfredusum', 'vfredosum', 'vfredmin', 'vfredmax')):
    fpcase(f'{mn}.vs', f'{mn}.vs v8, v16, v24', K_FRED, i, 0)
for i, mn in ((4, 'vfwredusum'), (5, 'vfwredosum')):
    fpcase(f'{mn}.vs', f'{mn}.vs v8, v16, v24', K_FRED, i, 0, ok=lambda sew, l: sew == 32)
W32 = WN((32,))
for i, mn, forms in ((0, 'vfwadd', 'vv vf'), (1, 'vfwsub', 'vv vf'), (2, 'vfwadd', 'wv wf'), (3, 'vfwsub', 'wv wf'),
                     (4, 'vfwmul', 'vv vf'), (5, 'vfwmacc', 'vv vf'), (6, 'vfwnmacc', 'vv vf'),
                     (7, 'vfwmsac', 'vv vf'), (8, 'vfwnmsac', 'vv vf')):
    for f in forms.split():
        if f[1] == 'v':
            body = f'{mn}.{f} v8, v24, v16' if i >= 5 else f'{mn}.{f} v8, v16, v24'
            fpcase(f'{mn}.{f}', body, K_FW, i, 0, ok=W32)
        else:
            body = f'fmv.d.x ft0, %[x]\\n' + (f'{mn}.{f} v8, ft0, v16' if i >= 5 else f'{mn}.{f} v8, v16, ft0')
            fpcase(f'{mn}.{f}', body, K_FW, i, 1, aux=1, ok=W32)
add('vfmv.f.s', 'vfmv.f.s ft0, v16\\nfmv.x.d %[r], ft0', K_FMVFS, masked=False, ok=FP_OK)
add('vfmv.s.f', 'fmv.d.x ft0, %[x]\\nvfmv.s.f v8, ft0', K_FMVSF, aux=1, masked=False, ok=FP_OK)

add('vid.v', 'vid.v v8', K_VID)
for f in (2, 4, 8):
    for s_ in ('z', 's'):
        add(f'v{s_}ext.vf{f}', f'v{s_}ext.vf{f} v8, v16', K_EXT, aux=f, sign=int(s_ == 's'),
            ok=lambda sew, l, f=f: sew // f >= 8 and lmul8(l) >= f)
for i, op in enumerate(('vmsbf', 'vmsif', 'vmsof')):
    add(f'{op}.m', f'{op}.m v8, v16', K_MSF, i, masked=False)
add('vcpop.m', 'vcpop.m %[r], v16', K_CPOP)
add('vfirst.m', 'vfirst.m %[r], v16', K_FIRST)
add('vmv.x.s', 'vmv.x.s %[r], v16', K_MVXS, masked=False)
add('vmv.s.x', 'vmv.s.x v8, %[x]', K_MVSX, masked=False)
for isz in (8, 16, 32, 64):
    for o in ('u', 'o'):
        add(f'vl{o}xei{isz}', f'vl{o}xei{isz}.v v8, (%[mem]), v16', K_LX, aux=isz // 8,
            ok=lambda sew, l, isz=isz: 1 <= lmul8(l) * isz // sew <= 64 and lmul8(l) * isz >= sew)
for isz in (32, 64):
    add(f'vluxei{isz} vd=vs2', f'vluxei{isz}.v v16, (%[mem]), v16', K_LX, vd=16, aux=isz // 8,
        ok=lambda sew, l, isz=isz: sew == isz, masked=False)

def run(vset, text):
    return f'    RUNV("{vset}", "{text}");\n'


out = []
w = out.append
w('''// GENERATED by tools/gen-rvv-gadget-test.py -- edit the generator, not this.
// riscv64_rvv_gadgets.c -- every instruction AOK's typed vector gadgets take
// (jit/guest-riscv64/vector.S), run after a vsetvli (the gadget typed for it
// at translation) and after vsetvl (a guess there, re-dispatched at run time),
// against a model of the instruction written from the spec: v0, v8-v31 and
// any scalar result, under each SEW/LMUL, masked and not, at random vl.
//
//     gcc -O1 -o riscv64_rvv_gadgets riscv64_rvv_gadgets.c

#include <fenv.h>
#include <math.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define V __attribute__((target("arch=rv64gcv_zvbb"), noinline))
static uint8_t in[512] __attribute__((aligned(16))), ou[512], om[512];
static uint8_t mem[8192 + 64] __attribute__((aligned(4096)));
static unsigned long checks, bad;
static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

// the register file in from in[], the instruction, v0 and v8-v31 out to ou[]
#define VREGS "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10", "v11", "v12", \\
        "v13", "v14", "v15", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25", \\
        "v26", "v27", "v28", "v29", "v30", "v31"
#define RUNV(VSET, TEXT) asm volatile("li %[r], 0\\n" \\
        "vsetvli t0, zero, e8, m8, ta, ma\\n" \\
        "vle8.v v8, (%[i8])\\n" "vle8.v v16, (%[i16])\\n" "vle8.v v24, (%[i24])\\n" \\
        "vsetvli t0, zero, e8, m1, ta, ma\\n" "vle8.v v0, (%[i0])\\n" \\
        VSET "\\n" TEXT "\\n" \\
        "vsetvli t0, zero, e8, m8, ta, ma\\n" \\
        "vse8.v v8, (%[o8])\\n" "vse8.v v16, (%[o16])\\n" "vse8.v v24, (%[o24])\\n" \\
        "vsetvli t0, zero, e8, m1, ta, ma\\n" "vse8.v v0, (%[o0])\\n" \\
        : [r] "=&r"(ru) \\
        : [i0] "r"(in), [i8] "r"(in + 128), [i16] "r"(in + 256), [i24] "r"(in + 384), \\
          [avl] "r"(avl), [x] "r"(x), [mem] "r"(mem), [vt] "r"(vt), \\
          [o0] "r"(ou), [o8] "r"(ou + 128), [o16] "r"(ou + 256), [o24] "r"(ou + 384) \\
        : "t0", "t1", "ft0", "memory", VREGS)

static void compare(const char *what, const char *vt, uint64_t avl, uint64_t ru, uint64_t rm) {
    checks++;
    int at = -1;
    for (int i = 0; i < 512 && at < 0; i++)
        if ((i < 16 || i >= 128) && ou[i] != om[i])
            at = i;
    if ((at >= 0 || ru != rm) && bad++ < 40) {
        if (at >= 0)
            printf("%s %s avl %llu: v%d byte %d = %#x, want %#x\\n", what, vt, (unsigned long long) avl,
                   at / 16, at % 16, ou[at], om[at]);
        else
            printf("%s %s avl %llu: result %#llx, want %#llx\\n", what, vt, (unsigned long long) avl,
                   (unsigned long long) ru, (unsigned long long) rm);
    }
}

// indices for the indexed loads: v16's group as isz-byte elements in mem,
// some crossing its page boundary
static void indices(int isz) {
    for (int i = 0; i < 128; i += isz) {
        uint64_t v = rnd() % 4 == 0 ? 4096 - 8 + rnd() % 16 : rnd() % 8184;
        if (isz == 1)
            v &= 0xff;
        memcpy(in + 256 + i, &v, (size_t) isz);
    }
}

static int bit(const uint8_t *p, unsigned i) { return p[i / 8] >> (i % 8) & 1; }
static void setbit(uint8_t *p, unsigned i, int b) {
    p[i / 8] = (uint8_t) ((p[i / 8] & ~(1u << (i % 8))) | (unsigned) b << (i % 8));
}

// ---- the model: the instruction on `in` (the register file as loaded),
// into om and *r. Element i of register group g is at g * 16 + i * SEW/8.
enum { K_BIN, K_CMP, K_RED, K_VID, K_EXT, K_MSF, K_CPOP, K_FIRST, K_MVXS, K_MVSX, K_LX, K_W,
       K_SLIDE, K_GATHER, K_COMPRESS, K_IOTA, K_CARRY, K_UNARY, K_FB, K_FC, K_FMVFS, K_FMVSF,
       K_FCVT, K_FUN1, K_FRED, K_FW };
struct mc { int kind, op, form, masked, vd, vs2, vs1; int64_t imm; int aux, sign; uint64_t xmask; };

static uint64_t ld(const uint8_t *f, int reg, unsigned i, int eb) {
    uint64_t v = 0;
    memcpy(&v, f + reg * 16 + i * (unsigned) eb, (size_t) eb);
    return v;
}
static void st(uint8_t *f, int reg, unsigned i, int eb, uint64_t v) {
    memcpy(f + reg * 16 + i * (unsigned) eb, &v, (size_t) eb);
}
static int64_t sx(uint64_t v, int bits) {
    return bits == 64 ? (int64_t) v : (int64_t) (v << (64 - bits)) >> (64 - bits);
}
static uint64_t binop(int op, uint64_t a, uint64_t b, uint64_t d, int bits) {
    uint64_t m = bits == 64 ? ~0ull : (1ull << bits) - 1, r = 0;
    a &= m;
    b &= m;
    d &= m;
    int64_t sa = sx(a, bits), sb = sx(b, bits), smin = (int64_t) (1ull << (bits - 1)) << (64 - bits) >> (64 - bits);
    unsigned sh = (unsigned) (b & (uint64_t) (bits - 1));
    switch (op) {
    case 0: r = a + b; break;
    case 1: r = a - b; break;
    case 2: r = b - a; break;
    case 3: r = a & b; break;
    case 4: r = a | b; break;
    case 5: r = a ^ b; break;
    case 6: r = a & ~b; break;
    case 7: r = b; break;
    case 8: r = a < b ? a : b; break;
    case 9: r = sx(a, bits) < sx(b, bits) ? a : b; break;
    case 10: r = a > b ? a : b; break;
    case 11: r = sx(a, bits) > sx(b, bits) ? a : b; break;
    case 12: r = a << sh; break;
    case 13: r = a >> sh; break;
    case 14: r = (uint64_t) (sx(a, bits) >> sh); break;
    case 15: r = sh ? a >> sh | a << (bits - (int) sh) : a; break;
    case 16: r = sh ? a << sh | a >> (bits - (int) sh) : a; break;
    case 17: r = a * b; break;
    case 18: r = d + a * b; break;
    case 19: r = d - a * b; break;
    case 20: r = b * d + a; break;
    case 21: r = a - b * d; break;
    case 23: r = b ? a / b : ~0ull; break;
    case 24: r = b == 0 ? ~0ull : sa == smin && sb == -1 ? a : (uint64_t) (sa / sb); break;
    case 25: r = b ? a % b : a; break;
    case 26: r = b == 0 ? a : sa == smin && sb == -1 ? 0 : (uint64_t) (sa % sb); break;
    case 27: r = (uint64_t) ((unsigned __int128) a * b >> bits); break;
    case 28: r = (uint64_t) ((__int128) sa * (__int128) b >> bits); break;
    case 29: r = (uint64_t) ((__int128) sa * sb >> bits); break;
    }
    return r & m;
}
static int cmpop(int op, uint64_t a, uint64_t b, int bits) {
    uint64_t m = bits == 64 ? ~0ull : (1ull << bits) - 1;
    a &= m;
    b &= m;
    int64_t sa = sx(a, bits), sb = sx(b, bits);
    switch (op) {
    case 0: return a == b;
    case 1: return a != b;
    case 2: return a < b;
    case 3: return sa < sb;
    case 4: return a <= b;
    case 5: return sa <= sb;
    case 6: return a > b;
    default: return sa > sb;
    }
}
static uint64_t redop(int op, uint64_t a, uint64_t b, int bits) {
    static const int as_bin[] = {0, 3, 4, 5, 8, 9, 10, 11};
    return binop(as_bin[op], a, b, 0, bits);
}
// ---- FP model, written out from the spec: canonical NaNs, minimumNumber,
// quiet and signalling compares; flags from the guest's own scalar FP
static int fnan(uint64_t v, int bits) {
    return bits == 32 ? ((v >> 23 & 0xff) == 0xff && (v & 0x7fffff)) : ((v >> 52 & 0x7ff) == 0x7ff && (v & 0xfffffffffffffull));
}
static int fsnan(uint64_t v, int bits) {
    return fnan(v, bits) && !(bits == 32 ? v >> 22 & 1 : v >> 51 & 1);
}
static uint64_t fcanon(int bits) { return bits == 32 ? 0x7fc00000u : 0x7ff8000000000000ull; }
static double fget(uint64_t v, int bits) {
    if (bits == 32) { uint32_t w = (uint32_t) v; float f; memcpy(&f, &w, 4); return f; }
    double d; memcpy(&d, &v, 8); return d;
}
// the f scalar as the instruction sees it: NaN-boxed at SEW 32 (aux 1) or
// boxed by fmv.w.x (aux 2)
static uint64_t fscal(uint64_t x, int bits, int aux) {
    if (bits == 32 && aux == 1 && x >> 32 != 0xffffffffu)
        return fcanon(32);
    return bits == 32 ? (uint32_t) x : x;
}
static uint64_t fop(int op, uint64_t a, uint64_t b, uint64_t d, int bits) {
    uint64_t sign = bits == 32 ? 0x80000000u : 0x8000000000000000ull, r;
    if (op == 6 || op == 7) { // min, max
        if (fsnan(a, bits) || fsnan(b, bits))
            feraiseexcept(FE_INVALID);
        if (fnan(a, bits) && fnan(b, bits)) return fcanon(bits);
        if (fnan(a, bits)) return b;
        if (fnan(b, bits)) return a;
        double fa = fget(a, bits), fb = fget(b, bits);
        if (fa == fb) // +-0: min takes the negative one
            return op == 6 ? ((a & sign) ? a : b) : ((a & sign) ? b : a);
        return (op == 6) == (fa < fb) ? a : b;
    }
    if (op == 8) return (a & ~sign) | (b & sign);
    if (op == 9) return (a & ~sign) | (~b & sign);
    if (op == 10) return a ^ (b & sign);
    if (op == 11) return b;
    if (bits == 32) {
        volatile float fa = (float) fget(a, 32), fb = (float) fget(b, 32), fd = (float) fget(d, 32), fr = 0;
        switch (op) {
        case 0: fr = fa + fb; break;
        case 1: fr = fa - fb; break;
        case 2: fr = fb - fa; break;
        case 3: fr = fa * fb; break;
        case 4: fr = fa / fb; break;
        case 5: fr = fb / fa; break;
        case 12: fr = sqrtf(fa); break;
        case 13: fr = fmaf(fb, fa, fd); break;
        case 14: fr = fmaf(-fb, fa, -fd); break;
        case 15: fr = fmaf(fb, fa, -fd); break;
        case 16: fr = fmaf(-fb, fa, fd); break;
        case 17: fr = fmaf(fb, fd, fa); break;
        case 18: fr = fmaf(-fb, fd, -fa); break;
        case 19: fr = fmaf(fb, fd, -fa); break;
        case 20: fr = fmaf(-fb, fd, fa); break;
        }
        float f = fr;
        uint32_t w;
        memcpy(&w, &f, 4);
        r = w;
    } else {
        volatile double fa = fget(a, 64), fb = fget(b, 64), fd = fget(d, 64), fr = 0;
        switch (op) {
        case 0: fr = fa + fb; break;
        case 1: fr = fa - fb; break;
        case 2: fr = fb - fa; break;
        case 3: fr = fa * fb; break;
        case 4: fr = fa / fb; break;
        case 5: fr = fb / fa; break;
        case 12: fr = sqrt(fa); break;
        case 13: fr = fma(fb, fa, fd); break;
        case 14: fr = fma(-fb, fa, -fd); break;
        case 15: fr = fma(fb, fa, -fd); break;
        case 16: fr = fma(-fb, fa, fd); break;
        case 17: fr = fma(fb, fd, fa); break;
        case 18: fr = fma(-fb, fd, -fa); break;
        case 19: fr = fma(fb, fd, -fa); break;
        case 20: fr = fma(-fb, fd, fa); break;
        }
        double f = fr;
        memcpy(&r, &f, 8);
    }
    return fnan(r, bits) ? fcanon(bits) : r;
}
static int fcmp(int op, uint64_t a, uint64_t b, int bits) {
    int an = fnan(a, bits), bn = fnan(b, bits);
    if (op <= 1 ? (fsnan(a, bits) || fsnan(b, bits)) : (an || bn))
        feraiseexcept(FE_INVALID);
    if (an || bn)
        return op == 1;
    double fa = fget(a, bits), fb = fget(b, bits);
    switch (op) {
    case 0: return fa == fb;
    case 1: return fa != fb;
    case 2: return fa < fb;
    case 3: return fa <= fb;
    case 4: return fa > fb;
    default: return fa >= fb;
    }
}
// rounding to an integer without disturbing fflags (musl's trunc raises NX)
static double rint_quiet(double v, int rtz) {
    fenv_t e;
    feholdexcept(&e);
    double r = rtz ? trunc(v) : nearbyint(v);
    fesetenv(&e);
    return r;
}
static uint64_t mf2i(double v, int ibits, int sign, int rtz) {
    uint64_t ones = ibits == 64 ? ~0ull : (1ull << ibits) - 1, smax = ones >> 1;
    if (isnan(v)) {
        feraiseexcept(FE_INVALID);
        return sign ? smax : ones;
    }
    double r = rint_quiet(v, rtz);
    double lo = sign ? -ldexp(1.0, ibits - 1) : 0.0, hi = ldexp(1.0, sign ? ibits - 1 : ibits);
    if (r < lo || r >= hi) {
        feraiseexcept(FE_INVALID);
        if (r < lo)
            return sign ? smax + 1 : 0;
        return sign ? smax : ones;
    }
    if (r != v)
        feraiseexcept(FE_INEXACT);
    return sign ? (uint64_t) (int64_t) r : (uint64_t) r;
}
// f16 <-> f32 through the guest's scalar Zfhmin
__attribute__((target("arch=rv64gc_zfhmin"))) static uint64_t mh2s(uint64_t h) {
    uint64_t r;
    __asm__ volatile("fmv.w.x ft1, %1\\n fcvt.s.h ft0, ft1\\n fmv.x.w %0, ft0" : "=r"(r) : "r"(h | 0xffffffffffff0000ull) : "ft0", "ft1");
    return (uint32_t) r;
}
__attribute__((target("arch=rv64gc_zfhmin"))) static uint64_t ms2h(uint64_t w) {
    uint64_t r;
    __asm__ volatile("fmv.w.x ft1, %1\\n fcvt.h.s ft0, ft1\\n fmv.x.w %0, ft0" : "=r"(r) : "r"(w) : "ft0", "ft1");
    return r & 0xffff;
}
static uint64_t mfconv(int code, uint64_t a, int bits) {
    int wide = code >= 8 && code < 16, narrow = code >= 16;
    int sbits = narrow ? 2 * bits : bits, dbits = wide ? 2 * bits : bits;
    int f2i = code <= 1 || code == 6 || code == 7 || code == 8 || code == 9 || code == 14 || code == 15 ||
              code == 16 || code == 17 || code == 22 || code == 23;
    int i2f = code == 2 || code == 3 || code == 10 || code == 11 || code == 18 || code == 19;
    int sign = code & 1, rtz = code == 6 || code == 7 || code == 14 || code == 15 || code == 22 || code == 23;
    if (f2i)
        return mf2i(fget(a, sbits), dbits, sign, rtz);
    if (i2f) {
        uint64_t m = sbits == 64 ? ~0ull : (1ull << sbits) - 1;
        if (dbits == 32) {
            volatile float f = sign ? (float) sx(a, sbits) : (float) (a & m);
            float g = f; uint32_t w; memcpy(&w, &g, 4); return w;
        }
        volatile double f = sign ? (double) sx(a, sbits) : (double) (a & m);
        double g = f; uint64_t r; memcpy(&r, &g, 8); return r;
    }
    // float -> float
    if (sbits == 16)
        return mh2s(a);
    if (dbits == 16)
        return ms2h(a);
    if (dbits == 64) {
        volatile double d = fget(a, 32);
        double g = d; uint64_t r; memcpy(&r, &g, 8);
        return fnan(r, 64) ? fcanon(64) : r;
    }
    double d = fget(a, 64);
    if (code == 21) { // round to odd
        if (isnan(d)) { if (fsnan(a, 64)) feraiseexcept(FE_INVALID); return fcanon(32); }
        int old = fegetround();
        fesetround(FE_TOWARDZERO);
        volatile float f = (float) d;
        fesetround(old);
        float g = f; uint32_t w; memcpy(&w, &g, 4);
        if (!isinf(d) && (double) g != d)
            w |= 1;
        return w;
    }
    volatile float f = (float) d;
    float g = f; uint32_t w; memcpy(&w, &g, 4);
    return fnan(w, 32) ? fcanon(32) : w;
}
static const uint8_t m_rsqrt7[128] = {52, 51, 50, 48, 47, 46, 44, 43, 42, 41, 40, 39, 38, 36, 35, 34, 33, 32, 31, 30, 30, 29, 28, 27, 26, 25, 24, 23, 23, 22, 21, 20, 19, 19, 18, 17, 16, 16, 15, 14, 14, 13, 12, 12, 11, 10, 10, 9, 9, 8, 7, 7, 6, 6, 5, 4, 4, 3, 3, 2, 2, 1, 1, 0, 127, 125, 123, 121, 119, 118, 116, 114, 113, 111, 109, 108, 106, 105, 103, 102, 100, 99, 97, 96, 95, 93, 92, 91, 90, 88, 87, 86, 85, 84, 83, 82, 80, 79, 78, 77, 76, 75, 74, 73, 72, 71, 70, 70, 69, 68, 67, 66, 65, 64, 63, 63, 62, 61, 60, 59, 59, 58, 57, 56, 56, 55, 54, 53};
static const uint8_t m_rec7[128] = {127, 125, 123, 121, 119, 117, 116, 114, 112, 110, 109, 107, 105, 104, 102, 100, 99, 97, 96, 94, 93, 91, 90, 88, 87, 85, 84, 83, 81, 80, 79, 77, 76, 75, 74, 72, 71, 70, 69, 68, 66, 65, 64, 63, 62, 61, 60, 59, 58, 57, 56, 55, 54, 53, 52, 51, 50, 49, 48, 47, 46, 45, 44, 43, 42, 41, 40, 40, 39, 38, 37, 36, 35, 35, 34, 33, 32, 31, 31, 30, 29, 28, 28, 27, 26, 25, 25, 24, 23, 23, 22, 21, 21, 20, 19, 19, 18, 17, 17, 16, 15, 15, 14, 14, 13, 12, 12, 11, 11, 10, 9, 9, 8, 8, 7, 7, 6, 5, 5, 4, 4, 3, 3, 2, 2, 1, 1, 0};

static uint64_t mclass(uint64_t a, int bits) {
    int m = bits == 32 ? 23 : 52, e = bits == 32 ? 8 : 11;
    uint64_t emax = (1ull << e) - 1, exp = a >> m & emax, frac = a & ((1ull << m) - 1);
    int sign = (int) (a >> (bits - 1) & 1);
    if (exp == emax)
        return frac == 0 ? (sign ? 1 : 1 << 7) : (frac >> (m - 1) ? 1 << 9 : 1 << 8);
    if (exp == 0)
        return frac == 0 ? (sign ? 1 << 3 : 1 << 4) : (sign ? 1 << 2 : 1 << 5);
    return sign ? 1 << 1 : 1 << 6;
}
// the spec's vfrsqrt7/vfrec7, by its text
static uint64_t mest7(int rec, uint64_t a, int bits) {
    int m = bits == 32 ? 23 : 52, e = bits == 32 ? 8 : 11;
    uint64_t emax = (1ull << e) - 1, bias = emax >> 1, mmask = (1ull << m) - 1;
    int sign = (int) (a >> (bits - 1) & 1);
    uint64_t exp = a >> m & emax, sig = a & mmask, sbit = (uint64_t) sign << (bits - 1);
    uint64_t canon = emax << m | 1ull << (m - 1);
    if (exp == emax && sig) {
        if (!(sig >> (m - 1)))
            feraiseexcept(FE_INVALID);
        return canon;
    }
    if (exp == 0 && sig == 0) {
        feraiseexcept(FE_DIVBYZERO);
        return sbit | emax << m;
    }
    if (!rec && sign) {
        feraiseexcept(FE_INVALID);
        return canon;
    }
    if (exp == emax)
        return rec ? sbit : 0;
    int64_t nexp = (int64_t) exp;
    if (exp == 0) {
        while (!(sig >> (m - 1) & 1)) { nexp--; sig <<= 1; }
        sig = sig << 1 & mmask;
    }
    if (!rec) {
        unsigned idx = (unsigned) ((nexp & 1) << 6 | (int64_t) (sig >> (m - 6)));
        return (uint64_t) ((int64_t) (3 * bias - 1) - nexp) / 2 << m | (uint64_t) m_rsqrt7[idx] << (m - 7);
    }
    int64_t oexp = (int64_t) (2 * bias - 1) - nexp;
    if (oexp < -1 || oexp > (int64_t) (2 * bias)) {
        feraiseexcept(FE_INEXACT | FE_OVERFLOW);
        int rm = fegetround();
        int inf = rm == FE_TONEAREST || (sign ? rm == FE_DOWNWARD : rm == FE_UPWARD);
        return inf ? sbit | emax << m : sbit | ((emax << m) - 1);
    }
    uint64_t osig = (uint64_t) m_rec7[sig >> (m - 7)] << (m - 7);
    if (oexp <= 0) {
        osig = (osig | 1ull << m) >> (1 - oexp);
        oexp = 0;
    }
    return sbit | (uint64_t) oexp << m | osig;
}

static uint64_t fflags_now(void) {
    return (uint64_t) fetestexcept(FE_ALL_EXCEPT);
}

static void model(const struct mc *c, int bits, int lmul8, uint64_t avl, uint64_t x, uint64_t *r) {
    memcpy(om, in, sizeof(om));
    *r = 0;
    int eb = bits / 8;
    unsigned vlmax = (unsigned) (128 * lmul8 / 8 / bits), vl = avl < vlmax ? (unsigned) avl : vlmax;
#define ACT(i) (!c->masked || bit(in, i))
#define SRC(i) (c->form == 0 ? ld(in, c->vs1, i, eb) : c->form == 1 ? x : (uint64_t) c->imm)
    switch (c->kind) {
    case K_BIN:
        for (unsigned i = 0; i < vl; i++) {
            uint64_t a = ld(in, c->vs2, i, eb);
            if (c->op == 22)
                st(om, c->vd, i, eb, bit(in, i) ? SRC(i) : a);
            else if (ACT(i))
                st(om, c->vd, i, eb, binop(c->op, a, SRC(i), ld(in, c->vd, i, eb), bits));
        }
        break;
    case K_CMP:
        for (unsigned i = 0; i < vl; i++)
            if (ACT(i))
                setbit(om + c->vd * 16, i, cmpop(c->op, ld(in, c->vs2, i, eb), SRC(i), bits));
        break;
    case K_RED:
        if (vl != 0) {
            uint64_t acc = ld(in, c->vs1, 0, eb);
            for (unsigned i = 0; i < vl; i++)
                if (ACT(i))
                    acc = redop(c->op, acc, ld(in, c->vs2, i, eb), bits);
            st(om, c->vd, 0, eb, acc);
        }
        break;
    case K_W:
        for (unsigned i = 0; i < vl; i++)
            if (ACT(i)) {
                int ab = eb << (c->aux & 1), db = eb << (c->aux >> 1);
                uint64_t a = ld(in, c->vs2, i, ab), b = c->form == 0 ? ld(in, c->vs1, i, eb) : c->form == 1 ? x : (uint64_t) c->imm;
                if (c->sign & 1)
                    a = (uint64_t) sx(a, ab * 8);
                if (c->form != 2) {
                    b &= bits == 64 ? ~0ull : (1ull << bits) - 1;
                    if (c->sign & 2)
                        b = (uint64_t) sx(b, bits);
                }
                unsigned sh = (unsigned) (b & (uint64_t) (2 * bits - 1));
                uint64_t r = 0;
                switch (c->op) {
                case 0: r = a + b; break;
                case 1: r = a - b; break;
                case 2: r = a * b; break;
                case 3: r = ld(in, c->vd, i, db) + a * b; break;
                case 4: r = a >> sh; break;
                case 5: r = (uint64_t) ((int64_t) a >> sh); break;
                case 6: r = a << sh; break;
                }
                st(om, c->vd, i, db, r);
            }
        break;
    case K_SLIDE: {
        uint64_t sc = x;
        if (c->aux == 1 && bits == 32 && x >> 32 != 0xffffffffu)
            sc = 0x7fc00000;                 // not NaN-boxed: the canonical NaN
        uint64_t off = c->form == 1 ? x : (uint64_t) c->imm;
        for (unsigned i = 0; i < vl; i++) {
            if (!ACT(i))
                continue;
            switch (c->op) {
            case 0: if (i >= off) st(om, c->vd, i, eb, ld(in, c->vs2, (unsigned) (i - off), eb)); break;
            case 1: st(om, c->vd, i, eb, off < vlmax && i + off < vlmax ? ld(in, c->vs2, (unsigned) (i + off), eb) : 0); break;
            case 2: st(om, c->vd, i, eb, i == 0 ? sc : ld(in, c->vs2, i - 1, eb)); break;
            case 3: st(om, c->vd, i, eb, i + 1 == vl ? sc : ld(in, c->vs2, i + 1, eb)); break;
            }
        }
        break;
    }
    case K_GATHER:
        for (unsigned i = 0; i < vl; i++)
            if (ACT(i)) {
                uint64_t idx = c->form == 0 ? ld(in, c->vs1, i, c->aux ? c->aux : eb) : c->form == 1 ? x : (uint64_t) c->imm;
                st(om, c->vd, i, eb, idx < vlmax ? ld(in, c->vs2, (unsigned) idx, eb) : 0);
            }
        break;
    case K_COMPRESS: {
        unsigned k = 0;
        for (unsigned i = 0; i < vl; i++)
            if (bit(in + c->vs1 * 16, i))
                st(om, c->vd, k++, eb, ld(in, c->vs2, i, eb));
        break;
    }
    case K_IOTA: {
        uint64_t cnt = 0;
        for (unsigned i = 0; i < vl; i++)
            if (ACT(i)) {
                st(om, c->vd, i, eb, cnt);
                cnt += bit(in + c->vs2 * 16, i);
            }
        break;
    }
    case K_CARRY: {
        uint64_t m = bits == 64 ? ~0ull : (1ull << bits) - 1;
        for (unsigned i = 0; i < vl; i++) {
            unsigned __int128 a = ld(in, c->vs2, i, eb), b = SRC(i) & m, cin = (c->aux & 2) ? (unsigned) bit(in, i) : 0;
            unsigned __int128 r = c->op == 0 ? a + b + cin : a - b - cin;
            if (c->aux & 1)
                setbit(om + c->vd * 16, i, (int) (c->op == 0 ? (r >> bits) & 1 : a < b + cin));
            else
                st(om, c->vd, i, eb, (uint64_t) r);
        }
        break;
    }
    case K_UNARY:
        for (unsigned i = 0; i < vl; i++)
            if (ACT(i)) {
                uint64_t a = ld(in, c->vs2, i, eb), r = 0;
                switch (c->op) {
                case 0: for (int k = 0; k < bits; k++) r |= (a >> k & 1) << ((k & ~7) | (7 - (k & 7))); break;
                case 1: for (int k = 0; k < eb; k++) r |= (a >> (8 * k) & 0xff) << (8 * (eb - 1 - k)); break;
                case 2: for (int k = 0; k < bits; k++) r |= (a >> k & 1) << (bits - 1 - k); break;
                case 3: r = (uint64_t) bits; for (int k = bits - 1; k >= 0; k--) if (a >> k & 1) { r = (uint64_t) (bits - 1 - k); break; } break;
                case 4: r = (uint64_t) bits; for (int k = 0; k < bits; k++) if (a >> k & 1) { r = (uint64_t) k; break; } break;
                case 5: for (int k = 0; k < bits; k++) r += a >> k & 1; break;
                }
                st(om, c->vd, i, eb, r);
            }
        break;
    case K_FB:
        feclearexcept(FE_ALL_EXCEPT);
        for (unsigned i = 0; i < vl; i++) {
            uint64_t a = ld(in, c->vs2, i, eb), b = c->form == 0 ? ld(in, c->vs1, i, eb) : fscal(x, bits, c->aux);
            if (c->op == 21)
                st(om, c->vd, i, eb, bit(in, i) ? b : a);
            else if (ACT(i))
                st(om, c->vd, i, eb, fop(c->op, a, b, ld(in, c->vd, i, eb), bits));
        }
        *r = fflags_now();
        break;
    case K_FC:
        feclearexcept(FE_ALL_EXCEPT);
        for (unsigned i = 0; i < vl; i++)
            if (ACT(i))
                setbit(om + c->vd * 16, i, fcmp(c->op, ld(in, c->vs2, i, eb),
                       c->form == 0 ? ld(in, c->vs1, i, eb) : fscal(x, bits, c->aux), bits));
        *r = fflags_now();
        break;
    case K_FCVT: {
        int code = c->op, wide = code >= 8 && code < 16, narrow = code >= 16;
        int sb = narrow ? 2 * eb : eb, db = wide ? 2 * eb : eb;
        feclearexcept(FE_ALL_EXCEPT);
        for (unsigned i = 0; i < vl; i++)
            if (ACT(i))
                st(om, c->vd, i, db, mfconv(code, ld(in, c->vs2, i, sb), bits));
        *r = fflags_now();
        break;
    }
    case K_FUN1:
        feclearexcept(FE_ALL_EXCEPT);
        for (unsigned i = 0; i < vl; i++)
            if (ACT(i)) {
                uint64_t a = ld(in, c->vs2, i, eb);
                st(om, c->vd, i, eb, c->op == 0 ? mclass(a, bits) : mest7(c->op == 2, a, bits));
            }
        *r = fflags_now();
        break;
    case K_FRED: {
        int w = c->op >= 4, ab = w ? 64 : bits;
        feclearexcept(FE_ALL_EXCEPT);
        if (vl != 0) {
            uint64_t acc = ld(in, c->vs1, 0, ab / 8);
            for (unsigned i = 0; i < vl; i++)
                if (ACT(i)) {
                    uint64_t e = ld(in, c->vs2, i, eb);
                    if (w) {
                        volatile double d = fget(e, 32);
                        double g = d;
                        memcpy(&e, &g, 8);
                    }
                    acc = c->op == 2 ? fop(6, acc, e, 0, ab) : c->op == 3 ? fop(7, acc, e, 0, ab) : fop(0, acc, e, 0, ab);
                }
            st(om, c->vd, 0, ab / 8, acc);
        }
        *r = fflags_now();
        break;
    }
    case K_FW:
        feclearexcept(FE_ALL_EXCEPT);
        for (unsigned i = 0; i < vl; i++)
            if (ACT(i)) {
                int aw = c->op == 2 || c->op == 3;
                volatile double a = aw ? fget(ld(in, c->vs2, i, 8), 64) : fget(ld(in, c->vs2, i, 4), 32);
                volatile double b = fget(c->form == 0 ? ld(in, c->vs1, i, 4) : fscal(x, 32, c->aux), 32);
                volatile double d = fget(ld(in, c->vd, i, 8), 64), rr = 0;
                switch (c->op) {
                case 0: case 2: rr = a + b; break;
                case 1: case 3: rr = a - b; break;
                case 4: rr = a * b; break;
                case 5: rr = fma(b, a, d); break;
                case 6: rr = fma(-b, a, -d); break;
                case 7: rr = fma(b, a, -d); break;
                case 8: rr = fma(-b, a, d); break;
                }
                double g = rr;
                uint64_t v;
                memcpy(&v, &g, 8);
                st(om, c->vd, i, 8, fnan(v, 64) ? fcanon(64) : v);
            }
        *r = fflags_now();
        break;
    case K_FMVFS:
        *r = bits == 32 ? 0xffffffff00000000ull | ld(in, c->vs2, 0, 4) : ld(in, c->vs2, 0, 8);
        break;
    case K_FMVSF:
        if (vl != 0)
            st(om, c->vd, 0, eb, fscal(x, bits, c->aux));
        break;
    case K_VID:
        for (unsigned i = 0; i < vl; i++)
            if (ACT(i))
                st(om, c->vd, i, eb, i);
        break;
    case K_EXT:
        for (unsigned i = 0; i < vl; i++)
            if (ACT(i)) {
                int sb = eb / c->aux;
                uint64_t v = ld(in, c->vs2, i, sb);
                st(om, c->vd, i, eb, c->sign ? (uint64_t) sx(v, sb * 8) : v);
            }
        break;
    case K_MSF: {
        unsigned first = vl;
        for (unsigned i = 0; i < vl && first == vl; i++)
            if (bit(in + c->vs2 * 16, i))
                first = i;
        for (unsigned i = 0; i < vl; i++)
            setbit(om + c->vd * 16, i, c->op == 0 ? i < first : c->op == 1 ? i <= first : i == first);
        break;
    }
    case K_CPOP:
        for (unsigned i = 0; i < vl; i++)
            *r += ACT(i) && bit(in + c->vs2 * 16, i);
        break;
    case K_FIRST:
        *r = (uint64_t) -1;
        for (unsigned i = 0; i < vl; i++)
            if (ACT(i) && bit(in + c->vs2 * 16, i)) {
                *r = i;
                break;
            }
        break;
    case K_MVXS:
        *r = (uint64_t) sx(ld(in, c->vs2, 0, eb), bits);
        break;
    case K_MVSX:
        if (vl != 0)
            st(om, c->vd, 0, eb, x);
        break;
    case K_LX:
        for (unsigned i = 0; i < vl; i++)
            if (ACT(i)) {
                uint64_t v = 0;
                memcpy(&v, mem + ld(in, c->vs2, i, c->aux), (size_t) eb);
                st(om, c->vd, i, eb, v);
            }
        break;
    }
#undef ACT
#undef SRC
}

// one instruction under one SEW/LMUL set by vsetvli, so typed at
// translation, against the model
#define TCASE(FN, SEW, L, LMUL8, VT, NAME, TEXT, ...) \\
    V static void FN(uint64_t avl, uint64_t x) { \\
        static const struct mc m = {__VA_ARGS__}; \\
        uint64_t ru, rm, vt = VT; \\
        x &= m.xmask; \\
        model(&m, SEW, LMUL8, avl, x, &rm); \\
        RUNV("vsetvli t1, %[avl], e" #SEW ", " #L ", tu, mu", TEXT); \\
        compare(NAME, "e" #SEW #L, avl, ru, rm); \\
    }
// the same under any SEW/LMUL, set by vsetvl: unknown at translation, so
// the gadget re-dispatches to the SEW in force
#define RCASE(FN, NAME, TEXT, ...) \\
    V static void FN(uint64_t avl, uint64_t x, int sew, int lmul8, uint64_t vt, const char *vtn) { \\
        static const struct mc m = {__VA_ARGS__}; \\
        uint64_t ru, rm; \\
        x &= m.xmask; \\
        model(&m, sew, lmul8, avl, x, &rm); \\
        RUNV("vsetvl t1, %[avl], %[vt]", TEXT); \\
        compare(NAME " (re-dispatched)", vtn, avl, ru, rm); \\
    }
''')

VTYPES = list(vtypes())
fns = []    # typed: (fn, vlmax, isz)
rfns = []   # re-dispatched: (fn, allowed-vtype mask, isz)
for name, text, f, ok in cases:
    isz = f[8] if f[0] == K_LX else 0
    allowed = [v for v in VTYPES if ok(*v)]
    mask = sum(1 << VTYPES.index(v) for v in allowed)
    fn = f'r{len(rfns)}'
    w(f'RCASE({fn}, "{name}", "{text}", {", ".join(str(v) for v in f)})')
    rfns.append((fn, mask, isz))
    for sew in SEWS:  # typed: one LMUL per SEW, the first allowed of m1 m2 mf2 m4 m8
        pick = next((v for l in ('m1', 'm2', 'mf2', 'm4', 'm8') for v in allowed if v == (sew, l)), None)
        if pick is None:
            continue
        sew_, l = pick
        fn = f'c{len(fns)}'
        w(f'TCASE({fn}, {sew}, {l}, {lmul8(l)}, {vtcode(sew, l)}, "{name}", "{text}", {", ".join(str(v) for v in f)})')
        fns.append((fn, vlmax(sew, l), isz))

# the mask-register logical ops and vmv<nr>r.v, which do not depend on SEW
mlog = [('vmandn', 'a & !b'), ('vmand', 'a & b'), ('vmor', 'a | b'), ('vmxor', 'a ^ b'),
        ('vmorn', 'a | !b'), ('vmnand', '!(a & b)'), ('vmnor', '!(a | b)'), ('vmxnor', '!(a ^ b)')]
models = []
for op, expr in mlog:
    for sew, l in vtypes():
        fn = f'm{len(models)}'
        w(f'V static void {fn}(uint64_t avl, uint64_t x) {{')
        w('    uint64_t ru, vt = 0;')
        out.append(run(f'vsetvli t1, %[avl], e{sew}, {l}, tu, mu', f'{op}.mm v8, v16, v24'))
        w(f'    uint64_t vl = avl < {vlmax(sew, l)} ? avl : {vlmax(sew, l)};')
        w('    memcpy(om, in, sizeof(om));')
        w('    for (unsigned i = 0; i < vl; i++) {')
        w('        int a = bit(in + 256, i), b = bit(in + 384, i);')
        w(f'        setbit(om + 128, i, {expr});')
        w('    }')
        w(f'    compare("{op}.mm", "e{sew}{l}", avl, ru, 0);')
        w('    (void) x;')
        w('}')
        models.append(fn)
for nr in (1, 2, 4, 8):
    fn = f'm{len(models)}'
    w(f'V static void {fn}(uint64_t avl, uint64_t x) {{')
    w('    uint64_t ru, vt = 0;')
    out.append(run('vsetvli t1, %[avl], e32, m1, tu, mu', f'vmv{nr}r.v v8, v16'))
    w(f'    memcpy(om, in, sizeof(om)); memcpy(om + 128, in + 256, {nr * 16});')
    w(f'    compare("vmv{nr}r.v", "e32m1", avl, ru, 0);')
    w('    (void) x;')
    w('}')
    models.append(fn)

# vsetvli x0, x0: vl kept when VLMAX is unchanged, else vill
pairs = [(('e32', 'm2'), ('e16', 'm1')), (('e8', 'm1'), ('e64', 'm8')), (('e32', 'm2'), ('e8', 'm1')),
         (('e16', 'mf2'), ('e32', 'm1')), (('e64', 'm1'), ('e64', 'm1'))]
for (s1, l1), (s2, l2) in pairs:
    fn = f'm{len(models)}'
    vm1, vm2 = vlmax(int(s1[1:]), l1), vlmax(int(s2[1:]), l2)
    w(f'V static void {fn}(uint64_t avl, uint64_t x) {{')
    w('    uint64_t vl, vt;')
    w(f'    asm volatile("vsetvli t1, %[avl], {s1}, {l1}, tu, mu\\n"')
    w(f'        "vsetvli zero, zero, {s2}, {l2}, tu, mu\\n"')
    w('        "csrr %[vl], vl\\n" "csrr %[vt], vtype\\n"')
    w('        : [vl] "=&r"(vl), [vt] "=&r"(vt) : [avl] "r"(avl) : "t1");')
    if vm1 == vm2:
        w(f'    uint64_t wl = avl < {vm1} ? avl : {vm1}, wt = {vtcode(int(s2[1:]), l2)};')
    else:
        w('    uint64_t wl = 0, wt = 1ull << 63;')
    w('    memset(ou, 0, sizeof(ou)); memset(om, 0, sizeof(om));')
    w(f'    compare("vsetvli x0,x0 {s1}{l1}->{s2}{l2} vl", "", avl, vl, wl);')
    w(f'    compare("vsetvli x0,x0 {s1}{l1}->{s2}{l2} vtype", "", avl, vt, wt);')
    w('    (void) x;')
    w('}')
    models.append(fn)

# a register group not aligned to LMUL is reserved: SIGILL, typed or re-dispatched
w("""
static sigjmp_buf ill_jmp;
static void on_sigill(int sig) { (void) sig; siglongjmp(ill_jmp, 1); }
static void expect_sigill(const char *what, void (*fn)(void)) {
    checks++;
    if (sigsetjmp(ill_jmp, 1) == 0) {
        fn();
        if (bad++ < 40)
            printf("%s: ran, want SIGILL\\n", what);
    }
}""")
ills = []
for text in ('vadd.vv v9, v16, v24', 'vadd.vv v8, v17, v24', 'vadd.vv v8, v16, v25',
             'vmseq.vv v8, v17, v24', 'vredsum.vs v8, v17, v24', 'vid.v v9', 'vxor.vx v13, v16, t1',
             'vwadd.vv v10, v16, v24', 'vnsrl.wv v8, v18, v24', 'e64:vwadd.vv v8, v16, v24', 'e64:vwmul.vx v8, v16, t1',
             'm8:vwadd.vv v16, v8, v24', 'm8:vnsrl.wi v8, v16, 3', 'e16:vfslide1up.vf v8, v16, ft0'):
    sew = 64 if text.startswith('e64:') else 16 if text.startswith('e16:') else 32
    lmuls = (('m8', 3),) if text.startswith('m8:') else (('m2', 1), ('m4', 2), ('m8', 3))
    text = text.split(':')[-1]
    for l, vt in lmuls:
        for how, vset in (('typed', f'vsetvli t1, %[avl], e{sew}, {l}, tu, mu'),
                          ('re-dispatched', 'vsetvl t1, %[avl], %[vt]')):
            fn = f'ill{len(ills)}'
            w(f'V static void {fn}(void) {{')
            w(f'    uint64_t avl = 4, vt = {({64: 3, 32: 2, 16: 1}[sew]) << 3 | vt};')
            w(f'    asm volatile("{vset}\\n" "{text}\\n" : : [avl] "r"(avl), [vt] "r"(vt) : "t1", "memory", VREGS);')
            w('}')
            ills.append((fn, f'{text} e{sew}{l} {how}'))
w('static void sigill_cases(void) {')
w('    signal(SIGILL, on_sigill);')
for fn, what in ills:
    w(f'    expect_sigill("{what}", {fn});')
w('    signal(SIGILL, SIG_DFL);')
w('}')

w('')
w('static const struct { int sew, lmul8; uint64_t vt; const char *name; } vts[] = {')
for sew, l in VTYPES:
    w(f'    {{{sew}, {lmul8(l)}, {vtcode(sew, l)}, "e{sew}{l}"}},')
w('};')
w('struct rcase { void (*fn)(uint64_t, uint64_t, int, int, uint64_t, const char *); unsigned mask, isz; };')
w('static const struct rcase rcases[] = {')
for fn, mask, isz in rfns:
    w(f'    {{{fn}, {mask}u, {isz}}},')
w('};')
w('struct tcase { void (*fn)(uint64_t, uint64_t); unsigned vlmax, isz; };')
w('static const struct tcase tcases[] = {')
for fn, vm, isz in fns:
    w(f'    {{{fn}, {vm}, {isz}}},')
for fn in models:
    w(f'    {{{fn}, 128, 0}},')
w('};')
w('''
int main(void) {
    for (int i = 0; i < (int) sizeof(mem); i++)
        mem[i] = (uint8_t) rnd();
    // the last rounds draw bytes from 00/01/7f/80/ff: zero, one, -1, MIN and
    // MAX at every SEW, for the division and overflow cases
    static const uint8_t special[5] = {0x00, 0x01, 0x7f, 0x80, 0xff};
    for (int round = 0; round < 12; round++) {
        for (unsigned c = 0; c < sizeof(tcases) / sizeof(tcases[0]); c++) {
            for (int i = 0; i < 512; i++)
                in[i] = round >= 8 ? special[rnd() % 5] : (uint8_t) rnd();
            if (tcases[c].isz)
                indices((int) tcases[c].isz);
            uint64_t avl = round == 0 ? 0 : round == 1 ? tcases[c].vlmax : rnd() % (tcases[c].vlmax + 3);
            static const int modes[4] = {FE_TONEAREST, FE_TOWARDZERO, FE_DOWNWARD, FE_UPWARD};
            fesetround(modes[round % 4]);
            uint64_t x = rnd();
            if (round >= 8)
                x = rnd() % 2 ? 0 : (uint64_t) -1;
            tcases[c].fn(avl, x);
        }
        for (unsigned c = 0; c < sizeof(rcases) / sizeof(rcases[0]); c++)
            for (unsigned v = 0; v < sizeof(vts) / sizeof(vts[0]); v++) {
                if (!(rcases[c].mask >> v & 1))
                    continue;
                for (int i = 0; i < 512; i++)
                    in[i] = round >= 8 ? special[rnd() % 5] : (uint8_t) rnd();
                if (rcases[c].isz)
                    indices((int) rcases[c].isz);
                unsigned vlmax = (unsigned) (128 * vts[v].lmul8 / 8 / vts[v].sew);
                uint64_t avl = round == 0 ? 0 : round == 1 ? vlmax : rnd() % (vlmax + 3);
                uint64_t x = rnd();
                if (round >= 8)
                    x = rnd() % 2 ? 0 : (uint64_t) -1;
                rcases[c].fn(avl, x, vts[v].sew, vts[v].lmul8, vts[v].vt, vts[v].name);
            }
    }
    fesetround(FE_TONEAREST);
    sigill_cases();
    printf("riscv64_rvv_gadgets: %s (%lu checks, %lu mismatches)\\n", bad ? "FAIL" : "PASS", checks, bad);
    return bad != 0;
}''')
print('\n'.join(out))
