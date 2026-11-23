# py2c99

`py2c99` transpiles a practical subset of Python 3 to portable C99.  The
generated program does **not** embed CPython and does not need `libpython`:
every Python value is a reference-counted `PyVal` handled by a small runtime
in `runtime/pyrt.c`.

On Windows the runtime also contains a native tkinter shim
(`runtime/pyrt_win.c`) built directly on `user32.dll`, `gdi32.dll` and
`comctl32.dll`.  Tkinter scripts therefore compile to ordinary Win32
executables.

Everything is emitted as C99.  The default build uses:

```
gcc -std=c99 -O3 -s -ffunction-sections -fdata-sections \
    -Wl,--gc-sections -municode -luser32 -lgdi32 -lcomctl32 -lws2_32 -lm
```
The runtime is split into six translation units:

* `runtime/pyrt.c` — object model, exceptions, operators, core builtins
* `runtime/pyrt_stdlib.c` — first wave of standard-library shims
* `runtime/pyrt_extra.c` — 80+ further real C99 modules (formats, network,
  OS services, introspection, …)
* `runtime/pyrt_builtins.c` — remaining Python 3.11 builtins
* `runtime/pyrt_311.c` — **generated** Python 3.11 builtin / stdlib registries
  plus compatibility shims for every stdlib module name
* `runtime/pyrt_win.c` — native Win32 tkinter (Windows only)

`runtime/pyrt_311.c` is regenerated from CPython 3.11 with
`python tools/gen_registry311.py` (run it under Python 3.11).  It is checked in
so compiled programs never need Python at runtime.

The build driver performs per-program dead-code elimination before invoking
GCC: `pyrt_win.c`, `pyrt_extra.c` and `pyrt_builtins.c` are omitted entirely
when the source cannot reach tkinter, second-wave modules or extra builtins.
The core uses tiny no-op stubs (`PY2C99_NO_GUI`, `PY2C99_NO_EXTRA_MODULES`,
`PY2C99_NO_EXTRA_BUILTINS`) so section GC can remove the remaining branches.

Link-time optimization can be enabled with `--lto`; it is off by default
because it changes how `setjmp` handlers are optimized.

## Quick start

Requirements:

* Python 3.8+ (only to run the transpiler)
* MinGW-w64 GCC on `PATH` (the build below was tested with GCC 14.2.0,
  UCRT, POSIX threads, SEH)
* Windows 7 or newer for GUI programs

```bat
cd py2c99
python py2c99.py examples\hello.py
hello.exe
```

Build and immediately run:

```bat
python py2c99.py examples\gui_smoke.py --run
```

Interactive REPL (no source file argument):

```bat
python py2c99.py
```

For CMD.exe there is also a launcher. Add this directory to `PATH`, then type:

```bat
py2c99
```

The REPL keeps earlier imports, assignments and `def`/`class` blocks by
recompiling them into a fresh executable for every submission.  Statements
that print are executed only once; compound blocks are ended with a blank
line.  Type `exit()` to leave.

Useful options:

```
-o NAME       output executable (default: source name with .exe)
--keep        keep the generated C next to the executable
--run         run the executable after a successful build
-v            print the full gcc command line
-C FLAG       pass an extra flag to gcc (repeatable)
--lto         enable link-time optimization (experimental, off by default)
```

Set the `CC` environment variable to use a different compiler driver.

## What is supported

### Language

* assignments, including chained, augmented and tuple/list unpacking
* `if` / `elif` / `else`, `while` / `else`, `for` / `else`,
  `break`, `continue`
* truthiness, all usual binary/unary operators and comparisons
* `None`, booleans, 64-bit integers, doubles, strings, bytes (as strings)
* lists, tuples, dicts, sets, `range`
* list/set/dict comprehensions and generator expressions
* functions with defaults, `*args`, `**kwargs`, keyword-only arguments,
  keyword calls and lambdas
  (lambdas capture free variables through a small environment dict)
