// ENet native — host (client/server) and per-peer handles over `module_enet`.
//
// Per-instance shape (consistent with sockets.cpp):
//   "__enet__"   -> EnetHandle*       (one ENetConnection host)
//   "__enetp__"  -> EnetPeerHandle*   (one ENetPacketPeer)
//
// Model: ENet is a single-host model. `ENet.connect` creates a host with
// one outbound peer; `ENet.listen` creates a host bound to a port that
// accepts up to maxPeers inbound peers. Both are pumped via
// `host.service(timeoutMs)` which returns one event at a time.
//
// Status-string vocabulary mirrors the rest of `docs/sockets.md`.

#include "core/crypto/crypto.h"
#include "core/io/ip.h"
#include "core/io/ip_address.h"
#include "core/os/os.h"
#include "core/string/ustring.h"
#include "core/variant/variant.h"
#include "modules/enet/enet_connection.h"
#include "modules/enet/enet_packet_peer.h"
#include <enet/enet.h>

#include "natives.hpp"

extern bool zymExtractCryptoKey(ZymVM* vm, ZymValue v, Ref<CryptoKey>* out);
extern bool zymExtractX509(ZymVM* vm, ZymValue v, Ref<X509Certificate>* out);

extern ZymValue makeBufferInstance(ZymVM* vm, const PackedByteArray& src);

// ---- handle plumbing ----

struct EnetHandle {
    Ref<ENetConnection> host;
    int                 maxChannels = 0;
    bool                bound = false;   // true if listen()
};
struct EnetPeerHandle {
    Ref<ENetPacketPeer> peer;
};

static void enetFinalizer (ZymVM*, void* d) { delete static_cast<EnetHandle*    >(d); }
static void enetpFinalizer(ZymVM*, void* d) { delete static_cast<EnetPeerHandle*>(d); }

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

static EnetHandle*     unwrapEnet (ZymValue ctx) { return static_cast<EnetHandle*    >(zym_getNativeData(ctx)); }
static EnetPeerHandle* unwrapEnetp(ZymValue ctx) { return static_cast<EnetPeerHandle*>(zym_getNativeData(ctx)); }

static ZymValue addrMap(ZymVM* vm, const IPAddress& ip, int port) {
    ZymValue m = zym_newMap(vm);
    zym_pushRoot(vm, m);
    zym_mapSet(vm, m, "host", strZ(vm, String(ip)));
    zym_mapSet(vm, m, "port", zym_newNumber((double)port));
    zym_popRoot(vm);
    return m;
}

// peer state -> shared status-string vocabulary.
static const char* peerStatusName(ENetPacketPeer::PeerState st) {
    switch (st) {
        case ENetPacketPeer::STATE_DISCONNECTED:
        case ENetPacketPeer::STATE_DISCONNECTING:
        case ENetPacketPeer::STATE_DISCONNECT_LATER:
        case ENetPacketPeer::STATE_ACKNOWLEDGING_DISCONNECT:
        case ENetPacketPeer::STATE_ZOMBIE:
            return "closed";
        case ENetPacketPeer::STATE_CONNECTING:
        case ENetPacketPeer::STATE_ACKNOWLEDGING_CONNECT:
        case ENetPacketPeer::STATE_CONNECTION_PENDING:
        case ENetPacketPeer::STATE_CONNECTION_SUCCEEDED:
            return "connecting";
        case ENetPacketPeer::STATE_CONNECTED:
            return "connected";
    }
    return "error";
}

// Forward decls.
static ZymValue makeEnetInstance(ZymVM* vm, Ref<ENetConnection> host, int maxChannels, bool bound);
static ZymValue makeEnetPeerInstance(ZymVM* vm, Ref<ENetPacketPeer> peer);
ZymValue nativeEnet_create(ZymVM* vm);

// ---- mode (reliable / unreliable / unsequenced) -> enet packet flag ----

static bool parseMode(ZymVM* vm, ZymValue v, const char* where, int* outFlags) {
    if (!zym_isString(v)) {
        zym_runtimeError(vm, "%s expects mode string \"reliable\", \"unreliable\" or \"unsequenced\"", where);
        return false;
    }
    String s = String::utf8(zym_asCString(v)).to_lower();
    if (s == "reliable")    { *outFlags = ENET_PACKET_FLAG_RELIABLE;    return true; }
    if (s == "unreliable")  { *outFlags = 0;                            return true; }  // unreliable-sequenced
    if (s == "unsequenced") { *outFlags = ENET_PACKET_FLAG_UNSEQUENCED; return true; }
    zym_runtimeError(vm, "%s: unknown mode \"%s\" (use \"reliable\", \"unreliable\", or \"unsequenced\")", where, zym_asCString(v));
    return false;
}

