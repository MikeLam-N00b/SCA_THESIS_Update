#!/usr/bin/env python3
"""
nvs_clear.py — Xóa NVS partition trên ESP32 (Anchor hoặc Tag)

Anchor: xóa pairing key (bleKey) + BLE bonding cache → fetch key mới từ server
Tag   : xóa BLE bonding cache → scan fresh, không tự reconnect Anchor cũ

Yêu cầu: pip install esptool
Dùng lệnh:
  python nvs_clear.py --port COM10              # Anchor (mặc định)
  python nvs_clear.py --port COM11 --role tag   # Tag
  python nvs_clear.py --port COM10 --port-tag COM11  # Cả hai cùng lúc
"""

import argparse
import subprocess
import sys

# Địa chỉ NVS partition mặc định của ESP32 (partition table mặc định)
NVS_OFFSET = "0x9000"
NVS_SIZE   = "0x5000"   # 20 KB


def run(cmd: list[str]) -> int:
    print(f"[RUN] {' '.join(cmd)}")
    result = subprocess.run(cmd)
    return result.returncode


def erase_nvs(port: str, baud: str, label: str) -> int:
    print(f"[INFO] [{label}] Xóa NVS partition tại offset {NVS_OFFSET}, size {NVS_SIZE} ...")
    rc = run([
        sys.executable, "-m", "esptool",
        "--port", port,
        "--baud", baud,
        "erase_region", NVS_OFFSET, NVS_SIZE,
    ])
    return rc


def main():
    parser = argparse.ArgumentParser(description="Xóa NVS trên ESP32 SCA (Anchor / Tag)")
    parser.add_argument("--port", "-p", default="COM3",
                        help="Serial port Anchor (mặc định: COM3)")
    parser.add_argument("--port-tag", default=None,
                        help="Serial port Tag (nếu muốn xóa cả Tag cùng lúc)")
    parser.add_argument("--role", choices=["anchor", "tag", "both"], default="anchor",
                        help="Thiết bị cần xóa: anchor | tag | both (mặc định: anchor)")
    parser.add_argument("--baud", "-b", default="921600",
                        help="Baud rate (mặc định: 921600)")
    args = parser.parse_args()

    # Nếu dùng --port-tag thì tự hiểu là both
    if args.port_tag and args.role == "anchor":
        args.role = "both"

    print("=" * 55)
    print("  SCA NVS Eraser")
    print("=" * 55)

    targets = []
    if args.role in ("anchor", "both"):
        targets.append((args.port, "ANCHOR",
                        "Khởi động lại Anchor → sẽ fetch key mới từ server."))
    if args.role in ("tag", "both"):
        tag_port = args.port_tag if args.port_tag else args.port
        targets.append((tag_port, "TAG",
                        "Khởi động lại Tag → scan fresh, không tự reconnect Anchor cũ."))

    for port, label, ok_msg in targets:
        print(f"\nPort  : {port}  [{label}]")

    print()
    overall_ok = True

    for port, label, ok_msg in targets:
        rc = erase_nvs(port, args.baud, label)
        if rc == 0:
            print(f"[OK]  [{label}] NVS đã xóa thành công!")
            print(f"[OK]  {ok_msg}")
        else:
            print(f"[ERR] [{label}] esptool thất bại (exit code {rc})")
            print( "      Kiểm tra lại port và driver CH340/CP210x.")
            overall_ok = False

    if not overall_ok:
        sys.exit(1)


if __name__ == "__main__":
    main()
