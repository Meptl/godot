/**************************************************************************/
/*  test_check_script.h                                                   */
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

#pragma once

#if defined(TOOLS_ENABLED) && !defined(MCP_NO_SERVER) && defined(MODULE_GDSCRIPT_ENABLED)

#include "tests/test_macros.h"

#include "core/config/project_settings.h"
#include "core/io/json.h"
#include "core/string/ustring.h"
#include "modules/gdscript/gdscript_warning.h"
Dictionary mcp_check_script_tool_handler_for_tests(const String &p_path, bool p_include_warnings);

namespace TestMCP {

static Dictionary run_check_script(const String &p_path, bool p_include_warnings) {
	const Dictionary call_result = mcp_check_script_tool_handler_for_tests(p_path, p_include_warnings);
	REQUIRE(call_result.has("threw"));
	REQUIRE(bool(call_result["threw"]) == false);
	const String json_text = call_result["json_text"];
	JSON json;
	REQUIRE(json.parse(json_text) == OK);
	REQUIRE(json.get_data().get_type() == Variant::DICTIONARY);
	const Dictionary result = json.get_data();
	REQUIRE(result.has("ok"));
	REQUIRE(bool(result["ok"]));
	REQUIRE(result.has("error_count"));
	REQUIRE(result.has("warning_count"));
	REQUIRE(result.has("diagnostics"));
	return result;
}

static void enable_all_gdscript_warnings_for_test() {
	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	project_settings->set_setting("debug/gdscript/warnings/enable", true);
	for (int i = 0; i < (int)GDScriptWarning::WARNING_MAX; i++) {
		const GDScriptWarning::Code code = (GDScriptWarning::Code)i;
		project_settings->set_setting(GDScriptWarning::get_setting_path_from_code(code), true);
	}
}

TEST_CASE("[MCP] check_script valid script has no diagnostics") {
	const Dictionary result = run_check_script("modules/mcp/tests/scripts/valid_script.gd", true);

	CHECK(bool(result["ok"]));
	CHECK(int(result["error_count"]) == 0);
	CHECK(int(result["warning_count"]) == 0);
	const Array diagnostics = result["diagnostics"];
	CHECK(diagnostics.size() == 0);
}

TEST_CASE("[MCP] check_script parse error includes range fields") {
	const Dictionary result = run_check_script("modules/mcp/tests/scripts/parse_error_script.gd", true);

	CHECK(bool(result["ok"]));
	CHECK(int(result["error_count"]) >= 1);
	const Array diagnostics = result["diagnostics"];
	REQUIRE(diagnostics.size() >= 1);

	const Dictionary diagnostic = diagnostics[0];
	CHECK(int(diagnostic["severity"]) == 1);
	CHECK(String(diagnostic["source"]) == "gdscript");
	const Dictionary range = diagnostic["range"];
	const Dictionary start = range["start"];
	const Dictionary end = range["end"];

	const int start_line = start["line"];
	const int start_character = start["character"];
	const int end_line = end["line"];
	const int end_character = end["character"];

	CHECK(start_line >= 0);
	CHECK(start_character >= 0);
	CHECK(end_line >= start_line);
	if (end_line == start_line) {
		CHECK(end_character >= start_character);
	}
}

TEST_CASE("[MCP] check_script include_warnings toggles warning diagnostics") {
	enable_all_gdscript_warnings_for_test();

	const Dictionary with_warnings = run_check_script("modules/mcp/tests/scripts/warning_script.gd", true);
	const Dictionary without_warnings = run_check_script("modules/mcp/tests/scripts/warning_script.gd", false);

	CHECK(bool(with_warnings["ok"]));
	CHECK(bool(without_warnings["ok"]));
	CHECK(int(with_warnings["error_count"]) == 0);
	CHECK(int(without_warnings["error_count"]) == 0);
	CHECK(int(without_warnings["warning_count"]) == 0);
	CHECK(int(with_warnings["warning_count"]) >= int(without_warnings["warning_count"]));

	const Array diagnostics_with_warnings = with_warnings["diagnostics"];
	const Array diagnostics_without_warnings = without_warnings["diagnostics"];
	CHECK(diagnostics_without_warnings.size() == 0);
	CHECK(diagnostics_with_warnings.size() >= diagnostics_without_warnings.size());
}

TEST_CASE("[MCP] check_script handles missing and invalid paths") {
	const Dictionary missing_path_result = mcp_check_script_tool_handler_for_tests("modules/mcp/tests/scripts/does_not_exist.gd", true);
	CHECK(bool(missing_path_result["threw"]));
	CHECK(String(missing_path_result["error"]).contains("Failed to read script"));

	const Dictionary empty_path_result = mcp_check_script_tool_handler_for_tests("", true);
	CHECK(bool(empty_path_result["threw"]));
	CHECK(String(empty_path_result["error"]).contains("must not be empty"));
}

} // namespace TestMCP

#endif // TOOLS_ENABLED && !MCP_NO_SERVER && MODULE_GDSCRIPT_ENABLED
