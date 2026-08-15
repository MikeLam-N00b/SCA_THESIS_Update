"""
Smart Car Access Server v3 - Sequence-Aligned Edition
======================================================
Aligned with 4 core sequences:
  [S1] Owner creates friend share
  [S2] Guest unlocks (anchor verifies OFFLINE)
  [S3] Anchor online validate (cache miss)
  [S4] Owner revokes + propagation

Changes from v2:
  [NEW]     Anchor authentication via pairing_key HMAC (for activity/validate)
  [NEW]     Challenge-response nonce for anti-replay on validate endpoint
  [NEW]     /pairing-bootstrap endpoint returns server_public_key for anchor cache
  [NEW]     issued_at field in bundle for clock skew detection
  [NEW]     max_key_ttl_hours in revocations response for NVS cleanup hint
  [NEW]     /health endpoint for anchor connectivity check
  [NEW]     /friend-used endpoint for anchor to report usage (uses_count tracking)
  [FIX]     claim_token now single-use (marked as claimed after first fetch)
  [FIX]     friend_id is always 16 hex chars (8 bytes) - canonical form
"""

from fastapi import FastAPI, HTTPException, Header, Request
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel, Field
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.hazmat.primitives import hashes, serialization, hmac as crypto_hmac
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
import base64
import hmac as stdhmac
import hashlib
import os
import secrets
import sqlite3
import socket
import json
import struct
import time
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature, encode_dss_signature
from datetime import datetime, timedelta, timezone
from typing import Optional
from contextlib import asynccontextmanager
from zeroconf import ServiceInfo, Zeroconf


# ============================================================
#  Constants
# ============================================================

# Permission bitmask (minimal scope for thesis)
PERM_UNLOCK = 0x01
PERM_LOCK   = 0x02
PERM_ALL    = PERM_UNLOCK | PERM_LOCK

VALID_PERMISSIONS = PERM_UNLOCK | PERM_LOCK

# Bundle format version (anchor firmware must match)
BUNDLE_VERSION = 1

# Max lifetime for any friend key (used by anchor for NVS cleanup)
MAX_KEY_TTL_HOURS = 720  # 30 days

# Nonce cache TTL for challenge-response
NONCE_TTL_SECONDS = 120

SERVER_SIGNING_KEY_PATH = "server_signing_key.pem"
_server_signing_key = None

# In-memory nonce cache (production should use Redis)
_active_nonces: dict = {}  # nonce_hex -> expiry_timestamp


def utcnow() -> datetime:
    return datetime.now(timezone.utc)


def utcnow_iso() -> str:
    """Naive ISO (no timezone) to keep DB format simple."""
    return utcnow().replace(tzinfo=None).isoformat()


def naive_now() -> datetime:
    return utcnow().replace(tzinfo=None)


# ============================================================
#  Server signing key (ECDSA SECP256R1)
# ============================================================

def get_server_signing_key():
    """Load or generate the server's ECDSA signing key (singleton)."""
    global _server_signing_key
    if _server_signing_key is None:
        if os.path.exists(SERVER_SIGNING_KEY_PATH):
            with open(SERVER_SIGNING_KEY_PATH, "rb") as f:
                _server_signing_key = serialization.load_pem_private_key(f.read(), password=None)
        else:
            _server_signing_key = ec.generate_private_key(ec.SECP256R1())
            with open(SERVER_SIGNING_KEY_PATH, "wb") as f:
                f.write(_server_signing_key.private_bytes(
                    serialization.Encoding.PEM,
                    serialization.PrivateFormat.PKCS8,
                    serialization.NoEncryption()
                ))
            print("✓ New server signing key generated")
    return _server_signing_key


def get_server_public_key_b64() -> str:
    """Return server's public key as base64 DER (for anchor to cache)."""
    pub = get_server_signing_key().public_key()
    pub_bytes = pub.public_bytes(
        encoding=serialization.Encoding.DER,
        format=serialization.PublicFormat.SubjectPublicKeyInfo,
    )
    return base64.b64encode(pub_bytes).decode()


def _build_bundle_signed_part(
    bundle_version: int,
    friend_id: bytes,       # 8 bytes binary
    vehicle_id_hash: bytes, # 8 bytes = SHA-256(VIN)[:8]
    friend_key: bytes,      # 16 bytes binary
    issued_at: int,         # epoch seconds
    expires_at: int,        # epoch seconds
    permissions: int,
) -> bytes:
    """Build the 42-byte signed part of the binary bundle (big-endian struct)."""
    return struct.pack(
        ">B8s8s16sIIB",
        bundle_version,
        friend_id,
        vehicle_id_hash,
        friend_key,
        issued_at,
        expires_at,
        permissions,
    )


def sign_friend_bundle_binary(
    friend_id: bytes,
    vehicle_id: str,
    friend_key: bytes,
    issued_at: int,
    expires_at: int,
    permissions: int,
) -> bytes:
    """Sign the bundle and return the full 106-byte bundle (signed_part[42] + raw_sig[64])."""
    vehicle_id_hash = hashlib.sha256(vehicle_id.encode()).digest()[:8]
    signed_part = _build_bundle_signed_part(
        BUNDLE_VERSION, friend_id, vehicle_id_hash, friend_key,
        issued_at, expires_at, permissions,
    )
    sig_der = get_server_signing_key().sign(signed_part, ec.ECDSA(hashes.SHA256()))
    r, s = decode_dss_signature(sig_der)
    sig_raw = r.to_bytes(32, 'big') + s.to_bytes(32, 'big')
    return signed_part + sig_raw  # 42 + 64 = 106 bytes


def verify_friend_bundle_binary(bundle_bytes: bytes) -> bool:
    """Verify the ECDSA signature on a 106-byte binary bundle."""
    if len(bundle_bytes) != 106:
        return False
    signed_part = bundle_bytes[:42]
    r = int.from_bytes(bundle_bytes[42:74], 'big')
    s = int.from_bytes(bundle_bytes[74:106], 'big')
    try:
        sig_der = encode_dss_signature(r, s)
        get_server_signing_key().public_key().verify(
            sig_der, signed_part, ec.ECDSA(hashes.SHA256())
        )
        return True
    except Exception:
        return False


