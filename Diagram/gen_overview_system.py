"""
SCA Thesis — System Overview Block Diagram
All arrows are strictly horizontal, vertical, or right-angle elbows (no diagonals).

Layout strategy:
  App, Tag, Anchor  → same x=2.85  (USB vertical, BLE+UWB vertical)
  App, Server       → same y=7.85  (HTTPS horizontal)
  Anchor, BCM, Actuator → same y=2.55 (CAN horizontal)
  LTE path: 3-segment elbow through the gap between groups
Output: D:/SCA/Diagram/Overview.png
"""

import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from matplotlib.patches import FancyBboxPatch
import warnings, os
warnings.filterwarnings('ignore')

plt.rcParams.update({"font.family": "DejaVu Sans"})

fig, ax = plt.subplots(figsize=(22, 14))
fig.patch.set_facecolor('white')
ax.set_facecolor('white')
ax.set_xlim(0, 16)
ax.set_ylim(0, 11)
ax.axis('off')

# ── Font sizes ────────────────────────────────────────────────────────────────
FS_NODE = 13.0
FS_LBL  = 11.0
FS_GRP  = 12.0

# ── Node geometry ─────────────────────────────────────────────────────────────
NW, NH = 3.70, 1.05    # node width / height  (half: 1.85 / 0.525)

# ── Node centre positions ─────────────────────────────────────────────────────
#  App, Tag, Anchor share x=2.85  → USB vertical, BLE+UWB vertical
#  App, Server       share y=7.85 → HTTPS horizontal
#  Anchor,BCM,Actuator share y=2.55 → CAN horizontal
CX_LEFT   = 2.85
CX_SERVER = 13.10
Y_TOP     = 7.85
Y_BOT     = 2.55

POS = {
    'app':      (CX_LEFT,   Y_TOP),
    'tag':      (CX_LEFT,   6.05),
    'server':   (CX_SERVER, Y_TOP),
    'anchor':   (CX_LEFT,   Y_BOT),
    'bcm':      (8.00,      Y_BOT),
    'actuator': (CX_SERVER, Y_BOT),
}

def left(k):  return POS[k][0] - NW/2
def right(k): return POS[k][0] + NW/2
def top(k):   return POS[k][1] + NH/2
def bot(k):   return POS[k][1] - NH/2
def cx(k):    return POS[k][0]
def cy(k):    return POS[k][1]

# ── Drawing helpers ───────────────────────────────────────────────────────────

def group_box(x1, y1, x2, y2, label, fc, ec):
    ax.add_patch(FancyBboxPatch((x1, y1), x2-x1, y2-y1,
                 boxstyle='round,pad=0.18',
                 facecolor=fc, edgecolor=ec, linewidth=2.4,
                 alpha=0.45, zorder=1))
    ax.text(x1+0.22, y2-0.14, label, ha='left', va='top',
            fontsize=FS_GRP, fontweight='bold', color=ec, zorder=6)

def node(k, lines, fc, ec):
    cx_, cy_ = POS[k]
    ax.add_patch(FancyBboxPatch((cx_-NW/2, cy_-NH/2), NW, NH,
                 boxstyle='round,pad=0.12',
                 facecolor=fc, edgecolor=ec, linewidth=2.4, zorder=4))
    n, step = len(lines), 0.23
    for i, line in enumerate(lines):
        offset = ((n-1)/2 - i) * step
        ax.text(cx_, cy_+offset, line, ha='center', va='center',
                fontsize=FS_NODE, fontweight='bold', color=ec, zorder=5)

def _arrowhead(toward, at, color, ms=22):
    """Draw an arrowhead at `at` pointing from direction of `toward`."""
    dx, dy = at[0]-toward[0], at[1]-toward[1]
    L = max((dx**2+dy**2)**0.5, 1e-9)
    near = (at[0]-dx/L*0.001, at[1]-dy/L*0.001)
    ax.annotate('', xy=at, xytext=near,
                arrowprops=dict(arrowstyle='->', color=color, lw=2.8,
                               mutation_scale=ms), zorder=6)

def conn(pts, color='#333', dashed=False, bidir=False,
         label='', lbl_idx=None, lbl_side='above'):
    """
    Draw an orthogonal path through waypoints + arrowheads.
    label      : text label to show on the segment at lbl_idx (0-based segment index)
    lbl_side   : 'above' or 'right' of segment midpoint
    """
    ls = (0, (5, 3)) if dashed else '-'
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    ax.plot(xs, ys, color=color, lw=2.8, ls=ls, zorder=3,
            solid_capstyle='round', dash_capstyle='round')

    _arrowhead(pts[-2], pts[-1], color)
    if bidir:
        _arrowhead(pts[1], pts[0], color)

    if label:
        # Pick segment for label
        idx = lbl_idx if lbl_idx is not None else 0
        p0, p1 = pts[idx], pts[idx+1]
        mx, my = (p0[0]+p1[0])/2, (p0[1]+p1[1])/2
        if lbl_side == 'above':
            lx, ly = mx, my + 0.22
        elif lbl_side == 'below':
            lx, ly = mx, my - 0.22
        elif lbl_side == 'right':
            lx, ly = mx + 0.22, my
        else:  # left
            lx, ly = mx - 0.22, my
        ax.text(lx, ly, label, ha='center', va='center',
                fontsize=FS_LBL, fontweight='bold', color=color, zorder=7,
                bbox=dict(fc='white', ec='none', pad=1.5))

