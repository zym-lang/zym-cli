// Godot-backed Time singleton.
#include <ctime>

#include "core/os/os.h"
#include "core/os/time.h"
#include "core/string/ustring.h"
#include "core/variant/dictionary.h"
#include "core/variant/variant.h"

#include "natives.hpp"

static ZymValue stringToZym(ZymVM* vm, const String& s) {
    CharString utf8 = s.utf8();
    return zym_newStringN(vm, utf8.get_data(), utf8.length());
}

static ZymValue variantToZym(ZymVM* vm, const Variant& v) {
    switch (v.get_type()) {
        case Variant::BOOL:   return zym_newBool((bool)v);
        case Variant::INT:    return zym_newNumber((double)(int64_t)v);
        case Variant::FLOAT:  return zym_newNumber((double)v);
        case Variant::STRING: return stringToZym(vm, (String)v);
        default:              return zym_newNull();
    }
}

static void copyKey(ZymVM* vm, ZymValue map, const Dictionary& d, const char* key) {
    Variant v = d.get_valid(String(key));
    if (v.get_type() == Variant::NIL) return;
    zym_mapSet(vm, map, key, variantToZym(vm, v));
}

static ZymValue datetimeDictToZym(ZymVM* vm, const Dictionary& d) {
    ZymValue map = zym_newMap(vm);
    zym_pushRoot(vm, map);
    copyKey(vm, map, d, "year");
    copyKey(vm, map, d, "month");
    copyKey(vm, map, d, "day");
    copyKey(vm, map, d, "weekday");
    copyKey(vm, map, d, "hour");
    copyKey(vm, map, d, "minute");
    copyKey(vm, map, d, "second");
    copyKey(vm, map, d, "dst");
    zym_popRoot(vm);
    return map;
}

static ZymValue timezoneDictToZym(ZymVM* vm, const Dictionary& d) {
    ZymValue map = zym_newMap(vm);
    zym_pushRoot(vm, map);
    copyKey(vm, map, d, "bias");
    copyKey(vm, map, d, "name");
    zym_popRoot(vm);
    return map;
}

static bool requireBool(ZymVM* vm, ZymValue v, const char* where, bool* out) {
    if (!zym_isBool(v)) {
        zym_runtimeError(vm, "%s expects a bool", where);
        return false;
    }
    *out = zym_asBool(v);
    return true;
}

static bool requireNumber(ZymVM* vm, ZymValue v, const char* where, double* out) {
    if (!zym_isNumber(v)) {
        zym_runtimeError(vm, "%s expects a number", where);
        return false;
    }
    *out = zym_asNumber(v);
    return true;
}

static bool requireString(ZymVM* vm, ZymValue v, const char* where, String* out) {
    if (!zym_isString(v)) {
        zym_runtimeError(vm, "%s expects a string", where);
        return false;
    }
    *out = String::utf8(zym_asCString(v));
    return true;
}

static bool requireMap(ZymVM* vm, ZymValue v, const char* where) {
    if (!zym_isMap(v)) {
        zym_runtimeError(vm, "%s expects a map", where);
        return false;
    }
    return true;
}

static Dictionary zymMapToDatetimeDict(ZymVM* vm, ZymValue map) {
    Dictionary d;
    static const char* keys[] = { "year", "month", "day", "weekday", "hour", "minute", "second", "dst", nullptr };
    for (int i = 0; keys[i]; i++) {
        if (!zym_mapHas(map, keys[i])) continue;
        ZymValue v = zym_mapGet(vm, map, keys[i]);
        if (zym_isNumber(v))      d[String(keys[i])] = (int64_t)zym_asNumber(v);
        else if (zym_isBool(v))   d[String(keys[i])] = zym_asBool(v);
        else if (zym_isString(v)) d[String(keys[i])] = String::utf8(zym_asCString(v));
    }
    return d;
}

// ---- methods ----

static ZymValue t_now(ZymVM*, ZymValue) {
    return zym_newNumber(Time::get_singleton()->get_unix_time_from_system());
}

static ZymValue t_clock(ZymVM*, ZymValue) {
    return zym_newNumber((double)clock() / CLOCKS_PER_SEC);
}

