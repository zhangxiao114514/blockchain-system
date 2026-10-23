"""keystore.py — 本地钱包库（私钥加密存储）

加密方案（仅用标准库）：
    key      = PBKDF2-HMAC-SHA256(passphrase, salt, 200000)
    enc_key  = HMAC-SHA256(key, "enc")
    mac_key  = HMAC-SHA256(key, "mac")
    stream   = HMAC-SHA256(enc_key, nonce || counter) 逐块拼接   （CTR 模式伪随机流）
    ct       = plaintext XOR stream
    tag      = HMAC-SHA256(mac_key, salt || nonce || ct)         （先加密后认证）

文件格式：{"v":1,"kdf":"pbkdf2-sha256","iter":200000,"salt":...,"nonce":...,"ct":...,"tag":...}
"""
from __future__ import annotations

import base64
import hashlib
import hmac
import json
import os
import secrets
import time

PBKDF2_ITER = 200000


def _hkdf(key: bytes, label: bytes) -> bytes:
    return hmac.new(key, label, hashlib.sha256).digest()


def _keystream(key: bytes, nonce: bytes, n: int) -> bytes:
    out = bytearray()
    counter = 0
    while len(out) < n:
        out += hmac.new(key, nonce + counter.to_bytes(8, "big"), hashlib.sha256).digest()
        counter += 1
    return bytes(out[:n])


def encrypt(password: str, plaintext: bytes, iterations: int = PBKDF2_ITER) -> dict:
    salt = secrets.token_bytes(16)
    nonce = secrets.token_bytes(8)
    key = hashlib.pbkdf2_hmac("sha256", password.encode(), salt, iterations, 32)
    enc_key = _hkdf(key, b"enc")
    mac_key = _hkdf(key, b"mac")
    ks = _keystream(enc_key, nonce, len(plaintext))
    ct = bytes(a ^ b for a, b in zip(plaintext, ks))
    tag = hmac.new(mac_key, salt + nonce + ct, hashlib.sha256).digest()
    return {
        "v": 1,
        "kdf": "pbkdf2-sha256",
        "iter": iterations,
        "salt": base64.b64encode(salt).decode(),
        "nonce": base64.b64encode(nonce).decode(),
        "ct": base64.b64encode(ct).decode(),
        "tag": base64.b64encode(tag).decode(),
    }


def decrypt(password: str, blob: dict) -> bytes:
    salt = base64.b64decode(blob["salt"])
    nonce = base64.b64decode(blob["nonce"])
    ct = base64.b64decode(blob["ct"])
    tag = base64.b64decode(blob["tag"])
    iterations = int(blob.get("iter", PBKDF2_ITER))
    key = hashlib.pbkdf2_hmac("sha256", password.encode(), salt, iterations, 32)
    enc_key = _hkdf(key, b"enc")
    mac_key = _hkdf(key, b"mac")
    expect = hmac.new(mac_key, salt + nonce + ct, hashlib.sha256).digest()
    if not hmac.compare_digest(expect, tag):
        raise ValueError("钱包库校验失败：口令错误或文件被篡改")
    ks = _keystream(enc_key, nonce, len(ct))
    return bytes(a ^ b for a, b in zip(ct, ks))


class Keystore:
    def __init__(self, path: str, passphrase: str | None = None):
        self.path = path
        self.passphrase = passphrase or self._auto_passphrase()
        self.wallets: dict[str, dict] = {}

    def _auto_passphrase(self) -> str:
        """未指定口令时，在数据目录生成随机口令文件（权限 600），避免明文私钥落盘。"""
        keyfile = self.path + ".key"
        if os.path.exists(keyfile):
            with open(keyfile, "r", encoding="utf-8") as f:
                return f.read().strip()
        pw = secrets.token_hex(32)
        os.makedirs(os.path.dirname(os.path.abspath(keyfile)), exist_ok=True)
        fd = os.open(keyfile, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(pw)
        try:
            os.chmod(keyfile, 0o600)
        except Exception:
            pass
        return pw

    def load(self) -> "Keystore":
        if not os.path.exists(self.path):
            return self
        with open(self.path, "r", encoding="utf-8") as f:
            blob = json.load(f)
        raw = decrypt(self.passphrase, blob)
        data = json.loads(raw.decode("utf-8"))
        self.wallets = {w["address"]: w for w in data.get("wallets", [])}
        return self

    def save(self):
        payload = json.dumps({"wallets": list(self.wallets.values())}, ensure_ascii=False).encode("utf-8")
        blob = encrypt(self.passphrase, payload)
        os.makedirs(os.path.dirname(os.path.abspath(self.path)), exist_ok=True)
        tmp = self.path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(blob, f)
        os.replace(tmp, self.path)

    # ---------- 钱包操作 ----------
    def add(self, address: str, public_key: str, private_key: str, label: str = "") -> dict:
        w = {
            "address": address,
            "public_key": public_key,
            "private_key": private_key,
            "label": label or address[:10],
            "created_at": int(time.time() * 1000),
        }
        self.wallets[address] = w
        self.save()
        return w

    def remove(self, address: str) -> bool:
        if address in self.wallets:
            del self.wallets[address]
            self.save()
            return True
        return False

    def get(self, address: str) -> dict | None:
        return self.wallets.get(address)

    def private_key(self, address: str) -> str | None:
        w = self.wallets.get(address)
        return w["private_key"] if w else None

    def list(self) -> list[dict]:
        """对外只暴露地址与标签，绝不返回私钥。"""
        return [
            {"address": w["address"], "label": w["label"], "created_at": w["created_at"]}
            for w in sorted(self.wallets.values(), key=lambda x: x["created_at"])
        ]

    def first(self) -> dict | None:
        if not self.wallets:
            return None
        return sorted(self.wallets.values(), key=lambda x: x["created_at"])[0]
