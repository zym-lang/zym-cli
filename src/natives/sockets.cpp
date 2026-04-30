// TCP / UDP / Sockets.waitAny native.
//
// Per-instance shape: each socket, server, and UDP peer is a map-of-closures
// bound to a context whose native data is a Ref<T>* heap pointer. Tag keys
// disambiguate handle types (and let `Sockets.waitAny` recover the underlying
// peer):
//
//   "__tcp__"   -> TcpHandle*       (StreamPeerTCP)
//   "__tcps__"  -> TcpsHandle*      (TCPServer)
//   "__udp__"   -> UdpHandle*       (PacketPeerUDP)
//
// The model is non-blocking under the hood with `timeoutMs`-driven helpers
// layered on top:
//   timeoutMs == -1  => block forever
//   timeoutMs ==  0  => return immediately
//   timeoutMs  >  0  => wait up to that many milliseconds
//
// See docs/sockets.md and docs/conventions.md for the full surface.

#include "core/crypto/crypto.h"
#include "core/io/ip.h"
#include "core/io/ip_address.h"
#include "core/io/net_socket.h"
#include "core/io/packet_peer_udp.h"
#include "core/io/stream_peer_tcp.h"
#include "core/io/stream_peer_tls.h"
#include "core/io/tcp_server.h"
#include "core/os/os.h"
#include "core/string/ustring.h"
#include "core/variant/variant.h"

#include "natives.hpp"

extern ZymValue makeBufferInstance(ZymVM* vm, const PackedByteArray& src);
extern bool zymExtractCryptoKey(ZymVM* vm, ZymValue v, Ref<CryptoKey>* out);
extern bool zymExtractX509(ZymVM* vm, ZymValue v, Ref<X509Certificate>* out);

// ---- handle plumbing ----

struct TcpHandle  { Ref<StreamPeerTCP>  s; };
struct TcpsHandle { Ref<TCPServer>      s; };
struct UdpHandle  { Ref<PacketPeerUDP>  s; };
// TLS handle keeps the inner TCP alive in the same struct so it can't be
// GC'd out from under the TLS layer (the StreamPeerTLS only holds a
// Ref<StreamPeer> internally; we want a typed handle for setNoDelay /
// localAddress / peerAddress / waitAny passthrough).
struct TlsHandle  { Ref<StreamPeerTLS>  tls; Ref<StreamPeerTCP> base; };

static void tcpFinalizer (ZymVM*, void* d) { delete static_cast<TcpHandle*>(d); }
static void tcpsFinalizer(ZymVM*, void* d) { delete static_cast<TcpsHandle*>(d); }
static void udpFinalizer (ZymVM*, void* d) { delete static_cast<UdpHandle*>(d); }
static void tlsFinalizer (ZymVM*, void* d) { delete static_cast<TlsHandle*>(d); }

// ---- value helpers ----

static ZymValue strZ(ZymVM* vm, const String& s) {
    CharString u = s.utf8();
    return zym_newStringN(vm, u.get_data(), u.length());
}

static bool reqStr(ZymVM* vm, ZymValue v, const char* where, String* out) {
    if (!zym_isString(v)) { zym_runtimeError(vm, "%s expects a string", where); return false; }
    *out = String::utf8(zym_asCString(v)); return true;
}

static bool reqNum(ZymVM* vm, ZymValue v, const char* where, double* out) {
    if (!zym_isNumber(v)) { zym_runtimeError(vm, "%s expects a number", where); return false; }
    *out = zym_asNumber(v); return true;
}

static bool reqBool(ZymVM* vm, ZymValue v, const char* where, bool* out) {
    if (!zym_isBool(v)) { zym_runtimeError(vm, "%s expects a bool", where); return false; }
    *out = zym_asBool(v); return true;
}

static int optInt(ZymVM* vm, const char* where, ZymValue* vargs, int vargc, int idx, int64_t* out) {
    if (idx >= vargc) return 0;
    if (!zym_isNumber(vargs[idx])) {
        zym_runtimeError(vm, "%s: argument %d must be a number", where, idx + 1);
        return -1;
    }
    *out = (int64_t)zym_asNumber(vargs[idx]);
    return 1;
}

// Resolve a Buffer arg (map with __pba__ context) -> PackedByteArray*.
static bool reqBuf(ZymVM* vm, ZymValue v, const char* where, PackedByteArray** out) {
    if (zym_isMap(v)) {
        ZymValue ctx = zym_mapGet(vm, v, "__pba__");
        if (ctx != ZYM_ERROR) {
            void* data = zym_getNativeData(ctx);
            if (data) { *out = static_cast<PackedByteArray*>(data); return true; }
        }
    }
    zym_runtimeError(vm, "%s expects a Buffer", where);
    return false;
}

static IPAddress parseBindHost(const String& host) {
    if (host.is_empty() || host == "*") return IPAddress("*");
    return IPAddress(host);
}

static const char* tcpStatusName(StreamPeerSocket::Status s) {
    switch (s) {
        case StreamPeerSocket::STATUS_NONE:       return "none";
        case StreamPeerSocket::STATUS_CONNECTING: return "connecting";
        case StreamPeerSocket::STATUS_CONNECTED:  return "connected";
        case StreamPeerSocket::STATUS_ERROR:      return "error";
    }
    return "error";
}

// TLS status -> shared status-string vocabulary. HANDSHAKING maps to
// "connecting" (handshake = TLS-level connection in progress), and the
// two error variants collapse onto "error" (the hostname-mismatch case
// is documented in docs/sockets.md).
static const char* tlsStatusName(StreamPeerTLS::Status s) {
    switch (s) {
        case StreamPeerTLS::STATUS_DISCONNECTED:            return "closed";
        case StreamPeerTLS::STATUS_HANDSHAKING:             return "connecting";
        case StreamPeerTLS::STATUS_CONNECTED:               return "connected";
        case StreamPeerTLS::STATUS_ERROR:                   return "error";
        case StreamPeerTLS::STATUS_ERROR_HOSTNAME_MISMATCH: return "error";
    }
    return "error";
}

static ZymValue addrMap(ZymVM* vm, const IPAddress& ip, int port) {
    ZymValue m = zym_newMap(vm);
    zym_pushRoot(vm, m);
    zym_mapSet(vm, m, "host", strZ(vm, String(ip)));
    zym_mapSet(vm, m, "port", zym_newNumber((double)port));
    zym_popRoot(vm);
    return m;
}

static TcpHandle*  unwrapTcp (ZymValue ctx) { return static_cast<TcpHandle* >(zym_getNativeData(ctx)); }
static TcpsHandle* unwrapTcps(ZymValue ctx) { return static_cast<TcpsHandle*>(zym_getNativeData(ctx)); }
static UdpHandle*  unwrapUdp (ZymValue ctx) { return static_cast<UdpHandle* >(zym_getNativeData(ctx)); }
static TlsHandle*  unwrapTls (ZymValue ctx) { return static_cast<TlsHandle* >(zym_getNativeData(ctx)); }

// Forward decls.
static ZymValue makeTcpInstance(ZymVM* vm, Ref<StreamPeerTCP> s);
static ZymValue makeTcpsInstance(ZymVM* vm, Ref<TCPServer> s);
static ZymValue makeUdpInstance(ZymVM* vm, Ref<PacketPeerUDP> s);
static ZymValue makeTlsInstance(ZymVM* vm, Ref<StreamPeerTLS> tls, Ref<StreamPeerTCP> base);

ZymValue nativeTcp_create(ZymVM* vm);
ZymValue nativeUdp_create(ZymVM* vm);
ZymValue nativeSockets_create(ZymVM* vm);
ZymValue nativeTls_create(ZymVM* vm);

// ============================================================================
// TCP socket instance methods
// ============================================================================

static int64_t now_ms() { return (int64_t)OS::get_singleton()->get_ticks_msec(); }

// Compute remaining timeout in ms given a starting deadline_ms (-1 = forever)
// and current time. Returns -1 for "wait forever", 0 for "expired", positive
// for "remaining". Pass deadline_ms == 0 for "single-shot, never wait".
static int remainingMs(int64_t deadline_ms) {
    if (deadline_ms < 0) return -1;
    int64_t r = deadline_ms - now_ms();
    if (r <= 0) return 0;
    if (r > INT32_MAX) return INT32_MAX;
    return (int)r;
}

static int64_t deadlineFor(int64_t timeoutMs) {
    if (timeoutMs < 0) return -1;
    if (timeoutMs == 0) return now_ms(); // already expired -> single-shot
    return now_ms() + timeoutMs;
}

