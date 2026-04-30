# TCP examples

Each example is a single self-contained `.zym` file. The comment header at
the top of every script explains what it does, how to run it, and what
output to expect - read that first.

For the underlying API see [`docs/sockets.md`](../../../docs/sockets.md).

## Examples in recommended order

### 1. `loopback.zym` - smallest possible round-trip

Stands up a server, connects to it, swaps one line in each direction, and
exits. Nothing more. Read this first.

```
zym examples/networking/tcp/loopback.zym
```

Concepts: `TCP.listen`, `TCP.connect`, `srv.accept`, `Buffer.fromString`,
`readLine`, `close`.

### 2. `echo_server.zym` + `echo_client.zym` - the canonical pair

A server that accepts connections forever and echoes every line back to
its sender. A client that opens a connection, sends each argv argument as
one line, and prints the reply.

Two terminals, or one shell with the server backgrounded:

```
# Terminal A
zym examples/networking/tcp/echo_server.zym -- 9000

# Terminal B
zym examples/networking/tcp/echo_client.zym -- 127.0.0.1 9000 hello world
```

Concepts on top of (1): an outer accept loop with a per-iteration timeout,
`peerAddress` for logging, `eof`/`closed` status strings, request/reply
lockstep on the client side.

### 3. `port_scanner.zym` - `timeoutMs` doing real work

Walks a port range on a host and prints which ports answered.

```
zym examples/networking/tcp/port_scanner.zym -- 127.0.0.1 1 1024
zym examples/networking/tcp/port_scanner.zym -- example.com 80 90 200
```

