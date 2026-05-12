# Networking (`IP`, `TCP`, `UDP`, `TLS`, `DTLS`, `Sockets`, `ENet`, `WebSocket`)

Cross-platform TCP / UDP / TLS / DTLS / ENet / WebSocket sockets and
DNS, exposed as a layered set of zym-flavored namespaces:

- `IP` — DNS resolution and local-interface enumeration.
- `TCP` — reliable byte streams; client `connect` + listening server.
- `UDP` — unreliable datagrams; bound socket with `send` / `recv`, plus `UDP.listen` for per-source server-side demultiplexing.
- `TLS` — encrypted TCP client and server over `X509Certificate` / `CryptoKey` from `Crypto`.
- `DTLS` — encrypted UDP (datagram TLS) client and server, layered over `UDP` + `Crypto` types.
- `Sockets` — `Sockets.waitAny(handles, mode, timeoutMs)` for multi-socket readiness (TCP, UDP, TLS, DTLS, WebSocket, and server handles).
- `ENet` — UDP with reliable + ordered + channels, optionally wrapped in DTLS.
- `WebSocket` — RFC 6455 WebSocket client and server, optionally over TLS (`wss://`).

---

## Conventions

The networking natives share a small set of cross-cutting rules:

- **Status strings.** Anywhere an operation can succeed *or* be in a
  recoverable not-ready state, the result is one of the standard
  status strings: `"ok"`, `"busy"`, `"timeout"`, `"eof"`, `"closed"`,
  `"error"`. Bad argument types still raise a Zym runtime error of
  the form `IP.method(args) ...`.
- **`Buffer` is the byte currency.** All byte payloads exchanged with
  TCP/UDP/TLS are `Buffer` instances (see [buffer.md](buffer.md)).
- **`null` on lookup miss.** Resolution failure (NXDOMAIN, garbage
  hostname, no such handle) is `null`; an empty result that is *not*
  a failure (no addresses returned for a host that resolved cleanly,
  no local interfaces) is an empty list.
- **Synchronous everywhere.** No async runtime, promises, callbacks,
  or event loop. Operations that *can* take time take an optional
  `timeoutMs` argument; non-blocking variants have explicit `*Some`
  names.

---

## `IP`

DNS lookups and local interface enumeration. The `IP` global is a
namespace of static functions; there is no instance type.

### Methods

| Method | Returns | Notes |
| --- | --- | --- |
| `IP.resolve(host)` | string \| `null` | Synchronous DNS lookup. Returns the first address (IPv4 or IPv6) the system resolver yields, or `null` on NXDOMAIN / lookup error. |
| `IP.resolveAll(host)` | list of string | All addresses for `host`. Empty list on NXDOMAIN / lookup error. Order is whatever the resolver returned. |
| `IP.localAddresses()` | list of string | Textual IPs assigned to local interfaces. Includes loopback (`127.0.0.1` / `::1`) and any virtual interface (`docker0`, etc.). |

### Address format

- **IPv4.** Dotted-quad: `"192.168.1.10"`.
- **IPv6.** Colon-hex: `"fe80::1"` for shortened forms, `"0:0:0:0:0:0:0:1"`
  for the fully-expanded form. The exact textual representation is
  whatever the engine's `IPAddress -> String` formatter produces;
  scripts should treat the value as opaque and feed it back into
  `TCP` / `UDP` rather than parsing it.
- **`localhost`.** On dual-stack hosts, `IP.resolve("localhost")` may
  return either `"127.0.0.1"` or `"::1"` depending on the resolver
  configuration. Use `IP.resolveAll("localhost")` to see all entries.

### Examples

```zym
print(IP.resolve("example.com"))
//  "93.184.216.34"  (or null on a network without DNS)

for addr in IP.resolveAll("dual-stack.example") {
    print(addr)
}
//  "203.0.113.10"
//  "2001:db8::10"

for ip in IP.localAddresses() {
    print(ip)
}
//  "127.0.0.1"
//  "::1"
//  "192.168.1.42"
//  "172.17.0.1"
//  ...
```

### Notes

- Resolution is **synchronous**. Long DNS lookups will block the
  calling script until the OS resolver responds (or hits its own
  timeout, typically 5–30s depending on `/etc/resolv.conf`). For a
  server-style script that wants to keep handling traffic during DNS,
  resolve in advance or use the OS resolver's caching.
- Literal addresses (`"127.0.0.1"`, `"::1"`) round-trip through
  `IP.resolve` cleanly — the engine treats them as already-resolved
  and returns them unchanged.
- `localAddresses()` enumerates *all* interfaces, including link-local
  (`fe80::`) and virtual ones. Filter the result list yourself if
  you only want, say, the first non-loopback IPv4 address.

---

## `TCP`

Reliable byte streams. Two globals (`TCP.connect` for clients,
`TCP.listen` for servers); both return instance handles whose methods
are documented below.

The connection model is **non-blocking under the hood** with
`timeoutMs`-driven helpers layered on top:

- `timeoutMs == -1` — block forever (default).
- `timeoutMs == 0` — return immediately; do not wait.
- `timeoutMs > 0` — wait up to that many milliseconds.

### `TCP` statics

| Method | Returns | Notes |
| --- | --- | --- |
| `TCP.connect(host, port)` | sock \| `null` | Resolves `host` via `IP`, opens a TCP connection. Blocks forever until connected, refused, or errored. |
| `TCP.connect(host, port, timeoutMs)` | sock \| `null` | Same, but bounded. `timeoutMs == 0` returns immediately with the partially-connected sock (`status() == "connecting"`); the caller drives `poll()` until ready. |
| `TCP.listen(host, port)` | server \| `null` | Bind + listen on `host:port`. `host == ""` or `"*"` binds all interfaces. `port == 0` lets the OS assign one (read it back via `server.localPort()`). Returns `null` on `EADDRINUSE` / permission errors. |

### Sock instance methods