// Drain status from underlying sock. Calls poll() to advance state.
static StreamPeerSocket::Status pollStatus(Ref<StreamPeerTCP>& s) {
    s->poll();
    return s->get_status();
}

// status() -> "none"|"connecting"|"connected"|"error"
static ZymValue tcp_status(ZymVM* vm, ZymValue ctx) {
    TcpHandle* h = unwrapTcp(ctx);
    if (!h || h->s.is_null()) return strZ(vm, String("error"));
    return strZ(vm, String(tcpStatusName(h->s->get_status())));
}

// poll() -> advances state, returns new status string
static ZymValue tcp_poll(ZymVM* vm, ZymValue ctx) {
    TcpHandle* h = unwrapTcp(ctx);
    if (!h || h->s.is_null()) return strZ(vm, String("error"));
    h->s->poll();
    return strZ(vm, String(tcpStatusName(h->s->get_status())));
}

// available() -> integer bytes immediately readable
static ZymValue tcp_available(ZymVM* vm, ZymValue ctx) {
    TcpHandle* h = unwrapTcp(ctx);
    if (!h || h->s.is_null()) return zym_newNumber(0.0);
    h->s->poll();
    return zym_newNumber((double)h->s->get_available_bytes());
}

// readSome(n) -> Buffer (possibly 0 bytes); on closed/error returns null
static ZymValue tcp_readSome(ZymVM* vm, ZymValue ctx, ZymValue nV) {
    TcpHandle* h = unwrapTcp(ctx);
    if (!h || h->s.is_null()) { zym_runtimeError(vm, "TCP.readSome(n) on closed socket"); return ZYM_ERROR; }
    double nd; if (!reqNum(vm, nV, "TCP.readSome(n)", &nd)) return ZYM_ERROR;
    if (nd < 0) { zym_runtimeError(vm, "TCP.readSome(n): n must be non-negative"); return ZYM_ERROR; }
    h->s->poll();
    if (h->s->get_status() != StreamPeerSocket::STATUS_CONNECTED) return zym_newNull();
    int n = (int)nd;
    if (n == 0) { PackedByteArray empty; return makeBufferInstance(vm, empty); }
    PackedByteArray buf; buf.resize(n);
    int got = 0;
    Error err = h->s->get_partial_data(buf.ptrw(), n, got);
    if (err != OK) return zym_newNull();
    buf.resize(got);
    return makeBufferInstance(vm, buf);
}

// read(n, ...) -> Buffer (exactly n bytes) | "timeout" | "eof" | "closed" | "error"
static ZymValue tcp_read(ZymVM* vm, ZymValue ctx, ZymValue nV, ZymValue* vargs, int vargc) {
    TcpHandle* h = unwrapTcp(ctx);
    if (!h || h->s.is_null()) return strZ(vm, String("closed"));
    double nd; if (!reqNum(vm, nV, "TCP.read(n, ...)", &nd)) return ZYM_ERROR;
    if (nd < 0) { zym_runtimeError(vm, "TCP.read(n, ...): n must be non-negative"); return ZYM_ERROR; }
    int64_t timeoutMs = -1;
    if (optInt(vm, "TCP.read(n, ...)", vargs, vargc, 0, &timeoutMs) < 0) return ZYM_ERROR;
    int n = (int)nd;
    if (n == 0) { PackedByteArray empty; return makeBufferInstance(vm, empty); }
    PackedByteArray buf; buf.resize(n);
    int filled = 0;
    int64_t deadline = deadlineFor(timeoutMs);
    while (filled < n) {
        h->s->poll();
        StreamPeerSocket::Status st = h->s->get_status();
        if (st == StreamPeerSocket::STATUS_ERROR) return strZ(vm, String("error"));
        if (st == StreamPeerSocket::STATUS_NONE)  return strZ(vm, String("closed"));
        if (st == StreamPeerSocket::STATUS_CONNECTED) {
            int got = 0;
            Error err = h->s->get_partial_data(buf.ptrw() + filled, n - filled, got);
            if (err != OK) {
                // Disconnect path inside Godot already set status; loop will catch it
                continue;
            }
            if (got > 0) {
                filled += got;
                continue;
            }
            // got == 0: nothing ready, fall through to wait
        }
        // Need to wait
        int rem = remainingMs(deadline);
        if (timeoutMs >= 0 && rem == 0) return strZ(vm, String("timeout"));
        // Use NetSocket-level wait via poll(IN, ms). StreamPeerSocket doesn't
        // expose wait() publicly, so fall back to sleep-quantum.
        int q = (rem < 0 || rem > 20) ? 20 : rem;
        OS::get_singleton()->delay_usec((uint32_t)q * 1000u);
    }
    return makeBufferInstance(vm, buf);
}

// readLine(...) -> string (no terminator) | "timeout" | "eof" | "closed" | "error"
// Strips both \r\n and \n. Returns "eof" if connection closed before any
// terminator was seen (any partial bytes are discarded).
static ZymValue tcp_readLine(ZymVM* vm, ZymValue ctx, ZymValue* vargs, int vargc) {
    TcpHandle* h = unwrapTcp(ctx);
    if (!h || h->s.is_null()) return strZ(vm, String("closed"));
    int64_t timeoutMs = -1;
    if (optInt(vm, "TCP.readLine(...)", vargs, vargc, 0, &timeoutMs) < 0) return ZYM_ERROR;
    int64_t deadline = deadlineFor(timeoutMs);
    PackedByteArray buf;
    while (true) {
        h->s->poll();
        StreamPeerSocket::Status st = h->s->get_status();
        if (st == StreamPeerSocket::STATUS_ERROR) return strZ(vm, String("error"));
        if (st == StreamPeerSocket::STATUS_NONE)  return strZ(vm, String("eof"));
        if (st == StreamPeerSocket::STATUS_CONNECTED) {
            int avail = h->s->get_available_bytes();
            if (avail > 0) {
                int prev = buf.size();
                buf.resize(prev + avail);
                int got = 0;
                Error err = h->s->get_partial_data(buf.ptrw() + prev, avail, got);
                if (err != OK) { buf.resize(prev); continue; }
                buf.resize(prev + got);
                // scan for \n
                const uint8_t* p = buf.ptr();
                for (int i = prev; i < buf.size(); i++) {
                    if (p[i] == '\n') {
                        int end = i;
                        if (end > 0 && p[end - 1] == '\r') end--;
                        String line = String::utf8((const char*)p, end);
                        // discard the consumed bytes (incl. \n); we don't keep
                        // the tail because StreamPeer has no pushback channel.
                        // Any trailing data after \n is lost -- documented.
                        return strZ(vm, line);
                    }
                }
                continue;
            }
        }
        int rem = remainingMs(deadline);
        if (timeoutMs >= 0 && rem == 0) return strZ(vm, String("timeout"));
        int q = (rem < 0 || rem > 20) ? 20 : rem;
        OS::get_singleton()->delay_usec((uint32_t)q * 1000u);
    }
}

// readAll(...) -> Buffer until EOF, or "timeout"/"error"
static ZymValue tcp_readAll(ZymVM* vm, ZymValue ctx, ZymValue* vargs, int vargc) {
    TcpHandle* h = unwrapTcp(ctx);
    if (!h || h->s.is_null()) return strZ(vm, String("closed"));
    int64_t timeoutMs = -1;
    if (optInt(vm, "TCP.readAll(...)", vargs, vargc, 0, &timeoutMs) < 0) return ZYM_ERROR;
    int64_t deadline = deadlineFor(timeoutMs);
    PackedByteArray buf;
    while (true) {
        h->s->poll();
        StreamPeerSocket::Status st = h->s->get_status();
        if (st == StreamPeerSocket::STATUS_ERROR) return strZ(vm, String("error"));
        if (st == StreamPeerSocket::STATUS_NONE)  return makeBufferInstance(vm, buf);
        if (st == StreamPeerSocket::STATUS_CONNECTED) {
            int avail = h->s->get_available_bytes();
            if (avail > 0) {
                int prev = buf.size();
                buf.resize(prev + avail);
                int got = 0;
                Error err = h->s->get_partial_data(buf.ptrw() + prev, avail, got);
                if (err != OK) { buf.resize(prev); continue; }
                buf.resize(prev + got);
                continue;
            }
        }
        int rem = remainingMs(deadline);
        if (timeoutMs >= 0 && rem == 0) return strZ(vm, String("timeout"));
        int q = (rem < 0 || rem > 20) ? 20 : rem;
        OS::get_singleton()->delay_usec((uint32_t)q * 1000u);
    }
}

