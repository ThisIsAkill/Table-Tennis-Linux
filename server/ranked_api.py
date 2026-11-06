# Copyright (c) 2026 Akhil Moola (ThisIsAkill). MIT License, see LICENSE.
"""Accounts, device linking, stat writes and leaderboard reads for ranked play (HTTP JSON API).

Identity: an account is a player name. Every device holds its own random secret, registered
once (when it first claims the name, or when it is linked with a one-time code shown on a device
that already has the account). Later requests are signed with an HMAC of the path, a timestamp and
the raw body, so the secret itself does not cross the network again.

Ranked results move TrueSkill only when both players' reports for the same session agree.

Routes (all JSON):
  POST /api/claim          {"name", "device", "secret"}        -> {"online_id", "name"}
  POST /api/link/create    signed, {}                          -> {"code", "expires_in"}
  POST /api/link/redeem    {"code", "device", "secret"}        -> {"online_id", "name"}
  POST /api/stats          signed, {"views": [{"view", "props": {"id": value}}],
                                    "ranked": {"session", "won"}}   -> {"applied"}
  GET  /api/leaderboard?view=N&start=1&count=10[&pivot=online_id]  -> {"total", "rows"}
  GET  /api/players?view=N&ids=a,b                                 -> {"rows"}
Signed requests carry ?device=ID&ts=UNIX&sig=HEX, sig = HMAC-SHA256(secret, path\nts\nbody).
"""

import hashlib
import hmac
import json
import re
import secrets
import sqlite3
import time
from urllib.parse import parse_qs

from leaderboards import Leaderboards

NAME_RE = re.compile(r"^[A-Za-z0-9_-]{1,15}$")
LINK_TTL = 300.0
LINK_ALPHABET = "ABCDEFGHJKMNPQRSTUVWXYZ23456789"
SIG_WINDOW = 300.0
RANKED_PAIR_WINDOW = 1800.0


