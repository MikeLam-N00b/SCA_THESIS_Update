import argparse
import base64
import json
import urllib.request
import urllib.error
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives import serialization, hashes
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.hazmat.primitives.ciphers.aead import AESGCM


def main() -> None:
    parser = argparse.ArgumentParser(description="Fetch pairing key from SCA secure-check-pairing endpoint")
    parser.add_argument("--server-url", default="http://127.0.0.1:8000", help="Base URL of the server")
    parser.add_argument("--vehicle-id", default="1HGBH41JXMN109186", help="Vehicle ID to query")
    parser.add_argument("--output", help="Optional output file to save the recovered key")
    args = parser.parse_args()

    # Generate temporary ECDH client key pair
    client_private_key = ec.generate_private_key(ec.SECP256R1())
    client_public_key = client_private_key.public_key()
    client_pub_bytes = client_public_key.public_bytes(
        encoding=serialization.Encoding.DER,
        format=serialization.PublicFormat.SubjectPublicKeyInfo,
    )
    client_pub_b64 = base64.b64encode(client_pub_bytes).decode()

    url = f"{args.server_url.rstrip('/')}/secure-check-pairing"
    payload = {
        "vehicle_id": args.vehicle_id,
        "client_public_key_b64": client_pub_b64,
    }

    req = urllib.request.Request(
        url,
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            data = json.load(resp)
    except urllib.error.HTTPError as exc:
        raise SystemExit(f"HTTP error {exc.code}: {exc.read().decode('utf-8', 'ignore')}") from exc
    except urllib.error.URLError as exc:
        raise SystemExit(f"URL error: {exc.reason}") from exc

    if not data.get("server_public_key_b64") or not data.get("encrypted_data_b64"):
        raise SystemExit(f"Unexpected response: {json.dumps(data, indent=2)}")

    server_pub_bytes = base64.b64decode(data["server_public_key_b64"])
    server_public_key = serialization.load_der_public_key(server_pub_bytes)

    shared_secret = client_private_key.exchange(ec.ECDH(), server_public_key)
    kek = HKDF(
        algorithm=hashes.SHA256(),
        length=16,
        salt=None,
        info=b"secure-check-kek",
    ).derive(shared_secret)

    aesgcm = AESGCM(kek)
    nonce = base64.b64decode(data["nonce_b64"])
    encrypted_data = base64.b64decode(data["encrypted_data_b64"])
    plaintext = aesgcm.decrypt(nonce, encrypted_data, None)
    decoded = json.loads(plaintext.decode())

    pairing_key_hex = decoded.get("pairing_key")
    if not pairing_key_hex:
        raise SystemExit(f"No pairing_key in response: {json.dumps(decoded, indent=2)}")

    print("Recovered pairing key (hex):", pairing_key_hex)
    print("Vehicle ID:", decoded.get("vehicle_id"))
    print("Paired:", decoded.get("paired"))

    if args.output:
        with open(args.output, "w", encoding="utf-8") as fh:
            fh.write(pairing_key_hex)
        print("Saved to:", args.output)


if __name__ == "__main__":
    main()
