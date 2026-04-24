#pragma once

#include "zym/zym.h"

void setupNatives(ZymVM* vm);

ZymValue nativePrint(ZymVM* vm, ZymValue* args, int argc);
ZymValue nativeTime_create(ZymVM* vm);
ZymValue nativeBuffer_create(ZymVM* vm);
ZymValue nativeFile_create(ZymVM* vm);
ZymValue nativeDir_create(ZymVM* vm);
