import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from matplotlib.patches import FancyBboxPatch
import warnings, os
warnings.filterwarnings('ignore')

fig = plt.figure(figsize=(13.333, 9.5))
ax = fig.add_axes([0, 0, 1, 1])
ax.set_xlim(0, 14)
ax.set_ylim(1.2, 12.0)       # range 10.8 → extra bottom room for Log self_loop
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

FS = 16   # main font

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
    ax.text((x1+x2)/2, y + 0.07, label, ha='center', va='bottom',
            fontsize=FS, color='#222222' if color == 'black' else color)

def self_loop(x, y, label, left=False):
    """Supports multiline label via '\\n'."""
    rad = 0.75 if left else -0.75
    ax.annotate('', xy=(x, y - 0.18), xytext=(x, y),
                arrowprops=dict(arrowstyle='->', color='black', lw=1.1,
                               connectionstyle=f'arc3,rad={rad}'))
    lines = label.split('\n')
    line_h = 0.21   # vertical step between lines (units)
    if left:
        for i, line in enumerate(lines):
            ax.text(x - 0.14, y - 0.09 - i * line_h, line,
                    ha='right', va='center', fontsize=FS, zorder=8)
    else:
        for i, line in enumerate(lines):
            ax.text(x + 0.14, y - 0.09 - i * line_h, line,
                    ha='left', va='center', fontsize=FS, zorder=8)

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

def section_label(x_left, y, label, w=1.15, h=0.38):
    ax.add_patch(FancyBboxPatch((x_left, y - h/2), w, h,
                 boxstyle='round,pad=0.07',
                 facecolor='#D1C4E9', edgecolor='#9575CD', linewidth=1.2, zorder=4))
    ax.text(x_left + w/2, y, label, ha='center', va='center',
            fontsize=FS-3, color='#4A148C', fontweight='bold', zorder=5)

# ─── Title & Actors ───────────────────────────────────────────────────
# All y coords scaled from original [1.5, 9.85] → [1.5, 12.0]  (×1.26 from base 1.5)
# f(y) = 1.5 + (y - 1.5) * 1.26

ax.text(7.5, 11.73, 'Friend Sharing — Security Flow',
        ha='center', va='center', fontsize=22, fontweight='bold')

Y_TOP = 11.33                        # was 9.30
Y_BOT = 1.50                         # lowered to clear Log self_loop text

for k, x in X.items():
    actor_box(x, Y_TOP, k)

lifelines(Y_TOP - 0.155, Y_BOT + 0.155)

# ═══════════════════════════════════════════════════════════════════════
#  CREATE SHARE
# ═══════════════════════════════════════════════════════════════════════
arr(X['owner'], X['server'], 10.67, 'Create share  (VIN, permissions, TTL)')

self_loop(X['server'], 10.23, 'Generate session key,  sign bundle', left=True)
uml_note(X['server'] + 0.35, 10.38, [
    'bundle = {',
    '  friend_id,  vehicle_id,',
    '  friend_key,  expires_at,',
    '  permissions',
    '}',
    'Signature',
])

arr(X['server'], X['owner'], 9.41, 'Claim token + URL', dashed=True)
arr(X['owner'], X['guest'],  8.91, 'Share URL')

# ═══════════════════════════════════════════════════════════════════════
#  CLAIM BUNDLE
# ═══════════════════════════════════════════════════════════════════════
arr(X['guest'], X['server'], 8.28, 'GET /claim/{token}   (single-use)')
arr(X['server'], X['guest'], 7.77, 'Return signed bundle', dashed=True)
uml_note(X['server'] + 0.35, 7.93, [
    'bundle = {',
    '  friend_id,  vehicle_id,',
    '  friend_key,  expires_at,',
    '  permissions',
    '}',
    'Signature',
])

self_loop(X['guest'], 7.33, 'Verify signature offline')

# "secure storage" on a separate line to avoid overflow
self_loop(X['guest'], 6.80, 'Store bundle (friend_key,...) in\nsecure storage')

# ═══════════════════════════════════════════════════════════════════════
#  ACCESS VEHICLE
# ═══════════════════════════════════════════════════════════════════════
ref_box(X['guest'] - 0.2, X['vehicle'] + 0.15, 5.99, 0.66,
        'BLE connection,  key retrieval,  verification & session',
        'See separate diagram  →')
# ref_box bottom = 5.99 - 0.66 = 5.33

