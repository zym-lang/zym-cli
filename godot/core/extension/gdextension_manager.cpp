/**************************************************************************/
/*  gdextension_manager.cpp                                               */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "gdextension_manager.h"

#include "core/extension/gdextension_function_loader.h"
#include "core/extension/gdextension_library_loader.h"
#include "core/extension/gdextension_special_compat_hashes.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/object/script_language.h"

GDExtensionManager::LoadStatus GDExtensionManager::_load_extension_internal(const Ref<GDExtension> &p_extension, bool p_first_load) {
	if (level >= 0) { // Already initialized up to some level.
		int32_t minimum_level = 0;
		if (!p_first_load) {
			minimum_level = p_extension->get_minimum_library_initialization_level();
			if (minimum_level < MIN(level, GDExtension::INITIALIZATION_LEVEL_SCENE)) {
				return LOAD_STATUS_NEEDS_RESTART;
			}
		}
		// Initialize up to current level.
		for (int32_t i = minimum_level; i <= level; i++) {
			p_extension->initialize_library(GDExtension::InitializationLevel(i));
		}
	}

	for (const KeyValue<String, String> &kv : p_extension->class_icon_paths) {
		gdextension_class_icon_paths[kv.key] = kv.value;
	}

	return LOAD_STATUS_OK;
}

void GDExtensionManager::_finish_load_extension(const Ref<GDExtension> &p_extension) {

	if (startup_callback_called) {
		// Extension is loading after the startup callback has already been called,
		// so we call it now for this extension to make sure it doesn't miss it.
		if (p_extension->startup_callback) {
			p_extension->startup_callback();
		}
	}
}

GDExtensionManager::LoadStatus GDExtensionManager::_unload_extension_internal(const Ref<GDExtension> &p_extension) {

	if (!shutdown_callback_called) {
		// Extension is unloading before the shutdown callback has been called,
		// which means the engine hasn't shutdown yet but we want to make sure
		// to call the shutdown callback so it doesn't miss it.
		if (p_extension->shutdown_callback) {
			p_extension->shutdown_callback();
		}
	}

	if (level >= 0) { // Already initialized up to some level.
		// Deinitialize down from current level.
		for (int32_t i = level; i >= GDExtension::INITIALIZATION_LEVEL_CORE; i--) {
			p_extension->deinitialize_library(GDExtension::InitializationLevel(i));
		}
	}

	for (const KeyValue<String, String> &kv : p_extension->class_icon_paths) {
		gdextension_class_icon_paths.erase(kv.key);
	}

	// Clear main loop callbacks.
	p_extension->startup_callback = nullptr;
	p_extension->shutdown_callback = nullptr;
	p_extension->frame_callback = nullptr;

	return LOAD_STATUS_OK;
}

GDExtensionManager::LoadStatus GDExtensionManager::load_extension(const String &p_path) {
	if (Engine::get_singleton()->is_recovery_mode_hint()) {
		return LOAD_STATUS_FAILED;
	}

	Ref<GDExtensionLibraryLoader> loader;
	loader.instantiate();
	return load_extension_with_loader(p_path, loader);
}

GDExtensionManager::LoadStatus GDExtensionManager::load_extension_from_function(const String &p_path, GDExtensionConstPtr<const GDExtensionInitializationFunction> p_init_func) {
	Ref<GDExtensionFunctionLoader> func_loader;
	func_loader.instantiate();
	func_loader->set_initialization_function((GDExtensionInitializationFunction)*p_init_func.data);
	return load_extension_with_loader(p_path, func_loader);
}

GDExtensionManager::LoadStatus GDExtensionManager::load_extension_with_loader(const String &p_path, const Ref<GDExtensionLoader> &p_loader) {
	DEV_ASSERT(p_loader.is_valid());

	if (gdextension_map.has(p_path)) {
		return LOAD_STATUS_ALREADY_LOADED;
	}

	Ref<GDExtension> extension;
	extension.instantiate();
	Error err = extension->open_library(p_path, p_loader);
	if (err != OK) {
		return LOAD_STATUS_FAILED;
	}

	LoadStatus status = _load_extension_internal(extension, true);
	if (status != LOAD_STATUS_OK) {
		return status;
	}

	_finish_load_extension(extension);

	extension->set_path(p_path);
	gdextension_map[p_path] = extension;
	return LOAD_STATUS_OK;
}