static ZymValue t_ticksMsec(ZymVM*, ZymValue) {
    return zym_newNumber((double)Time::get_singleton()->get_ticks_msec());
}

static ZymValue t_ticksUsec(ZymVM*, ZymValue) {
    return zym_newNumber((double)Time::get_singleton()->get_ticks_usec());
}


static ZymValue t_datetime(ZymVM* vm, ZymValue, ZymValue utcVal) {
    bool utc;
    if (!requireBool(vm, utcVal, "Time.datetime(utc)", &utc)) return ZYM_ERROR;
    return datetimeDictToZym(vm, Time::get_singleton()->get_datetime_dict_from_system(utc));
}

static ZymValue t_date(ZymVM* vm, ZymValue, ZymValue utcVal) {
    bool utc;
    if (!requireBool(vm, utcVal, "Time.date(utc)", &utc)) return ZYM_ERROR;
    return datetimeDictToZym(vm, Time::get_singleton()->get_date_dict_from_system(utc));
}

static ZymValue t_timeOfDay(ZymVM* vm, ZymValue, ZymValue utcVal) {
    bool utc;
    if (!requireBool(vm, utcVal, "Time.timeOfDay(utc)", &utc)) return ZYM_ERROR;
    return datetimeDictToZym(vm, Time::get_singleton()->get_time_dict_from_system(utc));
}

static ZymValue t_datetimeString(ZymVM* vm, ZymValue, ZymValue utcVal, ZymValue spaceVal) {
    bool utc, useSpace;
    if (!requireBool(vm, utcVal,   "Time.datetimeString(utc, useSpace)", &utc))      return ZYM_ERROR;
    if (!requireBool(vm, spaceVal, "Time.datetimeString(utc, useSpace)", &useSpace)) return ZYM_ERROR;
    return stringToZym(vm, Time::get_singleton()->get_datetime_string_from_system(utc, useSpace));
}

static ZymValue t_dateString(ZymVM* vm, ZymValue, ZymValue utcVal) {
    bool utc;
    if (!requireBool(vm, utcVal, "Time.dateString(utc)", &utc)) return ZYM_ERROR;
    return stringToZym(vm, Time::get_singleton()->get_date_string_from_system(utc));
}

static ZymValue t_timeString(ZymVM* vm, ZymValue, ZymValue utcVal) {
    bool utc;
    if (!requireBool(vm, utcVal, "Time.timeString(utc)", &utc)) return ZYM_ERROR;
    return stringToZym(vm, Time::get_singleton()->get_time_string_from_system(utc));
}

static ZymValue t_datetimeFromUnix(ZymVM* vm, ZymValue, ZymValue tsVal) {
    double ts;
    if (!requireNumber(vm, tsVal, "Time.datetimeFromUnix(ts)", &ts)) return ZYM_ERROR;
    return datetimeDictToZym(vm, Time::get_singleton()->get_datetime_dict_from_unix_time((int64_t)ts));
}

static ZymValue t_dateFromUnix(ZymVM* vm, ZymValue, ZymValue tsVal) {
    double ts;
    if (!requireNumber(vm, tsVal, "Time.dateFromUnix(ts)", &ts)) return ZYM_ERROR;
    return datetimeDictToZym(vm, Time::get_singleton()->get_date_dict_from_unix_time((int64_t)ts));
}

static ZymValue t_timeOfDayFromUnix(ZymVM* vm, ZymValue, ZymValue tsVal) {
    double ts;
    if (!requireNumber(vm, tsVal, "Time.timeOfDayFromUnix(ts)", &ts)) return ZYM_ERROR;
    return datetimeDictToZym(vm, Time::get_singleton()->get_time_dict_from_unix_time((int64_t)ts));
}

static ZymValue t_datetimeStringFromUnix(ZymVM* vm, ZymValue, ZymValue tsVal, ZymValue spaceVal) {
    double ts;
    bool useSpace;
    if (!requireNumber(vm, tsVal, "Time.datetimeStringFromUnix(ts, useSpace)", &ts))    return ZYM_ERROR;
    if (!requireBool(vm, spaceVal, "Time.datetimeStringFromUnix(ts, useSpace)", &useSpace)) return ZYM_ERROR;
    return stringToZym(vm, Time::get_singleton()->get_datetime_string_from_unix_time((int64_t)ts, useSpace));
}

