/**************************************************************************/
/*  gdextension.cpp                                                       */
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

#include "gdextension.h"
#include "gdextension.compat.inc"

#include "core/config/project_settings.h"
#include "core/object/class_db.h"
#include "core/object/method_bind.h"
#include "gdextension_library_loader.h"
#include "gdextension_manager.h"

extern void gdextension_setup_interface();
extern GDExtensionInterfaceFunctionPtr gdextension_get_proc_address(const char *p_name);

typedef GDExtensionBool (*GDExtensionLegacyInitializationFunction)(void *p_interface, GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization);

String GDExtension::get_extension_list_config_file() {
	return ProjectSettings::get_singleton()->get_project_data_path().path_join("extension_list.cfg");
}

class GDExtensionMethodBind : public MethodBind {
	GDExtensionClassMethodCall call_func;
	GDExtensionClassMethodValidatedCall validated_call_func;
	GDExtensionClassMethodPtrCall ptrcall_func;
	void *method_userdata;
	bool vararg;
	uint32_t argument_count;
	PropertyInfo return_value_info;
	GodotTypeInfo::Metadata return_value_metadata;
	List<PropertyInfo> arguments_info;
	List<GodotTypeInfo::Metadata> arguments_metadata;


protected:
	virtual Variant::Type _gen_argument_type(int p_arg) const override {
		if (p_arg < 0) {
			return return_value_info.type;
		} else {
			return arguments_info.get(p_arg).type;
		}
	}
	virtual PropertyInfo _gen_argument_type_info(int p_arg) const override {
		if (p_arg < 0) {
			return return_value_info;
		} else {
			return arguments_info.get(p_arg);
		}
	}

public:


	virtual Variant call(Object *p_object, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) const override {
		Variant ret;
		GDExtensionClassInstancePtr extension_instance = is_static() ? nullptr : p_object->_get_extension_instance();
		GDExtensionCallError ce{ GDEXTENSION_CALL_OK, 0, 0 };
		call_func(method_userdata, extension_instance, reinterpret_cast<GDExtensionConstVariantPtr *>(p_args), p_arg_count, (GDExtensionVariantPtr)&ret, &ce);
		r_error.error = Callable::CallError::Error(ce.error);
		r_error.argument = ce.argument;
		r_error.expected = ce.expected;
		return ret;
	}
	virtual void validated_call(Object *p_object, const Variant **p_args, Variant *r_ret) const override {
		ERR_FAIL_COND_MSG(vararg, "Vararg methods don't have validated call support. This is most likely an engine bug.");
		GDExtensionClassInstancePtr extension_instance = is_static() ? nullptr : p_object->_get_extension_instance();

		if (validated_call_func) {
			// This is added here, but it's unlikely to be provided by most extensions.
			validated_call_func(method_userdata, extension_instance, reinterpret_cast<GDExtensionConstVariantPtr *>(p_args), (GDExtensionVariantPtr)r_ret);
		} else {
			// If not provided, go via ptrcall, which is faster than resorting to regular call.
			const void **argptrs = (const void **)alloca(argument_count * sizeof(void *));
			for (uint32_t i = 0; i < argument_count; i++) {
				argptrs[i] = VariantInternal::get_opaque_pointer(p_args[i]);
			}

			void *ret_opaque = nullptr;
			if (r_ret) {
				VariantInternal::initialize(r_ret, return_value_info.type);
				ret_opaque = r_ret->get_type() == Variant::NIL ? r_ret : VariantInternal::get_opaque_pointer(r_ret);
			}

			ptrcall_func(method_userdata, extension_instance, reinterpret_cast<GDExtensionConstTypePtr *>(argptrs), (GDExtensionTypePtr)ret_opaque);

			if (r_ret && r_ret->get_type() == Variant::OBJECT) {
				VariantInternal::update_object_id(r_ret);
			}
		}
	}

	virtual void ptrcall(Object *p_object, const void **p_args, void *r_ret) const override {
		ERR_FAIL_COND_MSG(vararg, "Vararg methods don't have ptrcall support. This is most likely an engine bug.");
		GDExtensionClassInstancePtr extension_instance = is_static() ? nullptr : p_object->_get_extension_instance();
		ptrcall_func(method_userdata, extension_instance, reinterpret_cast<GDExtensionConstTypePtr *>(p_args), (GDExtensionTypePtr)r_ret);
	}

	virtual bool is_vararg() const override {
		return vararg;
	}


