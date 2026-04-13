# Development

## Compiling
Please use this command:
`scons platform=linuxbsd use_llvm=yes  cache_path="$HOME/.cache/godot-scons" c_compiler_launcher=ccache cpp_compiler_launcher=ccache &>/tmp/$(date +%s)-out`
Note that all output is sent to a file to reduce context bloat.
You can verify the exit code to see if it was successful.
Feel free to view the file but prefer grabbing snippets.
