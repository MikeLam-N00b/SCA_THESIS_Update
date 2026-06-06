"""
SCA Thesis — Hardware Schematic Overview (Block Diagram)
Components: 12V input, MP2307 DC-DC, AMS1733 LDO, ESP32-S3 Mini, DWM3000 (UWB), MCP2515 (CAN), A7680C (LTE)
"""
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from matplotlib.patches import FancyBboxPatch
import warnings, os
warnings.filterwarnings('ignore')

plt.rcParams.update({"font.family": "DejaVu Sans"})

fig, ax = plt.subplots(figsize=(26, 16))
fig.patch.set_facecolor('white')
ax.set_facecolor('white')
ax.set_xlim(0, 26)
ax.set_ylim(0, 16)
ax.axis('off')

# ── Font sizes ─────────────────────────────────────────────────────────
FS_TITLE  = 18
FS_BLOCK  = 12.5
FS_SUB    = 10.0
FS_SIGNAL = 9.5
FS_PIN    = 8.5
FS_GRP    = 10.0

LW_BOX    = 2.2
LW_PWR    = 2.8
LW_SIG    = 2.2

# ── Helpers ────────────────────────────────────────────────────────────
def blk(cx, cy, w, h, title, sub='', fc='#E3F2FD', ec='#1565C0'):
    ax.add_patch(FancyBboxPatch((cx-w/2, cy-h/2), w, h,
                 boxstyle='round,pad=0.15', facecolor=fc,
                 edgecolor=ec, linewidth=LW_BOX, zorder=4))
    if sub:
        ax.text(cx, cy+0.22, title, ha='center', va='center',
                fontsize=FS_BLOCK, fontweight='bold', color=ec, zorder=5)
        ax.text(cx, cy-0.28, sub, ha='center', va='center',
                fontsize=FS_SUB, color=ec, zorder=5)
    else:
        ax.text(cx, cy, title, ha='center', va='center',
                fontsize=FS_BLOCK, fontweight='bold', color=ec, zorder=5)

def pwr_label(x, y, txt, color='#B71C1C'):
    ax.text(x, y, txt, ha='center', va='center', fontsize=FS_SIGNAL,
            fontweight='bold', color=color, zorder=8,
            bbox=dict(fc='white', ec=color, pad=2.5,
                      linewidth=1.2, boxstyle='round,pad=0.25'))

def hv(pts, color, lw=LW_SIG, dashed=False, bidir=False,
       label='', lbl_idx=0, lbl_side='above'):
    ls = (0, (5, 3)) if dashed else '-'
    xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
    ax.plot(xs, ys, color=color, lw=lw, ls=ls, zorder=3,
            solid_capstyle='round', dash_capstyle='round')

    def tip(pa, pb):
        dx, dy = pb[0]-pa[0], pb[1]-pa[1]
        L = max((dx**2+dy**2)**0.5, 1e-9)
        near = (pb[0]-dx/L*0.001, pb[1]-dy/L*0.001)
        ax.annotate('', xy=pb, xytext=near,
                    arrowprops=dict(arrowstyle='->', color=color,
                                   lw=lw, mutation_scale=18), zorder=6)
    tip(pts[-2], pts[-1])
    if bidir:
        tip(pts[1], pts[0])

    if label:
        p0, p1 = pts[lbl_idx], pts[lbl_idx+1]
        mx, my = (p0[0]+p1[0])/2, (p0[1]+p1[1])/2
        offsets = {'above': (0, 0.28), 'below': (0, -0.28),
                   'right': (0.3, 0),  'left': (-0.3, 0)}
        dx, dy = offsets.get(lbl_side, (0, 0.28))
        ax.text(mx+dx, my+dy, label, ha='center', va='center',
                fontsize=FS_SIGNAL, fontweight='bold', color=color, zorder=7,
                bbox=dict(fc='white', ec='none', pad=1.5))

# ══════════════════════════════════════════════════════════════════════
# TITLE
# ══════════════════════════════════════════════════════════════════════
ax.text(13, 15.5, 'Smart Car Access — Hardware Schematic Overview',
        ha='center', va='center', fontsize=FS_TITLE,
        fontweight='bold', color='#1A237E', zorder=8)

# ══════════════════════════════════════════════════════════════════════
# POWER SUPPLY SECTION  (y ≈ 12.5 – 14.8)
# ══════════════════════════════════════════════════════════════════════
# Group border
ax.add_patch(FancyBboxPatch((0.2, 11.8), 17.4, 3.0,
             boxstyle='round,pad=0.2', facecolor='#FFF8E1',
             edgecolor='#E65100', linewidth=1.8, linestyle='--',
             alpha=0.55, zorder=1))
