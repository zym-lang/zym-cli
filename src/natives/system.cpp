// System native — abstraction over the OS singleton.
//
// Exposes:
//   System.osName(), System.distribution(), System.osVersion(), System.modelName()
//   System.cpuName(), System.cpuCount(), System.uniqueId()
//   System.locale(), System.localeLanguage(), System.hasFeature(name)
//   System.executablePath()
//   System.dataDir(), System.configDir(), System.cacheDir(), System.tempDir()
//   System.systemDir(name)   -- desktop-only: desktop|documents|downloads|movies|music|pictures
//   System.sleep(ms), System.sleepUsec(usec)
//   System.getEnv(name), System.hasEnv(name), System.setEnv(name, value), System.unsetEnv(name)

#include "core/os/os.h"
#include "core/string/ustring.h"
#include "core/variant/variant.h"

#include "natives.hpp"

namespace {

ZymValue stringToZym(ZymVM* vm, const String& s) {
    CharString u = s.utf8();
    return zym_newStringN(vm, u.get_data(), (int)u.length());
}

bool requireNumber(ZymVM* vm, ZymValue v, const char* where, double* out) {
    if (!zym_isNumber(v)) {
        zym_runtimeError(vm, "%s: argument must be a number", where);
        return false;
    }
    *out = zym_asNumber(v);
    return true;
}

bool requireString(ZymVM* vm, ZymValue v, const char* where, String* out) {
    if (!zym_isString(v)) {
        zym_runtimeError(vm, "%s: argument must be a string", where);
        return false;
    }
    *out = String::utf8(zym_asCString(v));
    return true;
}

// ---- identity ----

ZymValue s_osName(ZymVM* vm, ZymValue) {
    return stringToZym(vm, OS::get_singleton()->get_name());
}

ZymValue s_distribution(ZymVM* vm, ZymValue) {
    return stringToZym(vm, OS::get_singleton()->get_distribution_name());
}

ZymValue s_osVersion(ZymVM* vm, ZymValue) {
    return stringToZym(vm, OS::get_singleton()->get_version());
}

ZymValue s_modelName(ZymVM* vm, ZymValue) {
    return stringToZym(vm, OS::get_singleton()->get_model_name());
}

// ---- hardware ----

ZymValue s_cpuName(ZymVM* vm, ZymValue) {
    return stringToZym(vm, OS::get_singleton()->get_processor_name());
}

ZymValue s_cpuCount(ZymVM*, ZymValue) {
    return zym_newNumber((double)OS::get_singleton()->get_processor_count());
}

ZymValue s_uniqueId(ZymVM* vm, ZymValue) {
    return stringToZym(vm, OS::get_singleton()->get_unique_id());
}

// ---- locale / features ----

ZymValue s_locale(ZymVM* vm, ZymValue) {
    return stringToZym(vm, OS::get_singleton()->get_locale());
}

ZymValue s_localeLanguage(ZymVM* vm, ZymValue) {
    return stringToZym(vm, OS::get_singleton()->get_locale_language());
}

ZymValue s_hasFeature(ZymVM* vm, ZymValue, ZymValue nameV) {
    String name;
    if (!requireString(vm, nameV, "System.hasFeature(name)", &name)) return ZYM_ERROR;
    return zym_newBool(OS::get_singleton()->has_feature(name));
}

// ---- process info ----

ZymValue s_executablePath(ZymVM* vm, ZymValue) {
    return stringToZym(vm, OS::get_singleton()->get_executable_path());
}

// ---- directories ----

ZymValue s_dataDir(ZymVM* vm, ZymValue) {
    return stringToZym(vm, OS::get_singleton()->get_data_path());
}

ZymValue s_configDir(ZymVM* vm, ZymValue) {
    return stringToZym(vm, OS::get_singleton()->get_config_path());
}

ZymValue s_cacheDir(ZymVM* vm, ZymValue) {
    return stringToZym(vm, OS::get_singleton()->get_cache_path());
}

ZymValue s_tempDir(ZymVM* vm, ZymValue) {
    return stringToZym(vm, OS::get_singleton()->get_temp_path());
}

ZymValue s_systemDir(ZymVM* vm, ZymValue, ZymValue nameV) {
    String name;
    if (!requireString(vm, nameV, "System.systemDir(name)", &name)) return ZYM_ERROR;
    String lower = name.to_lower();
    OS::SystemDir kind;
    if (lower == "desktop")        kind = OS::SYSTEM_DIR_DESKTOP;
    else if (lower == "documents") kind = OS::SYSTEM_DIR_DOCUMENTS;
    else if (lower == "downloads") kind = OS::SYSTEM_DIR_DOWNLOADS;
    else if (lower == "movies")    kind = OS::SYSTEM_DIR_MOVIES;
    else if (lower == "music")     kind = OS::SYSTEM_DIR_MUSIC;
    else if (lower == "pictures")  kind = OS::SYSTEM_DIR_PICTURES;
    else {
        CharString u = name.utf8();
        zym_runtimeError(vm,
            "System.systemDir(name): unknown directory '%s' "
            "(expected one of: desktop, documents, downloads, movies, music, pictures)",
            u.get_data());
        return ZYM_ERROR;
    }
    return stringToZym(vm, OS::get_singleton()->get_system_dir(kind, true));
}

// ---- sleep ----

ZymValue s_sleep(ZymVM* vm, ZymValue, ZymValue msV) {
    double ms;
    if (!requireNumber(vm, msV, "System.sleep(ms)", &ms)) return ZYM_ERROR;
    if (ms < 0) ms = 0;
    OS::get_singleton()->delay_usec((uint32_t)(ms * 1000.0));
    return zym_newNull();
}

ZymValue s_sleepUsec(ZymVM* vm, ZymValue, ZymValue usecV) {
    double usec;
    if (!requireNumber(vm, usecV, "System.sleepUsec(usec)", &usec)) return ZYM_ERROR;
    if (usec < 0) usec = 0;
    OS::get_singleton()->delay_usec((uint32_t)usec);
    return zym_newNull();
}

// ---- environment ----

ZymValue s_getEnv(ZymVM* vm, ZymValue, ZymValue nameV) {
    String name;
    if (!requireString(vm, nameV, "System.getEnv(name)", &name)) return ZYM_ERROR;
    if (!OS::get_singleton()->has_environment(name)) return zym_newNull();
    return stringToZym(vm, OS::get_singleton()->get_environment(name));
}

ZymValue s_hasEnv(ZymVM* vm, ZymValue, ZymValue nameV) {
    String name;
    if (!requireString(vm, nameV, "System.hasEnv(name)", &name)) return ZYM_ERROR;
    return zym_newBool(OS::get_singleton()->has_environment(name));
}

ZymValue s_setEnv(ZymVM* vm, ZymValue, ZymValue nameV, ZymValue valV) {
    String name, val;
    if (!requireString(vm, nameV, "System.setEnv(name, value)", &name)) return ZYM_ERROR;
    if (!requireString(vm, valV,  "System.setEnv(name, value)", &val))  return ZYM_ERROR;
    OS::get_singleton()->set_environment(name, val);
    return zym_newNull();
}

ZymValue s_unsetEnv(ZymVM* vm, ZymValue, ZymValue nameV) {
    String name;
    if (!requireString(vm, nameV, "System.unsetEnv(name)", &name)) return ZYM_ERROR;
    OS::get_singleton()->unset_environment(name);
    return zym_newNull();
}

} // namespace

