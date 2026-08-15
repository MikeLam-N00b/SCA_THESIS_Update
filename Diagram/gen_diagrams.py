#!/usr/bin/env python3
"""
SCA Thesis — FreeRTOS Task Diagrams
Generates 6 PNG files for PPT:
  1. overview_tasks.png        — Block diagram: 5 tasks + IPC + external
  2. flow_ble_task.png         — bleTask flowchart
  3. flow_uwb_task.png         — uwbTask flowchart
  4. flow_can_task.png         — canTask flowchart
  5. flow_friend_task.png      — friendMgmtTask flowchart (5-step verify)
  6. flow_revoc_task.png       — revocationSyncTask flowchart
"""

import os
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from matplotlib.patches import FancyBboxPatch
import matplotlib.lines as mlines

OUT = r"D:\SCA\Diagram"
os.makedirs(OUT, exist_ok=True)

plt.rcParams.update({
    "font.family": "DejaVu Sans",
    "figure.facecolor": "white",
    "axes.facecolor": "white",
})

# ─── Palette ──────────────────────────────────────────────────────────────────
BLE_F,  BLE_E  = "#BBDEFB", "#1565C0"
UWB_F,  UWB_E  = "#E1BEE7", "#6A1B9A"
CAN_F,  CAN_E  = "#C8E6C9", "#2E7D32"
FRD_F,  FRD_E  = "#FFE0B2", "#E65100"
REV_F,  REV_E  = "#ECEFF1", "#455A64"
IPC_F,  IPC_E  = "#FFF9C4", "#F9A825"
EXT_F,  EXT_E  = "#F5F5F5", "#9E9E9E"
OK_F,   OK_E   = "#E8F5E9", "#388E3C"
NG_F,   NG_E   = "#FFEBEE", "#C62828"
PROC_F, PROC_E = "#E3F2FD", "#1565C0"
DEC_F,  DEC_E  = "#FFF8E1", "#F57F17"
TRM_F,  TRM_E  = "#263238", "#263238"
ACT_F,  ACT_E  = "#F3E5F5", "#6A1B9A"

# ─── Low-level drawing helpers ─────────────────────────────────────────────────

def _text(ax, cx, cy, txt, fs=8.5, color="black", bold=False, ha="center", va="center"):
    ax.text(cx, cy, txt, ha=ha, va=va, fontsize=fs,
            fontweight="bold" if bold else "normal",
            color=color, multialignment="center", zorder=5)


def proc(ax, cx, cy, w, h, txt, fc=PROC_F, ec=PROC_E, fs=8.5, bold=False):
    ax.add_patch(FancyBboxPatch((cx-w/2, cy-h/2), w, h,
                                boxstyle="round,pad=0.04",
                                fc=fc, ec=ec, lw=1.6, zorder=3))
    _text(ax, cx, cy, txt, fs=fs, bold=bold)
    return cy - h/2   # bottom edge


def term(ax, cx, cy, w, h, txt, fs=9):
    ax.add_patch(FancyBboxPatch((cx-w/2, cy-h/2), w, h,
                                boxstyle="round,pad=0.12",
                                fc=TRM_F, ec=TRM_E, lw=2, zorder=3))
    _text(ax, cx, cy, txt, fs=fs, color="white", bold=True)
    return cy - h/2


def dec(ax, cx, cy, w, h, txt, fc=DEC_F, ec=DEC_E, fs=8):
    pts = [[cx, cy+h/2], [cx+w/2, cy], [cx, cy-h/2], [cx-w/2, cy]]
    ax.add_patch(plt.Polygon(pts, closed=True, fc=fc, ec=ec, lw=1.6, zorder=3))
    _text(ax, cx, cy, txt, fs=fs)
    return cy - h/2


def arr(ax, x1, y1, x2, y2, lbl="", lbl_x_off=0.12, lbl_ha="left",
        color="#444", rad=0):
    cs = f"arc3,rad={rad}" if rad else "arc3,rad=0"
    ax.annotate("", xy=(x2, y2), xytext=(x1, y1),
                arrowprops=dict(arrowstyle="->", color=color, lw=1.4,
                                connectionstyle=cs), zorder=4)
    if lbl:
        mx, my = (x1+x2)/2, (y1+y2)/2
        ax.text(mx + lbl_x_off, my, lbl, fontsize=7.5, color="#C62828",
                ha=lbl_ha, va="center", fontweight="bold", zorder=5)


def arr_elbow(ax, x1, y1, x2, y2, lbl="", color="#444"):
    ax.annotate("", xy=(x2, y2), xytext=(x1, y1),
                arrowprops=dict(arrowstyle="->", color=color, lw=1.4,
                                connectionstyle="angle,angleA=90,angleB=0"), zorder=4)
    if lbl:
        ax.text((x1+x2)/2, (y1+y2)/2 + 0.08, lbl, fontsize=7.5, color="#C62828",
                ha="center", va="bottom", fontweight="bold", zorder=5)


def new_ax(title, w=9, h=17, xl=(0, 10), yl=(0, 20)):
    fig, ax = plt.subplots(figsize=(w, h))
    fig.patch.set_facecolor("white")
    ax.set_facecolor("white")
    ax.set_xlim(*xl)
    ax.set_ylim(*yl)
    ax.axis("off")
    ax.set_title(title, fontsize=13, fontweight="bold",
                 color="#1A237E", pad=14, loc="center")
    return fig, ax


def save(fig, fname):
    p = os.path.join(OUT, fname)
    fig.savefig(p, dpi=200, bbox_inches="tight",
                facecolor="white", edgecolor="none")
    plt.close(fig)
    print(f"  OK  {fname}")


# ══════════════════════════════════════════════════════════════════════════════
# 1. OVERVIEW BLOCK DIAGRAM
# ══════════════════════════════════════════════════════════════════════════════