# ── Group boxes ───────────────────────────────────────────────────────────────
# Gap between Mobile bottom (4.70) and Vehicle top (4.35): y ∈ [4.35, 4.70]
# LTE horizontal segment runs at y=4.525 (middle of gap)

group_box(0.25, 4.70, 5.45, 9.50,
          'Mobile System', '#FFF8E1', '#E65100')

group_box(10.50, 5.45, 15.75, 9.50,
          'Backend System', '#E3F2FD', '#1565C0')

group_box(0.25, 0.25, 15.75, 4.35,
          'Vehicle System', '#F3E5F5', '#6A1B9A')

# ── Nodes ─────────────────────────────────────────────────────────────────────
node('app',      ['Android App', '(Owner + Guest)'], '#BBDEFB', '#1565C0')
node('tag',      ['Tag', '(ESP32-S3)'],              '#FFE0B2', '#E65100')
node('server',   ['Server', '(FastAPI)'],            '#C8E6C9', '#2E7D32')
node('anchor',   ['Anchor', '(ESP32)'],              '#E8EAF6', '#283593')
node('bcm',      ['SJB', '(Smart Junction Box)'],    '#FFF9C4', '#F57F17')
node('actuator', ['Actuator', '(Door Lock/Unlock)'], '#FCE4EC', '#C62828')

# ── Connections ───────────────────────────────────────────────────────────────

Y_GAP = 4.525   # midpoint of gap between Mobile and Vehicle groups

# 1. USB Serial CDC/ACM — vertical, inside Mobile group
#    App bottom → Tag top  (same x)
conn([(cx('app'), bot('app')),
      (cx('app'), top('tag'))],
     color='#E65100', bidir=True,
     label='USB Serial\nCDC/ACM 115200',
     lbl_idx=0, lbl_side='right')

# 2. BLE + UWB — vertical, Tag bottom → Anchor top
#    Use x = CX_LEFT - 0.35 so it sits left of the LTE path (avoids overlap)
BLE_X = CX_LEFT - 0.35
conn([(BLE_X, bot('tag')),
      (BLE_X, top('anchor'))],
     color='#7B1FA2', bidir=True,
     label='BLE + UWB',
     lbl_idx=0, lbl_side='left')

# 3. HTTPS — horizontal, App right → Server left  (same y)
conn([(right('app'),    cy('app')),
      (left('server'),  cy('server'))],
     color='#1565C0', dashed=True, bidir=True,
     label='HTTPS', lbl_idx=0, lbl_side='above')

# 4. LTE HTTP+HMAC — 3-segment elbow (no diagonals):
#    Anchor top-right → up to gap → right to Server x → up to Server bottom
#    Exit from Anchor top slightly right of centre (CX_LEFT + 0.35)
LTE_X = CX_LEFT + 0.35
conn([(LTE_X,          top('anchor')),   # exit Anchor top
      (LTE_X,          Y_GAP),           # up to gap midpoint
      (cx('server'),   Y_GAP),           # right across gap
      (cx('server'),   bot('server'))],  # up into Server
     color='#283593', dashed=True, bidir=True,
     label='LTE  HTTP+HMAC',
     lbl_idx=1,         # label on the horizontal segment
     lbl_side='above')

# 5. CAN Bus — Anchor right → BCM left  (same y)
conn([(right('anchor'), cy('anchor')),
      (left('bcm'),     cy('bcm'))],
     color='#2E7D32',
     label='CAN Bus', lbl_idx=0, lbl_side='above')

# 6. CAN — BCM right → Actuator left  (same y)
conn([(right('bcm'),      cy('bcm')),
      (left('actuator'),  cy('actuator'))],
     color='#2E7D32',
     label='CAN', lbl_idx=0, lbl_side='above')

# ── Title ─────────────────────────────────────────────────────────────────────
ax.text(8.0, 10.55, 'Smart Car Access — System Architecture Overview',
        ha='center', va='center', fontsize=18, fontweight='bold', color='#1A237E')

# ── Legend ────────────────────────────────────────────────────────────────────
legend_items = [
    mpatches.Patch(fc='#BBDEFB', ec='#1565C0', label='Android App'),
    mpatches.Patch(fc='#FFE0B2', ec='#E65100', label='Tag (ESP32-S3, key fob)'),
    mpatches.Patch(fc='#C8E6C9', ec='#2E7D32', label='Server (FastAPI)'),
    mpatches.Patch(fc='#E8EAF6', ec='#283593', label='Anchor (ESP32, in-vehicle)'),
    mpatches.Patch(fc='#FFF9C4', ec='#F57F17', label='SJB (Smart Junction Box)'),
    mpatches.Patch(fc='#FCE4EC', ec='#C62828', label='Actuator (Door Lock/Unlock)'),
]
ax.legend(handles=legend_items, loc='lower right', fontsize=8.5,
          framealpha=0.92, edgecolor='#BDBDBD',
          bbox_to_anchor=(0.995, 0.01))

# ── Save ──────────────────────────────────────────────────────────────────────
out = r'D:\SCA\Diagram\Overview.png'
plt.savefig(out, dpi=180, bbox_inches='tight',
            facecolor='white', edgecolor='none')
plt.close()
print(f"Saved to {out}")
os.startfile(out)