GDExtensionManager::LoadStatus GDExtensionManager::reload_extension(const String &p_path) {
	ERR_FAIL_V_MSG(LOAD_STATUS_FAILED, "GDExtensions can only be reloaded in an editor build.");
}

GDExtensionManager::LoadStatus GDExtensionManager::unload_extension(const String &p_path) {
	if (Engine::get_singleton()->is_recovery_mode_hint()) {
		return LOAD_STATUS_FAILED;
	}

	if (!gdextension_map.has(p_path)) {
		return LOAD_STATUS_NOT_LOADED;
	}

	Ref<GDExtension> extension = gdextension_map[p_path];

	LoadStatus status = _unload_extension_internal(extension);
	if (status != LOAD_STATUS_OK) {
		return status;
	}

	gdextension_map.erase(p_path);
	return LOAD_STATUS_OK;
}

bool GDExtensionManager::is_extension_loaded(const String &p_path) const {
	return gdextension_map.has(p_path);
}

Vector<String> GDExtensionManager::get_loaded_extensions() const {
	Vector<String> ret;
	for (const KeyValue<String, Ref<GDExtension>> &E : gdextension_map) {
		ret.push_back(E.key);
	}
	return ret;
}
Ref<GDExtension> GDExtensionManager::get_extension(const String &p_path) {
	HashMap<String, Ref<GDExtension>>::Iterator E = gdextension_map.find(p_path);
	ERR_FAIL_COND_V(!E, Ref<GDExtension>());
	return E->value;
}

bool GDExtensionManager::class_has_icon_path(const String &p_class) const {
	// TODO: Check that the icon belongs to a registered class somehow.
	return gdextension_class_icon_paths.has(p_class);
}

String GDExtensionManager::class_get_icon_path(const String &p_class) const {
	// TODO: Check that the icon belongs to a registered class somehow.
	if (gdextension_class_icon_paths.has(p_class)) {
		return gdextension_class_icon_paths[p_class];
	}
	return "";
}

void GDExtensionManager::initialize_extensions(GDExtension::InitializationLevel p_level) {
	if (Engine::get_singleton()->is_recovery_mode_hint()) {
		return;
	}

	ERR_FAIL_COND(int32_t(p_level) - 1 != level);
	for (KeyValue<String, Ref<GDExtension>> &E : gdextension_map) {
		E.value->initialize_library(p_level);

		if (p_level == GDExtension::INITIALIZATION_LEVEL_EDITOR) {
			for (const KeyValue<String, String> &kv : E.value->class_icon_paths) {
				gdextension_class_icon_paths[kv.key] = kv.value;
			}
		}
	}
	level = p_level;
}

void GDExtensionManager::deinitialize_extensions(GDExtension::InitializationLevel p_level) {
	if (Engine::get_singleton()->is_recovery_mode_hint()) {
		return;
	}

	ERR_FAIL_COND(int32_t(p_level) != level);
	for (KeyValue<String, Ref<GDExtension>> &E : gdextension_map) {
		E.value->deinitialize_library(p_level);
	}
	level = int32_t(p_level) - 1;
}


void GDExtensionManager::load_extensions() {
	if (Engine::get_singleton()->is_recovery_mode_hint()) {
		return;
	}

	Ref<FileAccess> f = FileAccess::open(GDExtension::get_extension_list_config_file(), FileAccess::READ);
	while (f.is_valid() && !f->eof_reached()) {
		String s = f->get_line().strip_edges();
		if (!s.is_empty()) {
			LoadStatus err = load_extension(s);
			ERR_CONTINUE_MSG(err == LOAD_STATUS_FAILED, vformat("Error loading extension: '%s'.", s));
		}
	}

	OS::get_singleton()->load_platform_gdextensions();
}

void GDExtensionManager::reload_extensions() {
}