def draw_overview():
    fig, ax = plt.subplots(figsize=(18, 11))
    fig.patch.set_facecolor("white")
    ax.set_facecolor("white")
    ax.set_xlim(0, 22)
    ax.set_ylim(0, 13)
    ax.axis("off")
    ax.set_title("SCA Anchor — FreeRTOS Task Architecture",
                 fontsize=15, fontweight="bold", color="#1A237E", pad=16)

    # ── ESP32 outer box ──────────────────────────────────────────────────────
    esp = FancyBboxPatch((3.2, 1.2), 13.8, 10.2,
                         boxstyle="round,pad=0.1",
                         fc="#F8F9FA", ec="#455A64", lw=2.5, zorder=1)
    ax.add_patch(esp)
    ax.text(10.1, 11.65, "ESP32 (Anchor)", fontsize=11, fontweight="bold",
            ha="center", color="#37474F", zorder=6)

    # ── Core 0 ───────────────────────────────────────────────────────────────
    c0 = FancyBboxPatch((3.5, 1.5), 6.0, 9.5,
                         boxstyle="round,pad=0.08",
                         fc="#EDE7F6", ec="#7B1FA2", lw=1.8, zorder=2, alpha=0.5)
    ax.add_patch(c0)
    ax.text(6.5, 11.15, "Core 0  (WiFi / BLE Radio)", fontsize=9.5,
            ha="center", color="#4A148C", fontweight="bold", zorder=6)

    # ── Core 1 ───────────────────────────────────────────────────────────────
    c1 = FancyBboxPatch((10.1, 1.5), 6.6, 9.5,
                         boxstyle="round,pad=0.08",
                         fc="#E3F2FD", ec="#1565C0", lw=1.8, zorder=2, alpha=0.5)
    ax.add_patch(c1)
    ax.text(13.4, 11.15, "Core 1  (UWB / CAN)", fontsize=9.5,
            ha="center", color="#0D47A1", fontweight="bold", zorder=6)

    # ── IPC column (centre) ───────────────────────────────────────────────────
    def ipc_box(cy, txt):
        ax.add_patch(FancyBboxPatch((9.15, cy-0.35), 1.6, 0.7,
                                    boxstyle="round,pad=0.04",
                                    fc=IPC_F, ec=IPC_E, lw=1.3, zorder=3))
        ax.text(9.95, cy, txt, ha="center", va="center", fontsize=7.2,
                color="#5D4037", fontweight="bold", zorder=5)

    ipc_box(9.0,  "bleQueue")
    ipc_box(7.5,  "friendMgmt\nQueue")
    ipc_box(6.0,  "uwbQueue")
    ipc_box(4.5,  "canQueue")
    ipc_box(3.2,  "spiMutex")
    ipc_box(2.0,  "EventGroup")
    ax.text(9.95, 10.1, "IPC", ha="center", fontsize=8,
            color="#795548", fontweight="bold")

    # ── Task boxes ───────────────────────────────────────────────────────────
    def task_box(cx, cy, w, h, name, sub, fc, ec):
        ax.add_patch(FancyBboxPatch((cx-w/2, cy-h/2), w, h,
                                    boxstyle="round,pad=0.07",
                                    fc=fc, ec=ec, lw=2, zorder=4))
        ax.text(cx, cy+0.25, name, ha="center", va="center",
                fontsize=9.5, fontweight="bold", color=ec, zorder=5)
        ax.text(cx, cy-0.22, sub, ha="center", va="center",
                fontsize=7.5, color="#424242", zorder=5)

    # Core 0 tasks
    task_box(6.5, 9.5, 5.2, 1.4, "bleTask", "P3 | Stack 10 KB", BLE_F, BLE_E)
    task_box(6.5, 6.8, 5.2, 1.6, "friendMgmtTask", "P2 | Stack 16 KB", FRD_F, FRD_E)
    task_box(6.5, 3.8, 5.2, 1.4, "revocationSyncTask", "P1 | Stack 8 KB", REV_F, REV_E)

    # Core 1 tasks
    task_box(13.4, 9.2, 5.8, 1.4, "uwbTask", "P4 | Stack 8 KB", UWB_F, UWB_E)
    task_box(13.4, 5.8, 5.8, 1.4, "canTask", "P2 | Stack 4 KB", CAN_F, CAN_E)

    # ── External boxes ───────────────────────────────────────────────────────
    def ext(cx, cy, w, h, txt, fc, ec, fs=8.5):
        ax.add_patch(FancyBboxPatch((cx-w/2, cy-h/2), w, h,
                                    boxstyle="round,pad=0.08",
                                    fc=fc, ec=ec, lw=1.8, zorder=3))
        ax.text(cx, cy, txt, ha="center", va="center",
                fontsize=fs, fontweight="bold", color=ec, zorder=5,
                multialignment="center")

    # Left: App + Server
    ext(1.3, 10.2, 2.0, 1.0, "Android\nApp", "#E3F2FD", "#1565C0")
    ext(1.3, 7.5, 2.0, 1.0, "Cloud\nServer", "#E8EAF6", "#283593")

    # Right: hardware
    ext(20.7, 9.2, 2.2, 1.0, "DW3000\nUWB Module", UWB_F, UWB_E)
    ext(20.7, 5.8, 2.2, 1.0, "MCP2515\nCAN Module", CAN_F, CAN_E)
    ext(20.7, 3.5, 2.2, 0.85, "CAN Bus\n(Vehicle ECU)", "#FAFAFA", "#455A64")

    # ── Connection arrows ────────────────────────────────────────────────────
    def con(x1, y1, x2, y2, lbl="", col="#444"):
        ax.annotate("", xy=(x2, y2), xytext=(x1, y1),
                    arrowprops=dict(arrowstyle="<->", color=col, lw=1.4,
                                    connectionstyle="arc3,rad=0"), zorder=4)
        if lbl:
            ax.text((x1+x2)/2, (y1+y2)/2 + 0.12, lbl,
                    ha="center", fontsize=7, color=col, zorder=5)

    def one(x1, y1, x2, y2, lbl="", col="#444"):
        ax.annotate("", xy=(x2, y2), xytext=(x1, y1),
                    arrowprops=dict(arrowstyle="->", color=col, lw=1.3,
                                    connectionstyle="arc3,rad=0"), zorder=4)
        if lbl:
            ax.text((x1+x2)/2, (y1+y2)/2 + 0.10, lbl,
                    ha="center", fontsize=6.8, color=col, zorder=5)

    # App ↔ bleTask (BLE)
    con(2.3, 10.2, 3.5, 9.5, "BLE", "#1565C0")
    # App → friendMgmt (bundle claim)
    one(2.3, 9.8, 3.5, 7.2, "", "#E65100")
    # Server ↔ friendMgmt
    con(2.3, 7.5, 3.5, 6.8, "HTTP\nHMAC", "#283593")
    # Server ↔ revocSync
    con(2.3, 7.2, 3.5, 3.8, "HTTP\nrevoc.", "#283593")

    # uwbTask ↔ DW3000
    con(16.2, 9.2, 19.6, 9.2, "SPI", UWB_E)
    # canTask ↔ MCP2515
    con(16.2, 5.8, 19.6, 5.8, "SPI", CAN_E)
    # MCP2515 → CAN bus
    one(20.7, 5.3, 20.7, 3.93, "ISO 11898", "#455A64")

    # bleTask → friendMgmtQueue
    one(6.5, 8.8, 9.15, 7.5, "bundle", FRD_E)
    # bleTask ← friendMgmt (status)
    one(9.15, 7.0, 6.5, 8.65, "result", "#388E3C")
    # friendMgmt → uwbQueue
    one(8.7, 6.4, 9.15, 6.0, "INIT", UWB_E)
    # friendMgmt → canQueue
    one(8.7, 6.15, 9.15, 4.5, "UNLOCK", CAN_E)
    # uwbTask ← uwbQueue
    one(10.75, 6.0, 10.4, 8.85, "", UWB_E)
    # canTask ← canQueue
    one(10.75, 4.5, 10.4, 5.5, "", CAN_E)
    # revocSync ← EventGroup
    one(9.15, 2.0, 6.5, 3.1, "AUTH_BIT", "#F57F17")
    # spiMutex shared
    one(9.15, 3.2, 13.4, 8.5, "mutex", "#455A64")
    one(9.15, 3.2, 13.4, 5.1, "mutex", "#455A64")

    # ── Legend ───────────────────────────────────────────────────────────────
    legend_items = [
        mpatches.Patch(fc=BLE_F, ec=BLE_E, label="bleTask"),
        mpatches.Patch(fc=FRD_F, ec=FRD_E, label="friendMgmtTask"),
        mpatches.Patch(fc=REV_F, ec=REV_E, label="revocationSyncTask"),
        mpatches.Patch(fc=UWB_F, ec=UWB_E, label="uwbTask"),
        mpatches.Patch(fc=CAN_F, ec=CAN_E, label="canTask"),
        mpatches.Patch(fc=IPC_F, ec=IPC_E, label="IPC (queue/mutex/event)"),
    ]
    ax.legend(handles=legend_items, loc="lower right",
              fontsize=8, framealpha=0.9, edgecolor="#BDBDBD",
              bbox_to_anchor=(1.0, 0.0))

    save(fig, "overview_tasks.png")


