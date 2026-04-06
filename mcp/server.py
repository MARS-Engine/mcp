import asyncio
import sys
import os

# Allow imports from sibling tool folders
sys.path.insert(0, os.path.dirname(__file__))

from mcp.server.models import InitializationOptions
from mcp.server import NotificationOptions, Server
from mcp.server.stdio import stdio_server
import mcp.types as types

import compile.server as compile_tool
import server_compile.server as server_compile_tool
import submodule_list.server as submodule_list_tool

# Inject webhook base URL from CLI arg, e.g.: python server.py http://<host>:<port>
if len(sys.argv) >= 2:
    server_compile_tool.webhook_base_url = sys.argv[1]

server = Server("render-visualizer-tools")

TOOLS = {
    compile_tool.TOOL.name: compile_tool,
    server_compile_tool.TOOL.name: server_compile_tool,
    submodule_list_tool.TOOL.name: submodule_list_tool,
}


@server.list_tools()
async def handle_list_tools() -> list[types.Tool]:
    return [tool.TOOL for tool in TOOLS.values()]


@server.call_tool()
async def handle_call_tool(name: str, arguments: dict | None) -> list[types.TextContent]:
    if name not in TOOLS:
        raise ValueError(f"Unknown tool: {name}")
    return await TOOLS[name].handle(arguments)


async def main():
    async with stdio_server() as (read_stream, write_stream):
        await server.run(
            read_stream,
            write_stream,
            InitializationOptions(
                server_name="render-visualizer-tools",
                server_version="1.0.0",
                capabilities=server.get_capabilities(
                    notification_options=NotificationOptions(),
                    experimental_capabilities={},
                ),
            ),
        )


if __name__ == "__main__":
    asyncio.run(main())
