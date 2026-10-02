import numpy as np, subprocess, sys, os
SR = 48000
rng = np.random.default_rng(1)
def hz(m): return 440*2**((m-69)/12)
def pluck(f, dur, amps, decay=1.2, inharm=0.0003, t0=0.0, level=0.3):
    n = int(dur*SR); t = np.arange(n)/SR
    y = np.zeros(n)
    for k, a in enumerate(amps, 1):
        fk = f*k*np.sqrt(1+inharm*k*k)
        if fk > 8000: break
        y += a*np.sin(2*np.pi*fk*t + rng.uniform(0, 2*np.pi))*np.exp(-t*decay*(1+0.25*k))
    att = np.minimum(t/0.002, 1)          # 2 ms pick attack
    rel = np.clip((dur - t)/0.008, 0, 1)  # string muted by next note (8 ms)
    y *= att*rel
    # pick noise burst
    y += 0.05*rng.standard_normal(n)*np.exp(-t/0.004)
    return level*y/np.max(np.abs(y))
MAG = [1, .55, .45, .3, .25, .18, .12, .1, .08, .06]       # magnetic: strong fundamental
EL = [1, .5, .5, .35, .3, .25, .2, .15, .12, .1, .08, .06]       # electric magnetic
PIEZO = [.45, 1, .75, .55, .5, .4, .35, .3, .25, .2, .15]   # piezo/mic: weak fundamental
def place(buf, sig, at):
    i = int(at*SR); buf[i:i+len(sig)] += sig[:len(buf)-i]