// writeSome(buf) -> { sent: int, status: "ok"|"closed"|"error" }
static ZymValue tcp_writeSome(ZymVM* vm, ZymValue ctx, ZymValue bufV) {
    TcpHandle* h = unwrapTcp(ctx);
    PackedByteArray* pba; if (!reqBuf(vm, bufV, "TCP.writeSome(buf)", &pba)) return ZYM_ERROR;
    ZymValue m = zym_newMap(vm);
    zym_pushRoot(vm, m);
    if (!h || h->s.is_null()) {
        zym_mapSet(vm, m, "sent",   zym_newNumber(0.0));
        zym_mapSet(vm, m, "status", strZ(vm, String("closed")));
        zym_popRoot(vm); return m;
    }
    h->s->poll();
    if (h->s->get_status() != StreamPeerSocket::STATUS_CONNECTED) {
        zym_mapSet(vm, m, "sent",   zym_newNumber(0.0));
        zym_mapSet(vm, m, "status", strZ(vm, String(h->s->get_status() == StreamPeerSocket::STATUS_ERROR ? "error" : "closed")));
        zym_popRoot(vm); return m;
    }
    int sent = 0;
    Error err = pba->size() == 0
        ? OK
        : h->s->put_partial_data(pba->ptr(), pba->size(), sent);
    const char* st = "ok";
    if (err != OK) st = (h->s->get_status() == StreamPeerSocket::STATUS_ERROR) ? "error" : "closed";
    zym_mapSet(vm, m, "sent",   zym_newNumber((double)sent));
    zym_mapSet(vm, m, "status", strZ(vm, String(st)));
    zym_popRoot(vm);
    return m;
}

// write(buf, ...) -> "ok" | "timeout" | "closed" | "error"
static ZymValue tcp_write(ZymVM* vm, ZymValue ctx, ZymValue bufV, ZymValue* vargs, int vargc) {
    TcpHandle* h = unwrapTcp(ctx);
    PackedByteArray* pba; if (!reqBuf(vm, bufV, "TCP.write(buf, ...)", &pba)) return ZYM_ERROR;
    int64_t timeoutMs = -1;
    if (optInt(vm, "TCP.write(buf, ...)", vargs, vargc, 0, &timeoutMs) < 0) return ZYM_ERROR;
    if (!h || h->s.is_null()) return strZ(vm, String("closed"));
    int total = pba->size();
    if (total == 0) return strZ(vm, String("ok"));
    int sent_total = 0;
    int64_t deadline = deadlineFor(timeoutMs);
    while (sent_total < total) {
        h->s->poll();
        StreamPeerSocket::Status st = h->s->get_status();
        if (st == StreamPeerSocket::STATUS_ERROR) return strZ(vm, String("error"));
        if (st == StreamPeerSocket::STATUS_NONE)  return strZ(vm, String("closed"));
        if (st == StreamPeerSocket::STATUS_CONNECTED) {
            int sent = 0;
            Error err = h->s->put_partial_data(pba->ptr() + sent_total, total - sent_total, sent);
            if (err != OK) continue;
            sent_total += sent;
            if (sent_total >= total) break;
            if (sent > 0) continue;
        }
        int rem = remainingMs(deadline);
        if (timeoutMs >= 0 && rem == 0) return strZ(vm, String("timeout"));
        int q = (rem < 0 || rem > 20) ? 20 : rem;
        OS::get_singleton()->delay_usec((uint32_t)q * 1000u);
    }
    return strZ(vm, String("ok"));
}

static ZymValue tcp_setNoDelay(ZymVM* vm, ZymValue ctx, ZymValue v) {
    TcpHandle* h = unwrapTcp(ctx);
    bool on; if (!reqBool(vm, v, "TCP.setNoDelay(b)", &on)) return ZYM_ERROR;
    if (h && h->s.is_valid()) h->s->set_no_delay(on);
    return zym_newNull();
}

static ZymValue tcp_localAddress(ZymVM* vm, ZymValue ctx) {
    TcpHandle* h = unwrapTcp(ctx);
    if (!h || h->s.is_null()) return zym_newNull();
    return addrMap(vm, IPAddress(), h->s->get_local_port());
}

static ZymValue tcp_peerAddress(ZymVM* vm, ZymValue ctx) {
    TcpHandle* h = unwrapTcp(ctx);
    if (!h || h->s.is_null()) return zym_newNull();
    return addrMap(vm, h->s->get_connected_host(), h->s->get_connected_port());
}

static ZymValue tcp_close(ZymVM* vm, ZymValue ctx) {
    TcpHandle* h = unwrapTcp(ctx);
    if (h && h->s.is_valid()) h->s->disconnect_from_host();
    return zym_newNull();
}

// ============================================================================
// TCP server instance methods
// ============================================================================

static ZymValue tcps_localPort(ZymVM* vm, ZymValue ctx) {
    TcpsHandle* h = unwrapTcps(ctx);
    if (!h || h->s.is_null()) return zym_newNumber(0.0);
    return zym_newNumber((double)h->s->get_local_port());
}

static ZymValue tcps_close(ZymVM* vm, ZymValue ctx) {
    TcpsHandle* h = unwrapTcps(ctx);
    if (h && h->s.is_valid()) h->s->stop();
    return zym_newNull();
}

// accept(...) -> sock | null. Optional timeoutMs (-1 default = block forever).
static ZymValue tcps_accept(ZymVM* vm, ZymValue ctx, ZymValue* vargs, int vargc) {
    TcpsHandle* h = unwrapTcps(ctx);
    if (!h || h->s.is_null()) return zym_newNull();
    int64_t timeoutMs = -1;
    if (optInt(vm, "TCPServer.accept(...)", vargs, vargc, 0, &timeoutMs) < 0) return ZYM_ERROR;
    int64_t deadline = deadlineFor(timeoutMs);
    while (true) {
        if (!h->s->is_listening()) return zym_newNull();
        if (h->s->is_connection_available()) {
            Ref<StreamPeerTCP> s = h->s->take_connection();
            if (s.is_null()) return zym_newNull();
            return makeTcpInstance(vm, s);
        }
        int rem = remainingMs(deadline);
        if (timeoutMs >= 0 && rem == 0) return zym_newNull();
        int q = (rem < 0 || rem > 20) ? 20 : rem;
        OS::get_singleton()->delay_usec((uint32_t)q * 1000u);
    }
}

// ============================================================================
// TCP global statics
// ============================================================================

// connect(host, port, ...) -> sock | null
// Optional timeoutMs (-1 default = block until connected or error).
// timeoutMs == 0 returns immediately with status "connecting".
static ZymValue f_tcpConnect(ZymVM* vm, ZymValue, ZymValue hostV, ZymValue portV, ZymValue* vargs, int vargc) {
    String host;  if (!reqStr(vm, hostV, "TCP.connect(host, port, ...)", &host)) return ZYM_ERROR;
    double portD; if (!reqNum(vm, portV, "TCP.connect(host, port, ...)", &portD)) return ZYM_ERROR;
    int64_t timeoutMs = -1;
    if (optInt(vm, "TCP.connect(host, port, ...)", vargs, vargc, 0, &timeoutMs) < 0) return ZYM_ERROR;
    IP* ip = IP::get_singleton();
    if (!ip) { zym_runtimeError(vm, "TCP.connect(host, port, ...): IP singleton missing"); return ZYM_ERROR; }
    IPAddress addr = host.is_valid_ip_address() ? IPAddress(host) : ip->resolve_hostname(host, IP::TYPE_ANY);
    if (!addr.is_valid()) return zym_newNull();
    Ref<StreamPeerTCP> s; s.instantiate();
    Error err = s->connect_to_host(addr, (int)portD);
    if (err != OK) return zym_newNull();
    int64_t deadline = deadlineFor(timeoutMs);
    while (true) {
        s->poll();
        StreamPeerSocket::Status st = s->get_status();
        if (st == StreamPeerSocket::STATUS_CONNECTED) break;
        if (st == StreamPeerSocket::STATUS_ERROR)     return zym_newNull();
        if (st == StreamPeerSocket::STATUS_NONE)      return zym_newNull();
        int rem = remainingMs(deadline);
        if (timeoutMs == 0) break; // non-waiting: return now with status="connecting"
        if (timeoutMs > 0 && rem == 0) return zym_newNull();
        int q = (rem < 0 || rem > 20) ? 20 : rem;
        OS::get_singleton()->delay_usec((uint32_t)q * 1000u);
    }
    return makeTcpInstance(vm, s);
}