| Method | Returns | Notes |
| --- | --- | --- |
| `sock.status()` | string | One of `"none"`, `"connecting"`, `"connected"`, `"error"`. Does not advance state. |
| `sock.poll()` | string | Drives one round of state advancement (handshake progress, EOF detection) and returns the new status. Useful when you want to react to disconnects without reading. |
| `sock.available()` | number | Bytes immediately readable. Polls before reporting. |
| `sock.read(n)` | Buffer \| status | Read **exactly** `n` bytes; blocks forever. |
| `sock.read(n, timeoutMs)` | Buffer \| status | Read **exactly** `n` bytes within the deadline. On partial arrival before the deadline, returns `"timeout"` (the bytes that did arrive are lost — use `readSome` for partial-friendly reads). On peer close, returns `"eof"`. |
| `sock.readSome(n)` | Buffer \| `null` | Non-blocking read. Returns whatever is immediately available, up to `n` bytes; the result may be empty (length 0). On error / not-connected returns `null`. |
| `sock.readLine()` / `sock.readLine(timeoutMs)` | string \| status | Read up to and including the next `\n`, returning the line **without** the terminator. Strips both `\r\n` and `\n`. Returns `"eof"` if the connection closes before a newline arrives. Any bytes that arrive *after* the newline in the same `available()` window are discarded. |
| `sock.readAll()` / `sock.readAll(timeoutMs)` | Buffer \| status | Read until peer EOF (or deadline). Returns the accumulated payload as a Buffer; `"timeout"` if the deadline hits before EOF; `"error"` on connection error. |
| `sock.write(buf)` / `sock.write(buf, timeoutMs)` | status | Send the **entire** payload (or fail). Returns `"ok"`, `"timeout"`, `"closed"`, or `"error"`. Empty buffers always return `"ok"`. |
| `sock.writeSome(buf)` | map | Non-blocking. Returns `{ sent: number, status: "ok" \| "closed" \| "error" }`. Use this for back-pressure-aware sending. |
| `sock.setNoDelay(b)` | `null` | Toggle `TCP_NODELAY` (Nagle's algorithm). |
| `sock.localAddress()` | `{ host, port }` \| `null` | The local end of the connection. Note: `host` is reported as the wildcard form (engine-side limitation); `port` is the actual local port. |
| `sock.peerAddress()` | `{ host, port }` \| `null` | The remote end. |
| `sock.close()` | `null` | Disconnect. Idempotent. After close, `status()` returns `"none"` and read/write return `"closed"`. |

### Server instance methods

| Method | Returns | Notes |
| --- | --- | --- |
| `srv.accept()` | sock \| `null` | Block forever until a client connects, then take the connection. Returns `null` if the server has been stopped. |
| `srv.accept(timeoutMs)` | sock \| `null` | Same, bounded. `null` on timeout *or* on shutdown. |
| `srv.localPort()` | number | The actually-bound port (useful when `listen` was passed `0`). |
| `srv.close()` | `null` | Stop listening. Already-accepted client sockets are unaffected. Idempotent. |

### Examples

```zym
// Client: connect, exchange one line, close.
var c = TCP.connect("example.com", 80, 5000)
c.write(Buffer.fromString("GET / HTTP/1.0\r\nHost: example.com\r\n\r\n"), 5000)
var status_line = c.readLine(5000)
print(status_line)
c.close()

// Server: accept one connection, echo a line.
var srv = TCP.listen("127.0.0.1", 0)
print("listening on", srv.localPort())
var s = srv.accept()
var line = s.readLine(5000)
s.write(Buffer.fromString("got: " + line + "\n"), 5000)
s.close()
srv.close()
```

---

## `UDP`

Unreliable datagrams. One static (`UDP.bind`) returns an instance with
`send` / `recv` plus the usual lifecycle.

### `UDP` statics

| Method | Returns | Notes |
| --- | --- | --- |
| `UDP.bind(host, port)` | udp \| `null` | Bind a UDP socket. `host == ""` or `"*"` binds all interfaces; `port == 0` lets the OS assign one. Returns `null` on bind failure. The returned handle is non-blocking. |
| `UDP.listen(host, port)` | udp-server \| `null` | Bind a UDP socket and demultiplex incoming datagrams **per source** `(host, port)`. Each new source becomes a pending peer that you `accept(...)` to get a fresh `udp` handle bound to that one source. Used for stateful per-client UDP servers and as the entry point for `DTLS.accept`. Returns `null` on bind failure. |

### UDP instance methods

| Method | Returns | Notes |
| --- | --- | --- |
| `udp.send(buf, host, port)` | status | Send `buf` as one datagram to `host:port`. `host` may be a literal IP or a hostname (resolved via `IP`). Returns `"ok"`, `"busy"` (kernel send buffer full), or `"error"`. |
| `udp.recv()` | map \| status | Block forever until a datagram arrives. Returns `{ data: Buffer, host, port }` with the source address, or `"error"` if the socket is unbound. |
| `udp.recv(timeoutMs)` | map \| status | Same, bounded. Returns `"timeout"` if no datagram arrives in the deadline. |
| `udp.localPort()` | number | The actually-bound local port. |
| `udp.setBroadcast(b)` | `null` | Enable / disable `SO_BROADCAST`. Required to send to `255.255.255.255` or directed-broadcast addresses. |
| `udp.close()` | `null` | Close the socket. Idempotent. |

### UDP server instance methods

The handle returned by `UDP.listen` exposes a small surface, focused on
accepting per-source UDP peers. The accepted peer is a regular `udp`
handle (same instance methods as `UDP.bind`-returned ones).

| Method | Returns | Notes |
| --- | --- | --- |
| `srv.accept()` | udp \| `null` | Block forever until a new source sends a datagram, then return a fresh `udp` peer bound to that source. Subsequent datagrams from the same source flow into that peer's queue. |
| `srv.accept(timeoutMs)` | udp \| `null` | Same, bounded. `0` returns immediately (`null` if no pending source); `-1` is the same as no argument. |
| `srv.localPort()` | number | The actually-bound port. `0` if not listening. |
| `srv.close()` | `null` | Stop listening. Already-accepted peers remain usable until they themselves are closed. |

### Examples

```zym
// Send-and-forget
var u = UDP.bind("0.0.0.0", 0)
u.send(Buffer.fromString("ping"), "192.168.1.1", 9000)
u.close()

// Receive with source address
var s = UDP.bind("0.0.0.0", 9000)
var r = s.recv(5000)
if r != "timeout" {
    print("from", r["host"], r["port"], "got", r["data"].size(), "bytes")
}
s.close()
```

---

## `Sockets`

Multi-handle readiness primitive — the closest zym has to a `select(2)`
or `poll(2)` for scripts.

### Methods

| Method | Returns | Notes |
| --- | --- | --- |
| `Sockets.waitAny(handles, mode, timeoutMs)` | `{ ready, timedOut }` | Wait for any of `handles` (a list of sock / server / udp instances) to become ready in the given `mode`. `mode` is `"read"`, `"write"`, or `"any"`. Returns once at least one handle is ready, or when `timeoutMs` expires. `ready` is the subset of `handles` that became ready; `timedOut` is `true` when nothing fired before the deadline. |

`mode` semantics:

- `"read"` — handle is ready when bytes are immediately available
  (`sock`), a connection is pending (`server`), or a datagram has
  arrived (`udp`). A closed/errored socket also counts as readable so
  scripts can detect EOF in the same loop.
- `"write"` — handle is ready when `sock.status() == "connected"` or
  the UDP socket is bound. Useful for "wait until my outbound socket
  finishes its handshake".
- `"any"` — readable OR writable.

Implementation note: `waitAny` is a poll-with-quantum loop over the
existing readiness primitives, not a single multiplexed syscall. For a
handful of handles (the typical CLI workload) this is invisible; for
hundreds of fds, expect a quantum of latency (~20ms) on the slowest
spin.

### Example

```zym
// Accept one new client OR service one existing client, whichever comes first.
var clients = []
var srv = TCP.listen("127.0.0.1", 0)
while true {
    var watch = [srv]
    for c in clients { append(watch, c) }
    var w = Sockets.waitAny(watch, "read", -1)
    if w["timedOut"] { continue }
    for h in w["ready"] {
        if h == srv {
            append(clients, srv.accept(0))
        } else {
            var line = h.readLine(0)
            // ... handle `line`, including "eof"/"closed"/"error" ...
        }
    }
}
```

---

## `TLS`

Encrypted TCP. Client connections (`TLS.connect`) verify the peer's
certificate against the system trust store by default; server-side
acceptance (`TLS.accept`) wraps an already-accepted TCP socket with the
caller's `CryptoKey` + `X509Certificate`. The instance methods on a TLS
sock are identical to TCP — `status` / `poll` / `read` / `readSome` /
`readLine` / `readAll` / `write` / `writeSome` / `setNoDelay` /
`localAddress` / `peerAddress` / `close` — and obey the same status
vocabulary, so script code that talks TCP can be retargeted at TLS by
swapping the factory call.

### `TLS` statics

| Method | Returns | Notes |
| --- | --- | --- |
| `TLS.connect(host, port)` | sock \| `null` | Open a TLS client connection. Blocks until the handshake finishes. Verifies the peer cert against the system CA bundle (loaded at startup); returns `null` on DNS failure, TCP refusal, handshake failure, or hostname mismatch. |
| `TLS.connect(host, port, timeoutMs)` | sock \| `null` | Same, with a deadline. `timeoutMs == 0` kicks off the connection and returns the sock immediately while the handshake is still in flight (drive it with `sock.poll()`); the underlying TCP is given a brief grace (~250ms) to settle before the handshake call. |
| `TLS.connect(host, port, timeoutMs, opts)` | sock \| `null` | Same, with explicit TLS options. See **opts** below. Pass `null` for defaults. |
| `TLS.accept(tcp, opts)` | sock \| `null` | Wrap an already-accepted TCP socket as the server side of a TLS handshake. `tcp` must be a sock returned by `srv.accept(...)` (i.e. its `__tcp__` tag is recognized). `opts` must be a map `{ key: CryptoKey, cert: X509Certificate }`. Blocks until the handshake completes. |
| `TLS.accept(tcp, opts, timeoutMs)` | sock \| `null` | Same, with a deadline. `timeoutMs == 0` returns the TLS sock immediately in `"connecting"` state (drive both sides with `poll()`). |

### `opts` shape (client)

| Key | Default | Notes |
| --- | --- | --- |
| `verify` | `true` | When `true`, the client verifies the server's certificate against `trustedRoots` (or the system trust store if `trustedRoots` is omitted) and the server's hostname against the SAN/CN. When `false`, the connection skips chain & hostname validation entirely (an "unsafe client"). Useful for self-signed peers and tests; do not use against untrusted networks. |
| `trustedRoots` | `null` | Optional list of `X509Certificate` instances to use as the trust anchor for this connection. When provided, replaces the system trust store; the listed certs are concatenated and parsed as a single chain. |
| `commonName` | `null` | Override the hostname used for SAN/CN matching. Defaults to the `host` argument of `TLS.connect`. Has no effect when `verify` is `false`. |

### `opts` shape (server, for `TLS.accept`)

| Key | Required | Notes |
| --- | --- | --- |
| `key` | yes | A `CryptoKey` containing the server's private key (e.g. from `Crypto.generateRsa(2048)` or `CryptoKey().load(path)`). |
| `cert` | yes | An `X509Certificate` containing the server's public certificate (e.g. from `Crypto.generateSelfSignedCertificate(...)` or `X509Certificate().load(path)`). |

### TLS instance methods

Identical surface to TCP (see the **TCP instance methods** table above);
the only behavioural difference is the meaning of `"connecting"` —
on a TLS sock, `"connecting"` covers both the underlying TCP handshake
and the TLS handshake, and a sock stays in `"connecting"` until the
TLS handshake completes (or fails into `"error"`).

The four-state TCP vocabulary collapses onto TLS as:

- TLS `DISCONNECTED` → `"closed"`
- TLS `HANDSHAKING` → `"connecting"`
- TLS `CONNECTED` → `"connected"`
- TLS `ERROR` / `ERROR_HOSTNAME_MISMATCH` → `"error"`

`localAddress` / `peerAddress` / `setNoDelay` delegate to the underlying
TCP transport — TLS itself doesn't introduce its own concept of those.

### Examples

```zym
// HTTPS GET against a public host, full system-CA verification.
var s = TLS.connect("example.com", 443, 8000)
s.write(Buffer.fromString("GET / HTTP/1.0\r\nHost: example.com\r\n\r\n"))
var status_line = s.readLine(5000)
print(status_line)
s.close()

// In-process TLS round-trip with a self-signed cert (server + client
// driven concurrently from the same script via non-waiting handshakes).
var c    = Crypto.create()
var key  = c.generateRsa(2048)
var cert = c.generateSelfSignedCertificate(key, "CN=localhost",
                                           "20240101000000",
                                           "20440101000000")

var srv      = TCP.listen("127.0.0.1", 0)
var port     = srv.localPort()
var cli      = TLS.connect("127.0.0.1", port, 0, { verify: false })
var rawSrv   = srv.accept(2000)
var srvTls   = TLS.accept(rawSrv, { key: key, cert: cert }, 0)

// Drive both handshakes to completion.
while cli.status() == "connecting" || srvTls.status() == "connecting" {
    cli.poll()
    srvTls.poll()
    System.sleep(20)
}

cli.write(Buffer.fromString("hello\n"))
print(srvTls.readLine(2000))   // "hello"
srvTls.write(Buffer.fromString("hi back\n"))
print(cli.readLine(2000))      // "hi back"

cli.close()
srvTls.close()
srv.close()
```

### `Sockets.waitAny` and TLS

TLS sockets are valid handles for `Sockets.waitAny` — the readiness
primitives apply the same way. A TLS sock counts as readable when its
`available()` is positive **or** the connection has terminated
(`"closed"` / `"error"`), and as writable when its status is
`"connected"`. Mixing TCP, TLS, UDP, and TCP-server handles in one
`waitAny` call is supported.

### Notes

- The system trust store is loaded once at zym startup and reused for
  every `TLS.connect` that doesn't pass an explicit `trustedRoots`. On
  Linux this is whatever path the platform exposes as the system CA
  bundle (e.g. `/etc/ssl/cert.pem` on most distros).
- A self-signed peer will fail the default verification with
  `status() == "error"` (both chain-validation failures and hostname
  mismatches collapse onto the shared `"error"` status). Pass
  `{ verify: false }` or a matching `trustedRoots` to allow it.
- `TLS.accept` drives the server-side handshake the same way
  `TLS.connect` drives the client side: with `timeoutMs > 0` it blocks
  until handshake completion; with `timeoutMs == 0` it returns the TLS
  sock immediately in `"connecting"` and the caller drives the
  handshake with `poll()` (typically alongside the client side, since
  zym is single-threaded).
- Server-side TLS doesn't currently have a `TLS.serve(host, port,
  opts)` shorthand; clients use `TCP.listen` + `srv.accept` + `TLS.accept`
  explicitly. This is deliberate — it keeps the TCP and TLS server
  surfaces composable.

---

## `DTLS`

Datagram TLS (DTLS) is **TLS over UDP**. It provides confidentiality and
integrity over a `UDP` association, with the same status / lifecycle
shape as `TLS`, but a datagram-shaped data API (`send` / `recv`) instead
of `TLS`'s stream-shaped one (`read` / `write`). Each `dtls.send(buf)`
call corresponds to exactly one DTLS record on the wire.

DTLS keeps UDP's lossy semantics for **application data**: packets can
still be reordered, duplicated, or lost. DTLS does **not** retransmit
your `send(...)` payloads — it only retransmits its own handshake
messages. Use it where you need encryption + authentication on top of
UDP, not where you need reliability.

### `DTLS` statics

| Method | Returns | Notes |
| --- | --- | --- |
| `DTLS.connect(host, port)` | dtls \| `null` | Bind an ephemeral local UDP and run the client-side DTLS handshake to `host:port` with default options (system-CA verify). Blocks forever. Returns `null` on connect / handshake failure. |
| `DTLS.connect(host, port, timeoutMs)` | dtls \| `null` | Same, bounded. `0` returns immediately in `"connecting"` so the caller can drive the handshake via `dtls.poll()`. `-1` blocks. |
| `DTLS.connect(host, port, timeoutMs, opts)` | dtls \| `null` | With explicit client options (see below). |
| `DTLS.connectFrom(udp, host, port [, timeoutMs, opts])` | dtls \| `null` | Like `DTLS.connect`, but the caller supplies the underlying `udp` (e.g. for source-port pinning). The `udp` must come from `UDP.bind`. The destination is set on the `udp` if it isn't already connected; further plain-`udp` use is undefined while DTLS is using it. |
| `DTLS.accept(udpServer, opts [, timeoutMs])` | dtls \| `null` | Server-side handshake. `udpServer` is a handle from `UDP.listen(host, port)`. This call drives the entire DTLS-server flow internally — including the cookie exchange (HelloVerifyRequest) — and returns a fully-handshaken DTLS peer when one is ready, or `null` on timeout. Calling it repeatedly on the same `udpServer` accepts more peers; the server-side `DTLSServer` state (cookies, in-flight handshakes) is persisted on the `udpServer` handle for as long as it lives. `opts` is `{ key, cert }` (server-side, see TLS docs). |

### `opts` shape (client)

Same shape as `TLS.connect`'s client options:

```
{
  verify:       bool                       = true,
  trustedRoots: cert | [cert, ...] | null  = null,   // X509Certificate(s)
  commonName:   string                     = ""      // override SNI / CN check
}
```

`verify: false` disables certificate verification entirely (use only
for self-signed test scenarios).

### `opts` shape (server, for `DTLS.accept`)

```
{
  key:  cryptoKey,    // CryptoKey from Crypto.generateRsa / loaded PEM
  cert: x509          // X509Certificate matching the key
}
```

### DTLS instance methods

DTLS instances expose a UDP-shaped surface (`send` / `recv`), not a
stream-shaped one. The status vocabulary matches TLS:

| Method | Returns | Notes |
| --- | --- | --- |
| `dtls.status()` | string | `"connecting"` (handshake in progress), `"connected"`, `"closed"`, or `"error"`. Hostname-mismatch and other handshake failures collapse onto `"error"`. |
| `dtls.poll()` | string | Advance the DTLS state machine once and return the new status. Required during handshake on the client side when `DTLS.connect(... 0)` was used; safe to call any time. |
| `dtls.available()` | number | Pending DTLS records (decrypted datagrams ready to read). |
| `dtls.send(buf)` | status | Send `buf` as one DTLS record. Returns `"ok"`, `"busy"` if the handshake hasn't completed yet (caller should `poll()` and retry), `"closed"` if the peer is gone, or `"error"`. |
| `dtls.recv()` | Buffer \| status | Block forever until a record arrives. Returns the decrypted payload as a `Buffer`. No source address — DTLS is point-to-point post-handshake. |
| `dtls.recv(timeoutMs)` | Buffer \| status | Same, bounded. `"timeout"` on deadline; `"closed"` / `"error"` otherwise. |
| `dtls.peerAddress()` | map | `{ host, port }` of the remote peer (the host string supplied at connect time on the client; the source address of the first ClientHello on the server). |
| `dtls.close()` | `null` | Send a close-notify alert and tear down the underlying UDP. Idempotent. |

### Examples

#### Client (browser-style: trust the system CA bundle)

```zym
var c = DTLS.connect("dtls.example.com", 5684, 8000)
if (c == null) {
    print("connect / handshake failed")
    return
}
c.send(Buffer.fromString("hello"))
var reply = c.recv(2000)
if (typeof(reply) == "map") {
    print("got", reply.size(), "bytes back")
}
c.close()
```

#### In-process self-signed round-trip

```zym
// Mint a key + self-signed certificate via the Crypto native.
var crypto = Crypto.create()
var key  = crypto.generateRsa(2048)
var cert = crypto.generateSelfSignedCertificate(
    key,
    "CN=localhost,O=zym,C=US",
    "20230101000000",
    "20330101000000")