def scenario(name, seed=1):
    global rng
    rng = np.random.default_rng(seed)
    notes = []  # (onset, expected_lowest_midi or None)
    if name == 'single':
        seq = [36, 38, 40, 43, 45, 48, 50, 55, 57, 59]   # C2 .. A3, B3 above the A3 cutoff
        buf = np.zeros(int(SR*(len(seq)*1.0+0.5)))
        for i, m in enumerate(seq):
            place(buf, pluck(hz(m), 0.65, MAG), 0.2+i); notes.append((0.2+i, m))
    elif name.startswith('power'):
        # drop D power chords D2+A2+D3, down- and up-strums, fifth +6 dB in half of them
        buf = np.zeros(int(SR*8.5)); prof = PIEZO if 'piezo' in name else MAG
        roots = [38, 38, 41, 43, 36, 38, 45, 40]
        for i, r in enumerate(roots):
            on = 0.2+i; up = i % 2 == 1; loud5 = i >= 4
            voices = [(r, 1.0), (r+7, 2.0 if loud5 else 1.0), (r+12, 1.0)]
            if up: voices = voices[::-1]
            for j, (m, g) in enumerate(voices):
                place(buf, g*pluck(hz(m), 0.95-0.012*j, prof), on+0.012*j)
            notes.append((on + (0.024 if up else 0.0), r))
    elif name == 'std_e':
        # standard tuning, piezo with body knock: phantom low notes are the risk
        seq = [40, 43, 45, 47, 50, 52, 55, 57]
        buf = np.zeros(int(SR*(len(seq)+0.5)))
        for i, m in enumerate(seq):
            s_ = pluck(hz(m), 0.65, PIEZO)
            n_ = int(.05*SR); tt = np.arange(n_)/SR
            s_[:n_] += 0.2*np.sin(2*np.pi*95*tt)*np.exp(-tt/0.015)    # knock below E2
            place(buf, s_, 0.2+i); notes.append((0.2+i, m))
    elif name == 'std_e_thump':
        # standard tuning, magnetic, with a low thump (~65 Hz, e.g. palm slap / stomp)
        seq = [40, 43, 45, 47, 50, 52, 55, 57]
        buf = np.zeros(int(SR*(len(seq)+0.5)))
        for i, m in enumerate(seq):
            s_ = pluck(hz(m), 0.65, MAG)
            n_ = int(.08*SR); tt = np.arange(n_)/SR
            s_[:n_] += 0.25*np.sin(2*np.pi*65*tt)*np.exp(-tt/0.025)
            place(buf, s_, 0.2+i); notes.append((0.2+i, m))
    elif name.startswith('el_sustain'):
        # open low E rings (not muted) while higher notes are played on other strings
        buf = np.zeros(int(SR*3.4))
        place(buf, pluck(hz(40), 3.2, EL, decay=0.8), 0.2)
        seq = [(1.3, 45), (1.8, 50), (2.3, 55), (2.8, 57)]
        notes.append((0.2, 40))
        for j, (on, m) in enumerate(seq):
            dur = (seq[j+1][0] if j+1 < len(seq) else 3.35) - on
            place(buf, pluck(hz(m), dur, EL, decay=1.5, level=0.35), on)
            notes.append((on, 40 if name.endswith('lowest') else m))
    elif name == 'el_chug':
        # palm-muted E2 sixteenths at 150 bpm, with an accented E5 power chord every bar
        s16 = 60/150/4; buf = np.zeros(int(SR*(32*s16+0.5)))
        for i in range(32):
            on = 0.2 + i*s16
            if i % 8 == 0:
                for j, m in enumerate((40, 47, 52)):
                    place(buf, pluck(hz(m), s16*0.95, EL, decay=6), on+0.004*j)
            else:
                place(buf, 0.8*pluck(hz(40), s16*0.95, EL, decay=18), on)
            if i % 8 == 0: notes.append((on, 40))
    elif name == 'el_power':
        buf = np.zeros(int(SR*8.5))
        roots = [40, 43, 45, 38, 40, 47, 45, 43]
        for i, r in enumerate(roots):
            on = 0.2+i; up = i % 2 == 1; loud5 = i >= 4
            voices = [(r, 1.0), (r+7, 2.0 if loud5 else 1.0), (r+12, 1.0)]
            if up: voices = voices[::-1]
            for j, (m, g) in enumerate(voices):
                place(buf, g*pluck(hz(m), 0.95-0.012*j, EL, decay=0.8), on+0.012*j)
            notes.append((on + (0.024 if up else 0.0), r))
    elif name == 'riff':
        bpm = 140; e8 = 60/bpm/2
        seq = [38, 38, 41, 38, 43, 38, 41, 40]*2
        buf = np.zeros(int(SR*(len(seq)*e8+0.6)))
        for i, m in enumerate(seq):
            place(buf, pluck(hz(m), e8*0.95, MAG, decay=3), 0.2+i*e8); notes.append((0.2+i*e8, m))
    elif name == 'piezo':
        seq = [36, 38, 43, 45, 50]
        buf = np.zeros(int(SR*(len(seq)+0.5)))
        for i, m in enumerate(seq):
            s = pluck(hz(m), 0.95, PIEZO)
            thump = 0.15*np.sin(2*np.pi*105*np.arange(int(.05*SR))/SR)*np.exp(-np.arange(int(.05*SR))/SR/0.012)
            s[:len(thump)] += thump
            place(buf, s, 0.2+i); notes.append((0.2+i, m))
    return buf.astype(np.float32), notes

def run(buf, ll, synth=0, oct=-60, cut=57, **kw):
    buf.tofile('/tmp/in.f32')
    args = [f'll={ll}', f'synth={synth}', f'oct={oct}', f'cut={cut}'] + [f'{k}={v}' for k, v in kw.items()]
    subprocess.run(['./run_engine', '/tmp/in.f32', '/tmp/out.f32', '/tmp/log.f32', str(SR)] + args, check=True)
    out = np.fromfile('/tmp/out.f32', np.float32)
    lg = np.fromfile('/tmp/log.f32', np.float32).reshape(-1, 4)
    return out, lg