// ---- factory ----

ZymValue nativeSystem_create(ZymVM* vm) {
    ZymValue context = zym_createNativeContext(vm, nullptr, nullptr);
    zym_pushRoot(vm, context);

#define METHOD(name, sig, fn) \
    ZymValue name = zym_createNativeClosure(vm, sig, (void*)fn, context); \
    zym_pushRoot(vm, name);

    METHOD(osName,          "osName()",                 s_osName)
    METHOD(distribution,    "distribution()",           s_distribution)
    METHOD(osVersion,       "osVersion()",              s_osVersion)
    METHOD(modelName,       "modelName()",              s_modelName)
    METHOD(cpuName,         "cpuName()",                s_cpuName)
    METHOD(cpuCount,        "cpuCount()",               s_cpuCount)
    METHOD(uniqueId,        "uniqueId()",               s_uniqueId)
    METHOD(localeMethod,    "locale()",                 s_locale)
    METHOD(localeLanguage,  "localeLanguage()",         s_localeLanguage)
    METHOD(hasFeature,      "hasFeature(name)",         s_hasFeature)
    METHOD(executablePath,  "executablePath()",         s_executablePath)
    METHOD(dataDir,         "dataDir()",                s_dataDir)
    METHOD(configDir,       "configDir()",              s_configDir)
    METHOD(cacheDir,        "cacheDir()",               s_cacheDir)
    METHOD(tempDir,         "tempDir()",                s_tempDir)
    METHOD(systemDir,       "systemDir(name)",          s_systemDir)
    METHOD(sleepMethod,     "sleep(ms)",                s_sleep)
    METHOD(sleepUsec,       "sleepUsec(usec)",          s_sleepUsec)
    METHOD(getEnv,          "getEnv(name)",             s_getEnv)
    METHOD(hasEnv,          "hasEnv(name)",             s_hasEnv)
    METHOD(setEnv,          "setEnv(name, value)",      s_setEnv)
    METHOD(unsetEnv,        "unsetEnv(name)",           s_unsetEnv)

#undef METHOD

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);

    zym_mapSet(vm, obj, "osName",          osName);
    zym_mapSet(vm, obj, "distribution",    distribution);
    zym_mapSet(vm, obj, "osVersion",       osVersion);
    zym_mapSet(vm, obj, "modelName",       modelName);
    zym_mapSet(vm, obj, "cpuName",         cpuName);
    zym_mapSet(vm, obj, "cpuCount",        cpuCount);
    zym_mapSet(vm, obj, "uniqueId",        uniqueId);
    zym_mapSet(vm, obj, "locale",          localeMethod);
    zym_mapSet(vm, obj, "localeLanguage",  localeLanguage);
    zym_mapSet(vm, obj, "hasFeature",      hasFeature);
    zym_mapSet(vm, obj, "executablePath",  executablePath);
    zym_mapSet(vm, obj, "dataDir",         dataDir);
    zym_mapSet(vm, obj, "configDir",       configDir);
    zym_mapSet(vm, obj, "cacheDir",        cacheDir);
    zym_mapSet(vm, obj, "tempDir",         tempDir);
    zym_mapSet(vm, obj, "systemDir",       systemDir);
    zym_mapSet(vm, obj, "sleep",           sleepMethod);
    zym_mapSet(vm, obj, "sleepUsec",       sleepUsec);
    zym_mapSet(vm, obj, "getEnv",          getEnv);
    zym_mapSet(vm, obj, "hasEnv",          hasEnv);
    zym_mapSet(vm, obj, "setEnv",          setEnv);
    zym_mapSet(vm, obj, "unsetEnv",        unsetEnv);

    // context + 22 methods + obj = 24
    for (int i = 0; i < 24; i++) zym_popRoot(vm);

    return obj;
}