# ============================================================
#  Nonce management (anti-replay for Sequence 3)
# ============================================================

def issue_nonce(vehicle_id: str) -> str:
    """Generate a random nonce tied to a vehicle, valid for NONCE_TTL_SECONDS."""
    nonce = secrets.token_hex(16)  # 32 hex chars
    expiry = time.time() + NONCE_TTL_SECONDS
    _active_nonces[f"{vehicle_id}:{nonce}"] = expiry
    _cleanup_nonces()
    return nonce


def consume_nonce(vehicle_id: str, nonce: str) -> bool:
    """Check if nonce is valid and consume it (one-time use)."""
    key = f"{vehicle_id}:{nonce}"
    expiry = _active_nonces.pop(key, None)
    if expiry is None:
        return False
    return time.time() < expiry


def _cleanup_nonces():
    """Remove expired nonces (called opportunistically)."""
    now = time.time()
    expired = [k for k, v in _active_nonces.items() if v < now]
    for k in expired:
        _active_nonces.pop(k, None)


# ============================================================
#  Anchor authentication (HMAC with pairing_key)
# ============================================================

def compute_anchor_mac(pairing_key: bytes, message: str) -> str:
    """HMAC-SHA256 signature that anchor uses to prove identity."""
    mac = stdhmac.new(pairing_key, message.encode(), hashlib.sha256).digest()
    return base64.b64encode(mac).decode()


def verify_anchor_mac(pairing_key: bytes, message: str, mac_b64: str) -> bool:
    expected = compute_anchor_mac(pairing_key, message)
    return stdhmac.compare_digest(expected, mac_b64)


def require_anchor_auth(
    vehicle_id: str,
    x_anchor_mac: Optional[str],
    x_anchor_timestamp: Optional[str],
    body_digest: str,
) -> dict:
    """
    Anchor authentication: HMAC over "{vehicle_id}|{timestamp}|{body_digest}"
    using the shared pairing_key. Timestamp must be within 5 minutes.

    Protects Sequence 3 (/validate-friend-key) and /activity from forgery.
    """
    if not x_anchor_mac or not x_anchor_timestamp:
        raise HTTPException(status_code=401, detail="Missing anchor authentication headers")

    try:
        ts = int(x_anchor_timestamp)
    except ValueError:
        raise HTTPException(status_code=400, detail="Invalid timestamp")

    if abs(time.time() - ts) > 300:  # 5 min window
        raise HTTPException(status_code=401, detail="Anchor timestamp out of range")

    vehicle = get_vehicle_pairing(vehicle_id)
    if not vehicle:
        raise HTTPException(status_code=404, detail="Vehicle not found")

    message = f"{vehicle_id}|{x_anchor_timestamp}|{body_digest}"
    if not verify_anchor_mac(vehicle["pairing_key"], message, x_anchor_mac):
        raise HTTPException(status_code=403, detail="Invalid anchor MAC")

    return vehicle


def body_sha256_b64(body: str) -> str:
    """Hash request body for MAC binding (prevents request tampering)."""
    return base64.b64encode(hashlib.sha256(body.encode()).digest()).decode()


def get_local_ip() -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    finally:
        s.close()


# ============================================================
#  App lifespan + mDNS
# ============================================================

@asynccontextmanager
async def lifespan(app: FastAPI):
    import asyncio

    init_db()
    get_server_signing_key()

    print("\n" + "=" * 60)
    print("Smart Car Access Server v3 (Sequence-Aligned)")
    print("=" * 60)
    print("\n[Sequence 1] Owner create friend share:")
    print("  POST   /owner-pairing")
    print("  POST   /friend-sharing/create              [X-Owner-Key]")
    print("  GET    /friend-sharing/list/{vid}          [X-Owner-Key]")
    print("  DELETE /friend-sharing/{friend_id}         [X-Owner-Key]")
    print("\n[Sequence 2] Guest unlock (offline path):")
    print("  GET    /friend-sharing/claim/{token}       (Guest one-time)")
    print("  GET    /pairing-bootstrap                  (Anchor once)")
    print("\n[Sequence 3] Anchor online validate:")
    print("  GET    /validate-challenge/{vid}           (get nonce)")
    print("  POST   /validate-friend-key                [X-Anchor-MAC]")
    print("  POST   /friend-used                        [X-Anchor-MAC]")
    print("\n[Sequence 4] Revocation propagation:")
    print("  GET    /cars/{vid}/revocations             (anchor poll)")
    print("\n[Activity & Utility]")
    print("  POST   /cars/{vid}/activity                [X-Anchor-MAC]")
    print("  GET    /cars/{vid}/activity                [X-Owner-Key]")
    print("  GET    /health")
    print("  GET    /server-public-key")
    print("=" * 60)

    local_ip = get_local_ip()
    zeroconf = Zeroconf()
    mdns_info = ServiceInfo(
        "_http._tcp.local.",
        "smartcar._http._tcp.local.",
        addresses=[socket.inet_aton(local_ip)],
        port=8000,
        properties={"service": "smartcar", "version": "3"},
    )
    loop = asyncio.get_event_loop()
    try:
        await loop.run_in_executor(None, zeroconf.register_service, mdns_info)
        print(f"mDNS registered: smartcar._http._tcp.local -> {local_ip}:8000")
    except Exception as e:
        print(f"mDNS registration failed (non-fatal): {e}")
    print("=" * 60 + "\n")

    yield

    try:
        await loop.run_in_executor(None, zeroconf.unregister_service, mdns_info)
    except Exception:
        pass
    await loop.run_in_executor(None, zeroconf.close)


app = FastAPI(title="Smart Car Access Server v3", lifespan=lifespan)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=False,
    allow_methods=["*"],
    allow_headers=["*"],
)


