"""Plots from the 2026-11-12 bench night.

Decode the card first, then point this at the folder of LOGnnnn folders:
    python3 tools/hat_log.py /Volumes/CARD/LOG*.BIN --out decoded
    python3 docs/bench-2026-11-12/plots.py decoded docs/bench-2026-11-12
Needs pandas and matplotlib.
"""
import sys, pandas as pd, numpy as np, matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from pathlib import Path
D, OUT = Path(sys.argv[1]), Path(sys.argv[2])

# hat_log.py puts channel names in the headers (tc1_tank_top_c), so columns go by position.
LC_COLS = ["time_s", "code", "mv_per_v", "newtons"]
COLS = {
    "lc": LC_COLS,
    "lc2": LC_COLS,
    "tc": ["time_s"] + [f"tc{t}_{c}" for t in range(1, 5) for c in ("c", "cold_junction_c", "faults")],
    "actuators": ["time_s", "interlock", "vbat_v"] + [f"sol{n}" for n in range(1, 5)]
                 + [f"srv{n}_us" for n in range(1, 5)] + ["stepper_position", "stepper_awake"],
}
C1, C2, C3, MUTED, INK, GRID = "#2a78d6", "#eb6834", "#1baf7a", "#8a8984", "#0b0b0b", "#e6e5e1"
plt.rcParams.update({"font.size": 10, "axes.edgecolor": MUTED, "axes.labelcolor": INK,
    "xtick.color": "#52514e", "ytick.color": "#52514e", "axes.grid": True, "grid.color": GRID,
    "axes.spines.top": False, "axes.spines.right": False, "lines.linewidth": 1.2,
    "figure.facecolor": "#fcfcfb", "axes.facecolor": "#fcfcfb", "axes.titleweight": "bold",
    "axes.titlesize": 11, "axes.titlelocation": "left", "legend.frameon": False})
def r(log, name):
    df = pd.read_csv(D/log/f"{name}.csv")
    if name in COLS:
        columns = list(df.columns)
        columns[:len(COLS[name])] = COLS[name]   # later formats add columns at the end
        df.columns = columns
    return df

def save(fig, name):
    fig.savefig(OUT/name, dpi=150, bbox_inches="tight"); plt.close(fig); print("wrote", name)

# 1. LOG0059: final run, both cells
lc, lc2 = r("LOG0059","lc"), r("LOG0059","lc2")
fig, ax = plt.subplots(figsize=(11, 4.2))
ax.plot(lc.time_s, lc.newtons, color=C1, lw=0.6, label=f"LC1 rocket, 1 kHz ({len(lc):,} samples)")
ax.plot(lc2.time_s, lc2.newtons, color=C2, lw=1.4, label=f"LC2 supply, ~10 Hz ({len(lc2):,} samples)")
ax.set(xlabel="time since boot (s)", ylabel="force (N)",
       title="LOG0059: both load cells logged to SD for 9.4 minutes")
ax.legend(loc="upper left"); save(fig, "1-loadcells-LOG0059.png")

# 2. Zoom: 1 kHz detail of a load step + unloaded noise
lc = r("LOG0059","lc")
d = lc.newtons.diff().abs().rolling(50).sum()
t_step = lc.time_s[d.idxmax()]
z = lc[(lc.time_s > t_step-1.5) & (lc.time_s < t_step+1.5)]
quiet = r("LOG0055","lc")
qn = quiet.newtons
fig, (a, b) = plt.subplots(1, 2, figsize=(11, 3.8), gridspec_kw={"width_ratios":[1.4,1]})
a.plot(z.time_s, z.newtons, color=C1, lw=0.8)
a.set(xlabel="time since boot (s)", ylabel="force (N)", title=f"LC1 at 1 kHz around a load change ({t_step:.1f} s)")
b.plot(quiet.time_s - quiet.time_s.iloc[0], qn, color=C1, lw=0.4)
b.set(xlabel="time (s)", ylabel="force (N)", title=f"LC1 unloaded, LOG0055: noise {qn.std():.2f} N rms")
b.set_ylim(qn.mean()-5, qn.mean()+5)
save(fig, "2-loadcell-detail.png")

