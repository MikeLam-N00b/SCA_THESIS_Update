"""
friend_client.py — Simulate the full Friend Sharing flow:
  1. Owner creates a share link
  2. Friend claims the key bundle
  3. Anchor validates the friend key (per-session, no NVS cache)
  4. Owner revokes the key
  5. Anchor tries again → expects 401
"""

import requests

SERVER_URL = "http://10.0.7.178:8000"
VEHICLE_ID = "1HGBH41JXMN109186"


def separator(title=""):
    print("\n" + "=" * 50)
    if title:
        print(f"  {title}")
        print("=" * 50)


# ── Step 1: Owner creates a friend share link ─────────────────────────────────
separator("STEP 1 — Owner creates friend share link")

resp = requests.post(f"{SERVER_URL}/friend-sharing/create", json={
    "vehicle_id": VEHICLE_ID,
    "friend_name": "Nguyen Van B",
    "ttl_hours": 2
})

if resp.status_code != 200:
    print(f"❌ Failed to create share: {resp.status_code} {resp.text}")
    exit(1)

share = resp.json()
print(f"✓ Share created")
print(f"  friend_id   : {share['friend_id']}")
print(f"  claim_url   : {share['claim_url']}")
print(f"  expires_at  : {share['expires_at']}")

friend_id   = share["friend_id"]
claim_token = share["claim_token"]


# ── Step 2: Friend claims the key bundle via the link ─────────────────────────
separator("STEP 2 — Friend claims key bundle")

resp = requests.get(f"{SERVER_URL}/friend-sharing/claim/{claim_token}")

if resp.status_code != 200:
    print(f"❌ Failed to claim: {resp.status_code} {resp.text}")
    exit(1)

bundle = resp.json()
print(f"✓ Key bundle received by friend")
print(f"  friend_id     : {bundle['friend_id']}")
print(f"  friend_key_hex: {bundle['friend_key_hex']}")
print(f"  friend_name   : {bundle['friend_name']}")
print(f"  expires_at    : {bundle['expires_at']}")
print(f"  owner_sig_b64 : {bundle['owner_sig_b64'][:32]}...")

owner_sig_b64 = bundle["owner_sig_b64"]


# ── Step 3: Anchor validates (simulate cache miss every session) ───────────────
separator("STEP 3 — Anchor validates friend key (session 1)")

resp = requests.post(f"{SERVER_URL}/validate-friend-key", json={
    "vehicle_id": VEHICLE_ID,
    "friend_id": friend_id,
    "owner_sig_b64": owner_sig_b64
})

if resp.status_code == 200:
    result = resp.json()
    print(f"✓ Validation OK — access granted")
    print(f"  friend_key_hex: {result['friend_key_hex']}")
    print(f"  expires_at    : {result['expires_at']}")
    print(f"  (Anchor uses this key for HMAC verify, then discards)")
elif resp.status_code == 401:
    print(f"✗ Rejected: {resp.json()['detail']}")
else:
    print(f"❌ Error: {resp.status_code} {resp.text}")


# ── Step 4: Owner lists friend keys ───────────────────────────────────────────
separator("STEP 4 — Owner lists all friend keys")

resp = requests.get(f"{SERVER_URL}/friend-sharing/list/{VEHICLE_ID}")
data = resp.json()
print(f"✓ {data['total']} friend key(s) for {VEHICLE_ID}:")
for fk in data["friend_keys"]:
    print(f"  [{fk['status'].upper():8}] {fk['friend_id']}  {fk['friend_name']}  expires {fk['expires_at']}")


# ── Step 5: Owner revokes the key ─────────────────────────────────────────────
separator("STEP 5 — Owner revokes friend key")

resp = requests.delete(f"{SERVER_URL}/friend-sharing/{friend_id}")
if resp.status_code == 200:
    print(f"✓ Key revoked: {resp.json()['friend_id']}")
else:
    print(f"❌ Revoke failed: {resp.status_code} {resp.text}")


# ── Step 6: Anchor tries again after revocation → must get 401 ────────────────
separator("STEP 6 — Anchor validates again (expect 401 revoked)")

resp = requests.post(f"{SERVER_URL}/validate-friend-key", json={
    "vehicle_id": VEHICLE_ID,
    "friend_id": friend_id,
    "owner_sig_b64": owner_sig_b64
})

if resp.status_code == 401:
    print(f"✓ Correctly rejected: {resp.json()['detail']}")
elif resp.status_code == 200:
    print(f"❌ BUG: Still valid after revocation!")
else:
    print(f"❌ Unexpected: {resp.status_code} {resp.text}")


separator("DONE")
print("  Full friend sharing flow completed successfully.")
print("=" * 50)
