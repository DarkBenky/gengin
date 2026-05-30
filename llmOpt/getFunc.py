"""
getFunc.py -- regex-based C context extractor for LLM prompts.

Usage:  python getFunc.py [FunctionName] [--depth N]
        [--list|--diff|--callers|--def|--restore|--restore-all|--replace file.c]
        [--replace-lines FILE START END new_impl.c|--patch patches.json|--meta]
        [--src path/to/file.c|--src-pair path/to/file.c|--help-api]
        [--hot-func FunctionName [--threshold N]]
        [--hot-file path/to/file.c [--threshold N]]
"""

import os
import re
import subprocess
import sys

GENGIN = os.path.join(os.path.dirname(os.path.abspath(__file__)), "gengin")

_SKIP = {
    'if', 'for', 'while', 'switch', 'do', 'else', 'return',
    'sizeof', 'typeof', '__typeof__', 'alignof', 'offsetof', '__attribute__',
}

_FUNC_RE = re.compile(
    r'^((?:(?:static|inline|extern|const|unsigned|signed|void|struct|__kernel|__attribute__)\s+)*'
    r'[\w\s\*]+?)\s+(\w+)\s*\(([^;{]*?)\)\s*\{',
    re.MULTILINE,
)

_STRUCT_RE = re.compile(r'(typedef\s+)?struct\s+(\w*)\s*\{', re.MULTILINE)

_sources = None
_functions = None
_structs = None


def init(base_dir=None):
    global _sources, _functions, _structs
    _sources = _read_sources(base_dir or GENGIN)
    _functions = find_functions(_sources)
    _structs = find_structs(_sources)


# --- Source parsing (private) ---

def _read_sources(base_dir):
    sources = {}
    for root, dirs, files in os.walk(base_dir):
        for f in files:
            if f.endswith(('.c', '.h', '.cl')):
                path = os.path.join(root, f)
                with open(path, errors='replace') as fh:
                    sources[path] = fh.read()
    return sources


def _strip_comments(text):
    # Replace block comments with equivalent newlines so line numbers stay aligned
    text = re.sub(r'/\*.*?\*/', lambda m: '\n' * m.group(0).count('\n'), text, flags=re.DOTALL)
    text = re.sub(r'//[^\n]*', '', text)
    return text


def _extract_block(text, brace_pos):
    depth = 0
    for i in range(brace_pos, len(text)):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                return text[brace_pos:i + 1]
    return text[brace_pos:]


def _called_functions(body):
    return set(re.findall(r'\b(\w+)\s*\(', body)) - _SKIP


def _used_types(text):
    return set(re.findall(r'\b([A-Z][A-Za-z0-9_]*)\b', text))


# --- Index builders ---

def find_functions(sources):
    results = {}
    for filepath, raw in sources.items():
        content = _strip_comments(raw)
        for m in _FUNC_RE.finditer(content):
            name = m.group(2)
            if name in _SKIP:
                continue
            brace_pos = m.start() + m.group(0).rfind('{')
            body = _extract_block(content, brace_pos)
            sig = m.group(0)[:m.group(0).rfind('{')].strip()
            start_line = content[:m.start()].count('\n') + 1
            end_line = content[:brace_pos + len(body)].count('\n') + 1
            results[name] = {
                'sig': sig,
                'body': body,
                'full': sig + '\n' + body,
                'file': os.path.relpath(filepath, GENGIN),
                'start': start_line,
                'end': end_line,
            }
    return results


def find_structs(sources):
    results = {}
    for filepath, raw in sources.items():
        content = _strip_comments(raw)
        for m in _STRUCT_RE.finditer(content):
            brace_pos = m.start() + m.group(0).rfind('{')
            body = _extract_block(content, brace_pos)
            end_pos = brace_pos + len(body)
            after = content[end_pos:end_pos + 64]
            td_match = re.match(r'\s*(\w+)\s*;', after)
            struct_tag = m.group(2) or ''
            typedef_name = td_match.group(1) if (m.group(1) and td_match) else None
            key = typedef_name or struct_tag
            if not key:
                continue
            full = content[m.start():end_pos]
            if typedef_name:
                full += td_match.group(0)
            start_line = content[:m.start()].count('\n') + 1
            end_line = content[:end_pos].count('\n') + 1
            entry = {'full': full.strip(), 'file': os.path.relpath(filepath, GENGIN), 'start': start_line, 'end': end_line}
            results[key] = entry
            if struct_tag and struct_tag != key:
                results[struct_tag] = entry
    return results


# --- Public API ---

def showContext(func, functions=None, structs=None, depth=1, returnString=False, context=None):
    functions = functions if functions is not None else _functions
    structs = structs if structs is not None else _structs
    if func not in functions:
        print(f"Function '{func}' not found in codebase.")
        return None

    all_types = set()
    visited = set()

    def _collect(name, current_depth):
        if name in visited or name not in functions:
            return
        visited.add(name)
        all_types.update(_used_types(functions[name]['full']))
        if current_depth < depth:
            for callee in _called_functions(functions[name]['body']):
                _collect(callee, current_depth + 1)

    _collect(func, 0)

    target = functions[func]
    lines = []
    lines.append(f"// {'=' * 60}")
    lines.append(f"// TARGET: {func}  [{target['file']}:{target['start']}-{target['end']}]")
    lines.append(f"// {'=' * 60}")
    lines.append(target['full'])

    callees = {n: functions[n] for n in (visited - {func})}
    if callees:
        lines.append(f"\n// {'=' * 60}")
        lines.append(f"// CALLED FUNCTIONS (depth={depth})")
        lines.append(f"// {'=' * 60}")
        for fname, fdata in sorted(callees.items()):
            lines.append(f"\n// --- {fname}  [{fdata['file']}:{fdata['start']}-{fdata['end']}] ---")
            lines.append(fdata['full'])

    found_structs = {t: structs[t] for t in all_types if t in structs}
    if found_structs:
        lines.append(f"\n// {'=' * 60}")
        lines.append(f"// USED STRUCTS / TYPES")
        lines.append(f"// {'=' * 60}")
        for tname, tdata in sorted(found_structs.items()):
            lines.append(f"\n// --- {tname}  [{tdata['file']}:{tdata['start']}-{tdata['end']}] ---")
            lines.append(tdata['full'])

    output = '\n'.join(lines)
    if context is not None:
        context.append({"type": "tool_use", "tool": "showContext", "input": {"func": func, "depth": depth}, "output": output})
    if returnString:
        return output
    print(output)