ax.text(0.5, 14.75, 'Power Supply', ha='left', va='center',
        fontsize=FS_GRP, fontweight='bold', color='#E65100', zorder=6)

# J1 — 12V input
blk(1.8, 13.3, 2.6, 1.0, 'J1', '12V Input',
    fc='#FFEBEE', ec='#B71C1C')

# 12V rail: J1 right → U3
ax.plot([3.1, 4.8], [13.3, 13.3], color='#B71C1C', lw=LW_PWR, zorder=3)
pwr_label(3.95, 13.65, '12V')

# U3 MP2307 — DC-DC buck
blk(6.1, 13.3, 2.6, 1.0, 'U3  MP2307', 'DC-DC Buck',
    fc='#FFF3E0', ec='#E65100')

# 5V rail: U3 right → U1
ax.plot([7.4, 9.0], [13.3, 13.3], color='#B71C1C', lw=LW_PWR, zorder=3)
pwr_label(8.2, 13.65, '5V')

# U1 AMS1733 — LDO 3.3V
blk(10.5, 13.3, 2.7, 1.0, 'U1  AMS1733', 'LDO  →  3V3',
    fc='#E8F5E9', ec='#2E7D32')

# 3V3 rail extending right (dashed)
ax.plot([11.85, 25.5], [13.3, 13.3], color='#B71C1C', lw=2.0,
        ls=(0, (6, 3)), zorder=3)
pwr_label(13.5, 13.65, '3V3 Rail')

# U4 MP2307 — second DC-DC for VCC2 (A7680C needs higher current)
# Branch 12V from J1 line, drop down then right to U4
ax.plot([2.5, 2.5, 4.8], [13.3, 12.2, 12.2], color='#B71C1C', lw=LW_PWR-0.5, zorder=3)
blk(6.1, 12.2, 2.6, 0.9, 'U4  MP2307', 'DC-DC (VCC2)',
    fc='#FFF3E0', ec='#E65100')
# VCC2 rail right (to A7680C)
ax.plot([7.4, 9.0], [12.2, 12.2], color='#B71C1C', lw=LW_PWR-0.5, zorder=3)
pwr_label(8.2, 12.55, 'VCC2')
# VCC2 dashed to A7680C
ax.plot([9.0, 22.0, 22.0], [12.2, 12.2, 9.35],
        color='#B71C1C', lw=1.5, ls=(0, (4, 3)), zorder=3)

# ══════════════════════════════════════════════════════════════════════
# ESP32-S3 MINI  (large center block, x: 1–13.5, y: 1.5–11.2)
# ══════════════════════════════════════════════════════════════════════
MX1, MX2 = 1.0, 13.5
MY1, MY2 = 1.5, 11.2
MCX = (MX1+MX2)/2; MCY = (MY1+MY2)/2

ax.add_patch(FancyBboxPatch((MX1, MY1), MX2-MX1, MY2-MY1,
             boxstyle='round,pad=0.25',
             facecolor='#E8EAF6', edgecolor='#283593',
             linewidth=2.8, zorder=4))
ax.text(MCX, MY2-0.5, 'ESP32-S3 MINI', ha='center', va='center',
        fontsize=15, fontweight='bold', color='#1A237E', zorder=5)
ax.text(MCX, MY2-0.95, 'ESP32S3MINI1N4R2  •  FreeRTOS', ha='center', va='center',
        fontsize=FS_SUB, color='#3949AB', zorder=5)

# 3V3 drop to ESP32 from power rail
hv([(MCX, 13.3), (MCX, MY2)], color='#B71C1C', lw=1.8, dashed=True,
   label='3V3', lbl_idx=0, lbl_side='right')
ax.annotate('', xy=(MCX, MY2), xytext=(MCX, MY2+0.5),
            arrowprops=dict(arrowstyle='->', color='#B71C1C', lw=1.8, mutation_scale=16), zorder=6)

