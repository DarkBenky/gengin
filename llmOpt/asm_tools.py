"""Godbolt-style assembly inspection for the gengin sandbox.

Compiles one translation unit with the project's codegen flags and returns
Intel-syntax assembly (`showAsm`) or a normalized current-vs-baseline diff
(`compareAsm`).  This is per-TU `clang -S` output: LTO and PGO flags are
stripped, so it approximates rather than reproduces the final linked binary.
"""

import difflib
import json
import os
import re
import shlex
import shutil
import subprocess
import tempfile

MAX_ASM_BYTES = 200_000
MAX_DIFF_BYTES = 200_000
COMPILE_TIMEOUT = 120

_DROP_PREFIXES = (
    ".file", ".ident", ".section", ".type", ".size", ".globl",
    ".cfi_", ".loc", ".addrsig", ".llvm_addrsig", ".text", ".data",
)
_FUNC_LABEL_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_.$]*):\s*$", re.MULTILINE)
_FUNC_TYPE_RE = re.compile(r"^\s*\.type\s+([^,\s]+)\s*,\s*@function", re.MULTILINE)
_LOCAL_LABEL_RE = re.compile(r"\.L[A-Za-z0-9_.]+")
_ADDRESS_RE = re.compile(r"^[0-9a-f]+:\s+")
_DB_SEPARATE_FLAGS = (
    "-I", "-isystem", "-iquote", "-idirafter", "-D", "-U", "-include",
    "-imacros", "-target", "--target", "-isysroot", "--sysroot", "-B",
    "-resource-dir", "-x",
)
_DB_JOINED_FLAGS = (
    "-I", "-isystem", "-iquote", "-idirafter", "-D", "-U", "-std=",
    "-target", "--target", "-isysroot", "--sysroot", "-B",
    "-resource-dir", "-x", "-pthread", "-include", "-imacros",
)
_COMPILER_WRAPPERS = ("ccache", "sccache", "distcc", "icecc")


class AsmError(RuntimeError):
    pass


def _cap(text, limit):
    if len(text) <= limit:
        return text, False
    return text[:limit] + "\n... (truncated)", True


def _dedupe(items):
    seen = set()
    out = []
    for item in items:
        if item not in seen:
            seen.add(item)
            out.append(item)
    return out


def _makefile_cflags(project_dir):
    """Extract CFLAGS_BASE from the Makefile, expanding simple $(VAR) refs."""
    path = os.path.join(project_dir, "Makefile")
    try:
        with open(path, encoding="utf-8-sig", errors="replace") as handle:
            text = handle.read()
    except OSError:
        return []
    text = text.replace("\r\n", "\n").replace("\\\n", " ")
    variables = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        match = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\s*(\+=|\?=|:=|=)\s*(.*)$", line)
        if not match:
            continue
        name, op, value = match.groups()
        if op == "+=":
            variables[name] = variables.get(name, "") + " " + value
        else:
            variables[name] = value
    flags = variables.get("CFLAGS_BASE", "")
    for _ in range(8):
        expanded = re.sub(
            r"\$[({]([A-Za-z_][A-Za-z0-9_]*)[)}]",
            lambda m: variables.get(m.group(1), m.group(0)),
            flags,
        )
        if expanded == flags:
            break
        flags = expanded
    try:
        return shlex.split(flags, comments=True)
    except ValueError:
        return []


def _load_db(project_dir):
    path = os.path.join(project_dir, "compile_commands.json")
    try:
        with open(path, encoding="utf-8-sig") as handle:
            entries = json.load(handle)
    except (OSError, ValueError):
        return []
    if not isinstance(entries, list):
        return []
    return [entry for entry in entries if isinstance(entry, dict)]


def _entry_directory(entry, project_dir):
    directory = entry.get("directory") or project_dir
    if not os.path.isabs(directory):
        directory = os.path.join(project_dir, directory)
    return directory


def _db_entry(project_dir, source):
    target = os.path.realpath(source)
    for entry in _load_db(project_dir):
        directory = _entry_directory(entry, project_dir)
        name = entry.get("file") or ""
        full = name if os.path.isabs(name) else os.path.join(directory, name)
        try:
            resolved = os.path.realpath(full)
        except (OSError, ValueError):
            continue
        if resolved == target:
            return entry
    return None


