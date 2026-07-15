"""
gen_compile_commands.py — Generate compile_commands.json for clangd.

The gengin Makefile compiles all .c files in a single monolithic clang
invocation, so `bear -- make` would produce only one entry.  This generator
creates proper per-file entries by extracting include paths from the Makefile
and walking the source tree.

Usage:
    python gen_compile_commands.py <project_dir>

Output:
    <project_dir>/compile_commands.json
"""

import json
import os
import re
import sys


def _extract_includes(makefile_path: str) -> list[str]:
    """Extract unique -I flags from the Makefile.  Returns list of absolute paths."""
    includes: set[str] = set()
    project_dir = os.path.abspath(os.path.dirname(makefile_path))
    try:
        with open(makefile_path) as f:
            text = f.read()
        for m in re.finditer(r'-I\s*(\S+)', text):
            raw = m.group(1)
            if '$(' in raw or '${' in raw:
                continue
            if raw == '.':
                raw = project_dir
            elif not raw.startswith('/'):
                raw = os.path.join(project_dir, raw)
            includes.add(f"-I{raw}")
    except FileNotFoundError:
        pass
    includes.add(f"-I{project_dir}")
    return sorted(includes)


def _find_sources(project_dir: str) -> list[str]:
    """Walk project_dir for .c files, returning paths relative to project_dir.
    Skips deps/, bench/, tests/, and tools/ directories."""
    sources: list[str] = []
    skip_dirs = {'deps', 'bench', 'tests', 'tools', '.git', '__pycache__',
                 '.flamegraph', 'assets', 'img', 'codeSnippits', 'exploration'}
    for root, dirs, files in os.walk(project_dir):
        dirs[:] = [d for d in dirs if d not in skip_dirs]
        for f in files:
            if f.endswith('.c'):
                full = os.path.join(root, f)
                rel = os.path.relpath(full, project_dir)
                sources.append(rel)
    return sorted(sources)


def generate(project_dir: str) -> str:
    """Generate compile_commands.json in project_dir.  Returns the output path."""
    makefile = os.path.join(project_dir, 'Makefile')
    includes = _extract_includes(makefile)
    sources = _find_sources(project_dir)

    if not sources:
        print(f"No .c source files found in {project_dir}", file=sys.stderr)
        return ""

    # Build include flags string
    inc_flags = ' '.join(includes) if includes else ''

    entries = []
    for src in sources:
        entries.append({
            "directory": os.path.abspath(project_dir),
            "command": f"clang -c {inc_flags} {src}".strip(),
            "file": src,
        })

    out_path = os.path.join(project_dir, 'compile_commands.json')
    with open(out_path, 'w') as f:
        json.dump(entries, f, indent=2)

    print(f"Generated {out_path} with {len(entries)} entries", file=sys.stderr)
    return out_path


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <project_dir>", file=sys.stderr)
        sys.exit(1)
    generate(sys.argv[1])
