/**************************************************************************/
/*  main.cpp                                                              */
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

// zym: scene/* has been completely removed from this fork. The original
// body of every Main:: entrypoint in this file was orchestrating scene
// tree / window / theme / packed-scene / main-loop wiring. With scene/*
// nuked, none of that code can live here. zym's runtime drives engine
// startup from src/godot_host.cpp directly (register_core_types(),
// register_driver_types(), ...), so the Main:: API surface declared in
// main.h is preserved here only so that any stray link references
// resolve. Bodies are intentionally inert.

#include "main.h"

#include "core/error/error_list.h"
#include "core/string/ustring.h"
#include "core/templates/vector.h"

uint64_t Main::last_ticks = 0;
uint32_t Main::hide_print_fps_attempts = 0;
uint32_t Main::frames = 0;
uint32_t Main::frame = 0;
bool Main::force_redraw_requested = false;
int Main::iterating = 0;

void Main::print_header(bool p_rich) {}
void Main::print_help_copyright(const char *p_notice) {}
void Main::print_help_title(const char *p_title) {}
void Main::print_help_option(const char *p_option, const char *p_description, CLIOptionAvailability p_availability) {}
String Main::format_help_option(const char *p_option) {
	return String();
}
void Main::print_help(const char *p_binary) {}

bool Main::is_cmdline_tool() {
	return false;
}


int Main::test_entrypoint(int argc, char *argv[], bool &tests_need_run) {
	tests_need_run = false;
	return 0;
}

Error Main::setup(const char *execpath, int argc, char *argv[], bool p_second_phase) {
	return OK;
}

Error Main::setup2(bool p_show_boot_logo) {
	return OK;
}

String Main::get_rendering_driver_name() {
	return String();
}

String Main::get_locale_override() {
	return String();
}

void Main::setup_boot_logo() {}


int Main::start() {
	return 0;
}

bool Main::iteration() {
	// Returning true means "exit" in Godot's main loop contract; we return
	// true so any accidental caller bails out immediately instead of
	// spinning a non-existent scene tree.
	return true;
}

void Main::force_redraw() {
	force_redraw_requested = true;
}

bool Main::is_iterating() {
	return iterating > 0;
}

void Main::cleanup(bool p_force) {}