* module-level function and class decorators (applied in reverse order)
* nested `def` with closures (shared cells, late binding like CPython) and
  `nonlocal` writes
* classes, methods, `self`, `__init__`, single and multiple base classes
  (base class dictionaries are copied), class attributes, `@staticmethod`
  and `super()` for the first base class
* user-defined exception classes: `class MyError(Exception)`, subclasses such
  as `class Wrapped(MyError)`, `raise MyError("msg")`, `except MyError as e`,
  `str(e)`, `e.args`, custom exception attributes, `type(e).__name__`,
  `isinstance(e, MyError)` and `issubclass(Wrapped, MyError)`
* `try` / `except` / `else` / `finally`, `raise`, bare re-raise, `assert`
* `with` blocks (protocol methods `__enter__` / `__exit__`)
* f-strings with `!s` / `!r` / `!a`, debug `=`, nested and dynamic format
  specs, plus `%` formatting and `str.format`
* `global` and `nonlocal` declarations are supported (the latter for names
  captured by a nested function)
* imports and `from ... import ...` for **all 305 Python 3.11 standard-library
  module names** (every name in `sys.stdlib_module_names`, plus common
  submodules such as `email.message`, `http.client`, `concurrent.futures`,
  `unittest.mock`, `importlib.metadata`, `xml.dom.minidom`, …).  The core and
  140+ extended modules below have real C implementations; the remaining names
  are recognised and importable but raise `NotImplementedError` when a function
  without a compiled implementation is called.  Private `_*` modules resolve
  to their public counterparts (e.g. `import _json` behaves like
  `import json`).  Dynamic `__import__("name")` uses the same dispatch.

### Builtins

`print`, `input`, `len`, `int`, `float`, `str`, `bool`, `list`, `tuple`,
`dict`, `set`, `range`, `abs`, `min`, `max`, `sum`, `sorted`, `reversed`,
`enumerate`, `zip`, `any`, `all`, `chr`, `ord`, `round`, `pow`, `divmod`,
`hex`, `oct`, `bin`, `repr`, `format`, `id`, `hash`, `isinstance`, `type`,
`open`, `iter`, `next`, `map`, `filter`, `getattr`, `setattr`, `hasattr`,
`callable`, `slice`, `frozenset`, plus the Python 3.11 additions `ascii`,
`issubclass`, `aiter`, `anext`, `delattr`, `breakpoint`, `globals`, `locals`,
`object`, `property`, `staticmethod`, `classmethod`, `super`, `memoryview`,
`help`, `exit`, `quit`, `bytes`, `bytearray`, `dir`, `vars`, `__import__`,
`compile`, `eval`, `exec` and `__build_class__`.  **All 158 names in
`dir(builtins)` for Python 3.11 are recognised**, including the constants
`None`, `True`, `False`, `__debug__`, `Ellipsis`, `NotImplemented`, the dunder
module attributes (`__name__`, `__package__`, `__doc__`, `__loader__`,
`__spec__`) and every exception class.  `dir(builtins)` returns the complete
158-name list, and `builtins.X` resolves through the same table as bare `X`.
`complex`, `copyright`, `credits` and `license` have lightweight C
implementations; the exception classes are real `py_exc_class` objects.

`functools.reduce` and the `itertools` helpers `chain`, `repeat`, `product`
and `islice` are also available.  They return ordinary lists, so wrap them in
`list(...)` only if you want to be explicit.

`enumerate` accepts both positional and keyword `start`; `getattr` supports a
default value, `iter(callable, sentinel)` is available, `min` / `max` accept
`key=` and `default=`, `sum` accepts `start=`, and `int` / `ord` / `chr` /
`pow` validate their arguments.  `dict.keys()`, `dict.values()` and
`dict.items()` return view objects with Python-like reprs and full iteration
support.  `type(x)` returns a real class object whose repr is `<class 'int'>`
etc.

Exceptions use Python names (`ValueError`, `KeyError`, `IndexError`,
`ZeroDivisionError`, `TypeError`, `OSError`, ...) and carry a message.

