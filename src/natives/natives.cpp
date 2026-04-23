#include "./natives.hpp"

void setupNatives(ZymVM* vm)
{
    zym_defineNativeVariadic(vm, "print(...)", (void*)nativePrint);
    zym_defineNative(vm, "clock()", (void*)nativeTime_clock);
}