	void update(const GDExtensionClassMethodInfo *p_method_info) {
		method_userdata = p_method_info->method_userdata;
		call_func = p_method_info->call_func;
		validated_call_func = nullptr;
		ptrcall_func = p_method_info->ptrcall_func;
		set_name(*reinterpret_cast<StringName *>(p_method_info->name));

		if (p_method_info->has_return_value) {
			return_value_info = PropertyInfo(*p_method_info->return_value_info);
			return_value_metadata = GodotTypeInfo::Metadata(p_method_info->return_value_metadata);
		}

		arguments_info.clear();
		arguments_metadata.clear();
		for (uint32_t i = 0; i < p_method_info->argument_count; i++) {
			arguments_info.push_back(PropertyInfo(p_method_info->arguments_info[i]));
			arguments_metadata.push_back(GodotTypeInfo::Metadata(p_method_info->arguments_metadata[i]));
		}

		set_hint_flags(p_method_info->method_flags);
		argument_count = p_method_info->argument_count;
		vararg = p_method_info->method_flags & GDEXTENSION_METHOD_FLAG_VARARG;
		_set_returns(p_method_info->has_return_value);
		_set_const(p_method_info->method_flags & GDEXTENSION_METHOD_FLAG_CONST);
		_set_static(p_method_info->method_flags & GDEXTENSION_METHOD_FLAG_STATIC);
		set_argument_count(p_method_info->argument_count);

		Vector<Variant> defargs;
		defargs.resize(p_method_info->default_argument_count);
		for (uint32_t i = 0; i < p_method_info->default_argument_count; i++) {
			defargs.write[i] = *static_cast<Variant *>(p_method_info->default_arguments[i]);
		}

		set_default_arguments(defargs);
	}

	explicit GDExtensionMethodBind(const GDExtensionClassMethodInfo *p_method_info) {
		update(p_method_info);
	}
};

#ifndef DISABLE_DEPRECATED
void GDExtension::_register_extension_class(GDExtensionClassLibraryPtr p_library, GDExtensionConstStringNamePtr p_class_name, GDExtensionConstStringNamePtr p_parent_class_name, const GDExtensionClassCreationInfo *p_extension_funcs) {
	const GDExtensionClassCreationInfo5 class_info5 = {
		p_extension_funcs->is_virtual, // GDExtensionBool is_virtual;
		p_extension_funcs->is_abstract, // GDExtensionBool is_abstract;
		true, // GDExtensionBool is_exposed;
		false, // GDExtensionBool is_runtime;
		nullptr, // GDExtensionConstStringPtr icon_path;
		p_extension_funcs->set_func, // GDExtensionClassSet set_func;
		p_extension_funcs->get_func, // GDExtensionClassGet get_func;
		p_extension_funcs->get_property_list_func, // GDExtensionClassGetPropertyList get_property_list_func;
		nullptr, // GDExtensionClassFreePropertyList2 free_property_list_func;
		p_extension_funcs->property_can_revert_func, // GDExtensionClassPropertyCanRevert property_can_revert_func;
		p_extension_funcs->property_get_revert_func, // GDExtensionClassPropertyGetRevert property_get_revert_func;
		nullptr, // GDExtensionClassValidateProperty validate_property_func;
		nullptr, // GDExtensionClassNotification2 notification_func;
		p_extension_funcs->to_string_func, // GDExtensionClassToString to_string_func;
		p_extension_funcs->reference_func, // GDExtensionClassReference reference_func;
		p_extension_funcs->unreference_func, // GDExtensionClassUnreference unreference_func;
		nullptr, // GDExtensionClassCreateInstance2 create_instance_func; /* this one is mandatory */
		p_extension_funcs->free_instance_func, // GDExtensionClassFreeInstance free_instance_func; /* this one is mandatory */
		nullptr, // GDExtensionClassRecreateInstance recreate_instance_func;
		nullptr, // GDExtensionClassGetVirtual get_virtual_func;
		nullptr, // GDExtensionClassGetVirtualCallData get_virtual_call_data_func;
		nullptr, // GDExtensionClassCallVirtualWithData call_virtual_func;
		p_extension_funcs->class_userdata, // void *class_userdata;
	};

	const ClassCreationDeprecatedInfo legacy = {
		false,
		p_extension_funcs->notification_func, // GDExtensionClassNotification notification_func;
		p_extension_funcs->free_property_list_func, // GDExtensionClassFreePropertyList free_property_list_func;
		p_extension_funcs->create_instance_func, // GDExtensionClassCreateInstance create_instance_func;
		p_extension_funcs->get_rid_func, // GDExtensionClassGetRID get_rid;
		p_extension_funcs->get_virtual_func, // GDExtensionClassGetVirtual get_virtual_func;
		nullptr,
	};
	_register_extension_class_internal(p_library, p_class_name, p_parent_class_name, &class_info5, &legacy);
}