# ══════════════════════════════════════════════════════════════════════════════
# 2. bleTask FLOWCHART
# ══════════════════════════════════════════════════════════════════════════════

def draw_ble_task():
    fig, ax = new_ax("bleTask — FreeRTOS Flowchart", w=9, h=19, yl=(0, 22))
    CX = 5.0

    # Step positions (y, top to bottom)
    Y = {
        "start":      21.2,
        "init_gatt":  20.0,
        "advertise":  18.7,
        "wait_conn":  17.4,
        "gen_chall":  16.1,
        "wait_hmac":  14.8,
        "d_timeout":  13.4,
        "verify_hmac":12.0,
        "d_valid":    10.7,
        "set_auth":    9.4,
        "d_mode":      8.1,
        "owner_cmds":  6.8,
        "wait_bundle": 6.8,
        "accum":       5.5,
        "post_queue":  4.2,
        "wait_result": 2.9,
        "send_status": 1.6,
        "end":         0.4,
    }

    # Draw shapes
    term(ax, CX, Y["start"],      3.2, 0.6, "START  bleTask")
    proc(ax, CX, Y["init_gatt"],  5.5, 0.75, "Init BLE GATT Server\n(Service + 4 Characteristics)", fc=BLE_F, ec=BLE_E)
    proc(ax, CX, Y["advertise"],  5.5, 0.75, "Start BLE Advertising", fc=BLE_F, ec=BLE_E)
    proc(ax, CX, Y["wait_conn"],  5.5, 0.75, "Wait for BLE Connection\n(portMAX_DELAY)", fc=BLE_F, ec=BLE_E)
    proc(ax, CX, Y["gen_chall"],  5.5, 0.75, "Generate 16-byte Challenge\nSend via CHALLENGE_CHAR (delay 2s)", fc=BLE_F, ec=BLE_E)
    proc(ax, CX, Y["wait_hmac"],  5.5, 0.75, "Wait for HMAC response\nvia AUTH_CHAR  (timeout 30s)", fc=BLE_F, ec=BLE_E)
    dec(ax,  CX, Y["d_timeout"],  5.0, 1.1, "Timeout?", fc=DEC_F, ec=DEC_E)
    proc(ax, CX, Y["verify_hmac"],5.5, 0.75, "Verify HMAC-SHA256\n(pairing_key, challenge)", fc=BLE_F, ec=BLE_E)
    dec(ax,  CX, Y["d_valid"],    5.0, 1.1, "HMAC valid?", fc=DEC_F, ec=DEC_E)
    proc(ax, CX, Y["set_auth"],   5.5, 0.75, "Set AUTH_BIT in EventGroup\nSend ACK via AUTH_CHAR", fc=OK_F, ec=OK_E)
    dec(ax,  CX, Y["d_mode"],     5.0, 1.1, "Friend\nbundle mode?", fc=DEC_F, ec=DEC_E)

    # Owner branch (right)
    proc(ax, 8.2, Y["owner_cmds"], 3.2, 0.75,
         "Handle owner\ncommands\n(stay connected)", fc=BLE_F, ec=BLE_E, fs=7.5)

    # Friend branch (left / centre continues down)
    proc(ax, CX, Y["wait_bundle"], 5.5, 0.75,
         "Accumulate FRIEND_BUNDLE_CHAR\nwrites (fragmented, max 106B)", fc=FRD_F, ec=FRD_E)
    proc(ax, CX, Y["accum"],       5.5, 0.75,
         "Post 106-byte bundle\nto friendMgmtQueue", fc=FRD_F, ec=FRD_E)
    proc(ax, CX, Y["post_queue"],  5.5, 0.75,
         "Wait for verify result\nfrom friendMgmtTask", fc=FRD_F, ec=FRD_E)
    proc(ax, CX, Y["wait_result"], 5.5, 0.75,
         "Send FRIEND_STATUS_CHAR\n[accepted, reason_code]", fc=FRD_F, ec=FRD_E)
    proc(ax, CX, Y["send_status"], 5.5, 0.75,
         "Client disconnects\n→ restart advertising", fc=BLE_F, ec=BLE_E)
    term(ax, CX, Y["end"],         3.2, 0.6, "END  (never reached)")

    # ── Arrows ────────────────────────────────────────────────────────────────
    def dn(ya, yb): arr(ax, CX, Y[ya]-0.375, CX, Y[yb]+0.375)

    dn("start",      "init_gatt")
    dn("init_gatt",  "advertise")
    dn("advertise",  "wait_conn")
    dn("wait_conn",  "gen_chall")
    dn("gen_chall",  "wait_hmac")
    dn("wait_hmac",  "d_timeout")

    # Timeout YES → right → back to advertise
    arr(ax, CX+2.5, Y["d_timeout"], 8.5, Y["d_timeout"], lbl="Yes", lbl_ha="left", lbl_x_off=0.05)
    arr_elbow(ax, 8.5, Y["d_timeout"], CX+2.75, Y["advertise"])
    ax.text(8.55, (Y["d_timeout"]+Y["advertise"])/2, "Disconnect\n→ re-advertise",
            fontsize=7, color="#C62828", ha="left", va="center")

    # Timeout NO → verify
    arr(ax, CX-2.5, Y["d_timeout"], CX-2.5, Y["verify_hmac"],
        lbl="No", lbl_x_off=-0.5, lbl_ha="right")
    arr(ax, CX-2.5, Y["verify_hmac"], CX-2.75, Y["verify_hmac"])

    dn("d_timeout",  "verify_hmac")  # direct centre arrow
    dn("verify_hmac","d_valid")

    # HMAC invalid → right → disconnect
    arr(ax, CX+2.5, Y["d_valid"], 8.5, Y["d_valid"], lbl="No", lbl_ha="left", lbl_x_off=0.05)
    ax.text(8.55, Y["d_valid"] - 0.3, "Send NACK\nDisconnect", fontsize=7,
            color="#C62828", ha="left", va="top")

    # HMAC valid → set_auth
    arr(ax, CX, Y["d_valid"]-0.55, CX, Y["set_auth"]+0.375, lbl="Yes",
        lbl_x_off=0.08)
    dn("set_auth",   "d_mode")

    # Friend mode YES → centre
    arr(ax, CX, Y["d_mode"]-0.55, CX, Y["wait_bundle"]+0.375,
        lbl="Yes", lbl_x_off=0.08)

    # Friend mode NO → right (owner)
    arr(ax, CX+2.5, Y["d_mode"], 8.2-1.6, Y["d_mode"],
        lbl="No", lbl_ha="left", lbl_x_off=0.05)

    dn("wait_bundle","accum")
    dn("accum",      "post_queue")
    dn("post_queue", "wait_result")
    dn("wait_result","send_status")
    dn("send_status","end")

    # Loop arrow: send_status → advertise
    arr(ax, CX-2.75, Y["send_status"], 0.5, Y["send_status"])
    arr(ax, 0.5, Y["send_status"], 0.5, Y["advertise"])
    arr(ax, 0.5, Y["advertise"], CX-2.75, Y["advertise"])

    save(fig, "flow_ble_task.png")


