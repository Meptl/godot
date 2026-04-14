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
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/object/message_queue.h"
#include "core/os/mutex.h"
#include "core/os/semaphore.h"
#include "core/os/thread.h"
#include "core/string/print_string.h"
#include "core/version.h"
#include "editor/editor_log.h"
#include "editor/editor_main_screen.h"
#include "editor/editor_node.h"
#include "editor/file_system/editor_paths.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "editor/scene/canvas_item_editor_plugin.h"
#include "editor/settings/editor_settings.h"
#include "modules/modules_enabled.gen.h"
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

static Mutex _screenshot_capture_mutex;
static Semaphore _screenshot_capture_semaphore;
static String _screenshot_capture_requested_viewport;
static String _screenshot_capture_resolved_viewport;
static String _screenshot_capture_error;
static Ref<Image> _screenshot_capture_image;

static Ref<Image> _capture_2d_editor_viewport_image() {
	CanvasItemEditor *canvas_item_editor = CanvasItemEditor::get_singleton();
	if (canvas_item_editor == nullptr) {
		return Ref<Image>();
	}

	Control *viewport_control = canvas_item_editor->get_viewport_control();
	if (viewport_control == nullptr || !viewport_control->is_visible_in_tree()) {
		return Ref<Image>();
	}

	Viewport *viewport = viewport_control->get_viewport();
	if (viewport == nullptr) {
		return Ref<Image>();
	}

	Ref<ViewportTexture> texture = viewport->get_texture();
	if (texture.is_null()) {
		return Ref<Image>();
	}

	Ref<Image> image = texture->get_image();
	if (image.is_null() || image->is_empty()) {
		return Ref<Image>();
	}

	const Rect2 global_rect = viewport_control->get_global_rect();
	Rect2i crop_rect = Rect2i(global_rect);
	crop_rect = crop_rect.intersection(Rect2i(Point2i(), image->get_size()));
	if (crop_rect.size.x <= 0 || crop_rect.size.y <= 0) {
		return Ref<Image>();
	}

	return image->get_region(crop_rect);
}

static Ref<Image> _capture_3d_editor_viewport_image() {
	Node3DEditor *node_3d_editor = Node3DEditor::get_singleton();
	if (node_3d_editor == nullptr) {
		return Ref<Image>();
	}

	Node3DEditorViewport *editor_viewport = node_3d_editor->get_last_used_viewport();
	if (editor_viewport == nullptr) {
		editor_viewport = node_3d_editor->get_editor_viewport(0);
	}
	if (editor_viewport == nullptr) {
		return Ref<Image>();
	}

	SubViewport *viewport = editor_viewport->get_viewport_node();
	if (viewport == nullptr) {
		return Ref<Image>();
	}

	Ref<ViewportTexture> texture = viewport->get_texture();
	if (texture.is_null()) {
		return Ref<Image>();
	}

	Ref<Image> image = texture->get_image();
	if (image.is_null() || image->is_empty()) {
		return Ref<Image>();
	}

	return image;
}

static Ref<Image> _capture_editor_viewport_image_main_thread(const String &p_viewport_target, String &r_resolved_viewport_target, String &r_error) {
	ERR_FAIL_COND_V_MSG(!Thread::is_main_thread(), Ref<Image>(), "Editor viewport capture must run on the main thread.");

	String resolved_viewport_target = p_viewport_target;
	if (resolved_viewport_target == "current") {
		EditorMainScreen *main_screen = EditorNode::get_editor_main_screen();
		if (main_screen == nullptr) {
			r_error = "Editor main screen is not available";
			return Ref<Image>();
		}

		const int selected_screen = main_screen->get_selected_index();
		if (selected_screen == EditorMainScreen::EDITOR_2D) {
			resolved_viewport_target = "2d";
		} else if (selected_screen == EditorMainScreen::EDITOR_3D) {
			resolved_viewport_target = "3d";
		} else {
			r_error = "Current editor screen is neither 2D nor 3D. Specify viewport as '2d' or '3d'.";
			return Ref<Image>();
		}
	}

	Ref<Image> image;
	if (resolved_viewport_target == "2d") {
		image = _capture_2d_editor_viewport_image();
	} else if (resolved_viewport_target == "3d") {
		image = _capture_3d_editor_viewport_image();
	} else {
		r_error = "Parameter 'viewport' must be one of: '2d', '3d', 'current'";
		return Ref<Image>();
	}

	if (image.is_null() || image->is_empty()) {
		r_error = vformat("Failed to capture %s editor viewport image.", resolved_viewport_target);
		return Ref<Image>();
	}

	r_resolved_viewport_target = resolved_viewport_target;
	return image;
}