// ============================================================================
// ENet host instance methods (`__enet__`)
// ============================================================================

static ZymValue enet_service(ZymVM* vm, ZymValue ctx, ZymValue tmo) {
    auto* h = unwrapEnet(ctx);
    if (!h || h->host.is_null()) return zym_newNull();

    if (!zym_isNumber(tmo)) { zym_runtimeError(vm, "service(timeoutMs) expects a number"); return ZYM_ERROR; }
    int tms = (int)zym_asNumber(tmo);
    if (tms < 0) tms = 0; // clamp; ENet itself does not understand "block forever"

    ENetConnection::Event ev;
    ENetConnection::EventType et = h->host->service(tms, ev);
    if (et == ENetConnection::EVENT_NONE)  return zym_newNull();
    if (et == ENetConnection::EVENT_ERROR) {
        ZymValue m = zym_newMap(vm);
        zym_pushRoot(vm, m);
        zym_mapSet(vm, m, "type", strZ(vm, "error"));
        zym_popRoot(vm);
        return m;
    }

    ZymValue m = zym_newMap(vm);
    zym_pushRoot(vm, m);
    if (et == ENetConnection::EVENT_CONNECT) {
        zym_mapSet(vm, m, "type", strZ(vm, "connect"));
        zym_mapSet(vm, m, "peer", makeEnetPeerInstance(vm, ev.peer));
        zym_mapSet(vm, m, "data", zym_newNumber((double)ev.data));
        zym_mapSet(vm, m, "channel", zym_newNumber(0));
    } else if (et == ENetConnection::EVENT_DISCONNECT) {
        zym_mapSet(vm, m, "type", strZ(vm, "disconnect"));
        zym_mapSet(vm, m, "peer", makeEnetPeerInstance(vm, ev.peer));
        zym_mapSet(vm, m, "data", zym_newNumber((double)ev.data));
        zym_mapSet(vm, m, "channel", zym_newNumber(0));
    } else if (et == ENetConnection::EVENT_RECEIVE) {
        zym_mapSet(vm, m, "type", strZ(vm, "receive"));
        zym_mapSet(vm, m, "peer", makeEnetPeerInstance(vm, ev.peer));
        zym_mapSet(vm, m, "channel", zym_newNumber((double)ev.channel_id));
        // copy packet bytes into a Buffer, then destroy the ENet packet.
        PackedByteArray pba;
        if (ev.packet) {
            pba.resize((int)ev.packet->dataLength);
            if (ev.packet->dataLength > 0) {
                memcpy(pba.ptrw(), ev.packet->data, ev.packet->dataLength);
            }
            enet_packet_destroy(ev.packet);
        }
        zym_mapSet(vm, m, "data", makeBufferInstance(vm, pba));
    }
    zym_popRoot(vm);
    return m;
}

static ZymValue enet_flush(ZymVM* vm, ZymValue ctx) {
    (void)vm;
    auto* h = unwrapEnet(ctx);
    if (h && h->host.is_valid()) h->host->flush();
    return zym_newNull();
}

static ZymValue enet_localPort(ZymVM* vm, ZymValue ctx) {
    (void)vm;
    auto* h = unwrapEnet(ctx);
    if (!h || h->host.is_null()) return zym_newNumber(-1);
    return zym_newNumber((double)h->host->get_local_port());
}

static ZymValue enet_broadcast(ZymVM* vm, ZymValue ctx, ZymValue bufV, ZymValue chV, ZymValue modeV) {
    auto* h = unwrapEnet(ctx);
    if (!h || h->host.is_null()) return strZ(vm, "closed");

    PackedByteArray* buf = nullptr;
    if (!reqBuf(vm, bufV, "broadcast(buf, channel, mode)", &buf)) return ZYM_ERROR;
    double chD = 0; if (!reqNum(vm, chV, "broadcast(buf, channel, mode)", &chD)) return ZYM_ERROR;
    int channel = (int)chD;
    int flags = 0; if (!parseMode(vm, modeV, "broadcast(buf, channel, mode)", &flags)) return ZYM_ERROR;

    ENetPacket* pkt = enet_packet_create(buf->ptr(), (size_t)buf->size(), (uint32_t)flags);
    if (!pkt) return strZ(vm, "error");
    h->host->broadcast((enet_uint8)channel, pkt);
    return strZ(vm, "ok");
}