def _absolute_include(value, directory):
    if os.path.isabs(value) or value.startswith("="):
        return value
    return os.path.join(directory, value)


def _db_include_flags(project_dir):
    """Collect include paths from every DB entry for files missing an entry."""
    flags = []
    for entry in _load_db(project_dir):
        directory = _entry_directory(entry, project_dir)
        args = _entry_args(entry)
        index = 0
        while index < len(args):
            arg = args[index]
            if arg in ("-I", "-isystem", "-iquote", "-idirafter"):
                if index + 1 < len(args):
                    flags.append(arg + _absolute_include(args[index + 1], directory))
                    index += 2
                    continue
            for prefix in ("-I", "-isystem", "-iquote", "-idirafter"):
                if arg.startswith(prefix) and len(arg) > len(prefix):
                    flags.append(prefix + _absolute_include(arg[len(prefix):], directory))
                    break
            index += 1
    return _dedupe(flags)


def _entry_args(entry):
    if not entry:
        return []
    arguments = entry.get("arguments")
    if isinstance(arguments, list):
        return [str(item) for item in arguments]
    command = entry.get("command") or ""
    try:
        return shlex.split(command)
    except ValueError:
        return []


def _select_db_flags(args, directory=None):
    """Keep include/define/standard flags; drop codegen flags from the DB."""
    path_flags = ("-I", "-isystem", "-iquote", "-idirafter", "-include",
                  "-imacros")
    flags = []
    index = 1
    while index < len(args):
        arg = args[index]
        if arg in _DB_SEPARATE_FLAGS:
            if index + 1 < len(args):
                value = args[index + 1]
                if directory and arg in path_flags:
                    value = _absolute_include(value, directory)
                flags.extend([arg, value])
                index += 2
                continue
        if arg.startswith(_DB_JOINED_FLAGS):
            if directory:
                for prefix in path_flags:
                    if arg.startswith(prefix) and len(arg) > len(prefix):
                        arg = prefix + _absolute_include(arg[len(prefix):], directory)
                        break
            flags.append(arg)
        elif arg.startswith("-m") and not arg.startswith("-mllvm"):
            flags.append(arg)
        index += 1
    return flags


def _filter_flags(flags, compiler):
    out = []
    index = 0
    gcc = "clang" not in os.path.basename(compiler)
    while index < len(flags):
        flag = flags[index]
        if flag in ("-c", "-S", "-E"):
            index += 1
            continue
        if flag in ("-o", "-MF", "-MT", "-MQ", "-MJ"):
            index += 2
            continue
        if flag.startswith(("-flto", "-fprofile-use", "-fprofile-generate",
                            "-fprofile-instr", "-fprofile-correction",
                            "-fprofile-arcs", "-ftest-coverage",
                            "-fcoverage-mapping", "-fprofile-sample-use",
                            "-fauto-profile")):
            index += 1
            continue
        if flag.startswith(("-MD", "-MMD", "-MF", "-MT", "-MQ", "-MJ")):
            index += 1
            continue
        if flag.startswith(("-save-temps", "-ftime-trace", "-fdump-",
                            "-fstack-usage", "-fcallgraph-info")):
            index += 1
            continue
        if gcc and flag == "-mllvm":
            index += 2
            continue
        if gcc and flag in ("-fvectorize", "-fslp-vectorize"):
            index += 1
            continue
        out.append(flag)
        index += 1
    return out


def _db_optimization(args):
    optimization = None
    for arg in args[1:]:
        if arg.startswith("-O") and arg not in ("-ObjC", "-ObjC++"):
            optimization = arg
    return optimization


def _resolve_compiler(project_dir, entry, source):
    args = _entry_args(entry)
    index = 0
    while (index < len(args)
           and os.path.basename(args[index]) in _COMPILER_WRAPPERS):
        index += 1
    default = "clang++" if source.endswith((".cpp", ".cc", ".cxx")) else "clang"
    compiler = args[index] if index < len(args) else default
    path = shutil.which(compiler)
    if path is None and os.sep in compiler:
        candidate = compiler if os.path.isabs(compiler) else os.path.join(project_dir, compiler)
        if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            path = candidate
    if path is None:
        raise AsmError(f"compiler not found: {compiler}")
    return path


