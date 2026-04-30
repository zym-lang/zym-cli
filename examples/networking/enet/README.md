# ENet examples

Each example is a single self-contained `.zym` file. The comment header at
the top of every script explains what it does, how to run it, and what
output to expect - read that first.

ENet sits on top of UDP and adds three things UDP itself doesn't have:
**reliability** (per-message), **ordering** (per-channel), and
**channels** (multiple independent message streams over the same
connection). The trade-off is wire-protocol compatibility: ENet endpoints
only talk to other ENet endpoints. A zym ENet client can talk to another
zym ENet server, or to any C/C++ application using the upstream `enet`
library, but **not** to a generic UDP service.

For the underlying API see [`docs/sockets.md`](../../../docs/sockets.md).

## Examples in recommended order

### 1. `loopback.zym` - smallest possible round-trip

Single-process: stands up a server on `127.0.0.1`, connects a client,
drives both sides through the handshake, and round-trips one reliable
record in each direction.

```
zym examples/networking/enet/loopback.zym
```

Concepts: `ENet.listen`, `ENet.connect` returning `{ host, peer }`,
non-blocking handshake driven by `host.service(timeoutMs)`,
`peer.send(buf, channel, mode)`, `host.flush()`.

### 2. `echo_server.zym` + `echo_client.zym` - canonical pair, all three modes

A server that listens forever and echoes every record back. A client that
sends each argv argument once - cycling channel and mode (`reliable`,
`unreliable`, `unsequenced`) per message - and prints the replies.

```
# Terminal A
zym examples/networking/enet/echo_server.zym -- 9100
# Terminal B
zym examples/networking/enet/echo_client.zym -- 127.0.0.1 9100 alpha bravo charlie
```

Concepts on top of (1): one `service()` loop drives all peers (no
`accept()` per peer), `connect`/`disconnect`/`receive` events arrive in
the same stream, mode is a per-send choice (not bound to a channel), the
server doesn't see the original send mode - if your protocol needs that
distinction, label messages yourself.

### 3. `broadcast_server.zym` + `broadcast_client.zym` - one-to-many fan-out

Multi-client chat hub. Server uses `host.broadcast(buf, channel, mode)` to
fan a single send out to every connected peer; clients register a display
name with a small `HELLO:<name>` handshake on top of raw records, then
exchange chat lines visible to everyone.

Run the server, then start two or three clients in different terminals:

```
# Terminal A
zym examples/networking/enet/broadcast_server.zym -- 9101
# Terminal B
zym examples/networking/enet/broadcast_client.zym -- 127.0.0.1 9101 alice "hi all"
# Terminal C
zym examples/networking/enet/broadcast_client.zym -- 127.0.0.1 9101 bob "hi alice"
```

Concepts on top of (2): `host.broadcast()` (no per-peer iteration),
synthesizing a tiny text framing protocol (`HELLO:` / `JOIN:` / `MSG:` /
`LEAVE:`) on top of raw records - ENet itself is content-agnostic.

### 4. `dtls_self_signed_server.zym` + `dtls_client.zym` - encrypted ENet

Same shape as the echo pair, but the connection is wrapped in DTLS. The
server mints an in-process self-signed RSA-2048 key + cert via the
`Crypto` native and listens with `tls: { key, cert }` in the opts map.
The client connects with `tls: { verify: false }` since the cert isn't
chained to a trusted CA.

```
# Terminal A
zym examples/networking/enet/dtls_self_signed_server.zym -- 9102
# Terminal B
zym examples/networking/enet/dtls_client.zym -- 127.0.0.1 9102 alpha bravo charlie
```

Concepts on top of (2): `opts.tls` on `ENet.listen` / `ENet.connect`
turns on DTLS; everything above the socket layer is unchanged. Each peer
goes through a DTLS cookie exchange + handshake before the `connect`
event is delivered. For real deployments use system CAs (omit `tls`
entirely on the client) or pin specific roots via `trustedRoots`, not
`verify: false`.

## Defaults

| Script                      | Default port | Default host    | Default channels | Default payload |
|-----------------------------|--------------|-----------------|------------------|-----------------|
| `loopback.zym`              | OS-assigned  | `127.0.0.1`     | 4                | n/a             |
| `echo_server.zym`           | `9100`       | `0.0.0.0`       | 4                | n/a             |
| `echo_client.zym`           | `9100`       | `127.0.0.1`     | 4                | `["ping"]`      |
| `broadcast_server.zym`      | `9101`       | `0.0.0.0`       | 4                | n/a             |
| `broadcast_client.zym`      | `9101`       | `127.0.0.1`     | 4                | `["hello"]`     |
| `dtls_self_signed_server.zym` | `9102`     | `0.0.0.0`       | 4                | n/a             |
| `dtls_client.zym`           | `9102`       | `127.0.0.1`     | 4                | `["ping"]`      |

## Common pitfalls

- **Forgetting to call `host.service()`.** ENet is non-blocking under the
  hood; nothing happens between calls. Even with no traffic to send or
  receive, both ends must service their host regularly to drive
  handshake retransmits, keepalives, and timeouts.
- **Not flushing.** `host.flush()` pushes pending sends out the wire
  immediately. Without it, sends are buffered until the next
  `service()`. Loopback latency in particular is noticeably better with
  an explicit flush.
- **Treating `unreliable`/`unsequenced` as guaranteed.** Only `reliable`
  is delivered or reported as failed; the other modes can silently be
  dropped (especially on a real, lossy link). Don't write tests that
  count exact reply counts in those modes.
- **Mixing ENet with non-ENet services.** ENet has its own framing on
  top of UDP. An ENet client cannot talk to a generic UDP service, even
  if the port matches. Use `UDP.bind` for that.
- **DTLS handshake feels slower.** This is normal; the cookie exchange
  is one extra round trip. Give the deadline a few seconds, not
  milliseconds.