// listen(host, port) -> server | null
static ZymValue f_tcpListen(ZymVM* vm, ZymValue, ZymValue hostV, ZymValue portV) {
    String host;  if (!reqStr(vm, hostV, "TCP.listen(host, port)", &host)) return ZYM_ERROR;
    double portD; if (!reqNum(vm, portV, "TCP.listen(host, port)", &portD)) return ZYM_ERROR;
    Ref<TCPServer> srv; srv.instantiate();
    Error err = srv->listen((uint16_t)portD, parseBindHost(host));
    if (err != OK) return zym_newNull();
    return makeTcpsInstance(vm, srv);
}

// ============================================================================
// UDP instance methods
// ============================================================================

// send(buf, host, port) -> "ok" | "busy" | "error"
static ZymValue udp_send(ZymVM* vm, ZymValue ctx, ZymValue bufV, ZymValue hostV, ZymValue portV) {
    UdpHandle* h = unwrapUdp(ctx);
    PackedByteArray* pba; if (!reqBuf(vm, bufV, "UDP.send(buf, host, port)", &pba)) return ZYM_ERROR;
    String host;  if (!reqStr(vm, hostV, "UDP.send(buf, host, port)", &host)) return ZYM_ERROR;
    double portD; if (!reqNum(vm, portV, "UDP.send(buf, host, port)", &portD)) return ZYM_ERROR;
    if (!h || h->s.is_null() || !h->s->is_bound()) return strZ(vm, String("error"));
    IPAddress addr;
    if (host.is_valid_ip_address()) {
        addr = IPAddress(host);
    } else {
        IP* ip = IP::get_singleton();
        if (!ip) return strZ(vm, String("error"));
        addr = ip->resolve_hostname(host, IP::TYPE_ANY);
    }
    if (!addr.is_valid()) return strZ(vm, String("error"));
    h->s->set_dest_address(addr, (int)portD);
    Error err = h->s->put_packet(pba->ptr(), pba->size());
    if (err == OK) return strZ(vm, String("ok"));
    if (err == ERR_BUSY) return strZ(vm, String("busy"));
    return strZ(vm, String("error"));
}

// recv(...) -> { data: Buffer, host, port } | "timeout" | "error"
static ZymValue udp_recv(ZymVM* vm, ZymValue ctx, ZymValue* vargs, int vargc) {
    UdpHandle* h = unwrapUdp(ctx);
    if (!h || h->s.is_null() || !h->s->is_bound()) return strZ(vm, String("error"));
    int64_t timeoutMs = -1;
    if (optInt(vm, "UDP.recv(...)", vargs, vargc, 0, &timeoutMs) < 0) return ZYM_ERROR;
    int64_t deadline = deadlineFor(timeoutMs);
    while (true) {
        if (h->s->get_available_packet_count() > 0) {
            const uint8_t* p = nullptr;
            int sz = 0;
            Error err = h->s->get_packet(&p, sz);
            if (err != OK) return strZ(vm, String("error"));
            PackedByteArray buf; buf.resize(sz);
            if (sz > 0) memcpy(buf.ptrw(), p, sz);
            ZymValue m = zym_newMap(vm);
            zym_pushRoot(vm, m);
            ZymValue dataV = makeBufferInstance(vm, buf);
            zym_mapSet(vm, m, "data", dataV);
            zym_mapSet(vm, m, "host", strZ(vm, String(h->s->get_packet_address())));
            zym_mapSet(vm, m, "port", zym_newNumber((double)h->s->get_packet_port()));
            zym_popRoot(vm);
            return m;
        }
        int rem = remainingMs(deadline);
        if (timeoutMs >= 0 && rem == 0) return strZ(vm, String("timeout"));
        int q = (rem < 0 || rem > 20) ? 20 : rem;
        OS::get_singleton()->delay_usec((uint32_t)q * 1000u);
    }
}

static ZymValue udp_localPort(ZymVM* vm, ZymValue ctx) {
    UdpHandle* h = unwrapUdp(ctx);
    if (!h || h->s.is_null()) return zym_newNumber(0.0);
    return zym_newNumber((double)h->s->get_local_port());
}

static ZymValue udp_setBroadcast(ZymVM* vm, ZymValue ctx, ZymValue v) {
    UdpHandle* h = unwrapUdp(ctx);
    bool on; if (!reqBool(vm, v, "UDP.setBroadcast(b)", &on)) return ZYM_ERROR;
    if (h && h->s.is_valid()) h->s->set_broadcast_enabled(on);
    return zym_newNull();
}

static ZymValue udp_close(ZymVM* vm, ZymValue ctx) {
    UdpHandle* h = unwrapUdp(ctx);
    if (h && h->s.is_valid()) h->s->close();
    return zym_newNull();
}

// bind(host, port) -> udp instance | null
static ZymValue f_udpBind(ZymVM* vm, ZymValue, ZymValue hostV, ZymValue portV) {
    String host;  if (!reqStr(vm, hostV, "UDP.bind(host, port)", &host)) return ZYM_ERROR;
    double portD; if (!reqNum(vm, portV, "UDP.bind(host, port)", &portD)) return ZYM_ERROR;
    Ref<PacketPeerUDP> p; p.instantiate();
    p->set_blocking_mode(false);
    Error err = p->bind((int)portD, parseBindHost(host));
    if (err != OK) return zym_newNull();
    return makeUdpInstance(vm, p);
}

// ============================================================================
// TLS socket instance methods
// ============================================================================
//
// Mirrors the TCP surface but operates on a Ref<StreamPeerTLS> + the inner
// Ref<StreamPeerTCP> (kept alive in the same handle). Status string vocab
// is shared with TCP via the conventions doc; HANDSHAKING collapses onto
// "connecting" so script code can poll the same way it does for TCP.

static ZymValue tls_status(ZymVM* vm, ZymValue ctx) {
    TlsHandle* h = unwrapTls(ctx);
    if (!h || h->tls.is_null()) return strZ(vm, String("error"));
    return strZ(vm, String(tlsStatusName(h->tls->get_status())));
}

static ZymValue tls_poll(ZymVM* vm, ZymValue ctx) {
    TlsHandle* h = unwrapTls(ctx);
    if (!h || h->tls.is_null()) return strZ(vm, String("error"));
    h->tls->poll();
    return strZ(vm, String(tlsStatusName(h->tls->get_status())));
}

static ZymValue tls_available(ZymVM* vm, ZymValue ctx) {
    TlsHandle* h = unwrapTls(ctx);
    if (!h || h->tls.is_null()) return zym_newNumber(0.0);
    h->tls->poll();
    return zym_newNumber((double)h->tls->get_available_bytes());
}

static ZymValue tls_readSome(ZymVM* vm, ZymValue ctx, ZymValue nV) {
    TlsHandle* h = unwrapTls(ctx);
    if (!h || h->tls.is_null()) { zym_runtimeError(vm, "TLS.readSome(n) on closed socket"); return ZYM_ERROR; }
    double nd; if (!reqNum(vm, nV, "TLS.readSome(n)", &nd)) return ZYM_ERROR;
    if (nd < 0) { zym_runtimeError(vm, "TLS.readSome(n): n must be non-negative"); return ZYM_ERROR; }
    h->tls->poll();
    if (h->tls->get_status() != StreamPeerTLS::STATUS_CONNECTED) return zym_newNull();
    int n = (int)nd;
    if (n == 0) { PackedByteArray empty; return makeBufferInstance(vm, empty); }
    PackedByteArray buf; buf.resize(n);
    int got = 0;
    Error err = h->tls->get_partial_data(buf.ptrw(), n, got);
    if (err != OK) return zym_newNull();
    buf.resize(got);
    return makeBufferInstance(vm, buf);
}