def _compile_flags(project_dir, source, entry, extra_flags):
    compiler = _resolve_compiler(project_dir, entry, source)
    args = _entry_args(entry)
    base_flags = _makefile_cflags(project_dir)
    flags = list(base_flags)
    if entry is None:
        flags += _db_include_flags(project_dir)
    else:
        flags += _select_db_flags(args, _entry_directory(entry, project_dir))
        if not any(flag.startswith("-O") and not flag.startswith("-ObjC")
                   for flag in base_flags):
            optimization = _db_optimization(args)
            if optimization:
                flags.append(optimization)
    try:
        flags += shlex.split(extra_flags or "")
    except ValueError as exc:
        raise AsmError(f"invalid extra_flags: {exc}") from exc
    flags = _filter_flags(_dedupe(flags), compiler)
    flags = [f"-I{project_dir}", f"-I{project_dir}/object"] + flags
    return compiler, _dedupe(flags)


def _run_compile(project_dir, compiler, flags, source, output):
    command = [compiler] + flags + [
        "-S", "-masm=intel", "-fno-asynchronous-unwind-tables",
        "-fno-verbose-asm",
    ]
    if "clang" in os.path.basename(compiler):
        command.append("-fno-color-diagnostics")
    command += ["-o", output, source]
    try:
        result = subprocess.run(
            command, cwd=project_dir, capture_output=True, text=True,
            errors="replace", timeout=COMPILE_TIMEOUT,
        )
    except subprocess.TimeoutExpired as exc:
        raise AsmError(f"compiler timed out after {COMPILE_TIMEOUT}s") from exc
    except (OSError, ValueError) as exc:
        raise AsmError(f"cannot run compiler: {exc}") from exc
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or "").strip()
        raise AsmError(f"compiler failed:\n{detail[:4000]}")
    try:
        with open(output, encoding="utf-8", errors="replace") as handle:
            return handle.read()
    except OSError as exc:
        raise AsmError(f"cannot read assembly output: {exc}") from exc


def _project_root(project_dir):
    try:
        return os.path.realpath(project_dir)
    except (OSError, ValueError) as exc:
        raise AsmError(f"invalid project directory: {project_dir}") from exc


def _resolve_source(project_dir, rel_path):
    root = os.path.realpath(project_dir)
    if not rel_path or os.path.isabs(rel_path):
        raise AsmError("rel_path must be a path relative to the sandbox")
    try:
        path = os.path.realpath(os.path.join(root, rel_path))
        inside = os.path.commonpath([root, path]) == root
    except (OSError, ValueError) as exc:
        raise AsmError(f"invalid source path: {rel_path}") from exc
    if not inside:
        raise AsmError("rel_path escapes the sandbox")
    if not os.path.isfile(path):
        raise AsmError(f"source file not found: {rel_path}")
    return path


def _function_names(text):
    types = _FUNC_TYPE_RE.findall(text)
    if types:
        return sorted(set(types))
    names = [match.group(1) for match in _FUNC_LABEL_RE.finditer(text)]
    return sorted({name for name in names if not name.startswith(".L")})


def _extract_function(text, name):
    lines = text.splitlines()
    start = None
    for index, line in enumerate(lines):
        if re.match(rf"^{re.escape(name)}:\s*$", line):
            start = index
            break
    if start is None:
        return None
    size_re = re.compile(rf"^\s*\.size\s+{re.escape(name)}\s*,")
    end = len(lines)
    for index in range(start + 1, len(lines)):
        if size_re.match(lines[index]):
            end = index + 1
            break
        match = _FUNC_LABEL_RE.match(lines[index])
        if match and not match.group(1).startswith(".L"):
            end = index
            break
    return "\n".join(lines[start:end])


def _strip_comment(line):
    quote = None
    escaped = False
    for index, char in enumerate(line):
        if quote:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                quote = None
            continue
        if char in "\"'":
            quote = char
        elif char == "#" and (index == 0 or line[index - 1].isspace()):
            return line[:index].rstrip()
    return line