# ══════════════════════════════════════════════════════════════════════════════
# 3. uwbTask FLOWCHART
# ══════════════════════════════════════════════════════════════════════════════

def draw_uwb_task():
    fig, ax = new_ax("uwbTask — FreeRTOS Flowchart", w=9, h=18, yl=(0, 21))
    CX = 5.0

    Y = {
        "start":   20.3,
        "wait_init": 19.0,
        "acq_spi": 17.7,
        "d_mutex": 16.4,
        "init_dw": 15.1,
        "set_sts":  13.8,
        "rel_spi":  12.5,
        "set_rdy":  11.2,
        "rx_en":    9.9,
        "wait_rx":  8.6,
        "d_rxok":   7.3,
        "d_sts":    6.0,
        "d_poll":   4.7,
        "calc_tx":  3.4,
        "send_resp":2.1,
        "d_deinit": 0.9,
    }

    term(ax, CX, Y["start"],    3.2, 0.6, "START  uwbTask")
    proc(ax, CX, Y["wait_init"],5.5, 0.75,
         "xQueueReceive(uwbQueue, portMAX_DELAY)\nWait for UWB_CMD_INIT", fc=UWB_F, ec=UWB_E)
    proc(ax, CX, Y["acq_spi"],  5.5, 0.75,
         "Acquire spiMutex\n(timeout 500 ms)", fc=IPC_F, ec=IPC_E)
    dec(ax,  CX, Y["d_mutex"],  5.0, 1.1, "Mutex OK?")
    proc(ax, CX, Y["init_dw"],  5.5, 0.75,
         "Init DW3000\n(Ch5, STS Mode 1, 850 kbps, PAC32, Preamble 1024)", fc=UWB_F, ec=UWB_E)
    proc(ax, CX, Y["set_sts"],  5.5, 0.75,
         "Set STS key\n(pairing_key or active friend_key)", fc=UWB_F, ec=UWB_E)
    proc(ax, CX, Y["rel_spi"],  5.5, 0.75,
         "Release spiMutex", fc=IPC_F, ec=IPC_E)
    proc(ax, CX, Y["set_rdy"],  5.5, 0.75,
         "Set UWB_READY_BIT in EventGroup\n→ notify friendMgmtTask", fc=OK_F, ec=OK_E)

    # Ranging loop header
    ax.add_patch(FancyBboxPatch((2.0, Y["rx_en"]-0.45), 6.0, 0.9,
                                boxstyle="round,pad=0.06",
                                fc="#EDE7F6", ec=UWB_E, lw=2, ls="--", zorder=2))
    ax.text(CX, Y["rx_en"]+0.55, "── Ranging Loop ──",
            ha="center", fontsize=8, color=UWB_E, fontweight="bold")

    proc(ax, CX, Y["rx_en"],    5.5, 0.75,
         "dwt_rxenable(DWT_START_RX_IMMEDIATE)", fc=UWB_F, ec=UWB_E)
    proc(ax, CX, Y["wait_rx"],  5.5, 0.75,
         "xTaskNotifyWait — wait RX IRQ", fc=UWB_F, ec=UWB_E)
    dec(ax,  CX, Y["d_rxok"],   5.0, 1.1, "RX status OK?")
    dec(ax,  CX, Y["d_sts"],    5.0, 1.1, "STS quality\nvalid?")
    dec(ax,  CX, Y["d_poll"],   5.0, 1.1, "POLL frame\n(magic bytes)?")
    proc(ax, CX, Y["calc_tx"],  5.5, 0.75,
         "Compute TX time\nRX_ts + POLL_RX_TO_RESP_TX_DLY (3000 µs)", fc=UWB_F, ec=UWB_E)
    proc(ax, CX, Y["send_resp"],5.5, 0.75,
         "Transmit RESPONSE frame\n(RX & TX timestamps embedded)", fc=UWB_F, ec=UWB_E)
    dec(ax,  CX, Y["d_deinit"], 5.0, 1.1, "UWB_CMD_DEINIT\nreceived?")

    def dn(a, b): arr(ax, CX, Y[a]-0.375, CX, Y[b]+0.375)

    dn("start",    "wait_init")
    dn("wait_init","acq_spi")
    dn("acq_spi",  "d_mutex")
    # Mutex fail → loop back
    arr(ax, CX+2.5, Y["d_mutex"], 8.5, Y["d_mutex"], lbl="No", lbl_x_off=0.05)
    arr_elbow(ax, 8.5, Y["d_mutex"], CX+2.75, Y["acq_spi"])
    ax.text(8.55, (Y["d_mutex"]+Y["acq_spi"])/2, "Log error\nRetry", fontsize=7,
            color="#C62828", ha="left", va="center")
    arr(ax, CX, Y["d_mutex"]-0.55, CX, Y["init_dw"]+0.375, lbl="Yes", lbl_x_off=0.08)

    dn("init_dw",  "set_sts")
    dn("set_sts",  "rel_spi")
    dn("rel_spi",  "set_rdy")
    dn("set_rdy",  "rx_en")
    dn("rx_en",    "wait_rx")
    dn("wait_rx",  "d_rxok")

    # RX fail → restart
    arr(ax, CX+2.5, Y["d_rxok"], 8.5, Y["d_rxok"], lbl="No", lbl_x_off=0.05)
    arr_elbow(ax, 8.5, Y["d_rxok"], CX+2.75, Y["rx_en"])

    arr(ax, CX, Y["d_rxok"]-0.55, CX, Y["d_sts"]+0.55, lbl="Yes", lbl_x_off=0.08)

    # STS fail → restart
    arr(ax, CX+2.5, Y["d_sts"], 8.8, Y["d_sts"], lbl="No", lbl_x_off=0.05)
    arr_elbow(ax, 8.8, Y["d_sts"], CX+2.75, Y["rx_en"])
    ax.text(8.85, (Y["d_sts"]+Y["rx_en"])/2, "Discard\nframe", fontsize=7,
            color="#C62828", ha="left", va="center")

    arr(ax, CX, Y["d_sts"]-0.55, CX, Y["d_poll"]+0.55, lbl="Yes", lbl_x_off=0.08)

    # Not POLL → restart
    arr(ax, CX-2.5, Y["d_poll"], 0.8, Y["d_poll"], lbl="No", lbl_ha="right",
        lbl_x_off=-0.5)
    arr_elbow(ax, 0.8, Y["d_poll"], CX-2.75, Y["rx_en"])

    arr(ax, CX, Y["d_poll"]-0.55, CX, Y["calc_tx"]+0.375, lbl="Yes", lbl_x_off=0.08)
    dn("calc_tx",  "send_resp")
    dn("send_resp","d_deinit")

    # DEINIT YES → cleanup (off to right)
    arr(ax, CX+2.5, Y["d_deinit"], 8.5, Y["d_deinit"], lbl="Yes", lbl_x_off=0.05)
    ax.text(8.55, Y["d_deinit"]-0.3, "Cleanup\nDW3000 reset", fontsize=7,
            color="#C62828", ha="left", va="top")

    # DEINIT NO → loop back to rx_en
    arr(ax, CX-2.5, Y["d_deinit"], 0.5, Y["d_deinit"], lbl="No",
        lbl_ha="right", lbl_x_off=-0.5)
    arr(ax, 0.5, Y["d_deinit"], 0.5, Y["rx_en"])
    arr(ax, 0.5, Y["rx_en"], CX-2.75, Y["rx_en"])

    save(fig, "flow_uwb_task.png")


