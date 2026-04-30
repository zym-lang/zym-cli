// IP namespace -- DNS resolution and local-interface enumeration.
//
// All methods are synchronous and operate on the engine's `IP` singleton
// brought up in `src/boot/register_core.cpp`. There is no async resolver
// queue exposed here -- scripts that need concurrency drive it themselves
// via the TCP/UDP non-blocking primitives (see docs/sockets.md).
//
// Surface (see docs/sockets.md):
//   IP.resolve(host)         -> string | null
//   IP.resolveAll(host)      -> list<string>
//   IP.localAddresses()      -> list<string>
#include "core/io/ip.h"
#include "core/io/ip_address.h"
#include "core/string/ustring.h"
#include "core/templates/list.h"
#include "core/variant/variant.h"

#include "natives.hpp"

static ZymValue stringToZym(ZymVM* vm, const String& s) {
    CharString utf8 = s.utf8();
    return zym_newStringN(vm, utf8.get_data(), utf8.length());
}

static bool reqString(ZymVM* vm, ZymValue v, const char* where, String* out) {
    if (!zym_isString(v)) {
        zym_runtimeError(vm, "%s expects a string", where);
        return false;
    }
    *out = String::utf8(zym_asCString(v));
    return true;
}

// resolve(host) -> first IPv4-or-IPv6 address as string, or null on
// failure / NXDOMAIN. Synchronous; uses TYPE_ANY so the system resolver
// picks whichever family answers first (typically v4 on dual-stack
// hosts; on v6-only networks you'll get a v6 answer).
static ZymValue ip_resolve(ZymVM* vm, ZymValue, ZymValue hostV) {
    String host;
    if (!reqString(vm, hostV, "IP.resolve(host)", &host)) return ZYM_ERROR;
    IP* ip = IP::get_singleton();
    if (!ip) {
        zym_runtimeError(vm, "IP.resolve(host) IP singleton is not initialised");
        return ZYM_ERROR;
    }
    IPAddress addr = ip->resolve_hostname(host, IP::TYPE_ANY);
    if (!addr.is_valid()) return zym_newNull();
    return stringToZym(vm, String(addr));
}

// resolveAll(host) -> list of address strings (possibly empty). Each
// entry is a textual IP; ordering is whatever the resolver returned.
// Returns an empty list on failure rather than null so callers can
// always iterate the result without a null-check.
static ZymValue ip_resolveAll(ZymVM* vm, ZymValue, ZymValue hostV) {
    String host;
    if (!reqString(vm, hostV, "IP.resolveAll(host)", &host)) return ZYM_ERROR;
    IP* ip = IP::get_singleton();
    if (!ip) {
        zym_runtimeError(vm, "IP.resolveAll(host) IP singleton is not initialised");
        return ZYM_ERROR;
    }
    PackedStringArray addrs = ip->resolve_hostname_addresses(host, IP::TYPE_ANY);
    ZymValue out = zym_newList(vm);
    zym_pushRoot(vm, out);
    for (int i = 0; i < addrs.size(); i++) {
        ZymValue s = stringToZym(vm, addrs[i]);
        zym_listAppend(vm, out, s);
    }
    zym_popRoot(vm);
    return out;
}

// localAddresses() -> list of textual IPs assigned to local interfaces.
// Includes loopback (`127.0.0.1` / `::1`). Useful for servers binding
// to a "real" interface, or for diagnostics.
static ZymValue ip_localAddresses(ZymVM* vm, ZymValue) {
    IP* ip = IP::get_singleton();
    if (!ip) {
        zym_runtimeError(vm, "IP.localAddresses() IP singleton is not initialised");
        return ZYM_ERROR;
    }
    List<IPAddress> addrs;
    ip->get_local_addresses(&addrs);
    ZymValue out = zym_newList(vm);
    zym_pushRoot(vm, out);
    for (const IPAddress& a : addrs) {
        ZymValue s = stringToZym(vm, String(a));
        zym_listAppend(vm, out, s);
    }
    zym_popRoot(vm);
    return out;
}

// ---- assembly ----

ZymValue nativeIp_create(ZymVM* vm) {
    ZymValue context = zym_createNativeContext(vm, nullptr, nullptr);
    zym_pushRoot(vm, context);

#define M(name, sig, fn) \
    ZymValue name = zym_createNativeClosure(vm, sig, (void*)fn, context); \
    zym_pushRoot(vm, name);

    M(resolve,        "resolve(host)",     ip_resolve)
    M(resolveAll,     "resolveAll(host)",  ip_resolveAll)
    M(localAddresses, "localAddresses()",  ip_localAddresses)

#undef M

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);

    zym_mapSet(vm, obj, "resolve",        resolve);
    zym_mapSet(vm, obj, "resolveAll",     resolveAll);
    zym_mapSet(vm, obj, "localAddresses", localAddresses);

    // context + 3 methods + obj = 5
    for (int i = 0; i < 5; i++) zym_popRoot(vm);
    return obj;
}
