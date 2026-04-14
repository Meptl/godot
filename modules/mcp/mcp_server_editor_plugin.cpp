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
#include "core/os/os.h"
#include "core/os/semaphore.h"
#include "core/os/thread.h"
#include "core/string/print_string.h"
#include "core/version.h"
#include "editor/editor_log.h"
#include "editor/editor_main_screen.h"
#include "editor/editor_node.h"
#include "editor/debugger/script_editor_debugger.h"
#include "editor/file_system/editor_paths.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "editor/scene/canvas_item_editor_plugin.h"
#include "editor/settings/editor_settings.h"
#include "editor/debugger/editor_debugger_node.h"
#include "editor/run/editor_run_bar.h"
#include "modules/modules_enabled.gen.h"
#include "mcp_server.h"
#include "mcp_tool.h"
#include "scene/debugger/scene_debugger.h"
#include "servers/display/display_server.h"
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

static mcp::json _make_text_result(const mcp::json &p_result) {
	return {
		{
			{"type", "text"},
			{"text", p_result.dump()},
		},
	};
}

static mcp::json _serialize_remote_scene_tree(const SceneDebuggerTree *p_tree) {
	mcp::json nodes = mcp::json::array();
	if (p_tree == nullptr) {
		return nodes;
	}

	Vector<int> remaining_children_stack;
	Vector<int> parent_index_stack;
	int node_index = 0;
	for (const SceneDebuggerTree::RemoteNode &node : p_tree->nodes) {
		while (!remaining_children_stack.is_empty() && remaining_children_stack[remaining_children_stack.size() - 1] == 0) {
			remaining_children_stack.resize(remaining_children_stack.size() - 1);
			parent_index_stack.resize(parent_index_stack.size() - 1);
		}

		const int parent_index = parent_index_stack.is_empty() ? -1 : parent_index_stack[parent_index_stack.size() - 1];
		const int depth = remaining_children_stack.size();
		if (!remaining_children_stack.is_empty()) {
			remaining_children_stack.write[remaining_children_stack.size() - 1] -= 1;
		}

		mcp::json node_json;
		node_json["index"] = node_index;
		node_json["parent_index"] = parent_index;
		node_json["depth"] = depth;
		node_json["child_count"] = node.child_count;
		node_json["name"] = node.name.utf8().get_data();
		node_json["type"] = node.type_name.utf8().get_data();
		node_json["object_id"] = (uint64_t)node.id;
		node_json["scene_file_path"] = node.scene_file_path.utf8().get_data();
		node_json["view"] = {
			{ "has_visible_method", (node.view_flags & SceneDebuggerTree::RemoteNode::VIEW_HAS_VISIBLE_METHOD) != 0 },
			{ "visible", (node.view_flags & SceneDebuggerTree::RemoteNode::VIEW_VISIBLE) != 0 },
			{ "visible_in_tree", (node.view_flags & SceneDebuggerTree::RemoteNode::VIEW_VISIBLE_IN_TREE) != 0 },
		};
		nodes.push_back(node_json);

		remaining_children_stack.push_back(node.child_count);
		parent_index_stack.push_back(node_index);
		node_index++;
	}

	return nodes;
}

static mcp::json _read_remote_scene_tree_main_thread(String &r_error) {
	ERR_FAIL_COND_V_MSG(!Thread::is_main_thread(), mcp::json::object(), "Remote scene tree read must run on the main thread.");

	EditorDebuggerNode *debugger_node = EditorDebuggerNode::get_singleton();
	if (debugger_node == nullptr) {
		r_error = "Editor debugger is unavailable";
		return mcp::json::object();
	}

	ScriptEditorDebugger *debugger = debugger_node->get_current_debugger();
	if (debugger == nullptr) {
		r_error = "No debugger session is available";
		return mcp::json::object();
	}
	if (!debugger->is_session_active()) {
		r_error = "No active remote debugger session";
		return mcp::json::object();
	}

	debugger->request_remote_tree();

	const SceneDebuggerTree *tree = debugger->get_remote_tree();

	mcp::json result;
	result["debugger_pid"] = (int64_t)debugger->get_remote_pid();
	result["node_count"] = tree != nullptr ? tree->nodes.size() : 0;
	result["nodes"] = _serialize_remote_scene_tree(tree);
	return result;
}