# 3. LOG0037: rocket cell pushes
lc = r("LOG0037","lc")
fig, ax = plt.subplots(figsize=(11, 3.8))
ax.plot(lc.time_s, lc.newtons, color=C1, lw=0.6)
ax.set(xlabel="time since boot (s)", ylabel="force (N)",
       title=f"LOG0037: LC1 (rocket) at 1 kHz, {len(lc):,} samples in {lc.time_s.iloc[-1]-lc.time_s.iloc[0]:.0f} s, peak {lc.newtons.max():.0f} N")
save(fig, "3-loadcell-LC1-LOG0037.png")

# 4. Brownouts: aligned on the VBAT dip
logs = ["LOG0007","LOG0010","LOG0012","LOG0024","LOG0034","LOG0036","LOG0041","LOG0044","LOG0046","LOG0050","LOG0051","LOG0053"]
fig, (a, b) = plt.subplots(2, 1, figsize=(11, 5.8), sharex=True, gridspec_kw={"height_ratios":[2,1]})
gaps = []
for i, log in enumerate(logs):
    x = r(log, "actuators")
    t0 = x.time_s[x.vbat_v.idxmin()]
    w = x[(x.time_s > t0-1) & (x.time_s < t0+2)]
    gaps.append(x.time_s.iloc[-1]-t0)
    a.plot(w.time_s-t0, w.vbat_v, color=C1, lw=1, alpha=0.7, label="VBAT, one line per log" if i == 0 else None)
    b.plot(w.time_s-t0, w.srv4_us, color=C2, lw=1, alpha=0.7)
    if gaps[-1] < 2:
        a.plot(gaps[-1], w.vbat_v.iloc[-1], "o", color=INK, ms=5, label="log ends (last SD flush)" if i == 0 else None)
quick = sum(g < 1.2 for g in gaps)
a.set(ylabel="VBAT (V)", title=f"Brownouts: an SRV4 (MOV) move drops VBAT to 8-9.5 V; {quick} of {len(logs)} logs end within 1.2 s (SD flushes every 1 s)")
a.legend(loc="lower right"); b.set(ylabel="SRV4 pulse (us)", xlabel="time from the VBAT dip (s), VBAT sampled every 10 ms")
save(fig, "4-brownouts-srv4.png")
print("gaps", [f"{l}:{g:.2f}" for l, g in zip(logs, gaps)])

# 5. After: LOG0057, SRV4 1000/2000 swings, no reset
x = r("LOG0057","actuators")
fig, (a, b) = plt.subplots(2, 1, figsize=(11, 4.8), sharex=True, gridspec_kw={"height_ratios":[2,1]})
a.plot(x.time_s, x.vbat_v, color=C1, lw=0.8); a.set(ylabel="VBAT (V)",
    title=f"LOG0057: 11 SRV4 moves over 2 minutes, VBAT dips to {x.vbat_v.min():.1f} V and the board stays up")
b.plot(x.time_s, x.srv4_us, color=C2, lw=1.2); b.set(ylabel="SRV4 (us)", xlabel="time since boot (s)")
save(fig, "5-srv4-no-brownout-LOG0057.png")

# 6. Thermocouples: what was actually logged
tc = r("LOG0059","tc")
fig, (a, b) = plt.subplots(1, 2, figsize=(12, 3.8), gridspec_kw={"wspace":0.3})
t58 = r("LOG0058","tc")
for i, c in zip((1,2,4), (C1,C2,C3)):
    a.plot(t58.time_s, t58[f"tc{i}_cold_junction_c"], color=c, label=f"TC{i}")
a.set(xlabel="time since boot (s)", ylabel="cold junction (C)", title="LOG0058: MAX31856 cold-junction temps, 10 Hz")
a.legend(loc="center right")
state = np.where(tc.tc1_faults == 65, 2, np.where((tc.tc1_c == 0) & (tc.tc1_cold_junction_c == 0), 0, 1))
b.step(tc.time_s, state, color=C1, where="post")
b.set_yticks([0,1,2], ["zeros", "valid", "open"]); b.set_ylabel("TC1 state")
b.set(xlabel="time since boot (s)", title="LOG0059 TC1 (TC2-4 the same): open probe, then all-zero reads")
save(fig, "6-thermocouples.png")