# ── Internal GPIO table ──────────────────────────────────────────────
col_x = [MCX - 3.8, MCX - 1.0, MCX + 1.9]
rows = [
    # (col, y, text)
    (0, 9.5,  'SPI Bus (Shared)'),
    (0, 9.0,  'IO14 — CLK'),
    (0, 8.55, 'IO12 — MISO'),
    (0, 8.1,  'IO13 — MOSI'),
    (0, 7.2,  'CS / IRQ (UWB)'),
    (0, 6.75, 'IO15 — CS_UWB'),
    (0, 6.3,  'IO6  — IRQ_UWB'),
    (0, 5.4,  'CS / INT (CAN)'),
    (0, 4.95, 'IO5  — CS_CAN'),
    (0, 4.5,  'IO4  — INT_CAN'),
    (1, 9.5,  'UART0 (LTE)'),
    (1, 9.0,  'IO1  — TXD'),
    (1, 8.55, 'IO3  — RXD'),
    (1, 7.2,  'Power'),
    (1, 6.75, '3V3, GND'),
    (1, 5.4,  'Boot / RST'),
    (1, 4.95, 'IO0  — BOOT'),
    (1, 4.5,  'RST'),
    (2, 9.5,  'DWM3000 Extra'),
    (2, 9.0,  'RST → DWM RST'),
    (2, 8.55, 'EXTON → DWM EXTON'),
    (2, 7.2,  'MCP2515'),
    (2, 6.75, 'INT → IO4'),
    (2, 5.4,  'A7680C'),
    (2, 4.95, 'RXL → IO3'),
    (2, 4.5,  'TX  → IO1'),
]

HDR_YS = {9.5, 7.2, 5.4}
for col, y, txt in rows:
    x = col_x[col]
    bold = (y in HDR_YS)
    fs   = FS_PIN + (0.5 if bold else 0)
    color = '#1A237E' if bold else '#37474F'
    ax.text(x, y, txt, ha='left', va='center',
            fontsize=fs, fontweight='bold' if bold else 'normal',
            color=color, zorder=5)

# Divider lines
for dy in [7.45, 5.65]:
    ax.plot([MX1+0.4, MX2-0.3], [dy, dy],
            color='#9FA8DA', lw=0.9, ls='--', zorder=4)

# Task list bottom of MCU
ax.text(MCX, MY1+0.55,
        'Tasks: bleTask (Core0/P3)  •  uwbTask (Core1/P4)  •  '
        'canTask (Core1/P2)  •  friendMgmtTask (Core0/P2)  •  '
        'simInitTask (Core0/P1)  •  revocationSyncTask (Core0/P1)',
        ha='center', va='center', fontsize=7.8, color='#455A64', zorder=5,
        style='italic',
        bbox=dict(fc='#ECEFF1', ec='#90A4AE', pad=3.5, linewidth=1))

# ══════════════════════════════════════════════════════════════════════
# DWM3000 — UWB Module  (top-right)
# ══════════════════════════════════════════════════════════════════════
DW_CX, DW_CY, DW_W, DW_H = 20.5, 9.8, 4.8, 2.4
blk(DW_CX, DW_CY, DW_W, DW_H, 'DWM3000', 'UWB Module  (DW3000)',
    fc='#F3E5F5', ec='#6A1B9A')
# Pin labels inside
for i, pin in enumerate(['SPI: CLK / MOSI / MISO', 'CS (SPICS)', 'IRQ (GPIO8/IRQ)',
                          'RST,  EXTON,  WAKEUP']):
    ax.text(DW_CX, DW_CY + 0.55 - i*0.37, pin, ha='center', va='center',
            fontsize=7.8, color='#4A148C', zorder=5)

# SPI1 arrow ESP32 → DWM3000
hv([(MX2, 9.8), (DW_CX - DW_W/2, DW_CY)],
   color='#6A1B9A', lw=LW_SIG, bidir=True,
   label='SPI1 + RST/IRQ', lbl_side='above')

# 3V3 drop to DWM3000
hv([(DW_CX, 13.3), (DW_CX, DW_CY + DW_H/2)],
   color='#B71C1C', lw=1.4, dashed=True, label='3V3', lbl_idx=0, lbl_side='right')

# ══════════════════════════════════════════════════════════════════════
# A7680C — LTE Module  (middle-right)
# ══════════════════════════════════════════════════════════════════════
LT_CX, LT_CY, LT_W, LT_H = 20.5, 6.7, 4.8, 2.2
blk(LT_CX, LT_CY, LT_W, LT_H, 'A7680C', 'LTE / 4G Module',
    fc='#E8F5E9', ec='#1B5E20')
for i, pin in enumerate(['UART: TXD ↔ IO1,  RXD ↔ IO3', 'VCC (VCC2),  GND',
                          'RST,  STATUS']):
    ax.text(LT_CX, LT_CY + 0.42 - i*0.40, pin, ha='center', va='center',
            fontsize=7.8, color='#1B5E20', zorder=5)

# UART arrow ESP32 ↔ A7680C
hv([(MX2, 6.7), (LT_CX - LT_W/2, LT_CY)],
   color='#1B5E20', lw=LW_SIG, bidir=True,
   label='UART0  (AT commands)', lbl_side='above')