static ZymValue tls_read(ZymVM* vm, ZymValue ctx, ZymValue nV, ZymValue* vargs, int vargc) {
    TlsHandle* h = unwrapTls(ctx);
    if (!h || h->tls.is_null()) return strZ(vm, String("closed"));
    double nd; if (!reqNum(vm, nV, "TLS.read(n, ...)", &nd)) return ZYM_ERROR;
    if (nd < 0) { zym_runtimeError(vm, "TLS.read(n, ...): n must be non-negative"); return ZYM_ERROR; }
    int64_t timeoutMs = -1;
    if (optInt(vm, "TLS.read(n, ...)", vargs, vargc, 0, &timeoutMs) < 0) return ZYM_ERROR;
    int n = (int)nd;
    if (n == 0) { PackedByteArray empty; return makeBufferInstance(vm, empty); }
    PackedByteArray buf; buf.resize(n);
    int filled = 0;
    int64_t deadline = deadlineFor(timeoutMs);
    while (filled < n) {
        h->tls->poll();
        StreamPeerTLS::Status st = h->tls->get_status();
        if (st == StreamPeerTLS::STATUS_ERROR ||
            st == StreamPeerTLS::STATUS_ERROR_HOSTNAME_MISMATCH) return strZ(vm, String("error"));
        if (st == StreamPeerTLS::STATUS_DISCONNECTED) return strZ(vm, String("closed"));
        if (st == StreamPeerTLS::STATUS_CONNECTED) {
            int got = 0;
            Error err = h->tls->get_partial_data(buf.ptrw() + filled, n - filled, got);
            if (err != OK) continue;
            if (got > 0) { filled += got; continue; }
        }
        int rem = remainingMs(deadline);
        if (timeoutMs >= 0 && rem == 0) return strZ(vm, String("timeout"));
        int q = (rem < 0 || rem > 20) ? 20 : rem;
        OS::get_singleton()->delay_usec((uint32_t)q * 1000u);
    }
    return makeBufferInstance(vm, buf);
}

static ZymValue tls_readLine(ZymVM* vm, ZymValue ctx, ZymValue* vargs, int vargc) {
    TlsHandle* h = unwrapTls(ctx);
    if (!h || h->tls.is_null()) return strZ(vm, String("closed"));
    int64_t timeoutMs = -1;
    if (optInt(vm, "TLS.readLine(...)", vargs, vargc, 0, &timeoutMs) < 0) return ZYM_ERROR;
    int64_t deadline = deadlineFor(timeoutMs);
    PackedByteArray buf;
    while (true) {
        h->tls->poll();
        StreamPeerTLS::Status st = h->tls->get_status();
        if (st == StreamPeerTLS::STATUS_ERROR ||
            st == StreamPeerTLS::STATUS_ERROR_HOSTNAME_MISMATCH) return strZ(vm, String("error"));
        if (st == StreamPeerTLS::STATUS_DISCONNECTED) return strZ(vm, String("eof"));
        if (st == StreamPeerTLS::STATUS_CONNECTED) {
            int avail = h->tls->get_available_bytes();
            if (avail > 0) {
                int prev = buf.size();
                buf.resize(prev + avail);
                int got = 0;
                Error err = h->tls->get_partial_data(buf.ptrw() + prev, avail, got);
                if (err != OK) { buf.resize(prev); continue; }
                buf.resize(prev + got);
                const uint8_t* p = buf.ptr();
                for (int i = prev; i < buf.size(); i++) {
                    if (p[i] == '\n') {
                        int end = i;
                        if (end > 0 && p[end - 1] == '\r') end--;
                        String line = String::utf8((const char*)p, end);
                        return strZ(vm, line);
                    }
                }
                continue;
            }
        }
        int rem = remainingMs(deadline);
        if (timeoutMs >= 0 && rem == 0) return strZ(vm, String("timeout"));
        int q = (rem < 0 || rem > 20) ? 20 : rem;
        OS::get_singleton()->delay_usec((uint32_t)q * 1000u);
    }
}

static ZymValue tls_readAll(ZymVM* vm, ZymValue ctx, ZymValue* vargs, int vargc) {
    TlsHandle* h = unwrapTls(ctx);
    if (!h || h->tls.is_null()) return strZ(vm, String("closed"));
    int64_t timeoutMs = -1;
    if (optInt(vm, "TLS.readAll(...)", vargs, vargc, 0, &timeoutMs) < 0) return ZYM_ERROR;
    int64_t deadline = deadlineFor(timeoutMs);
    PackedByteArray buf;
    while (true) {
        h->tls->poll();
        StreamPeerTLS::Status st = h->tls->get_status();
        if (st == StreamPeerTLS::STATUS_ERROR ||
            st == StreamPeerTLS::STATUS_ERROR_HOSTNAME_MISMATCH) return strZ(vm, String("error"));
        if (st == StreamPeerTLS::STATUS_DISCONNECTED) return makeBufferInstance(vm, buf);
        if (st == StreamPeerTLS::STATUS_CONNECTED) {
            int avail = h->tls->get_available_bytes();
            if (avail > 0) {
                int prev = buf.size();
                buf.resize(prev + avail);
                int got = 0;
                Error err = h->tls->get_partial_data(buf.ptrw() + prev, avail, got);
                if (err != OK) { buf.resize(prev); continue; }
                buf.resize(prev + got);
                continue;
            }
        }
        int rem = remainingMs(deadline);
        if (timeoutMs >= 0 && rem == 0) return strZ(vm, String("timeout"));
        int q = (rem < 0 || rem > 20) ? 20 : rem;
        OS::get_singleton()->delay_usec((uint32_t)q * 1000u);
    }
}

static ZymValue tls_writeSome(ZymVM* vm, ZymValue ctx, ZymValue bufV) {
    TlsHandle* h = unwrapTls(ctx);
    PackedByteArray* pba; if (!reqBuf(vm, bufV, "TLS.writeSome(buf)", &pba)) return ZYM_ERROR;
    ZymValue m = zym_newMap(vm);
    zym_pushRoot(vm, m);
    if (!h || h->tls.is_null()) {
        zym_mapSet(vm, m, "sent",   zym_newNumber(0.0));
        zym_mapSet(vm, m, "status", strZ(vm, String("closed")));
        zym_popRoot(vm); return m;
    }
    h->tls->poll();
    StreamPeerTLS::Status st = h->tls->get_status();
    if (st != StreamPeerTLS::STATUS_CONNECTED) {
        zym_mapSet(vm, m, "sent",   zym_newNumber(0.0));
        const char* s = (st == StreamPeerTLS::STATUS_ERROR ||
                         st == StreamPeerTLS::STATUS_ERROR_HOSTNAME_MISMATCH) ? "error"
                       : (st == StreamPeerTLS::STATUS_HANDSHAKING) ? "connecting" : "closed";
        zym_mapSet(vm, m, "status", strZ(vm, String(s)));
        zym_popRoot(vm); return m;
    }
    int sent = 0;
    Error err = pba->size() == 0 ? OK : h->tls->put_partial_data(pba->ptr(), pba->size(), sent);
    const char* s = "ok";
    if (err != OK) {
        StreamPeerTLS::Status st2 = h->tls->get_status();
        s = (st2 == StreamPeerTLS::STATUS_ERROR ||
             st2 == StreamPeerTLS::STATUS_ERROR_HOSTNAME_MISMATCH) ? "error" : "closed";
    }
    zym_mapSet(vm, m, "sent",   zym_newNumber((double)sent));
    zym_mapSet(vm, m, "status", strZ(vm, String(s)));
    zym_popRoot(vm);
    return m;
}

static ZymValue tls_write(ZymVM* vm, ZymValue ctx, ZymValue bufV, ZymValue* vargs, int vargc) {
    TlsHandle* h = unwrapTls(ctx);
    PackedByteArray* pba; if (!reqBuf(vm, bufV, "TLS.write(buf, ...)", &pba)) return ZYM_ERROR;
    int64_t timeoutMs = -1;
    if (optInt(vm, "TLS.write(buf, ...)", vargs, vargc, 0, &timeoutMs) < 0) return ZYM_ERROR;
    if (!h || h->tls.is_null()) return strZ(vm, String("closed"));
    int total = pba->size();
    if (total == 0) return strZ(vm, String("ok"));
    int sent_total = 0;
    int64_t deadline = deadlineFor(timeoutMs);
    while (sent_total < total) {
        h->tls->poll();
        StreamPeerTLS::Status st = h->tls->get_status();
        if (st == StreamPeerTLS::STATUS_ERROR ||
            st == StreamPeerTLS::STATUS_ERROR_HOSTNAME_MISMATCH) return strZ(vm, String("error"));
        if (st == StreamPeerTLS::STATUS_DISCONNECTED) return strZ(vm, String("closed"));
        if (st == StreamPeerTLS::STATUS_CONNECTED) {
            int sent = 0;
            Error err = h->tls->put_partial_data(pba->ptr() + sent_total, total - sent_total, sent);
            if (err != OK) continue;
            sent_total += sent;
            if (sent_total >= total) break;
            if (sent > 0) continue;
        }
        int rem = remainingMs(deadline);
        if (timeoutMs >= 0 && rem == 0) return strZ(vm, String("timeout"));
        int q = (rem < 0 || rem > 20) ? 20 : rem;
        OS::get_singleton()->delay_usec((uint32_t)q * 1000u);
    }
    return strZ(vm, String("ok"));
}

