import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from matplotlib.patches import FancyBboxPatch
import warnings, os
warnings.filterwarnings('ignore')

fig = plt.figure(figsize=(13.333, 7.5))
ax = fig.add_axes([0, 0, 1, 1])
ax.set_xlim(0, 14)
ax.set_ylim(1.5, 9.85)       # range 8.35 → ~134.7 px/unit at 150 dpi
ax.axis('off')
fig.patch.set_facecolor('white')

X = {'owner': 2.0, 'guest': 5.5, 'server': 9.5, 'vehicle': 12.5}
COLORS = {
    'owner':   ('#D6EAF8', '#2E86C1', '#1A5276'),
    'guest':   ('#D7BDE2', '#8E44AD', '#6C3483'),
    'server':  ('#A9DFBF', '#27AE60', '#145A32'),
    'vehicle': ('#FAD7A0', '#CA6F1E', '#784212'),
}
LABELS = {'owner': 'Owner', 'guest': 'Guest', 'server': 'Server', 'vehicle': 'Vehicle'}

FS = 16   # main font; at 150 dpi ≈ 33 px = 0.25 units

# ─── Helpers ──────────────────────────────────────────────────────────

def actor_box(x, y, key):
    bg, border, fg = COLORS[key]
    ax.add_patch(FancyBboxPatch((x - 0.95, y - 0.155), 1.9, 0.31,
                 boxstyle='round,pad=0.06',
                 facecolor=bg, edgecolor=border, linewidth=2, zorder=5))
    ax.text(x, y, LABELS[key], ha='center', va='center',
            fontsize=FS, fontweight='bold', color=fg, zorder=6)

def lifelines(y_top, y_bot):
    for x in X.values():
        ax.plot([x, x], [y_top, y_bot], '--', color='#AAAAAA', lw=0.9, zorder=1)

def arr(x1, x2, y, label, dashed=False, color='black'):
    ls = (0, (5, 3)) if dashed else 'solid'
    ax.annotate('', xy=(x2, y), xytext=(x1, y),
                arrowprops=dict(arrowstyle='->', color=color, lw=1.2,
                               linestyle=ls, connectionstyle='arc3,rad=0'))
    # va='bottom' → text bottom is at y+0.07, text extends ~0.25 units upward
    ax.text((x1+x2)/2, y + 0.07, label, ha='center', va='bottom',
            fontsize=FS, color='#222222' if color == 'black' else color)

def self_loop(x, y, label, left=False):
    rad = 0.75 if left else -0.75
    ax.annotate('', xy=(x, y - 0.18), xytext=(x, y),
                arrowprops=dict(arrowstyle='->', color='black', lw=1.1,
                               connectionstyle=f'arc3,rad={rad}'))
    # va='center' → text spans [y-0.09-0.125, y-0.09+0.125] at FS=16
    if left:
        ax.text(x - 0.14, y - 0.09, label, ha='right', va='center',
                fontsize=FS, zorder=8)
    else:
        ax.text(x + 0.14, y - 0.09, label, ha='left', va='center',
                fontsize=FS, zorder=8)

def ref_box(x1, x2, y, h, line1, line2=''):
    ax.add_patch(FancyBboxPatch((x1, y - h), x2 - x1, h,
                 boxstyle='round,pad=0.1',
                 facecolor='#F4F4F4', edgecolor='#AAAAAA', linewidth=1.1, zorder=2))
    mid = (x1 + x2) / 2
    if line2:
        ax.text(mid, y - h*0.35, line1, ha='center', va='center', fontsize=FS-2, color='#333')
        ax.text(mid, y - h*0.72, line2, ha='center', va='center', fontsize=FS-4, color='#999')
    else:
        ax.text(mid, y - h/2,  line1, ha='center', va='center', fontsize=FS-2, color='#333')

def uml_note(x, y, lines, w=2.85):
    lh, pad, fold = 0.19, 0.08, 0.10
    h = len(lines) * lh + 2 * pad
    pts_body = [(x, y), (x+w-fold, y), (x+w, y-fold), (x+w, y-h), (x, y-h)]
    pts_ear  = [(x+w-fold, y), (x+w-fold, y-fold), (x+w, y-fold)]
    ax.add_patch(mpatches.Polygon(pts_body, closed=True,
                 facecolor='#FFFDE7', edgecolor='#F9A825', lw=1, zorder=4))
    ax.add_patch(mpatches.Polygon(pts_ear,  closed=True,
                 facecolor='#FFF8E1', edgecolor='#F9A825', lw=1, zorder=4))
    for i, line in enumerate(lines):
        ax.text(x + pad, y - pad - i*lh, line, ha='left', va='top',
                fontsize=FS-4, family='monospace', color='#212121', zorder=5)

