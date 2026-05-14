// WebSocket native — client (WebSocket.connect) and server-side wrap
// (WebSocket.accept) over Godot's `WebSocketPeer`.
//
// Per-instance shape (consistent with sockets.cpp / enet.cpp):
//   "__ws__"  -> WsHandle*  (one WebSocketPeer)
//
// Model: WebSocketPeer is a single bidirectional packet-framed stream
// driven by an explicit `poll()` (no background thread). Each
// `send`/`sendText` corresponds to one WebSocket frame; each
// `recv()` returns one received frame as a Buffer.
//
// Status-string vocabulary mirrors the rest of `docs/cli/sockets.md`:
//   CONNECTING -> "connecting"
//   OPEN       -> "connected"
//   CLOSING    -> "closing"
//   CLOSED     -> "closed"

#include "core/crypto/crypto.h"
#include "core/io/ip_address.h"
#include "core/io/stream_peer.h"
#include "core/os/os.h"
#include "core/string/ustring.h"
#include "core/variant/variant.h"
#include "modules/websocket/websocket_peer.h"

#include "natives.hpp"
#include "../boot/register_core.hpp"

extern bool zymExtractCryptoKey(ZymVM* vm, ZymValue v, Ref<CryptoKey>* out);
extern bool zymExtractX509(ZymVM* vm, ZymValue v, Ref<X509Certificate>* out);
extern ZymValue makeBufferInstance(ZymVM* vm, const PackedByteArray& src);

// Cross-TU accessors implemented in src/natives/sockets.cpp. Each pulls the
// underlying StreamPeer out of a TCP / TLS socket map (by `__tcp__` /
// `__tls__` tag). Returns false (and leaves *out untouched) when the input
// isn't of the expected kind; returns true with an empty Ref if the socket
// has been closed.
extern bool zymExtractTcpStream(ZymVM* vm, ZymValue v, Ref<StreamPeer>* out);
extern bool zymExtractTlsStream(ZymVM* vm, ZymValue v, Ref<StreamPeer>* out);

// ---- handle plumbing ----

struct WsHandle {
    Ref<WebSocketPeer> peer;
    // Server-side accept keeps the underlying TCP/TLS stream alive so it
    // doesn't get GC'd out from under the WebSocketPeer. The peer holds
    // a Ref<StreamPeer> internally; we just need to keep our typed
    // handle to the same object so its lifetime tracks ours.
    Ref<StreamPeer>    base;
};

static void wsFinalizer(ZymVM*, void* d) { delete static_cast<WsHandle*>(d); }

// ---- value helpers ----

static ZymValue strZ(ZymVM* vm, const String& s) {
    CharString u = s.utf8();
    return zym_newStringN(vm, u.get_data(), u.length());
}

static bool reqStr(ZymVM* vm, ZymValue v, const char* where, String* out) {
    if (!zym_isString(v)) { zym_runtimeError(vm, "%s expects a string", where); return false; }
    *out = String::utf8(zym_asCString(v)); return true;
}

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

static WsHandle* unwrapWs(ZymValue ctx) { return static_cast<WsHandle*>(zym_getNativeData(ctx)); }

static const char* wsStatusName(WebSocketPeer::State st) {
    switch (st) {
        case WebSocketPeer::STATE_CONNECTING: return "connecting";
        case WebSocketPeer::STATE_OPEN:       return "connected";
        case WebSocketPeer::STATE_CLOSING:    return "closing";
        case WebSocketPeer::STATE_CLOSED:     return "closed";
    }
    return "error";
}

static int64_t now_ms() { return (int64_t)OS::get_singleton()->get_ticks_msec(); }

// Forward decls.
static ZymValue makeWsInstance(ZymVM* vm, Ref<WebSocketPeer> peer, Ref<StreamPeer> base);
ZymValue nativeWebSocket_create(ZymVM* vm);

// ============================================================================
// Option helpers
// ============================================================================

