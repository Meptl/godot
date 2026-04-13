/**************************************************************************/
/*  mcp_server_editor_plugin.cpp                                          */
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

#include "mcp_server_editor_plugin.h"

#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "core/version.h"
#include "editor/editor_log.h"
#include "editor/editor_node.h"
#include "editor/settings/editor_settings.h"
#include "mcp_server.h"
#include "mcp_tool.h"
#ifdef MODULE_GDSCRIPT_ENABLED
#include "core/object/script_language.h"
#include "modules/gdscript/gdscript.h"
#endif

static mcp::json _echo_tool_handler(const mcp::json &p_params, const std::string &) {
	std::string echo = "pong";
	if (p_params.contains("message") && p_params["message"].is_string()) {
		echo = p_params["message"].get<std::string>();
	}

	return {
		{
			{"type", "text"},
			{"text", echo},
		},
	};
}

#ifdef MODULE_GDSCRIPT_ENABLED
static String _resolve_script_path(const String &p_path) {
	String path = p_path.strip_edges();
	if (path.is_empty()) {
		return path;
	}

	if (!path.begins_with("res://") && !path.begins_with("user://") && path.is_relative_path()) {
		path = "res://" + path;
	}

	const String localized = ProjectSettings::get_singleton()->localize_path(path);
	if (localized.begins_with("res://")) {
		path = localized;
	}

	return path;
}

static mcp::json _make_diagnostic(const String &p_path, const String &p_message, const String &p_source, int p_severity, int p_code, int p_line, int p_column, int p_end_line, int p_end_column) {
	mcp::json diagnostic;
	diagnostic["severity"] = p_severity;
	diagnostic["source"] = p_source.utf8().get_data();
	diagnostic["message"] = p_message.utf8().get_data();
	diagnostic["path"] = p_path.utf8().get_data();
	if (p_code >= 0) {
		diagnostic["code"] = p_code;
	}
	diagnostic["range"] = {
		{ "start", { { "line", p_line }, { "character", p_column } } },
		{ "end", { { "line", p_end_line }, { "character", p_end_column } } },
	};
	return diagnostic;
}
#endif

static mcp::json _check_script_tool_handler(const mcp::json &p_params, const std::string &) {
	if (!p_params.contains("path") || !p_params["path"].is_string()) {
		throw std::runtime_error("Missing required string parameter: path");
	}

#ifndef MODULE_GDSCRIPT_ENABLED
	return {
		{
			{"type", "text"},
			{"text", R"({"ok":false,"error":"GDScript module is disabled in this build."})"},
		},
	};
#else
	bool include_warnings = true;
	if (p_params.contains("include_warnings") && p_params["include_warnings"].is_boolean()) {
		include_warnings = p_params["include_warnings"].get<bool>();
	}

	const String input_path = String::utf8(p_params["path"].get<std::string>().c_str());
	const String script_path = _resolve_script_path(input_path);
	if (script_path.is_empty()) {
		throw std::runtime_error("Parameter 'path' must not be empty");
	}

	Error read_err = OK;
	const String source = FileAccess::get_file_as_string(script_path, &read_err);
	if (read_err != OK) {
		throw std::runtime_error(vformat("Failed to read script '%s' (error %d).", script_path, read_err).utf8().get_data());
	}

	const GDScriptLanguage *gdscript_language = GDScriptLanguage::get_singleton();
	if (gdscript_language == nullptr) {
		throw std::runtime_error("GDScript language singleton is not available");
	}

	List<ScriptLanguage::ScriptError> errors;
	List<ScriptLanguage::Warning> warnings;
	gdscript_language->validate(source, script_path, nullptr, &errors, &warnings, nullptr);

	const PackedStringArray source_lines = source.split("\n", false);
	auto get_line_end_column = [&source_lines](int p_line) -> int {
		if (p_line < 0 || p_line >= source_lines.size()) {
			return 0;
		}

		const String line_text = source_lines[p_line];
		return line_text.strip_edges(false).length();
	};

	mcp::json diagnostics = mcp::json::array();

	for (const ScriptLanguage::ScriptError &error : errors) {
		const String error_path = error.path.is_empty() ? script_path : error.path;
		const int line = MAX(0, error.line - 1);
		const int column = MAX(0, error.column - 1);
		diagnostics.push_back(_make_diagnostic(
				error_path,
				error.message,
				"gdscript",
				1,
				-1,
				line,
				column,
				line,
				MAX(column, get_line_end_column(line))));
	}

	if (include_warnings) {
		for (const ScriptLanguage::Warning &warning : warnings) {
			const int line = MAX(0, warning.start_line - 1);
			const int end_line = MAX(line, warning.end_line - 1);
			const int end_column = (line == end_line) ? get_line_end_column(line) : get_line_end_column(end_line);
			diagnostics.push_back(_make_diagnostic(
					script_path,
					vformat("(%s): %s", warning.string_code, warning.message),
					"gdscript",
					2,
					warning.code,
					line,
					0,
					end_line,
					end_column));
		}
	}

	mcp::json result;
	result["ok"] = true;
	result["path"] = script_path.utf8().get_data();
	result["diagnostics"] = diagnostics;
	result["error_count"] = errors.size();
	result["warning_count"] = include_warnings ? warnings.size() : 0;

	return {
		{
			{"type", "text"},
			{"text", result.dump()},
		},
	};
#endif
}

