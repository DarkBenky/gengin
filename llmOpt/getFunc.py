"""
getFunc.py -- regex-based C context extractor for LLM prompts.

Usage:  python getFunc.py [FunctionName] [--depth N]
        [--list|--diff|--callers|--def|--restore|--restore-all|--replace file.c]
        [--replace-lines FILE START END new_impl.c|--patch patches.json|--meta]
        [--src path/to/file.c|--src-pair path/to/file.c|--help-api]
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


def replaceLines(rel_path, start, end, new_text, context=None):
    filepath = os.path.join(GENGIN, rel_path)
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
    ]),
    ("Applying Changes", [
        ("applyChange(func_name, new_definition)",
         "Replace a named function in-place by locating its signature in the source file."),
        ("replaceLines(rel_path, start, end, new_text)",
         "Replace lines start..end (1-indexed inclusive) with new_text. Pairs with showSrc line numbers."),
        ("applyPatch(patches)",
         "Batch replacement. patches = [{file, start, end, text}, ...]. Applied in reverse line order per file."),
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
    elif '--help-api' in args:
        apiHelp()
    else:
        showContext(target, depth=depth)