// Server: bind a UDP listener for per-source demux.
var udps = UDP.listen("127.0.0.1", 0)
var port = udps.localPort()

// Client: kick off connect (non-waiting), then drive both sides in a
// shared loop until the handshake completes.
var dc = DTLS.connect("127.0.0.1", port, 0, { verify: false })
var sd = null
while (sd == null) {
    dc.poll()
    sd = DTLS.accept(udps, { key: key, cert: cert }, 0)
    System.sleep(5)
}
while (dc.status() != "connected") {
    dc.poll()
    sd.poll()
    System.sleep(5)
}

// Now we have an encrypted datagram channel.
dc.send(Buffer.fromString("hello dtls"))
sd.poll()
print(sd.recv(1000).toString())     // -> "hello dtls"

dc.close()
sd.close()
udps.close()
```

### `Sockets.waitAny` and DTLS / UDP server

DTLS handles and UDP-server handles are valid `Sockets.waitAny`
arguments. A DTLS sock counts as readable when a record is decrypted-
and-ready or the connection terminated, and as writable when its
status is `"connected"`. A `UDP.listen` server counts as readable
when at least one new source has arrived (`is_connection_available()`).
Mixing TCP, TLS, UDP, DTLS, TCP-server and UDP-server handles in one
`waitAny` call is supported.

### DTLS notes

- The cookie exchange (RFC 6347 §4.2.1) is handled inside `DTLS.accept`.
  Each call drives whatever progress the underlying state machine can
  make in the available `timeoutMs`; the server's cookie key and any
  in-flight client handshakes are persisted on the `udpServer` handle
  itself, so calling `DTLS.accept(udpServer, ...)` repeatedly with
  small timeouts is safe and is in fact the normal usage pattern.
- The first call to `DTLS.accept(udpServer, opts, ...)` locks in the
  server-side `{ key, cert }` for that `udpServer`. Subsequent calls
  ignore the `opts` argument's key/cert pair — to switch credentials,
  close the `udpServer` and start a new one.
- `DTLS.connect`'s underlying UDP grace settles automatically; you do
  not need to bind the source side yourself unless you want a specific
  source port (use `DTLS.connectFrom`).
- DTLS handshake timeouts: mbedTLS's internal retransmit timer starts
  at ~1s and doubles on each retransmit, capped near 60s. Pick a
  `timeoutMs` of at least 5–10 seconds for real-world peers; loopback
  testing typically completes in well under 100ms.
- `DTLS.send` / `dtls.recv` payloads are bounded by the path MTU. The
  engine doesn't fragment application data — try to keep individual
  records under ~1200 bytes for IPv4 internet paths.
- DTLS does not retransmit application data. If reliability is needed,
  either wrap a higher-level acknowledgement protocol on top, or use
  `TLS` over `TCP` instead.

## ENet — UDP with reliable + ordered + channels

ENet is a transport that sits on top of UDP and adds reliability
(per-packet, optional), packet ordering, multiplexed channels, and
connection liveness — without giving up the datagram model. It is the
right tool whenever you want game-style messaging from a script:
multi-channel custom protocols, peer-to-peer relay, distributed CLI
sync, or anything where TCP's single-stream byte semantics would force
you to invent your own framing on top.

> **ENet is its own wire protocol.** ENet endpoints can only talk to
> other ENet endpoints. It is **not** a way to add reliability on top
> of arbitrary UDP services — the bytes on the wire are an ENet-
> specific framing that other UDP listeners cannot decode. Pair zym's
> `ENet` against another zym `ENet`, or any C/C++ application using the
> upstream `enet` library.

### Statics

| Method                                          | Returns                          | Notes                                                                                                                                                                       |
|-------------------------------------------------|----------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `ENet.connect(host, port [, channels, opts])`         | `{ host, peer }` map, or `null`  | Non-blocking. `peer` is returned in `"connecting"` state; caller must drive `host.service(timeoutMs)` until the first `connect` event. Default `channels` = 8 (range 1–255). If `opts.tls` is provided, the host is wrapped with a DTLS client (see *ENet over DTLS* below). |
| `ENet.listen(host, port [, maxPeers, channels, opts])`| ENet host, or `null`             | Binds and starts a host that accepts incoming peers via `service()` events. Default `maxPeers` = 32, `channels` = 8. If `opts.tls = { key, cert }` is provided, every inbound peer must complete a DTLS handshake before being delivered as a `connect` event.                |

### Host instance methods (`__enet__`)

| Method                                   | Returns                                                                | Notes                                                                                                                                                                                                                                                                                       |
|------------------------------------------|------------------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `host.service(timeoutMs)`                | event map, or `null`                                                   | Pumps the host. Returns `null` if no event arrived within `timeoutMs` (clamped to 0). Event map: `{ type, peer, data, channel }`. `type` ∈ `"connect"`/`"disconnect"`/`"receive"`/`"error"`. For `"receive"`, `data` is a Buffer; otherwise `data` is the application-defined integer code. |
| `host.flush()`                           | `null`                                                                 | Force-pushes outbound queues onto the wire without waiting for the next `service()`.                                                                                                                                                                                                        |
| `host.localPort()`                       | port number                                                            | Useful with `listen("...", 0)` to discover the OS-assigned port.                                                                                                                                                                                                                            |
| `host.broadcast(buf, channel, mode)`     | status string                                                          | Send `buf` to every connected peer on `channel` with `mode`. `mode` ∈ `"reliable"`/`"unreliable"`/`"unsequenced"`. Status is `"ok"`, `"error"`, or `"closed"`.                                                                                                                               |
| `host.refuseNewConnections(refuse)`      | `null`                                                                 | When `refuse` is `true`, stop accepting new inbound connections; existing peers are unaffected. Useful for draining a server before shutdown (esp. with DTLS, where the cookie exchange is otherwise still served).                                                                          |
| `host.close()`                           | `null`                                                                 | Tears down the host. Idempotent.                                                                                                                                                                                                                                                            |

### Peer instance methods (`__enetp__`)

| Method                                   | Returns                                                                | Notes                                                                                                                                                                                                  |
|------------------------------------------|------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `peer.status()`                          | `"connecting"` / `"connected"` / `"closed"` / `"error"`                | See `docs/conventions.md` for the shared status vocabulary.                                                                                                                                            |
| `peer.send(buf, channel, mode)`          | status string                                                          | Send `buf` to this peer. `channel` must be in `[0, channelsAtConnect)`; out-of-range raises a runtime error. `mode` as in `broadcast`. Returns `"ok"`, `"closed"`, or `"error"`.                       |
| `peer.peerAddress()`                     | `{ host, port }`                                                       | Remote address of the peer.                                                                                                                                                                            |
| `peer.ping()`                            | `null`                                                                 | Force a ping packet immediately.                                                                                                                                                                       |
| `peer.pingMs()`                          | number                                                                 | Most recent round-trip-time sample in milliseconds (`0` until a measurement exists).                                                                                                                   |
| `peer.disconnect([data])`                | `null`                                                                 | Graceful disconnect: queues a disconnect packet that flushes after pending sends. Optional integer `data` is delivered to the peer's `disconnect` event.                                               |
| `peer.disconnectNow([data])`             | `null`                                                                 | Immediate disconnect: drops everything pending and notifies the peer in one shot.                                                                                                                      |
| `peer.close()`                           | `null`                                                                 | Local-only reset. Doesn't notify the remote (use `disconnect`/`disconnectNow` for that).                                                                                                               |

### Service event shapes

| `ev["type"]`   | Other fields                                                                |
|----------------|-----------------------------------------------------------------------------|
| `"connect"`    | `peer` (handle), `data` (integer code), `channel` (always 0)                |
| `"disconnect"` | `peer` (handle), `data` (integer code), `channel` (always 0)                |
| `"receive"`    | `peer` (handle), `data` (Buffer), `channel` (the channel the sender used)   |
| `"error"`      | (no other fields)                                                           |

### Examples

#### Minimal echo server

```
func main(argv) {
    var srv = ENet.listen("0.0.0.0", 9000, 32, 4)
    while (true) {
        var ev = srv.service(100)
        if (ev == null) continue
        if (ev["type"] == "receive") {
            ev["peer"].send(ev["data"], ev["channel"], "reliable")
        }
    }
}
```

#### Client with handshake loop

```
func main(argv) {
    var r = ENet.connect("127.0.0.1", 9000, 4)
    if (r == null) { print("connect failed\n"); return 1 }
    var host = r["host"]
    var peer = r["peer"]
    while (peer.status() == "connecting") {
        host.service(50)
    }
    if (peer.status() != "connected") { print("handshake failed\n"); return 1 }
    peer.send(Buffer.fromString("hello"), 0, "reliable")
    host.flush()
    var ev = host.service(2000)
    if (ev != null && ev["type"] == "receive") {
        print("got: %s\n", ev["data"].toUtf8())
    }
    peer.disconnect()
    host.flush()
    host.service(50)
    host.close()
    return 0
}
```

### Notes

- **Channel count is fixed at handshake.** Both sides agree on the
  channel count at `connect`/`listen` time. Sending on a channel index
  outside `[0, channels)` raises a runtime error.
- **Unreliable vs unsequenced.** `"unreliable"` packets are *sequenced*
  (newer packets supersede older ones on the same channel) but may be
  dropped. `"unsequenced"` packets are dropped *and* may be reordered;
  use them only when you don't care about order at all.
- **No retransmit of unreliable packets.** Reliability is opt-in per
  packet. ENet does not promise delivery for `"unreliable"` or
  `"unsequenced"` modes.
- **`service(timeoutMs)` is the heartbeat.** ENet has no background
  thread; **all** progress (handshake, retransmits, ack delivery,
  disconnect notifications) happens during a `service()` call. Long
  stretches without `service()` will trigger peer timeouts.
- **`ENet.connect` is non-blocking on purpose.** Both ends must pump
  `service()` for the handshake to advance — useful for in-process
  loopback (one process drives both sides) and required for any
  multi-peer client (one host serves many concurrent connections).
- **`ENet` does not currently appear in `Sockets.waitAny`.** Use
  `host.service(timeoutMs)` directly; the service call is itself a
  multi-peer poll across every peer attached to that host.

### ENet over DTLS

Both `ENet.connect` and `ENet.listen` accept an optional trailing `opts`
map whose `tls` field opts the host into DTLS. When set, every datagram
the host sends or receives is wrapped in a DTLS record — the channel,
ordering, reliability, and `service()` semantics are unchanged. Once
the handshake completes, scripts use the same `host`/`peer` instance
methods as for plain ENet.

> **Still ENet on the wire.** ENet+DTLS talks only to other ENet+DTLS
> peers. The encrypted bytes are an ENet-specific framing wrapped in
> DTLS records — generic DTLS endpoints will not understand them. Pair
> only against another ENet endpoint configured with the same DTLS
> options.

#### Client opts (`ENet.connect`)

`opts.tls` accepts the same shape as `TLS.connect`:

| Field          | Type                              | Default              | Notes                                                                                              |
|----------------|-----------------------------------|----------------------|----------------------------------------------------------------------------------------------------|
| `verify`       | bool                              | `true`               | When `false`, the server certificate is accepted without verification (useful for self-signed).    |
| `trustedRoots` | `X509Certificate` or list thereof | system trust store   | Roots used to validate the server certificate. Ignored when `verify` is `false`.                   |
| `commonName`   | string                            | the `host` argument  | Hostname used for SNI and certificate verification. Override when connecting by IP literal.        |

#### Server opts (`ENet.listen`)

`opts.tls` requires `{ key, cert }`:

| Field   | Type                | Notes                                                                |
|---------|---------------------|----------------------------------------------------------------------|
| `key`   | `CryptoKey`         | Private key matching `cert`. Required.                               |
| `cert`  | `X509Certificate`   | Server certificate. Required.                                        |

Missing `key` or `cert` raises a runtime error. Both come from the
`Crypto` native — see [crypto.md](crypto.md).

#### Example: in-process self-signed handshake

```
func main(argv) {
    var c = Crypto.create()
    var key = c.generateRsa(2048)
    var cert = c.generateSelfSignedCertificate(key, "CN=zym-test",
                                               "20240101000000",
                                               "20340101000000")

    var srv = ENet.listen("127.0.0.1", 0, 4, 4,
                          { tls: { key: key, cert: cert } })
    var port = srv.localPort()

    var pair = ENet.connect("127.0.0.1", port, 4,
                            { tls: { verify: false } })
    var host = pair["host"]
    var peer = pair["peer"]

    var serverPeer = null
    while (peer.status() == "connecting" || serverPeer == null) {
        var ev = srv.service(20)
        if (ev != null && ev["type"] == "connect") { serverPeer = ev["peer"] }
        host.service(20)
    }

    peer.send(Buffer.fromString("hello dtls"), 0, "reliable")
    host.flush()
    var ev = srv.service(500)
    print("server got: %s\n", ev["data"].toUtf8())

    peer.disconnect()
    host.flush(); host.service(50)
    host.close(); srv.close()
    return 0
}
```

#### Notes specific to ENet+DTLS

- **Handshake takes one extra round trip** vs plain ENet because of
  the DTLS cookie exchange (`HelloVerifyRequest`). Plan on a few
  hundred milliseconds of `service()` driving on real networks before
  the first application data flows.
- **Self-signed certificates require `verify: false`.** Same as
  `TLS.connect`; without it, the handshake fails with `"error"`.
- **`refuseNewConnections(true)` is the graceful-drain knob.** With
  DTLS this also stops the cookie machinery from responding to fresh
  ClientHellos, which is usually what you want during shutdown.
- **TLS bring-up is shared with the `Crypto` / `TLS` / `DTLS` natives.**
  No extra initialization is required — the TLS layer is already
  brought up at startup.

---

## `WebSocket` — RFC 6455 WebSocket client and server

RFC 6455 WebSocket client and server, exposed as a single namespace:

- `WebSocket.connect(url [, opts])` — open a WebSocket client to a
  `ws://` or `wss://` URL.