static void _capture_editor_viewport_image_deferred() {
	_screenshot_capture_error = String();
	_screenshot_capture_resolved_viewport = String();
	_screenshot_capture_image.unref();

	_screenshot_capture_image = _capture_editor_viewport_image_main_thread(
			_screenshot_capture_requested_viewport,
			_screenshot_capture_resolved_viewport,
			_screenshot_capture_error);
	_screenshot_capture_semaphore.post();
}

static Ref<Image> _capture_editor_viewport_image_thread_safe(const String &p_viewport_target, String &r_resolved_viewport_target, String &r_error) {
	if (Thread::is_main_thread()) {
		return _capture_editor_viewport_image_main_thread(p_viewport_target, r_resolved_viewport_target, r_error);
	}

	MutexLock lock(_screenshot_capture_mutex);
	_screenshot_capture_requested_viewport = p_viewport_target;

	CallQueue *main_message_queue = MessageQueue::get_main_singleton();
	if (main_message_queue == nullptr) {
		r_error = "Main message queue is unavailable";
		return Ref<Image>();
	}

	main_message_queue->push_callable(callable_mp_static(&_capture_editor_viewport_image_deferred));
	_screenshot_capture_semaphore.wait();

	r_error = _screenshot_capture_error;
	r_resolved_viewport_target = _screenshot_capture_resolved_viewport;
	return _screenshot_capture_image;
}

static mcp::json _screenshot_viewport_tool_handler(const mcp::json &p_params, const std::string &) {
	if (!p_params.contains("path") || !p_params["path"].is_string()) {
		throw std::runtime_error("Missing required string parameter: path");
	}

	String viewport_target = "current";
	if (p_params.contains("viewport")) {
		if (!p_params["viewport"].is_string()) {
			throw std::runtime_error("Parameter 'viewport' must be a string");
		}
		viewport_target = String::utf8(p_params["viewport"].get<std::string>().c_str()).strip_edges().to_lower();
	}

	String screenshot_path = String::utf8(p_params["path"].get<std::string>().c_str()).strip_edges();
	if (screenshot_path.is_empty()) {
		throw std::runtime_error("Parameter 'path' must not be empty");
	}
	if (!screenshot_path.is_absolute_path()) {
		throw std::runtime_error("Parameter 'path' must be an absolute filesystem path");
	}
	if (screenshot_path.get_extension().to_lower() != "png") {
		throw std::runtime_error("Only PNG output is supported. Path must end with .png");
	}

	String resolved_viewport_target;
	String capture_error;
	Ref<Image> image = _capture_editor_viewport_image_thread_safe(viewport_target, resolved_viewport_target, capture_error);
	if (image.is_null() || image->is_empty()) {
		if (capture_error.is_empty()) {
			capture_error = "Unknown editor viewport capture error";
		}
		throw std::runtime_error(capture_error.utf8().get_data());
	}

	const Error save_error = image->save_png(screenshot_path);
	if (save_error != OK) {
		throw std::runtime_error(vformat("Cannot save screenshot to '%s' (error %d).", screenshot_path, save_error).utf8().get_data());
	}

	mcp::json result;
	result["ok"] = true;
	result["path"] = screenshot_path.utf8().get_data();
	result["viewport"] = resolved_viewport_target.utf8().get_data();
	result["width"] = image->get_width();
	result["height"] = image->get_height();

	return {
		{
			{"type", "text"},
			{"text", result.dump()},
		},
	};
}

