#!/usr/bin/env python3
"""TCP + JSON backend for the recycling-station M100M-C2 DTU."""

from __future__ import annotations

import argparse
import csv
import json
import socketserver
import sqlite3
import tempfile
import webbrowser
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from device_status import mark_connected, mark_disconnected, mark_message


SCHEMA = """
CREATE TABLE IF NOT EXISTS students (
    card_uid TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    total_points INTEGER NOT NULL DEFAULT 0,
    updated_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS transactions (
    event_id TEXT PRIMARY KEY,
    device_id TEXT NOT NULL,
    card_uid TEXT NOT NULL,
    student_name TEXT NOT NULL,
    weight_kg REAL NOT NULL,
    bottles INTEGER NOT NULL,
    points INTEGER NOT NULL,
    started_uptime_ms INTEGER NOT NULL,
    ended_uptime_ms INTEGER NOT NULL,
    received_at TEXT NOT NULL,
    FOREIGN KEY (card_uid) REFERENCES students(card_uid)
);

CREATE TABLE IF NOT EXISTS card_reads (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    event_id TEXT,
    card_uid TEXT NOT NULL,
    student_name TEXT NOT NULL,
    total_points INTEGER NOT NULL,
    registered INTEGER NOT NULL,
    seen_at TEXT NOT NULL,
    FOREIGN KEY (card_uid) REFERENCES students(card_uid)
);

CREATE INDEX IF NOT EXISTS idx_transactions_received_at
ON transactions(received_at DESC);

CREATE INDEX IF NOT EXISTS idx_card_reads_seen_at
ON card_reads(seen_at DESC);
"""


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def normalize_uid(value: Any) -> str:
    return str(value or "").strip().upper()


def connect_database(path: str) -> sqlite3.Connection:
    connection = sqlite3.connect(path, timeout=10)
    connection.row_factory = sqlite3.Row
    connection.executescript(SCHEMA)
    columns = {
        row["name"] for row in connection.execute("PRAGMA table_info(card_reads)")
    }
    if "event_id" not in columns:
        connection.execute("ALTER TABLE card_reads ADD COLUMN event_id TEXT")
    connection.execute(
        """
        CREATE UNIQUE INDEX IF NOT EXISTS idx_card_reads_event_id
        ON card_reads(event_id)
        WHERE event_id IS NOT NULL
        """
    )
    connection.commit()
    return connection


def ensure_student(connection: sqlite3.Connection, card_uid: str) -> sqlite3.Row:
    """Return a card's student, creating the protocol placeholder if necessary."""
    connection.execute(
        """
        INSERT INTO students(card_uid, name, total_points, updated_at)
        VALUES (?, ?, 0, ?)
        ON CONFLICT(card_uid) DO NOTHING
        """,
        (card_uid, "未登记学生", utc_now()),
    )
    return connection.execute(
        "SELECT card_uid, name, total_points FROM students WHERE card_uid = ?",
        (card_uid,),
    ).fetchone()


def student_for_card(connection: sqlite3.Connection, card_uid: str) -> sqlite3.Row:
    student = ensure_student(connection, card_uid)
    connection.commit()
    return student


def record_card_read(
    connection: sqlite3.Connection, student: sqlite3.Row, event_id: str = ""
) -> None:
    connection.execute(
        """
        INSERT OR IGNORE INTO card_reads(
            event_id, card_uid, student_name, total_points, registered, seen_at
        ) VALUES (?, ?, ?, ?, ?, ?)
        """,
        (
            event_id or None,
            student["card_uid"],
            student["name"],
            int(student["total_points"]),
            int(student["name"] != "未登记学生"),
            utc_now(),
        ),
    )


