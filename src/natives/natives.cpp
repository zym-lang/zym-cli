#include "./natives.hpp"

void setupNatives(ZymVM* vm)
{
    zym_defineNativeVariadic(vm, "print(...)", (void*)nativePrint);
    zym_defineGlobal(vm, "Time", nativeTime_create(vm));
    zym_defineGlobal(vm, "Buffer", nativeBuffer_create(vm));
    zym_defineGlobal(vm, "File", nativeFile_create(vm));
    zym_defineGlobal(vm, "Dir", nativeDir_create(vm));
    zym_defineGlobal(vm, "Console", nativeConsole_create(vm));
    zym_defineGlobal(vm, "Process", nativeProcess_create(vm));
    zym_defineGlobal(vm, "RegEx", nativeRegex_create(vm));
    zym_defineGlobal(vm, "JSON", nativeJson_create(vm));
    zym_defineGlobal(vm, "Crypto", nativeCrypto_create(vm));
    zym_defineGlobal(vm, "Random", nativeRandom_create(vm));
    zym_defineGlobal(vm, "Hash", nativeHash_create(vm));
    zym_defineGlobal(vm, "System", nativeSystem_create(vm));
    zym_defineGlobal(vm, "Path", nativePath_create(vm));
    zym_defineGlobal(vm, "IP", nativeIp_create(vm));
    zym_defineGlobal(vm, "TCP", nativeTcp_create(vm));
    zym_defineGlobal(vm, "UDP", nativeUdp_create(vm));
    zym_defineGlobal(vm, "TLS", nativeTls_create(vm));
    zym_defineGlobal(vm, "DTLS", nativeDtls_create(vm));
    zym_defineGlobal(vm, "ENet", nativeEnet_create(vm));
    zym_defineGlobal(vm, "Sockets", nativeSockets_create(vm));
}
