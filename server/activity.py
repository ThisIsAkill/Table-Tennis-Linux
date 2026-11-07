# Copyright (c) 2026 Akhil Moola (ThisIsAkill). MIT License, see LICENSE.
"""Recent activity for the online server's status page.

Each join is stored as a time, whether it was the public room, and a hash of the player's name
(never the name or the address), so the page can say how many different players came by and when
people usually play. Rows older than KEEP_DAYS are dropped. Standard library only (sqlite3).
"""
import hashlib
import sqlite3
import time

KEEP_DAYS = 30
HOURS_WINDOW_DAYS = 14  # how far back the "usual hours" histogram looks


class Activity:
    def __init__(self, path):
        self.db = sqlite3.connect(path)
        self.db.execute("CREATE TABLE IF NOT EXISTS visits (ts INTEGER NOT NULL, player TEXT NOT NULL, "
                        "public INTEGER NOT NULL)")
        self.db.execute("CREATE INDEX IF NOT EXISTS visits_ts ON visits (ts)")
        self.db.commit()
        self.last_prune = 0.0

    @staticmethod
    def player_key(name):
        return hashlib.sha256(name.casefold().encode("utf-8")).hexdigest()[:16]

    def record(self, name, public, now=None):
        now = time.time() if now is None else now
        self.db.execute("INSERT INTO visits (ts, player, public) VALUES (?, ?, ?)",
                        (int(now), self.player_key(name), 1 if public else 0))
        if now - self.last_prune > 3600:
            self.last_prune = now
            self.db.execute("DELETE FROM visits WHERE ts < ?", (int(now - KEEP_DAYS * 86400),))
        self.db.commit()

    def summary(self, now=None):
        now = int(time.time() if now is None else now)

        def players_since(seconds):
            return self.db.execute("SELECT COUNT(DISTINCT player) FROM visits WHERE ts >= ?",
                                   (now - seconds,)).fetchone()[0]

        # Players per hour of the day (UTC): each player counts once per hour of each day.
        hours = [0] * 24
        rows = self.db.execute(
            "SELECT DISTINCT ts / 3600, player FROM visits WHERE ts >= ?",
            (now - HOURS_WINDOW_DAYS * 86400,)).fetchall()
        for hour_index, _ in rows:
            hours[hour_index % 24] += 1
        last = self.db.execute("SELECT MAX(ts) FROM visits").fetchone()[0]
        return {
            "players_last_24h": players_since(86400),
            "players_last_7d": players_since(7 * 86400),
            "players_by_utc_hour_last_14d": hours,
            "last_visit_seconds_ago": None if last is None else now - last,
        }