def _collapse_whitespace(line):
    out = []
    quote = None
    escaped = False
    for char in line:
        if quote:
            out.append(char)
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                quote = None
        elif char in "\"'":
            quote = char
            out.append(char)
        elif char.isspace():
            if out and out[-1] != " ":
                out.append(" ")
        else:
            out.append(char)
    return "".join(out).strip()


def _map_labels(line, mapping):
    out = []
    quote = None
    escaped = False
    index = 0
    while index < len(line):
        char = line[index]
        if quote:
            out.append(char)
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                quote = None
            index += 1
            continue
        if char in "\"'":
            quote = char
            out.append(char)
            index += 1
            continue
        match = _LOCAL_LABEL_RE.match(line, index)
        if match:
            out.append(mapping.get(match.group(0), ".LC"))
            index = match.end()
            continue
        out.append(char)
        index += 1
    return "".join(out)


def _normalize(text):
    lines = []
    mapping = {}
    in_text = True
    section_stack = []
    for raw in text.splitlines():
        line = _strip_comment(raw.strip()).strip()
        if not line:
            continue
        if line.startswith(".pushsection"):
            section_stack.append(in_text)
            in_text = ".text" in line
            continue
        if line == ".popsection":
            if section_stack:
                in_text = section_stack.pop()
            continue
        if line.startswith(".section"):
            in_text = ".text" in line
            continue
        if line in (".text", ".data", ".bss", ".rodata"):
            in_text = line == ".text"
            continue
        if line.startswith(_DROP_PREFIXES):
            continue
        if re.match(r"^\.Lfunc_(?:end|begin)\d+:$", line):
            continue
        line = _ADDRESS_RE.sub("", line)
        line = _collapse_whitespace(line)
        if in_text:
            match = re.match(r"^(\.L[A-Za-z0-9_.]+):$", line)
            if match and match.group(1) not in mapping:
                mapping[match.group(1)] = f".LL{len(mapping) + 1}"
        lines.append(line)
    return [_map_labels(line, mapping) for line in lines]


def _instruction_count(lines):
    return sum(1 for line in lines if not line.startswith(".") and not line.endswith(":"))


def _stats(base_lines, current_lines, diff_lines):
    added = sum(1 for line in diff_lines
                if line.startswith("+") and not line.startswith("+++"))
    removed = sum(1 for line in diff_lines
                  if line.startswith("-") and not line.startswith("---"))
    return {
        "base_instructions": _instruction_count(base_lines),
        "current_instructions": _instruction_count(current_lines),
        "instruction_delta": _instruction_count(current_lines) - _instruction_count(base_lines),
        "base_vector_instructions": sum(1 for line in base_lines
                                        if not line.endswith(":")
                                        and re.match(r"^v[a-z]", line)),
        "current_vector_instructions": sum(1 for line in current_lines
                                           if not line.endswith(":")
                                           and re.match(r"^v[a-z]", line)),
        "base_branch_instructions": sum(1 for line in base_lines
                                        if not line.endswith(":")
                                        and re.match(r"^(j\w+|call\w*|ret\w*|loop\w*)\b", line)),
        "current_branch_instructions": sum(1 for line in current_lines
                                           if not line.endswith(":")
                                           and re.match(r"^(j\w+|call\w*|ret\w*|loop\w*)\b", line)),
        "added_lines": added,
        "removed_lines": removed,
    }


def _notes():
    return [
        "per-TU -S output; -flto/-fprofile flags are stripped, so this is not "
        "the final LTO-linked binary",
        "flags merge Makefile CFLAGS_BASE with the compile_commands.json "
        "include/define flags",
    ]


