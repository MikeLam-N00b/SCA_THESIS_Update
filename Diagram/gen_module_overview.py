"""
SCA Thesis — Module Overview: TAG vs ANCHOR
Simple block diagram showing what's inside each hardware module.
"""
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from matplotlib.patches import FancyBboxPatch
import warnings, os
warnings.filterwarnings('ignore')

plt.rcParams.update({"font.family": "DejaVu Sans"})

fig, ax = plt.subplots(figsize=(25, 16))
fig.patch.set_facecolor('white')
ax.set_facecolor('white')
ax.set_xlim(0, 23)
ax.set_ylim(-1.5, 13.0)
ax.axis('off')

FS_TITLE   = 22
FS_MODULE  = 20
FS_BLOCK   = 18
FS_LABEL   = 15
LW_MODULE  = 2.8
LW_BLOCK   = 2.0
LW_CONN    = 2.2

# ── Helpers ────────────────────────────────────────────────────────────────────
def module_box(x1, y1, x2, y2, label, fc, ec):
    ax.add_patch(FancyBboxPatch((x1, y1), x2-x1, y2-y1,
                 boxstyle='round,pad=0.25',
                 facecolor=fc, edgecolor=ec,
                 linewidth=LW_MODULE, zorder=2))
    ax.text((x1+x2)/2, y2-0.45, label,
            ha='center', va='center',
            fontsize=FS_MODULE, fontweight='bold', color=ec, zorder=6)

def chip(cx, cy, w, h, title, sub='', fc='white', ec='#333'):
    ax.add_patch(FancyBboxPatch((cx-w/2, cy-h/2), w, h,
                 boxstyle='round,pad=0.12',
                 facecolor=fc, edgecolor=ec,
                 linewidth=LW_BLOCK, zorder=4))
    if sub:
        ax.text(cx, cy+0.2,  title, ha='center', va='center',
                fontsize=FS_BLOCK, fontweight='bold', color=ec, zorder=5)
        ax.text(cx, cy-0.25, sub,   ha='center', va='center',
                fontsize=FS_LABEL,  color=ec, zorder=5)
    else:
        ax.text(cx, cy, title, ha='center', va='center',
                fontsize=FS_BLOCK, fontweight='bold', color=ec, zorder=5)

def arrow(pts, color, lw=LW_CONN, bidir=False, dashed=False,
          label='', lbl_idx=0, lbl_side='above'):
    ls = (0, (5, 3)) if dashed else '-'
    xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
    ax.plot(xs, ys, color=color, lw=lw, ls=ls, zorder=3,
            solid_capstyle='round', dash_capstyle='round')

    def tip(pa, pb):
        dx, dy = pb[0]-pa[0], pb[1]-pa[1]
        L = max((dx**2+dy**2)**0.5, 1e-9)
        nb = (pb[0]-dx/L*0.001, pb[1]-dy/L*0.001)
        ax.annotate('', xy=pb, xytext=nb,
                    arrowprops=dict(arrowstyle='->', color=color,
                                   lw=lw, mutation_scale=20), zorder=7)
    tip(pts[-2], pts[-1])
    if bidir:
        tip(pts[1], pts[0])

    if label:
        p0, p1 = pts[lbl_idx], pts[lbl_idx+1]
        mx, my = (p0[0]+p1[0])/2, (p0[1]+p1[1])/2
        off = {'above': (0, .32), 'below': (0, -.32),
               'right': (.32, 0), 'left':  (-.32, 0)}
        dx, dy = off.get(lbl_side, (0, .32))
        ax.text(mx+dx, my+dy, label, ha='center', va='center',
                fontsize=FS_LABEL+0.5, fontweight='bold', color=color, zorder=8,
                bbox=dict(fc='white', ec='none', pad=1.5))

# ══════════════════════════════════════════════════════════════════════════════
# TITLE
# ══════════════════════════════════════════════════════════════════════════════
ax.text(11.5, 12.5, 'Smart Car Access — Hardware Module Overview',
        ha='center', va='center', fontsize=FS_TITLE,
        fontweight='bold', color='#1A237E', zorder=9)

# ══════════════════════════════════════════════════════════════════════════════
# TAG MODULE  (left, x: 0.4–6.8, y: 1.0–11.5)
# ══════════════════════════════════════════════════════════════════════════════
module_box(0.4, 1.0, 6.8, 11.5, 'TAG  (Key Fob)',
           fc='#FFF8E1', ec='#E65100')

# USB Interface at bottom
chip(3.6, 3.0, 4.8, 1.1, 'USB Interface', fc='#FFCCBC', ec='#BF360C')

# MCU centre
chip(3.6, 5.5, 4.8, 1.6, 'Microcontroller', fc='#E8EAF6', ec='#283593')

# BLE and UWB side by side at top — both connect directly to MCU
chip(2.0, 8.5, 2.4, 1.2, 'BLE\nModule',  fc='#E3F2FD', ec='#1565C0')
chip(5.2, 8.5, 2.4, 1.2, 'UWB\nModule',  fc='#F3E5F5', ec='#6A1B9A')

# Internal arrows (Tag)
# MCU ↔ USB
arrow([(3.6, 4.7), (3.6, 3.55)], color='#888', bidir=True)
# BLE ↔ MCU: straight vertical (cx=2.0 falls within MCU width 1.2–6.0)
arrow([(2.0, 7.9), (2.0, 6.3)], color='#888', bidir=True)
# UWB ↔ MCU: straight vertical (cx=5.2 falls within MCU width 1.2–6.0)
arrow([(5.2, 7.9), (5.2, 6.3)], color='#888', bidir=True)