static ZymValue enet_close(ZymVM* vm, ZymValue ctx) {
    (void)vm;
    auto* h = unwrapEnet(ctx);
    if (h && h->host.is_valid()) {
        h->host->destroy();
    }
    return zym_newNull();
}

// ============================================================================
// ENet peer instance methods (`__enetp__`)
// ============================================================================

static ZymValue enetp_status(ZymVM* vm, ZymValue ctx) {
    auto* p = unwrapEnetp(ctx);
    if (!p || p->peer.is_null()) return strZ(vm, "closed");
    return strZ(vm, peerStatusName(p->peer->get_state()));
}

static ZymValue enetp_send(ZymVM* vm, ZymValue ctx, ZymValue bufV, ZymValue chV, ZymValue modeV) {
    auto* p = unwrapEnetp(ctx);
    if (!p || p->peer.is_null() || p->peer->get_state() != ENetPacketPeer::STATE_CONNECTED) {
        return strZ(vm, "closed");
    }

    PackedByteArray* buf = nullptr;
    if (!reqBuf(vm, bufV, "send(buf, channel, mode)", &buf)) return ZYM_ERROR;
    double chD = 0; if (!reqNum(vm, chV, "send(buf, channel, mode)", &chD)) return ZYM_ERROR;
    int channel = (int)chD;
    if (channel < 0 || channel >= p->peer->get_channels()) {
        zym_runtimeError(vm, "send: channel %d out of range [0, %d)", channel, p->peer->get_channels());
        return ZYM_ERROR;
    }
    int flags = 0; if (!parseMode(vm, modeV, "send(buf, channel, mode)", &flags)) return ZYM_ERROR;

    ENetPacket* pkt = enet_packet_create(buf->ptr(), (size_t)buf->size(), (uint32_t)flags);
    if (!pkt) return strZ(vm, "error");
    int rc = p->peer->send((enet_uint8)channel, pkt);
    if (rc < 0) return strZ(vm, "error");
    return strZ(vm, "ok");
}

static ZymValue enetp_peerAddress(ZymVM* vm, ZymValue ctx) {
    auto* p = unwrapEnetp(ctx);
    if (!p || p->peer.is_null()) return zym_newNull();
    return addrMap(vm, p->peer->get_remote_address(), p->peer->get_remote_port());
}

static ZymValue enetp_ping(ZymVM* vm, ZymValue ctx) {
    (void)vm;
    auto* p = unwrapEnetp(ctx);
    if (p && p->peer.is_valid()) p->peer->ping();
    return zym_newNull();
}

static ZymValue enetp_pingMs(ZymVM* vm, ZymValue ctx) {
    (void)vm;
    auto* p = unwrapEnetp(ctx);
    if (!p || p->peer.is_null()) return zym_newNumber(-1);
    return zym_newNumber(p->peer->get_statistic(ENetPacketPeer::PEER_ROUND_TRIP_TIME));
}

static ZymValue enetp_disconnect(ZymVM* vm, ZymValue ctx, ZymValue* vargs, int vargc) {
    (void)vm;
    auto* p = unwrapEnetp(ctx);
    if (!p || p->peer.is_null()) return zym_newNull();
    int data = 0;
    if (vargc >= 1 && zym_isNumber(vargs[0])) data = (int)zym_asNumber(vargs[0]);
    p->peer->peer_disconnect(data);
    return zym_newNull();
}

static ZymValue enetp_disconnectNow(ZymVM* vm, ZymValue ctx, ZymValue* vargs, int vargc) {
    (void)vm;
    auto* p = unwrapEnetp(ctx);
    if (!p || p->peer.is_null()) return zym_newNull();
    int data = 0;
    if (vargc >= 1 && zym_isNumber(vargs[0])) data = (int)zym_asNumber(vargs[0]);
    p->peer->peer_disconnect_now(data);
    return zym_newNull();
}

static ZymValue enetp_close(ZymVM* vm, ZymValue ctx) {
    (void)vm;
    auto* p = unwrapEnetp(ctx);
    if (p && p->peer.is_valid()) p->peer->reset();
    return zym_newNull();
}

// ============================================================================
// Factory functions
// ============================================================================

static IPAddress parseBindHost(const String& host) {
    if (host.is_empty() || host == "*") return IPAddress("*");
    return IPAddress(host);
}