# ============================================================
#  Database
# ============================================================

def init_db():
    conn = sqlite3.connect('car_access.db')
    cursor = conn.cursor()

    cursor.execute('''
        CREATE TABLE IF NOT EXISTS vehicles (
            vehicle_id    TEXT PRIMARY KEY,
            pairing_id    TEXT NOT NULL,
            pairing_key   TEXT NOT NULL,
            owner_api_key TEXT,
            created_at    TEXT NOT NULL
        )
    ''')

    # Ensure owner_api_key column exists (migration safety)
    cursor.execute("PRAGMA table_info(vehicles)")
    cols = [row[1] for row in cursor.fetchall()]
    if "owner_api_key" not in cols:
        cursor.execute("ALTER TABLE vehicles ADD COLUMN owner_api_key TEXT")

    cursor.execute('''
        CREATE TABLE IF NOT EXISTS friend_keys (
            friend_id       TEXT PRIMARY KEY,
            vehicle_id      TEXT NOT NULL,
            friend_key      TEXT NOT NULL,
            friend_name     TEXT,
            permissions     INTEGER NOT NULL DEFAULT 3,
            issued_at       TEXT NOT NULL,
            expires_at      TEXT NOT NULL,
            issuer_sig      TEXT NOT NULL,
            is_revoked      INTEGER DEFAULT 0,
            revoked_at      TEXT,
            uses_count      INTEGER DEFAULT 0,
            last_used_at    TEXT,
            claim_token     TEXT UNIQUE NOT NULL,
            claimed         INTEGER DEFAULT 0,
            claimed_at      TEXT,
            created_at      TEXT NOT NULL
        )
    ''')

    # Migrations for older DBs
    cursor.execute("PRAGMA table_info(friend_keys)")
    cols = [row[1] for row in cursor.fetchall()]
    if "permissions" not in cols:
        cursor.execute("ALTER TABLE friend_keys ADD COLUMN permissions INTEGER NOT NULL DEFAULT 3")
    if "issuer_sig" not in cols and "owner_sig" in cols:
        cursor.execute("ALTER TABLE friend_keys ADD COLUMN issuer_sig TEXT")
        cursor.execute("UPDATE friend_keys SET issuer_sig = owner_sig WHERE issuer_sig IS NULL")
    if "revoked_at" not in cols:
        cursor.execute("ALTER TABLE friend_keys ADD COLUMN revoked_at TEXT")
    if "issued_at" not in cols:
        cursor.execute("ALTER TABLE friend_keys ADD COLUMN issued_at TEXT")
    if "uses_count" not in cols:
        cursor.execute("ALTER TABLE friend_keys ADD COLUMN uses_count INTEGER DEFAULT 0")
    if "last_used_at" not in cols:
        cursor.execute("ALTER TABLE friend_keys ADD COLUMN last_used_at TEXT")
    if "claimed" not in cols:
        cursor.execute("ALTER TABLE friend_keys ADD COLUMN claimed INTEGER DEFAULT 0")
    if "claimed_at" not in cols:
        cursor.execute("ALTER TABLE friend_keys ADD COLUMN claimed_at TEXT")
    if "bundle_b64" not in cols:
        cursor.execute("ALTER TABLE friend_keys ADD COLUMN bundle_b64 TEXT")

    cursor.execute('''
        CREATE TABLE IF NOT EXISTS access_events (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            vehicle_id  TEXT NOT NULL,
            friend_id   TEXT,
            event_type  TEXT NOT NULL,
            result      TEXT NOT NULL,
            timestamp   TEXT NOT NULL,
            details     TEXT
        )
    ''')
    cursor.execute('CREATE INDEX IF NOT EXISTS idx_events_vehicle ON access_events(vehicle_id)')
    cursor.execute('CREATE INDEX IF NOT EXISTS idx_events_time ON access_events(timestamp)')
    cursor.execute('CREATE INDEX IF NOT EXISTS idx_friends_vehicle ON friend_keys(vehicle_id)')
    cursor.execute('CREATE INDEX IF NOT EXISTS idx_friends_revoked ON friend_keys(vehicle_id, is_revoked, revoked_at)')

    conn.commit()
    conn.close()
    print("✓ Database initialized")


# ============================================================
#  Vehicle / pairing helpers
# ============================================================

def save_vehicle_pairing(vehicle_id: str, pairing_id: str, pairing_key: bytes) -> str:
    conn = sqlite3.connect('car_access.db')
    cursor = conn.cursor()
    owner_api_key = secrets.token_urlsafe(32)
    cursor.execute('''
        INSERT OR REPLACE INTO vehicles
            (vehicle_id, pairing_id, pairing_key, owner_api_key, created_at)
        VALUES (?, ?, ?, ?, ?)
    ''', (vehicle_id, pairing_id,
          base64.b64encode(pairing_key).decode(),
          owner_api_key, utcnow_iso()))
    conn.commit()
    conn.close()
    return owner_api_key


def get_vehicle_pairing(vehicle_id: str) -> Optional[dict]:
    conn = sqlite3.connect('car_access.db')
    cursor = conn.cursor()
    cursor.execute('''
        SELECT pairing_id, pairing_key, created_at, owner_api_key
        FROM vehicles WHERE vehicle_id = ?
    ''', (vehicle_id,))
    result = cursor.fetchone()
    conn.close()
    if result:
        return {
            "pairing_id": result[0],
            "pairing_key": base64.b64decode(result[1]),
            "created_at": result[2],
            "owner_api_key": result[3],
        }
    return None


# ============================================================
#  Owner auth dependency
# ============================================================

def verify_owner_auth(vehicle_id: str, x_owner_key: Optional[str]) -> dict:
    vehicle = get_vehicle_pairing(vehicle_id)
    if not vehicle:
        raise HTTPException(status_code=404, detail="Vehicle not found")
    if not x_owner_key or not stdhmac.compare_digest(
        x_owner_key, vehicle.get("owner_api_key") or ""
    ):
        raise HTTPException(status_code=401, detail="Invalid or missing X-Owner-Key")
    return vehicle


