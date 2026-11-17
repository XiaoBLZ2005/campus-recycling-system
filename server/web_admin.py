#!/usr/bin/env python3
"""Local web administration API for the recycling-station database."""

from __future__ import annotations

import base64
import hmac
import json
import os
import sqlite3
import threading
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, urlsplit
from uuid import uuid4

from device_status import snapshot as get_device_status
from server import connect_database, normalize_uid, process_message, utc_now


ASSET_DIR = Path(__file__).resolve().with_name("web")
PLACEHOLDER_NAME = "未登记学生"
MAX_BODY_BYTES = 64 * 1024


class ApiError(Exception):
    def __init__(self, message: str, status: HTTPStatus = HTTPStatus.BAD_REQUEST):
        super().__init__(message)
        self.status = status


def clean_text(value: Any, field: str, *, max_length: int = 120) -> str:
    text = str(value or "").strip()
    if not text:
        raise ApiError(f"{field}不能为空")
    if len(text) > max_length:
        raise ApiError(f"{field}不能超过 {max_length} 个字符")
    return text


def clean_limit(value: str | None, default: int = 50, maximum: int = 200) -> int:
    try:
        return max(1, min(maximum, int(value or default)))
    except (TypeError, ValueError) as exc:
        raise ApiError("limit 必须是整数") from exc


def clean_points(value: Any) -> int:
    try:
        points = int(value)
    except (TypeError, ValueError) as exc:
        raise ApiError("积分必须是整数") from exc
    if points < 0 or points > 1_000_000:
        raise ApiError("积分必须在 0 到 1000000 之间")
    return points


def student_dict(row: sqlite3.Row) -> dict[str, Any]:
    payload = dict(row)
    if "registered" not in payload:
        payload["registered"] = payload.get("name") != PLACEHOLDER_NAME
    return payload


def get_student(connection: sqlite3.Connection, card_uid: str) -> sqlite3.Row | None:
    return connection.execute(
        """
        SELECT s.card_uid, s.name, s.total_points, s.updated_at,
               COUNT(t.event_id) AS transaction_count,
               COALESCE(SUM(t.bottles), 0) AS total_bottles,
               COALESCE(SUM(t.weight_kg), 0) AS total_weight_kg
        FROM students AS s
        LEFT JOIN transactions AS t ON t.card_uid = s.card_uid
        WHERE s.card_uid = ?
        GROUP BY s.card_uid, s.name, s.total_points, s.updated_at
        """,
        (card_uid,),
    ).fetchone()


def list_students(connection: sqlite3.Connection, query: str, limit: int) -> list[dict[str, Any]]:
    term = f"%{query.strip()}%"
    rows = connection.execute(
        """
        SELECT s.card_uid, s.name, s.total_points, s.updated_at,
               COUNT(t.event_id) AS transaction_count,
               COALESCE(SUM(t.bottles), 0) AS total_bottles,
               COALESCE(SUM(t.weight_kg), 0) AS total_weight_kg
        FROM students AS s
        LEFT JOIN transactions AS t ON t.card_uid = s.card_uid
        WHERE ? = '%%'
           OR s.card_uid LIKE ?
           OR s.name LIKE ?
        GROUP BY s.card_uid, s.name, s.total_points, s.updated_at
        ORDER BY s.updated_at DESC, s.name ASC
        LIMIT ?
        """,
        (term, term, term, limit),
    ).fetchall()
    return [student_dict(row) for row in rows]


def database_summary(connection: sqlite3.Connection) -> dict[str, Any]:
    students = connection.execute(
        """
        SELECT COUNT(*) AS student_count,
               COALESCE(SUM(total_points), 0) AS total_points
        FROM students
        """
    ).fetchone()
    transactions = connection.execute(
        """
        SELECT COUNT(*) AS transaction_count,
               COALESCE(SUM(bottles), 0) AS total_bottles,
               COALESCE(SUM(weight_kg), 0) AS total_weight_kg
        FROM transactions
        """
    ).fetchone()
    return {
        "student_count": int(students["student_count"]),
        "total_points": int(students["total_points"]),
        "transaction_count": int(transactions["transaction_count"]),
        "total_bottles": int(transactions["total_bottles"]),
        "total_weight_kg": round(float(transactions["total_weight_kg"]), 3),
    }


def list_transactions(
    connection: sqlite3.Connection, card_uid: str, limit: int
) -> list[dict[str, Any]]:
    rows = connection.execute(
        """
        SELECT event_id, device_id, card_uid, student_name, weight_kg,
               bottles, points, received_at
        FROM transactions
        WHERE ? = '' OR card_uid = ?
        ORDER BY received_at DESC
        LIMIT ?
        """,
        (card_uid, card_uid, limit),
    ).fetchall()
    return [dict(row) for row in rows]


def list_card_reads(connection: sqlite3.Connection, limit: int) -> list[dict[str, Any]]:
    rows = connection.execute(
        """
        SELECT id, card_uid, student_name, total_points, registered, seen_at
        FROM card_reads
        ORDER BY id DESC
        LIMIT ?
        """,
        (limit,),
    ).fetchall()
    return [dict(row) for row in rows]