def process_message(connection: sqlite3.Connection, message: dict[str, Any]) -> dict[str, Any]:
    message_type = str(message.get("type", ""))
    message_id = int(message.get("msg_id", 0))
    card_uid = normalize_uid(message.get("card_uid"))

    if not card_uid:
        return {
            "type": "error",
            "msg_id": message_id,
            "ok": False,
            "error": "missing_card_uid",
        }

    if message_type == "card_start":
        student = ensure_student(connection, card_uid)
        record_card_read(connection, student, str(message.get("event_id") or ""))
        connection.commit()
        return {
            "type": "student",
            "msg_id": message_id,
            "ok": True,
            "card_uid": card_uid,
            "name": student["name"],
            "total_points": int(student["total_points"]),
            "registered": student["name"] != "未登记学生",
        }

    if message_type == "settle":
        event_id = str(message.get("event_id", "")).strip()
        if not event_id:
            return {
                "type": "error",
                "msg_id": message_id,
                "ok": False,
                "error": "missing_event_id",
            }

        connection.execute("BEGIN IMMEDIATE")
        try:
            student = ensure_student(connection, card_uid)
            existing = connection.execute(
                "SELECT points FROM transactions WHERE event_id = ?",
                (event_id,),
            ).fetchone()
            if existing is None:
                points = max(0, int(message.get("points", 0)))
                name = student["name"]
                if name == "未登记学生":
                    supplied_name = str(message.get("name", "")).strip()
                    if supplied_name:
                        name = supplied_name
                        connection.execute(
                            "UPDATE students SET name = ?, updated_at = ? "
                            "WHERE card_uid = ?",
                            (name, utc_now(), card_uid),
                        )
                connection.execute(
                    """
                    INSERT INTO transactions(
                        event_id, device_id, card_uid, student_name,
                        weight_kg, bottles, points, started_uptime_ms,
                        ended_uptime_ms, received_at
                    ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                    """,
                    (
                        event_id,
                        str(message.get("device_id", "")),
                        card_uid,
                        name,
                        float(message.get("weight_kg", 0.0)),
                        max(0, int(message.get("bottles", 0))),
                        points,
                        int(message.get("started_uptime_ms", 0)),
                        int(message.get("ended_uptime_ms", 0)),
                        utc_now(),
                    ),
                )
                connection.execute(
                    """
                    UPDATE students
                    SET total_points = total_points + ?, updated_at = ?
                    WHERE card_uid = ?
                    """,
                    (points, utc_now(), card_uid),
                )
            connection.commit()
        except Exception:
            connection.rollback()
            raise

        total_points = connection.execute(
            "SELECT total_points FROM students WHERE card_uid = ?",
            (card_uid,),
        ).fetchone()["total_points"]
        return {
            "type": "settle_ack",
            "msg_id": message_id,
            "ok": True,
            "event_id": event_id,
            "card_uid": card_uid,
            "saved": True,
            "total_points": int(total_points),
        }

    if message_type == "ping":
        return {"type": "pong", "msg_id": message_id, "ok": True}

    return {
        "type": "error",
        "msg_id": message_id,
        "ok": False,
        "error": "unknown_message_type",
    }


def import_students(connection: sqlite3.Connection, csv_path: Path) -> None:
    with csv_path.open("r", encoding="utf-8-sig", newline="") as handle:
        for row in csv.DictReader(handle):
            card_uid = normalize_uid(row.get("card_uid"))
            name = str(row.get("name", "")).strip()
            total_points = max(0, int(row.get("total_points") or 0))
            if not card_uid or not name:
                continue
            connection.execute(
                """
                INSERT INTO students(card_uid, name, total_points, updated_at)
                VALUES (?, ?, ?, ?)
                ON CONFLICT(card_uid) DO UPDATE SET
                    name = excluded.name,
                    total_points = excluded.total_points,
                    updated_at = excluded.updated_at
                """,
                (card_uid, name, total_points, utc_now()),
            )
    connection.commit()


