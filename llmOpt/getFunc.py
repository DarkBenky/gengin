"""getFunc.py — regex-based C function/struct index and perf annotation.

Serves the gengin MCP server.  Editing, line-range, search, and navigation
helpers were removed — the driving harness (Hermes Agent) owns file edits and
code search, and clangd (via lsp_client.py) owns semantic navigation.
"""

import os
import re
import subprocess

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
            entry = {'full': full.strip(), 'file': os.path.relpath(filepath, GENGIN),
                     'start': start_line, 'end': end_line}
            results[key] = entry
            if struct_tag and struct_tag != key:
                results[struct_tag] = entry
    return results


# --- Public API ---

def refreshFile(rel_path):
    """Re-index a single file (the harness may have edited it since init)."""
    filepath = os.path.join(GENGIN, rel_path)
    if not os.path.exists(filepath):
        return False
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
    return True


def restoreAll():
    """git checkout HEAD -- . in the sandbox.  Returns True on success."""
    result = subprocess.run(
        ['git', 'checkout', 'HEAD', '--', '.'],
        cwd=GENGIN, capture_output=True, text=True,
    )
    return result.returncode == 0


# --- Perf hot-line annotation ---

_ANNOT_LINE_RE = re.compile(r'^\s*([\d.]+)\s*:\s*([0-9a-f]+):\s')


def _perfAnnotateFunc(func_name, cwd=None):
    """Return {source_lineno: pct} for func_name using perf annotate + addr2line.

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
            capture_output=True, text=True, cwd=perf_cwd, timeout=30,
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
            capture_output=True, text=True, timeout=30,
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


def hotAnnotateFunc(func_name, threshold=0.5):
    """Return func_name's source annotated with per-line perf percentages.

    Lines consuming >= threshold% of samples are prefixed with /* HOT X.X% */.
    Requires perf.data from a previous makeFlame() run.
    """
    info = _functions.get(func_name)
    if info is None:
        return f"Function '{func_name}' not found in codebase index."
    refreshFile(info['file'])
    info = _functions.get(func_name)
    if info is None:
        return f"Function '{func_name}' not found after re-index (renamed or removed?)."

    filepath = next(
        (p for p in (_sources or {}) if os.path.relpath(p, GENGIN) == info['file']),
        None,
    )
    if filepath is None:
        return f"Source file not found for '{func_name}'."

    hotness = _perfAnnotateFunc(func_name)

    with open(filepath, errors='replace') as fh:
        all_lines = fh.read().splitlines()

    func_lines = all_lines[info['start'] - 1:info['end']]
    annotated = _annotateLines(func_lines, info['start'], hotness, threshold)

    header = (
        f"// hotAnnotateFunc: {func_name}  [{info['file']}:{info['start']}-{info['end']}]\n"
        f"// threshold={threshold}%  |  /* HOT X.X% */ marks hot lines\n"
    )
    suffix = "" if hotness else "\n// NOTE: no perf.data or perf annotate failed -- run make_flame() first."
    return header + '\n'.join(annotated) + suffix


def hotAnnotateFile(rel_path, threshold=0.5):
    """Return an entire file annotated with merged per-line hotness for every
    function in it.  Requires perf.data from a previous makeFlame() run."""
    refreshFile(rel_path)
    filepath = os.path.join(GENGIN, rel_path)
    try:
        with open(filepath, errors='replace') as fh:
            all_lines = fh.read().splitlines()
    except FileNotFoundError:
        return f"File not found: {rel_path}"

    file_funcs = [name for name, data in _functions.items() if data['file'] == rel_path]
    combined = {}
    for fn in file_funcs:
        for lineno, pct in _perfAnnotateFunc(fn).items():
            combined[lineno] = combined.get(lineno, 0.0) + pct

    annotated = _annotateLines(all_lines, 1, combined, threshold)
    header = (
        f"// hotAnnotateFile: {rel_path}\n"
        f"// threshold={threshold}%  |  /* HOT X.X% */ marks hot lines\n"
    )
    suffix = "" if combined else "\n// NOTE: no perf.data or no matching functions -- run make_flame() first."
    return header + '\n'.join(annotated) + suffix