# VCC2 arrow tip into A7680C (line drawn in power section)
ax.annotate('', xy=(LT_CX, LT_CY + LT_H/2),
            xytext=(LT_CX, LT_CY + LT_H/2 + 0.5),
            arrowprops=dict(arrowstyle='->', color='#B71C1C', lw=1.4, mutation_scale=14), zorder=6)
ax.text(LT_CX+0.25, LT_CY + LT_H/2 + 0.25, 'VCC2', ha='left', va='center',
        fontsize=8, color='#B71C1C', fontweight='bold', zorder=7)

# ══════════════════════════════════════════════════════════════════════
# MCP2515 — CAN Controller  (lower-right)
# ══════════════════════════════════════════════════════════════════════
CP_CX, CP_CY, CP_W, CP_H = 19.5, 3.7, 4.2, 2.2
blk(CP_CX, CP_CY, CP_W, CP_H, 'MCP2515', 'CAN Bus Controller',
    fc='#FFF8E1', ec='#F57F17')
for i, pin in enumerate(['SPI: CLK / MOSI / MISO  (shared)', 'CS → IO5,   INT → IO4',
                          '3V3,  GND']):
    ax.text(CP_CX, CP_CY + 0.42 - i*0.40, pin, ha='center', va='center',
            fontsize=7.8, color='#E65100', zorder=5)

# SPI2 arrow ESP32 → MCP2515
hv([(MX2, 3.7), (CP_CX - CP_W/2, CP_CY)],
   color='#F57F17', lw=LW_SIG, bidir=False,
   label='SPI2 (shared bus) + INT', lbl_side='above')

# 3V3 drop to MCP2515
hv([(CP_CX-0.8, 13.3), (CP_CX-0.8, CP_CY + CP_H/2)],
   color='#B71C1C', lw=1.4, dashed=True, label='3V3', lbl_idx=0, lbl_side='right')

# CAN Transceiver + Bus connector
CB_CX, CB_CY, CB_W, CB_H = 24.2, 3.7, 2.8, 1.8
blk(CB_CX, CB_CY, CB_W, CB_H, 'CAN Bus', 'CAN H / CAN L',
    fc='#FCE4EC', ec='#C62828')
hv([(CP_CX + CP_W/2, CP_CY), (CB_CX - CB_W/2, CB_CY)],
   color='#C62828', lw=LW_SIG,
   label='CAN H / CAN L', lbl_side='above')

# ══════════════════════════════════════════════════════════════════════
# Shared SPI note
# ══════════════════════════════════════════════════════════════════════
ax.text(MX2 + 0.1, 2.5,
        'Shared SPI bus: IO12(MISO) IO13(MOSI) IO14(CLK)\n'
        'spiMutex — uwbTask timeout 500ms, canTask timeout 2000ms',
        ha='left', va='center', fontsize=8.2, color='#37474F', zorder=5,
        style='italic',
        bbox=dict(fc='#ECEFF1', ec='#90A4AE', pad=4, linewidth=1, boxstyle='round,pad=0.3'))

# ══════════════════════════════════════════════════════════════════════
# Legend
# ══════════════════════════════════════════════════════════════════════
legend_items = [
    mpatches.Patch(fc='#FFEBEE', ec='#B71C1C', label='12V Power Input (J1)'),
    mpatches.Patch(fc='#FFF3E0', ec='#E65100', label='DC-DC Buck (MP2307)'),
    mpatches.Patch(fc='#E8F5E9', ec='#2E7D32', label='LDO 3.3V (AMS1733)'),
    mpatches.Patch(fc='#E8EAF6', ec='#283593', label='MCU (ESP32-S3 Mini)'),
    mpatches.Patch(fc='#F3E5F5', ec='#6A1B9A', label='UWB Module (DWM3000) — SPI1'),
    mpatches.Patch(fc='#E8F5E9', ec='#1B5E20', label='LTE Module (A7680C) — UART'),
    mpatches.Patch(fc='#FFF8E1', ec='#F57F17', label='CAN Controller (MCP2515) — SPI2'),
    mpatches.Patch(fc='#FCE4EC', ec='#C62828', label='CAN Bus (H/L)'),
]
ax.legend(handles=legend_items, loc='lower left', fontsize=9.5,
          framealpha=0.97, edgecolor='#BDBDBD',
          bbox_to_anchor=(0.005, 0.005))

# ══════════════════════════════════════════════════════════════════════
# Save
# ══════════════════════════════════════════════════════════════════════
out = r'D:\SCA\Diagram\schematic_overview.png'
plt.savefig(out, dpi=180, bbox_inches='tight',
            facecolor='white', edgecolor='none')
plt.close()
print(f'Saved: {out}')
os.startfile(out)
