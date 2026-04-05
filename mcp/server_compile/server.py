import asyncio
import os
import subprocess
import requests
import mcp.types as types

webhook_base_url: str = ""

TOOL = types.Tool(
    name="server_compile",
    description=(
        "Collects git diff output for a project and its submodules, then sends the result to the configured webhook and returns the response. "
        "Before calling this tool: "
        "1) set project_path to the relative path from the workspace root to the main git repository root (e.g. '.' if the workspace root is the repo root); "
        "2) run `git submodule foreach --quiet --recursive 'echo $displaypath'` inside the repo to discover all submodules, "
        "then pass their paths as submodule_paths."
    ),
    inputSchema={
        "type": "object",
        "required": ["project_path"],
        "properties": {
            "project_path": {
                "type": "string",
                "description": "Relative path from the workspace root to the main git repository root.",
            },
            "submodule_paths": {
                "type": "array",
                "items": {"type": "string"},
                "description": "Relative paths of each submodule as returned by `git submodule foreach --quiet --recursive 'echo $displaypath'`.",
                "default": [],
            },
        },
    },
)

# CREATE_NO_WINDOW prevents git from inheriting the MCP stdio pipe handles on Windows
_SUBPROCESS_FLAGS = subprocess.CREATE_NO_WINDOW if hasattr(subprocess, "CREATE_NO_WINDOW") else 0


def _git(args: list[str], cwd: str | None = None) -> str:
    result = subprocess.run(
        ["git", *args],
        capture_output=True,
        text=True,
        stdin=subprocess.DEVNULL,
        creationflags=_SUBPROCESS_FLAGS,
        cwd=cwd,
    )
    if result.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout


def _resolve(repo_root: str, input_path: str) -> tuple[str, str]:
    """Returns (display_name, git_path) for a given input path."""
    repo_parent = os.path.dirname(repo_root)
    resolved = os.path.normpath(os.path.join(repo_root, input_path))
    display_name = os.path.relpath(resolved, repo_parent).replace(os.sep, "/")
    git_path = os.path.relpath(resolved, repo_root).replace(os.sep, "/")
    return display_name, git_path


def _run(project_path: str, submodule_paths: list[str]) -> str:
    repo_root = os.path.abspath(_git(["rev-parse", "--show-toplevel"]).strip())

    proj_name, proj_git_path = _resolve(repo_root, project_path)
    proj_diff = _git(["diff", "--", proj_git_path], cwd=repo_root)

    submodules = []
    for sub_path in submodule_paths:
        sub_name, sub_git_path = _resolve(repo_root, sub_path)
        sub_diff = _git(["diff", "--", sub_git_path], cwd=repo_root)
        submodules.append({"project": sub_name, "diff": sub_diff})

    if not proj_diff.strip() and not any(s["diff"].strip() for s in submodules):
        raise ValueError("NO_CHANGES")

    payload = {"project": proj_name, "diff": proj_diff, "submodules": submodules}
    url = webhook_base_url.rstrip("/") + "/webhook/diff-test"

    response = requests.post(
        url,
        json=payload,
        headers={"Content-Type": "application/json", "Connection": "close"},
    )
    if not (200 <= response.status_code < 300):
        raise RuntimeError(f"Webhook returned {response.status_code}: {response.text.strip()}")
    return response.text


async def handle(arguments: dict | None) -> list[types.TextContent]:
    arguments = arguments or {}
    project_path = arguments.get("project_path", "")
    submodule_paths = arguments.get("submodule_paths", [])

    if not project_path:
        return [types.TextContent(type="text", text="Error: 'project_path' is required.")]
    if not webhook_base_url:
        return [types.TextContent(type="text", text="Error: server was not started with a webhook_base_url argument.")]

    loop = asyncio.get_running_loop()
    try:
        result = await loop.run_in_executor(None, _run, project_path, submodule_paths)
        return [types.TextContent(type="text", text=result)]
    except ValueError as e:
        if str(e) == "NO_CHANGES":
            return [types.TextContent(type="text", text="No changes detected — all diffs are empty. Nothing was sent to the webhook.")]
        return [types.TextContent(type="text", text=f"Error: {str(e)}")]
    except RuntimeError as e:
        return [types.TextContent(type="text", text=f"Webhook error: {str(e)}")]
    except Exception as e:
        return [types.TextContent(type="text", text=f"Error: {type(e).__name__}: {str(e)}")]