// Read `opts.protocols` as a list of strings -> PackedStringArray.
static bool readProtocols(ZymVM* vm, ZymValue optsV, Vector<String>* out, const char* who) {
    ZymValue v = zym_mapGet(vm, optsV, "protocols");
    if (v == ZYM_ERROR || zym_isNull(v)) return true;
    if (!zym_isList(v)) {
        zym_runtimeError(vm, "%s: opts.protocols must be a list of strings", who);
        return false;
    }
    int n = zym_listLength(v);
    for (int i = 0; i < n; i++) {
        ZymValue e = zym_listGet(vm, v, i);
        if (!zym_isString(e)) {
            zym_runtimeError(vm, "%s: opts.protocols[%d] must be a string", who, i);
            return false;
        }
        out->push_back(String::utf8(zym_asCString(e)));
    }
    return true;
}

// Read `opts.headers` as a list of strings -> Vector<String>.
static bool readHeaders(ZymVM* vm, ZymValue optsV, Vector<String>* out, const char* who) {
    ZymValue v = zym_mapGet(vm, optsV, "headers");
    if (v == ZYM_ERROR || zym_isNull(v)) return true;
    if (!zym_isList(v)) {
        zym_runtimeError(vm, "%s: opts.headers must be a list of strings", who);
        return false;
    }
    int n = zym_listLength(v);
    for (int i = 0; i < n; i++) {
        ZymValue e = zym_listGet(vm, v, i);
        if (!zym_isString(e)) {
            zym_runtimeError(vm, "%s: opts.headers[%d] must be a string", who, i);
            return false;
        }
        out->push_back(String::utf8(zym_asCString(e)));
    }
    return true;
}

// Build a client-side TLSOptions from a map (same shape as TLS.connect /
// ENet.connect): { verify, trustedRoots, commonName }. `null` -> defaults.
static Ref<TLSOptions> buildClientTls(ZymVM* vm, ZymValue tlsV, const char* who) {
    if (zym_isNull(tlsV)) return TLSOptions::client();
    if (!zym_isMap(tlsV)) {
        zym_runtimeError(vm, "%s: opts.tls must be a map { verify, trustedRoots, commonName }", who);
        return Ref<TLSOptions>();
    }
    bool verify = true;
    {
        ZymValue v = zym_mapGet(vm, tlsV, "verify");
        if (v != ZYM_ERROR && !zym_isNull(v)) {
            if (!zym_isBool(v)) { zym_runtimeError(vm, "%s: opts.tls.verify must be a bool", who); return Ref<TLSOptions>(); }
            verify = zym_asBool(v);
        }
    }
    Ref<X509Certificate> trusted;
    {
        ZymValue v = zym_mapGet(vm, tlsV, "trustedRoots");
        if (v != ZYM_ERROR && !zym_isNull(v)) {
            if (zym_isList(v)) {
                int n = zym_listLength(v);
                String combined;
                for (int i = 0; i < n; i++) {
                    ZymValue e = zym_listGet(vm, v, i);
                    Ref<X509Certificate> one;
                    if (!zymExtractX509(vm, e, &one)) {
                        zym_runtimeError(vm, "%s: opts.tls.trustedRoots[%d] must be an X509Certificate", who, i);
                        return Ref<TLSOptions>();
                    }
                    combined += one->save_to_string();
                }
                trusted = Ref<X509Certificate>(X509Certificate::create());
                if (trusted->load_from_string(combined) != OK) {
                    zym_runtimeError(vm, "%s: failed to combine opts.tls.trustedRoots", who);
                    return Ref<TLSOptions>();
                }
            } else if (!zymExtractX509(vm, v, &trusted)) {
                zym_runtimeError(vm, "%s: opts.tls.trustedRoots must be an X509Certificate or list of them", who);
                return Ref<TLSOptions>();
            }
        }
    }
    String commonName;
    {
        ZymValue v = zym_mapGet(vm, tlsV, "commonName");
        if (v != ZYM_ERROR && !zym_isNull(v)) {
            if (!zym_isString(v)) { zym_runtimeError(vm, "%s: opts.tls.commonName must be a string", who); return Ref<TLSOptions>(); }
            commonName = String::utf8(zym_asCString(v));
        }
    }
    if (!verify) return TLSOptions::client_unsafe(trusted);
    return TLSOptions::client(trusted, commonName);
}