- `WebSocket.accept(tcp [, opts])` — wrap an already-accepted TCP (or
  TLS) socket as the **server** side of a WebSocket handshake.

The instance returned by either factory exposes a frame-shaped API
(`send` / `sendText` / `recv`) that follows the same status-string
vocabulary and `Buffer` byte-currency as the rest of these networking
natives.

### Frame model

- **One frame per call.** Each `send(buf)` / `sendText(text)` produces
  exactly one WebSocket frame; each `recv(...)` returns exactly one
  received frame. There is no implicit fragmentation; continuation
  frames are reassembled before the frame surfaces to script.
- **Text vs binary.** Binary frames are exchanged as `Buffer` instances;
  text frames are sent and received as `string` (UTF-8 on the wire).
  After a successful `recv(...)`, `sock.wasStringPacket()` reports
  which kind of frame it was.
- **Handshake is `poll()`-driven.** Both `connect` and `accept` return
  immediately in `"connecting"` state. The caller must drive
  `sock.poll()` (or include the sock in `Sockets.waitAny`) until
  `sock.status() == "connected"`.

### Statics

| Method | Returns | Notes |
| --- | --- | --- |
| `WebSocket.connect(url)` | sock \| `null` | Open a client to `url`. Returns the sock immediately in `"connecting"` state (drive the handshake with `sock.poll()`). `null` on outright failure (bad URL shape, WebSocket support unavailable in this build). |
| `WebSocket.connect(url, opts)` | sock \| `null` | Same, with explicit options (see **opts** below). Pass `null` for defaults. |
| `WebSocket.accept(tcp)` | sock \| `null` | Wrap an already-accepted **TCP** sock (a handle returned by `TCP.listen(...).accept(...)`) as the server side of a WebSocket handshake. The TCP sock must remain live as long as the returned WebSocket handle is in use. Returns the sock in `"connecting"` state; drive the handshake with `sock.poll()`. |
| `WebSocket.accept(tcp, opts)` | sock \| `null` | Same, with explicit options. For `wss://` servers, wrap the accepted TCP with `TLS.accept(...)` first and pass the resulting TLS sock here. |

