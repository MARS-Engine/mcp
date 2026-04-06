import asyncio
import json
import os
import subprocess
import mcp.types as types

TOOL = types.Tool(
    name="submodule_list",
    description="Returns a JSON array of all submodule paths for a given git repository. Use this before calling server_compile to get the submodule_paths argument.",
    inputSchema={
        "type": "object",
        "required": ["project_path"],
        "properties": {
            "project_path": {
                "type": "string",
                "description": "Relative path from the workspace root to the git repository root.",
            },
        },
    },
)

_SUBPROCESS_FLAGS = subprocess.CREATE_NO_WINDOW if hasattr(subprocess, "CREATE_NO_WINDOW") else 0


def _run(project_path: str) -> str:
    result = subprocess.run(
        ["git", "submodule", "foreach", "--quiet", "--recursive", "echo $displaypath"],
        capture_output=True,
        text=True,
        stdin=subprocess.DEVNULL,
        creationflags=_SUBPROCESS_FLAGS,
        cwd=project_path,
    )
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip())
    paths = [p for p in result.stdout.splitlines() if p.strip()]
    return json.dumps(paths)


async def handle(arguments: dict | None) -> list[types.TextContent]:
    arguments = arguments or {}
    project_path = arguments.get("project_path", "")

    if not project_path:
        return [types.TextContent(type="text", text="Error: 'project_path' is required.")]

    loop = asyncio.get_running_loop()
    try:
        result = await loop.run_in_executor(None, _run, project_path)
        return [types.TextContent(type="text", text=result)]
    except RuntimeError as e:
        return [types.TextContent(type="text", text=f"Error: {str(e)}")]
    except Exception as e:
        return [types.TextContent(type="text", text=f"Error: {type(e).__name__}: {str(e)}")]