// Apply common WebSocketPeer tuning from an opts map (no-op if any key is
// missing): inboundBufferSize, outboundBufferSize, maxQueuedPackets,
// heartbeatInterval (seconds).
static bool applyCommonOpts(ZymVM* vm, ZymValue optsV, const Ref<WebSocketPeer>& peer, const char* who) {
    if (zym_isNull(optsV)) return true;
    if (!zym_isMap(optsV)) {
        zym_runtimeError(vm, "%s: opts must be a map or null", who);
        return false;
    }
    {
        ZymValue v = zym_mapGet(vm, optsV, "inboundBufferSize");
        if (v != ZYM_ERROR && !zym_isNull(v)) {
            if (!zym_isNumber(v)) { zym_runtimeError(vm, "%s: opts.inboundBufferSize must be a number", who); return false; }
            peer->set_inbound_buffer_size((int)zym_asNumber(v));
        }
    }
    {
        ZymValue v = zym_mapGet(vm, optsV, "outboundBufferSize");
        if (v != ZYM_ERROR && !zym_isNull(v)) {
            if (!zym_isNumber(v)) { zym_runtimeError(vm, "%s: opts.outboundBufferSize must be a number", who); return false; }
            peer->set_outbound_buffer_size((int)zym_asNumber(v));
        }
    }
    {
        ZymValue v = zym_mapGet(vm, optsV, "maxQueuedPackets");
        if (v != ZYM_ERROR && !zym_isNull(v)) {
            if (!zym_isNumber(v)) { zym_runtimeError(vm, "%s: opts.maxQueuedPackets must be a number", who); return false; }
            peer->set_max_queued_packets((int)zym_asNumber(v));
        }
    }
    {
        ZymValue v = zym_mapGet(vm, optsV, "heartbeatInterval");
        if (v != ZYM_ERROR && !zym_isNull(v)) {
            if (!zym_isNumber(v)) { zym_runtimeError(vm, "%s: opts.heartbeatInterval must be a number (seconds)", who); return false; }
            peer->set_heartbeat_interval(zym_asNumber(v));
        }
    }
    Vector<String> protos;
    if (!readProtocols(vm, optsV, &protos, who)) return false;
    if (!protos.is_empty()) peer->set_supported_protocols(protos);
    Vector<String> hdrs;
    if (!readHeaders(vm, optsV, &hdrs, who)) return false;
    if (!hdrs.is_empty()) peer->set_handshake_headers(hdrs);
    return true;
}

// ============================================================================
// Peer instance methods (`__ws__`)
// ============================================================================

static ZymValue ws_status(ZymVM* vm, ZymValue ctx) {
    WsHandle* h = unwrapWs(ctx);
    if (!h || h->peer.is_null()) return strZ(vm, "closed");
    return strZ(vm, wsStatusName(h->peer->get_ready_state()));
}

static ZymValue ws_poll(ZymVM* vm, ZymValue ctx) {
    WsHandle* h = unwrapWs(ctx);
    if (!h || h->peer.is_null()) return strZ(vm, "closed");
    h->peer->poll();
    return strZ(vm, wsStatusName(h->peer->get_ready_state()));
}

static ZymValue ws_available(ZymVM* vm, ZymValue ctx) {
    WsHandle* h = unwrapWs(ctx);
    if (!h || h->peer.is_null()) return zym_newNumber(0);
    h->peer->poll();
    return zym_newNumber((double)h->peer->get_available_packet_count());
}