// Build a server-side TLSOptions from a map { key, cert }. Returns null Ref
// if the map shape is wrong (and raises a runtime error).
static Ref<TLSOptions> buildEnetServerOpts(ZymVM* vm, ZymValue tlsV, const char* who) {
    if (!zym_isMap(tlsV)) {
        zym_runtimeError(vm, "%s: opts.tls (server side) must be a map { key, cert }", who);
        return Ref<TLSOptions>();
    }
    Ref<CryptoKey> key;
    Ref<X509Certificate> cert;
    {
        ZymValue v = zym_mapGet(vm, tlsV, "key");
        if (v == ZYM_ERROR || !zymExtractCryptoKey(vm, v, &key)) {
            zym_runtimeError(vm, "%s: opts.tls.key must be a CryptoKey", who);
            return Ref<TLSOptions>();
        }
    }
    {
        ZymValue v = zym_mapGet(vm, tlsV, "cert");
        if (v == ZYM_ERROR || !zymExtractX509(vm, v, &cert)) {
            zym_runtimeError(vm, "%s: opts.tls.cert must be an X509Certificate", who);
            return Ref<TLSOptions>();
        }
    }
    Ref<TLSOptions> o = TLSOptions::server(key, cert);
    if (o.is_null()) {
        zym_runtimeError(vm, "%s: failed to build server TLSOptions", who);
    }
    return o;
}

