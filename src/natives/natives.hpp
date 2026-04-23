#pragma once

#include "zym/zym.h"

void setupNatives(ZymVM* vm);

ZymValue nativePrint(ZymVM* vm, ZymValue* args, int argc);
ZymValue nativeTime_clock(ZymVM* vm);
