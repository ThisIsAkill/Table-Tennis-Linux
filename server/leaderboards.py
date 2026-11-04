"""Leaderboards and TrueSkill for the Table Tennis online server.

Stats arrive per player as (view, property, value) the way the game writes them on Xbox LIVE, and
are kept the way the game's own stats views (leaderboard_schema.py) describe: each property is
either summed over matches or replaced by the latest value, inside a weekly or yearly bucket.
Each view is ranked by its sort property, highest first. Ranked results also move a TrueSkill
rating (mu, sigma), shown on the leaderboard's TrueSkill column.

Players are identified by an online ID (a random value each install keeps next to its saves), not
by the game's XUID, which is the same on every install. Standard library only (sqlite3).
"""

import math
import sqlite3
import threading
import time

from leaderboard_schema import VIEWS, TRUESKILL_VIEW

MU0 = 25.0
SIGMA0 = MU0 / 3.0
BETA = SIGMA0 / 2.0
TAU = SIGMA0 / 100.0
# Xbox LIVE's TrueSkill view columns (X_STATS_COLUMN_SKILL_*).
SKILL_COLUMN, GAMES_COLUMN, MU_COLUMN, SIGMA_COLUMN = 61, 62, 63, 64


def period_key(period, now=None):
    t = time.gmtime(now if now is not None else time.time())
    if period == "week":
        year, week, _ = time.strftime("%G %V %u", t).split()
        return f"{year}-W{week}"
    return time.strftime("%Y", t)


def displayed_skill(mu, sigma):
    """The 1-50 skill Xbox LIVE showed: a conservative estimate, clamped."""
    return max(1, min(50, int(round(mu - 3.0 * sigma))))


def _pdf(x):
    return math.exp(-0.5 * x * x) / math.sqrt(2.0 * math.pi)


def _cdf(x):
    return 0.5 * (1.0 + math.erf(x / math.sqrt(2.0)))


def trueskill_1v1(winner, loser):
    """One TrueSkill update for a decided two-player game (no draws). Each side is (mu, sigma)."""
    (mw, sw), (ml, sl) = winner, loser
    sw2 = sw * sw + TAU * TAU
    sl2 = sl * sl + TAU * TAU
    c2 = 2.0 * BETA * BETA + sw2 + sl2
    c = math.sqrt(c2)
    t = (mw - ml) / c
    denom = max(_cdf(t), 1e-12)
    v = _pdf(t) / denom
    w = v * (v + t)
    mw2 = mw + sw2 / c * v
    ml2 = ml - sl2 / c * v
    sw_new = math.sqrt(max(sw2 * (1.0 - sw2 / c2 * w), 1e-6))
    sl_new = math.sqrt(max(sl2 * (1.0 - sl2 / c2 * w), 1e-6))
    return (mw2, sw_new), (ml2, sl_new)


