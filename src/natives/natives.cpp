#include "./natives.hpp"

void setupNatives(ZymVM* vm)
{
    zym_defineNativeVariadic(vm, "print(...)", (void*)nativePrint);
    zym_defineGlobal(vm, "Time", nativeTime_create(vm));
    zym_defineGlobal(vm, "Buffer", nativeBuffer_create(vm));
}