def getDefinition(name, functions=None, structs=None, returnString=False, context=None):
    functions = functions if functions is not None else _functions
    structs = structs if structs is not None else _structs
    lines = []
    if name in functions:
        fdata = functions[name]
        lines.append(f"// --- function: {name}  [{fdata['file']}:{fdata['start']}-{fdata['end']}] ---")
        lines.append(fdata['full'])
    if name in structs:
        sdata = structs[name]
        lines.append(f"// --- struct/type: {name}  [{sdata['file']}:{sdata['start']}-{sdata['end']}] ---")
        lines.append(sdata['full'])

    output = '\n'.join(lines) if lines else f"'{name}' not found in functions or structs."
    if context is not None:
        context.append({"type": "tool_use", "tool": "getDefinition", "input": name, "output": output})
    if returnString:
        return output
    print(output)


def getCallers(func, functions=None, returnString=False, context=None):
    functions = functions if functions is not None else _functions
    callers = {
        name: data for name, data in functions.items()
        if func in _called_functions(data['body'])
    }
    lines = [f"// Callers of '{func}':"]
    if callers:
        for fname, fdata in sorted(callers.items()):
            lines.append(f"//   {fname}  [{fdata['file']}]")
    else:
        lines.append("//   (none found)")

    output = '\n'.join(lines)
    if context is not None:
        context.append({"type": "tool_use", "tool": "getCallers", "input": func, "output": output})
    if returnString:
        return output
    print(output)


_CODE_EXTS = ('.c', '.h', '.cl', '.go', '.py')

def getDiff(returnString=False, context=None, code_only=False):
    cmd = ['git', 'diff', 'HEAD']
    if code_only:
        cmd += ['--', *[f'*{e}' for e in _CODE_EXTS]]
    result = subprocess.run(cmd, cwd=GENGIN, capture_output=True, text=True)
    output = result.stdout or "(no changes)"
    if context is not None:
        context.append({"type": "tool_use", "tool": "getDiff", "input": None, "output": output})
    if returnString:
        return output
    print(output)


def grepSource(pattern, rel_path=None, ignore_case=False, returnString=False, context=None):
    """grep pattern across project source files (or a specific path)."""
    search_root = GENGIN
    if rel_path:
        # reuse the same traversal guard used for all path args
        normalized = rel_path.replace("\\", "/")
        if ".." in normalized.split("/"):
            msg = f"path traversal rejected: {rel_path!r}"
            if context is not None:
                context.append({"type": "tool_use", "tool": "grepSource", "input": {"pattern": pattern, "rel_path": rel_path}, "output": msg})
            return msg
        search_root = os.path.join(GENGIN, rel_path)

    cmd = ["grep", "-rn", "--binary-files=without-match",
           "--include=*.c", "--include=*.h", "--include=*.cl"]
    if ignore_case:
        cmd.append("-i")
    cmd += [pattern, search_root]

    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode == 2:
        output = f"grep error: {result.stderr.strip()}"
    else:
        raw = result.stdout.strip()
        # make paths relative to project root for readability
        raw = raw.replace(GENGIN + "/", "")
        output = raw if raw else "(no matches)"

    if context is not None:
        context.append({"type": "tool_use", "tool": "grepSource",
                        "input": {"pattern": pattern, "rel_path": rel_path},
                        "output": output})
    if returnString:
        return output
    print(output)


def findSymbol(name, returnString=False, context=None):
    """Word-boundary grep for an identifier across all source files."""
    cmd = ["grep", "-rn", "--binary-files=without-match",
           "--include=*.c", "--include=*.h", "--include=*.cl",
           "-w", name, GENGIN]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode == 2:
        output = f"grep error: {result.stderr.strip()}"
    else:
        raw = result.stdout.strip().replace(GENGIN + "/", "")
        output = raw if raw else "(no matches)"
    if context is not None:
        context.append({"type": "tool_use", "tool": "findSymbol", "input": name, "output": output})
    if returnString:
        return output
    print(output)


def listDir(rel_path=".", returnString=False, context=None):
    """List files and directories under rel_path (relative to project root)."""
    normalized = rel_path.replace("\\", "/")
    if ".." in normalized.split("/"):
        msg = f"path traversal rejected: {rel_path!r}"
        if context is not None:
            context.append({"type": "tool_use", "tool": "listDir", "input": rel_path, "output": msg})
        return msg
    target = os.path.join(GENGIN, rel_path)
    if not os.path.isdir(target):
        output = f"Not a directory: {rel_path}"
    else:
        lines = []
        for entry in sorted(os.scandir(target), key=lambda e: (e.is_file(), e.name)):
            suffix = "/" if entry.is_dir() else ""
            lines.append(os.path.join(rel_path, entry.name) + suffix)
        output = "\n".join(lines) if lines else "(empty directory)"
    if context is not None:
        context.append({"type": "tool_use", "tool": "listDir", "input": rel_path, "output": output})
    if returnString:
        return output
    print(output)


def readLines(rel_path, start, end, returnString=False, context=None):
    """Read lines start..end (1-indexed inclusive) from a source file."""
    normalized = rel_path.replace("\\", "/")
    if ".." in normalized.split("/"):
        msg = f"path traversal rejected: {rel_path!r}"
        if context is not None:
            context.append({"type": "tool_use", "tool": "readLines",
                            "input": {"rel_path": rel_path, "start": start, "end": end}, "output": msg})
        return msg
    filepath = os.path.join(GENGIN, rel_path)
    try:
        with open(filepath, errors='replace') as fh:
            all_lines = fh.readlines()
        s = max(1, start) - 1
        e = min(len(all_lines), end)
        selected = all_lines[s:e]
        numbered = "".join(f"{s + i + 1}: {line}" for i, line in enumerate(selected))
        output = numbered if numbered else "(no lines in range)"
    except FileNotFoundError:
        output = f"File not found: {rel_path}"
    if context is not None:
        context.append({"type": "tool_use", "tool": "readLines",
                        "input": {"rel_path": rel_path, "start": start, "end": end}, "output": output})
    if returnString:
        return output
    print(output)


_ALLOWED_COMMANDS = {
    # binary analysis
    "nm", "size", "objdump", "readelf", "addr2line", "strings", "ldd", "file", "ar", "strip", "objcopy",
    # profiling
    "perf", "gprof", "valgrind",
    # build
    "make", "cmake", "ninja",
    # compilers / static analysis
    "gcc", "g++", "clang", "clang++", "cc", "clang-format", "clang-tidy", "cppcheck",
    # scripting / runtimes
    "python", "python3", "go", "perl",
    # file editing via CLI
    "patch",   # apply unified diff patches
    "tee",     # write stdin to a file (useful at end of a pipeline)
    "tr",      # translate/delete characters
    # text analysis (read-only)
    "diff", "wc", "awk", "sed", "sort", "uniq", "head", "tail", "xxd", "hexdump", "cat",
}