class Leaderboards:
    def __init__(self, path):
        self._lock = threading.RLock()
        self._db = sqlite3.connect(path, check_same_thread=False)
        self._db.executescript("""
            CREATE TABLE IF NOT EXISTS players (
                online_id TEXT PRIMARY KEY, gamertag TEXT NOT NULL, updated REAL NOT NULL);
            CREATE TABLE IF NOT EXISTS stats (
                view_id INTEGER, period TEXT, online_id TEXT, prop_id INTEGER, value REAL,
                PRIMARY KEY (view_id, period, online_id, prop_id));
            CREATE TABLE IF NOT EXISTS skill (
                online_id TEXT PRIMARY KEY, mu REAL NOT NULL, sigma REAL NOT NULL,
                games INTEGER NOT NULL DEFAULT 0);
        """)
        self._db.commit()

    def _touch_player(self, online_id, gamertag):
        self._db.execute(
            "INSERT INTO players (online_id, gamertag, updated) VALUES (?, ?, ?) "
            "ON CONFLICT(online_id) DO UPDATE SET gamertag=excluded.gamertag, updated=excluded.updated",
            (online_id, gamertag[:15], time.time()))

    def write(self, online_id, gamertag, view_id, props, now=None):
        """Applies one view's properties for one player. Unknown views/properties are ignored."""
        view = VIEWS.get(view_id)
        if not view:
            return 0
        key = period_key(view["period"], now)
        applied = 0
        with self._lock:
            self._touch_player(online_id, gamertag)
            for prop_id, value in props.items():
                agg = view["props"].get(prop_id)
                if agg is None:
                    continue
                if agg == "sum":
                    self._db.execute(
                        "INSERT INTO stats VALUES (?, ?, ?, ?, ?) ON CONFLICT DO UPDATE "
                        "SET value = value + excluded.value", (view_id, key, online_id, prop_id, value))
                else:
                    self._db.execute(
                        "INSERT INTO stats VALUES (?, ?, ?, ?, ?) ON CONFLICT DO UPDATE "
                        "SET value = excluded.value", (view_id, key, online_id, prop_id, value))
                applied += 1
            self._db.commit()
        return applied

    def record_ranked(self, winner_id, loser_id):
        """Moves both players' TrueSkill after a decided ranked game."""
        with self._lock:
            def get(pid):
                row = self._db.execute("SELECT mu, sigma FROM skill WHERE online_id=?", (pid,)).fetchone()
                return row if row else (MU0, SIGMA0)
            (mw, sw), (ml, sl) = trueskill_1v1(get(winner_id), get(loser_id))
            for pid, mu, sigma in ((winner_id, mw, sw), (loser_id, ml, sl)):
                self._db.execute(
                    "INSERT INTO skill VALUES (?, ?, ?, 1) ON CONFLICT DO UPDATE "
                    "SET mu=excluded.mu, sigma=excluded.sigma, games=games+1", (pid, mu, sigma))
            self._db.commit()

    def skill(self, online_id):
        row = self._db.execute("SELECT mu, sigma, games FROM skill WHERE online_id=?",
                               (online_id,)).fetchone()
        mu, sigma, games = row if row else (MU0, SIGMA0, 0)
        return dict(mu=mu, sigma=sigma, games=games, skill=displayed_skill(mu, sigma) if games else 0)

    def query(self, view_id, start=1, count=10, pivot=None, now=None):
        """Rows of a view ranked by its sort property (highest first).

        start is a 1-based rank; with pivot (an online ID) the page is centred on that player.
        Returns (total_rows, rows), each row {rank, online_id, gamertag, rating, columns, skill}.
        """
        if view_id in (TRUESKILL_VIEW, TRUESKILL_VIEW + 1):
            return self._query_skill(start, count, pivot)
        view = VIEWS.get(view_id)
        if not view:
            return 0, []
        key = period_key(view["period"], now)
        sort = view["sort"]
        with self._lock:
            ranked = self._db.execute(
                "SELECT s.online_id, s.value, p.gamertag FROM stats s JOIN players p USING (online_id) "
                "WHERE s.view_id=? AND s.period=? AND s.prop_id=? ORDER BY s.value DESC, s.online_id",
                (view_id, key, sort)).fetchall()
            total = len(ranked)
            if pivot is not None:
                ids = [r[0] for r in ranked]
                if pivot in ids:
                    start = max(1, ids.index(pivot) + 1 - count // 2)
            start = max(1, start)
            page = ranked[start - 1:start - 1 + count]
            rows = []
            for i, (oid, rating, gamertag) in enumerate(page):
                values = dict(self._db.execute(
                    "SELECT prop_id, value FROM stats WHERE view_id=? AND period=? AND online_id=?",
                    (view_id, key, oid)).fetchall())
                columns = {col: values.get(prop, 0) for col, prop in view["columns"].items()}
                rows.append(dict(rank=start + i, online_id=oid, gamertag=gamertag, rating=rating,
                                 columns=columns, skill=self.skill(oid)["skill"]))
        return total, rows

    def _query_skill(self, start, count, pivot):
        """The TrueSkill view: players with ranked games, by displayed skill (then mu)."""
        with self._lock:
            ranked = self._db.execute(
                "SELECT k.online_id, k.mu, k.sigma, k.games, p.gamertag FROM skill k "
                "JOIN players p USING (online_id) WHERE k.games > 0").fetchall()
        ranked.sort(key=lambda r: (-displayed_skill(r[1], r[2]), -r[1], r[0]))
        total = len(ranked)
        if pivot is not None:
            ids = [r[0] for r in ranked]
            if pivot in ids:
                start = max(1, ids.index(pivot) + 1 - count // 2)
        start = max(1, start)
        rows = []
        for i, (oid, mu, sigma, games, gamertag) in enumerate(ranked[start - 1:start - 1 + count]):
            skill = displayed_skill(mu, sigma)
            rows.append(dict(rank=start + i, online_id=oid, gamertag=gamertag, rating=skill,
                             columns={SKILL_COLUMN: skill, GAMES_COLUMN: games,
                                      MU_COLUMN: round(mu), SIGMA_COLUMN: round(sigma)},
                             skill=skill))
        return total, rows