# ============================================================
#  Friend key DB helpers
# ============================================================

def db_create_friend_key(
    vehicle_id: str, friend_name: str, ttl_hours: int, permissions: int,
) -> dict:
    friend_id_bytes = secrets.token_bytes(8)
    friend_id = friend_id_bytes.hex()              # 16 hex chars stored in DB
    friend_key = secrets.token_bytes(16)
    issued_at_epoch = int(time.time())
    expires_at_epoch = issued_at_epoch + ttl_hours * 3600
    issued_at = datetime.utcfromtimestamp(issued_at_epoch).isoformat()
    expires_at = datetime.utcfromtimestamp(expires_at_epoch).isoformat()
    claim_token = secrets.token_urlsafe(24)

    bundle_bytes = sign_friend_bundle_binary(
        friend_id_bytes, vehicle_id, friend_key,
        issued_at_epoch, expires_at_epoch, permissions,
    )
    bundle_b64 = base64.b64encode(bundle_bytes).decode()

    conn = sqlite3.connect('car_access.db')
    cursor = conn.cursor()
    cursor.execute('''
        INSERT INTO friend_keys
            (friend_id, vehicle_id, friend_key, friend_name, permissions,
             issued_at, expires_at, issuer_sig, is_revoked,
             claim_token, claimed, created_at, bundle_b64)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, 0, ?, 0, ?, ?)
    ''', (friend_id, vehicle_id,
          base64.b64encode(friend_key).decode(), friend_name, permissions,
          issued_at, expires_at, bundle_b64,
          claim_token, utcnow_iso(), bundle_b64))
    conn.commit()
    conn.close()

    return {
        "friend_id": friend_id,
        "claim_token": claim_token,
        "issued_at": issued_at,
        "expires_at": expires_at,
        "permissions": permissions,
    }


def db_get_friend_by_token(claim_token: str) -> Optional[dict]:
    conn = sqlite3.connect('car_access.db')
    cursor = conn.cursor()
    cursor.execute('''
        SELECT friend_id, vehicle_id, friend_key, friend_name, permissions,
               issued_at, expires_at, issuer_sig, is_revoked, claimed, created_at, bundle_b64
        FROM friend_keys WHERE claim_token = ?
    ''', (claim_token,))
    r = cursor.fetchone()
    conn.close()
    if not r:
        return None
    return {
        "friend_id": r[0], "vehicle_id": r[1],
        "friend_key": base64.b64decode(r[2]), "friend_name": r[3],
        "permissions": r[4], "issued_at": r[5], "expires_at": r[6],
        "issuer_sig": r[7], "is_revoked": r[8], "claimed": r[9],
        "created_at": r[10], "bundle_b64": r[11],
    }


def db_get_friend_by_id(friend_id: str) -> Optional[dict]:
    conn = sqlite3.connect('car_access.db')
    cursor = conn.cursor()
    cursor.execute('''
        SELECT friend_id, vehicle_id, friend_key, friend_name, permissions,
               issued_at, expires_at, issuer_sig, is_revoked, uses_count, created_at, bundle_b64
        FROM friend_keys WHERE friend_id = ?
    ''', (friend_id,))
    r = cursor.fetchone()
    conn.close()
    if not r:
        return None
    return {
        "friend_id": r[0], "vehicle_id": r[1],
        "friend_key": base64.b64decode(r[2]), "friend_name": r[3],
        "permissions": r[4], "issued_at": r[5], "expires_at": r[6],
        "issuer_sig": r[7], "is_revoked": r[8], "uses_count": r[9],
        "created_at": r[10], "bundle_b64": r[11],
    }


def db_mark_claimed(claim_token: str):
    conn = sqlite3.connect('car_access.db')
    cursor = conn.cursor()
    cursor.execute('''
        UPDATE friend_keys SET claimed = 1, claimed_at = ?
        WHERE claim_token = ? AND claimed = 0
    ''', (utcnow_iso(), claim_token))
    conn.commit()
    conn.close()


def db_increment_usage(friend_id: str):
    conn = sqlite3.connect('car_access.db')
    cursor = conn.cursor()
    cursor.execute('''
        UPDATE friend_keys
        SET uses_count = uses_count + 1, last_used_at = ?
        WHERE friend_id = ?
    ''', (utcnow_iso(), friend_id))
    conn.commit()
    conn.close()


# ============================================================
#  Activity log
# ============================================================

def log_event(vehicle_id: str, friend_id: Optional[str],
              event_type: str, result: str, details: Optional[str] = None):
    conn = sqlite3.connect('car_access.db')
    cursor = conn.cursor()
    cursor.execute('''
        INSERT INTO access_events
            (vehicle_id, friend_id, event_type, result, timestamp, details)
        VALUES (?, ?, ?, ?, ?, ?)
    ''', (vehicle_id, friend_id, event_type, result, utcnow_iso(), details))
    conn.commit()
    conn.close()


# ============================================================
#  Pydantic models
# ============================================================

class PairingRequest(BaseModel):
    vehicle_id: str
    vehicle_public_key_b64: str


class PairingResponse(BaseModel):
    pairing_id: str
    server_public_key_b64: str
    encrypted_pairing_key_b64: str
    nonce_b64: str
    owner_api_key: str
    # [NEW] Anchor signing public key for caching (used in S2 offline verify)
    server_signing_public_key_b64: str
    bundle_version: int
    max_key_ttl_hours: int


class SecureCheckRequest(BaseModel):
    vehicle_id: str
    client_public_key_b64: str


class SecureCheckResponse(BaseModel):
    server_public_key_b64: str
    encrypted_data_b64: str
    nonce_b64: str


class FriendShareCreateRequest(BaseModel):
    vehicle_id: str
    friend_name: str = "Friend"
    ttl_hours: int = Field(default=24, ge=1, le=MAX_KEY_TTL_HOURS)
    permissions: int = Field(default=PERM_ALL, ge=1, le=PERM_ALL)


