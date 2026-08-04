// Regression: an enum constant whose type_id is out of range must be
// rejected by the loader.
//
// ENUM_VAL packs the field straight into a NaN-boxed Value:
//
//     #define ENUM_VAL(type_id, variant) \
//         ((Value)(QNAN | TAG_ENUM | ((uint64_t)(type_id) << 32) | ...))
//
// A negative type_id sign-extends through the cast to uint64_t, so the
// high bits become all-ones — including SIGN_BIT, which is exactly what
// IS_OBJ() tests. The Value stops reading as an enum and starts reading
// as an object pointer whose low bits the file controls. The next GC
// mark then writes through that address.
//
//     ENUM_VAL(1, 2)       = 0x7ff8000100020004  IS_OBJ=0   (legitimate)
//     ENUM_VAL(-1, 0x1234) = 0xffffffff12340004  IS_OBJ=1   forged pointer
//
// Before the loader validated this, patching four bytes of a valid .zbc
// segfaulted the process once GC ran. ENUM_TYPE_ID / ENUM_VARIANT only
// ever read 16 bits, so anything outside 0..0xFFFF is malformed by
// construction and must not reach ENUM_VAL.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zym/zym.h"

static const char* SRC = "enum E { A, B }\nvar v = E.B\n";

// Constant tag for an enum value in the serialized form.
#define TAG_ENUM_VALUE 0x09

int main(void) {
    ZymCompilerConfig cfg = { 1 };

    ZymVM* vm = zym_newVM(NULL);
    ZymChunk* chunk = zym_newChunk(vm);
    if (zym_compile(vm, SRC, chunk, NULL, "poc.zym", cfg, NULL) != ZYM_STATUS_OK) {
        printf("FAIL  could not compile fixture\n");
        return 1;
    }

    char* bytes = NULL;
    size_t size = 0;
    if (zym_serializeChunk(vm, cfg, chunk, &bytes, &size) != ZYM_STATUS_OK) {
        printf("FAIL  could not serialize fixture\n");
        return 1;
    }
    zym_freeChunk(vm, chunk);
    zym_freeVM(vm);

    // Locate the enum-value constant and force its type_id negative.
    int patched = 0;
    for (size_t i = 0; i + 9 <= size; i++) {
        if ((unsigned char)bytes[i] != TAG_ENUM_VALUE) continue;
        int type_id, variant;
        memcpy(&type_id, bytes + i + 1, sizeof(int));
        memcpy(&variant, bytes + i + 5, sizeof(int));
        if (type_id >= 0 && type_id < 4096 && variant >= 0 && variant < 4096) {
            int forged = -1;
            memcpy(bytes + i + 1, &forged, sizeof(int));
            patched = 1;
            break;
        }
    }
    if (!patched) {
        printf("FAIL  no enum constant found in fixture (format changed?)\n");
        free(bytes);
        return 1;
    }

    // The loader must refuse this outright.
    ZymVM* victim = zym_newVM(NULL);
    ZymChunk* loaded = zym_newChunk(victim);
    ZymStatus st = zym_deserializeChunk(victim, loaded, bytes, size);

    int failures = 0;
    if (st == ZYM_STATUS_OK) {
        printf("FAIL  loader ACCEPTED an enum constant with type_id=-1\n");
        failures++;

        // Demonstrate why that is fatal rather than merely untidy: run it
        // and apply enough allocation pressure to force a collection, which
        // marks every chunk constant — including the forged pointer.
        if (zym_runChunk(victim, loaded) == ZYM_STATUS_OK) {
            char buf[32];
            for (int i = 0; i < 300000; i++) {
                snprintf(buf, sizeof(buf), "churn-%d", i);
                zym_newString(victim, buf);
            }
        }
    } else {
        printf("PASS  loader rejected forged enum constant\n");
    }

    zym_freeChunk(victim, loaded);
    zym_freeVM(victim);
    free(bytes);

    printf(failures == 0 ? "ALL PASS\n" : "%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
