# Table Tennis online server

`ttlink_server.py` lets players of the Linux port play System Link matches over the internet. Players connected to the same room form one virtual LAN:

- The game's own System Link traffic is used unchanged. Its match search and announce broadcasts go through the server to everyone in the room, so **ONLINE > PLAYER MATCH > QUICK MATCH / CREATE MATCH** work exactly like on a LAN.
- Match traffic goes straight between the two players once the server has introduced them (UDP hole punching). When that fails (strict NATs, some mobile networks), it is relayed through the server.
- Rooms: one public room (PLAY ANYONE), and private rooms with a six-character code (PLAY WITH A FRIEND). The codes avoid look-alike characters (no 0/O or 1/I/L).

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

## Latency

The game was built for a LAN: every hit waits for the other machine, so the round trip between the two players is felt on every action. Between players in the same region (under about 50 ms) a match plays well. Across continents (300 ms and more) every action is visibly delayed. A direct connection (shown in PC SETTINGS > ONLINE STATUS) is always faster than one relayed through the server, unless the server is between the two players. A server in the middle of where most players are is the best choice.

## Protocol

UDP messages start with `TTV1` and a type byte; fields are big-endian.

| Type | Direction | Contents |
|---|---|---|
| 1 HELLO | client to server | version, nonce, name, room code (empty = public), local address and port |
| 2 WELCOME | server to client | client id, room address (10.88.0.x), token, room, public address and port as seen by the server |
| 3 MEMBERS | server to client | the other players: id, room address, token, public and local endpoint, name |
| 4 KEEPALIVE / 5 STATUS | both | players in the room, online, rooms |
| 6 DATA / 7 RELAYED | client to server / server to client | datagram for one room address or the whole room (broadcast) |
| 8 BYE | client to server | leaving |
| 9 PUNCH / 10 PUNCH_ACK | between players | opens the direct path; carries the sender's id and token |
| 11 DIRECT | between players | datagram sent straight to the other player |
| 12 ERROR | server to client | version mismatch, room full, invalid code, limit reached |

The game side is in the SDK patch `project/sdk-patches/0010-online-play.patch` (`OnlineClient` in `src/system/xsocket.cpp`).

## Not included: ranked matches and leaderboards

RANKED MATCH, TABLE TENNIS NETWORK and LEADERBOARDS were Xbox LIVE services. Bringing them back would need the server to take over what LIVE did:

- player identity that lasts between sessions (an account, or a key kept by the game);
- arbitration: both machines report the result of a ranked match and the server settles disagreements and drop-outs;
- a stats store with the game's leaderboard views (the game reads and writes them with XUserReadStats and XSessionWriteStats, which the SDK would forward to the server);
- some protection against faked results.

That is a larger project with stored personal data, so it is left out for now.