void GDExtension::_register_extension_class2(GDExtensionClassLibraryPtr p_library, GDExtensionConstStringNamePtr p_class_name, GDExtensionConstStringNamePtr p_parent_class_name, const GDExtensionClassCreationInfo2 *p_extension_funcs) {
	const GDExtensionClassCreationInfo5 class_info5 = {
		p_extension_funcs->is_virtual, // GDExtensionBool is_virtual;
		p_extension_funcs->is_abstract, // GDExtensionBool is_abstract;
		p_extension_funcs->is_exposed, // GDExtensionBool is_exposed;
		false, // GDExtensionBool is_runtime;
		nullptr, // GDExtensionConstStringPtr icon_path;
		p_extension_funcs->set_func, // GDExtensionClassSet set_func;
		p_extension_funcs->get_func, // GDExtensionClassGet get_func;
		p_extension_funcs->get_property_list_func, // GDExtensionClassGetPropertyList get_property_list_func;
		nullptr, // GDExtensionClassFreePropertyList2 free_property_list_func;
		p_extension_funcs->property_can_revert_func, // GDExtensionClassPropertyCanRevert property_can_revert_func;
		p_extension_funcs->property_get_revert_func, // GDExtensionClassPropertyGetRevert property_get_revert_func;
		p_extension_funcs->validate_property_func, // GDExtensionClassValidateProperty validate_property_func;
		p_extension_funcs->notification_func, // GDExtensionClassNotification2 notification_func;
		p_extension_funcs->to_string_func, // GDExtensionClassToString to_string_func;
		p_extension_funcs->reference_func, // GDExtensionClassReference reference_func;
		p_extension_funcs->unreference_func, // GDExtensionClassUnreference unreference_func;
		nullptr, // GDExtensionClassCreateInstance2 create_instance_func; /* this one is mandatory */
		p_extension_funcs->free_instance_func, // GDExtensionClassFreeInstance free_instance_func; /* this one is mandatory */
		p_extension_funcs->recreate_instance_func, // GDExtensionClassRecreateInstance recreate_instance_func;
		nullptr, // GDExtensionClassGetVirtual get_virtual_func;
		nullptr, // GDExtensionClassGetVirtualCallData get_virtual_call_data_func;
		p_extension_funcs->call_virtual_with_data_func, // GDExtensionClassCallVirtualWithData call_virtual_func;
		p_extension_funcs->class_userdata, // void *class_userdata;
	};

	const ClassCreationDeprecatedInfo legacy = {
		!p_extension_funcs->is_exposed, // bool legacy_unexposed_class;
		nullptr, // GDExtensionClassNotification notification_func;
		p_extension_funcs->free_property_list_func, // GDExtensionClassFreePropertyList free_property_list_func;
		p_extension_funcs->create_instance_func, // GDExtensionClassCreateInstance create_instance_func;
		p_extension_funcs->get_rid_func, // GDExtensionClassGetRID get_rid;
		p_extension_funcs->get_virtual_func, // GDExtensionClassGetVirtual get_virtual_func;
		p_extension_funcs->get_virtual_call_data_func, // GDExtensionClassGetVirtual get_virtual_func;
	};
	_register_extension_class_internal(p_library, p_class_name, p_parent_class_name, &class_info5, &legacy);
}

void GDExtension::_register_extension_class3(GDExtensionClassLibraryPtr p_library, GDExtensionConstStringNamePtr p_class_name, GDExtensionConstStringNamePtr p_parent_class_name, const GDExtensionClassCreationInfo3 *p_extension_funcs) {
	const GDExtensionClassCreationInfo5 class_info5 = {
		p_extension_funcs->is_virtual, // GDExtensionBool is_virtual;
		p_extension_funcs->is_abstract, // GDExtensionBool is_abstract;
		p_extension_funcs->is_exposed, // GDExtensionBool is_exposed;
		p_extension_funcs->is_runtime, // GDExtensionBool is_runtime;
		nullptr, // GDExtensionConstStringPtr icon_path;
		p_extension_funcs->set_func, // GDExtensionClassSet set_func;
		p_extension_funcs->get_func, // GDExtensionClassGet get_func;
		p_extension_funcs->get_property_list_func, // GDExtensionClassGetPropertyList get_property_list_func;
		p_extension_funcs->free_property_list_func, // GDExtensionClassFreePropertyList free_property_list_func;
		p_extension_funcs->property_can_revert_func, // GDExtensionClassPropertyCanRevert property_can_revert_func;
		p_extension_funcs->property_get_revert_func, // GDExtensionClassPropertyGetRevert property_get_revert_func;
		p_extension_funcs->validate_property_func, // GDExtensionClassValidateProperty validate_property_func;
		p_extension_funcs->notification_func, // GDExtensionClassNotification2 notification_func;
		p_extension_funcs->to_string_func, // GDExtensionClassToString to_string_func;
		p_extension_funcs->reference_func, // GDExtensionClassReference reference_func;
		p_extension_funcs->unreference_func, // GDExtensionClassUnreference unreference_func;
		nullptr, // GDExtensionClassCreateInstance2 create_instance_func; /* this one is mandatory */
		p_extension_funcs->free_instance_func, // GDExtensionClassFreeInstance free_instance_func; /* this one is mandatory */
		p_extension_funcs->recreate_instance_func, // GDExtensionClassRecreateInstance recreate_instance_func;
		nullptr, // GDExtensionClassGetVirtual get_virtual_func;
		nullptr, // GDExtensionClassGetVirtualCallData get_virtual_call_data_func;
		p_extension_funcs->call_virtual_with_data_func, // GDExtensionClassCallVirtualWithData call_virtual_func;
		p_extension_funcs->class_userdata, // void *class_userdata;
	};

	const ClassCreationDeprecatedInfo legacy = {
		!p_extension_funcs->is_exposed, // bool legacy_unexposed_class;
		nullptr, // GDExtensionClassNotification notification_func;
		nullptr, // GDExtensionClassFreePropertyList free_property_list_func;
		p_extension_funcs->create_instance_func, // GDExtensionClassCreateInstance2 create_instance_func;
		p_extension_funcs->get_rid_func, // GDExtensionClassGetRID get_rid;
		p_extension_funcs->get_virtual_func, // GDExtensionClassGetVirtual get_virtual_func;
		p_extension_funcs->get_virtual_call_data_func, // GDExtensionClassGetVirtual get_virtual_func;
	};
	_register_extension_class_internal(p_library, p_class_name, p_parent_class_name, &class_info5, &legacy);
}