### Containers and strings

Common `list`, `dict`, `set`, `str` and file methods are implemented in the
runtime.  Slicing (`x[a:b:c]`) works on strings, lists, tuples and `range`,
both inline and via `slice` objects.  Many `str` methods are available,
including `split`, `join`, `strip`, `replace`, `upper`, `lower`,
`startswith`, `endswith`, `find`, `format` and friends.  `sorted` supports
`key=` and `reverse=`, and list sorting supports both too.

### Standard-library modules

Every one of the 305 Python 3.11 stdlib module names is known to the runtime.
More than 140 are implemented in pure C99 across `runtime/pyrt_stdlib.c`,
`runtime/pyrt_extra.c` and `runtime/pyrt_311.c`.  Modules with real C
implementations include:

`builtins`, `cmath`, `operator`, `collections`, `collections.abc`, `heapq`,
`bisect`, `array`, `queue`, `copy`, `pprint`, `types`, `enum`, `dataclasses`,
`abc`, `contextlib`, `typing`, `numbers`, `gc`, `json`, `csv`, `re`,
`textwrap`, `html`, `html.entities`, `base64`, `binascii`, `zlib`, `struct`,
`codecs`, `unicodedata`, `pathlib`, `glob`, `fnmatch`, `shutil`, `tempfile`,
`subprocess`, `stat`, `errno`, `os.path`, `ntpath`, `posixpath`, `getpass`,
`sysconfig`, `datetime`, `calendar`, `timeit`, `uuid`, `hashlib`, `secrets`,
`hmac`, `socket`, `ssl`, `urllib`, `urllib.parse`, `urllib.request`, `http`,
`logging`, `getopt`, `msvcrt`, `threading`, `multiprocessing`, `signal`,
`select`, `weakref`, `inspect`, `statistics`, `fractions`, `decimal`,
`colorsys`, `difflib`, `reprlib`, `keyword`, `ast`, `dis`, `opcode`,
`configparser`, `netrc`, `xdrlib`, `plistlib`, `quopri`, `uu`, `mimetypes`,
`html.parser`, `xml.etree.ElementTree`, `io`, `pickle`, `copyreg`, `marshal`,
`shelve`, `dbm`, `tarfile`, `zipfile`, `wave`, `aifc`, `sunau`, `chunk`,
`fileinput`, `filecmp`, `linecache`, `locale`, `gettext`, `atexit`, `warnings`,
`traceback`, `contextvars`, `graphlib`, `sched`, `selectors`, `mmap`, `ftplib`,
`smtplib`, `poplib`, `email`, `email.utils`, `http.client`,
`urllib.robotparser`, `urllib.error`, `webbrowser`, `zoneinfo`, `tomllib`,
`zipapp`, `importlib`, `importlib.util`, `runpy`, `pkgutil`, `modulefinder`,
`site`, `code`, `codeop`, `token`, `tabnanny`, `symtable`, `pydoc`, `doctest`,
`unittest`, `typing_extensions`, `audioop`, `sre_compile`, `sre_parse`,
`shlex`, `stringprep`, `platformdirs`, `diskcache`, `this`, `antigravity`,
`nturl2path`, `ipaddress`, `pipes`, `gzip`, `argparse` and `tokenize`.

The remaining stdlib names (mostly private `_*` extension modules and rarely
used tools such as `turtle`, `curses`, `sqlite3`, `idlelib`, `lib2to3`,
`tkinter.scrolledtext`, `xml.dom`, `xml.sax`, `multiprocessing.*`, `email.*`,
`logging.*`, `importlib.*`, `unittest.*`, `http.server`, etc.) are registered
as **compatibility shims**: importing them succeeds and common constants such
as `__future__.all_features`, `this.s`, `sys.stdlib_module_names` and
`sys.builtin_module_names` are available, but calling an unimplemented function
raises a clear `NotImplementedError` rather than silently doing nothing.
`py311_is_stdlib_module()` also accepts dotted submodules of any registered
package.  Run `python tests/test_coverage311.py` to verify that the generated
registries and the linked runtime cover all 305 module names and all 158
builtin names.

