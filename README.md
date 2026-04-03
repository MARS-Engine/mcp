# 🛠️ MCP Tools

MCP Tools for more efficient work.

This repository contains small MCP utilities that make development workflows faster and lighter for agents and editor integrations. The current focus is compact, useful output with less noise, especially for C++ diagnostics.

## ✨ Why This Repo Exists

- Reduce repetitive development work.
- Expose project-specific helpers through MCP.
- Return cleaner, smaller outputs that are easier for LLM tools to consume.

## 📦 MCP Tool Overview

| Tool | Path | Purpose |
| --- | --- | --- |
| `efficient-compiler` | `mcp/compile` | Runs cached C++ diagnostics from `compile_commands.json` and returns condensed JSON error output. |

## 🔧 `efficient-compiler`

| Field | Value |
| --- | --- |
| Server name | `efficient-compiler` |
| MCP tool | `get_code_errors` |
| Directory | `mcp/compile` |
| Input | `build_dir` pointing to a directory with `compile_commands.json` |
| Output | JSON diagnostics, or `{ "status": "successful build" }` when no issues are found |
| Reported gains | Quick tests noted in `main.cpp` reported about `4k -> 700` tokens for 1 error and `37k -> 1.3k` tokens for 4 errors in Google AI Studio. |
| Notes | The Python MCP server launches the compiled C++ binary, currently requests error-only diagnostics, and the token numbers above are approximate manual measurements. |