static ZymValue t_dateStringFromUnix(ZymVM* vm, ZymValue, ZymValue tsVal) {
    double ts;
    if (!requireNumber(vm, tsVal, "Time.dateStringFromUnix(ts)", &ts)) return ZYM_ERROR;
    return stringToZym(vm, Time::get_singleton()->get_date_string_from_unix_time((int64_t)ts));
}

static ZymValue t_timeStringFromUnix(ZymVM* vm, ZymValue, ZymValue tsVal) {
    double ts;
    if (!requireNumber(vm, tsVal, "Time.timeStringFromUnix(ts)", &ts)) return ZYM_ERROR;
    return stringToZym(vm, Time::get_singleton()->get_time_string_from_unix_time((int64_t)ts));
}

static ZymValue t_unixFromDatetimeString(ZymVM* vm, ZymValue, ZymValue sVal) {
    String s;
    if (!requireString(vm, sVal, "Time.unixFromDatetimeString(s)", &s)) return ZYM_ERROR;
    return zym_newNumber((double)Time::get_singleton()->get_unix_time_from_datetime_string(s));
}

static ZymValue t_datetimeFromDatetimeString(ZymVM* vm, ZymValue, ZymValue sVal, ZymValue wVal) {
    String s;
    bool weekday;
    if (!requireString(vm, sVal, "Time.datetimeFromDatetimeString(s, weekday)", &s))      return ZYM_ERROR;
    if (!requireBool(vm, wVal,   "Time.datetimeFromDatetimeString(s, weekday)", &weekday)) return ZYM_ERROR;
    return datetimeDictToZym(vm, Time::get_singleton()->get_datetime_dict_from_datetime_string(s, weekday));
}

static ZymValue t_unixFromDatetime(ZymVM* vm, ZymValue, ZymValue mapVal) {
    if (!requireMap(vm, mapVal, "Time.unixFromDatetime(map)")) return ZYM_ERROR;
    Dictionary d = zymMapToDatetimeDict(vm, mapVal);
    return zym_newNumber((double)Time::get_singleton()->get_unix_time_from_datetime_dict(d));
}

static ZymValue t_datetimeStringFromDatetime(ZymVM* vm, ZymValue, ZymValue mapVal, ZymValue spaceVal) {
    bool useSpace;
    if (!requireMap(vm, mapVal, "Time.datetimeStringFromDatetime(map, useSpace)"))         return ZYM_ERROR;
    if (!requireBool(vm, spaceVal, "Time.datetimeStringFromDatetime(map, useSpace)", &useSpace)) return ZYM_ERROR;
    Dictionary d = zymMapToDatetimeDict(vm, mapVal);
    return stringToZym(vm, Time::get_singleton()->get_datetime_string_from_datetime_dict(d, useSpace));
}

static ZymValue t_timezone(ZymVM* vm, ZymValue) {
    return timezoneDictToZym(vm, Time::get_singleton()->get_time_zone_from_system());
}

static ZymValue t_offsetString(ZymVM* vm, ZymValue, ZymValue minsVal) {
    double mins;
    if (!requireNumber(vm, minsVal, "Time.offsetString(minutes)", &mins)) return ZYM_ERROR;
    return stringToZym(vm, Time::get_singleton()->get_offset_string_from_offset_minutes((int64_t)mins));
}

// ---- singleton assembly ----