#ifdef TESTS_ENABLED
Dictionary mcp_check_script_tool_handler_for_tests(const String &p_path, bool p_include_warnings) {
	Dictionary result;
	mcp::json params;
	params["path"] = p_path.utf8().get_data();
	params["include_warnings"] = p_include_warnings;

	try {
		const mcp::json response = _check_script_tool_handler(params, "");
		if (!response.is_array() || response.empty() || !response[0].contains("text") || !response[0]["text"].is_string()) {
			result["threw"] = true;
			result["error"] = "Unexpected MCP check_script response format";
			return result;
		}

		result["threw"] = false;
		result["json_text"] = String::utf8(response[0]["text"].get<std::string>().c_str());
		return result;
	} catch (const std::runtime_error &e) {
		result["threw"] = true;
		result["error"] = String::utf8(e.what());
		return result;
	}
}
#endif // TESTS_ENABLED

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

void MCPServerEditorPlugin::_save_discovered_port(int p_port) {
	const String mcp_port_dir = EditorPaths::get_singleton()->get_project_settings_dir();
	const String mcp_port_path = mcp_port_dir.path_join("mcp_port");

	if (p_port < 0) {
		if (FileAccess::exists(mcp_port_path)) {
			const Error err = DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(mcp_port_path));
			if (err != OK) {
				EditorNode::get_log()->add_message(vformat("Cannot remove MCP port file %s.", mcp_port_path), EditorLog::MSG_TYPE_WARNING);
			}
		}
		return;
	}

	const Error mkdir_err = DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(mcp_port_dir));
	if (mkdir_err != OK) {
		EditorNode::get_log()->add_message(vformat("Cannot create MCP port directory %s.", mcp_port_dir), EditorLog::MSG_TYPE_WARNING);
		return;
	}

	Ref<FileAccess> f = FileAccess::open(mcp_port_path, FileAccess::WRITE);
	if (f.is_null()) {
		EditorNode::get_log()->add_message(vformat("Cannot write MCP port file %s.", mcp_port_path), EditorLog::MSG_TYPE_WARNING);
		return;
	}
	f->store_string(itos(p_port));
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

	int bind_port = port;
	const int max_attempts = 5;
	for (int attempt = 1;; ++attempt) {
		mcp::server::configuration config;
		config.host = host.utf8().get_data();
		config.port = bind_port;
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

		mcp::tool screenshot_viewport_tool = mcp::tool_builder("screenshot_viewport")
				.with_description("Capture the editor 2D or 3D viewport and save it as a PNG file.")
				.with_string_param("path", "Absolute output path ending with .png.", true)
				.with_string_param("viewport", "Viewport target: '2d', '3d', or 'current' (default).", false)
				.build();
		server->register_tool(screenshot_viewport_tool, _screenshot_viewport_tool_handler);

		if (server->start(false)) {
			started = true;
			bound_port = bind_port;
			_save_discovered_port(bound_port);
			const String started_msg = vformat("--- MCP server started on %s:%d ---", host, bound_port);
			EditorNode::get_log()->add_message(started_msg, EditorLog::MSG_TYPE_EDITOR);
			print_line(started_msg);
			break;
		}

		delete server;
		server = nullptr;

		if (attempt >= max_attempts) {
			_save_discovered_port(-1);
			const String err_msg = vformat("Cannot listen on port %d, MCP server unavailable.", bind_port);
			EditorNode::get_log()->add_message(err_msg, EditorLog::MSG_TYPE_ERROR);
			print_line(err_msg);
			break;
		}

		bind_port++;
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
	bound_port = -1;
	_save_discovered_port(-1);
	EditorNode::get_log()->add_message("--- MCP server stopped ---", EditorLog::MSG_TYPE_EDITOR);
}
