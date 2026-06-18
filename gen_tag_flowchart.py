#!/usr/bin/env python3
"""
Tạo tag_flowchart.svg — flowchart cho Tag firmware
(AndroidApp/esp32_firmware/s3_super_mini_central/s3_super_mini_central.ino)

Hai mode:
  Owner : SET_KEY → BLE scan → HMAC auth → UWB ranging
  Friend: SET_BUNDLE_BIN → ECDSA verify → BLE connect friend → UWB ranging

Chạy: python gen_tag_flowchart.py
"""

# ── Canvas ────────────────────────────────────────────────────────────────────
W, H = 1350, 2130

# ── Colour palette (khớp với owner_auth_unlock_flowchart.svg) ─────────────────
C_START = ("#a9d18e", "#548235")
C_END   = ("#f8cbad", "#c55a11")
C_RECT  = ("#9dc3e6", "#2e75b6")
C_DIAM  = ("#f4b183", "#c55a11")
AR      = "#333333"

buf = []

# ── SVG primitives ─────────────────────────────────────────────────────────────

def svg_open():
    buf.append(
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" '
        f'font-family="Arial, sans-serif" font-size="13">\n'
        f'  <rect width="100%" height="100%" fill="#ffffff"/>\n'
        f'  <defs>\n'
        f'    <marker id="ar" markerWidth="10" markerHeight="10" refX="8" refY="3"\n'
        f'            orient="auto" markerUnits="strokeWidth">\n'
        f'      <path d="M0,0 L8,3 L0,6 Z" fill="{AR}"/>\n'
        f'    </marker>\n'
        f'  </defs>'
    )

def svg_close():
    buf.append('</svg>')

def ell(cx, cy, rx, ry, col, *txts):
    f, s = col
    buf.append(f'  <ellipse cx="{cx}" cy="{cy}" rx="{rx}" ry="{ry}" '
               f'fill="{f}" stroke="{s}" stroke-width="1.5"/>')
    _tx(cx, cy, txts)