# ═══════════════════════════════════════════════════════════════════════
#  ACCESS / UWB  (par box)
# ═══════════════════════════════════════════════════════════════════════
par_box(X['guest'] - 0.3, X['vehicle'] + 0.3, 5.00, 2.20)

# "Ranging" section — UWB proximity check first
section_label(X['vehicle'] - 1.0, 4.80, 'Ranging')
arr(X['guest'],   X['vehicle'], 4.35, 'Start UWB ranging')
arr(X['vehicle'], X['guest'],   3.83, 'Distance data', dashed=True)

# "Access features" section — unlock after proximity confirmed
section_label(X['vehicle'] - 1.7, 3.43, 'Access features', w=1.9, h=0.50)
arr(X['guest'],   X['vehicle'], 3.00, 'Request unlock')
arr(X['vehicle'], X['guest'],   2.55, 'Unlock confirmation', dashed=True)

# ─── Bottom Actors ────────────────────────────────────────────────────
for k, x in X.items():
    actor_box(x, Y_BOT, k)

out = 'D:/SCA/Diagram/friend_sharing_flow.png'
plt.savefig(out, dpi=150, bbox_inches='tight', facecolor='white', edgecolor='none')
plt.close()
print(f"Saved to {out}")
os.startfile(out)

# ═══════════════════════════════════════════════════════════════════════
#  FIGURE 2: Secure Vehicle Key Exchange — Main Flow
#  Size: 13.33 × 7.5 in  |  min font: 20
# ═══════════════════════════════════════════════════════════════════════

fig2 = plt.figure(figsize=(13.333, 7.5))
ax2 = fig2.add_axes([0, 0, 1, 1])
ax2.set_xlim(0, 15)   # extra right room for section labels
ax2.set_ylim(-0.20, 12)   # bottom margin so actor box rounded corners are not clipped
ax2.axis('off')
fig2.patch.set_facecolor('white')

FS2 = 18   # reduced from 20 to gain vertical breathing room

# User pushed right to give left-loop text room; Vehicle at 11.5 leaves right margin
X2 = {'user': 3.5, 'server': 7.5, 'vehicle': 11.5}
COLORS2 = {
    'user':    ('#D6EAF8', '#2E86C1', '#1A5276'),
    'server':  ('#A9DFBF', '#27AE60', '#145A32'),
    'vehicle': ('#FAD7A0', '#CA6F1E', '#784212'),
}
LABELS2 = {'user': 'User', 'server': 'Server', 'vehicle': 'Vehicle'}

Y_TOP2 = 10.80
Y_BOT2 = 0.30   # box bottom = 0.04 > ylim_bottom=0 ✓


def actor_box2(x, y, key):
    bg, border, fg = COLORS2[key]
    ax2.add_patch(FancyBboxPatch((x - 1.05, y - 0.26), 2.1, 0.52,
                 boxstyle='round,pad=0.07',
                 facecolor=bg, edgecolor=border, linewidth=2, zorder=5))
    ax2.text(x, y, LABELS2[key], ha='center', va='center',
            fontsize=FS2, fontweight='bold', color=fg, zorder=6)


def lifelines2(y_top, y_bot):
    for x in X2.values():
        ax2.plot([x, x], [y_top, y_bot], '--', color='#AAAAAA', lw=0.9, zorder=1)


def arr2(x1, x2, y, label, dashed=False):
    ls = (0, (5, 3)) if dashed else 'solid'
    ax2.annotate('', xy=(x2, y), xytext=(x1, y),
                arrowprops=dict(arrowstyle='->', color='black', lw=1.5,
                               linestyle=ls, connectionstyle='arc3,rad=0'))
    ax2.text((x1 + x2) / 2, y + 0.10, label, ha='center', va='bottom',
            fontsize=FS2, color='#222222')


def self_loop2(x, y, label, left=False):
    rad = 0.75 if left else -0.75
    ax2.annotate('', xy=(x, y - 0.26), xytext=(x, y),
                arrowprops=dict(arrowstyle='->', color='black', lw=1.3,
                               connectionstyle=f'arc3,rad={rad}'))
    lines = label.split('\n')
    line_h = 0.40   # tăng giãn dòng cho label 2 dòng
    if left:
        for i, line in enumerate(lines):
            ax2.text(x - 0.20, y - 0.12 - i * line_h, line,
                    ha='right', va='center', fontsize=FS2, zorder=8)
    else:
        for i, line in enumerate(lines):
            ax2.text(x + 0.20, y - 0.12 - i * line_h, line,
                    ha='left', va='center', fontsize=FS2, zorder=8)