class FriendShareCreateResponse(BaseModel):
    friend_id: str
    claim_token: str
    claim_url: str
    issued_at: str
    expires_at: str
    permissions: int


class FriendBundleResponse(BaseModel):
    """106-byte binary bundle returned to Guest on claim. Opaque to client."""
    bundle_b64: str
    vehicle_id: str
    friend_name: str


class FriendKeyBundle(BaseModel):
    """The complete bundle given to Guest. Guest stores & presents to anchor."""
    bundle_version: int
    friend_id: str
    vehicle_id: str
    friend_key_hex: str
    friend_name: str
    permissions: int
    issued_at: str
    expires_at: str
    issuer_sig_b64: str


class ChallengeResponse(BaseModel):
    nonce: str
    expires_in: int


class ValidateFriendKeyRequest(BaseModel):
    vehicle_id: str
    nonce: str  # from /validate-challenge, anti-replay
    bundle_b64: str  # full 106-byte binary bundle, base64-encoded


class ValidateFriendKeyResponse(BaseModel):
    valid: bool
    friend_key_hex: str
    permissions: int
    issued_at: str
    expires_at: str


class FriendUsedRequest(BaseModel):
    vehicle_id: str
    friend_id: str
    used_at: str
    event_type: str  # UNLOCK, LOCK


class ActivityEventRequest(BaseModel):
    friend_id: Optional[str] = None
    event_type: str
    result: str
    details: Optional[str] = None


# ============================================================
#  Utility: health + server public key
# ============================================================

@app.get("/health")
def health():
    """Anchor checks connectivity here before attempting sync."""
    return {"status": "ok", "server_time": utcnow_iso(), "bundle_version": BUNDLE_VERSION}


@app.get("/server-public-key")
def server_public_key():
    """
    Public ECDSA key for verifying friend bundles offline.
    Anchor caches this in NVS during pairing and uses for S2 offline verify.
    """
    return {
        "server_public_key_b64": get_server_public_key_b64(),
        "bundle_version": BUNDLE_VERSION,
    }


@app.get("/pairing-bootstrap")
def pairing_bootstrap():
    """
    Alternative bootstrap endpoint for anchor. Returns everything needed
    for the anchor to operate offline: public key + format version + TTL hint.
    Call once during provisioning.
    """
    return {
        "server_signing_public_key_b64": get_server_public_key_b64(),
        "bundle_version": BUNDLE_VERSION,
        "max_key_ttl_hours": MAX_KEY_TTL_HOURS,
        "server_time": utcnow_iso(),
    }


# ============================================================
#  Sequence 1 - Pairing & Owner endpoints
# ============================================================

@app.post("/secure-check-pairing", response_model=SecureCheckResponse)
def secure_check_pairing(req: SecureCheckRequest):
    try:
        client_pub_bytes = base64.b64decode(req.client_public_key_b64)
        client_public_key = serialization.load_der_public_key(client_pub_bytes)
    except Exception:
        raise HTTPException(status_code=400, detail="Invalid client public key")

    server_private_key = ec.generate_private_key(ec.SECP256R1())
    server_public_key_obj = server_private_key.public_key()
    shared_secret = server_private_key.exchange(ec.ECDH(), client_public_key)

    kek = HKDF(
        algorithm=hashes.SHA256(),
        length=16, salt=None, info=b"secure-check-kek",
    ).derive(shared_secret)

    vehicle_data = get_vehicle_pairing(req.vehicle_id)
    if vehicle_data:
        response_data = {
            "paired": True, "vehicle_id": req.vehicle_id,
            "pairing_id": vehicle_data["pairing_id"],
            "paired_at": vehicle_data["created_at"],
            "pairing_key": vehicle_data["pairing_key"].hex(),
        }
    else:
        response_data = {
            "paired": False, "vehicle_id": req.vehicle_id,
            "message": "Vehicle not paired",
        }

    aesgcm = AESGCM(kek)
    nonce = os.urandom(12)
    encrypted_data = aesgcm.encrypt(nonce, json.dumps(response_data).encode(), None)

    server_pub_bytes = server_public_key_obj.public_bytes(
        encoding=serialization.Encoding.DER,
        format=serialization.PublicFormat.SubjectPublicKeyInfo,
    )

    return SecureCheckResponse(
        server_public_key_b64=base64.b64encode(server_pub_bytes).decode(),
        encrypted_data_b64=base64.b64encode(encrypted_data).decode(),
        nonce_b64=base64.b64encode(nonce).decode(),
    )


@app.get("/check-pairing/{vehicle_id}")
def check_pairing_status(vehicle_id: str):
    vehicle_data = get_vehicle_pairing(vehicle_id)
    if vehicle_data:
        return {
            "paired": True, "vehicle_id": vehicle_id,
            "pairing_id": vehicle_data["pairing_id"],
            "paired_at": vehicle_data["created_at"],
        }
    return {"paired": False, "vehicle_id": vehicle_id}


