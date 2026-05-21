"""
getFunc.py -- regex-based C context extractor for LLM prompts.

Usage:  python getFunc.py [FunctionName] [--depth N] [--list|--diff|--callers|--def|--restore|--restore-all|--replace file.c]
"""

import os
import re
import subprocess
import sys

GENGIN = os.path.join(os.path.dirname(os.path.abspath(__file__)), "gengin")

_SKIP = {
    'if', 'for', 'while', 'switch', 'do', 'else', 'return',
    'sizeof', 'typeof', '__typeof__', 'alignof', 'offsetof',
}

_FUNC_RE = re.compile(
    r'^((?:(?:static|inline|extern|const|unsigned|signed|void|struct)\s+)*'
    r'[\w\s\*]+?)\s+(\w+)\s*\(([^;{]*?)\)\s*\{',
    re.MULTILINE,
)

_STRUCT_RE = re.compile(r'(typedef\s+)?struct\s+(\w*)\s*\{', re.MULTILINE)


# --- Source parsing (private) ---

def _read_sources(base_dir):
    sources = {}
    for root, dirs, files in os.walk(base_dir):
        for f in files:
            if f.endswith(('.c', '.h')):
                path = os.path.join(root, f)
                with open(path, errors='replace') as fh:
                    sources[path] = fh.read()
    return sources