void GDExtension::_register_extension_class4(GDExtensionClassLibraryPtr p_library, GDExtensionConstStringNamePtr p_class_name, GDExtensionConstStringNamePtr p_parent_class_name, const GDExtensionClassCreationInfo4 *p_extension_funcs) {
	GDExtensionClassCreationInfo5 class_info5 = *p_extension_funcs;
	const ClassCreationDeprecatedInfo legacy = {
		!p_extension_funcs->is_exposed, // bool legacy_unexposed_class;
		nullptr, // GDExtensionClassNotification notification_func;
		nullptr, // GDExtensionClassFreePropertyList free_property_list_func;
		nullptr, // GDExtensionClassCreateInstance2 create_instance_func;
		nullptr, // GDExtensionClassGetRID get_rid;
		nullptr, // GDExtensionClassGetVirtual get_virtual_func;
		nullptr, // GDExtensionClassGetVirtual get_virtual_func;
	};
	_register_extension_class_internal(p_library, p_class_name, p_parent_class_name, &class_info5, &legacy);
}
#endif // DISABLE_DEPRECATED

void GDExtension::_register_extension_class5(GDExtensionClassLibraryPtr p_library, GDExtensionConstStringNamePtr p_class_name, GDExtensionConstStringNamePtr p_parent_class_name, const GDExtensionClassCreationInfo5 *p_extension_funcs) {
	_register_extension_class_internal(p_library, p_class_name, p_parent_class_name, p_extension_funcs);
}