Concepts on top of (2): the `timeoutMs` argument on `TCP.connect` is the
single knob that decides how fast the scan completes versus how thorough
it is. Smaller = faster scan, more false-negatives on slow hosts. `null`
return = refused / unreachable / timed out (we don't try to distinguish).

### 4. `http_get.zym` - composing a real protocol on top of TCP

Speaks plain HTTP/1.0 by hand and prints the request, the response
headers, and the response body of any URL you point it at.

```
zym examples/networking/tcp/http_get.zym -- example.com
zym examples/networking/tcp/http_get.zym -- example.com 80 /
zym examples/networking/tcp/http_get.zym -- neverssl.com 80 /
```

Concepts on top of (3): explicit `\r\n` framing, `readAll(timeoutMs)`
accumulating until the peer closes (FIN), `split(str, "\r\n\r\n")` to
slice headers from body, `Buffer.toUtf8()` to convert response bytes to
text for parsing.

This is also the example that motivates *why* `TLS` exists: every byte
above is on the wire in cleartext. The `https_get.zym` example below
(7) is this same script but with `TCP.connect` replaced by
`TLS.connect`.

### 5. `chat_server.zym` + `chat_client.zym` - many clients on one server

A multi-client chat hub. The server uses `Sockets.waitAny` to multiplex
the listening socket plus every connected client; each client's first
line is its nickname, every subsequent line is broadcast to every
other connected client.

```
# Terminal A
zym examples/networking/tcp/chat_server.zym -- 9100

# Terminals B and C (one per user)
zym examples/networking/tcp/chat_client.zym -- 127.0.0.1 9100 alice hello world
zym examples/networking/tcp/chat_client.zym -- 127.0.0.1 9100 bob   hi-alice
```

Concepts on top of (4): `Sockets.waitAny([srv, c1, c2, ...], "read", ms)`
is the only practical way to multiplex many sockets in a
single-threaded script (the closest thing zym has to `select(2)`).
Per-client state lives in a parallel map keyed by an integer client
id. The chat protocol itself is implicit and line-framed: first line
= nickname, rest = messages.

### 6. `proxy.zym` - bidirectional TCP forwarding

A single-file TCP proxy. Listens on a local port, dials a configured
upstream for every accepted client, and shovels bytes between the two
sockets in both directions until either side closes.

```
# Terminal A: real upstream (the one we're forwarding TO)
zym examples/networking/tcp/echo_server.zym -- 9000

# Terminal B: the proxy
zym examples/networking/tcp/proxy.zym -- 9300 127.0.0.1 9000

# Terminal C: a client speaking to the proxy
zym examples/networking/tcp/echo_client.zym -- 127.0.0.1 9300 ping pong
```

Concepts on top of (5): `Sockets.waitAny` over **exactly two** sockets
is the kernel of a proxy / NAT / port-forwarder / sidecar. Pair it
with `readSome(n)` (which never waits and returns `null` when the
source has closed) to get "block until something is ready, then drain
everything". Half-close semantics are deliberately not modeled - as
soon as either side closes, both sides are torn down.

### 7. `https_get.zym` - the same script as (4) but encrypted

Identical to `http_get.zym` except `TCP.connect` is replaced by
`TLS.connect`. By default the connection verifies the peer's
certificate against the system trust store and checks the hostname,
so this works against any real HTTPS site without setup.

```
zym examples/networking/tcp/https_get.zym -- example.com
zym examples/networking/tcp/https_get.zym -- www.google.com 443 /
```

Concepts on top of (4): `TLS.connect(host, port, timeoutMs)` blocks
until handshake completes; default opts verify the cert + hostname;
once `connected`, the byte-stream API (`write` / `readAll` /
`readLine`) is identical to TCP - encryption is invisible above the
sock handle.

### 8. `tls_self_signed_server.zym` + `tls_client.zym` - bring your own cert

A TLS-flavored echo server using a freshly-minted self-signed cert
(via `Crypto.create()` + `generateRsa` + `generateSelfSignedCertificate`)
and a matching client that opts out of cert verification.

```
# Terminal A
zym examples/networking/tcp/tls_self_signed_server.zym -- 9443

# Terminal B
zym examples/networking/tcp/tls_client.zym -- 127.0.0.1 9443 alpha bravo
```

Concepts on top of (7): server-side handshake (`TLS.accept(tcp,
{key, cert}, 0)` + a poll loop driving it to `connected`); the
`{ verify: false }` client opt for self-signed peers; same
byte-stream API once the handshake is done. Cross-references the
`Crypto` native (see `docs/crypto.md`).

## Defaults at a glance

| Script              | argv[1] default | argv[2] default | argv[3+] default      |
|---------------------|-----------------|-----------------|-----------------------|
| `loopback.zym`      | (no args)       | -               | -                     |
| `echo_server.zym`   | port = 9000     | -               | -                     |
| `echo_client.zym`   | host = 127.0.0.1| port = 9000     | payload = `["ping"]`  |
| `port_scanner.zym`  | host = 127.0.0.1| first = 1       | last = 1024, ms = 200 |
| `http_get.zym`      | host = example.com | port = 80    | path = /              |
| `chat_server.zym`   | port = 9100     | -               | -                     |
| `chat_client.zym`   | host = 127.0.0.1| port = 9100     | nick = anon, lines... |
| `proxy.zym`         | listen = 9300   | up = 127.0.0.1  | up port = 9000        |
| `https_get.zym`     | host = example.com | port = 443   | path = /              |
| `tls_self_signed_server.zym` | port = 9443 | -          | -                     |
| `tls_client.zym`    | host = 127.0.0.1| port = 9443     | lines (default `["ping"]`)|

## Common pitfalls

- **Servers print "could not listen on port N (port already in use?)".**
  Pick a different port, or pass `0` to let the OS choose one (the server
  prints the chosen port via `localPort()`).
- **`echo_client.zym` reports "could not connect".** Make sure the server
  is actually running and bound to the same port. `port_scanner.zym -- 127.0.0.1 8999 9001`
  is a quick way to confirm.
- **HTTP requests against an HTTPS-only host return nothing.** `http_get`
  only speaks plain HTTP/1.0; modern sites usually redirect to HTTPS. Use
  `neverssl.com` or `example.com` for a guaranteed-plaintext target.