class RankedApi:
    def __init__(self, path):
        self.boards = Leaderboards(path)
        self.db = self.boards._db
        self.lock = self.boards._lock
        self.db.executescript("""
            CREATE TABLE IF NOT EXISTS accounts (
                online_id TEXT PRIMARY KEY, name TEXT NOT NULL UNIQUE COLLATE NOCASE,
                created REAL NOT NULL);
            CREATE TABLE IF NOT EXISTS devices (
                device TEXT PRIMARY KEY, online_id TEXT NOT NULL, secret TEXT NOT NULL,
                created REAL NOT NULL, last_seen REAL NOT NULL);
            CREATE TABLE IF NOT EXISTS link_codes (
                code TEXT PRIMARY KEY, online_id TEXT NOT NULL, expires REAL NOT NULL);
            CREATE TABLE IF NOT EXISTS ranked_reports (
                session TEXT, online_id TEXT, won INTEGER, at REAL,
                PRIMARY KEY (session, online_id));
        """)
        self.db.commit()
        self.recent_sigs = {}

    # --- helpers --------------------------------------------------------------------------------
    def _account_of(self, device):
        row = self.db.execute(
            "SELECT a.online_id, a.name, d.secret FROM devices d JOIN accounts a USING (online_id) "
            "WHERE d.device=?", (device,)).fetchone()
        return row

    def _verify(self, path, query, body):
        device = query.get("device", [""])[0]
        ts = query.get("ts", ["0"])[0]
        sig = query.get("sig", [""])[0]
        row = self._account_of(device)
        if not row:
            return None, "unknown_device"
        try:
            age = abs(time.time() - float(ts))
        except ValueError:
            return None, "bad_timestamp"
        if age > SIG_WINDOW:
            return None, "stale_request"
        want = hmac.new(row[2].encode(), f"{path}\n{ts}\n".encode() + body, hashlib.sha256).hexdigest()
        if not hmac.compare_digest(want, sig):
            return None, "bad_signature"
        now = time.time()
        self.recent_sigs = {s: t for s, t in self.recent_sigs.items() if now - t < SIG_WINDOW}
        if sig in self.recent_sigs:
            return None, "replayed"
        self.recent_sigs[sig] = now
        self.db.execute("UPDATE devices SET last_seen=? WHERE device=?", (now, device))
        return row, None

    @staticmethod
    def _valid_device(device, secret=None):
        if not re.fullmatch(r"[0-9a-f]{16,64}", device or ""):
            return False
        return secret is None or bool(re.fullmatch(r"[0-9a-f]{32,128}", secret or ""))

    # --- routes ---------------------------------------------------------------------------------
    def handle(self, method, target, body):
        """Returns (http_status, json_dict)."""
        path, _, qs = target.partition("?")
        query = parse_qs(qs)
        try:
            payload = json.loads(body) if body else {}
        except ValueError:
            return 400, {"error": "bad_json"}
        with self.lock:
            try:
                return self._route(method, path, query, body, payload)
            finally:
                self.db.commit()

    def _route(self, method, path, query, body, payload):
        now = time.time()
        if method == "POST" and path == "/api/claim":
            name, device, secret = payload.get("name", ""), payload.get("device", ""), payload.get("secret", "")
            if not NAME_RE.match(name) or not self._valid_device(device, secret):
                return 400, {"error": "bad_request"}
            row = self._account_of(device)
            if row:
                if row[1].lower() != name.lower():
                    # The device's account wants a new name: allowed when the name is free.
                    taken = self.db.execute("SELECT 1 FROM accounts WHERE name=?", (name,)).fetchone()
                    if taken:
                        return 409, {"error": "name_taken", "online_id": row[0], "name": row[1]}
                    self.db.execute("UPDATE accounts SET name=? WHERE online_id=?", (name, row[0]))
                    return 200, {"online_id": row[0], "name": name}
                return 200, {"online_id": row[0], "name": row[1]}
            if self.db.execute("SELECT 1 FROM accounts WHERE name=?", (name,)).fetchone():
                return 409, {"error": "name_taken"}
            online_id = secrets.token_hex(8)
            self.db.execute("INSERT INTO accounts VALUES (?, ?, ?)", (online_id, name, now))
            self.db.execute("INSERT INTO devices VALUES (?, ?, ?, ?, ?)", (device, online_id, secret, now, now))
            return 200, {"online_id": online_id, "name": name}

        if method == "POST" and path == "/api/link/create":
            row, err = self._verify(path, query, body)
            if err:
                return 403, {"error": err}
            self.db.execute("DELETE FROM link_codes WHERE expires < ? OR online_id=?", (now, row[0]))
            code = "".join(secrets.choice(LINK_ALPHABET) for _ in range(6))
            self.db.execute("INSERT INTO link_codes VALUES (?, ?, ?)", (code, row[0], now + LINK_TTL))
            return 200, {"code": code, "expires_in": int(LINK_TTL)}

        if method == "POST" and path == "/api/link/redeem":
            code = str(payload.get("code", "")).upper().strip()
            device, secret = payload.get("device", ""), payload.get("secret", "")
            if not self._valid_device(device, secret):
                return 400, {"error": "bad_request"}
            link = self.db.execute("SELECT online_id, expires FROM link_codes WHERE code=?", (code,)).fetchone()
            if not link or link[1] < now:
                return 404, {"error": "bad_code"}
            self.db.execute("DELETE FROM link_codes WHERE code=?", (code,))
            self.db.execute("INSERT OR REPLACE INTO devices VALUES (?, ?, ?, ?, ?)",
                            (device, link[0], secret, now, now))
            name = self.db.execute("SELECT name FROM accounts WHERE online_id=?", (link[0],)).fetchone()[0]
            return 200, {"online_id": link[0], "name": name}

        if method == "POST" and path == "/api/stats":
            row, err = self._verify(path, query, body)
            if err:
                return 403, {"error": err}
            online_id, name = row[0], row[1]
            applied = 0
            for view in payload.get("views", [])[:32]:
                props = {int(k, 0) if isinstance(k, str) else int(k): float(v)
                         for k, v in dict(view.get("props", {})).items()}
                applied += self.boards.write(online_id, name, int(view.get("view", 0)), props)
            ranked = payload.get("ranked")
            if ranked and ranked.get("session"):
                self._ranked_report(str(ranked["session"])[:64], online_id, bool(ranked.get("won")), now)
            return 200, {"applied": applied}

        if method == "GET" and path == "/api/leaderboard":
            try:
                view = int(query.get("view", ["0"])[0], 0)
                start = max(1, int(query.get("start", ["1"])[0]))
                count = max(1, min(100, int(query.get("count", ["10"])[0])))
            except ValueError:
                return 400, {"error": "bad_request"}
            pivot = query.get("pivot", [None])[0]
            total, rows = self.boards.query(view, start, count, pivot)
            return 200, {"total": total, "rows": rows}

        if method == "GET" and path == "/api/players":
            try:
                view = int(query.get("view", ["0"])[0], 0)
            except ValueError:
                return 400, {"error": "bad_request"}
            ids = [i for i in query.get("ids", [""])[0].split(",") if i][:16]
            total, rows = self.boards.query(view, 1, 1_000_000)
            wanted = [r for r in rows if r["online_id"] in ids]
            return 200, {"total": total, "rows": wanted,
                         "skill": {i: self.boards.skill(i) for i in ids}}

        return 404, {"error": "not_found"}

    def _ranked_report(self, session, online_id, won, now):
        """Records one side's result; when both sides of the session agree, moves TrueSkill."""
        self.db.execute("DELETE FROM ranked_reports WHERE at < ?", (now - RANKED_PAIR_WINDOW,))
        self.db.execute("INSERT OR IGNORE INTO ranked_reports VALUES (?, ?, ?, ?)",
                        (session, online_id, int(won), now))
        reports = self.db.execute("SELECT online_id, won FROM ranked_reports WHERE session=?",
                                  (session,)).fetchall()
        if len(reports) == 2:
            (a, aw), (b, bw) = reports
            if aw != bw:  # exactly one winner: the reports agree
                winner, loser = (a, b) if aw else (b, a)
                self.boards.record_ranked(winner, loser)
            self.db.execute("DELETE FROM ranked_reports WHERE session=?", (session,))
