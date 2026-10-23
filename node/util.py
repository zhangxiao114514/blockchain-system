"""util.py — 通用工具：金额换算、日志、时间"""
from __future__ import annotations

import datetime
import sys
import threading
import time

COIN = 100_000_000  # 1 枚 = 1e8 最小单位


def parse_amount(text) -> int:
    """把 "1.5" 这样的十进制字符串转成最小单位整数。"""
    if isinstance(text, int):
        return text
    s = str(text).strip().replace(",", "")
    if not s:
        raise ValueError("金额为空")
    neg = s.startswith("-")
    if neg:
        s = s[1:]
    if s.count(".") > 1:
        raise ValueError("金额格式非法")
    if "." in s:
        ip, fp = s.split(".", 1)
    else:
        ip, fp = s, ""
    ip = ip or "0"
    fp = (fp + "00000000")[:8]
    if not ip.isdigit() or not fp.isdigit():
        raise ValueError("金额格式非法")
    value = int(ip) * COIN + int(fp)
    if value > 21_000_000 * COIN:
        raise ValueError("金额超出总量上限")
    return -value if neg else value


def format_amount(v: int, trim: bool = True) -> str:
    v = int(v)
    sign = "-" if v < 0 else ""
    v = abs(v)
    whole, frac = divmod(v, COIN)
    if frac == 0:
        return f"{sign}{whole}"
    s = f"{sign}{whole}.{frac:08d}"
    if trim:
        s = s.rstrip("0").rstrip(".")
    return s


def now_ms() -> int:
    return int(time.time() * 1000)


def fmt_time(ms: int) -> str:
    try:
        return datetime.datetime.fromtimestamp(ms / 1000).strftime("%m-%d %H:%M:%S")
    except Exception:
        return "-"


class Logger:
    _lock = threading.Lock()

    def __init__(self, name: str, quiet: bool = False):
        self.name = name
        self.quiet = quiet

    def __call__(self, msg: str, level: str = "INFO"):
        if self.quiet and level == "INFO":
            return
        ts = datetime.datetime.now().strftime("%H:%M:%S")
        line = f"[{ts}] [{self.name}] {level:<5} {msg}"
        with Logger._lock:
            try:
                print(line, flush=True)
            except UnicodeEncodeError:
                sys.stdout.buffer.write((line + "\n").encode("utf-8", "replace"))
                sys.stdout.flush()