def ref_box2(x1, x2, y, h, line1, line2=''):
    ax2.add_patch(FancyBboxPatch((x1, y - h), x2 - x1, h,
                 boxstyle='round,pad=0.12',
                 facecolor='#F4F4F4', edgecolor='#AAAAAA', linewidth=1.3, zorder=2))
    mid = (x1 + x2) / 2
    if line2:
        ax2.text(mid, y - h * 0.33, line1, ha='center', va='center', fontsize=FS2 - 2, color='#333')
        ax2.text(mid, y - h * 0.70, line2, ha='center', va='center', fontsize=FS2 - 4, color='#999')
    else:
        ax2.text(mid, y - h / 2, line1, ha='center', va='center', fontsize=FS2 - 2, color='#333')


def par_box2(x1, x2, y_top, y_bot):
    ax2.add_patch(FancyBboxPatch((x1, y_bot), x2 - x1, y_top - y_bot,
                 boxstyle='round,pad=0.12',
                 facecolor='#EDE7F6', edgecolor='#9575CD', linewidth=1.5,
                 alpha=0.45, zorder=2))
    ax2.text(x1 + 0.18, y_top - 0.12, 'par', ha='left', va='top',
            fontsize=FS2, fontweight='bold', color='#6A1B9A')


def section_label2(x_left, y, label, w=2.9, h=0.46):
    ax2.add_patch(FancyBboxPatch((x_left, y - h / 2), w, h,
                 boxstyle='round,pad=0.07',
                 facecolor='#D1C4E9', edgecolor='#9575CD', linewidth=1.2, zorder=4))
    ax2.text(x_left + w / 2, y, label, ha='center', va='center',
            fontsize=FS2 - 1, color='#4A148C', fontweight='bold', zorder=5)


# ─── Title & top actors ───────────────────────────────────────────────
ax2.text(7.5, 11.72, 'Secure Vehicle Key Exchange — Main Flow',
        ha='center', va='center', fontsize=FS2 + 4, fontweight='bold')

for k, x in X2.items():
    actor_box2(x, Y_TOP2, k)

lifelines2(Y_TOP2 - 0.26, Y_BOT2 + 0.26)

# ─── Key provisioning ────────────────────────────────────────────────
# At FS2=18: text height ≈0.40 units; self_loop text bottom = y-0.30
# Constraint self_loop→arr: arr_text_top (y+0.50) < self_loop_text_bottom
arr2(X2['user'], X2['server'], 9.70, 'Enter vehicle ID (VIN)')
self_loop2(X2['server'], 9.05, 'Generate key from VIN', left=True)
# self_loop text bottom: 9.05-0.30=8.75; arr text top: 8.15+0.50=8.65 < 8.75 ✓
arr2(X2['server'], X2['user'], 8.15, 'Return encrypted Vehicle_K', dashed=True)
self_loop2(X2['user'], 7.50, 'Decrypt Vehicle_K', left=True)
self_loop2(X2['user'], 6.80, 'Store Vehicle_K\nin secure element', left=True)
# store line-1 text bottom: 6.80-0.72=6.08

# ─── BLE ref box  (gap 0.28 below store text; top=5.80, bot=5.10) ────
ref_box2(X2['user'] - 0.3, X2['vehicle'] + 0.3, 5.80, 0.70,
        'BLE connection,  key retrieval,  verification & session',
        'See separate diagram  →')

# ─── content box (Ranging → Access features, no "par" label) ──────────
ax2.add_patch(FancyBboxPatch((X2['user'] - 0.4, 1.40), 14.6 - (X2['user'] - 0.4), 4.80 - 1.40,
             boxstyle='round,pad=0.12',
             facecolor='#EDE7F6', edgecolor='#9575CD', linewidth=1.5,
             alpha=0.45, zorder=2))

# "Ranging" section — UWB ranging determines proximity before unlock
section_label2(X2['vehicle'] + 0.25, 4.50, 'Ranging')
arr2(X2['user'], X2['vehicle'], 3.95, 'Start ranging')
arr2(X2['vehicle'], X2['user'], 3.30, 'Ranging data', dashed=True)