def runCommand(cmd_list, returnString=False, context=None):
    """Run a whitelisted analysis command in the project directory."""
    if not cmd_list or not isinstance(cmd_list, list):
        output = "cmd_list must be a non-empty list of strings"
        if context is not None:
            context.append({"type": "tool_use", "tool": "runCommand", "input": cmd_list, "output": output})
        return output
    executable = os.path.basename(cmd_list[0])
    if executable not in _ALLOWED_COMMANDS:
        output = f"Command not allowed: {executable!r}. Allowed: {sorted(_ALLOWED_COMMANDS)}"
        if context is not None:
            context.append({"type": "tool_use", "tool": "runCommand", "input": cmd_list, "output": output})
        return output
    # block path traversal in any argument
    for arg in cmd_list[1:]:
        normalized = str(arg).replace("\\", "/")
        if ".." in normalized.split("/"):
            output = f"path traversal rejected in args: {arg!r}"
            if context is not None:
                context.append({"type": "tool_use", "tool": "runCommand", "input": cmd_list, "output": output})
            return output
    try:
        result = subprocess.run(cmd_list, capture_output=True, text=True, cwd=GENGIN)
        raw = (result.stdout + result.stderr).strip()
        output = raw if raw else "(no output)"
    except FileNotFoundError:
        output = f"Command not found: {cmd_list[0]!r} — is it installed?"
    if context is not None:
        context.append({"type": "tool_use", "tool": "runCommand", "input": cmd_list, "output": output})
    if returnString:
        return output
    print(output)

def readSourceFile(rel_path, returnString=False, context=None):
    filepath = os.path.join(GENGIN, rel_path)
    try:
        with open(filepath, errors='replace') as fh:
            output = fh.read()
    except FileNotFoundError:
        output = f"File not found: {rel_path}"
    if context is not None:
        context.append({"type": "tool_use", "tool": "readSourceFile", "input": rel_path, "output": output})
    if returnString:
        return output
    print(output)


def listFunctions(functions=None, returnString=False, context=None):
    functions = functions if functions is not None else _functions
    entries = sorted(
        [{'name': name, 'file': data['file'], 'sig': data['sig']}
         for name, data in functions.items()],
        key=lambda x: (x['file'], x['name']),
    )
    lines = [f"{e['file']:<45}  {e['name']}" for e in entries]
    output = '\n'.join(lines)
    if context is not None:
        context.append({"type": "tool_use", "tool": "listFunctions", "input": None, "output": output})
    if returnString:
        return output
    print(output)
    return entries


def applyChange(func_name, new_definition, functions=None, sources=None, context=None):
    functions = functions if functions is not None else _functions
    sources = sources if sources is not None else _sources
    if func_name not in functions:
        if context is not None:
            context.append({"type": "tool_use", "tool": "applyChange", "input": func_name, "output": f"failed: '{func_name}' not found"})
        return False

    info = functions[func_name]
    filepath = next((p for p in sources if os.path.relpath(p, GENGIN) == info['file']), None)
    if filepath is None:
        if context is not None:
            context.append({"type": "tool_use", "tool": "applyChange", "input": func_name, "output": "failed: source file not found"})
        return False

    original_text = sources[filepath]
    sig_pattern = re.escape(info['sig'].split('(')[0].strip().split()[-1])
    func_start_re = re.compile(
        r'(?:^|\n)((?:(?:static|inline|extern|const|unsigned|signed|void|struct)\s+)*'
        r'[\w\s\*]+?\s+)?' + sig_pattern + r'\s*\([^;]*?\)\s*\{',
        re.MULTILINE,
    )
    match = func_start_re.search(original_text)
    if match is None:
        if context is not None:
            context.append({"type": "tool_use", "tool": "applyChange", "input": func_name, "output": f"failed: could not locate '{func_name}' in {info['file']}"})
        return False

    brace_pos = match.start() + match.group(0).rfind('{')
    old_block = _extract_block(original_text, brace_pos)
    old_span_start = match.start() if original_text[match.start()] == '\n' else match.start()
    old_span_end = brace_pos + len(old_block)

    new_text = original_text[:old_span_start] + '\n' + new_definition.strip() + '\n' + original_text[old_span_end:]
    with open(filepath, 'w') as fh:
        fh.write(new_text)
    _refresh_file(filepath)
    if context is not None:
        context.append({"type": "tool_use", "tool": "applyChange", "input": func_name, "output": f"replaced '{func_name}' in {info['file']}"})
    return True


def restoreAll(context=None):
    result = subprocess.run(
        ['git', 'checkout', 'HEAD', '--', '.'],
        cwd=GENGIN, capture_output=True, text=True,
    )
    success = result.returncode == 0
    if context is not None:
        context.append({"type": "tool_use", "tool": "restoreAll", "input": None, "output": "restored all files to HEAD" if success else f"failed: {result.stderr.strip()}"})
    return success


def restoreFile(rel_path, context=None):
    result = subprocess.run(
        ['git', 'checkout', 'HEAD', '--', rel_path],
        cwd=GENGIN, capture_output=True, text=True,
    )
    success = result.returncode == 0
    if context is not None:
        context.append({"type": "tool_use", "tool": "restoreFile", "input": rel_path, "output": f"restored {rel_path}" if success else f"failed: {result.stderr.strip()}"})
    return success


def restoreFunction(func_name, functions=None, context=None):
    functions = functions if functions is not None else _functions
    if func_name not in functions:
        if context is not None:
            context.append({"type": "tool_use", "tool": "restoreFunction", "input": func_name, "output": f"failed: '{func_name}' not found"})
        return False
    return restoreFile(functions[func_name]['file'], context=context)


def _find_normalized_lines(content_lines, old_lines):
    """Find old_lines in content_lines ignoring trailing whitespace. Returns start index or -1."""
    norm_old = [l.rstrip() for l in old_lines]
    norm_content = [l.rstrip() for l in content_lines]
    n = len(norm_old)
    for i in range(len(norm_content) - n + 1):
        if norm_content[i:i + n] == norm_old:
            return i
    return -1