def showAsm(project_dir, rel_path, func="", extra_flags=""):
    """Return the assembly for one file or function."""
    project_dir = _project_root(project_dir)
    source = _resolve_source(project_dir, rel_path)
    entry = _db_entry(project_dir, source)
    compiler, flags = _compile_flags(project_dir, source, entry, extra_flags)
    with tempfile.TemporaryDirectory(prefix="gengin-asm-") as temp_dir:
        output = os.path.join(temp_dir, "output.s")
        text = _run_compile(project_dir, compiler, flags, source, output)
    functions = _function_names(text)
    notes = _notes()
    if entry is None:
        notes.append("no compile_commands.json entry; used Makefile flags, "
                     "DB include paths, and the default compiler")
    if not functions:
        notes.append("translation unit emitted no functions")
    result = {
        "file": rel_path,
        "function": func,
        "compiler": compiler,
        "flags": flags,
        "functions": functions,
        "notes": notes,
    }
    if func:
        body = _extract_function(text, func)
        if body is None:
            result["error"] = (
                f"function {func!r} was not emitted (inlined or optimized away)")
            return result
        body, truncated = _cap(body, MAX_ASM_BYTES)
        result["asm"] = body
        result["line_count"] = len(body.splitlines())
        result["truncated"] = truncated
        return result
    text, truncated = _cap(text, MAX_ASM_BYTES)
    result["asm"] = text
    result["line_count"] = len(text.splitlines())
    result["truncated"] = truncated
    return result


def compareAsm(project_dir, rel_path, func="", base_ref="HEAD", extra_flags=""):
    """Return a normalized current-vs-baseline assembly diff."""
    project_dir = _project_root(project_dir)
    source = _resolve_source(project_dir, rel_path)
    if not base_ref or base_ref.startswith("-"):
        raise AsmError("base_ref must be a git ref, not an option")
    entry = _db_entry(project_dir, source)
    compiler, flags = _compile_flags(project_dir, source, entry, extra_flags)
    git_path = os.path.relpath(source, project_dir).replace(os.sep, "/")
    try:
        baseline = subprocess.run(
            ["git", "show", f"{base_ref}:{git_path}"],
            cwd=project_dir, capture_output=True, text=True,
            errors="replace", timeout=30,
        )
    except (OSError, ValueError, subprocess.TimeoutExpired) as exc:
        raise AsmError(f"cannot read baseline {base_ref}:{rel_path}: {exc}") from exc
    if baseline.returncode != 0:
        detail = (baseline.stderr or baseline.stdout or "").strip()
        raise AsmError(f"cannot read baseline {base_ref}:{rel_path}:\n{detail[:2000]}")
    with tempfile.TemporaryDirectory(prefix="gengin-asm-") as temp_dir:
        suffix = os.path.splitext(os.path.basename(source))[1]
        base_source = os.path.join(temp_dir, "baseline_input" + suffix)
        with open(base_source, "w", encoding="utf-8") as handle:
            handle.write(baseline.stdout)
        base_flags = ["-iquote", os.path.dirname(source)] + flags
        base_output = os.path.join(temp_dir, "baseline_output.s")
        base_asm = _run_compile(project_dir, compiler, base_flags, base_source, base_output)
        current_output = os.path.join(temp_dir, "current_output.s")
        current_asm = _run_compile(project_dir, compiler, flags, source, current_output)
    base_functions = _function_names(base_asm)
    current_functions = _function_names(current_asm)
    notes = _notes() + [
        "baseline source comes from git show; headers come from the current tree",
    ]
    if entry is None:
        notes.append("no compile_commands.json entry; used Makefile flags, "
                     "DB include paths, and the default compiler")
    if not current_functions:
        notes.append("translation unit emitted no functions")
    result = {
        "file": rel_path,
        "function": func,
        "base_ref": base_ref,
        "compiler": compiler,
        "flags": flags,
        "base_functions": base_functions,
        "functions": current_functions,
        "notes": notes,
    }
    if func:
        base_body = _extract_function(base_asm, func)
        current_body = _extract_function(current_asm, func)
        if base_body is None or current_body is None:
            missing = []
            if base_body is None:
                missing.append(f"baseline ({base_ref})")
            if current_body is None:
                missing.append("current tree")
            result["error"] = (
                f"function {func!r} was not emitted in " + " and ".join(missing))
            return result
    else:
        base_body, current_body = base_asm, current_asm
    base_lines = _normalize(base_body)
    current_lines = _normalize(current_body)
    diff_lines = list(difflib.unified_diff(
        base_lines, current_lines,
        fromfile=f"base:{base_ref}", tofile="current", n=3, lineterm="",
    ))
    diff, truncated = _cap("\n".join(diff_lines), MAX_DIFF_BYTES)
    result["diff"] = diff
    result["stats"] = _stats(base_lines, current_lines, diff_lines)
    result["truncated"] = truncated
    return result
