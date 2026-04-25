"""
view_keys.py — Xem pairing key của các xe trên server
Usage:
    python Server/view_keys.py                                        # Liệt kê tất cả xe (localhost)
    python Server/view_keys.py VIN123456                              # Xem key xe cụ thể (localhost)
    python Server/view_keys.py VIN123456 http://139.59.232.153:8000   # Server remote
    python Server/view_keys.py all        http://139.59.232.153:8000  # Liệt kê xe trên server remote
"""

import sys
import requests

BASE_URL = "http://127.0.0.1:8000"


def list_vehicles():
    resp = requests.get(f"{BASE_URL}/vehicles")
    resp.raise_for_status()
    data = resp.json()

    print(f"\nTong so xe: {data['total']}")
    print("-" * 60)

    if not data["vehicles"]:
        print("Chua co xe nao duoc pair.")
        return

    for v in data["vehicles"]:
        print(f"  Vehicle ID : {v['vehicle_id']}")
        print(f"  Pairing ID : {v['pairing_id']}")
        print(f"  Created At : {v['created_at']}")
        print("-" * 60)


def get_vehicle_key(vehicle_id: str):
    resp = requests.get(f"{BASE_URL}/vehicle/{vehicle_id}")
    if resp.status_code == 404:
        print(f"Khong tim thay xe: {vehicle_id}")
        return
    resp.raise_for_status()
    data = resp.json()

    key_hex = data["pairing_key_hex"]
    key_bytes = bytes.fromhex(key_hex)
    c_array = ", ".join(f"0x{b:02X}" for b in key_bytes)

    print(f"\nVehicle ID  : {data['vehicle_id']}")
    print(f"Pairing ID  : {data['pairing_id']}")
    print(f"Created At  : {data['created_at']}")
    print(f"\nPairing Key (hex)   : {key_hex}")
    print(f"Pairing Key (length): {len(key_bytes)} bytes")
    print(f"\nC array (cho tag_config.h):")
    print(f"  {{{c_array}}}")


if __name__ == "__main__":
    if len(sys.argv) >= 3:
        BASE_URL = sys.argv[2].rstrip("/")

    if len(sys.argv) == 1 or sys.argv[1] == "all":
        list_vehicles()
    else:
        get_vehicle_key(sys.argv[1])