void GDExtension::_register_extension_class_internal(GDExtensionClassLibraryPtr p_library, GDExtensionConstStringNamePtr p_class_name, GDExtensionConstStringNamePtr p_parent_class_name, const GDExtensionClassCreationInfo5 *p_extension_funcs, const ClassCreationDeprecatedInfo *p_deprecated_funcs) {
	GDExtension *self = reinterpret_cast<GDExtension *>(p_library);

	StringName class_name = *reinterpret_cast<const StringName *>(p_class_name);
	StringName parent_class_name = *reinterpret_cast<const StringName *>(p_parent_class_name);
	ERR_FAIL_COND_MSG(!String(class_name).is_valid_unicode_identifier(), vformat("Attempt to register extension class '%s', which is not a valid class identifier.", class_name));
	ERR_FAIL_COND_MSG(ClassDB::class_exists(class_name), vformat("Attempt to register extension class '%s', which appears to be already registered.", class_name));

	Extension *parent_extension = nullptr;

	if (self->extension_classes.has(parent_class_name)) {
		parent_extension = &self->extension_classes[parent_class_name];
	} else if (ClassDB::class_exists(parent_class_name)) {
		if (ClassDB::get_api_type(parent_class_name) == ClassDB::API_EXTENSION || ClassDB::get_api_type(parent_class_name) == ClassDB::API_EDITOR_EXTENSION) {
			ERR_PRINT("Unimplemented yet");
			//inheriting from another extension
		} else {
			//inheriting from engine class
		}
	} else {
		ERR_FAIL_MSG(vformat("Attempt to register an extension class '%s' using non-existing parent class '%s'.", String(class_name), String(parent_class_name)));
	}

	self->extension_classes[class_name] = Extension();
	Extension *extension = &self->extension_classes[class_name];

	if (parent_extension) {
		extension->gdextension.parent = &parent_extension->gdextension;
		parent_extension->gdextension.children.push_back(&extension->gdextension);
	}

	if (self->reloadable && p_extension_funcs->recreate_instance_func == nullptr) {
		bool can_create_class = (bool)p_extension_funcs->create_instance_func;
#ifndef DISABLE_DEPRECATED
		if (!can_create_class && p_deprecated_funcs) {
			can_create_class = (bool)p_deprecated_funcs->create_instance_func;
		}
#endif
		if (can_create_class) {
			ERR_PRINT(vformat("Extension marked as reloadable, but attempted to register class '%s' which doesn't support reloading. Perhaps your language binding don't support it? Reloading disabled for this extension.", class_name));
			self->reloadable = false;
		}
	}

	extension->gdextension.library = self;
	extension->gdextension.parent_class_name = parent_class_name;
	extension->gdextension.class_name = class_name;
	extension->gdextension.editor_class = self->level_initialized == INITIALIZATION_LEVEL_EDITOR;
	extension->gdextension.is_virtual = p_extension_funcs->is_virtual;
	extension->gdextension.is_abstract = p_extension_funcs->is_abstract;
	extension->gdextension.is_exposed = p_extension_funcs->is_exposed;
	extension->gdextension.set = p_extension_funcs->set_func;
	extension->gdextension.get = p_extension_funcs->get_func;
	extension->gdextension.get_property_list = p_extension_funcs->get_property_list_func;
	extension->gdextension.free_property_list2 = p_extension_funcs->free_property_list_func;
	extension->gdextension.property_can_revert = p_extension_funcs->property_can_revert_func;
	extension->gdextension.property_get_revert = p_extension_funcs->property_get_revert_func;
	extension->gdextension.validate_property = p_extension_funcs->validate_property_func;
#ifndef DISABLE_DEPRECATED
	if (p_deprecated_funcs) {
		extension->gdextension.legacy_unexposed_class = p_deprecated_funcs->legacy_unexposed_class;
		extension->gdextension.notification = p_deprecated_funcs->notification_func;
		extension->gdextension.free_property_list = p_deprecated_funcs->free_property_list_func;
		extension->gdextension.create_instance = p_deprecated_funcs->create_instance_func;
		extension->gdextension.get_rid = p_deprecated_funcs->get_rid_func;
		extension->gdextension.get_virtual = p_deprecated_funcs->get_virtual_func;
		extension->gdextension.get_virtual_call_data = p_deprecated_funcs->get_virtual_call_data_func;
	}
#endif // DISABLE_DEPRECATED
	extension->gdextension.notification2 = p_extension_funcs->notification_func;
	extension->gdextension.to_string = p_extension_funcs->to_string_func;
	extension->gdextension.reference = p_extension_funcs->reference_func;
	extension->gdextension.unreference = p_extension_funcs->unreference_func;
	extension->gdextension.class_userdata = p_extension_funcs->class_userdata;
	extension->gdextension.create_instance2 = p_extension_funcs->create_instance_func;
	extension->gdextension.free_instance = p_extension_funcs->free_instance_func;
	extension->gdextension.recreate_instance = p_extension_funcs->recreate_instance_func;
	extension->gdextension.get_virtual2 = p_extension_funcs->get_virtual_func;
	extension->gdextension.get_virtual_call_data2 = p_extension_funcs->get_virtual_call_data_func;
	extension->gdextension.call_virtual_with_data = p_extension_funcs->call_virtual_with_data_func;

	extension->gdextension.reloadable = self->reloadable;

	extension->gdextension.create_gdtype();

	ClassDB::register_extension_class(&extension->gdextension);

	if (p_extension_funcs->icon_path != nullptr) {
		const String icon_path = *reinterpret_cast<const String *>(p_extension_funcs->icon_path);
		if (!icon_path.is_empty()) {
			self->class_icon_paths[class_name] = icon_path;
		}
	}
}

void GDExtension::_register_extension_class_method(GDExtensionClassLibraryPtr p_library, GDExtensionConstStringNamePtr p_class_name, const GDExtensionClassMethodInfo *p_method_info) {
	GDExtension *self = reinterpret_cast<GDExtension *>(p_library);

	StringName class_name = *reinterpret_cast<const StringName *>(p_class_name);
	StringName method_name = *reinterpret_cast<const StringName *>(p_method_info->name);
	ERR_FAIL_COND_MSG(!self->extension_classes.has(class_name), vformat("Attempt to register extension method '%s' for unexisting class '%s'.", String(method_name), class_name));

	GDExtensionMethodBind *method = memnew(GDExtensionMethodBind(p_method_info));
	method->set_instance_class(class_name);

	ClassDB::bind_method_custom(class_name, method);
}

void GDExtension::_register_extension_class_virtual_method(GDExtensionClassLibraryPtr p_library, GDExtensionConstStringNamePtr p_class_name, const GDExtensionClassVirtualMethodInfo *p_method_info) {
	StringName class_name = *reinterpret_cast<const StringName *>(p_class_name);
	ClassDB::add_extension_class_virtual_method(class_name, p_method_info);
}

void GDExtension::_register_extension_class_integer_constant(GDExtensionClassLibraryPtr p_library, GDExtensionConstStringNamePtr p_class_name, GDExtensionConstStringNamePtr p_enum_name, GDExtensionConstStringNamePtr p_constant_name, GDExtensionInt p_constant_value, GDExtensionBool p_is_bitfield) {
	GDExtension *self = reinterpret_cast<GDExtension *>(p_library);

	StringName class_name = *reinterpret_cast<const StringName *>(p_class_name);
	StringName enum_name = *reinterpret_cast<const StringName *>(p_enum_name);
	StringName constant_name = *reinterpret_cast<const StringName *>(p_constant_name);
	ERR_FAIL_COND_MSG(!self->extension_classes.has(class_name), vformat("Attempt to register extension constant '%s' for unexisting class '%s'.", constant_name, class_name));


	ClassDB::bind_integer_constant(class_name, enum_name, constant_name, p_constant_value, p_is_bitfield);
}