static ZymValue ws_send(ZymVM* vm, ZymValue ctx, ZymValue bufV) {
    WsHandle* h = unwrapWs(ctx);
    if (!h || h->peer.is_null()) return strZ(vm, "closed");
    if (h->peer->get_ready_state() != WebSocketPeer::STATE_OPEN) return strZ(vm, "closed");
    PackedByteArray* buf = nullptr;
    if (!reqBuf(vm, bufV, "send(buf)", &buf)) return ZYM_ERROR;
    Error e = h->peer->send(buf->ptr(), buf->size(), WebSocketPeer::WRITE_MODE_BINARY);
    if (e != OK) return strZ(vm, "error");
    return strZ(vm, "ok");
}

static ZymValue ws_sendText(ZymVM* vm, ZymValue ctx, ZymValue textV) {
    WsHandle* h = unwrapWs(ctx);
    if (!h || h->peer.is_null()) return strZ(vm, "closed");
    if (h->peer->get_ready_state() != WebSocketPeer::STATE_OPEN) return strZ(vm, "closed");
    String s;
    if (!reqStr(vm, textV, "sendText(text)", &s)) return ZYM_ERROR;
    Error e = h->peer->send_text(s);
    if (e != OK) return strZ(vm, "error");
    return strZ(vm, "ok");
}

// recv([timeoutMs]) — pull one decoded frame.
//   timeoutMs == -1 (default): block forever until a frame arrives or the
//                              peer terminates.
//   timeoutMs ==  0          : peek; if no frame is queued return "busy".
//   timeoutMs  >  0          : wait up to that many milliseconds.
// Returns Buffer on success, or one of "busy"/"timeout"/"eof"/"closed"/"error".
static ZymValue ws_recv(ZymVM* vm, ZymValue ctx, ZymValue* vargs, int vargc) {
    WsHandle* h = unwrapWs(ctx);
    if (!h || h->peer.is_null()) return strZ(vm, "closed");

    int64_t timeoutMs = -1;
    if (vargc >= 1) {
        if (!zym_isNumber(vargs[0])) { zym_runtimeError(vm, "recv([timeoutMs]): timeoutMs must be a number"); return ZYM_ERROR; }
        timeoutMs = (int64_t)zym_asNumber(vargs[0]);
    }
    int64_t deadline = (timeoutMs < 0) ? -1 : (timeoutMs == 0 ? now_ms() : now_ms() + timeoutMs);

    while (true) {
        h->peer->poll();
        WebSocketPeer::State st = h->peer->get_ready_state();
        if (h->peer->get_available_packet_count() > 0) {
            const uint8_t* data = nullptr;
            int len = 0;
            Error e = h->peer->get_packet(&data, len);
            if (e != OK) return strZ(vm, "error");
            PackedByteArray pba;
            pba.resize(len);
            if (len > 0) memcpy(pba.ptrw(), data, len);
            return makeBufferInstance(vm, pba);
        }
        if (st == WebSocketPeer::STATE_CLOSED) return strZ(vm, "eof");
        if (timeoutMs == 0) return strZ(vm, "busy");
        if (timeoutMs > 0) {
            int64_t r = deadline - now_ms();
            if (r <= 0) return strZ(vm, "timeout");
        }
        OS::get_singleton()->delay_usec(20u * 1000u);
    }
}

static ZymValue ws_wasStringPacket(ZymVM* vm, ZymValue ctx) {
    WsHandle* h = unwrapWs(ctx);
    if (!h || h->peer.is_null()) return zym_newBool(false);
    return zym_newBool(h->peer->was_string_packet());
}

static ZymValue ws_selectedProtocol(ZymVM* vm, ZymValue ctx) {
    WsHandle* h = unwrapWs(ctx);
    if (!h || h->peer.is_null()) return strZ(vm, "");
    return strZ(vm, h->peer->get_selected_protocol());
}