ZymValue nativeTime_create(ZymVM* vm) {
    ZymValue context = zym_createNativeContext(vm, nullptr, nullptr);
    zym_pushRoot(vm, context);

#define METHOD(name, sig, fn) \
    ZymValue name = zym_createNativeClosure(vm, sig, (void*)fn, context); \
    zym_pushRoot(vm, name);

    METHOD(now,                          "now()",                                     t_now)
    METHOD(clockMethod,                  "clock()",                                   t_clock)
    METHOD(ticksMsec,                    "ticksMsec()",                               t_ticksMsec)
    METHOD(ticksUsec,                    "ticksUsec()",                               t_ticksUsec)
    METHOD(datetime,                     "datetime(utc)",                             t_datetime)
    METHOD(date,                         "date(utc)",                                 t_date)
    METHOD(timeOfDay,                    "timeOfDay(utc)",                            t_timeOfDay)
    METHOD(datetimeString,               "datetimeString(utc, useSpace)",             t_datetimeString)
    METHOD(dateString,                   "dateString(utc)",                           t_dateString)
    METHOD(timeString,                   "timeString(utc)",                           t_timeString)
    METHOD(datetimeFromUnix,             "datetimeFromUnix(ts)",                      t_datetimeFromUnix)
    METHOD(dateFromUnix,                 "dateFromUnix(ts)",                          t_dateFromUnix)
    METHOD(timeOfDayFromUnix,            "timeOfDayFromUnix(ts)",                     t_timeOfDayFromUnix)
    METHOD(datetimeStringFromUnix,       "datetimeStringFromUnix(ts, useSpace)",      t_datetimeStringFromUnix)
    METHOD(dateStringFromUnix,           "dateStringFromUnix(ts)",                    t_dateStringFromUnix)
    METHOD(timeStringFromUnix,           "timeStringFromUnix(ts)",                    t_timeStringFromUnix)
    METHOD(unixFromDatetimeString,       "unixFromDatetimeString(s)",                 t_unixFromDatetimeString)
    METHOD(datetimeFromDatetimeString,   "datetimeFromDatetimeString(s, weekday)",    t_datetimeFromDatetimeString)
    METHOD(unixFromDatetime,             "unixFromDatetime(map)",                     t_unixFromDatetime)
    METHOD(datetimeStringFromDatetime,   "datetimeStringFromDatetime(map, useSpace)", t_datetimeStringFromDatetime)
    METHOD(timezone,                     "timezone()",                                t_timezone)
    METHOD(offsetString,                 "offsetString(minutes)",                     t_offsetString)

#undef METHOD

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);

    zym_mapSet(vm, obj, "now",                        now);
    zym_mapSet(vm, obj, "clock",                      clockMethod);
    zym_mapSet(vm, obj, "ticksMsec",                  ticksMsec);
    zym_mapSet(vm, obj, "ticksUsec",                  ticksUsec);
    zym_mapSet(vm, obj, "datetime",                   datetime);
    zym_mapSet(vm, obj, "date",                       date);
    zym_mapSet(vm, obj, "timeOfDay",                  timeOfDay);
    zym_mapSet(vm, obj, "datetimeString",             datetimeString);
    zym_mapSet(vm, obj, "dateString",                 dateString);
    zym_mapSet(vm, obj, "timeString",                 timeString);
    zym_mapSet(vm, obj, "datetimeFromUnix",           datetimeFromUnix);
    zym_mapSet(vm, obj, "dateFromUnix",               dateFromUnix);
    zym_mapSet(vm, obj, "timeOfDayFromUnix",          timeOfDayFromUnix);
    zym_mapSet(vm, obj, "datetimeStringFromUnix",     datetimeStringFromUnix);
    zym_mapSet(vm, obj, "dateStringFromUnix",         dateStringFromUnix);
    zym_mapSet(vm, obj, "timeStringFromUnix",         timeStringFromUnix);
    zym_mapSet(vm, obj, "unixFromDatetimeString",     unixFromDatetimeString);
    zym_mapSet(vm, obj, "datetimeFromDatetimeString", datetimeFromDatetimeString);
    zym_mapSet(vm, obj, "unixFromDatetime",           unixFromDatetime);
    zym_mapSet(vm, obj, "datetimeStringFromDatetime", datetimeStringFromDatetime);
    zym_mapSet(vm, obj, "timezone",                   timezone);
    zym_mapSet(vm, obj, "offsetString",               offsetString);

    // context + 22 methods + obj = 24
    for (int i = 0; i < 24; i++) zym_popRoot(vm);

    return obj;
}
