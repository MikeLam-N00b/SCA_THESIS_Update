"""
SCA Thesis — Server Detailed Architecture
Detailed view for system design section of thesis.
"""
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch
import warnings, os
warnings.filterwarnings('ignore')

plt.rcParams.update({"font.family": "DejaVu Sans"})

fig, ax = plt.subplots(figsize=(20, 15))
fig.patch.set_facecolor('white')
ax.set_facecolor('white')
ax.set_xlim(0, 20)
ax.set_ylim(0, 15)
ax.axis('off')

FS_TITLE  = 22
FS_SERVER = 20
FS_GROUP  = 19
FS_CHIP   = 18
FS_SUB    = 14
FS_ITEM   = 15
LW_SERVER = 2.8
LW_GROUP  = 2.0
LW_CHIP   = 1.6
LW_CONN   = 2.0

# ── Helpers ────────────────────────────────────────────────────────────────────
def server_box(x1, y1, x2, y2, label, fc, ec):
    ax.add_patch(FancyBboxPatch((x1, y1), x2-x1, y2-y1,
                 boxstyle='round,pad=0.25',
                 facecolor=fc, edgecolor=ec,
                 linewidth=LW_SERVER, zorder=2))
    ax.text((x1+x2)/2, y2-0.22, label,
            ha='center', va='center',
            fontsize=FS_SERVER, fontweight='bold', color=ec, zorder=6)

def group_box(x1, y1, x2, y2, label, fc, ec):
    ax.add_patch(FancyBboxPatch((x1, y1), x2-x1, y2-y1,
                 boxstyle='round,pad=0.18',
                 facecolor=fc, edgecolor=ec,
                 linewidth=LW_GROUP, zorder=3))
    ax.text((x1+x2)/2, y2-0.42, label,
            ha='center', va='center',
            fontsize=FS_GROUP, fontweight='bold', color=ec, zorder=6)

def chip(cx, cy, w, h, title, sub='', fc='white', ec='#333'):
    ax.add_patch(FancyBboxPatch((cx-w/2, cy-h/2), w, h,
                 boxstyle='round,pad=0.12',
                 facecolor=fc, edgecolor=ec,
                 linewidth=LW_CHIP, zorder=5))
    if sub:
        ax.text(cx, cy+0.25, title, ha='center', va='center',
                fontsize=FS_CHIP, fontweight='bold', color=ec, zorder=6)
        ax.text(cx, cy-0.30, sub, ha='center', va='center',
                fontsize=FS_SUB, color='#555', zorder=6)
    else:
        ax.text(cx, cy, title, ha='center', va='center',
                fontsize=FS_CHIP, fontweight='bold', color=ec, zorder=6)

def detail_box(cx, cy, w, h, title, items, fc, ec):
    ax.add_patch(FancyBboxPatch((cx-w/2, cy-h/2), w, h,
                 boxstyle='round,pad=0.15',
                 facecolor=fc, edgecolor=ec,
                 linewidth=LW_GROUP, zorder=3))
    top = cy + h/2
    # Title
    ax.text(cx, top - 0.50, title,
            ha='center', va='center',
            fontsize=FS_GROUP, fontweight='bold', color=ec, zorder=6)
    # Divider — well below title
    div_y = top - 1.00
    ax.plot([cx-w/2+0.25, cx+w/2-0.25], [div_y, div_y],
            color=ec, lw=1.2, alpha=0.45, zorder=6)
    # Items — start clearly below divider
    item_start = div_y - 0.45
    step = (h - 1.55) / max(len(items) - 1, 1)
    for i, item in enumerate(items):
        iy = item_start - i * step
        ax.text(cx - w/2 + 0.45, iy, f'•  {item}',
                ha='left', va='center',
                fontsize=FS_ITEM, color='#333', zorder=6)

def arrow(pts, color='#555', lw=LW_CONN, bidir=False):
    xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
    ax.plot(xs, ys, color=color, lw=lw, zorder=4, solid_capstyle='round')
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

# ══════════════════════════════════════════════════════════════════════════════
# TITLE
ax.text(10, 14.6, 'Smart Car Access — Server Architecture',
        ha='center', va='center', fontsize=FS_TITLE,
        fontweight='bold', color='#1A237E', zorder=9)

# ══════════════════════════════════════════════════════════════════════════════
# SERVER BOX
server_box(0.3, 0.3, 19.7, 14.0, 'Server  (FastAPI)',
           fc='#E3F2FD', ec='#1565C0')

# ── REST API Layer ─────────────────────────────────────────────────────────────
group_box(0.65, 10.2, 19.35, 13.0, 'REST API  Layer',
          fc='#BBDEFB', ec='#1565C0')

chip(4.2,  11.3, 5.2, 1.3,
     'Owner API', 'Pairing  •  Vehicle  •  Friend Sharing',
     fc='#E3F2FD', ec='#1565C0')
chip(10.0, 11.3, 5.2, 1.3,
     'Anchor API', 'Validate  •  Revocation  •  Activity',
     fc='#E3F2FD', ec='#1565C0')
chip(15.8, 11.3, 5.2, 1.3,
     'Guest API', 'Claim Token',
     fc='#E3F2FD', ec='#1565C0')

# ── Authentication + Cryptography ─────────────────────────────────────────────
detail_box(5.2, 7.7, 7.2, 3.2,
           'Authentication',
           ['ECDH Key Exchange', 'HMAC-SHA256  (Anchor auth)', 'X-Owner-Key  (Owner auth)'],
           fc='#FFF9C4', ec='#F57F17')

detail_box(14.8, 7.7, 7.2, 3.2,
           'Cryptography',
           ['ECDSA P-256  (Bundle signing)', 'AES-GCM  (Key encryption)', 'HKDF  (Key derivation)'],
           fc='#F3E5F5', ec='#6A1B9A')

# ── Database ───────────────────────────────────────────────────────────────────
group_box(0.65, 1.0, 19.35, 5.2, 'Database  (SQLite)',
          fc='#E8F5E9', ec='#2E7D32')

chip(4.2,  3.1, 5.0, 1.3,
     'vehicles', 'vehicle_id  •  pairing_key  •  owner_key',
     fc='#C8E6C9', ec='#2E7D32')
chip(10.0, 3.1, 5.0, 1.3,
     'friend_keys', 'friend_id  •  key  •  permissions  •  expiry',
     fc='#C8E6C9', ec='#2E7D32')
chip(15.8, 3.1, 5.0, 1.3,
     'access_events', 'vehicle_id  •  event_type  •  timestamp',
     fc='#C8E6C9', ec='#2E7D32')

# ── Arrows ────────────────────────────────────────────────────────────────────
# Auth top = 7.7+1.6=9.3, REST API bottom = 10.2
arrow([(5.2,  10.2), (5.2,  9.3)],  bidir=True)
arrow([(14.8, 10.2), (14.8, 9.3)],  bidir=True)
# Auth bottom = 7.7-1.6=6.1, DB top = 5.2
arrow([(5.2,  6.1),  (5.2,  5.2)],  bidir=True)
arrow([(14.8, 6.1),  (14.8, 5.2)],  bidir=True)

# ══════════════════════════════════════════════════════════════════════════════
out = r'D:\SCA\Diagram\server_detail.png'
plt.savefig(out, dpi=180, bbox_inches='tight',
            facecolor='white', edgecolor='none')
plt.close()
print(f'Saved: {out}')
os.startfile(out)