@app.post("/owner-pairing", response_model=PairingResponse)
def owner_pairing(req: PairingRequest):
    """
    Sequence 1 initial step:
    - ECDH handshake with vehicle (anchor)
    - Returns encrypted pairing_key AND server_signing_public_key
    - Anchor caches both in NVS for offline operation
    """
    try:
        vehicle_pub_bytes = base64.b64decode(req.vehicle_public_key_b64)
        vehicle_public_key = serialization.load_der_public_key(vehicle_pub_bytes)
    except Exception:
        raise HTTPException(status_code=400, detail="Invalid public key")

    server_private_key = ec.generate_private_key(ec.SECP256R1())
    server_public_key_obj = server_private_key.public_key()
    shared_secret = server_private_key.exchange(ec.ECDH(), vehicle_public_key)

    kek = HKDF(
        algorithm=hashes.SHA256(),
        length=16, salt=None, info=b"owner-pairing-kek",
    ).derive(shared_secret)

    pairing_key = os.urandom(16)
    pairing_id = secrets.token_hex(8)

    aesgcm = AESGCM(kek)
    nonce = os.urandom(12)
    encrypted_pairing_key = aesgcm.encrypt(nonce, pairing_key, None)

    server_pub_bytes = server_public_key_obj.public_bytes(
        encoding=serialization.Encoding.DER,
        format=serialization.PublicFormat.SubjectPublicKeyInfo,
    )

    owner_api_key = save_vehicle_pairing(req.vehicle_id, pairing_id, pairing_key)

    print(f"\n{'=' * 60}")
    print(f"✓ Vehicle Paired: {req.vehicle_id}")
    print(f"  Pairing ID:    {pairing_id}")
    print(f"  Pairing Key:   {pairing_key.hex()}")
    print(f"  Owner API Key: {owner_api_key}   <-- give to Owner app ONCE")
    print(f"{'=' * 60}\n")

    log_event(req.vehicle_id, None, "VEHICLE_PAIRED", "OK",
              f"pairing_id={pairing_id}")

    return PairingResponse(
        pairing_id=pairing_id,
        server_public_key_b64=base64.b64encode(server_pub_bytes).decode(),
        encrypted_pairing_key_b64=base64.b64encode(encrypted_pairing_key).decode(),
        nonce_b64=base64.b64encode(nonce).decode(),
        owner_api_key=owner_api_key,
        server_signing_public_key_b64=get_server_public_key_b64(),
        bundle_version=BUNDLE_VERSION,
        max_key_ttl_hours=MAX_KEY_TTL_HOURS,
    )


@app.get("/vehicle/{vehicle_id}")
def get_vehicle_info(vehicle_id: str):
    v = get_vehicle_pairing(vehicle_id)
    if not v:
        raise HTTPException(status_code=404, detail="Vehicle not found")
    return {
        "vehicle_id": vehicle_id,
        "pairing_id": v["pairing_id"],
        "created_at": v["created_at"],
    }


@app.get("/vehicles")
def list_vehicles():
    conn = sqlite3.connect('car_access.db')
    cursor = conn.cursor()
    cursor.execute('SELECT vehicle_id, pairing_id, created_at FROM vehicles')
    rows = cursor.fetchall()
    conn.close()
    return {
        "total": len(rows),
        "vehicles": [{"vehicle_id": r[0], "pairing_id": r[1], "created_at": r[2]}
                     for r in rows],
    }


@app.delete("/vehicle/{vehicle_id}")
def delete_vehicle(vehicle_id: str, x_owner_key: Optional[str] = Header(default=None)):
    """Protected: only owner can unpair."""
    verify_owner_auth(vehicle_id, x_owner_key)
    conn = sqlite3.connect('car_access.db')
    cursor = conn.cursor()
    cursor.execute('DELETE FROM vehicles WHERE vehicle_id = ?', (vehicle_id,))
    cursor.execute('DELETE FROM friend_keys WHERE vehicle_id = ?', (vehicle_id,))
    conn.commit()
    conn.close()
    return {"message": "Vehicle deleted", "vehicle_id": vehicle_id}


# ============================================================
#  Sequence 1 continued: friend share creation
# ============================================================

@app.post("/friend-sharing/create", response_model=FriendShareCreateResponse)
def create_friend_share(
    req: FriendShareCreateRequest,
    x_owner_key: Optional[str] = Header(default=None),
):
    verify_owner_auth(req.vehicle_id, x_owner_key)

    if req.permissions & ~VALID_PERMISSIONS:
        raise HTTPException(status_code=400, detail="Unknown permission bits")
    if req.permissions == 0:
        raise HTTPException(status_code=400, detail="At least one permission required")

    result = db_create_friend_key(
        req.vehicle_id, req.friend_name, req.ttl_hours, req.permissions,
    )

    log_event(req.vehicle_id, result["friend_id"], "FRIEND_CREATED", "OK",
              f"perms=0x{req.permissions:02X} ttl={req.ttl_hours}h name={req.friend_name}")

    print(f"✓ Friend share created: {result['friend_id']} "
          f"perms=0x{req.permissions:02X} expires={result['expires_at']}")

    local_ip = get_local_ip()
    return FriendShareCreateResponse(
        friend_id=result["friend_id"],
        claim_token=result["claim_token"],
        claim_url=f"http://{local_ip}:8000/friend-sharing/claim/{result['claim_token']}",
        issued_at=result["issued_at"],
        expires_at=result["expires_at"],
        permissions=result["permissions"],
    )


@app.delete("/friend-sharing/{friend_id}")
def revoke_friend_key(
    friend_id: str,
    x_owner_key: Optional[str] = Header(default=None),
):
    """Sequence 4: Owner revokes a friend key (soft delete for propagation)."""
    data = db_get_friend_by_id(friend_id)
    if not data:
        raise HTTPException(status_code=404, detail="Friend key not found")

    verify_owner_auth(data["vehicle_id"], x_owner_key)

    conn = sqlite3.connect('car_access.db')
    cursor = conn.cursor()
    cursor.execute(
        'UPDATE friend_keys SET is_revoked = 1, revoked_at = ? '
        'WHERE friend_id = ? AND is_revoked = 0',
        (utcnow_iso(), friend_id),
    )
    changed = cursor.rowcount
    conn.commit()
    conn.close()

    if changed == 0:
        return {"message": "Already revoked", "friend_id": friend_id}

    log_event(data["vehicle_id"], friend_id, "FRIEND_REVOKED", "OK")
    print(f"✓ Revoked: {friend_id}")
    return {"message": "Friend key revoked", "friend_id": friend_id}


