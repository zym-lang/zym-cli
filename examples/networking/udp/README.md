# UDP examples

Each example is a single self-contained `.zym` file. The comment header at
the top of every script explains what it does, how to run it, and what
output to expect - read that first.

For the underlying API see [`docs/sockets.md`](../../../docs/sockets.md).

UDP is fundamentally different from TCP:

- **No connection.** Either side can send first; there is no handshake.
- **Datagrams, not streams.** One `send()` produces exactly one packet;
  one `recv()` returns exactly one packet (whole or nothing).
- **Unreliable.** Datagrams can be dropped, reordered, or duplicated.
  On `127.0.0.1` loss is effectively zero, but real networks lose
  packets routinely - real UDP code retransmits, sequences, or
  doesn't care.
- **Source address is part of the recv result.** `udp.recv(...)`
  returns `{ data, host, port }`; you reply by `send(buf, host, port)`.

## Examples in recommended order

### 1. `loopback.zym` - smallest possible round-trip

Sends one datagram from one socket on `127.0.0.1` to another on
`127.0.0.1`, in a single process. Read this first.

```
zym examples/networking/udp/loopback.zym
```

Concepts: `UDP.bind`, `localPort`, `send`, `recv` returning a map with
the source address.

### 2. `echo_server.zym` + `echo_client.zym` - stateless echo over UDP

A server that bounces every datagram straight back to its sender. A
client that sends each argv argument as one datagram and prints the
reply for each.

```
# Terminal A
zym examples/networking/udp/echo_server.zym -- 9100

# Terminal B
zym examples/networking/udp/echo_client.zym -- 127.0.0.1 9100 hello world
```

Concepts on top of (1): a `recv(timeoutMs)` loop that wakes every
500ms (so Ctrl-C is responsive), the source address as the reply
target, lockstep `send`/`recv` on the client side. Notice that one
single bound socket on the server handles all clients - no per-source
state, no `accept`.

### 3. `multi_client_server.zym` + `multi_client_client.zym` - per-source state

A UDP server that gives every distinct sender (host:port pair) its
own peer handle, tracks a per-client message counter, and replies
with numbered ACKs. Run two clients in parallel and you'll see
each one gets its own independent counter.

```
# Terminal A
zym examples/networking/udp/multi_client_server.zym -- 9200

# Terminal B
zym examples/networking/udp/multi_client_client.zym -- 127.0.0.1 9200 hi there

# Terminal C, simultaneously
zym examples/networking/udp/multi_client_client.zym -- 127.0.0.1 9200 ping pong
```

Concepts on top of (2):

- `UDP.listen(host, port)` returns a server that demultiplexes
  datagrams **by source**. Each new `(host, port)` becomes its own
  accepted peer handle.
- `Sockets.waitAny([srv, ...peers], "read", timeoutMs)` waits on the
  server *and* every accepted peer in one call - the closest thing
  zym has to `select(2)`.
- Per-client state lives in a script-level map keyed by `host:port`.
  This is where you'd put session info, sequence numbers, auth
  tokens, game state, etc.

When to reach for this over `udp/echo_server.zym`'s shape: any time
the server has to *remember* something about each sender.

### 4. `dtls_self_signed_server.zym` + `dtls_client.zym` - encrypted UDP

A DTLS-flavored echo server using a freshly-minted self-signed cert,
plus a matching client that opts out of cert verification. DTLS is
"TLS for datagrams": the handshake produces a session key, and from
then on every `send` / `recv` is an encrypted record on top of UDP.

```
# Terminal A
zym examples/networking/udp/dtls_self_signed_server.zym -- 9684

# Terminal B
zym examples/networking/udp/dtls_client.zym -- 127.0.0.1 9684 alpha bravo
```

Concepts on top of (3):

- `DTLS.connect(host, port, timeoutMs, opts)` is the client factory.
  It binds an ephemeral UDP, runs the DTLS handshake (cookie
  exchange + key exchange), and returns a handle whose API mirrors
  UDP's: `send` / `recv` / `poll` / `close`. **No** `read` /
  `readLine` / `readAll` - DTLS preserves UDP's datagram boundaries.
- `DTLS.accept(udpServer, { key, cert }, timeoutMs)` drives the
  server-side handshake on top of `UDP.listen`'s per-source demux.
  It internally consumes the UDP server's pending datagrams to
  advance any in-flight handshake, and returns the connected DTLS
  peer once one finishes.
- `recv(timeoutMs)` on a DTLS sock returns a **Buffer** (not a
  `{ data, host, port }` map): post-handshake, DTLS is a
  point-to-point association, so the source is implicit.
- DTLS reuses the `Crypto` native's `generateRsa` /
  `generateSelfSignedCertificate` exactly the way TLS does (see
  `docs/crypto.md`); cross-format use of the same cert/key for both
  is deliberate.
- `{ verify: false }` is the same opt as TLS: skip cert + hostname
  checks. Only use against self-signed peers you control.

Worth noting: DTLS doesn't make UDP reliable. Application datagrams
can still be dropped; only the handshake is internally retransmitted.

## Defaults at a glance

| Script                       | argv[1] default | argv[2] default | argv[3+] default              |
|------------------------------|-----------------|-----------------|-------------------------------|
| `loopback.zym`               | (no args)       | -               | -                             |
| `echo_server.zym`            | port = 9100     | -               | -                             |
| `echo_client.zym`            | host = 127.0.0.1| port = 9100     | payload = `["ping"]`          |
| `multi_client_server.zym`    | port = 9200     | -               | -                             |
| `multi_client_client.zym`    | host = 127.0.0.1| port = 9200     | payload = `["hi", "there"]`   |
| `dtls_self_signed_server.zym`| port = 9684     | -               | -                             |
| `dtls_client.zym`            | host = 127.0.0.1| port = 9684     | payload = `["ping"]`          |

## Common pitfalls

- **Server seems to "miss" a datagram.** UDP is allowed to drop. On
  loopback this is rare but possible (e.g. send buffer overflow with
  bursty traffic). Real-world UDP code expects loss and either
  retransmits or accepts it.
- **`recv()` returns `"timeout"` even though the client just sent.**
  Either the datagram was actually dropped (rare on loopback), or the
  client and server are bound to different ports. Double-check
  `localPort()` on each side.
- **`multi_client_server.zym` shows "+ new client" twice for the same
  client.** This happens if the client `close()`s its UDP socket (or
  exits and the OS reclaims the port) and a new run picks a different
  source port. The server can't tell that's the "same person" - it
  just sees a new `(host, port)`. If you need stable client identity
  across reconnects, use a session token in your payload.
- **Port already in use.** Pick a different one, or pass `0` to let the
  OS choose; the server prints the actual port via `localPort()`.