def searchReplace(rel_path, old_text, new_text, context=None):
    """Find old_text exactly once in rel_path and replace it with new_text."""
    filepath = os.path.join(GENGIN, rel_path)
    try:
        with open(filepath, errors='replace') as fh:
            content = fh.read()
    except FileNotFoundError:
        msg = f"File not found: {rel_path}"
        if context is not None:
            context.append({"type": "tool_use", "tool": "searchReplace", "input": {"file": rel_path}, "output": msg})
        return False

    count = content.count(old_text)
    if count > 1:
        msg = f"old_text matched {count} locations in {rel_path}. Add more surrounding lines to make it unique."
        if context is not None:
            context.append({"type": "tool_use", "tool": "searchReplace", "input": {"file": rel_path}, "output": msg})
        return False

    if count == 1:
        new_content = content.replace(old_text, new_text, 1)
    else:
        # Exact match failed — try trailing-whitespace-insensitive line match
        content_lines = content.splitlines(keepends=True)
        old_lines = old_text.splitlines(keepends=True)
        idx = _find_normalized_lines(
            [l.rstrip('\r\n') for l in content_lines],
            [l.rstrip('\r\n') for l in old_lines],
        )
        if idx == -1:
            msg = f"old_text not found in {rel_path} (tried exact and whitespace-normalized match). Use showSrcPair to copy the exact text."
            if context is not None:
                context.append({"type": "tool_use", "tool": "searchReplace", "input": {"file": rel_path}, "output": msg})
            return False
        # Check uniqueness of normalized match
        matches = 0
        norm_old = [l.rstrip() for l in old_lines]
        norm_content = [l.rstrip() for l in content_lines]
        n = len(norm_old)
        for i in range(len(norm_content) - n + 1):
            if norm_content[i:i + n] == norm_old:
                matches += 1
        if matches > 1:
            msg = f"old_text matched {matches} locations (normalized) in {rel_path}. Add more surrounding lines."
            if context is not None:
                context.append({"type": "tool_use", "tool": "searchReplace", "input": {"file": rel_path}, "output": msg})
            return False
        new_lines = new_text.splitlines(keepends=True)
        new_content = "".join(content_lines[:idx] + new_lines + content_lines[idx + n:])

    with open(filepath, 'w') as fh:
        fh.write(new_content)
    _refresh_file(filepath)
    msg = f"replaced 1 occurrence in {rel_path}"
    if context is not None:
        context.append({"type": "tool_use", "tool": "searchReplace", "input": {"file": rel_path}, "output": msg})
    return True


def replaceLines(rel_path, start, end, new_text, context=None):
    try:
        with open(filepath, errors='replace') as fh:
            lines = fh.readlines()
    except FileNotFoundError:
        msg = f"File not found: {rel_path}"
        if context is not None:
            context.append({"type": "tool_use", "tool": "replaceLines", "input": {"file": rel_path, "start": start, "end": end}, "output": msg})
        return False

    if start < 1 or end > len(lines) or start > end:
        msg = f"invalid range {start}-{end} for {rel_path} ({len(lines)} lines)"
        if context is not None:
            context.append({"type": "tool_use", "tool": "replaceLines", "input": {"file": rel_path, "start": start, "end": end}, "output": msg})
        return False

    new_lines = new_text.splitlines(keepends=True)
    if new_lines and not new_lines[-1].endswith('\n'):
        new_lines[-1] += '\n'
    lines[start - 1:end] = new_lines

    with open(filepath, 'w') as fh:
        fh.writelines(lines)
    _refresh_file(filepath)

    msg = f"replaced lines {start}-{end} in {rel_path}"
    if context is not None:
        context.append({"type": "tool_use", "tool": "replaceLines", "input": {"file": rel_path, "start": start, "end": end}, "output": msg})
    return True


def insertLines(rel_path, after_line, new_text, context=None):
    """Insert new_text after after_line (1-indexed). Use after_line=0 to prepend."""
    filepath = os.path.join(GENGIN, rel_path)
    try:
        with open(filepath, errors='replace') as fh:
            lines = fh.readlines()
    except FileNotFoundError:
        msg = f"File not found: {rel_path}"
        if context is not None:
            context.append({"type": "tool_use", "tool": "insertLines", "input": {"file": rel_path, "after_line": after_line}, "output": msg})
        return False

    if after_line < 0 or after_line > len(lines):
        msg = f"after_line {after_line} out of range for {rel_path} ({len(lines)} lines)"
        if context is not None:
            context.append({"type": "tool_use", "tool": "insertLines", "input": {"file": rel_path, "after_line": after_line}, "output": msg})
        return False

    new_lines = new_text.splitlines(keepends=True)
    if new_lines and not new_lines[-1].endswith('\n'):
        new_lines[-1] += '\n'
    lines[after_line:after_line] = new_lines

    with open(filepath, 'w') as fh:
        fh.writelines(lines)
    _refresh_file(filepath)
    msg = f"inserted {len(new_lines)} line(s) after line {after_line} in {rel_path}"
    if context is not None:
        context.append({"type": "tool_use", "tool": "insertLines", "input": {"file": rel_path, "after_line": after_line}, "output": msg})
    return True


def deleteLines(rel_path, start, end, context=None):
    """Delete lines start..end (1-indexed inclusive) from rel_path."""
    filepath = os.path.join(GENGIN, rel_path)
    try:
        with open(filepath, errors='replace') as fh:
            lines = fh.readlines()
    except FileNotFoundError:
        msg = f"File not found: {rel_path}"
        if context is not None:
            context.append({"type": "tool_use", "tool": "deleteLines", "input": {"file": rel_path, "start": start, "end": end}, "output": msg})
        return False

    if start < 1 or end > len(lines) or start > end:
        msg = f"invalid range {start}-{end} for {rel_path} ({len(lines)} lines)"
        if context is not None:
            context.append({"type": "tool_use", "tool": "deleteLines", "input": {"file": rel_path, "start": start, "end": end}, "output": msg})
        return False

    del lines[start - 1:end]
    with open(filepath, 'w') as fh:
        fh.writelines(lines)
    _refresh_file(filepath)
    msg = f"deleted lines {start}-{end} from {rel_path}"
    if context is not None:
        context.append({"type": "tool_use", "tool": "deleteLines", "input": {"file": rel_path, "start": start, "end": end}, "output": msg})
    return True