# "Access features" section — unlock only after proximity confirmed
section_label2(X2['vehicle'] + 0.25, 2.88, 'Access features')
arr2(X2['user'], X2['vehicle'], 2.33, 'Request unlock')
arr2(X2['vehicle'], X2['user'], 1.68, 'Unlock confirmation', dashed=True)

# ─── Bottom actors ────────────────────────────────────────────────────
for k, x in X2.items():
    actor_box2(x, Y_BOT2, k)

out2 = 'D:/SCA/Diagram/key_exchange_flow.png'
plt.savefig(out2, dpi=150, bbox_inches='tight', pad_inches=0.08, facecolor='white', edgecolor='none')
plt.close()
print(f"Saved to {out2}")
os.startfile(out2)

# ═══════════════════════════════════════════════════════════════════════
#  FIGURE 3: BLE Key Exchange & Verification — Steps 5–11
#  Size: 13.33 × 10.5 in  |  font: 16
# ═══════════════════════════════════════════════════════════════════════

fig3 = plt.figure(figsize=(13.333, 10.5))
ax3 = fig3.add_axes([0, 0, 1, 1])
ax3.set_xlim(0, 15)
ax3.set_ylim(-0.20, 14)
ax3.axis('off')
fig3.patch.set_facecolor('white')

FS3 = 16

X3 = {'user': 3.5, 'server': 7.5, 'vehicle': 11.5}
COLORS3 = {
    'user':    ('#D6EAF8', '#2E86C1', '#1A5276'),
    'server':  ('#A9DFBF', '#27AE60', '#145A32'),
    'vehicle': ('#FAD7A0', '#CA6F1E', '#784212'),
}
LABELS3 = {'user': 'User', 'server': 'Server', 'vehicle': 'Vehicle'}

Y_TOP3 = 13.2
Y_BOT3 = 0.30


def actor_box3(x, y, key):
    bg, border, fg = COLORS3[key]
    ax3.add_patch(FancyBboxPatch((x - 1.05, y - 0.26), 2.1, 0.52,
                 boxstyle='round,pad=0.07',
                 facecolor=bg, edgecolor=border, linewidth=2, zorder=5))
    ax3.text(x, y, LABELS3[key], ha='center', va='center',
            fontsize=FS3, fontweight='bold', color=fg, zorder=6)


def lifelines3(y_top, y_bot):
    for x in X3.values():
        ax3.plot([x, x], [y_top, y_bot], '--', color='#AAAAAA', lw=0.9, zorder=1)


def arr3(x1, x2, y, label, dashed=False, color='black'):
    ls = (0, (5, 3)) if dashed else 'solid'
    ax3.annotate('', xy=(x2, y), xytext=(x1, y),
                arrowprops=dict(arrowstyle='->', color=color, lw=1.5,
                               linestyle=ls, connectionstyle='arc3,rad=0'))
    ax3.text((x1 + x2) / 2, y + 0.10, label, ha='center', va='bottom',
            fontsize=FS3, color='#222222' if color == 'black' else color)


def self_loop3(x, y, label, left=False):
    rad = 0.75 if left else -0.75
    ax3.annotate('', xy=(x, y - 0.26), xytext=(x, y),
                arrowprops=dict(arrowstyle='->', color='black', lw=1.3,
                               connectionstyle=f'arc3,rad={rad}'))
    lines = label.split('\n')
    line_h = 0.38
    if left:
        for i, line in enumerate(lines):
            ax3.text(x - 0.20, y - 0.12 - i * line_h, line,
                    ha='right', va='center', fontsize=FS3, zorder=8)
    else:
        for i, line in enumerate(lines):
            ax3.text(x + 0.20, y - 0.12 - i * line_h, line,
                    ha='left', va='center', fontsize=FS3, zorder=8)


def alt_box3(x1, x2, y_top, y_bot, divider_y=None):
    ax3.add_patch(FancyBboxPatch((x1, y_bot), x2 - x1, y_top - y_bot,
                 boxstyle='round,pad=0.12',
                 facecolor='#FFEBEE', edgecolor='#EF5350', linewidth=1.5,
                 alpha=0.30, zorder=2))
    ax3.text(x1 + 0.15, y_top - 0.12, 'alt', ha='left', va='top',
            fontsize=FS3, fontweight='bold', color='#C62828', zorder=5)
    if divider_y is not None:
        ax3.plot([x1, x2], [divider_y, divider_y], '--',
                color='#EF5350', lw=1.2, zorder=3)