# ══════════════════════════════════════════════════════════════════════════════
# 4. canTask FLOWCHART
# ══════════════════════════════════════════════════════════════════════════════

def draw_can_task():
    fig, ax = new_ax("canTask — FreeRTOS Flowchart", w=8.5, h=14, yl=(0, 17))
    CX = 5.0

    Y = {
        "start":   16.2,
        "init_can": 14.9,
        "wait_q":   13.6,
        "d_cmd":    12.2,
        "unlock":   10.9,
        "lock":     10.9,   # right branch same row
        "acq_spi":   9.6,
        "d_mutex":   8.3,
        "send_can":  7.0,
        "rel_spi":   5.7,
        "delay50":   4.4,
        "d_loop":    3.1,
        "end":       1.5,
    }

    term(ax, CX, Y["start"],    3.2, 0.6, "START  canTask")
    proc(ax, CX, Y["init_can"], 5.5, 0.75,
         "Init MCP2515\n(100 kbps, 8 MHz clock, SPI)", fc=CAN_F, ec=CAN_E)
    proc(ax, CX, Y["wait_q"],   5.5, 0.75,
         "xQueueReceive(canQueue, portMAX_DELAY)\nWait for CAN command", fc=IPC_F, ec=IPC_E)
    dec(ax,  CX, Y["d_cmd"],    5.5, 1.2, "Command?")

    # Command branches
    proc(ax, 2.5, Y["unlock"], 3.5, 0.75, "Select UNLOCK\n15 frames", fc=OK_F, ec=OK_E)
    proc(ax, 7.5, Y["lock"],   3.5, 0.75, "Select LOCK\n16 frames",   fc=NG_F, ec=NG_E)

    proc(ax, CX, Y["acq_spi"],  5.5, 0.75,
         "For each frame i=0..n:\nAcquire spiMutex (timeout 2000 ms)", fc=IPC_F, ec=IPC_E)
    dec(ax,  CX, Y["d_mutex"],  5.0, 1.1, "Mutex OK?")
    proc(ax, CX, Y["send_can"], 5.5, 0.75,
         "MCP2515.sendMessage(frames[i])\n(8-byte data, CAN ID)", fc=CAN_F, ec=CAN_E)
    proc(ax, CX, Y["rel_spi"],  5.5, 0.75,
         "Release spiMutex", fc=IPC_F, ec=IPC_E)
    proc(ax, CX, Y["delay50"],  5.5, 0.75,
         "vTaskDelay(50 ms)\n(inter-frame gap)", fc=CAN_F, ec=CAN_E)
    dec(ax,  CX, Y["d_loop"],   5.0, 1.1, "More frames?")
    term(ax, CX, Y["end"],      3.2, 0.6, "END  (never reached)")

    def dn(a, b): arr(ax, CX, Y[a]-0.375, CX, Y[b]+0.375)

    dn("start",   "init_can")
    dn("init_can","wait_q")
    dn("wait_q",  "d_cmd")

    # branches from decision
    arr(ax, CX-2.75, Y["d_cmd"], 2.5-1.75, Y["d_cmd"], lbl="UNLOCK", lbl_ha="right", lbl_x_off=-0.5)
    arr(ax, CX+2.75, Y["d_cmd"], 7.5+1.75, Y["d_cmd"], lbl="LOCK",   lbl_x_off=0.05)
    # converge
    arr_elbow(ax, 2.5, Y["unlock"]-0.375, CX-2.75, Y["acq_spi"])
    arr_elbow(ax, 7.5, Y["lock"]-0.375,   CX+2.75, Y["acq_spi"])

    dn("acq_spi", "d_mutex")

    # Mutex fail
    arr(ax, CX+2.5, Y["d_mutex"], 8.5, Y["d_mutex"], lbl="No", lbl_x_off=0.05)
    ax.text(8.55, Y["d_mutex"]-0.3, "Log error\nskip frame", fontsize=7,
            color="#C62828", ha="left", va="top")
    arr_elbow(ax, 8.5, Y["d_mutex"], CX+2.75, Y["delay50"])

    arr(ax, CX, Y["d_mutex"]-0.55, CX, Y["send_can"]+0.375, lbl="Yes", lbl_x_off=0.08)
    dn("send_can","rel_spi")
    dn("rel_spi", "delay50")
    dn("delay50", "d_loop")

    # More frames → back to acq_spi
    arr(ax, CX-2.5, Y["d_loop"], 0.6, Y["d_loop"], lbl="Yes", lbl_ha="right", lbl_x_off=-0.5)
    arr(ax, 0.6, Y["d_loop"], 0.6, Y["acq_spi"])
    arr(ax, 0.6, Y["acq_spi"], CX-2.75, Y["acq_spi"])

    # Done → wait_q loop
    arr(ax, CX, Y["d_loop"]-0.55, CX, Y["end"]+0.3, lbl="No", lbl_x_off=0.08)
    arr(ax, CX+2.75, Y["end"], 9.2, Y["end"])
    arr(ax, 9.2, Y["end"], 9.2, Y["wait_q"])
    arr(ax, 9.2, Y["wait_q"], CX+2.75, Y["wait_q"])
    ax.text(9.3, (Y["end"]+Y["wait_q"])/2, "Back to\nqueue wait",
            fontsize=7, color="#444", ha="left", va="center")

    save(fig, "flow_can_task.png")


