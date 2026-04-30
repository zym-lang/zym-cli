# Networking (`IP`, `TCP`, `UDP`, `TLS`, `Sockets`)

Cross-platform TCP / UDP / TLS sockets and DNS, exposed as a layered set
of zym-flavored namespaces:

- `IP` — DNS resolution and local-interface enumeration.
- `TCP` — reliable byte streams; client `connect` + listening server.
- `UDP` — unreliable datagrams; bound socket with `send` / `recv`.
- `TLS` — encrypted TCP client and server over `X509Certificate` / `CryptoKey` from `Crypto`.
- `Sockets` — `Sockets.waitAny(handles, mode, timeoutMs)` for multi-socket readiness.

---

## Conventions

The networking natives follow the rules in [conventions.md](conventions.md)
verbatim. The most relevant ones:

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
  names. See [conventions.md](conventions.md) for the full mental
  model.

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

### UDP instance methods

| Method | Returns | Notes |
| --- | --- | --- |
| `udp.send(buf, host, port)` | status | Send `buf` as one datagram to `host:port`. `host` may be a literal IP or a hostname (resolved via `IP`). Returns `"ok"`, `"busy"` (kernel send buffer full), or `"error"`. |
| `udp.recv()` | map \| status | Block forever until a datagram arrives. Returns `{ data: Buffer, host, port }` with the source address, or `"error"` if the socket is unbound. |
| `udp.recv(timeoutMs)` | map \| status | Same, bounded. Returns `"timeout"` if no datagram arrives in the deadline. |
| `udp.localPort()` | number | The actually-bound local port. |
| `udp.setBroadcast(b)` | `null` | Enable / disable `SO_BROADCAST`. Required to send to `255.255.255.255` or directed-broadcast addresses. |
| `udp.close()` | `null` | Close the socket. Idempotent. |

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
