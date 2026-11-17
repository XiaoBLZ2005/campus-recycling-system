#!/usr/bin/env python3
"""End-to-end smoke test for the local student-points web admin."""

from __future__ import annotations

import json
import socket
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import Request, urlopen

sys.path.insert(0, str(Path(__file__).resolve().parent))

from server import RecyclingServer, connect_database, process_message
from web_admin import AdminHTTPServer


class WebAdminTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.database_path = str(Path(self.temp_dir.name) / "test.db")
        connection = connect_database(self.database_path)
        connection.close()

        self.server = AdminHTTPServer(("127.0.0.1", 0), self.database_path)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base_url = f"http://127.0.0.1:{self.server.server_port}"

    def tearDown(self) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=3)
        self.temp_dir.cleanup()

    def request(
        self, method: str, path: str, payload: dict | None = None
    ) -> tuple[int, dict | str]:
        data = None
        headers = {}
        if payload is not None:
            data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
            headers["Content-Type"] = "application/json"
        request = Request(self.base_url + path, data=data, headers=headers, method=method)
        try:
            with urlopen(request, timeout=3) as response:
                body = response.read()
                content_type = response.headers.get("Content-Type", "")
                if "application/json" in content_type:
                    return response.status, json.loads(body)
                return response.status, body.decode("utf-8")
        except HTTPError as exc:
            return exc.code, json.loads(exc.read())

    def test_full_card_and_settlement_flow(self) -> None:
        status, page = self.request("GET", "/")
        self.assertEqual(status, 200)
        self.assertIn("学生积分管理后台", page)

        status, student = self.request(
            "POST",
            "/api/students",
            {"card_uid": "01:23:45:67:89:ab:cd:ef", "name": "张三"},
        )
        self.assertEqual(status, 201)
        self.assertEqual(student["total_points"], 0)

        status, lookup = self.request(
            "GET", "/api/students/card?card_uid=01%3A23%3A45%3A67%3A89%3AAB%3ACD%3AEF"
        )
        self.assertEqual(status, 200)
        self.assertEqual(lookup["name"], "张三")

        settlement = {
            "card_uid": "01:23:45:67:89:ab:cd:ef",
            "event_id": "HTTP-TEST-1",
            "points": 24,
        }
        status, first = self.request("POST", "/api/settlements", settlement)
        self.assertEqual(status, 201)
        self.assertEqual(first["student"]["total_points"], 24)

        status, duplicate = self.request("POST", "/api/settlements", settlement)
        self.assertEqual(status, 201)
        self.assertEqual(duplicate["student"]["total_points"], 24)

        status, summary = self.request("GET", "/api/summary")
        self.assertEqual(status, 200)
        self.assertEqual(summary["student_count"], 1)
        self.assertEqual(summary["total_points"], 24)
        self.assertEqual(summary["transaction_count"], 1)

        connection = connect_database(self.database_path)
        card_start = {
            "type": "card_start",
            "msg_id": 1,
            "event_id": "CARD-READ-TEST-1",
            "card_uid": "01:23:45:67:89:AB:CD:EF",
        }
        process_message(connection, card_start)
        process_message(connection, card_start)
        connection.close()
        status, reads = self.request("GET", "/api/card-reads?limit=5")
        self.assertEqual(status, 200)
        self.assertEqual(len(reads["reads"]), 1)
        self.assertIsInstance(reads["reads"][0]["id"], int)
        self.assertEqual(reads["reads"][0]["student_name"], "张三")
        self.assertEqual(reads["reads"][0]["total_points"], 24)

    def test_device_online_status_tracks_tcp_connection(self) -> None:
        tcp_server = RecyclingServer(("127.0.0.1", 0), self.database_path)
        tcp_thread = threading.Thread(
            target=tcp_server.serve_forever, daemon=True
        )
        tcp_thread.start()
        client = socket.create_connection(
            ("127.0.0.1", tcp_server.server_address[1]), timeout=3
        )

        try:
            status, device = self.request("GET", "/api/device-status")
            self.assertEqual(status, 200)
            self.assertTrue(device["online"])
            self.assertEqual(device["active_connections"], 1)

            client.sendall(
                b'{"type":"ping","msg_id":1,"card_uid":"TEST"}\n'
            )
            reply = client.recv(1024).decode("utf-8")
            self.assertIn('"type": "pong"', reply)

            status, connected = self.request("GET", "/api/device-status")
            self.assertEqual(status, 200)
            self.assertTrue(connected["online"])
            self.assertIsNotNone(connected["last_message_at"])

            client.close()
            for _ in range(20):
                status, disconnected = self.request("GET", "/api/device-status")
                if disconnected["active_connections"] == 0:
                    break
                threading.Event().wait(0.05)
            self.assertEqual(status, 200)
            self.assertFalse(disconnected["online"])
            self.assertEqual(disconnected["active_connections"], 0)
        finally:
            client.close()
            tcp_server.shutdown()
            tcp_server.server_close()
            tcp_thread.join(timeout=3)


if __name__ == "__main__":
    unittest.main(verbosity=2)
