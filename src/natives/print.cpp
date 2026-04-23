// Godot-backed print native: builds a String and emits via print_line().
#include <cmath>

#include "core/string/print_string.h"
#include "core/string/ustring.h"
#include "core/variant/variant.h"

#include "natives.hpp"

static bool appendFormatted(ZymVM* vm, String& out, char format, ZymValue val, int argIndex) {
    switch (format) {
        case 's':
            if (!zym_isString(val)) {
                zym_runtimeError(vm, "print() format %%s at position %d expects string, got %s", argIndex, zym_typeName(val));
                return false;
            }
            out += String::utf8(zym_asCString(val));
            break;

        case 'n': {
            if (!zym_isNumber(val)) {
                zym_runtimeError(vm, "print() format %%n at position %d expects number, got %s", argIndex, zym_typeName(val));
                return false;
            }
            double num = zym_asNumber(val);
            if (std::isfinite(num) && num == (double)(long long)num && num >= -1e15 && num <= 1e15) {
                out += itos((int64_t)num);
            } else {
                out += rtos(num);
            }
            break;
        }

        case 'b':
            if (!zym_isBool(val)) {
                zym_runtimeError(vm, "print() format %%b at position %d expects bool, got %s", argIndex, zym_typeName(val));
                return false;
            }
            out += zym_asBool(val) ? "true" : "false";
            break;

        case 'l':
            if (!zym_isList(val)) {
                zym_runtimeError(vm, "print() format %%l at position %d expects list, got %s", argIndex, zym_typeName(val));
                return false;
            }
            goto stringify;

        case 'm':
            if (!zym_isMap(val)) {
                zym_runtimeError(vm, "print() format %%m at position %d expects map, got %s", argIndex, zym_typeName(val));
                return false;
            }
            goto stringify;

        case 't':
            if (!zym_isStruct(val)) {
                zym_runtimeError(vm, "print() format %%t at position %d expects struct, got %s", argIndex, zym_typeName(val));
                return false;
            }
            goto stringify;

        case 'e':
            if (!zym_isEnum(val)) {
                zym_runtimeError(vm, "print() format %%e at position %d expects enum, got %s", argIndex, zym_typeName(val));
                return false;
            }
            goto stringify;

        case 'v':
        stringify: {
            ZymValue s = zym_valueToString(vm, val);
            if (zym_isString(s)) {
                out += String::utf8(zym_asCString(s));
            }
            break;
        }

        default:
            zym_runtimeError(vm, "print() unknown format specifier '%%%c'", format);
            return false;
    }
    return true;
}

static ZymValue print_impl(ZymVM* vm, const char* format_str, ZymValue* args, int arg_count) {
    String out;
    const char* ptr = format_str;
    const char* run = ptr;
    int arg_index = 0;

    auto flush_run = [&](const char* end) {
        if (end > run) {
            out += String::utf8(run, (int)(end - run));
        }
    };

    while (*ptr) {
        if (*ptr == '%') {
            flush_run(ptr);
            ptr++;
            if (*ptr == '\0') {
                zym_runtimeError(vm, "print() format string ends with incomplete format specifier");
                return ZYM_ERROR;
            }

            if (*ptr == '%') {
                out += "%";
            } else {
                if (arg_index >= arg_count) {
                    zym_runtimeError(vm, "print() format string requires more arguments than provided");
                    return ZYM_ERROR;
                }
                if (!appendFormatted(vm, out, *ptr, args[arg_index], arg_index + 1)) {
                    return ZYM_ERROR;
                }
                arg_index++;
            }
            ptr++;
            run = ptr;
        } else {
            ptr++;
        }
    }
    flush_run(ptr);

    if (arg_index < arg_count) {
        zym_runtimeError(vm, "print() provided %d arguments but format string only uses %d", arg_count, arg_index);
        return ZYM_ERROR;
    }

    print_line(out);
    return zym_newNull();
}

ZymValue nativePrint(ZymVM* vm, ZymValue* args, int argc) {
    if (argc == 0) {
        print_line(String());
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

        ZymValue s = zym_valueToString(vm, value);
        if (zym_isString(s)) {
            print_line(String::utf8(zym_asCString(s)));
        } else {
            print_line(String());
        }
        return zym_newNull();
    }

    if (!zym_isString(args[0])) {
        zym_runtimeError(vm, "print() first argument must be a string");
        return ZYM_ERROR;
    }

    return print_impl(vm, zym_asCString(args[0]), &args[1], argc - 1);
}
