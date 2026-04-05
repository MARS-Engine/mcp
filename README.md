# 🛠️ MCP Tools

MCP Tools for more efficient work.

This repository contains small MCP utilities that make development workflows faster and lighter for agents and editor integrations. The current focus is compact, useful output with less noise, especially for C++ diagnostics.

## ✨ Why This Repo Exists

- Reduce repetitive development work.
- Expose project-specific helpers through MCP.
- Return cleaner, smaller outputs that are easier for LLM tools to consume.

## 📦 MCP Tool Overview

All tools are exposed through a single MCP server at `mcp/server.py`, started via `.vscode/mcp.json`.

| Tool | Module | Purpose |
| --- | --- | --- |
| `local_compile` | `mcp/compile` | Runs cached C++ diagnostics from `compile_commands.json` and returns condensed JSON error output. |
| `server_compile` | `mcp/server_compile` | Collects `git diff` for the project and its submodules, POSTs the result to a configured webhook, and returns the webhook response. |

## 🔧 `local_compile`

| Field | Value |
| --- | --- |
| MCP tool | `local_compile` |
| Module | `mcp/compile` |
| Input | `build_dir` pointing to a directory with `compile_commands.json` |
| Output | JSON diagnostics, or `{ "status": "successful build" }` when no issues are found |
| Reported gains | Quick tests noted in `main.cpp` reported about `4k -> 700` tokens for 1 error and `37k -> 1.3k` tokens for 4 errors in Google AI Studio. |
| Notes | Launches a compiled C++ binary (`mcp/compile/build/compiler.exe`) that uses libclang for high-fidelity diagnostics with caching. |

## 🔧 `server_compile`

| Field | Value |
| --- | --- |
| MCP tool | `server_compile` |
| Module | `mcp/server_compile` |
| Input | `project_path` (relative path to repo root), `submodule_paths` (discovered via `git submodule foreach`) |
| Output | Raw response body returned by the webhook |
| Webhook URL | Configured as a CLI argument to `mcp/server.py` — the path `/webhook/diff-test` is appended automatically |
| Notes | The AI is instructed to run `git submodule foreach --quiet --recursive 'echo $displaypath'` before calling the tool to discover submodule paths automatically. |
