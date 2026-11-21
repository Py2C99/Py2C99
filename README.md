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
    -Wl,--gc-sections -municode -luser32 -lgdi32 -lcomctl32
```
The runtime is split into `runtime/pyrt.c` (object model and core builtins),
`runtime/pyrt_stdlib.c` (standard-library shims) and, on Windows,
`runtime/pyrt_win.c` (native tkinter).

```
```

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
* `try` / `except` / `else` / `finally`, `raise`, bare re-raise, `assert`
* `with` blocks (protocol methods `__enter__` / `__exit__`)
* f-strings, `%` formatting, `str.format` (basic width/alignment/precision)
* `global` and `nonlocal` declarations are supported (the latter for names
  captured by a nested function)
* imports and `from ... import ...` for the core modules (`math`, `random`,
  `time`, `os`, `os.path`, `sys`, `platform`, `string`, `functools`,
  `itertools`, tkinter modules) and the 65 standard-library shims listed below

### Builtins

`print`, `input`, `len`, `int`, `float`, `str`, `bool`, `list`, `tuple`,
`dict`, `set`, `range`, `abs`, `min`, `max`, `sum`, `sorted`, `reversed`,
`enumerate`, `zip`, `any`, `all`, `chr`, `ord`, `round`, `pow`, `divmod`,
`hex`, `oct`, `bin`, `repr`, `format`, `id`, `hash`, `isinstance`, `type`,
`open`, `iter`, `next`, `map`, `filter`, `getattr`, `setattr`, `hasattr`,
`callable`, `slice`, `frozenset`.

`functools.reduce` and the `itertools` helpers `chain`, `repeat`, `product`
and `islice` are also available.  They return ordinary lists, so wrap them in
`list(...)` only if you want to be explicit.

`enumerate` accepts both positional and keyword `start`; `dict.keys()`,
`dict.values()` and `dict.items()` return view objects with Python-like
reprs and full iteration support.  `type(x)` returns a real class object
whose repr is `<class 'int'>` etc.

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

More than 60 standard-library modules are implemented as pure C99 shims in
`runtime/pyrt_stdlib.c`.  The supported module names are:

`builtins`, `cmath`, `operator`, `collections`, `collections.abc`, `heapq`,
`bisect`, `array`, `queue`, `copy`, `pprint`, `types`, `enum`, `dataclasses`,
`abc`, `contextlib`, `typing`, `numbers`, `gc`, `json`, `csv`, `re`,
`textwrap`, `html`, `html.entities`, `base64`, `binascii`, `zlib`, `struct`,
`codecs`, `unicodedata`, `pathlib`, `glob`, `fnmatch`, `shutil`, `tempfile`,
`subprocess`, `stat`, `errno`, `os.path`, `ntpath`, `posixpath`, `getpass`,
`sysconfig`, `datetime`, `calendar`, `timeit`, `uuid`, `hashlib`, `secrets`,
`hmac`, `socket`, `ssl`, `urllib`, `urllib.parse`, `urllib.request`, `http`,
`logging`, `getopt`, `msvcrt`, `threading`, `multiprocessing`, `signal`,
`select`, `weakref`, `inspect`.

Useful, deterministic functions are implemented for each module: for example
`json.dumps`/`loads`, regex `search`/`findall`/`sub`, `base64`, `zlib`,
`struct`, `hashlib`, `urllib.parse`, `pathlib`, `heapq`, `bisect`, `logging`
and the `collections`/`operator` helpers.  `examples/modules.py` is a smoke
test whose output matches CPython exactly.

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

* The standard-library modules listed above are deterministic C shims, not a
  full CPython stdlib.  Network, process and platform-specific modules expose
  their common, harmless surface; unsupported members raise an `AttributeError`.
  Importing an arbitrary third-party module remains a compile-time error.
* There is no `eval`, `exec`, `__dict__` introspection, descriptors,
  metaclasses, generators (`yield`) or `async` / `await`.  Nested `def` is
  supported; nested `class` is not.  Decorators are supported at module
  scope and on nested functions, but not yet on methods inside a class.
* `super()` works for the first base class of transpiled classes.  There is
  no full MRO: multiple inheritance copies methods from every base, with the
  last base winning on name clashes.
* Integers are signed 64-bit; arbitrary-precision arithmetic is not
  available.  Floating point follows the platform `double`.
* `isinstance` understands built-in type names, tuples of names and classes
  that were created by this transpiler; it is not a full object protocol.
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
runtime/pyrt_stdlib.c pure-C standard-library shims (65+ modules)
runtime/pyrt_win.c   native Win32 tkinter implementation
examples/hello.py    console demo
examples/modules.py  standard-library smoke test
examples/gui.py      tkinter demo
examples/gui_smoke.py self-terminating GUI smoke test
tests/run_tests.py   compare transpiled output against CPython and test REPL
```

## Optimizations

The emitted C is built with `-O3 -s` and section GC by default, so unused
runtime functions are stripped from the executable.  The code generator also
performs a few safe, local optimizations:

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