@app.get("/friend-sharing/list/{vehicle_id}")
def list_friend_keys(
    vehicle_id: str,
    x_owner_key: Optional[str] = Header(default=None),
):
    verify_owner_auth(vehicle_id, x_owner_key)

    conn = sqlite3.connect('car_access.db')
    cursor = conn.cursor()
    cursor.execute('''
        SELECT friend_id, friend_name, permissions,
               issued_at, expires_at, is_revoked, revoked_at,
               uses_count, last_used_at, claimed, created_at
        FROM friend_keys WHERE vehicle_id = ?
        ORDER BY created_at DESC
    ''', (vehicle_id,))
    rows = cursor.fetchall()
    conn.close()

    now = naive_now()

    def status(is_revoked, expires_at_str):
        if is_revoked:
            return "revoked"
        if now > datetime.fromisoformat(expires_at_str):
            return "expired"
        return "active"

    return {
        "vehicle_id": vehicle_id,
        "total": len(rows),
        "friend_keys": [
            {
                "friend_id": r[0], "friend_name": r[1], "permissions": r[2],
                "issued_at": r[3], "expires_at": r[4],
                "is_revoked": bool(r[5]), "revoked_at": r[6],
                "uses_count": r[7], "last_used_at": r[8],
                "claimed": bool(r[9]), "created_at": r[10],
                "status": status(r[5], r[4]),
            }
            for r in rows
        ],
    }


# ============================================================
#  Sequence 2: Guest claims bundle (one-time)
# ============================================================

@app.get("/friend-sharing/claim/{claim_token}", response_model=FriendBundleResponse)
def claim_friend_key(claim_token: str):
    """
    Guest one-time claim of the key bundle (106-byte binary, base64-encoded).
    After first fetch, claim_token is marked 'claimed' and subsequent fetches
    return 410 Gone. This prevents token sharing/leakage.
    """
    data = db_get_friend_by_token(claim_token)
    if not data:
        raise HTTPException(status_code=404, detail="Invalid claim token")
    if data["claimed"]:
        raise HTTPException(status_code=410, detail="Claim token already used")
    if data["is_revoked"]:
        raise HTTPException(status_code=410, detail="This share has been revoked")
    if naive_now() > datetime.fromisoformat(data["expires_at"]):
        raise HTTPException(status_code=410, detail="This share has expired")
    if not data.get("bundle_b64"):
        raise HTTPException(status_code=410, detail="Bundle not available — re-create share")

    # Mark as claimed (single-use)
    db_mark_claimed(claim_token)

    log_event(data["vehicle_id"], data["friend_id"], "FRIEND_CLAIMED", "OK")
    print(f"✓ Claimed: {data['friend_id']}")

    return FriendBundleResponse(
        bundle_b64=data["bundle_b64"],
        vehicle_id=data["vehicle_id"],
        friend_name=data["friend_name"] or "Friend",
    )


# ============================================================
#  Sequence 3: Anchor online validate (cache miss)
# ============================================================

@app.get("/validate-challenge/{vehicle_id}", response_model=ChallengeResponse)
def get_validate_challenge(vehicle_id: str):
    """
    Anchor calls this BEFORE /validate-friend-key to get a nonce.
    Prevents replay of /validate-friend-key requests.
    """
    if not get_vehicle_pairing(vehicle_id):
        raise HTTPException(status_code=404, detail="Vehicle not found")
    nonce = issue_nonce(vehicle_id)
    return ChallengeResponse(nonce=nonce, expires_in=NONCE_TTL_SECONDS)


@app.post("/validate-friend-key", response_model=ValidateFriendKeyResponse)
async def validate_friend_key(
    request: Request,
    x_anchor_mac: Optional[str] = Header(default=None),
    x_anchor_timestamp: Optional[str] = Header(default=None),
):
    """
    Sequence 3: Anchor validates a friend key it hasn't cached yet.

    Security:
      - HMAC-authenticated with pairing_key (anchor identity)
      - Nonce from /validate-challenge prevents replay
      - issuer_sig verified (ensures bundle integrity)
    """
    # Read raw body for MAC verification (before Pydantic parsing)
    raw_body = await request.body()
    body_str = raw_body.decode()

    try:
        parsed = json.loads(body_str)
        req = ValidateFriendKeyRequest(**parsed)
    except Exception as e:
        raise HTTPException(status_code=400, detail=f"Invalid request body: {e}")

    # Anchor authentication on raw body bytes
    body_digest = body_sha256_b64(body_str)
    require_anchor_auth(req.vehicle_id, x_anchor_mac, x_anchor_timestamp, body_digest)

    # Decode and validate bundle size
    try:
        bundle_bytes = base64.b64decode(req.bundle_b64)
    except Exception:
        raise HTTPException(status_code=400, detail="Invalid bundle_b64 encoding")
    if len(bundle_bytes) != 106:
        raise HTTPException(status_code=400, detail="Invalid bundle size (expected 106 bytes)")

    # Extract friend_id from bundle bytes[1..8]
    friend_id_bytes = bundle_bytes[1:9]
    friend_id_hex = friend_id_bytes.hex()

    # Anti-replay via nonce
    if not consume_nonce(req.vehicle_id, req.nonce):
        log_event(req.vehicle_id, friend_id_hex, "VALIDATE", "BAD_NONCE")
        raise HTTPException(status_code=401, detail="Invalid or expired nonce")

    data = db_get_friend_by_id(friend_id_hex)
    if not data:
        log_event(req.vehicle_id, friend_id_hex, "VALIDATE", "NOT_FOUND")
        raise HTTPException(status_code=404, detail="Friend key not found")

    if data["vehicle_id"] != req.vehicle_id:
        log_event(req.vehicle_id, friend_id_hex, "VALIDATE", "WRONG_VEHICLE")
        raise HTTPException(status_code=403, detail="Friend key does not belong to this vehicle")

    if data["is_revoked"]:
        log_event(req.vehicle_id, friend_id_hex, "VALIDATE", "REVOKED")
        raise HTTPException(status_code=401, detail="key revoked")

    if naive_now() > datetime.fromisoformat(data["expires_at"]):
        log_event(req.vehicle_id, friend_id_hex, "VALIDATE", "EXPIRED")
        raise HTTPException(status_code=401, detail="key expired")

    if not verify_friend_bundle_binary(bundle_bytes):
        log_event(req.vehicle_id, friend_id_hex, "VALIDATE", "BAD_SIG")
        raise HTTPException(status_code=401, detail="Invalid bundle signature")

    log_event(req.vehicle_id, friend_id_hex, "VALIDATE", "OK")
    print(f"✓ Validated: {friend_id_hex} perms=0x{data['permissions']:02X}")

    return ValidateFriendKeyResponse(
        valid=True,
        friend_key_hex=data["friend_key"].hex(),
        permissions=data["permissions"],
        issued_at=data["issued_at"],
        expires_at=data["expires_at"],
    )


