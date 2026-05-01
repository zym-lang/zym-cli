#pragma once

#include "zym/zym.h"

void setupNatives(ZymVM* vm);

ZymValue nativePrint(ZymVM* vm, ZymValue* args, int argc);
ZymValue nativeTime_create(ZymVM* vm);
ZymValue nativeBuffer_create(ZymVM* vm);
ZymValue nativeFile_create(ZymVM* vm);
ZymValue nativeDir_create(ZymVM* vm);
ZymValue nativeConsole_create(ZymVM* vm);
ZymValue nativeProcess_create(ZymVM* vm);
ZymValue nativeRegex_create(ZymVM* vm);
ZymValue nativeJson_create(ZymVM* vm);
ZymValue nativeCrypto_create(ZymVM* vm);
ZymValue nativeRandom_create(ZymVM* vm);
ZymValue nativeHash_create(ZymVM* vm);
ZymValue nativeSystem_create(ZymVM* vm);
ZymValue nativePath_create(ZymVM* vm);
ZymValue nativeIp_create(ZymVM* vm);
ZymValue nativeTcp_create(ZymVM* vm);
ZymValue nativeUdp_create(ZymVM* vm);
ZymValue nativeSockets_create(ZymVM* vm);
ZymValue nativeTls_create(ZymVM* vm);
ZymValue nativeDtls_create(ZymVM* vm);
ZymValue nativeEnet_create(ZymVM* vm);
ZymValue nativeAes_create(ZymVM* vm);