static ZymValue ws_requestedUrl(ZymVM* vm, ZymValue ctx) {
    WsHandle* h = unwrapWs(ctx);
    if (!h || h->peer.is_null()) return strZ(vm, "");
    return strZ(vm, h->peer->get_requested_url());
}

static ZymValue ws_closeCode(ZymVM* vm, ZymValue ctx) {
    WsHandle* h = unwrapWs(ctx);
    if (!h || h->peer.is_null()) return zym_newNumber(-1);
    return zym_newNumber((double)h->peer->get_close_code());
}

static ZymValue ws_closeReason(ZymVM* vm, ZymValue ctx) {
    WsHandle* h = unwrapWs(ctx);
    if (!h || h->peer.is_null()) return strZ(vm, "");
    return strZ(vm, h->peer->get_close_reason());
}

static ZymValue ws_peerAddress(ZymVM* vm, ZymValue ctx) {
    WsHandle* h = unwrapWs(ctx);
    if (!h || h->peer.is_null()) return zym_newNull();
    IPAddress ip = h->peer->get_connected_host();
    int port = (int)h->peer->get_connected_port();
    ZymValue m = zym_newMap(vm);
    zym_pushRoot(vm, m);
    zym_mapSet(vm, m, "host", strZ(vm, String(ip)));
    zym_mapSet(vm, m, "port", zym_newNumber((double)port));
    zym_popRoot(vm);
    return m;
}

static ZymValue ws_setNoDelay(ZymVM* vm, ZymValue ctx, ZymValue v) {
    WsHandle* h = unwrapWs(ctx);
    if (!h || h->peer.is_null()) return zym_newNull();
    if (!zym_isBool(v)) { zym_runtimeError(vm, "setNoDelay(b): b must be a bool"); return ZYM_ERROR; }
    h->peer->set_no_delay(zym_asBool(v));
    return zym_newNull();
}

// close([code, reason])
static ZymValue ws_close(ZymVM* vm, ZymValue ctx, ZymValue* vargs, int vargc) {
    WsHandle* h = unwrapWs(ctx);
    if (!h || h->peer.is_null()) return zym_newNull();
    int code = 1000;
    String reason;
    if (vargc >= 1 && !zym_isNull(vargs[0])) {
        if (!zym_isNumber(vargs[0])) { zym_runtimeError(vm, "close([code, reason]): code must be a number"); return ZYM_ERROR; }
        code = (int)zym_asNumber(vargs[0]);
    }
    if (vargc >= 2 && !zym_isNull(vargs[1])) {
        if (!zym_isString(vargs[1])) { zym_runtimeError(vm, "close([code, reason]): reason must be a string"); return ZYM_ERROR; }
        reason = String::utf8(zym_asCString(vargs[1]));
    }
    h->peer->close(code, reason);
    return zym_newNull();
}

// ============================================================================
// Factories: WebSocket.connect / WebSocket.accept
// ============================================================================

