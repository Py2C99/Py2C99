"""Coverage tests for the Python 3.11 builtin / stdlib registries.

Run with:  python tests/test_coverage311.py

These tests check three things that example-output comparisons cannot:

1. the generated C registry contains every CPython 3.11 builtin and stdlib
   module name;
2. the compiler and runtime tables together know every builtin name;
3. a real binary linked from the runtime answers ``py_module_known()`` for all
   305 stdlib module names and can enumerate the full builtin registry.
"""
import builtins
import os
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
RUNTIME = ROOT / "runtime"
GENERATED = RUNTIME / "pyrt_311.c"
PY2C99 = ROOT / "py2c99.py"


def audit_generated():
    text = GENERATED.read_text(encoding="utf-8")
    builtin_block = re.search(
        r"static const char \*const B311\[\] = \{(.*?)\n\};", text, re.S)
    module_block = re.search(
        r"static const char \*const M311\[\] = \{(.*?)\n\};", text, re.S)
    assert builtin_block and module_block, "registry arrays not found"
    in_c = set(re.findall(r'"([^"]+)"', builtin_block.group(1)))
    mods_c = set(re.findall(r'"([^"]+)"', module_block.group(1)))
    expected_builtins = set(dir(builtins))
    expected_modules = set(sys.stdlib_module_names)
    missing_b = sorted(expected_builtins - in_c)
    missing_m = sorted(expected_modules - mods_c)
    assert not missing_b, "builtins missing from registry: %s" % missing_b
    assert not missing_m, "modules missing from registry: %s" % missing_m
    print("PASS generated registry: %d builtins, %d modules" %
          (len(in_c), len(mods_c)))


def audit_compiler_tables():
    text = PY2C99.read_text(encoding="utf-8")
    names = set()
    for table in ("BUILTIN_NAMES", "EXC_KINDS", "TK_CONSTANTS"):
        match = re.search(table + r" = \{(.*?)\n\}", text, re.S)
        if match:
            names.update(re.findall(r'"([^"]+)"', match.group(1)))
    names.update({"True", "False", "None", "Ellipsis", "NotImplemented",
                  "__debug__", "__name__", "__file__", "__doc__",
                  "__loader__", "__package__", "__spec__",
                  "__build_class__", "copyright", "credits", "license"})
    missing = sorted(set(dir(builtins)) - names)
    assert not missing, "compiler cannot resolve builtins: %s" % missing
    assert GENERATED.exists(), "runtime/pyrt_311.c is missing"
    print("PASS compiler tables: all %d builtin names resolvable" %
          len(dir(builtins)))


def audit_runtime_binary():
    modules = sorted(sys.stdlib_module_names)
    mods = "\n".join('    "%s",' % m for m in modules)
    source = """
#include "pyrt.h"
#include <stdio.h>
static const char *const MODS[] = {
%s
    NULL
};
int main(void) {
    int i, bad = 0, total = 0;
    PyVal *names;
    py_init(0, NULL);
    for (i = 0; MODS[i]; i++) {
        total++;
        if (!py_module_known(MODS[i])) {
            printf("UNKNOWN %%s\\n", MODS[i]);
            bad++;
        }
    }
    names = py311_builtin_names();
    printf("MODULES %%d %%d\\n", total, bad);
    printf("BUILTIN_NAMES %%d\\n", (int)py_len(names));
    py_release(names);
    if (!py_builtin_lookup("complex") || !py_builtin_lookup("__debug__") ||
        !py_builtin_lookup("ValueError") || !py_builtin_lookup("print")) {
        printf("BUILTIN_LOOKUP_FAIL\\n");
        bad++;
    }
    py_shutdown();
    return bad != 0;
}
""" % mods
    with tempfile.TemporaryDirectory(prefix="py2c99_cov_") as tmp:
        c_path = pathlib.Path(tmp) / "coverage.c"
        exe = pathlib.Path(tmp) / "coverage.exe"
        c_path.write_text(source, encoding="utf-8", newline="\n")
        cmd = [os.environ.get("CC", "gcc"), "-std=c99", "-O0",
               "-DPY2C99_NO_GUI", "-o", str(exe), str(c_path),
               str(RUNTIME / "pyrt.c"), str(RUNTIME / "pyrt_stdlib.c"),
               str(RUNTIME / "pyrt_311.c"), str(RUNTIME / "pyrt_builtins.c"),
               str(RUNTIME / "pyrt_extra.c"), "-I", str(RUNTIME),
               "-lws2_32", "-lm"]
        proc = subprocess.run(cmd, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, text=True)
        if proc.returncode:
            raise AssertionError("harness build failed:\n" + proc.stdout)
        proc = subprocess.run([str(exe)], stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, text=True, timeout=30)
    lines = proc.stdout.splitlines()
    summary = [line for line in lines if line.startswith("MODULES ")]
    assert summary, proc.stdout
    _, total, bad = summary[0].split()
    assert int(total) == len(modules), summary[0]
    assert int(bad) == 0, proc.stdout
    assert "BUILTIN_NAMES %d" % len(dir(builtins)) in lines, proc.stdout
    assert "BUILTIN_LOOKUP_FAIL" not in lines, proc.stdout
    print("PASS runtime binary: all %d stdlib modules and %d builtin names" %
          (len(modules), len(dir(builtins))))


def main():
    audit_generated()
    audit_compiler_tables()
    audit_runtime_binary()
    print("3 coverage checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