def bx(x, y, w, h, col, *txts):
    f, s = col
    buf.append(f'  <rect x="{x}" y="{y}" width="{w}" height="{h}" '
               f'fill="{f}" stroke="{s}" stroke-width="1.5"/>')
    _tx(x + w // 2, y + h // 2, txts)

def dm(cx, cy, hw, hh, col, *txts):
    f, s = col
    pts = f"{cx},{cy - hh} {cx + hw},{cy} {cx},{cy + hh} {cx - hw},{cy}"
    buf.append(f'  <polygon points="{pts}" '
               f'fill="{f}" stroke="{s}" stroke-width="1.5"/>')
    _tx(cx, cy, txts)

def _esc(t):
    return t.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")

def _tx(cx, cy, txts):
    n = len(txts)
    for i, t in enumerate(txts):
        dy = round((i - (n - 1) / 2) * 17)
        buf.append(f'  <text x="{cx}" y="{cy + dy + 5}" '
                   f'text-anchor="middle">{_esc(t)}</text>')

def ar(x1, y1, x2, y2):
    buf.append(f'  <line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" '
               f'stroke="{AR}" stroke-width="1.5" marker-end="url(#ar)"/>')

def pa(pts, end=True):
    m = ' marker-end="url(#ar)"' if end else ''
    buf.append(f'  <polyline points="{pts}" fill="none" '
               f'stroke="{AR}" stroke-width="1.5"{m}/>')

def tx(x, y, t, anchor="start", sz=12, bold=False):
    fw = "bold" if bold else "normal"
    buf.append(f'  <text x="{x}" y="{y}" text-anchor="{anchor}" '
               f'font-size="{sz}" font-weight="{fw}">{_esc(t)}</text>')

def dash(x1, y1, x2, y2, c="#aaaaaa"):
    buf.append(f'  <line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" '
               f'stroke="{c}" stroke-width="1" stroke-dasharray="6,4"/>')

# ── Layout ─────────────────────────────────────────────────────────────────────
CX  = 660    # shared centre x
LOx = 275    # owner column centre
RFx = 990    # friend column centre
BW  = 310    # column box width
LX  = LOx - BW // 2   # 120
RX  = RFx - BW // 2   # 835

svg_open()

# ══════════════════════════════════════════════════════════════════════════════
# SHARED TOP
# ══════════════════════════════════════════════════════════════════════════════

# 1. START
ell(CX, 45, 95, 28, C_START, "Bắt Đầu")
ar(CX, 73, CX, 97)

# 2. Init
bx(CX - 245, 97, 490, 55, C_RECT,
   "Khởi động: init BLE (\"UserTag_01\"), đặt TX power +9 dBm,",
   "tải server pubkey + friend bundle từ NVS (nếu đã provision)")
ar(CX, 152, CX, 177)

# 3. NVS bundle?
dm(CX, 232, 205, 52, C_DIAM,
   "Có friend bundle",
   "trong NVS?")
# SAI → down
ar(CX, 284, CX, 308)
tx(CX + 5, 300, "SAI", sz=11)
# ĐÚNG → right → join Friend path tại R6 (BLE scan y=965)
pa(f"{CX + 205},{232} {RFx + 230},{232} {RFx + 230},{990} {RX + BW},{990}")
tx(CX + 210, 224, "ĐÚNG (bundle loaded, isFriendMode=true)", sz=11)

# 4. Wait USB command
bx(CX - 245, 308, 490, 58, C_RECT,
   "Chờ lệnh USB Serial (115200 baud):",
   "SET_KEY / SET_BUNDLE_BIN / SET_SERVER_PUBKEY / SET_TIME")
ar(CX, 366, CX, 391)

# 5. Command type?
dm(CX, 443, 205, 52, C_DIAM,
   "Loại lệnh",
   "nhận được?")
# → Owner (left)
pa(f"{CX - 205},{443} {LOx},{443} {LOx},{505}")
tx(CX - 200, 435, "SET_KEY", sz=11)
# → Friend (right)
pa(f"{CX + 205},{443} {RFx},{443} {RFx},{505}")
tx(CX + 210, 435, "SET_BUNDLE_BIN", sz=11)

# Section divider + labels
dash(CX, 498, CX, 1565)
tx(LOx, 496, "── OWNER MODE ──", anchor="middle", sz=13, bold=True)
tx(RFx, 496, "── FRIEND MODE ──", anchor="middle", sz=13, bold=True)

# ══════════════════════════════════════════════════════════════════════════════
# OWNER MODE  (left, cx = LOx = 275)
# ══════════════════════════════════════════════════════════════════════════════

# L1: Set key
bx(LX, 505, BW, 50, C_RECT,
   "Lưu pairingKey (16 byte hex),",
   "isFriendMode=false, authFailed=false")
ar(LOx, 555, LOx, 580)

# L2: BLE scan  ← re-entry point for retry
bx(LX, 580, BW, 48, C_RECT,
   "BLE scan 10s tìm Anchor",
   "(SERVICE_UUID, RSSI > −100 dBm)")
ar(LOx, 628, LOx, 653)

# L3: Found?
dm(LOx, 703, 155, 50, C_DIAM, "Tìm thấy", "Anchor?")
# SAI → loop left back to L2
pa(f"{LOx - 155},{703} {LX - 55},{703} {LX - 55},{604} {LX},{604}")
tx(LX - 115, 696, "SAI", sz=11)
# ĐÚNG → down
ar(LOx, 753, LOx, 778)
tx(LOx + 5, 771, "ĐÚNG", sz=11)

# L4: Connect BLE
bx(LX, 778, BW, 48, C_RECT,
   "Kết nối BLE (3 retry, MTU=517),",
   "discover SERVICE_UUID + characteristics")
ar(LOx, 826, LOx, 851)

# L5: Connect OK?
dm(LOx, 901, 155, 50, C_DIAM, "Kết nối và discover", "thành công?")
# SAI → loop left back to L2
pa(f"{LOx - 155},{901} {LX - 55},{901} {LX - 55},{604} {LX},{604}")
tx(LX - 110, 894, "SAI", sz=11)
# ĐÚNG → down
ar(LOx, 951, LOx, 976)
tx(LOx + 5, 969, "ĐÚNG", sz=11)

# L6: Get challenge
bx(LX, 976, BW, 50, C_RECT,
   "Nhận Challenge 16B từ Anchor",
   "(BLE notify; fallback readValue nếu miss)")
ar(LOx, 1026, LOx, 1051)

# L7: Compute + send HMAC
bx(LX, 1051, BW, 55, C_RECT,
   "Tính HMAC-SHA256(pairingKey, challenge)",
   "→ write response 32B lên AUTH_CHAR")
ar(LOx, 1106, LOx, 1131)

# L8: AUTH OK?
dm(LOx, 1181, 155, 50, C_DIAM, "Nhận AUTH_OK", "từ Anchor?")
# AUTH_FAIL → small end box right of column
pa(f"{LOx + 155},{1181} {LX + BW + 55},{1181} {LX + BW + 55},{1246}")
tx(LX + BW + 5, 1174, "AUTH_FAIL", sz=11)
bx(LX + BW + 5, 1246, 195, 48, C_END,
   "authFailed=true; chờ SET_KEY",
   "mới → restart scan từ L2")

# AUTH_OK → down toward merge point
ar(LOx, 1231, LOx, 1565)
tx(LOx + 5, 1265, "AUTH_OK", sz=11)

# ══════════════════════════════════════════════════════════════════════════════
# FRIEND MODE  (right, cx = RFx = 990)
# ══════════════════════════════════════════════════════════════════════════════

# R1: Decode base64
bx(RX, 505, BW, 50, C_RECT,
   "Decode base64 → 106-byte binary",
   "(BUNDLE_BIN_SIZE = 106 bytes)")
ar(RFx, 555, RFx, 580)

# R2: version + time check
dm(RFx, 635, 165, 55, C_DIAM,
   "version==1 &",
   "chưa hết hạn (expires_at)?")
# SAI → small BUNDLE_ERR box right of column
pa(f"{RFx + 165},{635} {RX + BW + 55},{635} {RX + BW + 55},{700}")
tx(RX + BW + 5, 628, "SAI", sz=11)
bx(RX + BW + 5, 700, 155, 40, C_END, "BUNDLE_ERR:", "expired / version")
# ĐÚNG → down
ar(RFx, 690, RFx, 715)
tx(RFx + 5, 708, "ĐÚNG", sz=11)

# R3: ECDSA verify
bx(RX, 715, BW, 62, C_RECT,
   "ECDSA verify offline (mbedTLS P-256):",
   "SHA256(bundle[0..41]) vs",
   "sig_r=bundle[42..73], sig_s=bundle[74..105]")
ar(RFx, 777, RFx, 802)

# R4: ECDSA OK?
dm(RFx, 852, 165, 50, C_DIAM, "Chữ ký ECDSA", "hợp lệ?")
# SAI → joins same BUNDLE_ERR box (bottom edge y=740)
pa(f"{RFx + 165},{852} {RX + BW + 82},{852} {RX + BW + 82},{740}")
tx(RX + BW + 5, 845, "SAI", sz=11)
# ĐÚNG → down
ar(RFx, 902, RFx, 927)
tx(RFx + 5, 920, "ĐÚNG", sz=11)

# R5: Save + activate
bx(RX, 927, BW, 48, C_RECT,
   "Save bundle NVS, isFriendMode=true,",
   "Serial \"BUNDLE_OK:<friend_id_hex>\"")
ar(RFx, 975, RFx, 998)

# R6: BLE scan  ← NVS path joins here
bx(RX, 998, BW, 48, C_RECT,
   "BLE scan tìm Anchor",
   "(khớp SERVICE_UUID trong advertisement)")
ar(RFx, 1046, RFx, 1071)

# R7: Found?
dm(RFx, 1121, 155, 50, C_DIAM, "Tìm thấy", "Anchor?")
# SAI → loop right back to R6
pa(f"{RFx + 155},{1121} {RX + BW + 50},{1121} {RX + BW + 50},{1022} {RX + BW},{1022}")
tx(RX + BW + 5, 1114, "SAI", sz=11)
# ĐÚNG → down
ar(RFx, 1171, RFx, 1196)
tx(RFx + 5, 1189, "ĐÚNG", sz=11)

# R8: Connect + discover friend service
bx(RX, 1196, BW, 48, C_RECT,
   "Kết nối BLE (MTU=517),",
   "discover FRIEND_SERVICE (UUID 0xFACE)")
ar(RFx, 1244, RFx, 1269)

# R9: Write bundle
bx(RX, 1269, BW, 50, C_RECT,
   "Đăng ký notify FRIEND_STATUS_CHAR,",
   "write 106-byte bundle → FRIEND_BUNDLE_CHAR")
ar(RFx, 1319, RFx, 1344)

# R10: Status received?
dm(RFx, 1394, 165, 50, C_DIAM,
   "Nhận FRIEND_STATUS notify",
   "trong 10 giây?")
# Timeout → retry (loop right back to R6 scan)
pa(f"{RFx + 165},{1394} {RX + BW + 50},{1394} {RX + BW + 50},{1022} {RX + BW},{1022}")
tx(RX + BW + 5, 1387, "Timeout", sz=11)
# ĐÚNG → down
ar(RFx, 1444, RFx, 1469)
tx(RFx + 5, 1462, "ĐÚNG", sz=11)

# R11: FRIEND_OK?
dm(RFx, 1519, 155, 50, C_DIAM, "FRIEND_OK", "(status byte[0] == 0)?")
# SAI → DENIED box left of diamond
pa(f"{RFx - 155},{1519} {RX - 75},{1519} {RX - 75},{1589}")
tx(RX - 135, 1512, "SAI", sz=11)
bx(RX - 205, 1589, 195, 48, C_END,
   "FRIEND_ACCESS_DENIED,",
   "ngắt BLE; chờ bundle mới")
# ĐÚNG → down toward merge
ar(RFx, 1569, RFx, 1600)
tx(RFx + 5, 1590, "ĐÚNG", sz=11)

# ══════════════════════════════════════════════════════════════════════════════
# MERGE  →  UWB RANGING
# ══════════════════════════════════════════════════════════════════════════════

tx(CX, 1615, "── UWB RANGING (Owner + Friend) ──",
   anchor="middle", sz=13, bold=True)

# Owner (LOx, 1565) → into UWB1 box left side
pa(f"{LOx},{1565} {LOx},{1627} {CX - 240},{1627}")
# Friend (RFx, 1600) → into UWB1 box right side
pa(f"{RFx},{1600} {RFx},{1627} {CX + 240},{1627}")

# UWB1: Arm UWB
bx(CX - 240, 1627, 480, 52, C_RECT,
   "Gửi TAG_UWB_READY → Anchor (BLE write);",
   "chờ UWB_ACTIVE notify (retry 5s nếu không nhận)")
ar(CX, 1679, CX, 1704)

# UWB2: SS-TWR initiator loop  ← re-entry for retry
bx(CX - 240, 1704, 480, 62, C_RECT,
   "UWB SS-TWR initiator: gửi POLL frame,",
   "nhận RESP, kiểm tra STS quality (chống relay attack),",
   "tính ToF → lọc moving average 5 mẫu → khoảng cách")
ar(CX, 1766, CX, 1791)

# UWB3: > FAR (10 m)?
dm(CX, 1844, 200, 53, C_DIAM, "Khoảng cách", "> 10m (FAR)?")
# ĐÚNG → right: stop UWB, RSSI monitor
pa(f"{CX + 200},{1844} {CX + 325},{1844} {CX + 325},{1910}")
tx(CX + 205, 1836, "ĐÚNG", sz=11)
bx(CX + 240, 1910, 245, 58, C_RECT,
   "Dừng UWB (deinit DW3000);",
   "giám sát RSSI mỗi 1s —",
   "RSSI > −100? → re-arm UWB1")
# Re-arm loop back to UWB1
pa(f"{CX + 362},{1910} {CX + 362},{1653} {CX + 240},{1653}")
tx(CX + 370, 1800, "re-arm", sz=11)
# SAI → down
ar(CX, 1897, CX, 1922)
tx(CX + 5, 1914, "SAI", sz=11)

# UWB4: UNLOCK threshold?
dm(CX, 1975, 215, 53, C_DIAM,
   "dist ≤ 10m VÀ",
   "chưa trong unlock zone?")
# ĐÚNG → left: send VERIFIED
pa(f"{CX - 215},{1975} {CX - 335},{1975} {CX - 335},{2045}")
tx(CX - 330, 1967, "ĐÚNG", sz=11)
bx(CX - 465, 2045, 260, 50, C_RECT,
   "Gửi VERIFIED:<dist>m → Anchor,",
   "tagInUnlockZone = true")
# SAI → down
ar(CX, 2028, CX, 2053)
tx(CX + 5, 2046, "SAI", sz=11)

# UWB5: LOCK threshold?
dm(CX, 2106, 215, 48, C_DIAM,
   "dist > 10.5m VÀ",
   "đang trong unlock zone?")
# ĐÚNG → right: send WARNING
pa(f"{CX + 215},{2106} {CX + 335},{2106} {CX + 335},{2045}")
tx(CX + 220, 2098, "ĐÚNG", sz=11)
bx(CX + 245, 2045, 255, 50, C_RECT,
   "Gửi WARNING:<dist>m → Anchor,",
   "tagInUnlockZone = false")

# Loop back to UWB2: từ VERIFIED box bottom
pa(f"{CX - 335},{2095} {CX - 335},{2150} {CX - 400},{2150} {CX - 400},{1734} {CX - 240},{1734}")
# Loop back to UWB2: từ WARNING box bottom
pa(f"{CX + 335},{2095} {CX + 335},{2150} {CX + 400},{2150} {CX + 400},{1734} {CX + 240},{1734}")
# SAI từ LOCK diamond: loop back
pa(f"{CX - 215},{2106} {CX - 400},{2106} {CX - 400},{1734} {CX - 240},{1734}")

svg_close()

with open("tag_flowchart.svg", "w", encoding="utf-8") as f:
    f.write('\n'.join(buf))

print(f"Generated: tag_flowchart.svg  ({W}x{H})")
