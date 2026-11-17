"""Shared DTU connection state for the TCP and HTTP servers."""

from __future__ import annotations

import threading
import time
from datetime import datetime, timezone
from typing import Any


LOCK = threading.Lock()
STATUS = {
    "active_connections": 0,
    "last_connected_at": None,
    "last_message_at": None,
    "last_disconnected_at": None,
    "last_message_epoch": 0.0,
}


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def mark_connected() -> None:
    with LOCK:
        STATUS["active_connections"] += 1
        STATUS["last_connected_at"] = utc_now()
        STATUS["last_message_epoch"] = time.time()


def mark_message() -> None:
    with LOCK:
        STATUS["last_message_at"] = utc_now()
        STATUS["last_message_epoch"] = time.time()


def mark_disconnected() -> None:
    with LOCK:
        STATUS["active_connections"] = max(0, STATUS["active_connections"] - 1)
        STATUS["last_disconnected_at"] = utc_now()


def snapshot(online_window_seconds: int = 45) -> dict[str, Any]:
    with LOCK:
        result = dict(STATUS)

    last_message_epoch = float(result.pop("last_message_epoch", 0.0))
    result["recently_active"] = (
        last_message_epoch > 0
        and time.time() - last_message_epoch <= online_window_seconds
    )
    result["online"] = (
        result["active_connections"] > 0 and result["recently_active"]
    )
    result["online_window_seconds"] = online_window_seconds
    return result