// WebSocket.connect(url [, opts])
//   url: "ws://host:port/path" or "wss://host:port/path".
//   opts (all optional):
//     { tls: { verify, trustedRoots, commonName },
//       protocols: [string, ...],
//       headers:   [string, ...],
//       inboundBufferSize: number,
//       outboundBufferSize: number,
//       maxQueuedPackets:  number,
//       heartbeatInterval: number  // seconds
//     }
//   Returns the WebSocket handle in `"connecting"` state. The caller drives
//   `sock.poll()` until `status() == "connected"` (or terminal). On outright
//   failure (bad URL, build without TLS, etc.) returns `null`.
static ZymValue f_wsConnect(ZymVM* vm, ZymValue ctx, ZymValue urlV, ZymValue* vargs, int vargc) {
    (void)ctx;
    // wss:// goes through the same TLS client path as TLS.connect and needs
    // `default_certs` populated. See src/boot/register_core.cpp.
    zym::boot::ensure_default_certificates_loaded();
    String url;
    if (!reqStr(vm, urlV, "connect(url, ...)", &url)) return ZYM_ERROR;

    ZymValue optsV = zym_newNull();
    if (vargc >= 1) optsV = vargs[0];
    ZymValue tlsV = zym_newNull();
    if (!zym_isNull(optsV)) {
        if (!zym_isMap(optsV)) { zym_runtimeError(vm, "WebSocket.connect: opts must be a map or null"); return ZYM_ERROR; }
        ZymValue v = zym_mapGet(vm, optsV, "tls");
        if (v != ZYM_ERROR && !zym_isNull(v)) tlsV = v;
    }

    Ref<WebSocketPeer> peer = Ref<WebSocketPeer>(WebSocketPeer::create());
    if (peer.is_null()) {
        zym_runtimeError(vm, "WebSocket.connect: WebSocketPeer unavailable in this build");
        return ZYM_ERROR;
    }
    if (!applyCommonOpts(vm, optsV, peer, "WebSocket.connect")) return ZYM_ERROR;

    Ref<TLSOptions> tlsOpts;
    if (url.begins_with("wss://") || !zym_isNull(tlsV)) {
        tlsOpts = buildClientTls(vm, tlsV, "WebSocket.connect");
        if (tlsOpts.is_null() && !zym_isNull(tlsV)) return ZYM_ERROR;
    }
    Error e = peer->connect_to_url(url, tlsOpts);
    if (e != OK) return zym_newNull();

    return makeWsInstance(vm, peer, Ref<StreamPeer>());
}

// WebSocket.accept(tcp [, opts])
//   tcp: a TCP socket (`__tcp__` tag) or a TLS socket (`__tls__` tag)
//        returned by `srv.accept(...)` / `TLS.accept(...)`. For wss://
//        servers, wrap the accepted TCP with `TLS.accept` first and then
//        hand the TLS socket here.
//   opts (all optional): same as `WebSocket.connect` minus `tls`
//        (the TLS layer has already been applied to the underlying stream).
//   Returns the WebSocket handle in `"connecting"` state; drive the
//   handshake with `sock.poll()` as on the client side.
static ZymValue f_wsAccept(ZymVM* vm, ZymValue ctx, ZymValue baseV, ZymValue* vargs, int vargc) {
    (void)ctx;
    if (!zym_isMap(baseV)) {
        zym_runtimeError(vm, "WebSocket.accept(tcp, ...): tcp must be a TCP or TLS socket");
        return ZYM_ERROR;
    }
    // Pull out the underlying Ref<StreamPeer> from either a __tcp__ or
    // __tls__ tagged socket. The accessors live in `sockets.cpp` so we
    // don't depend on the file-local handle struct layouts.
    Ref<StreamPeer> stream;
    if (zymExtractTcpStream(vm, baseV, &stream)) {
        if (stream.is_null()) {
            zym_runtimeError(vm, "WebSocket.accept: tcp socket is closed");
            return ZYM_ERROR;
        }
    } else if (zymExtractTlsStream(vm, baseV, &stream)) {
        if (stream.is_null()) {
            zym_runtimeError(vm, "WebSocket.accept: tls socket is closed");
            return ZYM_ERROR;
        }
    } else {
        zym_runtimeError(vm, "WebSocket.accept(tcp, ...): tcp must be a TCP or TLS socket");
        return ZYM_ERROR;
    }

    ZymValue optsV = zym_newNull();
    if (vargc >= 1) optsV = vargs[0];

    Ref<WebSocketPeer> peer = Ref<WebSocketPeer>(WebSocketPeer::create());
    if (peer.is_null()) {
        zym_runtimeError(vm, "WebSocket.accept: WebSocketPeer unavailable in this build");
        return ZYM_ERROR;
    }
    if (!applyCommonOpts(vm, optsV, peer, "WebSocket.accept")) return ZYM_ERROR;

    Error e = peer->accept_stream(stream);
    if (e != OK) return zym_newNull();

    return makeWsInstance(vm, peer, stream);
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

static ZymValue makeWsInstance(ZymVM* vm, Ref<WebSocketPeer> peer, Ref<StreamPeer> base) {
    auto* data = new WsHandle{ peer, base };
    ZymValue ctx = zym_createNativeContext(vm, data, wsFinalizer);
    zym_pushRoot(vm, ctx);
    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);
    zym_mapSet(vm, obj, "__ws__", ctx);
    M (obj, ctx, "status",           "status()",              ws_status);
    M (obj, ctx, "poll",             "poll()",                ws_poll);
    M (obj, ctx, "available",        "available()",           ws_available);
    M (obj, ctx, "send",             "send(buf)",             ws_send);
    M (obj, ctx, "sendText",         "sendText(text)",        ws_sendText);
    MV(obj, ctx, "recv",             "recv(...)",             ws_recv);
    M (obj, ctx, "wasStringPacket",  "wasStringPacket()",     ws_wasStringPacket);
    M (obj, ctx, "selectedProtocol", "selectedProtocol()",    ws_selectedProtocol);
    M (obj, ctx, "requestedUrl",     "requestedUrl()",        ws_requestedUrl);
    M (obj, ctx, "closeCode",        "closeCode()",           ws_closeCode);
    M (obj, ctx, "closeReason",      "closeReason()",         ws_closeReason);
    M (obj, ctx, "peerAddress",      "peerAddress()",         ws_peerAddress);
    M (obj, ctx, "setNoDelay",       "setNoDelay(b)",         ws_setNoDelay);
    MV(obj, ctx, "close",            "close(...)",            ws_close);
    zym_popRoot(vm); // obj
    zym_popRoot(vm); // ctx
    return obj;
}

