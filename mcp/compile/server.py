import asyncio
import json
import os
import sys
from mcp.server.models import InitializationOptions
from mcp.server import NotificationOptions, Server
from mcp.server.stdio import stdio_server
import mcp.types as types

# --- Configuration ---
# Path to your compiled C++ binary
# Using abspath to ensure it's found regardless of where the server is started
COMPILER_BIN = os.path.abspath("./tools/mcp/compile/build/compiler.exe") 

# Renamed to efficient-compiler as requested
server = Server("efficient-compiler")

@server.list_tools()
async def handle_list_tools() -> list[types.Tool]:
    """List available tools."""
    return [
        types.Tool(
            name="get_code_errors",
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
    ]

@server.call_tool()
async def handle_call_tool(
    name: str, arguments: dict | None
) -> list[types.TextContent]:
    """Handle tool execution."""
    if name != "get_code_errors":
        raise ValueError(f"Unknown tool: {name}")

    if not os.path.exists(COMPILER_BIN):
        return [
            types.TextContent(
                type="text",
                text=f"Error: Binary not found at {COMPILER_BIN}. Please ensure it is compiled."
            )
        ]

    arguments = arguments or {}
    build_dir = arguments.get("build_dir", "build")
    invalidate_cache = arguments.get("invalidate_cache", False)

    if invalidate_cache:
        cache_path = os.path.join(build_dir, "mcp_cache.json")
        if os.path.exists(cache_path):
            os.remove(cache_path)

    # Construct arguments for the C++ binary
    # We now always force the --errors flag and removed the warnings option
    cmd = [COMPILER_BIN, build_dir, "--errors"]

    try:
        # Run the C++ binary using asyncio for non-blocking execution
        process = await asyncio.create_subprocess_exec(
            *cmd,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE
        )

        stdout, stderr = await process.communicate()
        
        if process.returncode != 0:
            error_msg = stderr.decode().strip()
            return [
                types.TextContent(
                    type="text",
                    text=f"Error running diagnostics tool (Exit {process.returncode}):\n{error_msg}"
                )
            ]

        # The C++ tool prints the clean JSON to stdout
        output_json = stdout.decode().strip()
        
        if not output_json:
            return [
                types.TextContent(
                    type="text",
                    text="The diagnostic tool returned an empty response."
                )
            ]

        # Verify it is valid JSON and return to LLM
        try:
            data = json.loads(output_json)
            return [
                types.TextContent(
                    type="text",
                    text=json.dumps(data, indent=2)
                )
            ]
        except json.JSONDecodeError:
            return [
                types.TextContent(
                    type="text",
                    text=f"Failed to parse JSON output from tool. Raw output:\n{output_json}"
                )
            ]

    except Exception as e:
        return [
            types.TextContent(
                type="text",
                text=f"Critical server error: {str(e)}"
            )
        ]

async def main():
    # Run the server using stdin/stdout streams
    async with stdio_server() as (read_stream, write_stream):
        await server.run(
            read_stream,
            write_stream,
            InitializationOptions(
                server_name="efficient-compiler",
                server_version="1.0.0",
                capabilities=server.get_capabilities(
                    notification_options=NotificationOptions(),
                    experimental_capabilities={},
                ),
            ),
        )

if __name__ == "__main__":
    asyncio.run(main())