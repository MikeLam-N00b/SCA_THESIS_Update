#!/usr/bin/env python3
"""
SCA Anchor — Current Consumption Profile
Data source: INA226 Serial Monitor log (14:40:44 session)
X-axis: seconds from boot; Y-axis: mA (actual measured)
"""

import os
import numpy as np
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
def savgol_simple(y, window=5):
    """Moving average smoothing, edge-padded."""
    hw = window // 2
    out = np.empty_like(y, dtype=float)
    for i in range(len(y)):
        lo, hi = max(0, i - hw), min(len(y), i + hw + 1)
        out[i] = y[lo:hi].mean()
    return out

OUT = r"D:\SCA\Diagram"
os.makedirs(OUT, exist_ok=True)

plt.rcParams.update({
    "font.family": "DejaVu Sans",
    "figure.facecolor": "white",
    "axes.facecolor": "#FAFAFA",
    "axes.grid": True,
    "grid.linestyle": "--",
    "grid.alpha": 0.35,
    "grid.color": "#BBBBBB",
})

# ─── Palette ──────────────────────────────────────────────────────────────────
C_IDLE  = "#546E7A"
C_SIM   = "#E65100"   # LTE/SIM active
C_UWB   = "#4527A0"   # UWB ranging
C_LINE  = "#1A237E"   # measurement line

# ─── Actual measured data (t=seconds from 14:40:44.139, I=mA) ─────────────────
# Source: INA226 0.1Ω shunt, sampled ~500ms, deadband 1.0mA
# Three readings at t≈2.79s are startup burst — spread slightly
raw = [
    # t(s)    I(mA)
    (2.789,  28.90),   # boot burst (lowest)
    (3.131,  37.66),
    (3.670,  37.32),
    (4.169,  36.76),
    (4.665,  36.66),
    (5.181,  36.36),
    (5.666,  36.46),
    (6.146,  36.50),
    (6.668,  36.06),
    (7.167,  36.48),
    (7.687,  37.06),
    (8.191,  36.30),
    (8.690,  35.36),
    (9.189,  36.50),
    (9.688,  36.70),
    (10.160, 37.38),
    (10.705, 36.08),
    (11.192, 36.16),
    (11.710, 36.66),
    (12.170, 36.28),
    (12.680, 37.12),   # ← BLE Tag connected (gen=1) at t=12.52s
    (13.219, 36.30),   #   SIM init starts, no current change yet
    (13.710, 35.72),
    (14.207, 56.76),   # ← SIM LTE active (AT+CGACT=1,1) — instant jump
    (14.720, 50.78),
    (15.199, 51.72),
    (15.683, 52.18),
    (16.202, 51.42),
    (16.703, 50.78),
    (17.206, 52.00),
    (17.691, 51.68),
    (18.213, 50.90),
    (18.695, 51.42),
    (19.218, 54.92),
    (19.699, 52.46),
    (20.204, 61.06),   # HTTP POST /secure-check-pairing
    (20.722, 62.06),   # ← peak SIM (AT+CFUN=0 issued at t=20.41s)
    (21.241, 55.08),   # SIM powering down
    (21.744, 40.78),   # ← SIM sleep confirmed
    (22.230, 36.36),   # BLE gen=2 connected at t=21.84s, key from NVS
    (22.711, 36.38),
    (23.241, 36.16),
    (23.715, 36.08),   # Auth challenge sent at t=23.80s
    (24.256, 42.06),   # Auth OK t=23.95s; UWB init t=24.09s; CAN unlock t=24.21s
    (24.763, 49.18),
    (25.221, 54.46),   # UWB STS mode 1 active, SS-TWR loop
    (25.764, 54.38),
    (26.250, 54.48),
    (26.750, 54.96),
    (27.253, 53.96),
    (27.755, 54.88),
    (28.265, 54.50),
    (28.763, 54.96),
    (29.252, 54.58),
    (29.759, 53.72),
    (30.242, 53.90),
    (30.771, 54.68),
    (31.285, 55.00),
    (31.773, 55.10),
    (32.279, 54.62),
    (32.751, 54.66),
    (33.254, 54.78),
    (33.775, 54.76),
    (34.275, 54.30),
    (34.761, 55.32),
    (35.281, 54.86),
    (35.800, 55.20),
    (36.303, 54.98),
    (36.768, 54.90),
    (37.270, 55.52),
    (37.771, 55.18),
    (38.291, 55.92),
    (38.788, 54.10),
    (39.310, 54.16),
    (39.812, 55.26),
    (40.290, 54.70),
    (40.817, 57.82),   # current drifting up (thermal / STS re-sync)
    (41.323, 58.78),
    (41.824, 58.82),
    (42.291, 58.46),
    (42.792, 60.08),
    (43.313, 59.42),
    (43.797, 60.42),
    (44.330, 61.22),
    (44.839, 61.62),
    (45.303, 61.56),
    (45.822, 62.52),
    (46.306, 62.70),   # ← peak UWB; Tag disconnects at t=46.40s
    (46.826, 43.90),   # UWB stopped at t=46.52s
    (47.314, 38.02),
    (47.838, 37.88),
    (48.350, 37.36),
    (48.828, 38.46),
    (49.356, 37.56),
    (49.848, 37.92),
    (50.323, 38.02),
    (50.825, 38.10),
    (51.352, 38.40),
    (51.854, 39.02),
    (52.363, 38.42),
    (52.833, 38.62),
    (53.367, 37.72),
    (53.837, 37.88),
    (54.362, 38.48),
    (54.886, 37.56),
    (55.358, 38.56),
    (55.847, 38.16),
    (56.375, 38.10),
    (56.890, 37.92),
    (57.380, 38.00),
    (57.883, 38.22),
    (58.399, 37.98),
    (58.895, 37.76),
    (59.381, 38.42),
]

