"""Tests for Mode (context / smart mono / dual) and Unison.
Run from test/:  python3 test_modes.py
"""
import numpy as np, subprocess
import gen_and_eval as G
from gen_and_eval import SR, hz, pluck, place, MAG, EL

CTX = {0: 'silence', 1: 'single', 2: 'melody', 3: 'chords', 4: 'unison'}


def runx(buf, dry_sig=None, **kw):
    buf.astype(np.float32).tofile('/tmp/in.f32')
    env = None
    if dry_sig is not None:
        dry_sig.astype(np.float32).tofile('/tmp/dry.f32'); env = {'DRY': '/tmp/dry.f32'}
    args = [f'{k}={v}' for k, v in kw.items()]
    subprocess.run(['./run_engine', '/tmp/in.f32', '/tmp/out.f32', '/tmp/log.f32', str(SR)] + args,
                   check=True, env=env)
    out = np.fromfile('/tmp/out.f32', np.float32)
    bass = np.fromfile('/tmp/bass.f32', np.float32)
    lg = np.fromfile('/tmp/log.f32', np.float32).reshape(-1, 4)
    return out, bass, lg


def seq_signal(events, total, prof=MAG):
    """events: list of (onset, midi, dur, gain)"""
    buf = np.zeros(int(SR * total))
    for on, m, dur, g in events:
        place(buf, g * pluck(hz(m), dur, prof, decay=2.0), on)
    return buf


def scale(bpm_notes_per_s, notes, overlap=0.0, start=0.2):
    dt = 1.0 / bpm_notes_per_s
    return [(start + i * dt, m, dt * 0.97 + overlap, 1.0) for i, m in enumerate(notes)], start + len(notes) * dt + 0.5


def strum(chord, per_s, count, spread=0.012, start=0.2):
    ev = []
    dt = 1.0 / per_s
    for i in range(count):
        on = start + i * dt
        order = chord if i % 2 == 0 else chord[::-1]
        for j, m in enumerate(order):
            ev.append((on + j * spread, m, dt * 0.97 - j * spread, 0.8))
    return ev, start + count * dt + 0.5


PENTA = [40, 43, 45, 47, 50, 52, 55, 57, 55, 52, 50, 47, 45, 43, 40, 43, 45, 47, 50, 52]
CASES = [
    # name, events builder, expected context, bass expected in smart mono
    ('scale 8/s legato',     lambda: scale(8, PENTA),                  'melody', False),
    ('scale 12/s legato',    lambda: scale(12, PENTA),                 'melody', False),
    ('scale 8/s 40ms ring',  lambda: scale(8, PENTA, overlap=0.04),    'melody', False),
    ('scale 3/s (slow)',     lambda: scale(3, PENTA[:8]),              'single', True),
    ('strum E 8ths 3.3/s',   lambda: strum([40, 47, 52, 56, 59, 64], 3.3, 8), 'chords', True),
    ('strum E 16ths 6.7/s',  lambda: strum([40, 47, 52, 56, 59, 64], 6.7, 16), 'chords', True),
    ('power D5 16ths 8/s',   lambda: strum([38, 45, 50], 8, 16, spread=0.006), 'chords', True),
    ('arpeggio ringing 6/s', lambda: ([(0.2 + i / 6, m, 1.5, 0.8) for i, m in enumerate([40, 47, 52, 56, 59, 56, 52, 47] * 2)], 3.6), 'chords', True),
    ('double stops 4/s',     lambda: ([(0.2 + i / 4 + j * 0.004, m, 0.24, 0.8) for i in range(12) for j, m in enumerate([(40, 45), (43, 47), (45, 50)][i % 3])], 3.7), 'chords', True),
]