// setNoDelay / localAddress / peerAddress all delegate to the inner TCP
// socket. The TLS layer doesn't surface these directly -- they live on
// the transport, not on the encryption layer.
static ZymValue tls_setNoDelay(ZymVM* vm, ZymValue ctx, ZymValue v) {
    TlsHandle* h = unwrapTls(ctx);
    bool on; if (!reqBool(vm, v, "TLS.setNoDelay(b)", &on)) return ZYM_ERROR;
    if (h && h->base.is_valid()) h->base->set_no_delay(on);
    return zym_newNull();
}
static ZymValue tls_localAddress(ZymVM* vm, ZymValue ctx) {
    TlsHandle* h = unwrapTls(ctx);
    if (!h || h->base.is_null()) return zym_newNull();
    return addrMap(vm, IPAddress(), h->base->get_local_port());
}
static ZymValue tls_peerAddress(ZymVM* vm, ZymValue ctx) {
    TlsHandle* h = unwrapTls(ctx);
    if (!h || h->base.is_null()) return zym_newNull();
    return addrMap(vm, h->base->get_connected_host(), h->base->get_connected_port());
}
static ZymValue tls_close(ZymVM* vm, ZymValue ctx) {
    TlsHandle* h = unwrapTls(ctx);
    if (h && h->tls.is_valid())  h->tls->disconnect_from_stream();
    if (h && h->base.is_valid()) h->base->disconnect_from_host();
    return zym_newNull();
}

// ============================================================================
// TLS global statics
// ============================================================================
//
// connect(host, port, ...) -> tls sock | null
// Optional args (in order):
//   timeoutMs   (-1 default = block until handshake completes or fails)
//   opts        map: { verify: bool, trustedRoots: [X509Certificate],
//                      commonName: string }
//
// Verify defaults to TRUE. Pass `verify: false` for an "unsafe client"
// mode that skips chain & hostname validation (useful for self-signed
// peers in tests; documented as such). When trustedRoots is provided
// AND verify is true, those certs replace the system trust store for
// this connection.

static Ref<TLSOptions> buildClientOpts(ZymVM* vm, ZymValue optsV) {
    if (zym_isNull(optsV)) return TLSOptions::client();
    if (!zym_isMap(optsV)) {
        zym_runtimeError(vm, "TLS.connect(host, port, [timeoutMs, opts]): opts must be a map");
        return Ref<TLSOptions>();
    }
    bool verify = true;
    {
        ZymValue v = zym_mapGet(vm, optsV, "verify");
        if (v != ZYM_ERROR && !zym_isNull(v)) {
            if (!zym_isBool(v)) { zym_runtimeError(vm, "TLS.connect: opts.verify must be a bool"); return Ref<TLSOptions>(); }
            verify = zym_asBool(v);
        }
    }
    Ref<X509Certificate> trusted;
    {
        ZymValue v = zym_mapGet(vm, optsV, "trustedRoots");
        if (v != ZYM_ERROR && !zym_isNull(v)) {
            if (!zym_isList(v)) { zym_runtimeError(vm, "TLS.connect: opts.trustedRoots must be a list of X509Certificate"); return Ref<TLSOptions>(); }
            int n = zym_listLength(v);
            if (n > 0) {
                // Combine roots into a single X509Certificate by concatenating
                // PEMs (X509Certificate accepts a chain via load_from_string).
                String combined;
                for (int i = 0; i < n; i++) {
                    Ref<X509Certificate> one;
                    if (!zymExtractX509(vm, zym_listGet(vm, v, i), &one)) {
                        zym_runtimeError(vm, "TLS.connect: opts.trustedRoots[%d] is not an X509Certificate", i);
                        return Ref<TLSOptions>();
                    }
                    combined += one->save_to_string();
                }
                trusted = Ref<X509Certificate>(X509Certificate::create());
                if (trusted->load_from_string(combined) != OK) {
                    zym_runtimeError(vm, "TLS.connect: failed to combine opts.trustedRoots");
                    return Ref<TLSOptions>();
                }
            }
        }
    }
    String commonName;
    {
        ZymValue v = zym_mapGet(vm, optsV, "commonName");
        if (v != ZYM_ERROR && !zym_isNull(v)) {
            if (!zym_isString(v)) { zym_runtimeError(vm, "TLS.connect: opts.commonName must be a string"); return Ref<TLSOptions>(); }
            commonName = String::utf8(zym_asCString(v));
        }
    }
    if (!verify) return TLSOptions::client_unsafe(trusted);
    return TLSOptions::client(trusted, commonName);
}

static ZymValue f_tlsConnect(ZymVM* vm, ZymValue, ZymValue hostV, ZymValue portV, ZymValue* vargs, int vargc) {
    String host;  if (!reqStr(vm, hostV, "TLS.connect(host, port, ...)", &host)) return ZYM_ERROR;
    double portD; if (!reqNum(vm, portV, "TLS.connect(host, port, ...)", &portD)) return ZYM_ERROR;
    int64_t timeoutMs = -1;
    if (optInt(vm, "TLS.connect(host, port, ...)", vargs, vargc, 0, &timeoutMs) < 0) return ZYM_ERROR;
    ZymValue optsV = (vargc >= 2) ? vargs[1] : zym_newNull();
    Ref<TLSOptions> opts = buildClientOpts(vm, optsV);
    if (opts.is_null()) return ZYM_ERROR; // error already raised

    IP* ip = IP::get_singleton();
    if (!ip) { zym_runtimeError(vm, "TLS.connect: IP singleton missing"); return ZYM_ERROR; }
    IPAddress addr = host.is_valid_ip_address() ? IPAddress(host) : ip->resolve_hostname(host, IP::TYPE_ANY);
    if (!addr.is_valid()) return zym_newNull();

    // 1) Open the underlying TCP and drive it to CONNECTED. The mbedTLS
    // handshake's first record needs to be writable on the wire, so we
    // require a connected base before calling connect_to_stream(). With
    // timeoutMs == 0 we still wait briefly (up to TLS_BASE_GRACE_MS) for
    // the loopback / already-routed case; remote unreachable peers will
    // fall through to error/null promptly because the kernel will mark
    // the sock STATUS_ERROR within one quantum.
    const int TLS_BASE_GRACE_MS = 250;
    Ref<StreamPeerTCP> base; base.instantiate();
    if (base->connect_to_host(addr, (int)portD) != OK) return zym_newNull();
    int64_t baseDeadline = deadlineFor(timeoutMs == 0 ? TLS_BASE_GRACE_MS : timeoutMs);
    while (true) {
        base->poll();
        StreamPeerSocket::Status st = base->get_status();
        if (st == StreamPeerSocket::STATUS_CONNECTED) break;
        if (st == StreamPeerSocket::STATUS_ERROR) return zym_newNull();
        if (st == StreamPeerSocket::STATUS_NONE)  return zym_newNull();
        int rem = remainingMs(baseDeadline);
        if (rem == 0) return zym_newNull();
        int q = (rem < 0 || rem > 20) ? 20 : rem;
        OS::get_singleton()->delay_usec((uint32_t)q * 1000u);
    }
    int64_t deadline = deadlineFor(timeoutMs);

    // 2) Wrap with TLS and drive the handshake.
    Ref<StreamPeerTLS> tls = Ref<StreamPeerTLS>(StreamPeerTLS::create());
    if (tls.is_null()) { zym_runtimeError(vm, "TLS.connect: StreamPeerTLS unavailable (mbedtls module not built?)"); return ZYM_ERROR; }
    if (tls->connect_to_stream(base, host, opts) != OK) return zym_newNull();
    while (true) {
        tls->poll();
        StreamPeerTLS::Status st = tls->get_status();
        if (st == StreamPeerTLS::STATUS_CONNECTED) break;
        if (st == StreamPeerTLS::STATUS_ERROR ||
            st == StreamPeerTLS::STATUS_ERROR_HOSTNAME_MISMATCH ||
            st == StreamPeerTLS::STATUS_DISCONNECTED) return zym_newNull();
        int rem = remainingMs(deadline);
        if (timeoutMs == 0) break; // non-waiting: caller can poll() + status()
        if (timeoutMs > 0 && rem == 0) return zym_newNull();
        int q = (rem < 0 || rem > 20) ? 20 : rem;
        OS::get_singleton()->delay_usec((uint32_t)q * 1000u);
    }
    return makeTlsInstance(vm, tls, base);
}