# ══════════════════════════════════════════════════════════════════════════════
# 5. friendMgmtTask FLOWCHART
# ══════════════════════════════════════════════════════════════════════════════

def draw_friend_task():
    fig, ax = new_ax("friendMgmtTask — 5-Step Bundle Verification",
                     w=10, h=22, yl=(0, 26))
    CX = 5.0

    Y = {
        "start":    25.2,
        "wait_q":   23.8,
        "parse":    22.4,
        # Step 1
        "s1_lbl":   21.3,
        "ecdsa":    20.7,
        "d_ecdsa":  19.4,
        # Step 2
        "s2_lbl":   18.3,
        "d_clock":  17.7,
        "d_time":   16.4,
        # Step 3
        "s3_lbl":   15.3,
        "d_revok":  14.7,
        # Step 4
        "s4_lbl":   13.6,
        "d_cache":  13.0,
        # Step 5
        "s5_lbl":   11.6,
        "http_val": 11.0,
        "d_http":    9.7,
        "cache_save":8.4,
        # UWB + CAN
        "uwb_init":  7.1,
        "d_uwb_rdy": 5.8,
        "proximity": 4.5,
        "d_close":   3.2,
        "verified":  2.0,
        "report":    0.8,
    }

    def step_label(y, n, txt, col):
        ax.text(0.2, y, f"Step {n}", fontsize=8.5, fontweight="bold",
                color=col, va="center", ha="left", zorder=6)
        ax.text(0.2, y-0.3, txt, fontsize=7.5, color=col, va="center", ha="left", zorder=6)
        ax.plot([0.15, 0.15], [y-0.5, y+0.5], color=col, lw=3, zorder=6)

    def fail_right(y_dec, label_txt):
        """Arrow from decision → right → fail label."""
        arr(ax, CX+2.5, y_dec, 9.0, y_dec, lbl="No", lbl_x_off=0.05)
        ax.text(9.1, y_dec, label_txt, fontsize=7, color="#C62828",
                ha="left", va="center", fontweight="bold")

    term(ax, CX, Y["start"],   3.2, 0.6, "START  friendMgmtTask")
    proc(ax, CX, Y["wait_q"],  5.5, 0.75,
         "xQueueReceive(friendMgmtQueue, portMAX_DELAY)", fc=FRD_F, ec=FRD_E)
    proc(ax, CX, Y["parse"],   5.5, 0.75,
         "Parse 106-byte bundle\n(ft_parse_bundle_wire)", fc=FRD_F, ec=FRD_E)

    # ── Step 1 ────────────────────────────────────────────────────────────────
    step_label(Y["s1_lbl"], 1, "Offline ECDSA Verify", BLE_E)
    proc(ax, CX, Y["ecdsa"],   5.5, 0.75,
         "mbedTLS ECDSA-P256-SHA256\n(signed bytes[0..41], sig bytes[42..105])", fc=BLE_F, ec=BLE_E)
    dec(ax,  CX, Y["d_ecdsa"], 5.0, 1.1, "ECDSA OK?")
    fail_right(Y["d_ecdsa"], "Reject  BAD_SIG\n→ back to queue")

    # ── Step 2 ────────────────────────────────────────────────────────────────
    step_label(Y["s2_lbl"], 2, "Time validity check", FRD_E)
    dec(ax, CX, Y["d_clock"], 5.0, 1.0, "Clock synced\n(SNTP OK)?")
    # Clock not synced → use issued_at fallback
    arr(ax, CX+2.5, Y["d_clock"], 8.5, Y["d_clock"], lbl="No", lbl_x_off=0.05)
    ax.text(8.55, Y["d_clock"]-0.3, "Fallback: use\nbundle.issued_at\n(⚠ insecure)", fontsize=7,
            color="#C62828", ha="left", va="top")

    dec(ax, CX, Y["d_time"],  5.0, 1.1, "issued_at ≤ now\n≤ expires_at?")
    fail_right(Y["d_time"], "Reject  EXPIRED\n→ back to queue")

    # ── Step 3 ────────────────────────────────────────────────────────────────
    step_label(Y["s3_lbl"], 3, "Revocation check", CAN_E)
    dec(ax, CX, Y["d_revok"], 5.0, 1.0, "friend_id in\nNVS blacklist?")
    fail_right(Y["d_revok"], "Reject  REVOKED\n→ back to queue")

    # ── Step 4 ────────────────────────────────────────────────────────────────
    step_label(Y["s4_lbl"], 4, "NVS Cache lookup", REV_E)
    dec(ax, CX, Y["d_cache"], 5.0, 1.0, "Cache hit?\n(NVS sca_friends)")

    # ── Step 5 (cache miss) ───────────────────────────────────────────────────
    step_label(Y["s5_lbl"], 5, "Online validate (cache miss)", UWB_E)
    proc(ax, CX, Y["http_val"], 5.5, 0.75,
         "POST /validate-friend-key\n(HMAC-authenticated, anti-replay nonce)", fc=UWB_F, ec=UWB_E)
    dec(ax, CX, Y["d_http"],   5.0, 1.1, "HTTP 200\napproved?")
    fail_right(Y["d_http"], "Reject  SERVER_DENY\n→ back to queue")
    proc(ax, CX, Y["cache_save"],5.5, 0.75,
         "Save to NVS cache\n(namespace sca_friends)", fc=REV_F, ec=REV_E)

    # ── UWB Proximity ─────────────────────────────────────────────────────────
    ax.text(CX, Y["uwb_init"]+0.62, "── UWB Proximity Verification ──",
            ha="center", fontsize=8, color=UWB_E, fontweight="bold")
    proc(ax, CX, Y["uwb_init"], 5.5, 0.75,
         "Set friend_key as STS key\nNotify uwbTask: UWB_CMD_INIT", fc=UWB_F, ec=UWB_E)
    dec(ax, CX, Y["d_uwb_rdy"],5.0, 1.1, "UWB_READY_BIT\n(timeout 10s)?")
    fail_right(Y["d_uwb_rdy"], "Abort  TIMEOUT\n→ deinit UWB")

    proc(ax, CX, Y["proximity"],5.5, 0.75,
         "SS-TWR ranging (DW3000)\nMeasure distance to Tag", fc=UWB_F, ec=UWB_E)
    dec(ax, CX, Y["d_close"],  5.0, 1.1, "Distance <\nthreshold?")
    # Not close enough → keep ranging
    arr(ax, CX-2.5, Y["d_close"], 0.5, Y["d_close"], lbl="No",
        lbl_ha="right", lbl_x_off=-0.5)
    arr(ax, 0.5, Y["d_close"], 0.5, Y["proximity"])
    arr(ax, 0.5, Y["proximity"], CX-2.75, Y["proximity"])

    proc(ax, CX, Y["verified"], 5.5, 0.75,
         "Send VERIFIED + permissions → bleTask\nbleTask → canQueue: CAN_CMD_UNLOCK", fc=OK_F, ec=OK_E)
    proc(ax, CX, Y["report"],   5.5, 0.75,
         "POST /friend-used  (best-effort)\n→ back to queue wait", fc=FRD_F, ec=FRD_E)

    # ── Main arrows ───────────────────────────────────────────────────────────
    def dn(a, b): arr(ax, CX, Y[a]-0.375, CX, Y[b]+0.375)
    def yes_dn(a, b): arr(ax, CX, Y[a]-0.55, CX, Y[b]+0.375, lbl="Yes", lbl_x_off=0.08)

    dn("start",  "wait_q")
    dn("wait_q", "parse")
    dn("parse",  "ecdsa")
    dn("ecdsa",  "d_ecdsa")
    yes_dn("d_ecdsa", "d_clock")

    arr(ax, CX, Y["d_clock"]-0.5, CX, Y["d_time"]+0.55, lbl="Yes", lbl_x_off=0.08)
    yes_dn("d_time",  "d_revok")
    arr(ax, CX, Y["d_revok"]-0.5, CX, Y["d_cache"]+0.5, lbl="No", lbl_x_off=0.08)

    # Cache hit → skip step 5 → go to uwb_init
    arr(ax, CX+2.5, Y["d_cache"], 8.8, Y["d_cache"], lbl="Yes", lbl_x_off=0.05)
    arr_elbow(ax, 8.8, Y["d_cache"], CX+2.75, Y["uwb_init"])

    # Cache miss → step 5
    arr(ax, CX, Y["d_cache"]-0.5, CX, Y["http_val"]+0.375, lbl="No", lbl_x_off=0.08)
    dn("http_val","d_http")
    yes_dn("d_http", "cache_save")
    dn("cache_save","uwb_init")

    dn("uwb_init", "d_uwb_rdy")
    yes_dn("d_uwb_rdy","proximity")
    dn("proximity","d_close")
    arr(ax, CX, Y["d_close"]-0.55, CX, Y["verified"]+0.375, lbl="Yes", lbl_x_off=0.08)
    dn("verified","report")

    # All reject arrows loop back to wait_q (left side)
    for y_reject, label in [
        (Y["d_ecdsa"], "BAD_SIG"),
        (Y["d_time"],  "EXPIRED"),
        (Y["d_revok"], "REVOKED"),
        (Y["d_http"],  "SERVER_DENY"),
        (Y["d_uwb_rdy"],"TIMEOUT"),
    ]:
        pass   # already drawn inline as fail_right

    # report → back to wait_q
    arr(ax, CX-2.75, Y["report"], 0.3, Y["report"])
    arr(ax, 0.3, Y["report"], 0.3, Y["wait_q"])
    arr(ax, 0.3, Y["wait_q"], CX-2.75, Y["wait_q"])

    save(fig, "flow_friend_task.png")