def context_test(ll):
    print(f"\n== context / smart mono (low_latency={ll})")
    print(f"  {'case':24s} {'expected':8s} {'share of time in expected state':>32s}   smart-mono bass level (on/classic)")
    for name, build, exp, bass_expected in CASES:
        ev, total = build()
        buf = seq_signal(ev, total, EL)
        _, bass_c, lg = runx(buf, ll=ll, mode=0, cut=57, inst=1, tun=0)
        _, bass_s, lg_s = runx(buf, ll=ll, mode=1, cut=57, inst=1, tun=0)
        a = int((ev[0][0] + 0.45) * SR); b = int((total - 0.5) * SR)   # skip first ~3 notes
        ctx = lg[a:b, 2].astype(int)
        share = np.mean(ctx == [k for k, v in CTX.items() if v == exp][0])
        dist = {CTX[k]: round(float(np.mean(ctx == k)) * 100) for k in np.unique(ctx)}
        rc = np.sqrt(np.mean(bass_c[a:b] ** 2)) + 1e-12
        rs = np.sqrt(np.mean(bass_s[a:b] ** 2))
        ok = (share > 0.7) and ((rs / rc > 0.7) == bass_expected)
        print(f"  {name:24s} {exp:8s} {share*100:6.0f}%  {str(dist):32s} {20*np.log10(rs/rc + 1e-9):6.1f} dB  {'OK' if ok else '--'}")


def component_amp(x, f):
    t = np.arange(len(x)) / SR
    M = np.array([np.cos(2 * np.pi * f * t), np.sin(2 * np.pi * f * t)]).T
    c = np.linalg.lstsq(M, x, rcond=None)[0]
    return np.hypot(*c)


def dual_test():
    print("\n== dual: guitar notes removed from the dry (dB change of each component, 100-400 ms after onset)")
    cases = [('E2 alone', [40], 40), ('A2 alone', [45], 45), ('D2 alone', [38], 38),
             ('E2+B3+E4 (low root)', [40, 59, 64], 40), ('A2+C#4+E4', [45, 61, 64], 45),
             ('E4 alone (out of range)', [64], None)]
    for name, notes, low in cases:
        buf = np.zeros(int(SR * 1.2))
        for j, m in enumerate(notes):
            place(buf, 0.7 * pluck(hz(m), 1.0, MAG, decay=0.5), 0.2 + 0.005 * j)
        out0, _, _ = runx(buf, mode=0, dry=0, synth=-60, cut=57)
        out2, bass2, _ = runx(buf, mode=2, dry=0, synth=-60, cut=57)
        a, b = int(0.3 * SR), int(0.6 * SR)
        parts = []
        for m in notes:
            for h in (1, 2, 3):
                f = hz(m) * h
                d = 20 * np.log10(component_amp(out2[a:b], f) / (component_amp(out0[a:b], f) + 1e-12) + 1e-12)
                parts.append(f"{m}x{h}:{d:+.0f}")
        print(f"  {name:26s} " + "  ".join(parts))


def unison_test():
    print("\n== unison: bass pitch for a solo line (bass out only)")
    notes = [52, 55, 57, 60, 64, 67, 69, 72, 76, 79, 81, 84, 88]
    for oct_mode, lbl in ((0, '-1'), (1, '-2'), (2, 'auto')):
        buf = np.zeros(int(SR * (len(notes) * 0.3 + 0.4)))
        for i, m in enumerate(notes):
            place(buf, pluck(hz(m), 0.29, EL, decay=1.5), 0.2 + i * 0.3)
        _, bass, lg = runx(buf, uni=1, unioct=oct_mode, inst=1, tun=0, tone=0)
        res = []
        for i, m in enumerate(notes):
            a = int((0.2 + i * 0.3 + 0.08) * SR); b = int((0.2 + i * 0.3 + 0.27) * SR)
            seg = bass[a:b]
            z = np.nonzero((seg[:-1] < 0) & (seg[1:] >= 0))[0]
            f = SR / np.median(np.diff(z)) if len(z) > 3 else 0
            k = int(round(np.log2(hz(m) / f))) if f > 0 else -1
            cents = 1200 * np.log2(f / (hz(m) / 2 ** k)) if f > 0 else 0
            res.append(f"{m}:{f:.0f}Hz(-{k}oct,{cents:+.0f}c)")
        print(f"  octave {lbl:4s} " + " ".join(res))
        ctx = lg[int(0.5 * SR):, 2]
        assert np.all(ctx == 4), "context should be unison"


if __name__ == '__main__':
    import sys
    what = sys.argv[1:] or ['context', 'dual', 'unison']
    if 'context' in what:
        for ll in (1, 0): context_test(ll)
    if 'dual' in what: dual_test()
    if 'unison' in what: unison_test()