static Mutex _remote_scene_tree_capture_mutex;
static Semaphore _remote_scene_tree_capture_semaphore;
static String _remote_scene_tree_capture_error;
static mcp::json _remote_scene_tree_capture_result = mcp::json::object();

static void _capture_remote_scene_tree_deferred() {
	_remote_scene_tree_capture_error = String();
	_remote_scene_tree_capture_result = _read_remote_scene_tree_main_thread(_remote_scene_tree_capture_error);
	_remote_scene_tree_capture_semaphore.post();
}

static mcp::json _read_remote_scene_tree_thread_safe(String &r_error) {
	if (Thread::is_main_thread()) {
		return _read_remote_scene_tree_main_thread(r_error);
	}

	MutexLock lock(_remote_scene_tree_capture_mutex);

	CallQueue *main_message_queue = MessageQueue::get_main_singleton();
	if (main_message_queue == nullptr) {
		r_error = "Main message queue is unavailable";
		return mcp::json::object();
	}

	main_message_queue->push_callable(callable_mp_static(&_capture_remote_scene_tree_deferred));
	_remote_scene_tree_capture_semaphore.wait();

	r_error = _remote_scene_tree_capture_error;
	return _remote_scene_tree_capture_result;
}

static mcp::json _get_remote_scene_tree_tool_handler(const mcp::json &p_params, const std::string &) {
	(void)p_params;

	String error;
	const mcp::json tree_data = _read_remote_scene_tree_thread_safe(error);
	if (!error.is_empty()) {
		throw std::runtime_error(error.utf8().get_data());
	}

	mcp::json result;
	result["ok"] = true;
	result["tree"] = tree_data;
	return _make_text_result(result);
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

void MCPServerEditorPlugin::_perform_launch_main_scene(int64_t p_launch_id, int p_timeout_sec, int p_headless_mode) {
	LaunchRecord updated_record;
	updated_record.launch_id = p_launch_id;
	updated_record.launch_requested = true;
	updated_record.launch_completed = true;
	updated_record.timeout_sec = p_timeout_sec;

	EditorRunBar *run_bar = EditorRunBar::get_singleton();
	EditorDebuggerNode *debugger = EditorDebuggerNode::get_singleton();

	if (run_bar == nullptr || debugger == nullptr) {
		updated_record.launch_succeeded = false;
		updated_record.error = "Editor run/debug interfaces are unavailable.";
	} else {
		const bool launch_headless = (p_headless_mode < 0) ? (DisplayServer::get_singleton()->get_name() == "headless") : (p_headless_mode == 1);
		updated_record.launched_headless = launch_headless;

		Vector<String> launch_args;
		if (launch_headless) {
			launch_args.push_back("--headless");
		}
		run_bar->play_main_scene(false, launch_args);

		updated_record.launch_succeeded = run_bar->is_playing();
		updated_record.scene_path = run_bar->get_playing_scene();
		updated_record.process_id = (int64_t)run_bar->get_current_process();
		updated_record.debugger_uri = debugger->get_server_uri();
		updated_record.closed = !updated_record.launch_succeeded;
		if (updated_record.launch_succeeded && p_timeout_sec > 0) {
			updated_record.timeout_deadline_msec = OS::get_singleton()->get_ticks_msec() + ((int64_t)p_timeout_sec * 1000);
		}
		if (!updated_record.launch_succeeded) {
			updated_record.error = "Main scene did not start. Check editor output for details.";
		}
	}

	MutexLock lock(launch_records_mutex);
	if (updated_record.launch_succeeded && active_launch_id >= 0 && launch_records.has(active_launch_id) && !launch_records[active_launch_id].closed) {
		launch_records[active_launch_id].closed = true;
	}
	launch_records[p_launch_id] = updated_record;
	active_launch_id = updated_record.launch_succeeded ? p_launch_id : -1;
}

void MCPServerEditorPlugin::_perform_close_scene_launch(int64_t p_launch_id, bool p_timed_out) {
	EditorRunBar *run_bar = EditorRunBar::get_singleton();
	if (run_bar != nullptr && run_bar->is_playing()) {
		run_bar->stop_playing();
	}

	MutexLock lock(launch_records_mutex);
	const int64_t target_launch_id = p_launch_id >= 0 ? p_launch_id : active_launch_id;
	if (target_launch_id >= 0 && launch_records.has(target_launch_id)) {
		LaunchRecord &record = launch_records[target_launch_id];
		record.closed = true;
		record.closed_by_timeout = p_timed_out;
	}
	if (target_launch_id == active_launch_id) {
		active_launch_id = -1;
	}
}

void MCPServerEditorPlugin::_process_launch_timeouts() {
	EditorRunBar *run_bar = EditorRunBar::get_singleton();
	if (run_bar != nullptr && !run_bar->is_playing()) {
		MutexLock lock(launch_records_mutex);
		if (active_launch_id >= 0 && launch_records.has(active_launch_id) && !launch_records[active_launch_id].closed) {
			launch_records[active_launch_id].closed = true;
		}
		active_launch_id = -1;
		return;
	}

	int64_t launch_id_to_close = -1;
	{
		MutexLock lock(launch_records_mutex);
		if (active_launch_id < 0 || !launch_records.has(active_launch_id)) {
			return;
		}

		const LaunchRecord &record = launch_records[active_launch_id];
		if (!record.launch_succeeded || record.closed || record.timeout_sec <= 0 || record.timeout_deadline_msec < 0) {
			return;
		}

		if (OS::get_singleton()->get_ticks_msec() >= record.timeout_deadline_msec) {
			launch_id_to_close = active_launch_id;
		}
	}

	if (launch_id_to_close >= 0) {
		_perform_close_scene_launch(launch_id_to_close, true);
	}
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
			_process_launch_timeouts();
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

		mcp::tool get_remote_scene_tree_tool = mcp::tool_builder("get_remote_scene_tree")
				.with_description("Returns a snapshot of the active remote debugger scene tree. Always requests a refresh before reading.")
				.build();
		server->register_tool(get_remote_scene_tree_tool, _get_remote_scene_tree_tool_handler);

		mcp::tool launch_main_scene_tool = mcp::tool_builder("launch_main_scene")
				.with_description("Launches the project's main scene (equivalent to pressing F5 in the editor). Mirrors headless mode from the editor process.")
				.with_number_param("timeout_sec", "Auto-close timeout in seconds (default 15). Set to 0 to disable auto-close.", false)
				.with_boolean_param("headless", "Optional override for headless launch mode. If omitted, mirrors editor mode.", false)
				.build();
		server->register_tool(launch_main_scene_tool, [this](const mcp::json &p_params, const std::string &p_session_id) -> mcp::json {
			(void)p_session_id;

			int timeout_sec = 15;
			if (p_params.contains("timeout_sec")) {
				if (!p_params["timeout_sec"].is_number_integer()) {
					throw std::runtime_error("Parameter 'timeout_sec' must be an integer");
				}
				timeout_sec = p_params["timeout_sec"].get<int>();
			}
			if (timeout_sec < 0) {
				throw std::runtime_error("Parameter 'timeout_sec' must be >= 0");
			}

			int headless_mode = -1;
			if (p_params.contains("headless")) {
				if (!p_params["headless"].is_boolean()) {
					throw std::runtime_error("Parameter 'headless' must be a boolean");
				}
				headless_mode = p_params["headless"].get<bool>() ? 1 : 0;
			}

			int64_t launch_id = -1;
			{
				MutexLock lock(launch_records_mutex);
				launch_id = next_launch_id++;
				LaunchRecord record;
				record.launch_id = launch_id;
				record.launch_requested = true;
				record.timeout_sec = timeout_sec;
				launch_records[launch_id] = record;
			}

			callable_mp(this, &MCPServerEditorPlugin::_perform_launch_main_scene).call_deferred(launch_id, timeout_sec, headless_mode);

			mcp::json result;
			result["ok"] = true;
			result["launch_id"] = launch_id;
			result["status"] = "queued";
			result["timeout_sec"] = timeout_sec;
			if (headless_mode >= 0) {
				result["headless"] = headless_mode == 1;
			}
			result["message"] = "Main scene launch request queued.";
			return _make_text_result(result);
		});

		mcp::tool close_scene_launch_tool = mcp::tool_builder("close_scene_launch")
				.with_description("Stops the running launched scene.")
				.with_number_param("launch_id", "Optional launch identifier returned by launch_main_scene. Defaults to current active launch.", false)
				.build();
		server->register_tool(close_scene_launch_tool, [this](const mcp::json &p_params, const std::string &p_session_id) -> mcp::json {
			(void)p_session_id;

			int64_t launch_id = -1;
			if (p_params.contains("launch_id")) {
				if (!p_params["launch_id"].is_number_integer()) {
					throw std::runtime_error("Parameter 'launch_id' must be an integer");
				}
				launch_id = p_params["launch_id"].get<int64_t>();
			}

			callable_mp(this, &MCPServerEditorPlugin::_perform_close_scene_launch).call_deferred(launch_id, false);

			mcp::json result;
			result["ok"] = true;
			result["launch_id"] = launch_id;
			result["status"] = "queued";
			result["message"] = "Scene close request queued.";
			return _make_text_result(result);
		});

		mcp::tool get_scene_launch_info_tool = mcp::tool_builder("get_scene_launch_info")
				.with_description("Internal/debug tool. Returns launch bookkeeping and runtime metadata for a launch_id.")
				.with_number_param("launch_id", "Identifier returned by launch_main_scene.", true)
				.build();
		server->register_tool(get_scene_launch_info_tool, [this](const mcp::json &p_params, const std::string &p_session_id) -> mcp::json {
			(void)p_session_id;

			if (!p_params.contains("launch_id") || !p_params["launch_id"].is_number_integer()) {
				throw std::runtime_error("Missing required integer parameter: launch_id");
			}
			const int64_t launch_id = p_params["launch_id"].get<int64_t>();

			LaunchRecord record;
			{
				MutexLock lock(launch_records_mutex);
				if (!launch_records.has(launch_id)) {
					throw std::runtime_error(vformat("Unknown launch_id: %d", launch_id).utf8().get_data());
				}
				record = launch_records[launch_id];
			}

			const EditorRunBar *run_bar = EditorRunBar::get_singleton();
			const EditorDebuggerNode *debugger = EditorDebuggerNode::get_singleton();

			const bool is_playing = run_bar != nullptr ? run_bar->is_playing() : false;
			const int64_t current_process_id = run_bar != nullptr ? (int64_t)run_bar->get_current_process() : -1;
			const String current_scene = run_bar != nullptr ? run_bar->get_playing_scene() : String();
			const String current_debugger_uri = debugger != nullptr ? debugger->get_server_uri() : String();

			mcp::json result;
			result["ok"] = true;
			result["launch_id"] = launch_id;
			result["launch_requested"] = record.launch_requested;
			result["launch_completed"] = record.launch_completed;
			result["launch_succeeded"] = record.launch_succeeded;
			result["closed"] = record.closed;
			result["closed_by_timeout"] = record.closed_by_timeout;
			result["launched_headless"] = record.launched_headless;
			result["timeout_sec"] = record.timeout_sec;
			result["timeout_deadline_msec"] = record.timeout_deadline_msec;
			result["error"] = record.error.utf8().get_data();
			result["scene_path"] = record.scene_path.utf8().get_data();
			result["debugger_uri"] = record.debugger_uri.utf8().get_data();
			result["process_id"] = record.process_id;
			result["runtime"] = {
				{ "is_playing", is_playing },
				{ "current_process_id", current_process_id },
				{ "current_scene_path", current_scene.utf8().get_data() },
				{ "current_debugger_uri", current_debugger_uri.utf8().get_data() },
				{ "matches_recorded_process", (record.process_id >= 0 && record.process_id == current_process_id) },
			};
			return _make_text_result(result);
		});

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
