"""
SCA Thesis — Server Internal Architecture Overview
High-level block diagram: what's inside the Server component.
"""
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch
import warnings, os
warnings.filterwarnings('ignore')

plt.rcParams.update({"font.family": "DejaVu Sans"})

fig, ax = plt.subplots(figsize=(14, 10))
fig.patch.set_facecolor('white')
ax.set_facecolor('white')
ax.set_xlim(0, 14)
ax.set_ylim(0, 10)
ax.axis('off')

FS_TITLE  = 16
FS_MODULE = 13
FS_BLOCK  = 11.5
FS_LABEL  = 9.5
LW_MODULE = 2.8
LW_BLOCK  = 2.0
LW_CONN   = 2.0

# ── Helpers ────────────────────────────────────────────────────────────────────
def module_box(x1, y1, x2, y2, label, fc, ec):
    ax.add_patch(FancyBboxPatch((x1, y1), x2-x1, y2-y1,
                 boxstyle='round,pad=0.25',
                 facecolor=fc, edgecolor=ec,
                 linewidth=LW_MODULE, zorder=2))
    ax.text((x1+x2)/2, y2-0.42, label,
            ha='center', va='center',
            fontsize=FS_MODULE, fontweight='bold', color=ec, zorder=6)

def chip(cx, cy, w, h, title, sub='', fc='white', ec='#333'):
    ax.add_patch(FancyBboxPatch((cx-w/2, cy-h/2), w, h,
                 boxstyle='round,pad=0.12',
                 facecolor=fc, edgecolor=ec,
                 linewidth=LW_BLOCK, zorder=4))
    if sub:
        ax.text(cx, cy+0.22, title, ha='center', va='center',
                fontsize=FS_BLOCK, fontweight='bold', color=ec, zorder=5)
        ax.text(cx, cy-0.22, sub,   ha='center', va='center',
                fontsize=FS_LABEL,  color='#555', zorder=5)
    else:
        ax.text(cx, cy, title, ha='center', va='center',
                fontsize=FS_BLOCK, fontweight='bold', color=ec, zorder=5)

def arrow(pts, color, lw=LW_CONN, bidir=False, dashed=False,
          label='', lbl_side='above'):
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
                                   lw=lw, mutation_scale=18), zorder=7)
    tip(pts[-2], pts[-1])
    if bidir:
        tip(pts[1], pts[0])

    if label:
        p0, p1 = pts[0], pts[1]
        mx, my = (p0[0]+p1[0])/2, (p0[1]+p1[1])/2
        off = {'above': (0, .28), 'below': (0, -.28),
               'right': (.32, 0), 'left':  (-.32, 0)}
        dx, dy = off.get(lbl_side, (0, .28))
        ax.text(mx+dx, my+dy, label, ha='center', va='center',
                fontsize=FS_LABEL+0.5, fontweight='bold', color=color, zorder=8,
                bbox=dict(fc='white', ec='none', pad=1.5))

# ══════════════════════════════════════════════════════════════════════════════
# TITLE
# ══════════════════════════════════════════════════════════════════════════════
ax.text(7, 9.55, 'Smart Car Access — Server Architecture Overview',
        ha='center', va='center', fontsize=FS_TITLE,
        fontweight='bold', color='#1A237E', zorder=9)

# ══════════════════════════════════════════════════════════════════════════════
# SERVER BOX
# ══════════════════════════════════════════════════════════════════════════════
module_box(0.4, 0.4, 13.6, 9.0, 'Server  (FastAPI)',
           fc='#E3F2FD', ec='#1565C0')

# ── Layer 1: REST API ─────────────────────────────────────────────────────────
chip(7.0, 7.7, 10.5, 1.1,
     'REST API',
     fc='#BBDEFB', ec='#1565C0')

# ── Layer 2: Auth + Crypto (side by side) ────────────────────────────────────
chip(3.8, 5.6, 5.0, 1.4,
     'Authentication',
     fc='#FFF9C4', ec='#F57F17')

chip(10.2, 5.6, 5.0, 1.4,
     'Cryptography',
     fc='#F3E5F5', ec='#6A1B9A')

# ── Layer 3: Database ─────────────────────────────────────────────────────────
chip(7.0, 3.2, 10.5, 1.1,
     'Database',
     fc='#E8F5E9', ec='#2E7D32')

# ── Arrows REST API → Auth / Crypto ─────────────────────────────────────────
arrow([(3.8, 7.15), (3.8, 6.30)], color='#555', bidir=True)   # REST ↔ Auth
arrow([(10.2, 7.15), (10.2, 6.30)], color='#555', bidir=True)  # REST ↔ Crypto

# ── Arrows Auth / Crypto → Database ─────────────────────────────────────────
arrow([(3.8, 4.90), (3.8, 3.75)], color='#555', bidir=True)   # Auth ↔ DB
arrow([(10.2, 4.90), (10.2, 3.75)], color='#555', bidir=True)  # Crypto ↔ DB

# ══════════════════════════════════════════════════════════════════════════════
# Save
# ══════════════════════════════════════════════════════════════════════════════
out = r'D:\SCA\Diagram\server_overview.png'
plt.savefig(out, dpi=180, bbox_inches='tight',
            facecolor='white', edgecolor='none')
plt.close()
print(f'Saved: {out}')
os.startfile(out)