// accept(base, opts) -- wrap an existing accepted TCP socket in TLS as the
// server side. Used by server-side TLS code; opts must be a server-side
// TLSOptions with key + cert. The factory mirrors `TLS.connect` shape.
static ZymValue f_tlsAccept(ZymVM* vm, ZymValue, ZymValue baseV, ZymValue optsV, ZymValue* vargs, int vargc) {
    if (!zym_isMap(baseV)) { zym_runtimeError(vm, "TLS.accept(tcp, opts, ...): tcp must be a TCP socket"); return ZYM_ERROR; }
    ZymValue ctx = zym_mapGet(vm, baseV, "__tcp__");
    if (ctx == ZYM_ERROR) { zym_runtimeError(vm, "TLS.accept(tcp, opts, ...): tcp must be a TCP socket"); return ZYM_ERROR; }
    TcpHandle* th = static_cast<TcpHandle*>(zym_getNativeData(ctx));
    if (!th || th->s.is_null()) { zym_runtimeError(vm, "TLS.accept: tcp socket is closed"); return ZYM_ERROR; }
    if (!zym_isMap(optsV)) { zym_runtimeError(vm, "TLS.accept(tcp, opts, ...): opts must be a map { key, cert }"); return ZYM_ERROR; }
    Ref<CryptoKey> key;
    Ref<X509Certificate> cert;
    {
        ZymValue v = zym_mapGet(vm, optsV, "key");
        if (v == ZYM_ERROR || !zymExtractCryptoKey(vm, v, &key)) {
            zym_runtimeError(vm, "TLS.accept: opts.key must be a CryptoKey"); return ZYM_ERROR;
        }
    }
    {
        ZymValue v = zym_mapGet(vm, optsV, "cert");
        if (v == ZYM_ERROR || !zymExtractX509(vm, v, &cert)) {
            zym_runtimeError(vm, "TLS.accept: opts.cert must be an X509Certificate"); return ZYM_ERROR;
        }
    }
    int64_t timeoutMs = -1;
    if (optInt(vm, "TLS.accept(tcp, opts, ...)", vargs, vargc, 0, &timeoutMs) < 0) return ZYM_ERROR;
    Ref<TLSOptions> serverOpts = TLSOptions::server(key, cert);
    if (serverOpts.is_null()) { zym_runtimeError(vm, "TLS.accept: failed to build server TLSOptions"); return ZYM_ERROR; }
    Ref<StreamPeerTLS> tls = Ref<StreamPeerTLS>(StreamPeerTLS::create());
    if (tls.is_null()) { zym_runtimeError(vm, "TLS.accept: StreamPeerTLS unavailable"); return ZYM_ERROR; }
    if (tls->accept_stream(th->s, serverOpts) != OK) return zym_newNull();
    int64_t deadline = deadlineFor(timeoutMs);
    while (true) {
        tls->poll();
        StreamPeerTLS::Status st = tls->get_status();
        if (st == StreamPeerTLS::STATUS_CONNECTED) break;
        if (st == StreamPeerTLS::STATUS_ERROR ||
            st == StreamPeerTLS::STATUS_ERROR_HOSTNAME_MISMATCH ||
            st == StreamPeerTLS::STATUS_DISCONNECTED) return zym_newNull();
        int rem = remainingMs(deadline);
        if (timeoutMs == 0) break;
        if (timeoutMs > 0 && rem == 0) return zym_newNull();
        int q = (rem < 0 || rem > 20) ? 20 : rem;
        OS::get_singleton()->delay_usec((uint32_t)q * 1000u);
    }
    return makeTlsInstance(vm, tls, th->s);
}

// ============================================================================
// Sockets.waitAny(handles, mode, timeoutMs)
// ============================================================================
//
// Returns { ready: [...handles...], timedOut: bool }. `mode` is "read",
// "write", or "any". Implemented as a poll-with-quantum loop over the
// existing readiness primitives (`available()` for TCP, `get_available_
// packet_count` for UDP, `is_connection_available` for TCP servers, plus
// terminal status detection so closed/error sockets register as readable).

enum WaitMode { WAIT_READ, WAIT_WRITE, WAIT_ANY };

static bool isReady(ZymVM* vm, ZymValue handle, WaitMode mode) {
    if (!zym_isMap(handle)) return false;
    // TCP socket
    {
        ZymValue ctx = zym_mapGet(vm, handle, "__tcp__");
        if (ctx != ZYM_ERROR) {
            TcpHandle* h = static_cast<TcpHandle*>(zym_getNativeData(ctx));
            if (!h || h->s.is_null()) return mode != WAIT_WRITE; // dead -> readable
            h->s->poll();
            StreamPeerSocket::Status st = h->s->get_status();
            bool readable = (st != StreamPeerSocket::STATUS_CONNECTING) &&
                            (h->s->get_available_bytes() > 0
                             || st == StreamPeerSocket::STATUS_NONE
                             || st == StreamPeerSocket::STATUS_ERROR);
            bool writable = (st == StreamPeerSocket::STATUS_CONNECTED)
                            || st == StreamPeerSocket::STATUS_ERROR
                            || st == StreamPeerSocket::STATUS_NONE;
            switch (mode) {
                case WAIT_READ:  return readable;
                case WAIT_WRITE: return writable;
                case WAIT_ANY:   return readable || writable;
            }
        }
    }
    // TCP server
    {
        ZymValue ctx = zym_mapGet(vm, handle, "__tcps__");
        if (ctx != ZYM_ERROR) {
            TcpsHandle* h = static_cast<TcpsHandle*>(zym_getNativeData(ctx));
            if (!h || h->s.is_null()) return true;
            return h->s->is_connection_available() || !h->s->is_listening();
        }
    }
    // UDP
    {
        ZymValue ctx = zym_mapGet(vm, handle, "__udp__");
        if (ctx != ZYM_ERROR) {
            UdpHandle* h = static_cast<UdpHandle*>(zym_getNativeData(ctx));
            if (!h || h->s.is_null()) return true;
            switch (mode) {
                case WAIT_READ:  return h->s->get_available_packet_count() > 0;
                case WAIT_WRITE: return h->s->is_bound();
                case WAIT_ANY:   return h->s->get_available_packet_count() > 0 || h->s->is_bound();
            }
        }
    }
    // TLS
    {
        ZymValue ctx = zym_mapGet(vm, handle, "__tls__");
        if (ctx != ZYM_ERROR) {
            TlsHandle* h = static_cast<TlsHandle*>(zym_getNativeData(ctx));
            if (!h || h->tls.is_null()) return mode != WAIT_WRITE;
            h->tls->poll();
            StreamPeerTLS::Status st = h->tls->get_status();
            bool dead = (st == StreamPeerTLS::STATUS_DISCONNECTED ||
                         st == StreamPeerTLS::STATUS_ERROR ||
                         st == StreamPeerTLS::STATUS_ERROR_HOSTNAME_MISMATCH);
            bool readable = dead || h->tls->get_available_bytes() > 0;
            bool writable = dead || st == StreamPeerTLS::STATUS_CONNECTED;
            switch (mode) {
                case WAIT_READ:  return readable;
                case WAIT_WRITE: return writable;
                case WAIT_ANY:   return readable || writable;
            }
        }
    }
    return false;
}

static ZymValue f_waitAny(ZymVM* vm, ZymValue, ZymValue handlesV, ZymValue modeV, ZymValue timeoutV) {
    if (!zym_isList(handlesV)) { zym_runtimeError(vm, "Sockets.waitAny(handles, mode, timeoutMs) expects a list of handles"); return ZYM_ERROR; }
    String mode; if (!reqStr(vm, modeV, "Sockets.waitAny(handles, mode, timeoutMs)", &mode)) return ZYM_ERROR;
    double tD;   if (!reqNum(vm, timeoutV, "Sockets.waitAny(handles, mode, timeoutMs)", &tD)) return ZYM_ERROR;
    int64_t timeoutMs = (int64_t)tD;
    WaitMode wm;
    if (mode == "read")       wm = WAIT_READ;
    else if (mode == "write") wm = WAIT_WRITE;
    else if (mode == "any")   wm = WAIT_ANY;
    else { zym_runtimeError(vm, "Sockets.waitAny: mode must be \"read\", \"write\", or \"any\""); return ZYM_ERROR; }
    int64_t deadline = deadlineFor(timeoutMs);
    int n = zym_listLength(handlesV);
    ZymValue ready = zym_newList(vm);
    zym_pushRoot(vm, ready);
    while (true) {
        for (int i = 0; i < n; i++) {
            ZymValue h = zym_listGet(vm, handlesV, i);
            if (h == ZYM_ERROR) continue;
            if (isReady(vm, h, wm)) zym_listAppend(vm, ready, h);
        }
        if (zym_listLength(ready) > 0) break;
        int rem = remainingMs(deadline);
        if (timeoutMs >= 0 && rem == 0) break;
        int q = (rem < 0 || rem > 20) ? 20 : rem;
        OS::get_singleton()->delay_usec((uint32_t)q * 1000u);
    }
    ZymValue out = zym_newMap(vm);
    zym_pushRoot(vm, out);
    zym_mapSet(vm, out, "ready",    ready);
    zym_mapSet(vm, out, "timedOut", zym_newBool(zym_listLength(ready) == 0));
    zym_popRoot(vm); // out
    zym_popRoot(vm); // ready
    return out;
}

