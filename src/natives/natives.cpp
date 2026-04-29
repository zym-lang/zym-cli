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
}