def par_box(x1, x2, y_top, y_bot):
    ax.add_patch(FancyBboxPatch((x1, y_bot), x2-x1, y_top-y_bot,
                 boxstyle='round,pad=0.1',
                 facecolor='#EDE7F6', edgecolor='#9575CD', linewidth=1.3,
                 alpha=0.45, zorder=2))
    ax.text(x1+0.14, y_top-0.06, 'par', ha='left', va='top',
            fontsize=FS+2, fontweight='bold', color='#6A1B9A')

# ─── Title & Actors ───────────────────────────────────────────────────
ax.text(7.5, 9.62, 'Friend Sharing — Security Flow',
        ha='center', va='center', fontsize=22, fontweight='bold')

Y_TOP = 9.30                         # actor box: [9.145, 9.455]
for k, x in X.items():
    actor_box(x, Y_TOP, k)

lifelines(Y_TOP - 0.155, 2.005)      # top=9.145, bottom=Y_BOT+0.155

# ═══════════════════════════════════════════════════════════════════════
#  CREATE SHARE
# ═══════════════════════════════════════════════════════════════════════
# First arrow: text-top = y+0.07+0.25 = y+0.32 must be < 9.145  → y < 8.825
# Use y = 8.78  (text-top ≈ 9.10, margin 6 px above actor box)
arr(X['owner'], X['server'], 8.78, 'Create share  (VIN, permissions, TTL)')

# self_loop Generate (arrow→SL: step 0.35, SL text top ≈ y+0.04 < prev arrow text-bottom 8.85 ✓)
self_loop(X['server'], 8.43, 'Generate session key,  sign bundle', left=True)
uml_note(X['server'] + 0.35, 8.55, [
    'bundle = {',
    '  friend_id,  vehicle_id,',
    '  friend_key,  expires_at,',
    '  permissions',
    '}',
    'Signature',
])

# SL→arr: step 0.65; SL text-bottom ≈ 8.43-0.215=8.215; arr text-top = 7.78+0.32=8.10 < 8.215 ✓
arr(X['server'], X['owner'], 7.78, 'Claim token + URL', dashed=True)

# arr→arr: step 0.40; text gap ≈ (0.40-0.25)×134.7 = 20 px ✓
arr(X['owner'], X['guest'],  7.38, 'Share URL')

# ═══════════════════════════════════════════════════════════════════════
#  CLAIM BUNDLE  (section gap 0.50 from 7.38)
# ═══════════════════════════════════════════════════════════════════════
arr(X['guest'], X['server'], 6.88, 'GET /claim/{token}   (single-use)')
arr(X['server'], X['guest'], 6.48, 'Return signed bundle', dashed=True)
uml_note(X['server'] + 0.35, 6.60, [
    'bundle = {',
    '  friend_id,  vehicle_id,',
    '  friend_key,  expires_at,',
    '  permissions',
    '}',
    'Signature',
])

# arr→SL: step 0.35 fine; SL Verify text top ≈ 6.13+0.04 = 6.17 < 6.48+0.07 = 6.55 ✓
self_loop(X['guest'], 6.13, 'Verify signature offline')

# SL→SL: step 0.42; SL1 text-bottom ≈ 6.13-0.215=5.915; SL2 text-top ≈ 5.71+0.04=5.75 < 5.915 ✓
self_loop(X['guest'], 5.71, 'Store bundle (friend_key,...) in secure storage')

# ═══════════════════════════════════════════════════════════════════════
#  ACCESS VEHICLE  (section gap 0.65 from 5.71)
# ═══════════════════════════════════════════════════════════════════════
ref_box(X['guest'] - 0.2, X['vehicle'] + 0.15, 5.06, 0.52,
        'BLE connection,  key retrieval,  verification & session',
        'See separate diagram  →')

# ═══════════════════════════════════════════════════════════════════════
#  ACCESS / UWB  (par box; ref_box bottom = 4.54, gap → par_top = 4.28)
# ═══════════════════════════════════════════════════════════════════════
# First UWB arr at 4.28-0.32=3.96; par_bot at 3.96-3×0.40-0.32=2.32
par_box(X['guest'] - 0.3, X['vehicle'] + 0.3, 4.28, 2.32)

arr(X['guest'],   X['vehicle'], 3.84, 'Request unlock')
arr(X['vehicle'], X['guest'],   3.44, 'Unlock confirmation', dashed=True)
arr(X['guest'],   X['vehicle'], 3.04, 'Start UWB ranging')
arr(X['vehicle'], X['guest'],   2.64,
    'Distance data   (STS encrypted with session key)', dashed=True)

# self_loop Log is BELOW par_bot (2.32); SL at 2.10 < 2.32 ✓
self_loop(X['vehicle'], 2.10, 'Log session result  →  Server', left=True)

# ─── Bottom Actors ────────────────────────────────────────────────────
Y_BOT = 1.85                         # actor box top = 2.005 = lifeline bottom ✓
for k, x in X.items():
    actor_box(x, Y_BOT, k)

out = 'D:/Download/friend_sharing_flow.png'
plt.savefig(out, dpi=150, bbox_inches='tight', facecolor='white', edgecolor='none')
plt.close()
print(f"Saved to {out}")
os.startfile(out)
