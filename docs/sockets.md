# Networking (`IP`, `TCP`, `UDP`, `TLS`, `Sockets`)

Cross-platform TCP / UDP / TLS sockets and DNS, exposed as a layered set
of zym-flavored namespaces:

- `IP` — DNS resolution and local-interface enumeration. *(Phase 1 — implemented.)*
- `TCP` — reliable byte streams; client `connect` + listening server. *(Phase 2 — pending.)*
- `UDP` — unreliable datagrams; bound socket with `send` / `recv`. *(Phase 2 — pending.)*
- `TLS` — encrypted TCP client over `X509Certificate` / `CryptoKey` from `Crypto`. *(Phase 3 — pending.)*
- `Sockets` — `Sockets.waitAny(handles, mode, timeoutMs)` for multi-socket readiness. *(Phase 2 — pending.)*

This file documents the surface that exists today (Phase 1: `IP`).
Sections for `TCP` / `UDP` / `TLS` / `Sockets` will be filled in as
each phase lands.

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

## `TCP`, `UDP`, `TLS`, `Sockets`

*Not yet implemented. Sections will be added in Phase 2 (TCP/UDP/Sockets) and Phase 3 (TLS).*

The shape of the API is locked in (see the project plan). Highlights:

- Sockets are **non-blocking under the hood**. Every method that *can*
  wait takes an optional `timeoutMs` (default `-1` = block forever,
  `0` = never wait, anything else = millisecond deadline).
- `read` / `write` are "all-or-nothing" with a deadline; `readSome` /
  `writeSome` return whatever the kernel had ready and never wait.
- `Sockets.waitAny(handles, mode, timeoutMs)` will give scripts a
  multi-socket readiness primitive.
- `TLS.connect(host, port, timeoutMs, opts)` will reuse
  `X509Certificate` / `CryptoKey` from [crypto.md](crypto.md).