def searchReplaceMulti(rel_path, replacements, context=None):
    """
    Apply multiple find-and-replace pairs in one call, top-to-bottom.
    replacements: list of {"old": str, "new": str}
    Each old_text must be unique in the file at the time it is applied.
    Stops and reports the first failure without writing the file.
    """
    filepath = os.path.join(GENGIN, rel_path)
    try:
        with open(filepath, errors='replace') as fh:
            content = fh.read()
    except FileNotFoundError:
        msg = f"File not found: {rel_path}"
        if context is not None:
            context.append({"type": "tool_use", "tool": "searchReplaceMulti", "input": {"file": rel_path}, "output": msg})
        return False

    for i, pair in enumerate(replacements):
        old, new = pair["old"], pair["new"]
        count = content.count(old)
        if count == 0:
            # fallback: whitespace-normalised line match
            content_lines = content.splitlines(keepends=True)
            old_lines = old.splitlines(keepends=True)
            idx = _find_normalized_lines(
                [l.rstrip('\r\n') for l in content_lines],
                [l.rstrip('\r\n') for l in old_lines],
            )
            if idx == -1:
                msg = f"replacement {i}: old_text not found in {rel_path}"
                if context is not None:
                    context.append({"type": "tool_use", "tool": "searchReplaceMulti", "input": {"file": rel_path}, "output": msg})
                return False
            norm_old = [l.rstrip() for l in old_lines]
            norm_content = [l.rstrip() for l in content_lines]
            n = len(norm_old)
            matches = sum(1 for j in range(len(norm_content) - n + 1) if norm_content[j:j+n] == norm_old)
            if matches > 1:
                msg = f"replacement {i}: old_text matched {matches} locations (normalized) in {rel_path}. Add more context."
                if context is not None:
                    context.append({"type": "tool_use", "tool": "searchReplaceMulti", "input": {"file": rel_path}, "output": msg})
                return False
            new_lines = new.splitlines(keepends=True)
            content = "".join(content_lines[:idx] + new_lines + content_lines[idx + n:])
        elif count > 1:
            msg = f"replacement {i}: old_text matched {count} locations in {rel_path}. Add more surrounding lines."
            if context is not None:
                context.append({"type": "tool_use", "tool": "searchReplaceMulti", "input": {"file": rel_path}, "output": msg})
            return False
        else:
            content = content.replace(old, new, 1)

    with open(filepath, 'w') as fh:
        fh.write(content)
    _refresh_file(filepath)
    msg = f"applied {len(replacements)} replacement(s) in {rel_path}"
    if context is not None:
        context.append({"type": "tool_use", "tool": "searchReplaceMulti", "input": {"file": rel_path}, "output": msg})
    return True


def applyPatch(patches, context=None):
    """patches: list of {"file": rel_path, "start": int, "end": int, "text": str}"""
    from collections import defaultdict
    by_file = defaultdict(list)
    for p in patches:
        by_file[p['file']].append(p)

    results = []
    for rel_path, file_patches in by_file.items():
        filepath = os.path.join(GENGIN, rel_path)
        try:
            with open(filepath, errors='replace') as fh:
                lines = fh.readlines()
        except FileNotFoundError:
            results.append(f"failed: {rel_path} not found")
            continue

        # apply in reverse line order so earlier patches don't shift later indices
        for p in sorted(file_patches, key=lambda x: x['start'], reverse=True):
            new_lines = p['text'].splitlines(keepends=True)
            if new_lines and not new_lines[-1].endswith('\n'):
                new_lines[-1] += '\n'
            lines[p['start'] - 1:p['end']] = new_lines

        with open(filepath, 'w') as fh:
            fh.writelines(lines)
        _refresh_file(filepath)
        results.append(f"patched {len(file_patches)} region(s) in {rel_path}")

    output = '\n'.join(results) if results else "no patches applied"
    if context is not None:
        context.append({"type": "tool_use", "tool": "applyPatch", "input": patches, "output": output})
    print(output)
    return bool(results)


def showContextWithMeta(func, functions=None, structs=None, depth=1, context=None):
    """Like showContext but returns {"text": str, "functions": [...], "structs": [...]} with line metadata."""
    functions = functions if functions is not None else _functions
    structs = structs if structs is not None else _structs
    if func not in functions:
        return None

    all_types = set()
    visited = set()

    def _collect(name, current_depth):
        if name in visited or name not in functions:
            return
        visited.add(name)
        all_types.update(_used_types(functions[name]['full']))
        if current_depth < depth:
            for callee in _called_functions(functions[name]['body']):
                _collect(callee, current_depth + 1)

    _collect(func, 0)

    target = functions[func]
    lines = []
    lines.append(f"// {'=' * 60}")
    lines.append(f"// TARGET: {func}  [{target['file']}:{target['start']}-{target['end']}]")
    lines.append(f"// {'=' * 60}")
    lines.append(target['full'])

    meta_functions = [{'name': func, 'file': target['file'], 'start': target['start'], 'end': target['end']}]

    callees = {n: functions[n] for n in (visited - {func})}
    if callees:
        lines.append(f"\n// {'=' * 60}")
        lines.append(f"// CALLED FUNCTIONS (depth={depth})")
        lines.append(f"// {'=' * 60}")
        for fname, fdata in sorted(callees.items()):
            lines.append(f"\n// --- {fname}  [{fdata['file']}:{fdata['start']}-{fdata['end']}] ---")
            lines.append(fdata['full'])
            meta_functions.append({'name': fname, 'file': fdata['file'], 'start': fdata['start'], 'end': fdata['end']})

    found_structs = {t: structs[t] for t in all_types if t in structs}
    meta_structs = []
    if found_structs:
        lines.append(f"\n// {'=' * 60}")
        lines.append(f"// USED STRUCTS / TYPES")
        lines.append(f"// {'=' * 60}")
        for tname, tdata in sorted(found_structs.items()):
            lines.append(f"\n// --- {tname}  [{tdata['file']}:{tdata['start']}-{tdata['end']}] ---")
            lines.append(tdata['full'])
            meta_structs.append({'name': tname, 'file': tdata['file'], 'start': tdata['start'], 'end': tdata['end']})

    result = {'text': '\n'.join(lines), 'functions': meta_functions, 'structs': meta_structs}
    if context is not None:
        context.append({"type": "tool_use", "tool": "showContextWithMeta", "input": {"func": func, "depth": depth}, "output": result})
    return result


def _refresh_file(filepath):
    """Re-parse a single file into the live _functions/_structs/_sources indexes."""
    with open(filepath, errors='replace') as fh:
        _sources[filepath] = fh.read()
    single = {filepath: _sources[filepath]}
    new_funcs = find_functions(single)
    for k in [k for k, v in _functions.items() if os.path.join(GENGIN, v['file']) == filepath]:
        del _functions[k]
    _functions.update(new_funcs)
    new_structs = find_structs(single)
    for k in [k for k, v in _structs.items() if os.path.join(GENGIN, v['file']) == filepath]:
        del _structs[k]
    _structs.update(new_structs)