void GDExtension::_register_extension_class_property(GDExtensionClassLibraryPtr p_library, GDExtensionConstStringNamePtr p_class_name, const GDExtensionPropertyInfo *p_info, GDExtensionConstStringNamePtr p_setter, GDExtensionConstStringNamePtr p_getter) {
	_register_extension_class_property_indexed(p_library, p_class_name, p_info, p_setter, p_getter, -1);
}

void GDExtension::_register_extension_class_property_indexed(GDExtensionClassLibraryPtr p_library, GDExtensionConstStringNamePtr p_class_name, const GDExtensionPropertyInfo *p_info, GDExtensionConstStringNamePtr p_setter, GDExtensionConstStringNamePtr p_getter, GDExtensionInt p_index) {
	GDExtension *self = reinterpret_cast<GDExtension *>(p_library);

	StringName class_name = *reinterpret_cast<const StringName *>(p_class_name);
	StringName setter = *reinterpret_cast<const StringName *>(p_setter);
	StringName getter = *reinterpret_cast<const StringName *>(p_getter);
	String property_name = *reinterpret_cast<const StringName *>(p_info->name);
	ERR_FAIL_COND_MSG(!self->extension_classes.has(class_name), vformat("Attempt to register extension class property '%s' for unexisting class '%s'.", property_name, class_name));


	PropertyInfo pinfo(*p_info);

	ClassDB::add_property(class_name, pinfo, setter, getter, p_index);
}

void GDExtension::_register_extension_class_property_group(GDExtensionClassLibraryPtr p_library, GDExtensionConstStringNamePtr p_class_name, GDExtensionConstStringPtr p_group_name, GDExtensionConstStringPtr p_prefix) {
	GDExtension *self = reinterpret_cast<GDExtension *>(p_library);

	StringName class_name = *reinterpret_cast<const StringName *>(p_class_name);
	String group_name = *reinterpret_cast<const String *>(p_group_name);
	String prefix = *reinterpret_cast<const String *>(p_prefix);
	ERR_FAIL_COND_MSG(!self->extension_classes.has(class_name), vformat("Attempt to register extension class property group '%s' for unexisting class '%s'.", group_name, class_name));


	ClassDB::add_property_group(class_name, group_name, prefix);
}

void GDExtension::_register_extension_class_property_subgroup(GDExtensionClassLibraryPtr p_library, GDExtensionConstStringNamePtr p_class_name, GDExtensionConstStringPtr p_subgroup_name, GDExtensionConstStringPtr p_prefix) {
	GDExtension *self = reinterpret_cast<GDExtension *>(p_library);

	StringName class_name = *reinterpret_cast<const StringName *>(p_class_name);
	String subgroup_name = *reinterpret_cast<const String *>(p_subgroup_name);
	String prefix = *reinterpret_cast<const String *>(p_prefix);
	ERR_FAIL_COND_MSG(!self->extension_classes.has(class_name), vformat("Attempt to register extension class property subgroup '%s' for unexisting class '%s'.", subgroup_name, class_name));


	ClassDB::add_property_subgroup(class_name, subgroup_name, prefix);
}

void GDExtension::_register_extension_class_signal(GDExtensionClassLibraryPtr p_library, GDExtensionConstStringNamePtr p_class_name, GDExtensionConstStringNamePtr p_signal_name, const GDExtensionPropertyInfo *p_argument_info, GDExtensionInt p_argument_count) {
	GDExtension *self = reinterpret_cast<GDExtension *>(p_library);

	StringName class_name = *reinterpret_cast<const StringName *>(p_class_name);
	StringName signal_name = *reinterpret_cast<const StringName *>(p_signal_name);
	ERR_FAIL_COND_MSG(!self->extension_classes.has(class_name), vformat("Attempt to register extension class signal '%s' for unexisting class '%s'.", signal_name, class_name));


	MethodInfo s;
	s.name = signal_name;
	for (int i = 0; i < p_argument_count; i++) {
		PropertyInfo arg(p_argument_info[i]);
		s.arguments.push_back(arg);
	}
	ClassDB::add_signal(class_name, s);
}

void GDExtension::_unregister_extension_class(GDExtensionClassLibraryPtr p_library, GDExtensionConstStringNamePtr p_class_name) {
	GDExtension *self = reinterpret_cast<GDExtension *>(p_library);

	StringName class_name = *reinterpret_cast<const StringName *>(p_class_name);
	ERR_FAIL_COND_MSG(!self->extension_classes.has(class_name), vformat("Attempt to unregister unexisting extension class '%s'.", class_name));

	Extension *ext = &self->extension_classes[class_name];
	ERR_FAIL_COND_MSG(ext->gdextension.children.size(), vformat("Attempt to unregister class '%s' while other extension classes inherit from it.", class_name));

	ClassDB::unregister_extension_class(class_name);

	if (ext->gdextension.parent != nullptr) {
		ext->gdextension.parent->children.erase(&ext->gdextension);
	}

	self->extension_classes.erase(class_name);
}