bool GDExtensionManager::ensure_extensions_loaded(const HashSet<String> &p_extensions) {
	Vector<String> extensions_added;
	Vector<String> extensions_removed;

	for (const String &E : p_extensions) {
		if (!is_extension_loaded(E)) {
			extensions_added.push_back(E);
		}
	}

	Vector<String> loaded_extensions = get_loaded_extensions();
	for (const String &loaded_extension : loaded_extensions) {
		if (!p_extensions.has(loaded_extension)) {
			// The extension may not have a .gdextension file.
			const Ref<GDExtension> extension = GDExtensionManager::get_singleton()->get_extension(loaded_extension);
			if (!extension->get_loader()->library_exists()) {
				extensions_removed.push_back(loaded_extension);
			}
		}
	}

	String extension_list_config_file = GDExtension::get_extension_list_config_file();
	if (p_extensions.size()) {
		if (extensions_added.size() || extensions_removed.size()) {
			// Extensions were added or removed.
			Ref<FileAccess> f = FileAccess::open(extension_list_config_file, FileAccess::WRITE);
			for (const String &E : p_extensions) {
				f->store_line(E);
			}
		}
	} else {
		if (loaded_extensions.size() || FileAccess::exists(extension_list_config_file)) {
			// Extensions were removed.
			Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_RESOURCES);
			da->remove(extension_list_config_file);
		}
	}

	bool needs_restart = false;
	for (const String &extension : extensions_added) {
		GDExtensionManager::LoadStatus st = GDExtensionManager::get_singleton()->load_extension(extension);
		if (st == GDExtensionManager::LOAD_STATUS_NEEDS_RESTART) {
			needs_restart = true;
		}
	}

	for (const String &extension : extensions_removed) {
		GDExtensionManager::LoadStatus st = GDExtensionManager::get_singleton()->unload_extension(extension);
		if (st == GDExtensionManager::LOAD_STATUS_NEEDS_RESTART) {
			needs_restart = true;
		}
	}


	return needs_restart;
}

void GDExtensionManager::startup() {
	startup_callback_called = true;

	for (const KeyValue<String, Ref<GDExtension>> &E : gdextension_map) {
		const Ref<GDExtension> &extension = E.value;
		if (extension->startup_callback) {
			extension->startup_callback();
		}
	}
}

void GDExtensionManager::shutdown() {
	shutdown_callback_called = true;

	for (const KeyValue<String, Ref<GDExtension>> &E : gdextension_map) {
		const Ref<GDExtension> &extension = E.value;
		if (extension->shutdown_callback) {
			extension->shutdown_callback();
		}
	}
}

void GDExtensionManager::frame() {
	for (const KeyValue<String, Ref<GDExtension>> &E : gdextension_map) {
		const Ref<GDExtension> &extension = E.value;
		if (extension->frame_callback) {
			extension->frame_callback();
		}
	}
}

GDExtensionManager *GDExtensionManager::get_singleton() {
	return singleton;
}

void GDExtensionManager::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load_extension", "path"), &GDExtensionManager::load_extension);
	ClassDB::bind_method(D_METHOD("load_extension_from_function", "path", "init_func"), &GDExtensionManager::load_extension_from_function);
	ClassDB::bind_method(D_METHOD("reload_extension", "path"), &GDExtensionManager::reload_extension);
	ClassDB::bind_method(D_METHOD("unload_extension", "path"), &GDExtensionManager::unload_extension);
	ClassDB::bind_method(D_METHOD("is_extension_loaded", "path"), &GDExtensionManager::is_extension_loaded);

	ClassDB::bind_method(D_METHOD("get_loaded_extensions"), &GDExtensionManager::get_loaded_extensions);
	ClassDB::bind_method(D_METHOD("get_extension", "path"), &GDExtensionManager::get_extension);

	BIND_ENUM_CONSTANT(LOAD_STATUS_OK);
	BIND_ENUM_CONSTANT(LOAD_STATUS_FAILED);
	BIND_ENUM_CONSTANT(LOAD_STATUS_ALREADY_LOADED);
	BIND_ENUM_CONSTANT(LOAD_STATUS_NOT_LOADED);
	BIND_ENUM_CONSTANT(LOAD_STATUS_NEEDS_RESTART);

	ADD_SIGNAL(MethodInfo("extensions_reloaded"));
	ADD_SIGNAL(MethodInfo("extension_loaded", PropertyInfo(Variant::OBJECT, "extension", PROPERTY_HINT_RESOURCE_TYPE, "GDExtension")));
	ADD_SIGNAL(MethodInfo("extension_unloading", PropertyInfo(Variant::OBJECT, "extension", PROPERTY_HINT_RESOURCE_TYPE, "GDExtension")));
}

GDExtensionManager::GDExtensionManager() {
	ERR_FAIL_COND(singleton != nullptr);
	singleton = this;

#ifndef DISABLE_DEPRECATED
	GDExtensionSpecialCompatHashes::initialize();
#endif
}

GDExtensionManager::~GDExtensionManager() {
	if (singleton == this) {
		singleton = nullptr;
	}
#ifndef DISABLE_DEPRECATED
	GDExtensionSpecialCompatHashes::finalize();
#endif
}