### `opts` shape

All keys are optional. Unknown keys are ignored.

| Key | Default | Notes |
| --- | --- | --- |
| `tls` | `null` | Client-side only. Controls TLS verification when `url` is `wss://`. Same shape as `TLS.connect`'s `opts`: `{ verify: bool = true, trustedRoots: cert \| [cert, ...] \| null, commonName: string }`. When `verify` is `false`, the handshake skips chain & hostname validation entirely. Ignored on `WebSocket.accept` (the underlying stream already carries the TLS layer). |
| `protocols` | `[]` | List of WebSocket subprotocol names to advertise. On a client these go out as `Sec-WebSocket-Protocol`; on a server they constrain the set the peer is allowed to pick. The negotiated protocol is available after handshake via `sock.selectedProtocol()`. |
| `headers` | `[]` | List of additional handshake header lines as `"Name: value"` strings. Client-side: appended to the HTTP `GET` upgrade request. Server-side: appended to the HTTP 101 upgrade response. |
| `inboundBufferSize` | `65535` | Maximum size in bytes of the inbound packet buffer. Frames larger than this are rejected. |
| `outboundBufferSize` | `65535` | Maximum size in bytes of the outbound packet buffer. |
| `maxQueuedPackets` | `4096` | Maximum number of frames that can sit in the receive queue between `poll()`s before the peer is forced closed. |
| `heartbeatInterval` | `0` | Seconds between automatic ping frames. `0` disables heartbeats. |