T0 = 2.789  # seconds from boot to first INA226 reading (MCU init time)

t_meas = np.array([r[0] for r in raw]) - T0
I_meas = np.array([r[1] for r in raw])

# ─── Plot ─────────────────────────────────────────────────────────────────────
fig, ax = plt.subplots(figsize=(15, 6))

# Phase background shading
phases = [
    (0,              12.52 - T0,  "#ECEFF1", "IDLE\n(BLE adv)"),
    (12.52 - T0,     14.21 - T0,  "#E3F2FD", "BLE\nconnect"),
    (14.21 - T0,     21.74 - T0,  "#FFF3E0", "SIM LTE ACTIVE"),
    (21.74 - T0,     24.26 - T0,  "#ECEFF1", "IDLE"),
    (24.26 - T0,     46.40 - T0,  "#EDE7F6", "UWB SS-TWR RANGING + BLE AUTH"),
    (46.40 - T0,     58.0,        "#ECEFF1", "IDLE\n(BLE adv)"),
]
for (x0, x1, fc, _) in phases:
    ax.axvspan(x0, x1, color=fc, alpha=0.7, zorder=0)

# Raw measured data — thin connecting line + prominent dots
ax.plot(t_meas, I_meas, color=C_LINE, lw=1.0, alpha=0.45, zorder=2)
ax.scatter(t_meas, I_meas, color=C_LINE, s=28, zorder=4, alpha=0.92)

# ─── Event annotations ────────────────────────────────────────────────────────
events = [
    # (t_s,            label,                   x_current,  y_text,  ha)
    (12.52 - T0,  "BLE Tag\nconnected",      36.5,  50, "center"),
    (14.21 - T0,  "SIM LTE\nactive",         56.76, 70, "center"),
    (20.41 - T0,  "AT+CFUN=0\n(SIM sleep)",  61.06, 77, "center"),
    (21.84 - T0,  "BLE gen=2\n+ key NVS",    40.78, 50, "center"),
    (24.09 - T0,  "UWB init\n+ CAN unlock",  42.06, 27, "center"),
    (40.5  - T0,  "Thermal drift\n(~8 mA)",  58.0,  70, "center"),
    (46.40 - T0,  "Tag disconnect\nUWB stop", 62.70, 77, "center"),
]

ann_colors = {
    "BLE Tag\nconnected":      "#1565C0",
    "SIM LTE\nactive":         C_SIM,
    "AT+CFUN=0\n(SIM sleep)":  C_SIM,
    "BLE gen=2\n+ key NVS":    "#1565C0",
    "UWB init\n+ CAN unlock":  C_UWB,
    "Thermal drift\n(~8 mA)":  "#555555",
    "Tag disconnect\nUWB stop": C_UWB,
}