def showSrc(rel_path, returnString=False, context=None):
    filepath = os.path.join(GENGIN, rel_path)
    try:
        with open(filepath, errors='replace') as fh:
            file_lines = fh.readlines()
    except FileNotFoundError:
        output = f"File not found: {rel_path}"
        if context is not None:
            context.append({"type": "tool_use", "tool": "showSrc", "input": rel_path, "output": output})
        if returnString:
            return output
        print(output)
        return

    numbered = [f"{i + 1:4}: {line}" for i, line in enumerate(file_lines)]
    output = f"// {rel_path}  ({len(file_lines)} lines)\n" + ''.join(numbered)
    if context is not None:
        context.append({"type": "tool_use", "tool": "showSrc", "input": rel_path, "output": output})
    if returnString:
        return output
    print(output)


def showSrcPair(rel_path, returnString=False, context=None):
    """Show a .c/.h file together with its companion (swaps extension to find it)."""
    base, ext = os.path.splitext(rel_path)
    companion = base + ('.h' if ext == '.c' else '.c') if ext in ('.c', '.h') else None

    parts = [showSrc(rel_path, returnString=True)]
    if companion and os.path.exists(os.path.join(GENGIN, companion)):
        parts.append(showSrc(companion, returnString=True))

    output = '\n\n'.join(parts)
    if context is not None:
        context.append({"type": "tool_use", "tool": "showSrcPair", "input": rel_path, "output": output})
    if returnString:
        return output
    print(output)


# --- Perf hot-line annotation ---

_ANNOT_LINE_RE = re.compile(r'^\s*([\d.]+)\s*:\s*([0-9a-f]+):\s')


def _perfAnnotateFunc(func_name, cwd=None):
    """
    Return {source_lineno: pct} for func_name using perf annotate + addr2line.
    Requires perf.data and a binary built with -g in the same directory.
    """
    search = cwd or GENGIN
    perf_cwd = None
    for _ in range(4):
        if os.path.exists(os.path.join(search, "perf.data")):
            perf_cwd = search
            break
        parent = os.path.dirname(search)
        if parent == search:
            break
        search = parent
    if perf_cwd is None:
        return {}

    binary = os.path.join(perf_cwd, "main")
    if not os.path.exists(binary):
        return {}

    try:
        result = subprocess.run(
            ["sudo", "perf", "annotate", "--stdio", "-s", func_name,
             "-i", "perf.data", "-f"],
            capture_output=True, text=True, cwd=perf_cwd, timeout=30
        )

        # collect {addr: pct} from lines like "    4.17 :   124dd:  movss ..."
        addr_pct = {}
        for line in result.stdout.splitlines():
            m = _ANNOT_LINE_RE.match(line)
            if m:
                pct = float(m.group(1))
                if pct > 0.0:
                    addr = int(m.group(2), 16)
                    addr_pct[addr] = addr_pct.get(addr, 0.0) + pct

        if not addr_pct:
            return {}

        # map addresses to source line numbers via addr2line
        addrs_hex = [hex(a) for a in addr_pct]
        a2l = subprocess.run(
            ["addr2line", "-e", binary, "-f"] + addrs_hex,
            capture_output=True, text=True, timeout=30
        )

        hotness = {}
        a2l_lines = a2l.stdout.splitlines()
        pcts = list(addr_pct.values())
        # addr2line -f outputs 2 lines per address: func_name, then file:lineno
        for i, pct in enumerate(pcts):
            loc_idx = i * 2 + 1
            if loc_idx >= len(a2l_lines):
                break
            loc = a2l_lines[loc_idx]
            if ':' in loc and not loc.startswith('?'):
                try:
                    lineno = int(loc.rsplit(':', 1)[1])
                    hotness[lineno] = hotness.get(lineno, 0.0) + pct
                except (ValueError, IndexError):
                    pass

        return hotness
    except Exception:
        return {}


def _annotateLines(source_lines, start_lineno, hotness, threshold):
    """Prepend /* HOT X.X% */ markers to hot lines; pad cold lines to same width."""
    pad = " " * 16
    result = []
    for i, line in enumerate(source_lines):
        pct = hotness.get(start_lineno + i, 0.0)
        if pct >= threshold:
            result.append(f"/* HOT {pct:5.1f}% */ {line}")
        else:
            result.append(pad + line)
    return result


def hotAnnotateFunc(func_name, threshold=0.5, functions=None, context=None):
    """
    Return the source of func_name annotated with per-line perf percentages.
    Lines consuming >= threshold% of samples are prefixed with /* HOT X.X% */.
    Requires perf.data from a previous makeFlame() run.
    """
    functions = functions if functions is not None else _functions
    if func_name not in functions:
        output = f"Function '{func_name}' not found in codebase index."
        if context is not None:
            context.append({"type": "tool_use", "tool": "hotAnnotateFunc",
                            "input": func_name, "output": output})
        return output

    info = functions[func_name]
    filepath = next(
        (p for p in (_sources or {}) if os.path.relpath(p, GENGIN) == info['file']),
        None
    )
    if filepath is None:
        output = f"Source file not found for '{func_name}'."
        if context is not None:
            context.append({"type": "tool_use", "tool": "hotAnnotateFunc",
                            "input": func_name, "output": output})
        return output

    hotness = _perfAnnotateFunc(func_name)

    with open(filepath, errors='replace') as fh:
        all_lines = fh.read().splitlines()

    func_lines = all_lines[info['start'] - 1:info['end']]
    annotated = _annotateLines(func_lines, info['start'], hotness, threshold)

    header = (
        f"// hotAnnotateFunc: {func_name}  [{info['file']}:{info['start']}-{info['end']}]\n"
        f"// threshold={threshold}%  |  /* HOT X.X% */ marks hot lines\n"
    )
    suffix = "" if hotness else "\n// NOTE: no perf.data or perf annotate failed -- run makeFlame() first."
    output = header + '\n'.join(annotated) + suffix

    if context is not None:
        context.append({"type": "tool_use", "tool": "hotAnnotateFunc",
                        "input": {"func": func_name, "threshold": threshold}, "output": output})
    return output


