#include <ctime>
#include "./natives.hpp"

ZymValue nativeTime_clock(ZymVM* vm) {
    clock_t current = clock();
    double seconds = (double)current / CLOCKS_PER_SEC;
    return zym_newNumber(seconds);
}