void GDExtension::_get_library_path(GDExtensionClassLibraryPtr p_library, GDExtensionUninitializedStringPtr r_path) {
	GDExtension *self = reinterpret_cast<GDExtension *>(p_library);

	Ref<GDExtensionLibraryLoader> library_loader = self->loader;
	String library_path;
	if (library_loader.is_valid()) {
		library_path = library_loader->library_path;
	}

	memnew_placement(r_path, String(library_path));
}

void GDExtension::_register_get_classes_used_callback(GDExtensionClassLibraryPtr p_library, GDExtensionEditorGetClassesUsedCallback p_callback) {
}

void GDExtension::_register_main_loop_callbacks(GDExtensionClassLibraryPtr p_library, const GDExtensionMainLoopCallbacks *p_callbacks) {
	GDExtension *self = reinterpret_cast<GDExtension *>(p_library);
	self->startup_callback = p_callbacks->startup_func;
	self->shutdown_callback = p_callbacks->shutdown_func;
	self->frame_callback = p_callbacks->frame_func;
}

void GDExtension::register_interface_function(const StringName &p_function_name, GDExtensionInterfaceFunctionPtr p_function_pointer) {
	ERR_FAIL_COND_MSG(gdextension_interface_functions.has(p_function_name), vformat("Attempt to register interface function '%s', which appears to be already registered.", p_function_name));
	gdextension_interface_functions.insert(p_function_name, p_function_pointer);
}

GDExtensionInterfaceFunctionPtr GDExtension::get_interface_function(const StringName &p_function_name) {
	GDExtensionInterfaceFunctionPtr *function = gdextension_interface_functions.getptr(p_function_name);
	ERR_FAIL_NULL_V_MSG(function, nullptr, vformat("Attempt to get non-existent interface function: '%s'.", String(p_function_name)));
	return *function;
}

Error GDExtension::open_library(const String &p_path, const Ref<GDExtensionLoader> &p_loader) {
	ERR_FAIL_COND_V_MSG(p_loader.is_null(), FAILED, "Can't open GDExtension without a loader.");
	loader = p_loader;

	Error err = loader->open_library(p_path);

	ERR_FAIL_COND_V_MSG(err == ERR_FILE_NOT_FOUND, err, vformat("GDExtension dynamic library not found: '%s'.", p_path));
	ERR_FAIL_COND_V_MSG(err != OK, err, vformat("Can't open GDExtension dynamic library: '%s'.", p_path));

	err = loader->initialize(&gdextension_get_proc_address, this, &initialization);

	if (err != OK) {
		// Errors already logged in initialize().
		loader->close_library();
		return err;
	}

	level_initialized = -1;

	return OK;
}

void GDExtension::close_library() {
	ERR_FAIL_COND(!is_library_open());
	loader->close_library();

	class_icon_paths.clear();

}

bool GDExtension::is_library_open() const {
	return loader.is_valid() && loader->is_library_open();
}

GDExtension::InitializationLevel GDExtension::get_minimum_library_initialization_level() const {
	ERR_FAIL_COND_V(!is_library_open(), INITIALIZATION_LEVEL_CORE);
	return InitializationLevel(initialization.minimum_initialization_level);
}

void GDExtension::initialize_library(InitializationLevel p_level) {
	ERR_FAIL_COND(!is_library_open());
	ERR_FAIL_COND_MSG(p_level <= int32_t(level_initialized), vformat("Level '%d' must be higher than the current level '%d'", p_level, level_initialized));

	level_initialized = int32_t(p_level);

	ERR_FAIL_NULL(initialization.initialize);

	initialization.initialize(initialization.userdata, GDExtensionInitializationLevel(p_level));
}
void GDExtension::deinitialize_library(InitializationLevel p_level) {
	ERR_FAIL_COND(!is_library_open());
	ERR_FAIL_COND(p_level > int32_t(level_initialized));

	level_initialized = int32_t(p_level) - 1;

	ERR_FAIL_NULL(initialization.deinitialize);

	initialization.deinitialize(initialization.userdata, GDExtensionInitializationLevel(p_level));
}

void GDExtension::_bind_methods() {
	ClassDB::bind_method(D_METHOD("is_library_open"), &GDExtension::is_library_open);
	ClassDB::bind_method(D_METHOD("get_minimum_library_initialization_level"), &GDExtension::get_minimum_library_initialization_level);

	BIND_ENUM_CONSTANT(INITIALIZATION_LEVEL_CORE);
	BIND_ENUM_CONSTANT(INITIALIZATION_LEVEL_SERVERS);
	BIND_ENUM_CONSTANT(INITIALIZATION_LEVEL_SCENE);
	BIND_ENUM_CONSTANT(INITIALIZATION_LEVEL_EDITOR);
}