def _strip_comments(text):
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.DOTALL)
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
            results[name] = {
                'sig': sig,
                'body': body,
                'full': sig + '\n' + body,
                'file': os.path.relpath(filepath, GENGIN),
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
            entry = {'full': full.strip(), 'file': os.path.relpath(filepath, GENGIN)}
            results[key] = entry
            if struct_tag and struct_tag != key:
                results[struct_tag] = entry
    return results


# --- Public API ---

def showContext(target_func, functions, structs, depth=1, returnString=False):
    if target_func not in functions:
        print(f"Function '{target_func}' not found in codebase.")
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

    _collect(target_func, 0)

    target = functions[target_func]
    lines = []
    lines.append(f"// {'=' * 60}")
    lines.append(f"// TARGET: {target_func}  [{target['file']}]")
    lines.append(f"// {'=' * 60}")
    lines.append(target['full'])

    callees = {n: functions[n] for n in (visited - {target_func})}
    if callees:
        lines.append(f"\n// {'=' * 60}")
        lines.append(f"// CALLED FUNCTIONS (depth={depth})")
        lines.append(f"// {'=' * 60}")
        for fname, fdata in sorted(callees.items()):
            lines.append(f"\n// --- {fname}  [{fdata['file']}] ---")
            lines.append(fdata['full'])

    found_structs = {t: structs[t] for t in all_types if t in structs}
    if found_structs:
        lines.append(f"\n// {'=' * 60}")
        lines.append(f"// USED STRUCTS / TYPES")
        lines.append(f"// {'=' * 60}")
        for tname, tdata in sorted(found_structs.items()):
            lines.append(f"\n// --- {tname}  [{tdata['file']}] ---")
            lines.append(tdata['full'])

    output = '\n'.join(lines)
    CONTEXT.append({
        "type": "tool_use",
        "tool": "showContext",
        "input": {"func": target_func, "depth": depth},
        "output": output,
    })
    if returnString:
        return output
    print(output)


def getDefinition(name, functions, structs, returnString=False):
    lines = []
    if name in functions:
        fdata = functions[name]
        lines.append(f"// --- function: {name}  [{fdata['file']}] ---")
        lines.append(fdata['full'])
    if name in structs:
        sdata = structs[name]
        lines.append(f"// --- struct/type: {name}  [{sdata['file']}] ---")
        lines.append(sdata['full'])

    output = '\n'.join(lines) if lines else f"'{name}' not found in functions or structs."
    CONTEXT.append({
        "type": "tool_use",
        "tool": "getDefinition",
        "input": name,
        "output": output,
    })
    if returnString:
        return output
    print(output)


def getCallers(target_func, functions, returnString=False):
    callers = {
        name: data for name, data in functions.items()
        if target_func in _called_functions(data['body'])
    }
    lines = [f"// Callers of '{target_func}':"]
    if callers:
        for fname, fdata in sorted(callers.items()):
            lines.append(f"//   {fname}  [{fdata['file']}]")
    else:
        lines.append("//   (none found)")

    output = '\n'.join(lines)
    CONTEXT.append({
        "type": "tool_use",
        "tool": "getCallers",
        "input": target_func,
        "output": output,
    })
    if returnString:
        return output
    print(output)


def getDiff(returnString=False):
    result = subprocess.run(
        ['git', 'diff', 'HEAD'],
        cwd=GENGIN, capture_output=True, text=True,
    )
    output = result.stdout or "(no changes)"
    CONTEXT.append({
        "type": "tool_use",
        "tool": "getDiff",
        "input": None,
        "output": output,
    })
    if returnString:
        return output
    print(output)


def readSourceFile(rel_path, returnString=False):
    filepath = os.path.join(GENGIN, rel_path)
    try:
        with open(filepath, errors='replace') as fh:
            output = fh.read()
    except FileNotFoundError:
        output = f"File not found: {rel_path}"
    CONTEXT.append({
        "type": "tool_use",
        "tool": "readSourceFile",
        "input": rel_path,
        "output": output,
    })
    if returnString:
        return output
    print(output)


def listFunctions(functions, returnString=False):
    entries = sorted(
        [{'name': name, 'file': data['file'], 'sig': data['sig']}
         for name, data in functions.items()],
        key=lambda x: (x['file'], x['name']),
    )
    lines = [f"{e['file']:<45}  {e['name']}" for e in entries]
    output = '\n'.join(lines)
    CONTEXT.append({
        "type": "tool_use",
        "tool": "listFunctions",
        "input": None,
        "output": output,
    })
    if returnString:
        return output
    print(output)
    return entries


def applyChange(func_name, new_definition, functions, sources):
    if func_name not in functions:
        CONTEXT.append({"type": "tool_use", "tool": "applyChange",
                        "input": func_name, "output": f"failed: '{func_name}' not found"})
        return False

    info = functions[func_name]
    filepath = next((p for p in sources if os.path.relpath(p, GENGIN) == info['file']), None)
    if filepath is None:
        CONTEXT.append({"type": "tool_use", "tool": "applyChange",
                        "input": func_name, "output": f"failed: source file not found"})
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
        CONTEXT.append({"type": "tool_use", "tool": "applyChange",
                        "input": func_name, "output": f"failed: could not locate '{func_name}' in {info['file']}"})
        return False

    brace_pos = match.start() + match.group(0).rfind('{')
    old_block = _extract_block(original_text, brace_pos)
    old_span_start = match.start() if original_text[match.start()] == '\n' else match.start()
    old_span_end = brace_pos + len(old_block)

    new_text = original_text[:old_span_start] + '\n' + new_definition.strip() + '\n' + original_text[old_span_end:]
    with open(filepath, 'w') as fh:
        fh.write(new_text)

    CONTEXT.append({"type": "tool_use", "tool": "applyChange",
                    "input": func_name, "output": f"replaced '{func_name}' in {info['file']}"})
    return True


def restoreAll():
    result = subprocess.run(
        ['git', 'checkout', 'HEAD', '--', '.'],
        cwd=GENGIN, capture_output=True, text=True,
    )
    success = result.returncode == 0
    CONTEXT.append({
        "type": "tool_use",
        "tool": "restoreAll",
        "input": None,
        "output": "restored all files to HEAD" if success else f"failed: {result.stderr.strip()}",
    })
    return success


def restoreFile(rel_path):
    result = subprocess.run(
        ['git', 'checkout', 'HEAD', '--', rel_path],
        cwd=GENGIN, capture_output=True, text=True,
    )
    success = result.returncode == 0
    CONTEXT.append({
        "type": "tool_use",
        "tool": "restoreFile",
        "input": rel_path,
        "output": f"restored {rel_path}" if success else f"failed: {result.stderr.strip()}",
    })
    return success


def restoreFunction(func_name, functions):
    if func_name not in functions:
        CONTEXT.append({"type": "tool_use", "tool": "restoreFunction",
                        "input": func_name, "output": f"failed: '{func_name}' not found"})
        return False
    return restoreFile(functions[func_name]['file'])


if __name__ == '__main__':
    args = sys.argv[1:]
    target = args[0] if args and not args[0].startswith('--') else 'RayTraceRowFunc'

    sources = _read_sources(GENGIN)
    functions = find_functions(sources)
    structs = find_structs(sources)

    depth = 1
    if '--depth' in args:
        idx = args.index('--depth')
        if idx + 1 < len(args):
            depth = int(args[idx + 1])

    if '--list' in args:
        listFunctions(functions)
    elif '--restore-all' in args:
        restoreAll()
    elif '--restore' in args:
        restoreFunction(target, functions)
    elif '--diff' in args:
        getDiff()
    elif '--callers' in args:
        getCallers(target, functions)
    elif '--def' in args:
        getDefinition(target, functions, structs)
    elif '--replace' in args:
        idx = args.index('--replace')
        if idx + 1 >= len(args):
            print("Usage: python getFunc.py FunctionName --replace new_impl.c")
            sys.exit(1)
        with open(args[idx + 1]) as fh:
            new_def = fh.read()
        applyChange(target, new_def, functions, sources)
    else:
        showContext(target, functions, structs, depth=depth)