def cond_label3(x_center, y, label, neg=True, w=3.8, h=0.35):
    fc, ec, tc = ('#FFCDD2', '#EF5350', '#B71C1C') if neg else ('#C8E6C9', '#4CAF50', '#1B5E20')
    ax3.add_patch(FancyBboxPatch((x_center - w / 2, y - h / 2), w, h,
                 boxstyle='round,pad=0.07',
                 facecolor=fc, edgecolor=ec, linewidth=1.2, zorder=4))
    ax3.text(x_center, y, label, ha='center', va='center',
            fontsize=FS3 - 1, color=tc, fontweight='bold', zorder=5)


# ─── Title & top actors ───────────────────────────────────────────────
ax3.text(7.5, 13.80, 'BLE Key Exchange & Verification — Steps 5–11',
        ha='center', va='center', fontsize=FS3 + 4, fontweight='bold')

for k, x in X3.items():
    actor_box3(x, Y_TOP3, k)

lifelines3(Y_TOP3 - 0.26, Y_BOT3 + 0.26)

# ─── Initial BLE connection ───────────────────────────────────────────
arr3(X3['user'], X3['vehicle'], 12.30, 'Start BLE connection')
self_loop3(X3['vehicle'], 11.45, 'Check stored key')
# sl text bottom: 11.45-0.30=11.15; alt1_top=10.55; gap=0.60 ✓

# ─── Alt block 1: key retrieval ──────────────────────────────────────
alt_box3(3.0, 12.0, 10.55, 6.30, divider_y=7.55)

# Section 1: No key available
cond_label3(9.5, 10.30, 'No key available', neg=True)
# cond bottom: 10.30-0.175=10.125; arr text top: 9.45+0.50=9.95 < 10.125 ✓
arr3(X3['vehicle'], X3['server'], 9.45, 'Request key (VIN)')
self_loop3(X3['server'], 8.65, 'Lookup Vehicle_K', left=True)
# sl text bottom: 8.65-0.30=8.35; arr text top: 7.75+0.50=8.25; gap=0.10 ✓
arr3(X3['server'], X3['vehicle'], 7.75, 'Send Vehicle_K', dashed=True)

# Section 2: Key available
# divider=7.55; cond top: 7.30+0.175=7.475 < 7.55 ✓
cond_label3(9.5, 7.30, 'Key available', neg=False)
self_loop3(X3['vehicle'], 6.75, 'Use local key')
# sl text bottom: 6.75-0.30=6.45 > alt1_bot=6.30 ✓

# ─── BLE key exchange ────────────────────────────────────────────────
# alt1_bot=6.30; arr text top: 5.65+0.50=6.15 < 6.30 ✓
arr3(X3['user'], X3['vehicle'], 5.65, 'Initiate BLE key exchange')
arr3(X3['vehicle'], X3['user'], 4.90, 'Respond to BLE key exchange', dashed=True)
self_loop3(X3['vehicle'], 4.10, 'Verify key match')
# sl text bottom: 4.10-0.30=3.80 > alt2_top=3.40; gap=0.40 ✓

# ─── Alt block 2: key verification result ────────────────────────────
alt_box3(3.0, 12.0, 3.40, 0.65, divider_y=2.05)

# Section 1: Key mismatch
cond_label3(9.5, 3.15, 'Key mismatch', neg=True)
# cond bottom: 3.15-0.175=2.975; arr text top: 2.30+0.50=2.80 < 2.975 ✓
arr3(X3['vehicle'], X3['user'], 2.30, 'BLE disconnect  (key mismatch)', dashed=True, color='#C62828')

# Section 2: Key match
# divider=2.05; cond top: 1.75+0.175=1.925 < 2.05 ✓
cond_label3(9.5, 1.75, 'Key match', neg=False)
# cond bottom: 1.75-0.175=1.575; arr text top: 1.00+0.50=1.50 < 1.575 ✓
arr3(X3['vehicle'], X3['user'], 1.00, 'Secure session established', dashed=True, color='#2E7D32')

# ─── Bottom actors ────────────────────────────────────────────────────
for k, x in X3.items():
    actor_box3(x, Y_BOT3, k)

out3 = 'D:/SCA/Diagram/ble_key_exchange_flow.png'
plt.savefig(out3, dpi=150, bbox_inches='tight', facecolor='white', edgecolor='none')
plt.close()
print(f"Saved to {out3}")
os.startfile(out3)