// Build a client-side TLSOptions from a map
//   { verify: bool = true, trustedRoots: cert | [cert...] = null, commonName: string = "" }
// hostnameOut receives the SNI / verify hostname (defaults to `host` of the
// outer connect call if `commonName` is absent or empty).
static Ref<TLSOptions> buildEnetClientOpts(ZymVM* vm, ZymValue tlsV, const String& fallbackHost,
                                            String* hostnameOut, const char* who) {
    *hostnameOut = fallbackHost;
    if (zym_isNull(tlsV)) return TLSOptions::client();
    if (!zym_isMap(tlsV)) {
        zym_runtimeError(vm, "%s: opts.tls (client side) must be a map { verify, trustedRoots, commonName }", who);
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
    {
        ZymValue v = zym_mapGet(vm, tlsV, "commonName");
        if (v != ZYM_ERROR && !zym_isNull(v)) {
            if (!zym_isString(v)) { zym_runtimeError(vm, "%s: opts.tls.commonName must be a string", who); return Ref<TLSOptions>(); }
            String cn = String::utf8(zym_asCString(v));
            if (!cn.is_empty()) *hostnameOut = cn;
        }
    }
    if (!verify) return TLSOptions::client_unsafe(trusted);
    return TLSOptions::client(trusted, *hostnameOut);
}

// Look up `opts.tls` from a top-level opts map, if present. Returns
// ZYM_ERROR/ZymValue marker for "no opts" via zym_isNull check. We treat:
//   - opts == null         -> no TLS
//   - opts is a map        -> read opts.tls (which may be null/missing)
//   - anything else        -> runtime error
static bool readTopOpts(ZymVM* vm, ZymValue optsV, ZymValue* tlsOut, const char* who) {
    *tlsOut = zym_newNull();
    if (zym_isNull(optsV)) return true;
    if (!zym_isMap(optsV)) {
        zym_runtimeError(vm, "%s: opts must be a map (e.g. { tls: { ... } }) or null", who);
        return false;
    }
    ZymValue v = zym_mapGet(vm, optsV, "tls");
    if (v != ZYM_ERROR && !zym_isNull(v)) *tlsOut = v;
    return true;
}

// ENet.connect(host, port, [channels=8, opts])
//   Creates a host with one outbound peer. Non-blocking: returns
//   `{ host, peer }` immediately with the peer in "connecting" state.
//   The caller MUST drive `host.service(timeoutMs)` until either a
//   `connect` event arrives (peer reaches "connected") or `peer.status()`
//   reports `"closed"`/`"error"`. Returns `null` on outright failure
//   (host create failed, or `connect_to_host` rejected the address).
//
//   If `opts.tls` is provided, the host's UDP socket is wrapped with a
//   DTLS client (mbedTLS) before `connect_to_host` is called. opts.tls
//   accepts the same shape as `TLS.connect`'s opts:
//     { verify: bool = true, trustedRoots: cert | [cert...], commonName: string }
static ZymValue f_enetConnect(ZymVM* vm, ZymValue ctx, ZymValue hostV, ZymValue portV, ZymValue* vargs, int vargc) {
    (void)ctx;
    String hostS;
    if (!reqStr(vm, hostV, "connect(host, port, ...)", &hostS)) return ZYM_ERROR;
    double portD = 0;
    if (!reqNum(vm, portV, "connect(host, port, ...)", &portD)) return ZYM_ERROR;
    int port = (int)portD;

    int channels = 8;
    if (vargc >= 1) {
        if (!zym_isNumber(vargs[0])) { zym_runtimeError(vm, "connect: channels must be a number"); return ZYM_ERROR; }
        channels = (int)zym_asNumber(vargs[0]);
        if (channels < 1 || channels > 255) {
            zym_runtimeError(vm, "connect: channels must be in [1, 255]"); return ZYM_ERROR;
        }
    }

    ZymValue tlsV = zym_newNull();
    if (vargc >= 2) {
        if (!readTopOpts(vm, vargs[1], &tlsV, "ENet.connect")) return ZYM_ERROR;
    }

    Ref<ENetConnection> hostRef;
    hostRef.instantiate();
    Error e = hostRef->create_host(1, channels, 0, 0);
    if (e != OK) return zym_newNull();

    if (!zym_isNull(tlsV)) {
        String hostname;
        Ref<TLSOptions> opts = buildEnetClientOpts(vm, tlsV, hostS, &hostname, "ENet.connect");
        if (opts.is_null()) { hostRef->destroy(); return ZYM_ERROR; }
        Error de = hostRef->dtls_client_setup(hostname, opts);
        if (de == ERR_UNAVAILABLE) {
            zym_runtimeError(vm, "ENet.connect: DTLS unavailable in this build (GODOT_ENET / mbedtls)");
            hostRef->destroy();
            return ZYM_ERROR;
        }
        if (de != OK) {
            hostRef->destroy();
            return zym_newNull();
        }
    }

    Ref<ENetPacketPeer> p = hostRef->connect_to_host(hostS, port, channels, 0);
    if (p.is_null()) {
        hostRef->destroy();
        return zym_newNull();
    }

    ZymValue hostObj = makeEnetInstance(vm, hostRef, channels, false);
    zym_pushRoot(vm, hostObj);
    ZymValue peerObj = makeEnetPeerInstance(vm, p);
    zym_pushRoot(vm, peerObj);

    ZymValue out = zym_newMap(vm);
    zym_pushRoot(vm, out);
    zym_mapSet(vm, out, "host", hostObj);
    zym_mapSet(vm, out, "peer", peerObj);
    zym_popRoot(vm); // out
    zym_popRoot(vm); // peerObj
    zym_popRoot(vm); // hostObj
    return out;
}

// ENet.listen(host, port, [maxPeers=32, channels=8, opts])
//   If `opts.tls` is provided as `{ key, cert }`, the listening host is
//   wrapped with a DTLS server: every inbound connection runs through a
//   DTLS handshake before being delivered as a `connect` event from
//   `host.service(...)`.
static ZymValue f_enetListen(ZymVM* vm, ZymValue ctx, ZymValue hostV, ZymValue portV, ZymValue* vargs, int vargc) {
    (void)ctx;
    String hostS;
    if (!reqStr(vm, hostV, "listen(host, port, ...)", &hostS)) return ZYM_ERROR;
    double portD = 0;
    if (!reqNum(vm, portV, "listen(host, port, ...)", &portD)) return ZYM_ERROR;
    int port = (int)portD;

    int maxPeers = 32;
    int channels = 8;
    if (vargc >= 1) {
        if (!zym_isNumber(vargs[0])) { zym_runtimeError(vm, "listen: maxPeers must be a number"); return ZYM_ERROR; }
        maxPeers = (int)zym_asNumber(vargs[0]);
        if (maxPeers < 1) { zym_runtimeError(vm, "listen: maxPeers must be >= 1"); return ZYM_ERROR; }
    }
    if (vargc >= 2) {
        if (!zym_isNumber(vargs[1])) { zym_runtimeError(vm, "listen: channels must be a number"); return ZYM_ERROR; }
        channels = (int)zym_asNumber(vargs[1]);
        if (channels < 1 || channels > 255) {
            zym_runtimeError(vm, "listen: channels must be in [1, 255]"); return ZYM_ERROR;
        }
    }
    ZymValue tlsV = zym_newNull();
    if (vargc >= 3) {
        if (!readTopOpts(vm, vargs[2], &tlsV, "ENet.listen")) return ZYM_ERROR;
    }

    Ref<ENetConnection> hostRef;
    hostRef.instantiate();
    IPAddress bindIp = parseBindHost(hostS);
    Error e = hostRef->create_host_bound(bindIp, port, maxPeers, channels, 0, 0);
    if (e != OK) return zym_newNull();

    if (!zym_isNull(tlsV)) {
        Ref<TLSOptions> opts = buildEnetServerOpts(vm, tlsV, "ENet.listen");
        if (opts.is_null()) { hostRef->destroy(); return ZYM_ERROR; }
        Error de = hostRef->dtls_server_setup(opts);
        if (de == ERR_UNAVAILABLE) {
            zym_runtimeError(vm, "ENet.listen: DTLS unavailable in this build (GODOT_ENET / mbedtls)");
            hostRef->destroy();
            return ZYM_ERROR;
        }
        if (de != OK) {
            hostRef->destroy();
            return zym_newNull();
        }
    }

    return makeEnetInstance(vm, hostRef, channels, true);
}

// host.refuseNewConnections(bool) — server-side only. Stops accepting new
// inbound connections; existing peers are unaffected. Useful to drain a
// DTLS server gracefully before shutdown. Returns null.
static ZymValue enet_refuseNewConnections(ZymVM* vm, ZymValue ctx, ZymValue refuseV) {
    auto* h = unwrapEnet(ctx);
    if (!h || h->host.is_null()) return zym_newNull();
    if (!zym_isBool(refuseV)) {
        zym_runtimeError(vm, "refuseNewConnections(refuse): refuse must be a bool");
        return ZYM_ERROR;
    }
    h->host->refuse_new_connections(zym_asBool(refuseV));
    return zym_newNull();
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

static ZymValue makeEnetInstance(ZymVM* vm, Ref<ENetConnection> host, int maxChannels, bool bound) {
    auto* data = new EnetHandle{ host, maxChannels, bound };
    ZymValue ctx = zym_createNativeContext(vm, data, enetFinalizer);
    zym_pushRoot(vm, ctx);
    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);
    zym_mapSet(vm, obj, "__enet__", ctx);
    M (obj, ctx, "service",   "service(timeoutMs)",            enet_service);
    M (obj, ctx, "flush",     "flush()",                       enet_flush);
    M (obj, ctx, "localPort", "localPort()",                   enet_localPort);
    M (obj, ctx, "broadcast", "broadcast(buf, channel, mode)", enet_broadcast);
    M (obj, ctx, "refuseNewConnections", "refuseNewConnections(refuse)", enet_refuseNewConnections);
    M (obj, ctx, "close",     "close()",                       enet_close);
    zym_popRoot(vm); // obj
    zym_popRoot(vm); // ctx
    return obj;
}

static ZymValue makeEnetPeerInstance(ZymVM* vm, Ref<ENetPacketPeer> peer) {
    auto* data = new EnetPeerHandle{ peer };
    ZymValue ctx = zym_createNativeContext(vm, data, enetpFinalizer);
    zym_pushRoot(vm, ctx);
    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);
    zym_mapSet(vm, obj, "__enetp__", ctx);
    M (obj, ctx, "status",        "status()",                  enetp_status);
    M (obj, ctx, "send",          "send(buf, channel, mode)",  enetp_send);
    M (obj, ctx, "peerAddress",   "peerAddress()",             enetp_peerAddress);
    M (obj, ctx, "ping",          "ping()",                    enetp_ping);
    M (obj, ctx, "pingMs",        "pingMs()",                  enetp_pingMs);
    MV(obj, ctx, "disconnect",    "disconnect(...)",           enetp_disconnect);
    MV(obj, ctx, "disconnectNow", "disconnectNow(...)",        enetp_disconnectNow);
    M (obj, ctx, "close",         "close()",                   enetp_close);
    zym_popRoot(vm); // obj
    zym_popRoot(vm); // ctx
    return obj;
}

#undef M
#undef MV

// ============================================================================
// Global factory
// ============================================================================

ZymValue nativeEnet_create(ZymVM* vm) {
    ZymValue ctx = zym_createNativeContext(vm, nullptr, nullptr);
    zym_pushRoot(vm, ctx);
    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);

#define FV(name, sig, fn) do { \
    ZymValue cl = zym_createNativeClosureVariadic(vm, sig, (void*)fn, ctx); \
    zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
} while (0)

    FV("connect", "connect(host, port, ...)", f_enetConnect);
    FV("listen",  "listen(host, port, ...)",  f_enetListen);

#undef FV

    zym_popRoot(vm); // obj
    zym_popRoot(vm); // ctx
    return obj;
}
