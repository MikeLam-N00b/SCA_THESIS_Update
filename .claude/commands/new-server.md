# New Feature — Server (SCA)

Bạn là senior FastAPI engineer. Implement tính năng server mới cho SCA project.

## Yêu cầu bắt buộc

### FastAPI pattern chuẩn (Pydantic V2)
```python
from pydantic import BaseModel, Field
from fastapi import HTTPException

class MyRequest(BaseModel):
    vehicle_id: str = Field(min_length=1, max_length=50)
    some_value: int = Field(gt=0, le=1000)
    optional_field: str | None = None  # KHÔNG Optional[str]

@app.post("/my-endpoint", response_model=MyResponse)
def my_endpoint(req: MyRequest):  # sync, không async (SQLite)
    # Validate business logic
    vehicle = get_vehicle_pairing(req.vehicle_id)
    if not vehicle:
        raise HTTPException(status_code=404, detail="Vehicle not found")
    ...
```

### SQLite pattern (từ sql-pro skill)
```python
def db_my_operation(vehicle_id: str, data: str) -> dict | None:
    conn = sqlite3.connect('car_access.db')
    cursor = conn.cursor()
    try:
        # LUÔN parameterized queries
        cursor.execute(
            "SELECT col1, col2 FROM table WHERE vehicle_id = ?",
            (vehicle_id,)   # tuple
        )
        result = cursor.fetchone()
        conn.commit()
        return {"col1": result[0], "col2": result[1]} if result else None
    finally:
        conn.close()  # luôn close
```

### Nếu feature liên quan đến crypto
```python
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
import os, base64, secrets

# Nonce: os.urandom(12) — không hardcode
# AES key: 16 bytes (128-bit) hoặc 32 bytes (256-bit)
# Token: secrets.token_hex(32) — không random.random()
# Sign: server_signing_key.sign(message.encode(), ec.ECDSA(hashes.SHA256()))
```

### Security checklist (secure-code-guardian skill)
- Không log key/secret trong print()
- Không return raw exception trong HTTP response
- TTL: validate ttl_hours range (> 0, ≤ 168)
- `is_revoked` check trước khi return bất kỳ key nào
- Input validation ở Pydantic model, không chỉ trong function body

### Output cần có
1. Pydantic request/response models
2. DB helper function(s)
3. FastAPI endpoint
4. Test với `anchor_client.py` hoặc `friend_client.py` pattern

## Tính năng cần implement:
$ARGUMENTS
