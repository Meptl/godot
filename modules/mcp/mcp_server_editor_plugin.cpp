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

#include "core/version.h"
#include "editor/editor_log.h"
#include "editor/editor_node.h"
#include "editor/settings/editor_settings.h"
#include "mcp_server.h"
#include "mcp_tool.h"

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
