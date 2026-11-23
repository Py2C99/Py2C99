"""Regression runner: compare py2c99 output with CPython for console examples."""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXAMPLES = os.path.join(ROOT, "examples")
CASES = [
    "hello.py",
    "features.py",
    "coverage.py",
    "morebuiltins.py",
    "decorators.py",
    "sorttest.py",
    "kwtest.py",
    "exctest3.py",
    "ziptest3.py",
    "globaltest.py",
    "supertest.py",
    "nestedtest.py",
    "nestedtest2.py",
    "nestedtest3.py",
    "defaults2.py",
    "optstest.py",
    "itertoolstest.py",
    "modules.py",
    "fstrings.py",
    "builtins311.py",
    "stdlib_extra.py",
    "hashlib311.py",
]
GUI_CASES = ["gui_smoke.py", "invoke_test.py"]


def run(cmd, cwd=ROOT, timeout=60):
    env = os.environ.copy()
    env["PYTHONIOENCODING"] = "utf-8"
    try:
        p = subprocess.run(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=timeout)
        out = p.stdout.decode("utf-8", "replace")
        return p.returncode, out
    except subprocess.TimeoutExpired as exc:
        out = exc.stdout.decode("utf-8", "replace") if exc.stdout else ""
        return 124, out + "\nTIMEOUT after %ss: %s\n" % (timeout, " ".join(cmd))


def repl_case():
    """The no-argument launcher must run an interactive session with state."""
    payload = "x = 3 + 4\nprint(x)\nprint(x * 2)\nexit()\n"
    try:
        p = subprocess.run([sys.executable, os.path.join(ROOT, "py2c99.py")],
                           cwd=ROOT, input=payload.encode("utf-8"),
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           timeout=180)
        out = p.stdout.decode("utf-8", "replace")
        code = p.returncode
    except subprocess.TimeoutExpired as exc:
        out = exc.stdout.decode("utf-8", "replace") if exc.stdout else ""
        code = 124
        out += "\nTIMEOUT in REPL test\n"
    values = [tok for line in out.splitlines() for tok in line.split()
              if tok in ("7", "14")]
    ok = code == 0 and values == ["7", "14"] and "REPL" in out
    print(("PASS " if ok else "FAIL ") + "repl")
    if not ok:
        print("  exit: %d" % code)
        print(out)
    return 0 if ok else 1


def main():
    failed = 0
    for name in CASES + GUI_CASES:
        path = os.path.join(EXAMPLES, name)
        if not os.path.isfile(path):
            continue
        code, expected = run([sys.executable, path])
        if name in GUI_CASES:
            expected = ""  # GUI smoke tests only need to terminate cleanly
        name_exe = os.path.splitext(name)[0] + ".exe"
        code, build_out = run([sys.executable, os.path.join(ROOT, "py2c99.py"),
                               path, "-o", name_exe], timeout=120)
        # gcc warnings are written to stderr, which run() merges into stdout.
        # They are not part of the compiled program's output.
        if code == 0:
            build_out = ""
        if code != 0:
            ok = False
            out = build_out
        else:
            code, out = run([os.path.join(ROOT, name_exe)], timeout=30)
        ok = code == 0 and (name in GUI_CASES or out.splitlines() ==
                            expected.splitlines())
        print(("PASS " if ok else "FAIL ") + name)
        if not ok:
            failed += 1
            print("  exit: %d" % code)
            if name not in GUI_CASES:
                exp = expected.splitlines()
                act = out.splitlines()
                for i in range(max(len(exp), len(act))):
                    e = exp[i] if i < len(exp) else "<missing>"
                    a = act[i] if i < len(act) else "<missing>"
                    if e != a:
                        print("  line %d: expected %r, got %r" % (i + 1, e, a))
                        break
    failed += repl_case()
    code, out = run([sys.executable, os.path.join(ROOT, "tests",
                                                  "test_coverage311.py")],
                    timeout=180)
    print(("PASS " if code == 0 else "FAIL ") + "coverage311")
    if code != 0:
        failed += 1
        print("  exit: %d" % code)
        print(out)
    print("%d failure(s)" % failed)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