# ══════════════════════════════════════════════════════════════════════════════
# 6. revocationSyncTask FLOWCHART
# ══════════════════════════════════════════════════════════════════════════════

def draw_revoc_task():
    fig, ax = new_ax("revocationSyncTask — FreeRTOS Flowchart", w=9, h=15, yl=(0, 18))
    CX = 5.0

    Y = {
        "start":   17.2,
        "read_nvs": 15.9,
        "d_wifi":   14.6,
        "http_get": 13.3,
        "d_http":   12.0,
        "parse_j":  10.7,
        "d_newrev":  9.4,
        "add_nvs":   8.1,
        "cleanup":   6.8,
        "update_ts": 5.5,
        "delay":     4.2,
        "end":       2.8,
    }

    term(ax, CX, Y["start"],    3.2, 0.6, "START  revocationSyncTask")
    proc(ax, CX, Y["read_nvs"], 5.5, 0.75,
         "Read last_sync from NVS\n(key rev_last_sync, default: epoch 0)", fc=REV_F, ec=REV_E)

    # Loop header
    ax.add_patch(FancyBboxPatch((1.8, Y["d_wifi"]-0.6), 6.4, 11.8,
                                boxstyle="round,pad=0.1",
                                fc="#FAFAFA", ec=REV_E, lw=2, ls="--", zorder=1))
    ax.text(CX, Y["d_wifi"]+11.35, "── Main Sync Loop (every 5 min) ──",
            ha="center", fontsize=8, color=REV_E, fontweight="bold")

    dec(ax, CX, Y["d_wifi"],   5.0, 1.1, "WiFi connected?")
    proc(ax, CX, Y["http_get"],5.5, 0.75,
         "GET /cars/{vehicle_id}/revocations\n?since={last_sync}", fc=REV_F, ec=REV_E)
    dec(ax, CX, Y["d_http"],   5.0, 1.1, "HTTP 200 OK?")
    proc(ax, CX, Y["parse_j"], 5.5, 0.75,
         "Parse JSON revocation list\n(DynamicJsonDocument 4 KB)", fc=REV_F, ec=REV_E)
    dec(ax, CX, Y["d_newrev"], 5.0, 1.1, "New revocations\nreceived?")
    proc(ax, CX, Y["add_nvs"], 5.5, 0.75,
         "For each friend_id:\nAdd to NVS blacklist (sca_revoked, max 100)", fc=NG_F, ec=NG_E)
    proc(ax, CX, Y["cleanup"], 5.5, 0.75,
         "Cleanup NVS: remove entries\nolder than max_ttl_h (720 h)", fc=REV_F, ec=REV_E)
    proc(ax, CX, Y["update_ts"],5.5, 0.75,
         "Update last_sync = now (ISO-8601)\nWrite to NVS", fc=OK_F, ec=OK_E)
    proc(ax, CX, Y["delay"],   5.5, 0.75,
         "vTaskDelay(300 000 ms)\n= 5 minutes", fc=REV_F, ec=REV_E)
    term(ax, CX, Y["end"],     3.2, 0.6, "END  (never reached)")

    def dn(a, b): arr(ax, CX, Y[a]-0.375, CX, Y[b]+0.375)

    dn("start",   "read_nvs")
    dn("read_nvs","d_wifi")

    # WiFi not connected → delay then retry
    arr(ax, CX+2.5, Y["d_wifi"], 8.5, Y["d_wifi"], lbl="No", lbl_x_off=0.05)
    arr_elbow(ax, 8.5, Y["d_wifi"], CX+2.75, Y["delay"])
    ax.text(8.55, (Y["d_wifi"]+Y["delay"])/2, "vTaskDelay\n(300 s)\nthen retry",
            fontsize=7, color="#C62828", ha="left", va="center")

    arr(ax, CX, Y["d_wifi"]-0.55, CX, Y["http_get"]+0.375, lbl="Yes", lbl_x_off=0.08)
    dn("http_get","d_http")

    # HTTP fail → skip to delay
    arr(ax, CX-2.5, Y["d_http"], 0.8, Y["d_http"], lbl="No",
        lbl_ha="right", lbl_x_off=-0.5)
    arr_elbow(ax, 0.8, Y["d_http"], CX-2.75, Y["update_ts"])
    ax.text(0.7, (Y["d_http"]+Y["update_ts"])/2, "Log error\nskip update",
            fontsize=7, color="#C62828", ha="right", va="center")

    arr(ax, CX, Y["d_http"]-0.55, CX, Y["parse_j"]+0.375, lbl="Yes", lbl_x_off=0.08)
    dn("parse_j", "d_newrev")

    # No new revocations → skip add_nvs/cleanup
    arr(ax, CX+2.5, Y["d_newrev"], 8.5, Y["d_newrev"], lbl="No", lbl_x_off=0.05)
    arr_elbow(ax, 8.5, Y["d_newrev"], CX+2.75, Y["update_ts"])

    arr(ax, CX, Y["d_newrev"]-0.55, CX, Y["add_nvs"]+0.375, lbl="Yes", lbl_x_off=0.08)
    dn("add_nvs", "cleanup")
    dn("cleanup", "update_ts")
    dn("update_ts","delay")
    dn("delay",   "end")

    # Loop back arrow
    arr(ax, CX-2.75, Y["end"], 0.4, Y["end"])
    arr(ax, 0.4, Y["end"], 0.4, Y["d_wifi"])
    arr(ax, 0.4, Y["d_wifi"], CX-2.75, Y["d_wifi"])
    ax.text(0.3, (Y["end"]+Y["d_wifi"])/2, "Loop",
            fontsize=7.5, color="#455A64", ha="right", va="center",
            fontweight="bold", rotation=90)

    save(fig, "flow_revoc_task.png")


# ══════════════════════════════════════════════════════════════════════════════
# MAIN
# ══════════════════════════════════════════════════════════════════════════════

if __name__ == "__main__":
    print("Generating SCA diagrams ...")
    draw_overview()
    draw_ble_task()
    draw_uwb_task()
    draw_can_task()
    draw_friend_task()
    draw_revoc_task()
    print(f"\nAll files saved to:  {OUT}")
