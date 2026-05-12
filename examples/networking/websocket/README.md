# WebSocket examples

Each example is a single self-contained `.zym` file. The comment header
at the top of every script explains what it does, how to run it, and
what output to expect - read that first.

WebSocket is a message-oriented protocol layered on top of TCP. Each
`send` / `sendText` call produces exactly one WebSocket frame; each
successful `recv` returns exactly one frame. Text frames travel as zym
strings (UTF-8 on the wire); binary frames travel as `Buffer`.
`wasStringPacket()` reports which kind the last received frame was.

For the underlying API see
[`docs/sockets.md`](../../../docs/cli/sockets.md).

## Examples in recommended order

### 1. `loopback.zym` - smallest possible round-trip

Single-process: stands up a TCP listener on `127.0.0.1`, accepts one
inbound connection, upgrades both ends with `WebSocket.connect` /
`WebSocket.accept`, drives the handshake to `"connected"`, and
round-trips one text frame and one binary frame in each direction.

```
zym examples/networking/websocket/loopback.zym
```

Concepts: `WebSocket.connect("ws://...")`, `WebSocket.accept(tcp)`,
non-blocking handshake driven by `sock.poll()` until
`status() == "connected"`, `sendText` vs `send`, `wasStringPacket()`.

### 2. `echo_server.zym` + `echo_client.zym` - canonical pair, multi-client

A server that listens forever and echoes every frame back on the same
kind it was received (text -> `sendText`, binary -> `send`). A client
that sends each argv argument as a text frame and prints the replies.
The server multiplexes many clients with a single `Sockets.waitAny`
loop.

```
# Terminal A
zym examples/networking/websocket/echo_server.zym -- 9200
# Terminal B
zym examples/networking/websocket/echo_client.zym -- 127.0.0.1 9200 alpha bravo charlie
```

Concepts on top of (1): `Sockets.waitAny` over a mix of a listening TCP
and many WebSocket socks, draining `sock.available()` frames per wake,
detecting `"closed"` / `"error"` and compacting the watch list.

### 3. `broadcast_server.zym` + `broadcast_client.zym` - one-to-many chat hub

Multi-client chat. The first frame each client sends is its nickname;
every subsequent frame is broadcast to every OTHER connected client as
`"<nick>: <msg>"`. The client sends each argv message, then listens
for other clients' chatter for a few seconds before disconnecting.

Run the server, then start two or three clients in different
terminals:

```
# Terminal A
zym examples/networking/websocket/broadcast_server.zym -- 9202
# Terminal B
zym examples/networking/websocket/broadcast_client.zym -- 127.0.0.1 9202 alice "hi all"
# Terminal C
zym examples/networking/websocket/broadcast_client.zym -- 127.0.0.1 9202 bob "hi alice"
```

Concepts on top of (2): a tiny in-band handshake on top of plain text
frames (first frame = nick), per-client state in a parallel map,
fan-out by iterating the connected-sock list. The framing layer above
WebSocket is up to you - WebSocket itself is just a message pipe.

### 4. `wss_self_signed_server.zym` + `wss_client.zym` - encrypted (`wss://`)

Same shape as the echo pair, but the connection is wrapped in TLS. The
server mints an in-process self-signed RSA-2048 key + cert via the
`Crypto` native, accepts a TCP, drives the TLS handshake to completion,
*then* hands the TLS sock to `WebSocket.accept`. The client connects
with `wss://...` plus `{ tls: { verify: false } }` since the cert isn't
chained to a trusted CA.

```
# Terminal A
zym examples/networking/websocket/wss_self_signed_server.zym -- 9201
# Terminal B
zym examples/networking/websocket/wss_client.zym -- 127.0.0.1 9201 hello world
```

Concepts on top of (2): URL scheme drives TLS (`wss://` on, `ws://`
off), `opts.tls` mirrors `TLS.connect`'s `opts`, the layered server
path is `TCP.accept` -> `TLS.accept(tcp, {key, cert}, 0)` ->
`WebSocket.accept(tls)`, and `{ verify: false }` is for demos only.
For real deployments use a CA-issued cert and the default
`verify=true`, or pin a specific root via `trustedRoots`.

## Defaults

| Script                         | Default port | Default host  | Default payload  |
|--------------------------------|--------------|---------------|------------------|
| `loopback.zym`                 | OS-assigned  | `127.0.0.1`   | n/a              |
| `echo_server.zym`              | `9200`       | `0.0.0.0`     | n/a              |
| `echo_client.zym`              | `9200`       | `127.0.0.1`   | `["ping"]`       |
| `broadcast_server.zym`         | `9202`       | `0.0.0.0`     | n/a              |
| `broadcast_client.zym`         | `9202`       | `127.0.0.1`   | `["hello"]` as nick `"anon"` |
| `wss_self_signed_server.zym`   | `9201`       | `0.0.0.0`     | n/a              |
| `wss_client.zym`               | `9201`       | `127.0.0.1`   | `["ping"]`       |

## Common pitfalls

- **Forgetting to call `sock.poll()`.** WebSocket is non-blocking under
  the hood. Both the handshake and frame parsing happen during
  `poll()` (and indirectly inside `recv(timeout)`). A script that
  never polls will stay in `"connecting"` forever.
- **Closing the underlying TCP / TLS sock too early.** On the server
  side, the TCP (or TLS) sock returned by `srv.accept` is what carries
  the bytes - keep it in scope until the WebSocket handle is closed.
  Closing the WebSocket does *not* automatically close the TCP / TLS
  sock; that's why every server example closes them in
  WebSocket -> TLS -> TCP order.
- **Treating `recv` as line-oriented.** Each successful `recv` returns
  exactly one frame; there is no line buffering, no implicit
  fragmentation, no partial reads to assemble. If you need a different
  message shape, build it on top of the frame stream yourself (see the
  broadcast pair for one in-band example).
- **Confusing text and binary frames.** `recv` returns a `Buffer`
  either way; `sock.wasStringPacket()` immediately afterwards tells
  you which kind it was. Echoing back on the wrong kind is a common
  bug in client/server mismatches.
- **Using `{ verify: false }` in production.** It disables hostname
  and certificate-chain validation entirely. Fine for demos against a
  self-signed cert (like `wss_self_signed_server.zym`); never for
  anything you actually care about.