# ══════════════════════════════════════════════════════════════════════════════
# ANCHOR MODULE — MCU at centre, star topology
#   BLE above MCU, UWB left, LTE right, CAN below MCU
#   Power Supply at bottom feeding all modules directly
# ══════════════════════════════════════════════════════════════════════════════
module_box(8.0, 1.0, 20.0, 11.5, 'ANCHOR  (In-Vehicle)',
           fc='#E8F5E9', ec='#1B5E20')

# centre x shared by MCU / BLE / LTE
A_MCX = 14.0
# middle y shared by MCU / UWB / CAN
A_CY  = 6.5

MCU_W, MCU_H = 2.8, 1.6
PER_W, PER_H = 2.6, 1.6
BLE_W, BLE_H = 2.6, 1.2

UWB_CX = 9.8
CAN_CX = 18.2   # CAN Bus Module on the right
BLE_CY = 9.2
LTE_CY = 3.9    # LTE Module below MCU
PS_CY  = 2.0

# derived edges
MCU_L = A_MCX - MCU_W/2;  MCU_R = A_MCX + MCU_W/2
MCU_B = A_CY  - MCU_H/2;  MCU_T = A_CY  + MCU_H/2  # 5.7, 7.3
UWB_R = UWB_CX + PER_W/2;                           # 12.6
CAN_L = CAN_CX - PER_W/2;  CAN_R = CAN_CX + PER_W/2  # 18.4, 21.0
BLE_B = BLE_CY - BLE_H/2                            # 8.6
LTE_T = LTE_CY + BLE_H/2;  LTE_B = LTE_CY - BLE_H/2  # 4.5, 3.3
PS_TOP = PS_CY + 0.4                                 # 2.4

# ── modules ───────────────────────────────────────────────────────────────────
chip(A_MCX,  A_CY,   MCU_W, MCU_H, 'Microcontroller', fc='#E8EAF6', ec='#283593')
chip(UWB_CX, A_CY,   PER_W, PER_H, 'UWB Module',      fc='#F3E5F5', ec='#6A1B9A')
chip(CAN_CX, A_CY,   PER_W, PER_H, 'CAN Bus\nModule', fc='#FFF8E1', ec='#F57F17')
chip(A_MCX,  BLE_CY, BLE_W, BLE_H, 'BLE Module',      fc='#E3F2FD', ec='#1565C0')
chip(A_MCX,  LTE_CY, BLE_W, BLE_H, 'LTE Module',      fc='#E8F5E9', ec='#1B5E20')
chip(A_MCX,  PS_CY,  9.5,   0.8,   'Power Supply',    fc='#FFEBEE', ec='#B71C1C')

# ── power arrows (dashed red) — Power Supply → each module directly ───────────
# → LTE  (direct vertical, same x=15.5)
arrow([(A_MCX, PS_TOP), (A_MCX, LTE_B)], color='#B71C1C', dashed=True)
# → MCU  (elbow left of LTE: LTE_L=12.7, MCU_L=12.6, elbow at 12.2 avoids both)
arrow([(12.2, PS_TOP), (12.2, MCU_B), (MCU_L, MCU_B)],
      color='#B71C1C', dashed=True)
# → UWB  (straight up on UWB x)
arrow([(UWB_CX, PS_TOP), (UWB_CX, MCU_B)], color='#B71C1C', dashed=True)
# → CAN  (straight up on CAN x=19.7)
arrow([(CAN_CX, PS_TOP), (CAN_CX, MCU_B)], color='#B71C1C', dashed=True)

# ── communication arrows (solid dark, bidirectional) ─────────────────────────
arrow([(UWB_R, A_CY),  (MCU_L, A_CY)],  color='#333', bidir=True)  # UWB ↔ MCU
arrow([(MCU_R, A_CY),  (CAN_L, A_CY)],  color='#333', bidir=True)  # MCU ↔ CAN
arrow([(A_MCX, MCU_B), (A_MCX, LTE_T)], color='#333', bidir=True)  # MCU ↔ LTE
arrow([(A_MCX, MCU_T), (A_MCX, BLE_B)], color='#333', bidir=True)  # MCU ↔ BLE

# ══════════════════════════════════════════════════════════════════════════════
# EXTERNAL connections
# ══════════════════════════════════════════════════════════════════════════════

# Android App (below Tag module, connects to USB Interface chip)
chip(3.6, 0.0, 4.8, 0.9, 'Android App', fc='#BBDEFB', ec='#1565C0')
arrow([(3.6, 0.45), (3.6, 2.45)],
      color='#1565C0', label='USB CDC', lbl_side='right', bidir=True)

# CAN Bus (Vehicle) — to the right of CAN Bus Module
chip(21.7, A_CY, 2.2, 0.9, 'CAN Bus\n(Vehicle)', fc='#FCE4EC', ec='#C62828')
arrow([(CAN_R, A_CY), (20.6, A_CY)],
      color='#C62828', label='CAN H/L', lbl_side='above')

# ══════════════════════════════════════════════════════════════════════════════
# Save
# ══════════════════════════════════════════════════════════════════════════════
out = r'D:\SCA\Diagram\module_overview.png'
plt.savefig(out, dpi=180, bbox_inches='tight',
            facecolor='white', edgecolor='none')
plt.close()
print(f'Saved: {out}')
os.startfile(out)