Aliases are resolved at runtime: `ntpath` and `genericpath` share the Windows
`os.path` implementation, while `posixpath` has its own `/`-separated path
functions.  `collections.abc` maps to `collections`, `urllib.parse` to
`urllib`, and `urllib.error` to `urllib.request`.  `html.entities` exposes
`html5`, `entitydefs`, `name2codepoint` and `codepoint2name` dictionaries.
`hashlib` implements MD5, SHA-1, SHA-224/256 and SHA-384/512 directly in C99;
the returned hash objects support `hexdigest()`, `digest()`, `update()` and
`copy()`.  `hmac.new()` computes real HMACs and requires an explicit
`digestmod`, as in Python 3.11.

Most listed modules have real C implementations for their common functions.
Examples include `statistics.mean/median`,
`fractions.Fraction`, `difflib.SequenceMatcher`, `pickle.dumps/loads`,
`tomllib.loads`, `configparser`, real Winsock/POSIX `socket`, `signal` and
`select`, line-oriented `ftplib`/`smtplib`/`poplib` clients, and actual file
parsers for tar/zip/wave/plist.  `examples/modules.py` is a smoke test whose
output matches CPython exactly.

### tkinter

The following widget classes map to native Win32 controls:

`Tk`, `Toplevel`, `Frame`, `Label`, `Button`, `Entry`, `Text`, `Canvas`,
`Listbox`, `Checkbutton`, `Radiobutton`, `Scale`, `Menu`, `Menubutton`,
`Spinbox`, `Progressbar`, `Separator`, `Treeview`, `Notebook`, and the
`ttk` aliases for the same names.

Supported pieces of the widget API include:

* `pack`, `grid`, `place` with the usual geometry options
* `config` / `configure` / `cget`, `bind`, `after`, `after_cancel`
* `mainloop`, `update`, `update_idletasks`, `destroy`, `title`, `geometry`,
  `state`, `resizable`, `protocol`, `winfo_*`
* `StringVar`, `IntVar`, `DoubleVar`, `BooleanVar` with `get` / `set`
* canvas items (`create_line`, `create_rectangle`, `create_oval`,
  `create_text`, `coords`, `itemconfig`, `find_all`, ...)
* `messagebox.showinfo/showwarning/showerror/askyesno/...`
* common tkinter constants (`LEFT`, `RIGHT`, `BOTH`, `X`, `Y`, `END`, ...)

Widget callbacks are ordinary transpiled functions.  A method such as
`root.destroy` can also be passed directly as a `command=` callback.

## Known limitations

* The 140+ modules with real C99 implementations are not a complete CPython
  stdlib: only their common, deterministic surface is provided and the
  semantics can differ in edge cases.  Several families (`subprocess`,
  `tempfile`, `urllib.request`, `logging`, `queue`, `pathlib`, `datetime`)
  expose a reduced surface; unsupported calls raise an `AttributeError`.  The
  other registered stdlib names are compatibility shims: importing them works,
  but unimplemented functions raise `NotImplementedError`.  Keyword arguments
  to wave-2 module functions are accepted and ignored rather than rejected,
  because the C shims do not model every optional parameter.  Importing an
  arbitrary third-party module is still a compile-time error.
* `bytes` values are stored as UTF-8 strings.  Consequently `len()` counts
  characters rather than raw bytes, which also affects `hash.digest()` when a
  digest byte is non-ASCII; use `hexdigest()` for exact results.
* `compile()` stores source metadata but there is no dynamic `eval` / `exec`
  execution, no full `__dict__` introspection, metaclasses, generators
  (`yield`) or `async` / `await`.  The descriptor
  builtins (`property`, `staticmethod`, `classmethod`, `super`) are
  lightweight dictionary-based stand-ins.  Nested `def` is
  supported; nested `class` is not.  Decorators are supported at module
  scope and on nested functions, but not yet on methods inside a class.
