# ConnectX web gateway

The front door that lets **web browsers** play on a ConnectX server.

A browser cannot open a TCP connection (it is not allowed to: all it can open is HTTP, WebSocket and a few other
things), and TCP is what every client of the server speaks: the CLI, the GUI, the computer players. The gateway is
the adapter. It listens for WebSockets and gives each browser a TCP connection of its own to the game server,
carrying the messages from one to the other.

```
browser ──wss──► nginx (TLS) ──ws──► connectx-gateway ──TCP──► connectx-server
 (a page)         :443                 127.0.0.1:8081           127.0.0.1:8080
```

* **The game server is not touched**: to it a browser is a client like any other, one that happens to connect
  from the same machine. It does not know the gateway exists, and everything it offers (lobby, rooms, quick match,
  any board, up to four players) is available from the browser.
* **The gateway holds no game.** Whatever is in a game lives in the server. The gateway keeps, for each browser, its
  two connections, a few flags and nothing else; it can be restarted at any time (the players lose their games, as
  they would if they closed the page).
* **It is the one component exposed to the internet**, so it does not trust the browser: see
  [what it checks](#what-the-gateway-does-to-the-messages).

## Try it

```bash
cmake -S . -B build && cmake --build build -j       # the gateway is built unless -DBUILD_GATEWAY=OFF
cd build/bin
./connectx-server --bind 127.0.0.1 &                # the game server, on port 8080
./connectx-gateway -v                               # the gateway, on 127.0.0.1:8081
```

Then, in the console of any web page (the gateway accepts any page unless it is told which ones with `--origin`):

```js
const ws = new WebSocket("ws://127.0.0.1:8081/connectx/ws");
ws.onmessage = (event) => console.log(event.data);
ws.onopen = () => ws.send(JSON.stringify({ type: 1, payload: { player_name: "Me" } }));
```

### Options

| Option | Default | |
|---|---|---|
| `-p`, `--port` | `8081` | port to listen on |
| `-b`, `--bind` | `127.0.0.1` | address of the network interface to listen on. The default is the machine itself: the reverse proxy is the one that talks to the world |
| `--server-host`, `--server-port` | `127.0.0.1`, `8080` | the game server |
| `--origin` | any page | a page that may connect, as the browser names it (`https://bocchio.dev`); repeat for more |
| `--max-connections` | `100` | browsers connected at once |
| `--max-per-address` | `6` | connections from one address |
| `--max-rooms` | `30` | games created through the gateway and still open |
| `--max-computers` | `4` | computer players at once; `0` switches them off |
| `-v`, `--verbose` | | log what every connection does |

The computer players are the AI module of this repository (`BUILD_AI`, on by default). The strongest levels use the
pretrained network, which is looked up in `models/` next to the executable; without it they fall back on a Monte Carlo
search, a little weaker. If the gateway is built without the AI module there is simply no computer to choose.

## The protocol, for the people who write a web client

One WebSocket **text** message is one message of the [game protocol](../protocol): the same JSON envelope,
`{"type": N, "payload": {...}}`, the same message types and fields. A message is at most 8192 bytes, as in the server.
What the gateway changes is what a browser may *do*:

| Browser sends | What happens |
|---|---|
| `REQ_CONNECT` (`0x01`) `{player_name}` | forwarded, once per connection. The name is cleaned up (no control characters, at most 32 bytes; `Guest` if nothing is left). The answer comes back with `session_token: ""`: **the token never leaves the gateway**, which puts it in every request itself |
| `REQ_LIST_GAMES` (`0x22`) | forwarded. The server does not push the lobby: ask again every few seconds |
| `REQ_CREATE_GAME` (`0x20`) `{name, config}` | the shape is checked (4–20 rows and columns, 2–4 players, connect length 3 or more that fits the board); then forwarded. **The creator joins at once**: the browser gets `RES_CREATE_GAME` followed by `RES_JOIN_GAME` without asking for it (the server forgets a game when its last player leaves, but never one that nobody joined) |
| `REQ_JOIN_GAME` (`0x24`) `{game_id}` | forwarded. `0` is the quick match: any game that is waiting for players, or a new standard one |
| `MAKE_MOVE` (`0x10`) `{column}` | forwarded, if the browser is in a game |
| `HEARTBEAT` (`0x04`) | answered by the gateway itself |
| `DISCONNECT` (`0x03`) | the gateway closes the connection |
| `REQ_COMPUTER_LEVELS` (`0x30`) | answered by the gateway: `RES_COMPUTER_LEVELS` (`0x31`) `{available, levels: [{level, name, summary}]}` |
| `REQ_ADD_COMPUTER` (`0x32`) `{level}` | the computer of that level joins the game the browser is in: `RES_ADD_COMPUTER` (`0x33`) `{success, message, name}` |
| anything else | `ERROR` (`0xFF`) `{error_code: 1}`: the message is not passed on |

Everything the server sends (`RES_*`, `MOVE_RESULT`, `GAME_STATE_UPDATE`, `ERROR`) goes to the browser as it is.

* **One game per connection**: a second `REQ_CREATE_GAME` or `REQ_JOIN_GAME` is refused with the gateway's own error
  `204`. There is no message to leave a game, in the game protocol: **leaving is closing the connection** (the
  server takes the player out of the game, and a game left without players disappears from the lobby). To play
  again, open a new WebSocket.
* `205` is the error for *too many open rooms*: the limit of `--max-rooms` is reached.
* A computer plays two-player games on boards that fit the AI module (up to 64 bits, i.e. `columns × (rows + 1) ≤ 64`);
  `RES_ADD_COMPUTER` explains the refusal when it cannot. One computer for each visitor, and no more than
  `--max-computers` at once.
* The board of `GAME_STATE_UPDATE` is `board[row][column]`, row `0` at the top, `0` for an empty cell and the id of the
  player (`1`, `2`...) otherwise.

### Closing

The gateway ends a connection with a WebSocket Close frame, and the code says why:

| Code | When |
|---|---|
| `1000` | the browser asked to leave |
| `1001` | shutting down, a browser that does not answer the pings (every 25 s, give up after 75 s) or that does nothing for 30 minutes |
| `1002`, `1003`, `1007`, `1009` | the browser broke the WebSocket rules: protocol error, binary data, text that is not UTF-8, a message that is too long |
| `1008` | too many messages: a burst of 30 is fine, then 15 a second |
| `1011` | the game server closed the connection |
| `1013` | the game server is not there (the browser first gets an `ERROR` with code `100`) |

Before the WebSocket is open, the gateway answers the HTTP request with `400` (not a WebSocket handshake), `403` (with
`--origin`: a page that is not in the list, or no `Origin` at all, which a browser always sends), `404` (wrong path: it
is `/connectx/ws`, or `/`), `426` (WebSocket version other than 13), `429` (too many connections from the address) or
`503` (full).

## What the gateway does to the messages

The browser is not trusted: anybody can open a WebSocket to a public gateway with a program of their own and say
anything. The gatekeeper (`Gatekeeper`) stands between the browser and its connection to the server:

* the **token** stays in the gateway, the requests that reach the server are *built* by the gateway from the fields it
  has checked, not forwarded: whatever else the browser put in a message is dropped;
* names and board shapes are validated before the server sees them, and the types that a browser may send are a short
  list (the table above);
* **limits** keep one visitor, or one program that pretends to be many, from filling the server: connections (in all,
  and per address), open rooms, computers, message rate. The address of a visitor is the one that the reverse proxy
  writes in `X-Forwarded-For` / `X-Real-IP`, believed only when the proxy is on this very machine;
* the `Origin` header keeps *pages of other websites* from using the gateway (a visitor's browser would do it for
  them). It is no authentication: a program can write any `Origin` it likes, and it is the limits that protect the
  gateway from those.

## Deploying it

Everything on one machine (say, a VPS), the gateway and the server listening on the machine itself and **only nginx
facing the internet**. The `wss://` address is mandatory for a page that is served over `https`, and TLS is a job for
nginx: the certificate you already have for the site is the one to use.

Run the two programs as services, **without** `--daemon` (systemd does that job). In `/etc/systemd/system/`:

```ini
# connectx-server.service
[Unit]
Description=ConnectX game server
After=network.target

[Service]
User=connectx
WorkingDirectory=/opt/connectx
ExecStart=/opt/connectx/connectx-server --bind 127.0.0.1 --port 8080
Restart=on-failure
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
PrivateTmp=true

[Install]
WantedBy=multi-user.target
```

```ini
# connectx-gateway.service
[Unit]
Description=ConnectX web gateway
After=connectx-server.service
Requires=connectx-server.service

[Service]
User=connectx
WorkingDirectory=/opt/connectx
ExecStart=/opt/connectx/connectx-gateway --port 8081 --origin https://bocchio.dev
Restart=on-failure
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
PrivateTmp=true

[Install]
WantedBy=multi-user.target
```

(The executables are in `/opt/connectx`, with the `models/` folder next to them if you want the best computer.)
Then, in the `server` block of the site that has the certificate:

```nginx
location /connectx/ws {
    proxy_pass http://127.0.0.1:8081;

    proxy_http_version 1.1;
    proxy_set_header Upgrade $http_upgrade;
    proxy_set_header Connection "upgrade";
    proxy_set_header Host $host;

    # `set`, not `add`: a visitor must not be able to invent the address the gateway counts him by
    proxy_set_header X-Forwarded-For $remote_addr;
    proxy_set_header X-Real-IP $remote_addr;

    # The gateway pings every 25 seconds: the connection is never silent for longer than that
    proxy_read_timeout 1h;
    proxy_send_timeout 1h;
}
```

If another proxy (a CDN, a load balancer) stands in front of nginx, `$remote_addr` is its address: use nginx's `realip`
module to recover the visitor's. The ports `8080` and `8081` stay closed in the firewall: nothing but nginx needs to
reach them. To follow what happens: `journalctl -u connectx-gateway -f` (add `-v` to the command for more).

## Tests

```bash
cmake -S . -B build -DBUILD_TESTS=ON && cmake --build build -j
cd build && ctest -R gateway --output-on-failure
```

| Test | What it covers |
|---|---|
| `gateway_test_websocket` | the WebSocket protocol: SHA-1 and Base64 against the standard vectors, the handshake, frames (masking, lengths, fragmentation, control frames), every way a peer can break the rules |
| `gateway_test_gatekeeper` | what a browser may send and what it gets back, without any socket: token hiding, validation, one game per connection, rooms, rate limit |
| `gateway_test_gateway` | the whole thing over real sockets, with a real game server in the process: handshake rules, a game between two browsers, a game against the computer, limits, abuse, a game server that is not there |

## The code

| File | |
|---|---|
| [`websocket`](include/gateway/websocket.hpp) | RFC 6455 from scratch: handshake, frame encoder, incremental frame reader. No network, no threads |
| [`web_connection`](include/gateway/web_connection.hpp) | a socket that speaks WebSocket: pings answered, orderly closing |
| [`gatekeeper`](include/gateway/gatekeeper.hpp) | the doorman of one browser (see above): strings in, strings out, easy to test |
| [`session`](include/gateway/session.hpp) | the two connections of one browser, and the two threads that carry the messages between them |
| [`computer`](include/gateway/computer.hpp) | the computer opponents, on top of the AI module's `BotClient` |
| [`gateway`](include/gateway/gateway.hpp) | listening, the Origin check, the limits |
| [`app/main.cpp`](app/main.cpp) | command line, signals |