#undef M
#undef MV

// ============================================================================
// Readiness query for `Sockets.waitAny`
// ============================================================================
//
// Called from src/natives/sockets.cpp. Returns true if `handle` is a
// WebSocket map (carries `__ws__`); writes the readiness verdict to
// `*out`. The mode encoding matches the order in `WaitMode`: 0=read,
// 1=write, 2=any.
//
// Readability is true when a frame is queued OR the peer has terminated
// (so EOF can be observed in the same select loop). Writability is true
// once the peer is OPEN, and also when terminated so the caller's send
// returns its own "closed" status without spinning forever in waitAny.
extern "C++" bool zymWsReady(ZymVM* vm, ZymValue handle, int mode, bool* out) {
    if (!zym_isMap(handle)) return false;
    ZymValue ctx = zym_mapGet(vm, handle, "__ws__");
    if (ctx == ZYM_ERROR) return false;
    WsHandle* h = unwrapWs(ctx);
    if (!h || h->peer.is_null()) { *out = (mode != 1); return true; }
    h->peer->poll();
    WebSocketPeer::State st = h->peer->get_ready_state();
    bool dead = (st == WebSocketPeer::STATE_CLOSED || st == WebSocketPeer::STATE_CLOSING);
    bool readable = dead || h->peer->get_available_packet_count() > 0;
    bool writable = dead || (st == WebSocketPeer::STATE_OPEN);
    switch (mode) {
        case 0:  *out = readable; break;
        case 1:  *out = writable; break;
        default: *out = readable || writable; break;
    }
    return true;
}

// ============================================================================
// Global factory
// ============================================================================

ZymValue nativeWebSocket_create(ZymVM* vm) {
    ZymValue ctx = zym_createNativeContext(vm, nullptr, nullptr);
    zym_pushRoot(vm, ctx);
    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);

#define FV(name, sig, fn) do { \
    ZymValue cl = zym_createNativeClosureVariadic(vm, sig, (void*)fn, ctx); \
    zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
} while (0)

    FV("connect", "connect(url, ...)",   f_wsConnect);
    FV("accept",  "accept(tcp, ...)",    f_wsAccept);

#undef FV

    zym_popRoot(vm); // obj
    zym_popRoot(vm); // ctx
    return obj;
}