GDExtension::~GDExtension() {
	if (is_library_open()) {
		close_library();
	}
}

void GDExtension::initialize_gdextensions() {
	gdextension_setup_interface();

#ifndef DISABLE_DEPRECATED
	register_interface_function("classdb_register_extension_class", (GDExtensionInterfaceFunctionPtr)&GDExtension::_register_extension_class);
	register_interface_function("classdb_register_extension_class2", (GDExtensionInterfaceFunctionPtr)&GDExtension::_register_extension_class2);
	register_interface_function("classdb_register_extension_class3", (GDExtensionInterfaceFunctionPtr)&GDExtension::_register_extension_class3);
	register_interface_function("classdb_register_extension_class4", (GDExtensionInterfaceFunctionPtr)&GDExtension::_register_extension_class4);
#endif // DISABLE_DEPRECATED
	register_interface_function("classdb_register_extension_class5", (GDExtensionInterfaceFunctionPtr)&GDExtension::_register_extension_class5);
	register_interface_function("classdb_register_extension_class_method", (GDExtensionInterfaceFunctionPtr)&GDExtension::_register_extension_class_method);
	register_interface_function("classdb_register_extension_class_virtual_method", (GDExtensionInterfaceFunctionPtr)&GDExtension::_register_extension_class_virtual_method);
	register_interface_function("classdb_register_extension_class_integer_constant", (GDExtensionInterfaceFunctionPtr)&GDExtension::_register_extension_class_integer_constant);
	register_interface_function("classdb_register_extension_class_property", (GDExtensionInterfaceFunctionPtr)&GDExtension::_register_extension_class_property);
	register_interface_function("classdb_register_extension_class_property_indexed", (GDExtensionInterfaceFunctionPtr)&GDExtension::_register_extension_class_property_indexed);
	register_interface_function("classdb_register_extension_class_property_group", (GDExtensionInterfaceFunctionPtr)&GDExtension::_register_extension_class_property_group);
	register_interface_function("classdb_register_extension_class_property_subgroup", (GDExtensionInterfaceFunctionPtr)&GDExtension::_register_extension_class_property_subgroup);
	register_interface_function("classdb_register_extension_class_signal", (GDExtensionInterfaceFunctionPtr)&GDExtension::_register_extension_class_signal);
	register_interface_function("classdb_unregister_extension_class", (GDExtensionInterfaceFunctionPtr)&GDExtension::_unregister_extension_class);
	register_interface_function("get_library_path", (GDExtensionInterfaceFunctionPtr)&GDExtension::_get_library_path);
	register_interface_function("editor_register_get_classes_used_callback", (GDExtensionInterfaceFunctionPtr)&GDExtension::_register_get_classes_used_callback);
	register_interface_function("register_main_loop_callbacks", (GDExtensionInterfaceFunctionPtr)&GDExtension::_register_main_loop_callbacks);
}

void GDExtension::finalize_gdextensions() {
	gdextension_interface_functions.clear();
}

Error GDExtensionResourceLoader::load_gdextension_resource(const String &p_path, Ref<GDExtension> &p_extension) {
	ERR_FAIL_COND_V_MSG(p_extension.is_valid() && p_extension->is_library_open(), ERR_ALREADY_IN_USE, "Cannot load GDExtension resource into already opened library.");

	GDExtensionManager *extension_manager = GDExtensionManager::get_singleton();

	GDExtensionManager::LoadStatus status = extension_manager->load_extension(p_path);
	if (status != GDExtensionManager::LOAD_STATUS_OK && status != GDExtensionManager::LOAD_STATUS_ALREADY_LOADED) {
		// Errors already logged in load_extension().
		return FAILED;
	}

	p_extension = extension_manager->get_extension(p_path);
	return OK;
}

Ref<Resource> GDExtensionResourceLoader::load(const String &p_path, const String &p_original_path, Error *r_error, bool p_use_sub_threads, float *r_progress, CacheMode p_cache_mode) {
	// We can't have two GDExtension resource object representing the same library, because
	// loading (or unloading) a GDExtension affects global data. So, we need reuse the same
	// object if one has already been loaded (even if caching is disabled at the resource
	// loader level).
	GDExtensionManager *manager = GDExtensionManager::get_singleton();
	if (manager->is_extension_loaded(p_path)) {
		return manager->get_extension(p_path);
	}

	Ref<GDExtension> lib;
	Error err = load_gdextension_resource(p_path, lib);
	if (err != OK && r_error) {
		// Errors already logged in load_gdextension_resource().
		*r_error = err;
	}
	return lib;
}

void GDExtensionResourceLoader::get_recognized_extensions(List<String> *p_extensions) const {
	p_extensions->push_back("gdextension");
}

bool GDExtensionResourceLoader::handles_type(const String &p_type) const {
	return p_type == "GDExtension";
}

String GDExtensionResourceLoader::get_resource_type(const String &p_path) const {
	if (p_path.has_extension("gdextension")) {
		return "GDExtension";
	}
	return "";
}