MCPServerEditorPlugin::MCPServerEditorPlugin() {
	_EDITOR_DEF("network/mcp_server/enable", enabled);
	_EDITOR_DEF("network/mcp_server/host", host);
	_EDITOR_DEF("network/mcp_server/port", port);

	set_process_internal(true);
}

void MCPServerEditorPlugin::_read_settings() {
	enabled = (bool)_EDITOR_GET("network/mcp_server/enable");
	host = String(_EDITOR_GET("network/mcp_server/host"));
	port = (int)_EDITOR_GET("network/mcp_server/port");
}

void MCPServerEditorPlugin::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_EXIT_TREE: {
			stop();
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			if (!start_attempted && EditorNode::get_singleton()->is_editor_ready()) {
				start_attempted = true;
				start();
			}
		} break;
		case EditorSettings::NOTIFICATION_EDITOR_SETTINGS_CHANGED: {
			if (!EditorSettings::get_singleton()->check_changed_settings_in_group("network/mcp_server")) {
				break;
			}

			bool old_enabled = enabled;
			String old_host = host;
			int old_port = port;

			_read_settings();
			if (old_enabled != enabled || old_host != host || old_port != port) {
				stop();
				start();
			}
		} break;
	}
}

void MCPServerEditorPlugin::start() {
	if (started) {
		return;
	}

	_read_settings();
	if (!enabled) {
		return;
	}

	mcp::server::configuration config;
	config.host = host.utf8().get_data();
	config.port = port;
	config.name = "Godot MCP Server";
	config.version = VERSION_FULL_CONFIG;
	config.http_endpoint = "/";

	server = new mcp::server(config);
	server->set_server_info(config.name, config.version);
	server->set_capabilities({
			{"tools", mcp::json::object()},
	});

	mcp::tool echo_tool = mcp::tool_builder("echo")
			.with_description("Echoes the provided message.")
			.with_string_param("message", "Message to echo back.", false)
			.build();
	server->register_tool(echo_tool, _echo_tool_handler);

	mcp::tool check_script_tool = mcp::tool_builder("check_script")
			.with_description("Validates a GDScript file and returns diagnostics similar to editor/LSP output.")
			.with_string_param("path", "Path to the script file. Supports res:// and filesystem paths.", true)
			.with_boolean_param("include_warnings", "Include parser/analyzer warnings in the output.", false)
			.build();
	server->register_tool(check_script_tool, _check_script_tool_handler);

	if (server->start(false)) {
		started = true;
		EditorNode::get_log()->add_message(vformat("--- MCP server started on %s:%d ---", host, port), EditorLog::MSG_TYPE_EDITOR);
	} else {
		delete server;
		server = nullptr;
	}
}

void MCPServerEditorPlugin::stop() {
	if (!started) {
		return;
	}

	if (server) {
		server->stop();
	}
	delete server;
	server = nullptr;
	started = false;
	EditorNode::get_log()->add_message("--- MCP server stopped ---", EditorLog::MSG_TYPE_EDITOR);
}
