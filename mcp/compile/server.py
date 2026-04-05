import asyncio
import json
import os
import mcp.types as types

COMPILER_BIN = os.path.abspath("./tools/mcp/compile/build/compiler.exe")

TOOL = types.Tool(
    name="local_compile",
    description="Runs high-fidelity C++ compiler diagnostics to detect code errors, including template instantiation stacks and source snippets.",
    inputSchema={
        "type": "object",
        "properties": {
            "build_dir": {
                "type": "string",
                "description": "Path to the build directory containing compile_commands.json (default: 'build')",
                "default": "build"
            },
            "invalidate_cache": {
                "type": "boolean",
                "description": "Delete the timestamp cache before running, forcing a full re-parse. Use after git apply or when timestamps may be unreliable.",
                "default": False
            }
        },
    },
)


async def handle(arguments: dict | None) -> list[types.TextContent]:
    if not os.path.exists(COMPILER_BIN):
        return [types.TextContent(type="text", text=f"Error: Binary not found at {COMPILER_BIN}. Please ensure it is compiled.")]

    arguments = arguments or {}
    build_dir = arguments.get("build_dir", "build")
    invalidate_cache = arguments.get("invalidate_cache", False)

    if invalidate_cache:
        cache_path = os.path.join(build_dir, "mcp_cache.json")
        if os.path.exists(cache_path):
            os.remove(cache_path)

    cmd = [COMPILER_BIN, build_dir, "--errors"]

    try:
        process = await asyncio.create_subprocess_exec(
            *cmd,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE
        )
        stdout, stderr = await process.communicate()

        if process.returncode != 0:
            return [types.TextContent(type="text", text=f"Error running diagnostics tool (Exit {process.returncode}):\n{stderr.decode().strip()}")]

        output_json = stdout.decode().strip()
        if not output_json:
            return [types.TextContent(type="text", text="The diagnostic tool returned an empty response.")]

        try:
            data = json.loads(output_json)
            return [types.TextContent(type="text", text=json.dumps(data, indent=2))]
        except json.JSONDecodeError:
            return [types.TextContent(type="text", text=f"Failed to parse JSON output from tool. Raw output:\n{output_json}")]

    except Exception as e:
        return [types.TextContent(type="text", text=f"Critical error: {str(e)}")]
