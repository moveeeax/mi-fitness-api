"""Snapshot the upstream crypto as fixed input/output pairs.

Run once against the Python bridge's venv. The C++ port must reproduce these
byte for byte; that is the only cheap way to verify a hand-ported RC4 variant
and signature scheme without a live Xiaomi account.

    BRIDGE_SRC=/path/to/mi_fitness_data_bridge/src \
      python3 tools/gen_crypto_vectors.py

The upstream is AGPL-3.0-only, same as this repository: the vectors below are
its output on fixed inputs, not a copy of its source.
"""

import base64
import json
import os
import pathlib
import sys

BRIDGE_SRC = os.environ.get(
    "BRIDGE_SRC", "/Users/moveeeax/Public/github/mi_fitness_data_bridge/src"
)
sys.path.insert(0, BRIDGE_SRC)

from mi_fitness_mcp.adapters.mi_fitness_cloud import (  # noqa: E402
    _gen_signature,
    _gen_signed_nonce,
    _rc4_crypt,
)

# Ключи разной длины, включая короткий: RC4 крутит ключ по модулю длины.
KEYS = [b"\x01" * 32, bytes(range(32)), b"short-key"]
# Полезные нагрузки: пустая, однобайтовая, реальный payload и все 256 значений байта.
PAYLOADS = [b"", b"a", b'{"start_time":1,"end_time":2}', bytes(range(256))]

vectors: dict[str, list[dict]] = {"rc4": [], "signed_nonce": [], "signature": []}

for key in KEYS:
    for payload in PAYLOADS:
        vectors["rc4"].append(
            {
                "key_b64": base64.b64encode(key).decode(),
                "payload_b64": base64.b64encode(payload).decode(),
                "cipher_b64": base64.b64encode(_rc4_crypt(key, payload)).decode(),
            }
        )

for ssecurity in (b"\x00" * 16, b"secret-material!"):
    for nonce in (b"\x00" * 12, bytes(range(12))):
        vectors["signed_nonce"].append(
            {
                "ssecurity_b64": base64.b64encode(ssecurity).decode(),
                "nonce_b64": base64.b64encode(nonce).decode(),
                "signed_nonce_b64": base64.b64encode(
                    _gen_signed_nonce(ssecurity, nonce)
                ).decode(),
            }
        )

signed_nonce = _gen_signed_nonce(b"secret-material!", bytes(range(12)))
PATH = "/app/v1/data/get_fitness_data_by_time"
for data in ('{"key":"steps"}', ""):
    for rc4_hash in (None, "QUJD"):
        values = {"data": data} | ({"rc4_hash__": rc4_hash} if rc4_hash else {})
        vectors["signature"].append(
            {
                "method": "POST",
                "path": PATH,
                "data": data,
                "rc4_hash": rc4_hash,
                "signed_nonce_b64": base64.b64encode(signed_nonce).decode(),
                "signature": _gen_signature("POST", PATH, values, signed_nonce),
            }
        )

out = pathlib.Path("tests/fixtures/xiaomi_crypto_vectors.json")
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(json.dumps(vectors, indent=2, sort_keys=True) + "\n")
print(f"{out}: {sum(len(v) for v in vectors.values())} vectors")