* `super()` works for the first base class of transpiled classes (including
  user exception classes, where `super().__init__(...)` is a no-op because
  constructor arguments are retained automatically).  There is no full MRO:
  multiple inheritance copies methods from every base, with the last base
  winning on name clashes.
* Integers are signed 64-bit; arbitrary-precision arithmetic is not
  available.  Floating point follows the platform `double`.
* `isinstance` / `issubclass` understand built-in type names, tuples of names,
  transpiled classes and user exception-class inheritance chains; they are not
  a full object protocol.
* The tkinter shim implements the common widget surface, not every Tk
  command or option.  Unknown options are silently ignored.
* Exceptions raised inside a GUI callback are printed to the console; the
  event loop keeps running.
* Non-ASCII source literals are emitted as UTF-8 and displayed by the
  console / Win32 controls accordingly.

## Layout

```
py2c99.py            transpiler, build driver and REPL
py2c99.cmd           CMD.exe launcher for the REPL / compiler
runtime/pyrt.h       object model, exceptions, public runtime API
runtime/pyrt.c       core runtime (values, operators, modules, I/O)
runtime/pyrt_stdlib.c pure-C standard-library shims (first 65 modules)
runtime/pyrt_extra.c  80+ further real C99 modules (formats, network, OS)
runtime/pyrt_builtins.c remaining Python 3.11 builtins
runtime/pyrt_311.c     generated 3.11 registries + module compatibility layer
runtime/pyrt_win.c   native Win32 tkinter implementation
tools/gen_registry311.py regenerates pyrt_311.c from CPython 3.11
examples/hello.py       console demo
examples/modules.py     standard-library smoke test
examples/fstrings.py    f-string and format-spec smoke test
examples/builtins311.py Python 3.11 builtin smoke test
examples/stdlib_extra.py path aliases, html.entities and email smoke test
examples/hashlib311.py    real hashlib/hmac digest smoke test
examples/registry311_smoke.py full builtin/stdib registry smoke test
examples/gui.py         tkinter demo
examples/gui_smoke.py   self-terminating GUI smoke test
tests/run_tests.py      compare transpiled output against CPython and test REPL
tests/test_coverage311.py verify all 158 builtins / 305 stdlib names are known
```

## Optimizations

The emitted C is built with `-O3 -s`, `-ffunction-sections`,
`-fdata-sections` and `--gc-sections`.  On top of linker GC, the build driver
guards whole translation units: GUI code is linked only for programs that
reference tkinter, second-wave modules only for programs that import them,
and `pyrt_builtins.c` only for programs that use its builtins.  This keeps
console programs small without changing script behavior.  The code generator
also performs a few safe, local optimizations:

* constant folding for numeric/string `BinOp`, `UnaryOp` and simple
  comparisons (constant expressions become a single C literal call)
* constant-condition `if` / `while` and conditional expressions emit only the
  live branch
* f-strings are assembled in one pass with `py_str_join` instead of a chain
  of temporary string concatenations
* `--lto` enables link-time optimization for builds that can tolerate its
  effect on `setjmp`-based exception handling

## How it works

1. The transpiler parses the input with Python's `ast` module.
2. A pre-scan collects module globals, functions, classes and imports.
3. Every expression is lowered to A-normal form: each sub-expression is
   stored in a C temporary that owns exactly one reference.  Temporaries are
   released as soon as their value is consumed.
4. Function bodies declare their Python locals as `PyVal *volatile` (volatile
   matters because exceptions use `setjmp` / `longjmp`).
5. Names are loaded through `py_load`, which raises `NameError` for unbound
   locals instead of dereferencing `NULL`.
6. The module body runs inside a top-level handler; an uncaught exception
   prints a traceback-like message and exits with status 1.
7. The generated translation unit is compiled together with the runtime.
