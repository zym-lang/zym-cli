#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cmath>

#include "natives.hpp"

static bool printFormattedValue(ZymVM* vm, char format, ZymValue val, int argIndex) {
    switch (format) {
        case 's':
            if (!zym_isString(val)) {
                zym_runtimeError(vm, "print() format %%s at position %d expects string, got %s", argIndex, zym_typeName(val));
                return false;
            }
            printf("%s", zym_asCString(val));
            break;

        case 'n':
            if (!zym_isNumber(val)) {
                zym_runtimeError(vm, "print() format %%n at position %d expects number, got %s", argIndex, zym_typeName(val));
                return false;
            }
            {
                double num = zym_asNumber(val);
                // Check if integer safely - avoid casting NaN/Inf to long long
                if (std::isfinite(num) && num == (double)(long long)num && num >= -1e15 && num <= 1e15) {
                    printf("%.0f", num);
                } else {
                    printf("%g", num);
                }
            }
            break;

        case 'b':
            if (!zym_isBool(val)) {
                zym_runtimeError(vm, "print() format %%b at position %d expects bool, got %s", argIndex, zym_typeName(val));
                return false;
            }
            printf("%s", zym_asBool(val) ? "true" : "false");
            break;

        case 'l':
            if (!zym_isList(val)) {
                zym_runtimeError(vm, "print() format %%l at position %d expects list, got %s", argIndex, zym_typeName(val));
                return false;
            }
            zym_printValue(vm, val);
            break;

        case 'm':
            if (!zym_isMap(val)) {
                zym_runtimeError(vm, "print() format %%m at position %d expects map, got %s", argIndex, zym_typeName(val));
                return false;
            }
            zym_printValue(vm, val);
            break;

        case 't':
            if (!zym_isStruct(val)) {
                zym_runtimeError(vm, "print() format %%t at position %d expects struct, got %s", argIndex, zym_typeName(val));
                return false;
            }
            zym_printValue(vm, val);
            break;

        case 'e':
            if (!zym_isEnum(val)) {
                zym_runtimeError(vm, "print() format %%e at position %d expects enum, got %s", argIndex, zym_typeName(val));
                return false;
            }
            zym_printValue(vm, val);
            break;

        case 'v':
            zym_printValue(vm, val);
            break;

        default:
            zym_runtimeError(vm, "print() unknown format specifier '%%%c'", format);
            return false;
    }
    return true;
}

static ZymValue print_impl(ZymVM* vm, const char* format_str, ZymValue* args, int arg_count) {
    const char* ptr = format_str;
    int arg_index = 0;

    while (*ptr) {
        if (*ptr == '%') {
            ptr++;
            if (*ptr == '\0') {
                zym_runtimeError(vm, "print() format string ends with incomplete format specifier");
                return ZYM_ERROR;
            }

            if (*ptr == '%') {
                printf("%%");
                ptr++;
            } else {
                if (arg_index >= arg_count) {
                    zym_runtimeError(vm, "print() format string requires more arguments than provided");
                    return ZYM_ERROR;
                }

                if (!printFormattedValue(vm, *ptr, args[arg_index], arg_index + 1)) {
                    return ZYM_ERROR;
                }

                arg_index++;
                ptr++;
            }
        } else {
            printf("%c", *ptr);
            ptr++;
        }
    }

    if (arg_index < arg_count) {
        zym_runtimeError(vm, "print() provided %d arguments but format string only uses %d", arg_count, arg_index);
        return ZYM_ERROR;
    }

    printf("\n");
    return zym_newNull();
}

ZymValue nativePrint(ZymVM* vm, ZymValue* args, int argc) {
    if (argc == 0) {
        printf("\n");
        return zym_newNull();
    }

    if (argc == 1) {
        ZymValue value = args[0];
        if (zym_isString(value)) {
            const char* str = zym_asCString(value);
            bool has_format = false;
            for (const char* p = str; *p; p++) {
                if (*p == '%' && *(p + 1) != '%') {
                    has_format = true;
                    break;
                }
            }

            if (has_format) {
                return print_impl(vm, str, nullptr, 0);
            }
        }

        zym_printValue(vm, value);
        printf("\n");

        return zym_newNull();
    }

    // 2+ args: first arg is format string
    if (!zym_isString(args[0])) {
        zym_runtimeError(vm, "print() first argument must be a string");
        return ZYM_ERROR;
    }

    return print_impl(vm, zym_asCString(args[0]), &args[1], argc - 1);
}