### Address forms

- **Client URLs.** `ws://host:port/path` (plain) or `wss://host:port/path`
  (TLS). The `tls` option only matters for `wss://` URLs; if `tls` is
  passed on a plain `ws://` URL it is ignored.
- **Server side.** `WebSocket.accept` takes an already-bound TCP or TLS
  socket — `WebSocket` itself does not bind a listening port. Pair it
  with `TCP.listen(...)` + `srv.accept(...)` for plain `ws://`, or with
  `TCP.listen(...)` + `srv.accept(...)` + `TLS.accept(...)` for `wss://`.

### Sock instance methods

| Method | Returns | Notes |
| --- | --- | --- |
| `sock.status()` | string | One of `"connecting"`, `"connected"`, `"closing"`, `"closed"`. Does not advance state. |
| `sock.poll()` | string | Drives one round of state advancement (handshake progress, frame parsing, heartbeats) and returns the new status. Required during the handshake when the caller used the non-waiting form of `connect` / `accept`. Safe to call any time. |
| `sock.available()` | number | Pending frames (decoded, ready to read). Polls before reporting. |
| `sock.send(buf)` | status | Send `buf` as one binary WebSocket frame. Returns `"ok"`, `"closed"` (peer is no longer OPEN), or `"error"`. Empty buffers send an empty binary frame. |
| `sock.sendText(text)` | status | Send `text` as one text WebSocket frame. `text` is interpreted as UTF-8 on the wire. Status vocabulary is the same as `send`. |
| `sock.recv()` | Buffer \| status | Block forever until one frame arrives, then return its payload as a `Buffer`. Use `sock.wasStringPacket()` immediately afterwards to find out whether it was a text frame. Returns `"eof"` if the peer closes before any frame arrives, or `"error"` if the connection drops. |
| `sock.recv(timeoutMs)` | Buffer \| status | Same, bounded. `0` is a non-blocking peek — returns `"busy"` if no frame is queued. `> 0` waits up to that many milliseconds and returns `"timeout"` if nothing arrives before the deadline. `-1` blocks (default). |
| `sock.wasStringPacket()` | bool | `true` if the most recently received frame was a text (UTF-8) frame, `false` for a binary frame. Reflects the *last* successful `recv(...)`; the value is undefined before the first frame arrives. |
| `sock.selectedProtocol()` | string | Negotiated subprotocol name, or `""` if no subprotocol was selected. Valid once `status() == "connected"`. |
| `sock.requestedUrl()` | string | Server-side: the URL the client used in its `GET` upgrade. Client-side: the URL passed to `WebSocket.connect`. Valid once `status() == "connected"`. |
| `sock.closeCode()` | number | The 16-bit WebSocket close code reported by the peer (or sent locally) once the close handshake has happened. `-1` if no close has been observed yet. |
| `sock.closeReason()` | string | The UTF-8 close reason that accompanied `closeCode()`, or `""` if none was provided. |
| `sock.peerAddress()` | `{ host, port }` \| `null` | Remote address of the underlying TCP / TLS stream. `null` before the handshake completes. |
| `sock.setNoDelay(b)` | `null` | Toggle `TCP_NODELAY` (Nagle's algorithm) on the underlying transport. |
| `sock.close()` | `null` | Send a normal-closure (1000, no reason) close frame and tear down the underlying stream. Idempotent. |
| `sock.close(code)` | `null` | Same, with an explicit close code. |
| `sock.close(code, reason)` | `null` | Same, with an explicit close code **and** reason. `reason` is sent as UTF-8 and is limited by the WebSocket spec to 123 bytes; over-long reasons are truncated. |

### Examples

#### Client: connect to a public echo, exchange one message

```zym
var s = WebSocket.connect("ws://echo.websocket.events/")
while s.status() == "connecting" {
    s.poll()
    System.sleep(20)
}
if s.status() != "connected" {
    print("handshake failed:", s.status())
    return
}

s.sendText("hello")
var frame = s.recv(5000)
if typeof(frame) == "string" {
    print("error/timeout:", frame)
} else {
    if s.wasStringPacket() {
        print("text reply:", frame.toString())
    } else {
        print("binary reply:", frame.size(), "bytes")
    }
}
s.close()
```

#### Server: accept one plain `ws://` connection and echo a frame

```zym
var srv = TCP.listen("127.0.0.1", 0)
print("listening on", srv.localPort())

var raw = srv.accept()                  // TCP sock
var ws  = WebSocket.accept(raw)         // upgrade to WebSocket
while ws.status() == "connecting" {
    ws.poll()
    System.sleep(20)
}

var frame = ws.recv(5000)
if typeof(frame) != "string" {
    ws.send(frame)                       // echo back
}
ws.close()
raw.close()
srv.close()
```

#### Server: `wss://` with a self-signed certificate (in-process round-trip)

```zym
var c    = Crypto.create()
var key  = c.generateRsa(2048)
var cert = c.generateSelfSignedCertificate(key, "CN=localhost",
                                           "20240101000000",
                                           "20440101000000")

var srv = TCP.listen("127.0.0.1", 0)
var port = srv.localPort()

// Client side runs concurrently; since zym is single-threaded we drive
// both sides from one loop.
var cli = WebSocket.connect("wss://127.0.0.1:" + port + "/",
                            { tls: { verify: false } })

var rawSrv = srv.accept(2000)
var tlsSrv = TLS.accept(rawSrv, { key: key, cert: cert }, 0)
var wsSrv  = WebSocket.accept(tlsSrv)

while cli.status() == "connecting" || wsSrv.status() == "connecting" {
    cli.poll()
    tlsSrv.poll()
    wsSrv.poll()
    System.sleep(20)
}

cli.sendText("hello wss")
wsSrv.poll()
print(wsSrv.recv(2000).toString())     // "hello wss"

cli.close()
wsSrv.close()
tlsSrv.close()
srv.close()
```

#### Selecting a subprotocol

```zym
// Client advertises two protocols; server picks the first one it knows.
var s = WebSocket.connect("ws://chat.example/",
                          { protocols: ["chat.v2", "chat.v1"] })
while s.status() == "connecting" { s.poll(); System.sleep(20) }
print("server picked:", s.selectedProtocol())
```

### `Sockets.waitAny` and WebSocket

WebSocket socks are valid handles for `Sockets.waitAny` — the readiness
primitives apply the same way:

- `"read"` — ready when a frame has been decoded and is sitting in the
  queue (`available() > 0`), **or** the peer has terminated
  (`"closing"` / `"closed"`) so EOF can be observed in the same loop.
- `"write"` — ready when `status() == "connected"`, and also when
  terminated so a send returns its own `"closed"` status without
  spinning forever.
- `"any"` — readable OR writable.

Mixing WebSocket socks with TCP, TLS, UDP, DTLS, and server handles in
one `waitAny` call is supported:

```zym
var clients = []                       // list of WebSocket handles
var srv = TCP.listen("127.0.0.1", 0)
while true {
    var watch = [srv]
    for c in clients { append(watch, c) }
    var w = Sockets.waitAny(watch, "read", -1)
    if w["timedOut"] { continue }
    for h in w["ready"] {
        if h == srv {
            var raw = srv.accept(0)
            append(clients, WebSocket.accept(raw))
        } else {
            var f = h.recv(0)
            // handle `f`, including "busy"/"eof"/"closed"/"error"
        }
    }
}
```

### Notes

- **Peer-level API only.** `WebSocket` exposes a single peer per handle.
  Game-style multi-peer messaging (many clients addressed through one
  object) is what `ENet` covers above.
- **The underlying TCP / TLS sock must outlive the WebSocket.** On the
  server side, the sock returned by `srv.accept(...)` (or
  `TLS.accept(...)`) is what carries the bytes — keep it in scope
  until the WebSocket handle is closed. Closing the WebSocket does
  *not* automatically close the TCP / TLS sock.
- **Heartbeats are off by default.** Pass `heartbeatInterval` (in
  seconds) in `opts` to send WebSocket ping frames on an interval. The
  default behaviour is to leave the connection idle unless data flows,
  matching the RFC 6455 baseline.
- **Buffer sizes are hard caps.** `inboundBufferSize` and
  `outboundBufferSize` are static at handshake time. Frames larger
  than the inbound cap are rejected and force the connection into
  `"error"`; sends larger than the outbound cap fail with `"error"`.
  For applications that need to ship multi-megabyte payloads, raise
  these in `opts` before connecting.
- **Headers are unvalidated.** Lines passed via `opts.headers` are
  written verbatim into the HTTP upgrade exchange. Don't pass
  attacker-controlled strings without filtering for `\r\n`.
- **`recv` returns one frame at a time.** Continuation frames and
  fragmentation are reassembled before the frame surfaces to script;
  from the script's point of view, every successful `recv(...)` is
  one logical message.