class RecyclingHandler(socketserver.StreamRequestHandler):
    def handle(self) -> None:
        connection = connect_database(self.server.database_path)  # type: ignore[attr-defined]
        debug_raw = bool(getattr(self.server, "debug_raw", False))
        client = f"{self.client_address[0]}:{self.client_address[1]}"
        print(f"[{utc_now()}] connected {client}", flush=True)
        mark_connected()
        try:
            for raw_line in self.rfile:
                line = raw_line.decode("utf-8", errors="replace").strip()
                line = line.lstrip("\ufeff")
                if debug_raw:
                    print(f"[{utc_now()}] raw {client}: {line!r}", flush=True)
                if not line:
                    # A newline-only heartbeat is still valid proof that the
                    # DTU connection is alive.
                    mark_message()
                    continue
                message: dict[str, Any] = {}
                try:
                    message = json.loads(line)
                    if not isinstance(message, dict):
                        raise ValueError("message must be an object")
                    response = process_message(connection, message)
                except Exception as exc:
                    response = {
                        "type": "error",
                        "msg_id": 0,
                        "ok": False,
                        "error": str(exc),
                    }
                encoded = (json.dumps(response, ensure_ascii=False) + "\n").encode("utf-8")
                self.wfile.write(encoded)
                self.wfile.flush()
                mark_message()
                print(
                    f"[{utc_now()}] {client} {message.get('type', '?')} -> "
                    f"{response.get('type', '?')}",
                    flush=True,
                )
        finally:
            connection.close()
            mark_disconnected()
            print(f"[{utc_now()}] disconnected {client}", flush=True)


class RecyclingServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, address: tuple[str, int], database_path: str, debug_raw: bool = False):
        self.database_path = database_path
        self.debug_raw = debug_raw
        super().__init__(address, RecyclingHandler)


def selftest() -> None:
    with tempfile.TemporaryDirectory() as temp_dir:
        connection = connect_database(str(Path(temp_dir) / "selftest.db"))
        connection.execute(
            "INSERT INTO students(card_uid, name, total_points, updated_at) VALUES (?, ?, ?, ?)",
            ("01:23:45:67:89:AB:CD:EF", "测试学生", 100, utc_now()),
        )
        connection.commit()

        started = process_message(
            connection,
            {
                "type": "card_start",
                "msg_id": 1,
                "card_uid": "01:23:45:67:89:ab:cd:ef",
            },
        )
        assert started["name"] == "测试学生"
        assert started["total_points"] == 100

        settlement = {
            "type": "settle",
            "msg_id": 2,
            "event_id": "SELFTEST-1",
            "device_id": "TEST",
            "card_uid": "01:23:45:67:89:AB:CD:EF",
            "weight_kg": 0.48,
            "bottles": 2,
            "points": 24,
            "started_uptime_ms": 1000,
            "ended_uptime_ms": 2000,
        }
        first = process_message(connection, settlement)
        duplicate = process_message(connection, settlement)
        assert first["total_points"] == 124
        assert duplicate["total_points"] == 124
        rows = connection.execute("SELECT COUNT(*) FROM transactions").fetchone()[0]
        assert rows == 1
        connection.close()
        print("selftest passed: card lookup, settlement, and duplicate protection")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--database", default="recycling.db")
    parser.add_argument("--import-csv", type=Path)
    parser.add_argument("--http-host", default="127.0.0.1")
    parser.add_argument("--http-port", type=int, default=8765)
    parser.add_argument("--no-http", action="store_true")
    parser.add_argument("--open-browser", action="store_true")
    parser.add_argument("--debug-raw", action="store_true")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()

    if args.selftest:
        selftest()
        return

    connection = connect_database(args.database)
    if args.import_csv:
        import_students(connection, args.import_csv)
    connection.close()

    with RecyclingServer(
        (args.host, args.port), args.database, debug_raw=args.debug_raw
    ) as server:
        web_server = None
        try:
            if not args.no_http:
                from web_admin import start_in_background, stop

                web_server, _ = start_in_background(
                    args.http_host, args.http_port, args.database
                )
                browser_host = (
                    "127.0.0.1"
                    if args.http_host in {"0.0.0.0", "::", "[::]"}
                    else args.http_host
                )
                http_url = f"http://{browser_host}:{args.http_port}/"
                print(f"web admin: {http_url}", flush=True)
                if args.http_host not in {"127.0.0.1", "localhost", "::1"}:
                    print(
                        "warning: 网页管理台正在监听局域网；当前 MVP 未配置登录认证，"
                        "请只在可信网络中开放。",
                        flush=True,
                    )
                if args.open_browser:
                    webbrowser.open(http_url)

            print(
                f"DTU TCP listening on {args.host}:{args.port}, "
                f"database={args.database}",
                flush=True,
            )
            server.serve_forever()
        except KeyboardInterrupt:
            print("shutting down", flush=True)
        finally:
            if web_server is not None:
                stop(web_server)


if __name__ == "__main__":
    main()