// ============================================================================
// Instance assembly
// ============================================================================

#define M(obj, ctx, name, sig, fn) do { \
    ZymValue cl = zym_createNativeClosure(vm, sig, (void*)fn, ctx); \
    zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
} while (0)
#define MV(obj, ctx, name, sig, fn) do { \
    ZymValue cl = zym_createNativeClosureVariadic(vm, sig, (void*)fn, ctx); \
    zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
} while (0)

static ZymValue makeTcpInstance(ZymVM* vm, Ref<StreamPeerTCP> s) {
    auto* data = new TcpHandle{ s };
    ZymValue ctx = zym_createNativeContext(vm, data, tcpFinalizer);
    zym_pushRoot(vm, ctx);
    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);
    zym_mapSet(vm, obj, "__tcp__", ctx);

    M (obj, ctx, "status",       "status()",            tcp_status);
    M (obj, ctx, "poll",         "poll()",              tcp_poll);
    M (obj, ctx, "available",    "available()",         tcp_available);
    M (obj, ctx, "readSome",     "readSome(n)",         tcp_readSome);
    MV(obj, ctx, "read",         "read(n, ...)",        tcp_read);
    MV(obj, ctx, "readLine",     "readLine(...)",       tcp_readLine);
    MV(obj, ctx, "readAll",      "readAll(...)",        tcp_readAll);
    M (obj, ctx, "writeSome",    "writeSome(buf)",      tcp_writeSome);
    MV(obj, ctx, "write",        "write(buf, ...)",     tcp_write);
    M (obj, ctx, "setNoDelay",   "setNoDelay(b)",       tcp_setNoDelay);
    M (obj, ctx, "localAddress", "localAddress()",      tcp_localAddress);
    M (obj, ctx, "peerAddress",  "peerAddress()",       tcp_peerAddress);
    M (obj, ctx, "close",        "close()",             tcp_close);

    zym_popRoot(vm); // obj
    zym_popRoot(vm); // ctx
    return obj;
}

static ZymValue makeTcpsInstance(ZymVM* vm, Ref<TCPServer> s) {
    auto* data = new TcpsHandle{ s };
    ZymValue ctx = zym_createNativeContext(vm, data, tcpsFinalizer);
    zym_pushRoot(vm, ctx);
    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);
    zym_mapSet(vm, obj, "__tcps__", ctx);

    MV(obj, ctx, "accept",    "accept(...)",   tcps_accept);
    M (obj, ctx, "localPort", "localPort()",   tcps_localPort);
    M (obj, ctx, "close",     "close()",       tcps_close);

    zym_popRoot(vm); // obj
    zym_popRoot(vm); // ctx
    return obj;
}

static ZymValue makeUdpInstance(ZymVM* vm, Ref<PacketPeerUDP> s) {
    auto* data = new UdpHandle{ s };
    ZymValue ctx = zym_createNativeContext(vm, data, udpFinalizer);
    zym_pushRoot(vm, ctx);
    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);
    zym_mapSet(vm, obj, "__udp__", ctx);

    M (obj, ctx, "send",         "send(buf, host, port)", udp_send);
    MV(obj, ctx, "recv",         "recv(...)",             udp_recv);
    M (obj, ctx, "localPort",    "localPort()",           udp_localPort);
    M (obj, ctx, "setBroadcast", "setBroadcast(b)",       udp_setBroadcast);
    M (obj, ctx, "close",        "close()",               udp_close);

    zym_popRoot(vm); // obj
    zym_popRoot(vm); // ctx
    return obj;
}

static ZymValue makeTlsInstance(ZymVM* vm, Ref<StreamPeerTLS> tls, Ref<StreamPeerTCP> base) {
    auto* data = new TlsHandle{ tls, base };
    ZymValue ctx = zym_createNativeContext(vm, data, tlsFinalizer);
    zym_pushRoot(vm, ctx);
    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);
    zym_mapSet(vm, obj, "__tls__", ctx);

    M (obj, ctx, "status",       "status()",            tls_status);
    M (obj, ctx, "poll",         "poll()",              tls_poll);
    M (obj, ctx, "available",    "available()",         tls_available);
    M (obj, ctx, "readSome",     "readSome(n)",         tls_readSome);
    MV(obj, ctx, "read",         "read(n, ...)",        tls_read);
    MV(obj, ctx, "readLine",     "readLine(...)",       tls_readLine);
    MV(obj, ctx, "readAll",      "readAll(...)",        tls_readAll);
    M (obj, ctx, "writeSome",    "writeSome(buf)",      tls_writeSome);
    MV(obj, ctx, "write",        "write(buf, ...)",     tls_write);
    M (obj, ctx, "setNoDelay",   "setNoDelay(b)",       tls_setNoDelay);
    M (obj, ctx, "localAddress", "localAddress()",      tls_localAddress);
    M (obj, ctx, "peerAddress",  "peerAddress()",       tls_peerAddress);
    M (obj, ctx, "close",        "close()",             tls_close);

    zym_popRoot(vm); // obj
    zym_popRoot(vm); // ctx
    return obj;
}

#undef M
#undef MV

// ============================================================================
// Global factories
// ============================================================================

ZymValue nativeTcp_create(ZymVM* vm) {
    ZymValue ctx = zym_createNativeContext(vm, nullptr, nullptr);
    zym_pushRoot(vm, ctx);
    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);

#define F(name, sig, fn) do { \
    ZymValue cl = zym_createNativeClosure(vm, sig, (void*)fn, ctx); \
    zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
} while (0)
#define FV(name, sig, fn) do { \
    ZymValue cl = zym_createNativeClosureVariadic(vm, sig, (void*)fn, ctx); \
    zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
} while (0)

    FV("connect", "connect(host, port, ...)", f_tcpConnect);
    F ("listen",  "listen(host, port)",       f_tcpListen);

#undef F
#undef FV

    zym_popRoot(vm); // obj
    zym_popRoot(vm); // ctx
    return obj;
}

ZymValue nativeUdp_create(ZymVM* vm) {
    ZymValue ctx = zym_createNativeContext(vm, nullptr, nullptr);
    zym_pushRoot(vm, ctx);
    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);

    ZymValue cl = zym_createNativeClosure(vm, "bind(host, port)", (void*)f_udpBind, ctx);
    zym_pushRoot(vm, cl);
    zym_mapSet(vm, obj, "bind", cl);
    zym_popRoot(vm);

    zym_popRoot(vm); // obj
    zym_popRoot(vm); // ctx
    return obj;
}

ZymValue nativeSockets_create(ZymVM* vm) {
    ZymValue ctx = zym_createNativeContext(vm, nullptr, nullptr);
    zym_pushRoot(vm, ctx);
    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);

    ZymValue cl = zym_createNativeClosure(vm, "waitAny(handles, mode, timeoutMs)", (void*)f_waitAny, ctx);
    zym_pushRoot(vm, cl);
    zym_mapSet(vm, obj, "waitAny", cl);
    zym_popRoot(vm);

    zym_popRoot(vm); // obj
    zym_popRoot(vm); // ctx
    return obj;
}

ZymValue nativeTls_create(ZymVM* vm) {
    ZymValue ctx = zym_createNativeContext(vm, nullptr, nullptr);
    zym_pushRoot(vm, ctx);
    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);

#define F(name, sig, fn) do { \
    ZymValue cl = zym_createNativeClosure(vm, sig, (void*)fn, ctx); \
    zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
} while (0)
#define FV(name, sig, fn) do { \
    ZymValue cl = zym_createNativeClosureVariadic(vm, sig, (void*)fn, ctx); \
    zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
} while (0)

    FV("connect", "connect(host, port, ...)",  f_tlsConnect);
    FV("accept",  "accept(tcp, opts, ...)",    f_tlsAccept);

#undef F
#undef FV

    zym_popRoot(vm); // obj
    zym_popRoot(vm); // ctx
    return obj;
}
