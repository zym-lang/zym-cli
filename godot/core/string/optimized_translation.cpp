/**************************************************************************/
/*  optimized_translation.cpp                                             */
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

#include "optimized_translation.h"

#include "core/templates/pair.h"

extern "C" {
#include "thirdparty/misc/smaz.h"
}

struct CompressedString {
	int orig_len = 0;
	CharString compressed;
	int offset = 0;
};

void OptimizedTranslation::generate(const Ref<Translation> &p_from) {
	// This method compresses a Translation instance.
	// Right now, it doesn't handle context or plurals.
}

bool OptimizedTranslation::_set(const StringName &p_name, const Variant &p_value) {
	String prop_name = p_name.operator String();
	if (prop_name == "hash_table") {
		hash_table = p_value;
	} else if (prop_name == "bucket_table") {
		bucket_table = p_value;
	} else if (prop_name == "strings") {
		strings = p_value;
	} else if (prop_name == "load_from") {
		generate(p_value);
	} else {
		return false;
	}

	return true;
}

bool OptimizedTranslation::_get(const StringName &p_name, Variant &r_ret) const {
	String prop_name = p_name.operator String();
	if (prop_name == "hash_table") {
		r_ret = hash_table;
	} else if (prop_name == "bucket_table") {
		r_ret = bucket_table;
	} else if (prop_name == "strings") {
		r_ret = strings;
	} else {
		return false;
	}

	return true;
}

StringName OptimizedTranslation::get_message(const StringName &p_src_text, const StringName &p_context) const {
	// p_context passed in is ignore. The use of context is not yet supported in OptimizedTranslation.

	int htsize = hash_table.size();

	if (htsize == 0) {
		return StringName();
	}

	CharString str = p_src_text.operator String().utf8();
	uint32_t h = hash(0, str.get_data());

	const int *htr = hash_table.ptr();
	const uint32_t *htptr = (const uint32_t *)&htr[0];
	const int *btr = bucket_table.ptr();
	const uint32_t *btptr = (const uint32_t *)&btr[0];
	const uint8_t *sr = strings.ptr();
	const char *sptr = (const char *)&sr[0];

	uint32_t p = htptr[h % htsize];

	if (p == 0xFFFFFFFF) {
		return StringName(); //nothing
	}

	const Bucket &bucket = *(const Bucket *)&btptr[p];

	h = hash(bucket.func, str.get_data());

	int idx = -1;

	for (int i = 0; i < bucket.size; i++) {
		if (bucket.elem[i].key == h) {
			idx = i;
			break;
		}
	}

	if (idx == -1) {
		return StringName();
	}

	if (bucket.elem[idx].comp_size == bucket.elem[idx].uncomp_size) {
		return String::utf8(&sptr[bucket.elem[idx].str_offset], bucket.elem[idx].uncomp_size);
	} else {
		CharString uncomp;
		uncomp.resize_uninitialized(bucket.elem[idx].uncomp_size + 1);
		smaz_decompress(&sptr[bucket.elem[idx].str_offset], bucket.elem[idx].comp_size, uncomp.ptrw(), bucket.elem[idx].uncomp_size);
		return String::utf8(uncomp.get_data());
	}
}

Vector<String> OptimizedTranslation::get_translated_message_list() const {
	Vector<String> msgs;

	const int *htr = hash_table.ptr();
	const uint32_t *htptr = (const uint32_t *)&htr[0];
	const int *btr = bucket_table.ptr();
	const uint32_t *btptr = (const uint32_t *)&btr[0];
	const uint8_t *sr = strings.ptr();
	const char *sptr = (const char *)&sr[0];

	for (int i = 0; i < hash_table.size(); i++) {
		uint32_t p = htptr[i];
		if (p != 0xFFFFFFFF) {
			const Bucket &bucket = *(const Bucket *)&btptr[p];
			for (int j = 0; j < bucket.size; j++) {
				if (bucket.elem[j].comp_size == bucket.elem[j].uncomp_size) {
					String rstr = String::utf8(&sptr[bucket.elem[j].str_offset], bucket.elem[j].uncomp_size);
					msgs.push_back(rstr);
				} else {
					CharString uncomp;
					uncomp.resize_uninitialized(bucket.elem[j].uncomp_size + 1);
					smaz_decompress(&sptr[bucket.elem[j].str_offset], bucket.elem[j].comp_size, uncomp.ptrw(), bucket.elem[j].uncomp_size);
					String rstr = String::utf8(uncomp.get_data());
					msgs.push_back(rstr);
				}
			}
		}
	}
	return msgs;
}

StringName OptimizedTranslation::get_plural_message(const StringName &p_src_text, const StringName &p_plural_text, int p_n, const StringName &p_context) const {
	// The use of plurals translation is not yet supported in OptimizedTranslation.
	return get_message(p_src_text, p_context);
}

Vector<String> OptimizedTranslation::_get_message_list() const {
	WARN_PRINT_ONCE("OptimizedTranslation does not store the message texts to be translated.");
	return {};
}

void OptimizedTranslation::get_message_list(List<StringName> *r_messages) const {
	WARN_PRINT_ONCE("OptimizedTranslation does not store the message texts to be translated.");
}

int OptimizedTranslation::get_message_count() const {
	WARN_PRINT_ONCE("OptimizedTranslation does not store the message texts to be translated.");
	return 0;
}

void OptimizedTranslation::_get_property_list(List<PropertyInfo> *p_list) const {
	p_list->push_back(PropertyInfo(Variant::PACKED_INT32_ARRAY, "hash_table", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR));
	p_list->push_back(PropertyInfo(Variant::PACKED_INT32_ARRAY, "bucket_table", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR));
	p_list->push_back(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "strings", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR));
	p_list->push_back(PropertyInfo(Variant::OBJECT, "load_from", PROPERTY_HINT_RESOURCE_TYPE, "Translation", PROPERTY_USAGE_EDITOR));
}

void OptimizedTranslation::_bind_methods() {
	ClassDB::bind_method(D_METHOD("generate", "from"), &OptimizedTranslation::generate);
}
