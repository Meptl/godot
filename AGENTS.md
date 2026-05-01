# Development
We're developing modules/mcp which adds mcp capabilities to the Godot game engine.

## Compiling
Please use this command:
`scons platform=linuxbsd use_llvm=yes  cache_path="$HOME/.cache/godot-scons" c_compiler_launcher=ccache cpp_compiler_launcher=ccache &>/tmp/$(date +%s)-out`
Note that all output is sent to a file to reduce context bloat.
You can verify the exit code to see if it was successful.
Feel free to view the file but prefer grabbing snippets.

## Manual MCP HTTP testing (curl)
Use `curl` directly against the editor MCP server endpoint (`http://127.0.0.1:8927/`).

1. Start the editor (headless is fine for anything that doesn't need a "camera"):
`./bin/godot.linuxbsd.editor.x86_64.llvm --headless ~/balldrop/project.godot`

2. Initialize MCP and capture `Mcp-Session-Id`:
```bash
INIT_HEADERS=$(mktemp)
curl -sS -D "$INIT_HEADERS" -o /tmp/mcp_init.json \
  -H 'Content-Type: application/json' \
  -X POST http://127.0.0.1:8927/ \
  --data '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"manual-test","version":"1.0"}}}'

SESSION_ID=$(awk -F': ' 'tolower($1)=="mcp-session-id" {gsub("\r", "", $2); print $2}' "$INIT_HEADERS" | tail -n1)
echo "$SESSION_ID"
```

3. Send required initialized notification:
```bash
curl -sS \
  -H 'Content-Type: application/json' \
  -H "Mcp-Session-Id: $SESSION_ID" \
  -X POST http://127.0.0.1:8927/ \
  --data '{"jsonrpc":"2.0","method":"notifications/initialized","params":{}}'
```

4. List tools:
```bash
curl -sS \
  -H 'Content-Type: application/json' \
  -H "Mcp-Session-Id: $SESSION_ID" \
  -X POST http://127.0.0.1:8927/ \
  --data '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}' | jq .
```

5. Call tool in this example `launch_main_scene`:
```bash
LAUNCH_RESP=$(curl -sS \
  -H 'Content-Type: application/json' \
  -H "Mcp-Session-Id: $SESSION_ID" \
  -X POST http://127.0.0.1:8927/ \
  --data '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"launch_main_scene","arguments":{}}}')

LAUNCH_TEXT=$(echo "$LAUNCH_RESP" | jq -r '.result.content[0].text')
LAUNCH_ID=$(echo "$LAUNCH_TEXT" | jq -r '.launch_id')
echo "$LAUNCH_TEXT"
```

Important:
- `notifications/initialized` is required after `initialize`. Without it, `tools/list` and `tools/call` will fail with `Session not initialized`.

## Unit test
bin/godot.linuxbsd.editor.x86_64.llvm --test --test-case="[MCP]*" --test-verbose
