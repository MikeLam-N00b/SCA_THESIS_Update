import requests
import base64
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives import serialization, hashes
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

# --- Vehicle key pair ---
vehicle_private_key = ec.generate_private_key(ec.SECP256R1())
vehicle_public_key = vehicle_private_key.public_key()

vehicle_pub_bytes = vehicle_public_key.public_bytes(
    encoding=serialization.Encoding.DER,
    format=serialization.PublicFormat.SubjectPublicKeyInfo
)

vehicle_pub_b64 = base64.b64encode(vehicle_pub_bytes).decode()

# --- Send request ---
resp = requests.post(
    "http://127.0.0.1:8000/owner-pairing",
    json={
        "vehicle_id": "VIN123456",
        "vehicle_public_key_b64": vehicle_pub_b64
    }
)

data = resp.json()
print("Server response received")

# --- Load server public key ---
server_pub_bytes = base64.b64decode(data["server_public_key_b64"])
server_public_key = serialization.load_der_public_key(server_pub_bytes)

# --- ECDH ---
shared_secret = vehicle_private_key.exchange(ec.ECDH(), server_public_key)

# --- Derive same KEK ---
kek = HKDF(
    algorithm=hashes.SHA256(),
    length=16,


    salt=None,
    info=b"owner-pairing-kek",
).derive(shared_secret)

# --- Decrypt pairing key ---
aesgcm = AESGCM(kek)
nonce = base64.b64decode(data["nonce_b64"])
encrypted_pairing_key = base64.b64decode(data["encrypted_pairing_key_b64"])

pairing_key = aesgcm.decrypt(
    nonce,
    encrypted_pairing_key,
    None
)

print("Recovered pairing key (hex):", pairing_key.hex())
print("Pairing key length:", len(pairing_key), "bytes")