for (tx, lbl, I_tip, y_txt, ha) in events:
    ec = ann_colors.get(lbl, "#333333")
    ax.annotate(
        lbl,
        xy=(tx, I_tip),
        xytext=(tx, y_txt),
        fontsize=8,
        fontweight="bold",
        color=ec,
        ha=ha,
        va="bottom",
        arrowprops=dict(arrowstyle="-|>", color=ec, lw=1.2),
        bbox=dict(boxstyle="round,pad=0.22", fc="white", ec=ec, lw=1.0, alpha=0.93),
        zorder=5,
    )

# ─── Horizontal reference lines ───────────────────────────────────────────────
refs = [
    (36.5,  "BLE adv idle ~36 mA",  C_IDLE),
    (54.5,  "UWB ranging ~54 mA",   C_UWB),
    (62.7,  "Peak ~62.7 mA",        "#B71C1C"),
]
for (yv, lbl, col) in refs:
    ax.axhline(yv, color=col, lw=0.9, ls=":", alpha=0.7, zorder=1)
    ax.text(55.5, yv, lbl, fontsize=7.5, color=col, va="center",
            fontweight="bold")

# ─── Phase labels (top of shading) ────────────────────────────────────────────
phase_labels = [
    (4.5,   "IDLE",               C_IDLE),
    (15.0,  "SIM LTE",            C_SIM),
    (20.0,  "IDLE",               C_IDLE),
    (32.5,  "UWB SS-TWR RANGING", C_UWB),
    (50.5,  "IDLE",               C_IDLE),
]
for (xm, lbl, col) in phase_labels:
    ax.text(xm, 64.5, lbl, fontsize=8.5, color=col, ha="center",
            fontweight="bold", alpha=0.85)

# ─── Axes ─────────────────────────────────────────────────────────────────────
ax.set_xlim(0, 58)
ax.set_ylim(20, 83)
ax.set_xlabel("Thời gian (giây từ lúc khởi động)", fontsize=11)
ax.set_ylabel("Dòng điện (mA)", fontsize=11)
ax.set_title(
    "SCA Anchor — Biểu đồ tiêu thụ dòng điện (đo thực tế INA226)\n"
    "ESP32 + A7680C LTE + DW3000 UWB + MCP2515 CAN  |  FreeRTOS",
    fontsize=12, fontweight="bold", pad=10,
)

# X ticks at key events
_s = lambda v: round(v - T0, 1)
xticks = [0, _s(12.52), _s(14.21), _s(21.74), _s(24.09), _s(40.5), _s(46.40), 58]
xlbls  = ["0",
          f"{_s(12.52)}\nBLE conn",
          f"{_s(14.21)}\nSIM on",
          f"{_s(21.74)}\nSIM off",
          f"{_s(24.09)}\nUWB+CAN",
          f"{_s(40.5)}",
          f"{_s(46.40)}\nStop",
          "58"]
ax.set_xticks(xticks)
ax.set_xticklabels(xlbls, fontsize=8, rotation=30, ha="right")

ax.set_yticks(range(20, 84, 5))

# ─── Legend ───────────────────────────────────────────────────────────────────
legend_items = [
    mpatches.Patch(facecolor="#ECEFF1", edgecolor=C_IDLE,  label="Idle — BLE advertising (~36 mA)"),
    mpatches.Patch(facecolor="#FFF3E0", edgecolor=C_SIM,   label="SIM LTE active — HTTP + NTP (~51–62 mA)"),
    mpatches.Patch(facecolor="#EDE7F6", edgecolor=C_UWB,   label="UWB DW3000 SS-TWR ranging (~54–63 mA)"),
]
ax.legend(handles=legend_items, loc="lower right", fontsize=8.5,
          framealpha=0.96, edgecolor="#CCCCCC")

plt.tight_layout()
out_path = os.path.join(OUT, "current_profile.png")
plt.savefig(out_path, dpi=150, bbox_inches="tight")
plt.close()
print(f"Saved: {out_path}")
