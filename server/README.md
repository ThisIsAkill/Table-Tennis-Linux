# Table Tennis online server

`ttlink_server.py` lets players of the Linux port play System Link matches over the internet. Players connected to the same room form one virtual LAN:

- The game's own System Link traffic is used unchanged. Its match search and announce broadcasts go through the server to everyone in the room, so **ONLINE > PLAYER MATCH > QUICK MATCH / CREATE MATCH** work exactly like on a LAN.
- Match traffic goes straight between the two players once the server has introduced them (UDP hole punching). When that fails (strict NATs, some mobile networks), it is relayed through the server.
- Rooms: one public room (PLAY ANYONE), and private rooms with a six-character code (PLAY WITH A FRIEND). The codes avoid look-alike characters (no 0/O or 1/I/L).

- Nearby matchmaking in the public room: a search first only reaches players close to you, and widens the longer you keep searching (see below). Private rooms are never filtered.

No accounts and no stored data: while a player is connected the server holds their player name, room and IP address in memory, and forgets them when they leave or time out.

## Requirements

- A small Linux VM or VPS with a **public IPv4 address** (1 vCPU and 512 MB are plenty; a relayed match uses about 50-100 kbit/s per player).
- Python 3.8 or newer (standard library only). Debian, Ubuntu and Fedora ship it.
- One port, **27360**, open for **UDP** (game traffic) and **TCP** (status page).

## Deploy

On the VM, as root:

```sh
mkdir -p /opt/ttlink
cp ttlink_server.py /opt/ttlink/
cp ttlink.service /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now ttlink
```

Open the port in the VM's firewall and in the cloud provider's firewall or security group:

```sh
ufw allow 27360/udp && ufw allow 27360/tcp            # Ubuntu/Debian with ufw
# or: firewall-cmd --permanent --add-port=27360/udp --add-port=27360/tcp && firewall-cmd --reload
```

Check it:

```sh
systemctl status ttlink
journalctl -u ttlink -f                  # joins, leaves, time-outs
curl http://SERVER:27360/status          # {"players_online": ..., "public_room_players": ...}
```

A DNS name for the server (for example `tt.example.org`) is easier to keep stable than an IP address.

To run it by hand, for a test: `python3 ttlink_server.py --port 27360`.

## Point the game at it

Add the server to `tt-recomp.ini` next to the game (later releases can have it built in):

```ini
server=tt.example.org
```

`server=host:port` uses another port. Then choose **ONLINE** in the main menu. PC SETTINGS shows the server and the connection status under ONLINE.

## Limits against abuse

- Traffic is relayed only between players in the same room. The server is not an open relay.
- Each client gets a budget of 400 packets/s and 256 KB/s, and 20 broadcasts/s. Packets larger than 1400 bytes are dropped.
- At most 8 clients per IP address, 250 in the public room, 16 in a private room and 2000 in total (constants at the top of the file).
- Clients that send nothing for 30 s are dropped. The game sends a keepalive every 3 s.
- Join requests are limited to 5 per second per IP address.

## Nearby matchmaking

The game was made for a LAN, so in the public room the server tries to pair players who are close to each other. It knows two rough things about every player, and nothing more precise:

- the player's round trip to the server, which the game measures and reports with every keepalive;
- the player's UTC offset (time zone), sent once when joining.

The estimated round trip between two players is the smaller of (a) the sum of their round trips to the server and (b) 30 ms plus 25 ms per hour of time zone difference. (a) is an upper bound and good for players on opposite sides of the server; (b) catches two players who are close to each other but far from the server.

A match search in the game is a series of broadcasts. The server passes a broadcast from one public-room player to another only when their estimated round trip is within the larger of their two search radii. A player's radius grows with how long they have been searching (a "search streak" starts with the first broadcast after a minute without one):

| Searching for | Radius |
|---|---|
| under 6 s | 100 ms |
| under 20 s | 200 ms |
| under 40 s | 400 ms |
| longer | everyone |

In practice: the first QUICK MATCH only finds nearby players. If it finds nothing, answering No to "create a match?" and searching again within a minute looks further away, and the third or fourth search sees everyone. Players who are not searching (a host waiting in a lobby) are reached by any searcher whose radius covers them. The constants are at the top of `ttlink_server.py`. The game's ONLINE prompt shows how many public-room players the server counts as near you (within 150 ms), and `/status` lists the public room by time zone.

## Latency

The game was built for a LAN: every hit waits for the other machine, so the round trip between the two players is felt on every action. Between players in the same region (under about 50 ms) a match plays well. Across continents (300 ms and more) every action is visibly delayed. A direct connection (shown in PC SETTINGS > ONLINE STATUS) is always faster than one relayed through the server, unless the server is between the two players. A server in the middle of where most players are is the best choice.

## Protocol

UDP messages start with `TTV1` and a type byte; fields are big-endian.

| Type | Direction | Contents |
|---|---|---|
| 1 HELLO | client to server | version, nonce, name, room code (empty = public), local address and port, UTC offset in minutes |
| 2 WELCOME | server to client | client id, room address (10.88.0.x), token, room, public address and port as seen by the server |
| 3 MEMBERS | server to client | the other players: id, room address, token, public and local endpoint, name |
| 4 KEEPALIVE / 5 STATUS | both | keepalive: the client's round trip to the server; status: players in the room, online, rooms, nearby players |
| 6 DATA / 7 RELAYED | client to server / server to client | datagram for one room address or the whole room (broadcast) |
| 8 BYE | client to server | leaving |
| 9 PUNCH / 10 PUNCH_ACK | between players | opens the direct path; carries the sender's id and token |
| 11 DIRECT | between players | datagram sent straight to the other player |

Datagrams to port 0 (in DATA/RELAYED or DIRECT) are the clients' connection-quality pings: kind (1 ping, 2 pong), sequence number, timestamp. They run once a second over the path the match uses, so the round trip and loss the game shows are the ones the match gets.
| 12 ERROR | server to client | version mismatch, room full, invalid code, limit reached |

The game side is in the SDK patches `project/sdk-patches/0010-online-play.patch` and `0012-online-quality.patch` (`OnlineClient` in `src/system/xsocket.cpp`).

For tests, the game can simulate a distant player without root: `--online_test_delay_ms=300` delays every packet it sends, `--online_test_loss_percent=5` drops some, and `--online_utc_offset=330` reports another time zone.

## Not included: ranked matches and leaderboards

RANKED MATCH, TABLE TENNIS NETWORK and LEADERBOARDS were Xbox LIVE services. Bringing them back would need the server to take over what LIVE did:

- player identity that lasts between sessions (an account, or a key kept by the game);
- arbitration: both machines report the result of a ranked match and the server settles disagreements and drop-outs;
- a stats store with the game's leaderboard views (the game reads and writes them with XUserReadStats and XSessionWriteStats, which the SDK would forward to the server);
- some protection against faked results.

That is a larger project with stored personal data, so it is left out for now.