def pitch_of(seg):
    # FFT peak with parabolic interpolation, 20..250 Hz
    w = seg*np.hanning(len(seg)); N = 1 << 18
    S = np.abs(np.fft.rfft(w, N)); fr = np.fft.rfftfreq(N, 1/SR)
    lo, hi = np.searchsorted(fr, 20), np.searchsorted(fr, 250)
    k = lo + np.argmax(S[lo:hi]); a, b, c = np.log(S[k-1:k+2]+1e-20)
    return fr[k] + (a-c)/(2*(a-2*b+c))*(fr[1]-fr[0])

SC_ARGS = {
    'std_e': dict(tun=0), 'std_e_thump': dict(tun=0),
    'el_chug': dict(inst=1, tun=0, rel=40), 'el_power': dict(inst=1, tun=1),
}
def evaluate(name, ll, verbose=True, seed=1, **kw):
    buf, notes = scenario(name, seed)
    args = dict(SC_ARGS.get(name, {})); args.update(kw)
    if name == 'el_chug': args.setdefault('tone', 0)
    out, lg = run(buf, ll, **args)
    env = np.sqrt(np.convolve(out.astype(np.float64)**2, np.ones(96)/96, 'same'))
    rows = []
    for i, (on, m) in enumerate(notes):
        nxt = notes[i+1][0] if i+1 < len(notes) else len(buf)/SR
        seg_end = min(nxt, on+0.9)
        a, b = int(on*SR), int(seg_end*SR)
        st0 = int((on+0.12)*SR); st1 = int(min(on+0.4, seg_end-0.02)*SR)
        exp_f = hz(m)/2
        if m > 57:   # above cutoff: expect silence
            rows.append((m, None, float(np.max(env[a:b])), None, None)); continue
        if name == 'el_chug':
            # amplitude-pulsed signal: FFT peak is biased by AM sidebands -> median period
            seg = out[st0:st1]; z = np.nonzero((seg[:-1] < 0) & (seg[1:] >= 0))[0]
            f_out = SR/np.median(np.diff(z)) if len(z) > 3 else float('nan')
        else:
            f_out = pitch_of(out[st0:st1]) if st1-st0 > 2000 else float('nan')
        steady = np.median(env[st0:st1])
        ev = lg[a:b, 1]; pk = ev.max()
        idx = np.nonzero(ev > 0.5*pk)[0]
        lat = idx[0]/SR*1000 if len(idx) else float('nan')
        # purity of the first 50 ms of output after it becomes audible (10% env):
        # share of energy NOT explained by harmonics of the correct sub frequency
        idx10 = np.nonzero(env[a:b] > 0.1*steady)[0]
        s0 = a + (idx10[0] if len(idx10) else 0); N = int(0.05*SR)
        seg = out[s0:s0+N].astype(np.float64); tt = np.arange(len(seg))/SR
        cols = []
        for k in range(1, 7):
            for fn in (np.cos, np.sin):
                v = fn(2*np.pi*k*exp_f*tt); cols += [v, v*tt/0.05, v*(tt/0.05)**2]
        M = np.array(cols).T; coef, *_ = np.linalg.lstsq(M, seg, rcond=None)
        wrong = float(np.sum((seg - M@coef)**2)/max(np.sum(seg**2), 1e-20))
        rows.append((m, f_out, lat, 1200*np.log2(f_out/exp_f), wrong))
    if verbose:
        print(f"== {name}  low_latency={ll} {args}")
        for m, f, lat, cents, wrong in rows:
            if f is None: print(f"  midi {m}: above cutoff, peak env {lat:.4f}")
            else: print(f"  midi {m}: sub {f:7.2f} Hz  err {cents:+6.1f} c  onset(50%) {lat:5.1f} ms  impurity {wrong*100:5.1f}% (first 50 ms audible)")
    return rows, out

if __name__ == '__main__':
    for sc in sys.argv[1:] or ['single', 'power', 'power_piezo', 'riff', 'piezo']:
        for ll in (1, 0):
            evaluate(sc, ll)