class AdminRequestHandler(BaseHTTPRequestHandler):
    server_version = "RecyclingAdmin/1.0"

    @property
    def database_path(self) -> str:
        return self.server.database_path  # type: ignore[attr-defined]

    def log_message(self, *_args: Any) -> None:
        return

    def require_admin(self) -> bool:
        password = os.environ.get("ADMIN_PASSWORD", "")
        if not password:
            return True

        username = os.environ.get("ADMIN_USERNAME", "teacher")
        credentials = base64.b64encode(f"{username}:{password}".encode("utf-8")).decode("ascii")
        supplied = self.headers.get("Authorization", "")
        if hmac.compare_digest(supplied, f"Basic {credentials}"):
            return True

        body = b"401 Unauthorized"
        self.send_response(HTTPStatus.UNAUTHORIZED)
        self.send_header("WWW-Authenticate", 'Basic realm="Student Points Admin"')
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)
        return False

    def send_json(self, payload: Any, status: HTTPStatus = HTTPStatus.OK) -> None:
        body = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(body)

    def send_asset(self, relative_path: str, content_type: str) -> None:
        path = ASSET_DIR / relative_path
        if not path.is_file():
            raise ApiError("网页资源不存在", HTTPStatus.NOT_FOUND)
        body = path.read_bytes()
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-cache")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(body)

    def read_json(self) -> dict[str, Any]:
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError as exc:
            raise ApiError("Content-Length 无效") from exc
        if length <= 0:
            raise ApiError("请求体不能为空")
        if length > MAX_BODY_BYTES:
            raise ApiError("请求体过大", HTTPStatus.REQUEST_ENTITY_TOO_LARGE)
        try:
            payload = json.loads(self.rfile.read(length).decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise ApiError("请求体必须是 UTF-8 JSON") from exc
        if not isinstance(payload, dict):
            raise ApiError("请求体必须是 JSON 对象")
        return payload

    def handle_api(self, method: str, path: str, params: dict[str, list[str]]) -> None:
        connection = connect_database(self.database_path)
        try:
            if method == "GET" and path == "/api/health":
                self.send_json({"ok": True, "time": utc_now()})
                return

            if method == "GET" and path == "/api/device-status":
                self.send_json(get_device_status())
                return

            if method == "GET" and path == "/api/summary":
                self.send_json(database_summary(connection))
                return

            if method == "GET" and path == "/api/students":
                query = params.get("q", [""])[0]
                limit = clean_limit(params.get("limit", [None])[0])
                self.send_json({"students": list_students(connection, query, limit)})
                return

            if method == "GET" and path == "/api/students/card":
                card_uid = normalize_uid(params.get("card_uid", [""])[0])
                if not card_uid:
                    raise ApiError("card_uid 不能为空")
                row = get_student(connection, card_uid)
                if row is None:
                    raise ApiError("没有找到这张学生卡", HTTPStatus.NOT_FOUND)
                self.send_json(student_dict(row))
                return

            if method == "POST" and path == "/api/students":
                payload = self.read_json()
                card_uid = normalize_uid(clean_text(payload.get("card_uid"), "卡号", max_length=80))
                name = clean_text(payload.get("name"), "姓名", max_length=80)
                try:
                    connection.execute(
                        """
                        INSERT INTO students(card_uid, name, total_points, updated_at)
                        VALUES (?, ?, 0, ?)
                        """,
                        (card_uid, name, utc_now()),
                    )
                    connection.commit()
                except sqlite3.IntegrityError as exc:
                    raise ApiError("这张卡已经登记过", HTTPStatus.CONFLICT) from exc
                self.send_json(student_dict(get_student(connection, card_uid)), HTTPStatus.CREATED)
                return

            if method == "PUT" and path == "/api/students":
                payload = self.read_json()
                card_uid = normalize_uid(clean_text(payload.get("card_uid"), "卡号", max_length=80))
                name = clean_text(payload.get("name"), "姓名", max_length=80)
                row = get_student(connection, card_uid)
                if row is None:
                    raise ApiError("没有找到这张学生卡", HTTPStatus.NOT_FOUND)
                connection.execute(
                    "UPDATE students SET name = ?, updated_at = ? WHERE card_uid = ?",
                    (name, utc_now(), card_uid),
                )
                connection.commit()
                self.send_json(student_dict(get_student(connection, card_uid)))
                return

            if method == "DELETE" and path == "/api/students":
                card_uid = normalize_uid(params.get("card_uid", [""])[0])
                if not card_uid:
                    raise ApiError("card_uid 不能为空")
                row = get_student(connection, card_uid)
                if row is None:
                    raise ApiError("没有找到这张学生卡", HTTPStatus.NOT_FOUND)
                if int(row["transaction_count"]) > 0:
                    raise ApiError("该学生已有积分流水，不能删除；可以修改姓名", HTTPStatus.CONFLICT)
                connection.execute("DELETE FROM students WHERE card_uid = ?", (card_uid,))
                connection.commit()
                self.send_json({"ok": True})
                return

            if method == "GET" and path == "/api/transactions":
                limit = clean_limit(params.get("limit", ["50"])[0])
                card_uid = normalize_uid(params.get("card_uid", [""])[0])
                self.send_json({"transactions": list_transactions(connection, card_uid, limit)})
                return

            if method == "GET" and path == "/api/card-reads":
                limit = clean_limit(params.get("limit", ["10"])[0], default=10, maximum=50)
                self.send_json({"reads": list_card_reads(connection, limit)})
                return

            if method == "POST" and path == "/api/settlements":
                payload = self.read_json()
                card_uid = normalize_uid(payload.get("card_uid"))
                if not card_uid:
                    raise ApiError("card_uid 不能为空")
                event_id = str(payload.get("event_id") or f"WEB-{uuid4().hex}").strip()
                message = {
                    "type": "settle",
                    "msg_id": 0,
                    "event_id": event_id,
                    "device_id": str(payload.get("device_id") or "WEB_ADMIN"),
                    "card_uid": card_uid,
                    "name": str(payload.get("name") or ""),
                    "weight_kg": float(payload.get("weight_kg") or 0),
                    "bottles": max(0, int(payload.get("bottles") or 0)),
                    "points": clean_points(payload.get("points")),
                    "started_uptime_ms": 0,
                    "ended_uptime_ms": 0,
                }
                result = process_message(connection, message)
                if not result.get("ok"):
                    raise ApiError(str(result.get("error") or "结算失败"))
                student = get_student(connection, card_uid)
                self.send_json(
                    {
                        "ok": True,
                        "event_id": event_id,
                        "student": student_dict(student),
                    },
                    HTTPStatus.CREATED,
                )
                return

            raise ApiError("接口不存在", HTTPStatus.NOT_FOUND)
        finally:
            connection.close()

    def do_GET(self) -> None:
        if not self.require_admin():
            return
        parsed = urlsplit(self.path)
        path = parsed.path
        try:
            if path == "/":
                self.send_asset("index.html", "text/html; charset=utf-8")
                return
            if path == "/app.js":
                self.send_asset("app.js", "text/javascript; charset=utf-8")
                return
            if path == "/styles.css":
                self.send_asset("styles.css", "text/css; charset=utf-8")
                return
            if path == "/favicon.ico":
                self.send_response(HTTPStatus.NO_CONTENT)
                self.end_headers()
                return
            self.handle_api("GET", path, parse_qs(parsed.query))
        except ApiError as exc:
            self.send_json({"ok": False, "error": str(exc)}, exc.status)
        except (BrokenPipeError, ConnectionResetError):
            return
        except Exception as exc:
            print(f"[web] GET {path} failed: {exc}", flush=True)
            self.send_json(
                {"ok": False, "error": "服务器内部错误"},
                HTTPStatus.INTERNAL_SERVER_ERROR,
            )

    def do_POST(self) -> None:
        if not self.require_admin():
            return
        parsed = urlsplit(self.path)
        try:
            self.handle_api("POST", parsed.path, parse_qs(parsed.query))
        except ApiError as exc:
            self.send_json({"ok": False, "error": str(exc)}, exc.status)
        except (BrokenPipeError, ConnectionResetError):
            return
        except Exception as exc:
            print(f"[web] POST {parsed.path} failed: {exc}", flush=True)
            self.send_json(
                {"ok": False, "error": "服务器内部错误"},
                HTTPStatus.INTERNAL_SERVER_ERROR,
            )

    def do_PUT(self) -> None:
        if not self.require_admin():
            return
        parsed = urlsplit(self.path)
        try:
            self.handle_api("PUT", parsed.path, parse_qs(parsed.query))
        except ApiError as exc:
            self.send_json({"ok": False, "error": str(exc)}, exc.status)
        except (BrokenPipeError, ConnectionResetError):
            return
        except Exception as exc:
            print(f"[web] PUT {parsed.path} failed: {exc}", flush=True)
            self.send_json(
                {"ok": False, "error": "服务器内部错误"},
                HTTPStatus.INTERNAL_SERVER_ERROR,
            )

    def do_DELETE(self) -> None:
        if not self.require_admin():
            return
        parsed = urlsplit(self.path)
        try:
            self.handle_api("DELETE", parsed.path, parse_qs(parsed.query))
        except ApiError as exc:
            self.send_json({"ok": False, "error": str(exc)}, exc.status)
        except (BrokenPipeError, ConnectionResetError):
            return
        except Exception as exc:
            print(f"[web] DELETE {parsed.path} failed: {exc}", flush=True)
            self.send_json(
                {"ok": False, "error": "服务器内部错误"},
                HTTPStatus.INTERNAL_SERVER_ERROR,
            )


class AdminHTTPServer(ThreadingHTTPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, address: tuple[str, int], database_path: str):
        self.database_path = database_path
        super().__init__(address, AdminRequestHandler)


def start_in_background(
    host: str, port: int, database_path: str
) -> tuple[AdminHTTPServer, threading.Thread]:
    server = AdminHTTPServer((host, port), database_path)
    thread = threading.Thread(target=server.serve_forever, name="web-admin", daemon=True)
    thread.start()
    return server, thread


def stop(server: AdminHTTPServer) -> None:
    server.shutdown()
    server.server_close()
