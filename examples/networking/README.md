# Networking examples

Runnable, single-file zym scripts that demonstrate the `IP`, `TCP`, `UDP`,
`TLS`, and `DTLS` natives. Each script is meant to be **read** as much as run -
the comment header on top of every file explains what it does, how to run it,
and what to expect.

## How they're organized

```
examples/networking/
├─ tcp/    — reliable byte-stream sockets (TCP and TLS)
├─ udp/    — datagram sockets (UDP and DTLS)
└─ enet/   — reliable / unreliable / unsequenced over UDP via ENet (and ENet+DTLS)
```

## Recommended reading order

Examples are ordered easiest -> hardest. Each one introduces one new concept;
following the order means every script is "the previous one plus one new
idea".

| # | Path                                                                | New concept                                |
|---|---------------------------------------------------------------------|--------------------------------------------|
| 1 | `tcp/loopback.zym`                                                  | TCP listen / connect / accept              |
| 2 | `tcp/echo_server.zym` + `tcp/echo_client.zym`                       | Accept loop, request/reply lockstep        |
| 3 | `tcp/port_scanner.zym`                                              | `timeoutMs` on `TCP.connect`               |
| 4 | `tcp/http_get.zym`                                                  | Composing a real protocol on top of TCP    |
| 5 | `udp/loopback.zym`                                                  | UDP bind / send / recv with source address |
| 6 | `udp/echo_server.zym` + `udp/echo_client.zym`                       | Stateless datagram server, no `accept`     |
| 7 | `udp/multi_client_server.zym` + `udp/multi_client_client.zym`       | `UDP.listen` per-source demux, `Sockets.waitAny` |
| 8 | `tcp/chat_server.zym` + `tcp/chat_client.zym`                       | Multi-client server: `Sockets.waitAny` over many handles |
| 9 | `tcp/proxy.zym`                                                     | Bidirectional forwarding: `waitAny` + `readSome`         |
| 10 | `tcp/https_get.zym`                                                | TLS as a drop-in for TCP; system-CA verify by default    |
| 11 | `tcp/tls_self_signed_server.zym` + `tcp/tls_client.zym`            | Server-side TLS handshake; self-signed cert + `verify: false` |
| 12 | `udp/dtls_self_signed_server.zym` + `udp/dtls_client.zym`          | Encrypted UDP: DTLS handshake on top of `UDP.listen`     |
| 13 | `enet/loopback.zym`                                                | ENet handshake + reliable round-trip in a single process |
| 14 | `enet/echo_server.zym` + `enet/echo_client.zym`                    | Reliable / unreliable / unsequenced sends across channels |
| 15 | `enet/broadcast_server.zym` + `enet/broadcast_client.zym`          | `host.broadcast()` fan-out + tiny text framing protocol  |
| 16 | `enet/dtls_self_signed_server.zym` + `enet/dtls_client.zym`        | Encrypted ENet: DTLS via `opts.tls` on `ENet.connect`/`listen` |

## How arguments work

Every example uses zym's `func main(argv)` entry point. The CLI takes
arguments in this shape:

```
zym <script.zym> [zym-cli args...] -- [script args...]
```

The `--` is the separator. Everything after `--` is what `argv[1..]` sees
inside the script (`argv[0]` is the path to the `zym` binary).

So:

```
zym examples/networking/tcp/echo_server.zym -- 9000
```

…runs the echo server with `argv[1] == "9000"`.

If you don't pass any args after `--`, every example falls back to a
sensible default (typically port 9000, host `127.0.0.1`). Look at the
banner header on each script for the exact defaults.

## Conventions every script follows

- **Top-of-file banner** with `What this does` / `How to run it` /
  `What you'll see` blocks. Read this first; it's the help text.
- **Defaults for every argument**, so any example runs with no `--` args.
- **Port `0`** (where applicable) means "let the OS pick", and the
  server prints the actual port via `localPort()`.
- **No external dependencies** beyond stock zym - no `nc`, no `curl`,
  no Python.
- **Stop a long-running server with Ctrl-C.** No graceful-shutdown
  handler is wired up; the kernel reclaims the listening socket on exit.

## Running multiple at once

For client/server pairs, open two terminals: run the server in one,
then the client in the other. Or background the server in the same
shell:

```
zym examples/networking/tcp/echo_server.zym -- 9000 &
zym examples/networking/tcp/echo_client.zym -- 127.0.0.1 9000 hello world
kill %1
```

## When something goes wrong

- `connect failed` - the server isn't running, or the port is wrong, or a
  firewall is in the way.
- `could not listen on port N (port already in use?)` - another process is
  already bound to that port. Pick a different one, or pass `0` to
  let the OS pick.
- `ERROR: ...` or `mbedtls error: ...` lines on stderr - these are
  diagnostic messages from the underlying network layer, not from your
  script. They show up most often when a TCP `connect` is refused, when
  a TLS handshake fails, or on DTLS dead-port tests. They don't affect
  the outcome of the script - the call still returns the right status
  string or `null`. They can be silenced project-wide via the
  `USE_ENGINE_ERRORS=OFF` build option.

## API references

For the underlying native APIs, see:

- [`docs/sockets.md`](../../docs/sockets.md) - `IP`/`TCP`/`UDP`/`TLS`/`DTLS`/`ENet`/`Sockets.waitAny`
- [`docs/buffer.md`](../../docs/buffer.md) - the byte type used by sockets
- [`docs/conventions.md`](../../docs/conventions.md) - the status-string
  vocabulary (`"ok"`/`"busy"`/`"timeout"`/`"eof"`/`"closed"`/`"error"`)