def hotAnnotateFile(rel_path, threshold=0.5, functions=None, context=None):
    """
    Return an entire source file annotated with per-line perf percentages.
    Collects perf annotate data for every function found in rel_path and
    merges the line-level hotness across all of them.
    Requires perf.data from a previous makeFlame() run.
    """
    functions = functions if functions is not None else _functions
    filepath = os.path.join(GENGIN, rel_path)
    try:
        with open(filepath, errors='replace') as fh:
            all_lines = fh.read().splitlines()
    except FileNotFoundError:
        output = f"File not found: {rel_path}"
        if context is not None:
            context.append({"type": "tool_use", "tool": "hotAnnotateFile",
                            "input": rel_path, "output": output})
        return output

    file_funcs = [name for name, data in functions.items() if data['file'] == rel_path]
    combined = {}
    for fn in file_funcs:
        for lineno, pct in _perfAnnotateFunc(fn).items():
            combined[lineno] = combined.get(lineno, 0.0) + pct

    annotated = _annotateLines(all_lines, 1, combined, threshold)
    header = (
        f"// hotAnnotateFile: {rel_path}\n"
        f"// threshold={threshold}%  |  /* HOT X.X% */ marks hot lines\n"
    )
    suffix = "" if combined else "\n// NOTE: no perf.data or no matching functions -- run makeFlame() first."
    output = header + '\n'.join(annotated) + suffix

    if context is not None:
        context.append({"type": "tool_use", "tool": "hotAnnotateFile",
                        "input": {"rel_path": rel_path, "threshold": threshold}, "output": output})
    return output


_API = [
    ("Exploration", [
        ("showContext(func, depth=1)",
         "Target function + callees + used types. Headers show file:start-end line ranges."),
        ("showContextWithMeta(func, depth=1)",
         "Same as showContext but returns dict {text, functions:[{name,file,start,end}], structs:[...]}"),
        ("getDefinition(name)",
         "Print function or struct/typedef definition with file:start-end."),
        ("getCallers(func)",
         "List every function that calls the given function."),
        ("showSrc(rel_path)",
         "Print file with   N: line   prefixes. Every N maps directly to replaceLines."),
        ("showSrcPair(rel_path)",
         "Print a .c or .h file and its companion (auto-finds by swapping extension)."),
        ("listFunctions()",
         "List all indexed functions sorted by file. Returns list of {name, file, sig}."),
        ("readSourceFile(rel_path)",
         "Read raw file content (no line numbers)."),
        ("hotAnnotateFunc(func_name, threshold=0.5)",
         "Source of func_name annotated with /* HOT X.X% */ on lines that consumed >= threshold% of perf samples. Requires makeFlame() to have been run."),
        ("hotAnnotateFile(rel_path, threshold=0.5)",
         "Entire source file annotated with /* HOT X.X% */ per-line hotness across all functions in it. Requires makeFlame() to have been run."),
        ("grepSource(pattern, rel_path=None, ignore_case=False)",
         "grep a regex/literal pattern across all .c/.h/.cl source files. rel_path limits search to a subdirectory. Returns file:line:match lines."),
        ("findSymbol(name)",
         "Word-boundary grep for a symbol name across all source files. Finds declarations, definitions, and call sites without noise from partial matches."),
        ("listDir(rel_path='.')",
         "List files and subdirectories under rel_path (relative to project root). Useful to discover file layout before reading."),
        ("readLines(rel_path, start, end)",
         "Read lines start..end (1-indexed inclusive) from a file with line-number prefixes. Pairs with showSrc output to zoom into a specific region."),
        ("runCommand(cmd_list)",
         "Run a whitelisted command in the project directory. cmd_list is a list of strings. "
         "Analysis: nm, size, objdump, readelf, addr2line, strings, ldd, file, ar, strip, objcopy, perf, gprof, valgrind. "
         "Build: make, cmake, ninja. "
         "Compilers/static analysis: gcc, g++, clang, clang++, cc, clang-format, clang-tidy, cppcheck. "
         "Scripting: python, python3, go, perl. "
         "File editing: patch (apply unified diffs), tee (write pipeline output to file), tr, sed -i, awk. "
         "Text/read: diff, wc, sort, uniq, head, tail, xxd, hexdump, cat."),
    ]),
    ("Applying Changes", [
        ("searchReplace(rel_path, old_text, new_text)",
         "PREFERRED edit tool. Find old_text (must be unique in file) and replace with new_text. No line numbers needed — copy exact text from showSrc/showContext output."),
        ("searchReplaceMulti(rel_path, replacements)",
         "Apply multiple find-and-replace pairs in one call. replacements = [{\"old\": str, \"new\": str}, ...]. Applied top-to-bottom; stops on first failure. Use when renaming a variable/symbol across a file or making several non-adjacent edits at once."),
        ("applyChange(func_name, new_definition)",
         "Replace a named function in-place by locating its signature in the source file."),
        ("replaceLines(rel_path, start, end, new_text)",
         "Replace lines start..end (1-indexed inclusive) with new_text. Pairs with showSrc line numbers."),
        ("insertLines(rel_path, after_line, new_text)",
         "Insert new_text after after_line (1-indexed). Use after_line=0 to prepend at top of file. Use to add #includes, helpers, or a new function without disturbing existing line numbers."),
        ("deleteLines(rel_path, start, end)",
         "Delete lines start..end (1-indexed inclusive). Use to remove dead code, redundant includes, or debug prints."),
        ("applyPatch(patches)",
         "Batch line-range replacement. patches = [{file, start, end, text}, ...]. Applied in reverse line order per file."),
    ]),
    ("Git / Restore", [
        ("getDiff()",
         "Show current git diff vs HEAD."),
        ("restoreAll()",
         "git checkout HEAD -- . to discard all changes."),
        ("restoreFile(rel_path)",
         "Restore a single file to HEAD."),
        ("restoreFunction(func_name)",
         "Restore the source file that contains the named function to HEAD."),
    ]),
    ("Build / Profiling  [main.py]", [
        ("buildProject()",
         "Run make in the project directory. Raises on failure."),
        ("makeBench()",
         "Run make bench, returns (stdout, bench_results_dict)."),
        ("makeFlame()",
         "Run make flame then parse perf.data. Returns {total_samples, hot_functions, hot_paths}."),
        ("getTree(path)",
         "Return directory tree as string."),
        ("getTodos(path)",
         "Grep all TODO comments in path."),
        ("createPR(title, body, branch, commit_msg=None)",
         "Commit all changes, push to branch, open a GitHub PR via gh CLI. body should describe what changed and why. Returns the PR URL."),
    ]),
    ("Micro-Benchmark Sandbox  [main.py]", [
        ("createFuncBench(func_name, header_code, impl_code)",
         "Create bench/<func_name>.h (header_code) and bench/<func_name>.c (impl_code). "
         "impl_code must #include \"<func_name>.h\" and \"timings.h\", implement the variants, "
         "and contain a main() that times each variant with clock_gettime, calls "
         "ComputePerformanceMetrics(), prints results, and validates correctness. "
         "No OpenCL/minifb allowed — only -lm."),
        ("runFuncBench(func_name)",
         "Build and run bench/<func_name>.c (linked with tests/timings.c). "
         "Returns stdout with timing and validation output. Use before touching main codebase."),
        ("deleteFuncBench(func_name)",
         "Remove bench/<func_name>.h, bench/<func_name>.c, and the compiled binary."),
    ]),
    ("Knowledge Persistence  [main.py]", [
        ("syncPlannerToCodebaseContext()",
         "Ask the model to distill planner notes/tasks into HIGH-LEVEL insights "
         "(architectural findings, confirmed wins, remaining hotspots, techniques "
         "to try/avoid) and inject them into codebase_context.md. Clears processed "
         "planner notes. Call this before shutting down or after major milestones "
         "so insights survive across runs. NEVER stores rejected code — only "
         "high-level lessons."),
    ]),
    ("Planner  [planner.py]", [
        ("addTask(text)",
         "Add a task with status 'todo'. Returns assigned task id."),
        ("listTasks()",
         "Print all tasks with id, status marker [ ] [~] [x], and text."),
        ("markTaskDone(task_id)",
         "Mark a task as done."),
        ("markTaskInProgress(task_id)",
         "Mark a task as in_progress."),
        ("markTaskTodo(task_id)",
         "Reset a task back to todo."),
        ("removeTask(task_id)",
         "Delete a task by id."),
        ("clearDoneTasks()",
         "Remove all tasks with status done."),
        ("clearAllTasks()",
         "Remove every task."),
        ("addNote(text)",
         "Append a free-form note. Returns assigned note id."),
        ("listNotes()",
         "Print all notes with id and text."),
        ("removeNote(note_id)",
         "Delete a note by id."),
        ("clearNotes()",
         "Remove all notes."),
        ("showBoard()",
         "Print tasks and notes together in one view."),
        ("resetBoard()",
         "Wipe all tasks and notes."),
    ]),
    ("Command Execution  [executor.py]", [
        ("extractCommands(text)",
         "Parse all ```json command blocks from model response text. Returns list of dicts."),
        ("validateCommand(cmd, tool_map)",
         "Check tool name is in whitelist and args are safe. Raises ExecutorError on failure."),
        ("executeCommand(cmd, tool_map)",
         "Validate and call a single command dict. Returns {tool, result, error}."),
        ("executeAll(text, tool_map)",
         "Full pipeline: extract -> validate -> execute all commands in model response. Returns list of results."),
        ("buildToolMap(gf, planner, main=None)",
         "Assemble the whitelist dict of allowed tool names -> callables. Pass main module to include build/PR tools."),
    ]),
]

