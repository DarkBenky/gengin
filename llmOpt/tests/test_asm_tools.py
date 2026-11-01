import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import asm_tools  # noqa: E402


SAMPLE_HEADER = """#ifndef SAMPLE_H
#define SAMPLE_H
#define SCALE 2
#endif
"""

SAMPLE_SOURCE = """#include "sample.h"

int add(int a, int b) {
    return a + b;
}

int scale(int a) {
    return a * SCALE;
}

static inline int helper(int x) {
    return x + 1;
}

int loopSum(const int *values, int count) {
    int total = 0;
    for (int i = 0; i < count; i++) {
        total += values[i];
    }
    return total;
}
"""

MAKEFILE = """MINIFB_DIR = deps/minifb
CFLAGS_BASE = -O2 -fno-inline -w -I$(MINIFB_DIR)/include

all:
\t@true
"""


def _git(directory, *args):
    subprocess.run(
        ["git"] + list(args), cwd=directory, check=True,
        capture_output=True, text=True,
    )


@unittest.skipUnless(shutil.which("clang"), "clang is required")
class AsmToolsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.mkdtemp(prefix="gengin-asm-test-")
        self.addCleanup(shutil.rmtree, self.temp, True)
        self.source = os.path.join(self.temp, "sample.c")
        with open(os.path.join(self.temp, "sample.h"), "w", encoding="utf-8") as handle:
            handle.write(SAMPLE_HEADER)
        with open(self.source, "w", encoding="utf-8") as handle:
            handle.write(SAMPLE_SOURCE)
        with open(os.path.join(self.temp, "Makefile"), "w", encoding="utf-8") as handle:
            handle.write(MAKEFILE)
        commands = [{
            "directory": self.temp,
            "command": f"clang -c -O0 -I{self.temp} -DSAMPLE=1 {self.source}",
            "file": self.source,
        }]
        with open(os.path.join(self.temp, "compile_commands.json"), "w",
                  encoding="utf-8") as handle:
            handle.write(json.dumps(commands))
        _git(self.temp, "init", "-q")
        _git(self.temp, "add", ".")
        _git(self.temp, "-c", "user.name=Asm Test", "-c",
             "user.email=asm@example.com", "commit", "-qm", "baseline")

    def _write(self, rel_path, text):
        path = os.path.join(self.temp, rel_path)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(text)
        return path

    def _commit(self, message):
        _git(self.temp, "add", "-A")
        _git(self.temp, "-c", "user.name=Asm Test", "-c",
             "user.email=asm@example.com", "commit", "-qm", message)

    def test_show_asm_function(self):
        result = asm_tools.showAsm(self.temp, "sample.c", "add")
        self.assertNotIn("error", result)
        self.assertIn("add", result["functions"])
        self.assertIn("add:", result["asm"])
        self.assertIn("lea", result["asm"].lower())
        self.assertIn("-O2", result["flags"])
        self.assertNotIn("-O0", result["flags"])
        self.assertNotIn("-c", result["flags"])
        self.assertNotIn("-flto", " ".join(result["flags"]))
        self.assertTrue(result["compiler"].endswith("clang"))

    def test_show_asm_extra_flags(self):
        result = asm_tools.showAsm(self.temp, "sample.c", "scale", "-DSCALE=7")
        self.assertNotIn("error", result)
        self.assertIn("-DSCALE=7", result["flags"])

    def test_show_asm_missing_function(self):
        result = asm_tools.showAsm(self.temp, "sample.c", "helper")
        self.assertIn("error", result)
        self.assertIn("helper", result["error"])
        self.assertNotIn("asm", result)

    def test_show_asm_rejects_path_traversal(self):
        outside = os.path.join(os.path.dirname(self.temp), "outside.c")
        with self.assertRaises(asm_tools.AsmError):
            asm_tools.showAsm(self.temp, outside)

    def test_show_asm_without_compile_db(self):
        os.remove(os.path.join(self.temp, "compile_commands.json"))
        result = asm_tools.showAsm(self.temp, "sample.c", "add")
        self.assertNotIn("error", result)
        self.assertIn("add:", result["asm"])
        self.assertIn("-fno-inline", result["flags"])
        self.assertTrue(any("no compile_commands.json" in note
                            for note in result["notes"]))

    def test_compare_asm_unchanged(self):
        result = asm_tools.compareAsm(self.temp, "sample.c", "add")
        self.assertNotIn("error", result)
        self.assertEqual(result["diff"], "")
        self.assertEqual(result["stats"]["instruction_delta"], 0)
        self.assertEqual(result["stats"]["added_lines"], 0)
        self.assertEqual(result["stats"]["removed_lines"], 0)

    def test_compare_asm_detects_change(self):
        with open(self.source, "w", encoding="utf-8") as handle:
            handle.write(SAMPLE_SOURCE.replace("return a + b;", "return a - b;"))
        result = asm_tools.compareAsm(self.temp, "sample.c", "add")
        self.assertNotIn("error", result)
        self.assertNotEqual(result["diff"], "")
        self.assertGreater(result["stats"]["added_lines"], 0)
        self.assertGreater(result["stats"]["removed_lines"], 0)

    def test_compare_asm_unknown_ref(self):
        with self.assertRaises(asm_tools.AsmError):
            asm_tools.compareAsm(self.temp, "sample.c", "add", "does-not-exist")

    def test_compare_asm_current_compile_error(self):
        self._write("sample.c", "this is not valid C\n")
        with self.assertRaises(asm_tools.AsmError) as ctx:
            asm_tools.compareAsm(self.temp, "sample.c", "add")
        self.assertIn("compiler failed", str(ctx.exception))

    def test_compare_asm_requires_git_repository(self):
        shutil.rmtree(os.path.join(self.temp, ".git"))
        with self.assertRaises(asm_tools.AsmError):
            asm_tools.compareAsm(self.temp, "sample.c", "add")

    def test_compare_asm_rejects_option_ref(self):
        with self.assertRaises(asm_tools.AsmError):
            asm_tools.compareAsm(self.temp, "sample.c", "add", "--help")

    def test_compare_asm_rejects_nul_ref(self):
        with self.assertRaises(asm_tools.AsmError):
            asm_tools.compareAsm(self.temp, "sample.c", "add", "HEAD\0")

    def test_compare_asm_function_added_after_baseline(self):
        with open(self.source, "a", encoding="utf-8") as handle:
            handle.write("\nint brandNew(int a) {\n    return a - 3;\n}\n")
        result = asm_tools.compareAsm(self.temp, "sample.c", "brandNew")
        self.assertIn("error", result)
        self.assertIn("baseline", result["error"])

    def test_normalize_renumbers_local_labels(self):
        first = "\t.text\n\t.file \"a.c\"\nfoo:\n\tjne\t.LBB0_2\n\tret\n.LBB0_2:\n\tret\n"
        second = "\t.text\n\t.file \"b.c\"\nfoo:\n\tjne\t.LBB1_9\n\tret\n.LBB1_9:\n\tret\n"
        self.assertEqual(asm_tools._normalize(first), asm_tools._normalize(second))

    def test_makefile_flags_expand_variables(self):
        flags = asm_tools._makefile_cflags(self.temp)
        self.assertIn("-O2", flags)
        self.assertIn("-Ideps/minifb/include", flags)

    def test_select_db_flags_keeps_machine_flags(self):
        flags = asm_tools._select_db_flags(
            ["clang", "-c", "-march=x86-64-v3", "-mllvm", "-polly",
             "-I.", "-DNAME=1", "-target", "x86_64-linux-gnu",
             "-iquote", "inc", "-isysroot", "/sdk", "--sysroot=/sys",
             "-B/tools", "-x", "c++", "sample.c"])
        self.assertIn("-march=x86-64-v3", flags)
        self.assertIn("-I.", flags)
        self.assertIn("-DNAME=1", flags)
        self.assertIn("x86_64-linux-gnu", flags)
        self.assertIn("inc", flags)
        self.assertIn("/sdk", flags)
        self.assertIn("--sysroot=/sys", flags)
        self.assertIn("-B/tools", flags)
        self.assertIn("c++", flags)
        self.assertNotIn("-mllvm", flags)

    def test_select_db_flags_absolutizes_relative_paths(self):
        flags = asm_tools._select_db_flags(
            ["clang", "-I", "inc", "-Iother", "-include", "cfg.h",
             "-include", "pre.h", "sample.c"], "/build")
        self.assertIn("/build/inc", flags)
        self.assertIn("-I/build/other", flags)
        self.assertIn("/build/cfg.h", flags)
        self.assertIn("/build/pre.h", flags)

    def test_filter_flags_strips_lto_and_gcc_polly(self):
        flags = asm_tools._filter_flags(
            ["-O3", "-flto", "-c", "-o", "out.o", "-mllvm", "-polly",
             "-fvectorize", "-fprofile-use=x.profdata", "-fprofile-arcs",
             "-ftest-coverage", "-MF", "dep.d", "-MT", "target.o",
             "-save-temps", "-ftime-trace", "-fdump-tree-all"],
            "/usr/bin/gcc")
        self.assertEqual(flags, ["-O3"])

    def test_show_asm_whole_file(self):
        result = asm_tools.showAsm(self.temp, "sample.c")
        self.assertNotIn("error", result)
        self.assertIn("add", result["functions"])
        self.assertIn("add:", result["asm"])
        self.assertEqual(result["function"], "")
        self.assertGreater(result["line_count"], 0)

    def test_show_asm_relative_project_dir(self):
        rel_project = os.path.relpath(self.temp, os.getcwd())
        result = asm_tools.showAsm(rel_project, "sample.c", "add")
        self.assertNotIn("error", result)
        self.assertIn("add:", result["asm"])

    def test_show_asm_empty_translation_unit(self):
        self._write("empty.c", "")
        result = asm_tools.showAsm(self.temp, "empty.c")
        self.assertNotIn("error", result)
        self.assertEqual(result["functions"], [])
        self.assertTrue(any("emitted no functions" in note
                            for note in result["notes"]))
        missing = asm_tools.showAsm(self.temp, "empty.c", "anything")
        self.assertIn("error", missing)

    def test_show_asm_truncates_large_output(self):
        original = asm_tools.MAX_ASM_BYTES
        asm_tools.MAX_ASM_BYTES = 50
        self.addCleanup(setattr, asm_tools, "MAX_ASM_BYTES", original)
        result = asm_tools.showAsm(self.temp, "sample.c", "add")
        self.assertTrue(result["truncated"])
        self.assertTrue(result["asm"].endswith("(truncated)"))

    def test_show_asm_rejects_nul_path(self):
        with self.assertRaises(asm_tools.AsmError):
            asm_tools.showAsm(self.temp, "sample\0.c")

    def test_show_asm_rejects_symlink_escape(self):
        outside = tempfile.NamedTemporaryFile("w", suffix=".c", delete=False)
        outside.write("int outside(void) { return 1; }\n")
        outside.close()
        self.addCleanup(os.unlink, outside.name)
        os.symlink(outside.name, os.path.join(self.temp, "link.c"))
        with self.assertRaises(asm_tools.AsmError):
            asm_tools.showAsm(self.temp, "link.c")

    def test_show_asm_path_with_spaces(self):
        self._write("dir with space/source file.c",
                    "int spaced(int x) { return x + 1; }\n")
        result = asm_tools.showAsm(
            self.temp, "dir with space/source file.c", "spaced")
        self.assertNotIn("error", result)
        self.assertIn("spaced:", result["asm"])

    def test_show_asm_uses_db_optimization_without_makefile_flags(self):
        os.remove(os.path.join(self.temp, "Makefile"))
        commands = [{
            "directory": self.temp,
            "command": f"clang -c -O2 -I{self.temp} {self.source}",
            "file": self.source,
        }]
        self._write("compile_commands.json", json.dumps(commands))
        result = asm_tools.showAsm(self.temp, "sample.c", "add")
        self.assertNotIn("error", result)
        self.assertIn("-O2", result["flags"])

    def test_show_asm_compiler_wrapper(self):
        commands = [{
            "directory": self.temp,
            "command": f"ccache clang -c -I{self.temp} {self.source}",
            "file": self.source,
        }]
        self._write("compile_commands.json", json.dumps(commands))
        result = asm_tools.showAsm(self.temp, "sample.c", "add")
        self.assertNotIn("error", result)
        self.assertIn("add:", result["asm"])

    def test_show_asm_relative_compiler(self):
        compiler = os.path.join(self.temp, "myclang")
        os.symlink(shutil.which("clang"), compiler)
        commands = [{
            "directory": self.temp,
            "command": f"./myclang -c -I{self.temp} {self.source}",
            "file": self.source,
        }]
        self._write("compile_commands.json", json.dumps(commands))
        result = asm_tools.showAsm(self.temp, "sample.c", "add")
        self.assertNotIn("error", result)
        self.assertIn("add:", result["asm"])

    def test_show_asm_relative_db_paths(self):
        commands = [{
            "directory": self.temp,
            "command": "clang -c -I. sample.c",
            "file": "sample.c",
        }]
        self._write("compile_commands.json", json.dumps(commands))
        result = asm_tools.showAsm(self.temp, "sample.c", "add")
        self.assertNotIn("error", result)
        self.assertIn(f"-I{self.temp}", result["flags"])
        self.assertIn("add:", result["asm"])

    def test_show_asm_arguments_db(self):
        commands = [{
            "directory": self.temp,
            "arguments": ["clang", "-c", f"-I{self.temp}", "-DARG=3",
                          self.source],
            "file": self.source,
        }]
        self._write("compile_commands.json", json.dumps(commands))
        result = asm_tools.showAsm(self.temp, "sample.c", "add")
        self.assertNotIn("error", result)
        self.assertIn("-DARG=3", result["flags"])

    def test_show_asm_relative_include_fallback(self):
        self._write("include/probe.h", "#define PROBE 1\n")
        self._write("probe.c",
                    "#include <probe.h>\nint probeFn(void) { return PROBE; }\n")
        commands = [{
            "directory": self.temp,
            "command": f"clang -c -Iinclude {self.source}",
            "file": self.source,
        }]
        self._write("compile_commands.json", json.dumps(commands))
        result = asm_tools.showAsm(self.temp, "probe.c", "probeFn")
        self.assertNotIn("error", result)
        self.assertIn("probeFn:", result["asm"])

    def test_show_asm_malformed_db_falls_back(self):
        self._write("compile_commands.json", "{not json")
        result = asm_tools.showAsm(self.temp, "sample.c", "add")
        self.assertNotIn("error", result)
        self.assertTrue(any("no compile_commands.json" in note
                            for note in result["notes"]))

    def test_show_asm_missing_compiler(self):
        commands = [{
            "directory": self.temp,
            "command": f"/no/such/compiler -c {self.source}",
            "file": self.source,
        }]
        self._write("compile_commands.json", json.dumps(commands))
        with self.assertRaises(asm_tools.AsmError):
            asm_tools.showAsm(self.temp, "sample.c", "add")

    def test_show_asm_compile_error(self):
        with self.assertRaises(asm_tools.AsmError) as ctx:
            asm_tools.showAsm(self.temp, "sample.c", "add",
                              "-include missing_probe.h")
        self.assertIn("compiler failed", str(ctx.exception))

    def test_show_asm_invalid_extra_flags(self):
        with self.assertRaises(asm_tools.AsmError) as ctx:
            asm_tools.showAsm(self.temp, "sample.c", "add", "-D'")
        self.assertIn("invalid extra_flags", str(ctx.exception))

    @unittest.skipUnless(shutil.which("gcc"), "gcc is required")
    def test_show_asm_with_gcc(self):
        gcc = shutil.which("gcc")
        commands = [{
            "directory": self.temp,
            "command": f"{gcc} -c -I{self.temp} {self.source}",
            "file": self.source,
        }]
        self._write("compile_commands.json", json.dumps(commands))
        result = asm_tools.showAsm(self.temp, "sample.c", "add")
        self.assertNotIn("error", result)
        self.assertIn("add:", result["asm"])
        self.assertTrue(result["compiler"].endswith("gcc"))
        joined = " ".join(result["flags"])
        self.assertNotIn("-mllvm", joined)
        self.assertNotIn("-fvectorize", joined)

    def test_show_asm_extra_flags_change_codegen(self):
        self._write("flags.c", "int applyFactor(int a) { return a * FACTOR; }\n")
        first = asm_tools.showAsm(
            self.temp, "flags.c", "applyFactor", "-DFACTOR=2")
        second = asm_tools.showAsm(
            self.temp, "flags.c", "applyFactor", "-DFACTOR=9")
        self.assertNotIn("error", first)
        self.assertNotIn("error", second)
        self.assertNotEqual(first["asm"], second["asm"])

    def test_compare_asm_dot_path(self):
        result = asm_tools.compareAsm(self.temp, "./sample.c", "add")
        self.assertNotIn("error", result)
        self.assertEqual(result["diff"], "")

    def test_compare_asm_whole_file_change(self):
        self._write("sample.c",
                    SAMPLE_SOURCE.replace("return a + b;", "return a - b;"))
        result = asm_tools.compareAsm(self.temp, "sample.c")
        self.assertNotIn("error", result)
        self.assertNotEqual(result["diff"], "")
        self.assertGreater(result["stats"]["added_lines"], 0)

    def test_compare_asm_function_missing_in_current(self):
        self._write("sample.c",
                    SAMPLE_SOURCE + "\nint oldOnly(int a) { return a; }\n")
        self._commit("add oldOnly")
        self._write("sample.c", SAMPLE_SOURCE)
        result = asm_tools.compareAsm(self.temp, "sample.c", "oldOnly")
        self.assertIn("error", result)
        self.assertIn("current tree", result["error"])

    def test_compare_asm_baseline_compile_failure(self):
        self._write("header.h", "#define HEADER_VALUE 7\n")
        self._write("sample.c",
                    '#include "header.h"\n'
                    "int useHeader(void) { return HEADER_VALUE; }\n")
        self._commit("add header use")
        self._write("sample.c", "int noHeader(void) { return 1; }\n")
        os.remove(os.path.join(self.temp, "header.h"))
        self._commit("drop header")
        with self.assertRaises(asm_tools.AsmError) as ctx:
            asm_tools.compareAsm(self.temp, "sample.c", "useHeader", "HEAD~1")
        self.assertIn("compiler failed", str(ctx.exception))

    def test_normalize_keeps_quoted_hash_and_labels(self):
        text = ('\t.asciz\t"a # b .Lfoo"\n'
                '\tmov\teax, 1 # trailing comment\n'
                'foo:\n\tjne\t.LBB0_2\n\tret\n'
                '.LBB0_2:\n\tret\n')
        lines = asm_tools._normalize(text)
        self.assertIn('.asciz "a # b .Lfoo"', lines)
        self.assertIn("mov eax, 1", lines)
        self.assertIn("jne .LL1", lines)

    def test_normalize_keeps_string_spacing(self):
        lines = asm_tools._normalize('\t.asciz\t"a  b"\n')
        self.assertEqual(lines, ['.asciz "a  b"'])

    def test_normalize_collapses_constant_pool_labels(self):
        first = "foo:\n\tmov eax, dword ptr [rip + .LCPI0_0]\n\tret\n"
        second = "foo:\n\tmov eax, dword ptr [rip + .LCPI0_9]\n\tret\n"
        self.assertEqual(asm_tools._normalize(first),
                         asm_tools._normalize(second))

    def test_normalize_ignores_rodata_labels(self):
        text = (".text\nfoo:\n\tmov eax, dword ptr [rip + .LCPI0_0]\n\tret\n"
                ".size foo, .-foo\n.section .rodata\n.LCPI0_0:\n\t.long 1\n")
        lines = asm_tools._normalize(text)
        self.assertIn("mov eax, dword ptr [rip + .LC]", lines)
        self.assertIn(".LC:", lines)

    def test_normalize_tracks_pushsection(self):
        text = (".text\nfoo:\n\t.pushsection .rodata\n.LCPI0_0:\n"
                "\t.long 1\n\t.popsection\n\tjne .LBB0_2\n"
                ".LBB0_2:\n\tret\n")
        lines = asm_tools._normalize(text)
        self.assertIn(".LC:", lines)
        self.assertIn("jne .LL1", lines)

    def test_stats_counts_instructions_not_labels(self):
        base = asm_tools._normalize("foo:\n\tvmovaps xmm0, xmm1\n\tret\n")
        current = asm_tools._normalize(
            "foo:\n\tvmovaps xmm0, xmm1\n\tvmovaps xmm2, xmm3\n"
            "\tjne .LL1\n\tret\n.LL1:\n\tret\n")
        stats = asm_tools._stats(base, current, [])
        self.assertEqual(stats["base_instructions"], 2)
        self.assertEqual(stats["current_instructions"], 5)
        self.assertEqual(stats["instruction_delta"], 3)
        self.assertEqual(stats["base_vector_instructions"], 1)
        self.assertEqual(stats["current_vector_instructions"], 2)
        self.assertEqual(stats["base_branch_instructions"], 1)
        self.assertEqual(stats["current_branch_instructions"], 3)

    def test_makefile_appends_flags(self):
        self._write("Makefile",
                    "CFLAGS_BASE = -O2\n"
                    "CFLAGS_BASE += -fno-inline\n"
                    "all:\n\t@true\n")
        flags = asm_tools._makefile_cflags(self.temp)
        self.assertIn("-O2", flags)
        self.assertIn("-fno-inline", flags)

    def test_makefile_comments_and_continuations(self):
        self._write("Makefile",
                    "MINIFB_DIR = deps/minifb\n"
                    "CFLAGS_BASE = -O2 \\\n"
                    " -fno-inline \\\n"
                    " -I$(MINIFB_DIR)/include # trailing comment\n"
                    "all:\n\t@true\n")
        flags = asm_tools._makefile_cflags(self.temp)
        self.assertIn("-O2", flags)
        self.assertIn("-fno-inline", flags)
        self.assertIn("-Ideps/minifb/include", flags)
        self.assertNotIn("#", " ".join(flags))

    def test_cap_truncates(self):
        text, truncated = asm_tools._cap("x" * 20, 10)
        self.assertTrue(truncated)
        self.assertTrue(text.endswith("(truncated)"))


if __name__ == "__main__":
    unittest.main()