@app.post("/friend-used")
async def friend_used(
    request: Request,
    x_anchor_mac: Optional[str] = Header(default=None),
    x_anchor_timestamp: Optional[str] = Header(default=None),
):
    """
    Anchor reports that a friend key was successfully used (unlock/lock).
    Updates uses_count and last_used_at. Opportunistic — anchor may batch
    these reports when connectivity is restored.
    """
    raw_body = await request.body()
    body_str = raw_body.decode()
    try:
        parsed = json.loads(body_str)
        req = FriendUsedRequest(**parsed)
    except Exception as e:
        raise HTTPException(status_code=400, detail=f"Invalid request body: {e}")

    body_digest = body_sha256_b64(body_str)
    require_anchor_auth(req.vehicle_id, x_anchor_mac, x_anchor_timestamp, body_digest)

    data = db_get_friend_by_id(req.friend_id)
    if not data or data["vehicle_id"] != req.vehicle_id:
        raise HTTPException(status_code=404, detail="Friend not found for this vehicle")

    db_increment_usage(req.friend_id)
    log_event(req.vehicle_id, req.friend_id, req.event_type, "OK",
              f"anchor_ts={req.used_at}")
    return {"message": "usage recorded"}


# ============================================================
#  Sequence 4: Revocation list polling
# ============================================================

@app.get("/cars/{vehicle_id}/revocations")
def get_revocations(vehicle_id: str, since: Optional[str] = None):
    """
    Anchor polls this periodically (default 5 min).
    Returns revoked friend_ids after `since` timestamp for delta sync.

    Response includes max_key_ttl_hours so anchor can safely drop
    revocation entries older than that (friend would be expired anyway).
    """
    if not get_vehicle_pairing(vehicle_id):
        raise HTTPException(status_code=404, detail="Vehicle not found")

    conn = sqlite3.connect('car_access.db')
    cursor = conn.cursor()
    if since:
        cursor.execute('''
            SELECT friend_id, revoked_at, expires_at FROM friend_keys
            WHERE vehicle_id = ? AND is_revoked = 1 AND revoked_at > ?
            ORDER BY revoked_at ASC
        ''', (vehicle_id, since))
    else:
        cursor.execute('''
            SELECT friend_id, revoked_at, expires_at FROM friend_keys
            WHERE vehicle_id = ? AND is_revoked = 1
            ORDER BY revoked_at ASC
        ''', (vehicle_id,))
    rows = cursor.fetchall()
    conn.close()

    return {
        "vehicle_id": vehicle_id,
        "server_time": utcnow_iso(),
        "max_key_ttl_hours": MAX_KEY_TTL_HOURS,
        "count": len(rows),
        "revoked": [
            {"friend_id": r[0], "revoked_at": r[1], "original_expires_at": r[2]}
            for r in rows
        ],
    }


# ============================================================
#  Activity log endpoints
# ============================================================

@app.post("/cars/{vehicle_id}/activity")
async def post_activity(
    vehicle_id: str,
    request: Request,
    x_anchor_mac: Optional[str] = Header(default=None),
    x_anchor_timestamp: Optional[str] = Header(default=None),
):
    """Anchor-authenticated event reporting."""
    raw_body = await request.body()
    body_str = raw_body.decode()
    try:
        parsed = json.loads(body_str)
        req = ActivityEventRequest(**parsed)
    except Exception as e:
        raise HTTPException(status_code=400, detail=f"Invalid request body: {e}")

    body_digest = body_sha256_b64(body_str)
    require_anchor_auth(vehicle_id, x_anchor_mac, x_anchor_timestamp, body_digest)

    log_event(vehicle_id, req.friend_id, req.event_type, req.result, req.details)
    return {"message": "event logged"}


@app.get("/cars/{vehicle_id}/activity")
def get_activity(
    vehicle_id: str,
    limit: int = 50,
    x_owner_key: Optional[str] = Header(default=None),
):
    verify_owner_auth(vehicle_id, x_owner_key)
    limit = max(1, min(limit, 500))

    conn = sqlite3.connect('car_access.db')
    cursor = conn.cursor()
    cursor.execute('''
        SELECT id, friend_id, event_type, result, timestamp, details
        FROM access_events
        WHERE vehicle_id = ?
        ORDER BY id DESC LIMIT ?
    ''', (vehicle_id, limit))
    rows = cursor.fetchall()
    conn.close()

    return {
        "vehicle_id": vehicle_id,
        "count": len(rows),
        "events": [
            {"id": r[0], "friend_id": r[1], "event_type": r[2],
             "result": r[3], "timestamp": r[4], "details": r[5]}
            for r in rows
        ],
    }


# ============================================================
#  Root
# ============================================================

@app.get("/")
def root():
    return {
        "service": "Smart Car Access Server",
        "version": "3.0",
        "bundle_version": BUNDLE_VERSION,
        "sequences": {
            "S1_owner_create":   ["/owner-pairing", "/friend-sharing/create"],
            "S2_guest_unlock":   ["/friend-sharing/claim/{token}", "/pairing-bootstrap"],
            "S3_anchor_validate":["/validate-challenge/{vid}", "/validate-friend-key"],
            "S4_revocation":     ["/cars/{vid}/revocations"],
        },
    }
