import numpy as np, sys
import gen_and_eval as G
SCEN = sys.argv[1:] or ['single', 'piezo', 'power', 'power_piezo', 'std_e', 'el_chug', 'el_power']
tot = {}
for ll in (1, 0):
    lat = []; wrong = 0; n = 0; imp = []; W = {}
    for sc in SCEN:
      for seed in (1, 2, 3):
        buf, notes = G.scenario(sc, seed); G.scenario_cache = (buf, notes)
        rows, _ = G.evaluate(sc, ll, verbose=False, seed=seed)
        for m, f, l, c, w in rows:
            if f is None: continue
            n += 1
            if abs(c) > 50: wrong += 1; W[sc]=W.get(sc,0)+1
            else:
                imp.append(w)
                if l > 1: lat.append(l)
    print(f"low_latency={ll}: onset50 mean {np.mean(lat):5.1f} ms (max {np.max(lat):5.1f})  wrong notes {wrong}/{n}  impurity mean {100*np.mean(imp):4.1f}% max {100*np.max(imp):4.1f}%  {W}")