# IMPORTANT: keep this function up to date
def apiHelp(returnString=False, context=None):
    lines = ["Available API  (all functions accept optional context=[] to record tool use)\n"]
    for section, entries in _API:
        lines.append(f"[{section}]")
        for sig, desc in entries:
            lines.append(f"  {sig}")
            lines.append(f"      {desc}")
        lines.append("")
    output = '\n'.join(lines)
    if context is not None:
        context.append({"type": "tool_use", "tool": "apiHelp", "input": None, "output": output})
    if returnString:
        return output
    print(output)


if __name__ == '__main__':
    args = sys.argv[1:]
    target = args[0] if args and not args[0].startswith('--') else 'RayTraceRowFunc'

    init()

    depth = 1
    if '--depth' in args:
        idx = args.index('--depth')
        if idx + 1 < len(args):
            depth = int(args[idx + 1])

    if '--list' in args:
        listFunctions()
    elif '--restore-all' in args:
        restoreAll()
    elif '--restore' in args:
        restoreFunction(target)
    elif '--diff' in args:
        getDiff()
    elif '--callers' in args:
        getCallers(target)
    elif '--def' in args:
        getDefinition(target)
    elif '--replace' in args:
        idx = args.index('--replace')
        if idx + 1 >= len(args):
            print("Usage: python getFunc.py FunctionName --replace new_impl.c")
            sys.exit(1)
        with open(args[idx + 1]) as fh:
            new_def = fh.read()
        applyChange(target, new_def)
    elif '--replace-lines' in args:
        idx = args.index('--replace-lines')
        if idx + 4 >= len(args):
            print("Usage: python getFunc.py --replace-lines FILE START END replacement.c")
            sys.exit(1)
        with open(args[idx + 4]) as fh:
            new_text = fh.read()
        replaceLines(args[idx + 1], int(args[idx + 2]), int(args[idx + 3]), new_text)
    elif '--patch' in args:
        import json
        idx = args.index('--patch')
        if idx + 1 >= len(args):
            print("Usage: python getFunc.py --patch patches.json")
            sys.exit(1)
        with open(args[idx + 1]) as fh:
            patches = json.load(fh)
        applyPatch(patches)
    elif '--meta' in args:
        import json
        result = showContextWithMeta(target, depth=depth)
        if result:
            print(json.dumps(result, indent=2))
        else:
            print(f"Function '{target}' not found.")
    elif '--src-pair' in args:
        idx = args.index('--src-pair')
        if idx + 1 >= len(args):
            print("Usage: python getFunc.py --src-pair path/to/file.c")
            sys.exit(1)
        showSrcPair(args[idx + 1])
    elif '--src' in args:
        idx = args.index('--src')
        if idx + 1 >= len(args):
            print("Usage: python getFunc.py --src path/to/file.c")
            sys.exit(1)
        showSrc(args[idx + 1])
    elif '--hot-func' in args:
        idx = args.index('--hot-func')
        if idx + 1 >= len(args):
            print("Usage: python getFunc.py --hot-func FunctionName [--threshold N]")
            sys.exit(1)
        threshold = 0.5
        if '--threshold' in args:
            tidx = args.index('--threshold')
            if tidx + 1 < len(args):
                threshold = float(args[tidx + 1])
        print(hotAnnotateFunc(args[idx + 1], threshold=threshold))
    elif '--hot-file' in args:
        idx = args.index('--hot-file')
        if idx + 1 >= len(args):
            print("Usage: python getFunc.py --hot-file path/to/file.c [--threshold N]")
            sys.exit(1)
        threshold = 0.5
        if '--threshold' in args:
            tidx = args.index('--threshold')
            if tidx + 1 < len(args):
                threshold = float(args[tidx + 1])
        print(hotAnnotateFile(args[idx + 1], threshold=threshold))
    elif '--help-api' in args:
        apiHelp()
    else:
        showContext(target, depth=depth)
