#!/usr/bin/env python3
"""
py2c99 - transpile a useful subset of Python 3 to portable C99.

The generated program links against runtime/pyrt.c (core object model),
runtime/pyrt_stdlib.c (65+ standard-library shims) and, on Windows, the
native tkinter shim runtime/pyrt_win.c.  There is no CPython involved: every
Python value is a reference counted PyVal.

Usage:
    py2c99                                      # start the interactive REPL
    python py2c99.py program.py                 # build program.exe
    python py2c99.py program.py -o app.exe      # choose output name
    python py2c99.py program.py --run           # build and run
    python py2c99.py program.py --keep          # keep generated C
    python py2c99.py program.py -v              # show the gcc command

Supported highlights:
    functions, defaults, *args/**kwargs, classes/methods/inheritance,
    if/while/for/break/continue/else, try/except/finally, with, raise,
    comprehensions, lambdas with closures, f-strings, imports of 65+
    standard-library modules, a persistent no-argument REPL, and
    tkinter/ttk widgets.
"""

import argparse
import ast
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
RUNTIME = os.path.join(HERE, "runtime")

# --------------------------------------------------------------------------
# small helpers


def warn(msg, node=None):
    where = ""
    if node is not None and hasattr(node, "lineno"):
        where = " (line %d)" % node.lineno
    sys.stderr.write("py2c99: warning: %s%s\n" % (msg, where))


def die(msg):
    sys.stderr.write("py2c99: error: %s\n" % msg)
    raise SystemExit(2)


def cstr(text):
    """Escape a Python string into a C string literal (UTF-8 bytes)."""
    out = ['"']
    for b in text.encode("utf-8"):
        if b == 0x22:
            out.append('\\"')
        elif b == 0x5C:
            out.append("\\\\")
        elif b == 0x0A:
            out.append("\\n")
        elif b == 0x0D:
            out.append("\\r")
        elif b == 0x09:
            out.append("\\t")
        elif 32 <= b < 127:
            out.append(chr(b))
        else:
            out.append("\\%03o" % b)
    out.append('"')
    return "".join(out)


def sanitize(name):
    keep = []
    for ch in name:
        if (ch.isascii() and (ch.isalnum() or ch == "_")):
            keep.append(ch)
        else:
            keep.append("_")
    s = "".join(keep) or "n"
    if s[0].isdigit():
        s = "n_" + s
    return s


class NamePool:
    """Allocate collision-free C identifiers."""

    def __init__(self):
        self.used = set()

    def take(self, base):
        base = sanitize(base)
        cand = base
        n = 2
        while cand in self.used:
            cand = "%s_%d" % (base, n)
            n += 1
        self.used.add(cand)
        return cand


# --------------------------------------------------------------------------
# constants / tables

BUILTIN_NAMES = {
    "print", "input", "len", "int", "float", "str", "bool", "list", "tuple",
    "dict", "set", "range", "abs", "min", "max", "sum", "sorted", "reversed",
    "enumerate", "zip", "any", "all", "chr", "ord", "round", "pow", "divmod",
    "hex", "oct", "bin", "repr", "format", "id", "hash", "isinstance", "type",
    "open", "iter", "next", "map", "filter", "getattr", "setattr", "hasattr",
    "callable", "slice", "frozenset",
}

TYPE_NAMES = {
    "int": "int", "float": "float", "str": "str", "bool": "bool",
    "list": "list", "tuple": "tuple", "dict": "dict", "set": "set",
    "bytes": "str", "object": "object", "range": "range", "type": "type",
    "BaseException": "BaseException", "Exception": "Exception",
}

EXC_KINDS = {
    "BaseException": "PY_EXC_BASE",
    "Exception": "PY_EXC_EXCEPTION",
    "ArithmeticError": "PY_EXC_ARITHMETIC",
    "AssertionError": "PY_EXC_ASSERTION",
    "AttributeError": "PY_EXC_ATTRIBUTE",
    "IndexError": "PY_EXC_INDEX",
    "KeyError": "PY_EXC_KEY",
    "NameError": "PY_EXC_NAME",
    "OSError": "PY_EXC_OS",
    "IOError": "PY_EXC_OS",
    "FileNotFoundError": "PY_EXC_FILE",
    "RuntimeError": "PY_EXC_RUNTIME",
    "StopIteration": "PY_EXC_STOP_ITERATION",
    "TypeError": "PY_EXC_TYPE",
    "ValueError": "PY_EXC_VALUE",
    "ZeroDivisionError": "PY_EXC_ZERO_DIVISION",
    "OverflowError": "PY_EXC_OVERFLOW",
    "MemoryError": "PY_EXC_MEMORY",
    "ImportError": "PY_EXC_IMPORT",
    "ModuleNotFoundError": "PY_EXC_IMPORT",
    "NotImplementedError": "PY_EXC_UNSUPPORTED",
    "EOFError": "PY_EXC_EOF",
    "SystemExit": "PY_EXC_BASE",
    "TclError": "PY_EXC_RUNTIME",
    "tkinter.TclError": "PY_EXC_RUNTIME",
}

BIN_OPS = {
    ast.Add: "PY_OP_ADD",
    ast.Sub: "PY_OP_SUB",
    ast.Mult: "PY_OP_MUL",
    ast.Div: "PY_OP_DIV",
    ast.FloorDiv: "PY_OP_FLOORDIV",
    ast.Mod: "PY_OP_MOD",
    ast.Pow: "PY_OP_POW",
    ast.LShift: "PY_OP_LSHIFT",
    ast.RShift: "PY_OP_RSHIFT",
    ast.BitAnd: "PY_OP_BITAND",
    ast.BitOr: "PY_OP_BITOR",
    ast.BitXor: "PY_OP_BITXOR",
    ast.MatMult: "PY_OP_MATMUL",
}

CMP_OPS = {
    ast.Eq: "PY_CMP_EQ",
    ast.NotEq: "PY_CMP_NE",
    ast.Lt: "PY_CMP_LT",
    ast.LtE: "PY_CMP_LE",
    ast.Gt: "PY_CMP_GT",
    ast.GtE: "PY_CMP_GE",
    ast.In: "PY_CMP_IN",
    ast.NotIn: "PY_CMP_NOTIN",
    ast.Is: "PY_CMP_IS",
    ast.IsNot: "PY_CMP_ISNOT",
}

WIDGET_KINDS = {
    "Frame": "PY_W_FRAME",
    "Labelframe": "PY_W_FRAME",
    "LabelFrame": "PY_W_FRAME",
    "PanedWindow": "PY_W_FRAME",
    "Label": "PY_W_LABEL",
    "Button": "PY_W_BUTTON",
    "Entry": "PY_W_ENTRY",
    "Text": "PY_W_TEXT",
    "Canvas": "PY_W_CANVAS",
    "Listbox": "PY_W_LISTBOX",
    "Checkbutton": "PY_W_CHECKBUTTON",
    "Radiobutton": "PY_W_RADIOBUTTON",
    "Scale": "PY_W_SCALE",
    "Menu": "PY_W_MENU",
    "Menubutton": "PY_W_MENUBUTTON",
    "Spinbox": "PY_W_SPINBOX",
    "Progressbar": "PY_W_PROGRESSBAR",
    "Separator": "PY_W_SEPARATOR",
    "Treeview": "PY_W_TREEVIEW",
    "Notebook": "PY_W_NOTEBOOK",
    "Scrollbar": "PY_W_SEPARATOR",
}

VAR_CTORS = {"StringVar", "IntVar", "DoubleVar", "BooleanVar"}

MSGBOX_FUNCS = {
    "showinfo": 0,
    "showwarning": 1,
    "showerror": 2,
    "askquestion": 3,
    "askyesno": 4,
    "askyesnocancel": 5,
    "askokcancel": 6,
    "askretrycancel": 7,
}

TK_CONSTANTS = {
    "LEFT": "left", "RIGHT": "right", "TOP": "top", "BOTTOM": "bottom",
    "BOTH": "both", "X": "x", "Y": "y", "NONE": "none",
    "N": "n", "S": "s", "E": "e", "W": "w",
    "NW": "nw", "NE": "ne", "SW": "sw", "SE": "se", "NSEW": "nsew",
    "END": "end", "CENTER": "center", "HORIZONTAL": "horizontal",
    "VERTICAL": "vertical", "DISABLED": "disabled", "NORMAL": "normal",
    "ACTIVE": "active",
}

MODULE_MEMBERS = {
    ("tkinter", "messagebox"), ("tkinter", "ttk"), ("tkinter", "constants"),
    ("tkinter", "simpledialog"), ("os", "path"), ("tkinter", "font"),
}


# --------------------------------------------------------------------------
# scope analysis


def target_names(node):
    """Yield Name ids bound by an assignment/for/with target."""
    if isinstance(node, ast.Name):
        yield node.id
    elif isinstance(node, (ast.Tuple, ast.List)):
        for elt in node.elts:
            yield from target_names(elt)
    elif isinstance(node, ast.Starred):
        yield from target_names(node.value)


class LocalCollector(ast.NodeVisitor):
    """Collect names that are local to one function body."""

    def __init__(self):
        self.locals = set()
        self.globals = set()
        self.nonlocals = set()

    def visit_Global(self, node):
        self.globals.update(node.names)

    def visit_Nonlocal(self, node):
        self.nonlocals.update(node.names)

    def visit_FunctionDef(self, node):
        self.locals.add(node.name)

    visit_AsyncFunctionDef = visit_FunctionDef

    def visit_ClassDef(self, node):
        self.locals.add(node.name)

    def visit_Lambda(self, node):
        # Lambda has its own scope; its body is handled separately.
        pass

    def visit_Name(self, node):
        if isinstance(node.ctx, (ast.Store, ast.Del)):
            self.locals.add(node.id)

    def visit_ExceptHandler(self, node):
        if node.name:
            self.locals.add(node.name)
        self.generic_visit(node)

    def visit_Import(self, node):
        pass

    def visit_ImportFrom(self, node):
        pass

    def visit_NamedExpr(self, node):
        self.visit(node.target)
        self.visit(node.value)


def collect_info(fn):
    c = LocalCollector()
    for a in list(fn.args.posonlyargs) + list(fn.args.args) + list(fn.args.kwonlyargs):
        c.locals.add(a.arg)
    if fn.args.vararg:
        c.locals.add(fn.args.vararg.arg)
    if fn.args.kwarg:
        c.locals.add(fn.args.kwarg.arg)
    for stmt in fn.body:
        c.visit(stmt)
    c.locals -= c.globals
    c.locals -= c.nonlocals
    return c.locals, c.globals


def collect_locals(fn):
    return collect_info(fn)[0]


def collect_free(fn):
    """Names read by ``fn`` that are not local to it (its free variables)."""
    local, glob = collect_info(fn)
    lc = LoadCollector(local)
    for stmt in fn.body:
        lc.visit(stmt)
    return lc.loads - glob


def collect_nonlocals(fn):
    c = LocalCollector()
    for stmt in fn.body:
        c.visit(stmt)
    return c.nonlocals


def collect_cells(fn):
    """Names that a local function must keep in a closure cell.

    A name becomes a cell when a lambda or nested ``def`` used inside ``fn``
    reads it and the name is local to ``fn``.  Such names live in a hidden
    dictionary so that the inner function sees later assignments, matching
    Python's late binding.
    """
    local = collect_locals(fn)
    cells = set()
    for node in ast.walk(fn):
        if node is fn:
            continue
        if isinstance(node, ast.Lambda):
            params = list(node.args.posonlyargs) + list(node.args.args)
            bound = {a.arg for a in params}
            for extra in (node.args.vararg, node.args.kwarg):
                if extra:
                    bound.add(extra.arg)
            lc = LoadCollector(bound)
            lc.visit(node.body)
            cells |= lc.loads & local
        elif isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            cells |= collect_free(node) & local
    return cells


class LoadCollector(ast.NodeVisitor):
    """Collect free Name loads for lambda closure analysis."""

    def __init__(self, bound):
        self.bound = set(bound)
        self.loads = set()

    def visit_Name(self, node):
        if isinstance(node.ctx, ast.Load) and node.id not in self.bound:
            self.loads.add(node.id)

    def visit_Lambda(self, node):
        # a lambda has its own scope; only its free loads propagate outward
        bound = {a.arg for a in list(node.args.posonlyargs) + list(node.args.args)}
        for extra in (node.args.vararg, node.args.kwarg):
            if extra:
                bound.add(extra.arg)
        sub = LoadCollector(bound)
        sub.visit(node.body)
        self.loads |= sub.loads

    def visit_FunctionDef(self, node):
        # names local (or global) in the nested function are not free outside
        _, glob = collect_info(node)
        sub = LoadCollector(collect_locals(node))
        for stmt in node.body:
            sub.visit(stmt)
        self.loads |= sub.loads - glob

    visit_AsyncFunctionDef = visit_FunctionDef


# --------------------------------------------------------------------------
# specs


class FuncSpec:
    def __init__(self, node, cname, display, is_method=False,
                 is_static=False, env_name=None):
        self.node = node
        self.cname = cname
        self.display = display
        self.is_method = is_method
        self.is_static = is_static
        self.env_name = env_name
        self.cells = set()
        self.free = set()
        self.body_c = None
        self.defaults = []           # C expressions for defaults
        self.default_flags = []      # True when the parameter has a default
        self.params = []             # ast.arg
        self.vararg = None
        self.kwarg = None
        self.kwonly = []             # ast.arg
        self.kw_defaults = []        # ast.expr or None, parallel to kwonly
        self.owner_class = None      # class name for methods
        self.bases = []              # base expressions of the owner class
        self.is_lambda = isinstance(node, ast.Lambda)


class ClassSpec:
    def __init__(self, node, cname):
        self.node = node
        self.cname = cname
        self.methods = []            # (name, FuncSpec)


# --------------------------------------------------------------------------
# code generation


class FnEmitter:
    def __init__(self, master, spec):
        self.m = master
        self.spec = spec
        self.node = spec.node
        self.lines = []
        self.indent = 1
        self.tc = 0
        self.lc = 0
        self.handlers = []           # (var, loop_depth)
        self.loops = []              # (break_flag var, id)
        self.withs = []              # (entered var, loop_depth)
        self.exc_vars = []           # exception values available to bare raise
        self.locals = {}             # python name -> c name
        self.local_names = set()
        self.globals = master.module_globals
        self.in_function = True

        if spec.is_lambda:
            fn = spec.node
            all_args = list(fn.args.posonlyargs) + list(fn.args.args)
            self.params = all_args
            self.vararg = fn.args.vararg
            self.kwarg = fn.args.kwarg
            self.kwonly = list(fn.args.kwonlyargs)
            self.local_names = {a.arg for a in all_args}
            self.local_names |= collect_locals(fn) if False else set()
            self.local_names |= {a.arg for a in self.kwonly}
            body = fn.body
            self.body = [ast.Return(value=body)]
            self.is_static = False
            self.env_name = spec.env_name
        else:
            fn = spec.node
            all_args = list(fn.args.posonlyargs) + list(fn.args.args)
            self.params = all_args
            self.vararg = fn.args.vararg
            self.kwarg = fn.args.kwarg
            self.kwonly = list(fn.args.kwonlyargs)
            self.local_names = collect_locals(fn)
            self.body = fn.body
            self.is_static = spec.is_static
            self.env_name = spec.env_name

        if spec.is_method and not spec.is_static:
            self.self_arg = self.params[0].arg if self.params else "self"
            self.bind_params = self.params[1:]
        else:
            self.self_arg = None
            self.bind_params = list(self.params)

        self.cells = set() if spec.is_lambda else set(spec.cells)
        self.free = set(getattr(spec, "free", ()))
        self.nonlocals = set() if spec.is_lambda else collect_nonlocals(fn)
        self.cell_env = "v__cells" if self.cells else None

        self.local_names |= {a.arg for a in self.params}
        if self.vararg:
            self.local_names.add(self.vararg.arg)
        if self.kwarg:
            self.local_names.add(self.kwarg.arg)
        self.has_defaults = any(d is not None for d in spec.defaults)
        pool = NamePool()
        self.local_pool = pool
        for name in sorted(self.local_names):
            pool.take(name)  # reserve sanitized names in a stable order

    # -------------------------------------------------------- optimizations
    def const_value(self, node):
        """Return (True, python_value) for side-effect-free constant expressions."""
        if isinstance(node, ast.Constant):
            v = node.value
            if v is None or isinstance(v, (bool, int, float, str)):
                return (True, v)
            return (False, None)
        if isinstance(node, ast.UnaryOp):
            ok, v = self.const_value(node.operand)
            if not ok or v is None:
                return (False, None)
            try:
                if isinstance(node.op, ast.UAdd): return (True, +v)
                if isinstance(node.op, ast.USub): return (True, -v)
                if isinstance(node.op, ast.Not): return (True, not v)
                if isinstance(node.op, ast.Invert): return (True, ~v)
            except Exception:
                pass
            return (False, None)
        if isinstance(node, ast.BinOp):
            ok, a = self.const_value(node.left)
            if not ok: return (False, None)
            ok, b = self.const_value(node.right)
            if not ok: return (False, None)
            if isinstance(a, str) or isinstance(b, str):
                if isinstance(node.op, ast.Add):
                    if isinstance(a, str) and isinstance(b, str) and len(a) + len(b) < 4096:
                        return (True, a + b)
                if isinstance(node.op, ast.Mult):
                    s, n = (a, b) if isinstance(a, str) else (b, a)
                    if isinstance(s, str) and isinstance(n, int) and not isinstance(n, bool) \
                            and 0 <= n <= 64 and len(s) * n < 4096:
                        return (True, s * n)
                return (False, None)
            if not isinstance(a, (int, float, bool)) or not isinstance(b, (int, float, bool)):
                return (False, None)
            try:
                if isinstance(node.op, ast.Add): r = a + b
                elif isinstance(node.op, ast.Sub): r = a - b
                elif isinstance(node.op, ast.Mult): r = a * b
                elif isinstance(node.op, ast.Div): r = a / b
                elif isinstance(node.op, ast.FloorDiv): r = a // b
                elif isinstance(node.op, ast.Mod): r = a % b
                elif isinstance(node.op, ast.Pow):
                    if isinstance(a, int) and isinstance(b, int) and (b < 0 or b > 64):
                        return (False, None)
                    r = a ** b
                elif isinstance(node.op, ast.LShift):
                    if b < 0 or b > 62: return (False, None)
                    r = a << b
                elif isinstance(node.op, ast.RShift):
                    if b < 0 or b > 62: return (False, None)
                    r = a >> b
                elif isinstance(node.op, ast.BitAnd): r = a & b
                elif isinstance(node.op, ast.BitOr): r = a | b
                elif isinstance(node.op, ast.BitXor): r = a ^ b
                else: return (False, None)
                if isinstance(r, int) and (r > (1 << 63) - 1 or r < -(1 << 63)):
                    return (False, None)
                return (True, r)
            except Exception:
                return (False, None)
        return (False, None)

    def const_expr(self, node):
        ok, v = self.const_value(node)
        if ok:
            return self.gen_const(ast.Constant(value=v))
        return None

    # ---------------------------------------------------------------- output
    def e(self, text=""):
        self.lines.append("    " * self.indent + text if text else "")

    def tmp(self):
        n = "t%d" % self.tc
        self.tc += 1
        return n

    def label(self):
        n = "l%d" % self.lc
        self.lc += 1
        return n

    def local(self, name):
        if name not in self.locals:
            self.locals[name] = "v_" + sanitize(name)
        return self.locals[name]

    # ------------------------------------------------------------- assembly
    def render(self):
        cname = self.spec.cname
        out = []
        out.append("static PyVal *%s(PyVal **args, size_t argc, PyVal *kwargs) {" % cname)

        # local declarations (volatile because of setjmp)
        decl = []
        for name in sorted(self.local_names):
            decl.append("PyVal * volatile %s = NULL;" % self.local(name))
        if self.env_name:
            decl.append("PyVal * volatile %s = NULL;" % self.env_name)
        if self.cell_env:
            decl.append("PyVal * volatile %s = NULL;" % self.cell_env)
        for d in decl:
            out.append("    " + d)

        # bind method self / lambda environment
        if self.env_name:
            out.append("    if (argc < 1) py_raise(PY_EXC_TYPE, \"missing closure environment\");")
            out.append("    py_assign(&%s, args[0]);" % self.env_name)
            bind_args = "args + 1"
            bind_argc = "(argc > 0 ? argc - 1 : 0)"
        elif self.self_arg is not None:
            out.append("    if (argc < 1) py_raise(PY_EXC_TYPE, \"missing self argument\");")
            out.append("    py_assign(&%s, args[0]);" % self.local(self.self_arg))
            bind_args = "args + 1"
            bind_argc = "(argc > 0 ? argc - 1 : 0)"
        else:
            bind_args = "args"
            bind_argc = "argc"

        nparams = len(self.bind_params)
        names = ", ".join(cstr(a.arg) for a in self.bind_params)
        offset = 1 if (self.is_static is False and self.self_arg is not None) else 0
        def_array = "py_defs_" + cname if self.has_defaults else "NULL"
        if self.env_name and self.has_defaults:
            def_array = "defs_c_" + cname
            out.append("    PyVal *%s[%d];" % (def_array, nparams))
            for i in range(nparams):
                src = i + offset
                if src < len(self.spec.defaults) and self.spec.defaults[src] is not None:
                    out.append("    %s[%d] = py_env_get(%s, %s);" %
                               (def_array, i, self.env_name,
                                cstr("__def_%s_%d" % (cname, i))))
                else:
                    out.append("    %s[%d] = NULL;" % (def_array, i))
        if nparams:
            out.append("    {")
            out.append("        PyVal **slots[] = {%s};" %
                       ", ".join("&%s" % self.local(a.arg) for a in self.bind_params))
            if self.vararg:
                out.append("        py_bind_args_ex(%s, %s, kwargs, (const char *[]){%s}, %s, %d, slots, 1);"
                           % (bind_args, bind_argc, names, def_array, nparams))
            else:
                out.append("        py_bind_args(%s, %s, kwargs, (const char *[]){%s}, %s, %d, slots);"
                           % (bind_args, bind_argc, names, def_array, nparams))
            out.append("    }")
        if self.env_name and self.has_defaults:
            out.append("    for (int __di = 0; __di < %d; __di++) py_release(%s[__di]);" %
                       (nparams, def_array))

        if self.vararg:
            out.append("    py_assign(&%s, py_args_tail(%s, %s, %d));" %
                       (self.local(self.vararg.arg), bind_args, bind_argc, nparams))
        exclude_names = names
        if self.kwonly:
            all_names = ", ".join(cstr(a.arg) for a in self.bind_params + self.kwonly)
            exclude_names = all_names
            kwdef_array = "py_kwdefs_" + cname
            if self.env_name and any(d is not None for d in self.spec.kw_defaults):
                kwdef_array = "kwdefs_c_" + cname
                out.append("    PyVal *%s[%d];" % (kwdef_array, len(self.kwonly)))
                for i, d in enumerate(self.spec.kw_defaults):
                    if d is not None:
                        out.append("    %s[%d] = py_env_get(%s, %s);" %
                                   (kwdef_array, i, self.env_name,
                                    cstr("__kwdef_%s_%d" % (cname, i))))
                    else:
                        out.append("    %s[%d] = NULL;" % (kwdef_array, i))
            out.append("    {")
            for i, a in enumerate(self.kwonly):
                out.append("        PyVal *__kw = py_kw_get(kwargs, %s);" % cstr(a.arg))
                if i < len(self.spec.kw_defaults) and self.spec.kw_defaults[i] is not None:
                    out.append("        if (!__kw) __kw = %s[%d];" % (kwdef_array, i))
                out.append("        if (!__kw) py_raise(PY_EXC_TYPE, \"missing required keyword-only argument: %s\");" % a.arg)
                out.append("        py_assign(&%s, __kw);" % self.local(a.arg))
            out.append("    }")
            if self.env_name and kwdef_array == "kwdefs_c_" + cname:
                out.append("    for (int __ki = 0; __ki < %d; __ki++) py_release(%s[__ki]);" %
                           (len(self.kwonly), kwdef_array))
        if self.kwarg:
            out.append("    py_assign(&%s, py_kwargs_tail(kwargs, (const char *[]){%s}, %d));" %
                       (self.local(self.kwarg.arg), exclude_names, nparams + len(self.kwonly)))

        if self.cell_env:
            out.append("    {")
            out.append("        PyVal *__cells = py_dict_new();")
            out.append("        py_assign(&%s, __cells);" % self.cell_env)
            out.append("        py_release(__cells);")
            out.append("    }")
            bound_names = {a.arg for a in self.params}
            if self.vararg:
                bound_names.add(self.vararg.arg)
            if self.kwarg:
                bound_names.add(self.kwarg.arg)
            bound_names |= {a.arg for a in self.kwonly}
            for name in sorted(self.cells & bound_names):
                out.append("    py_dict_put(%s, %s, %s);" %
                           (self.cell_env, cstr(name), self.local(name)))

        self.lines = []
        self.indent = 1
        self.stmts(self.body)

        # implicit return None
        t = self.tmp()
        self.e("PyVal *%s = py_none();" % t)
        self.e("py_retain(%s);" % t)
        self.emit_cleanup(release_locals=True)
        self.e("return %s;" % t)

        out.extend(self.lines)
        out.append("}")
        out.append("")
        return "\n".join(out)

    def emit_cleanup(self, release_locals=False):
        for ent, _ in reversed(self.withs):
            self.e("py_exit(%s);" % ent)
        for h, _ in reversed(self.handlers):
            self.e("py_pop_handler(%s);" % h)
        if release_locals:
            for name in sorted(self.local_names):
                self.e("py_release(%s);" % self.local(name))
                self.e("%s = NULL;" % self.local(name))
            if self.env_name:
                self.e("py_release(%s);" % self.env_name)
                self.e("%s = NULL;" % self.env_name)
            if self.cell_env:
                self.e("py_release(%s);" % self.cell_env)
                self.e("%s = NULL;" % self.cell_env)

    # ------------------------------------------------------------ statements
    def stmts(self, body):
        for stmt in body:
            self.stmt(stmt)

    def stmt(self, node):
        if isinstance(node, ast.Expr):
            if isinstance(node.value, ast.Constant) and isinstance(node.value.value, str):
                return  # docstring
            t = self.gen(node.value)
            self.e("py_release(%s);" % t)
        elif isinstance(node, ast.Assign):
            self.stmt_assign(node)
        elif isinstance(node, ast.AnnAssign):
            if node.value is not None:
                self.stmt_assign(ast.Assign(targets=[node.target], value=node.value))
        elif isinstance(node, ast.AugAssign):
            self.stmt_augassign(node)
        elif isinstance(node, ast.If):
            self.stmt_if(node)
        elif isinstance(node, ast.While):
            self.stmt_while(node)
        elif isinstance(node, ast.For):
            self.stmt_for(node)
        elif isinstance(node, ast.Break):
            self.emit_break()
        elif isinstance(node, ast.Continue):
            self.emit_continue()
        elif isinstance(node, ast.Return):
            self.stmt_return(node)
        elif isinstance(node, ast.Pass):
            pass
        elif isinstance(node, ast.Raise):
            self.stmt_raise(node)
        elif isinstance(node, ast.Assert):
            self.stmt_assert(node)
        elif isinstance(node, ast.Try):
            self.stmt_try(node)
        elif isinstance(node, ast.With):
            self.stmt_with(node)
        elif isinstance(node, (ast.Import, ast.ImportFrom)):
            self.m.note_import(node)
        elif isinstance(node, ast.Delete):
            self.stmt_delete(node)
        elif isinstance(node, (ast.Global, ast.Nonlocal)):
            pass
        elif isinstance(node, ast.FunctionDef):
            if not self.in_function:
                self.m.define_function(node, self)
            elif id(node) in self.m.func_map:
                self.m.define_nested_function(node, self)
            else:
                warn("nested function '%s' is not supported" % node.name, node)
        elif isinstance(node, ast.ClassDef):
            if not self.in_function:
                self.m.define_class(node, self)
            else:
                warn("nested class '%s' is not supported" % node.name, node)
        elif isinstance(node, (ast.AsyncFunctionDef, ast.AsyncFor, ast.AsyncWith, ast.Await)):
            warn("async syntax is not supported", node)
        else:
            warn("unsupported statement %s" % type(node).__name__, node)

    def stmt_assign(self, node):
        t = self.gen(node.value)
        for target in node.targets:
            self.assign(target, t)
        self.e("py_release(%s);" % t)

    def stmt_augassign(self, node):
        op = BIN_OPS.get(type(node.op))
        if not op:
            warn("unsupported augmented operator", node)
            return
        target = node.target
        if isinstance(target, ast.Name):
            old = self.load_name(target.id)
            val = self.gen(node.value)
            r = self.tmp()
            self.e("PyVal *%s = py_binop(%s, %s, %s);" % (r, op, old, val))
            self.e("py_release(%s);" % old)
            self.e("py_release(%s);" % val)
            self.store_name(target.id, r)
            self.e("py_release(%s);" % r)
        elif isinstance(target, ast.Attribute):
            obj = self.gen(target.value)
            old = self.tmp()
            self.e("PyVal *%s = py_getattr(%s, %s);" % (old, obj, cstr(target.attr)))
            val = self.gen(node.value)
            r = self.tmp()
            self.e("PyVal *%s = py_binop(%s, %s, %s);" % (r, op, old, val))
            self.e("py_release(%s); py_release(%s);" % (old, val))
            self.e("py_setattr(%s, %s, %s);" % (obj, cstr(target.attr), r))
            self.e("py_release(%s); py_release(%s);" % (r, obj))
        elif isinstance(target, ast.Subscript):
            obj = self.gen(target.value)
            key = self.gen_slice(target.slice)
            old = self.tmp()
            self.e("PyVal *%s = py_getitem(%s, %s);" % (old, obj, key))
            val = self.gen(node.value)
            r = self.tmp()
            self.e("PyVal *%s = py_binop(%s, %s, %s);" % (r, op, old, val))
            self.e("py_release(%s); py_release(%s);" % (old, val))
            self.e("py_setitem(%s, %s, %s);" % (obj, key, r))
            self.e("py_release(%s); py_release(%s); py_release(%s);" % (r, obj, key))
        else:
            warn("unsupported augmented assignment target", node)

    def stmt_if(self, node):
        ok, cv = self.const_value(node.test)
        if ok:
            self.stmts(node.body if bool(cv) else node.orelse)
            return
        cond = self.gen(node.test)
        self.e("if (py_truth(%s)) {" % cond)
        self.e("    py_release(%s);" % cond)
        self.indent += 1
        self.stmts(node.body)
        self.indent -= 1
        if node.orelse:
            self.e("} else {")
            self.indent += 1
            self.stmts(node.orelse)
            self.indent -= 1
        self.e("}")

    def stmt_while(self, node):
        ok, cv = self.const_value(node.test)
        if ok:
            if not bool(cv):
                self.stmts(node.orelse)
                return
            brk = self.label()
            self.e("int brk_%s = 0;" % brk)
            self.e("while (1) {")
            self.indent += 1
            self.loops.append(brk)
            self.stmts(node.body)
            self.loops.pop()
            self.indent -= 1
            self.e("}")
            return
        brk = self.label()
        self.e("int brk_%s = 0;" % brk)
        self.e("while (1) {")
        self.indent += 1
        cond = self.gen(node.test)
        self.e("int c_%s = py_truth(%s);" % (brk, cond))
        self.e("py_release(%s);" % cond)
        self.e("if (!c_%s) break;" % brk)
        self.loops.append(brk)
        self.stmts(node.body)
        self.loops.pop()
        self.indent -= 1
        self.e("}")
        if node.orelse:
            self.e("if (!brk_%s) {" % brk)
            self.indent += 1
            self.stmts(node.orelse)
            self.indent -= 1
            self.e("}")

    def stmt_for(self, node):
        brk = self.label()
        it = self.tmp()
        item = self.tmp()
        iterable = self.gen(node.iter)
        self.e("PyVal *%s = py_iter(%s);" % (it, iterable))
        self.e("py_release(%s);" % iterable)
        self.e("PyVal *%s = NULL;" % item)
        self.e("int brk_%s = 0;" % brk)
        self.e("while (py_iter_next(%s, &%s)) {" % (it, item))
        self.indent += 1
        self.assign(node.target, item)
        self.loops.append(brk)
        self.stmts(node.body)
        self.loops.pop()
        self.indent -= 1
        self.e("}")
        self.e("py_release(%s);" % item)
        self.e("py_release(%s);" % it)
        if node.orelse:
            self.e("if (!brk_%s) {" % brk)
            self.indent += 1
            self.stmts(node.orelse)
            self.indent -= 1
            self.e("}")

    def emit_break(self):
        if not self.loops:
            warn("break outside loop")
            return
        depth = len(self.loops)
        for ent, ld in reversed(self.withs):
            if ld >= depth:
                self.e("py_exit(%s);" % ent)
        for h, ld in reversed(self.handlers):
            if ld >= depth:
                self.e("py_pop_handler(%s);" % h)
        self.e("brk_%s = 1;" % self.loops[-1])
        self.e("break;")

    def emit_continue(self):
        if not self.loops:
            warn("continue outside loop")
            return
        depth = len(self.loops)
        for ent, ld in reversed(self.withs):
            if ld >= depth:
                self.e("py_exit(%s);" % ent)
        for h, ld in reversed(self.handlers):
            if ld >= depth:
                self.e("py_pop_handler(%s);" % h)
        self.e("continue;")

    def stmt_return(self, node):
        t = self.gen(node.value) if node.value is not None else None
        if t is None:
            t = self.tmp()
            self.e("PyVal *%s = py_none();" % t)
            self.e("py_retain(%s);" % t)
        self.emit_cleanup(release_locals=True)
        self.e("return %s;" % t)

    def stmt_raise(self, node):
        if node.exc is None:
            if self.exc_vars:
                self.e("py_raise_val(%s);" % self.exc_vars[-1])
            else:
                self.e("py_reraise();")
            return
        exc = node.exc
        if isinstance(exc, ast.Call) and isinstance(exc.func, ast.Name) and exc.func.id in EXC_KINDS:
            kind = EXC_KINDS[exc.func.id]
            e = self.tmp()
            if exc.args:
                a = self.gen(exc.args[0])
                self.e("PyVal *%s = py_make_exc_val(%s, %s);" % (e, kind, a))
                self.e("py_release(%s);" % a)
            else:
                self.e("PyVal *%s = py_make_exc(%s, \"\");" % (e, kind))
            self.e("py_raise_val(%s);" % e)
        elif isinstance(exc, ast.Name) and exc.id in EXC_KINDS:
            self.e("py_raise(%s, \"\");" % EXC_KINDS[exc.id])
        else:
            t = self.gen(exc)
            self.e("py_raise_val(%s);" % t)

    def stmt_assert(self, node):
        c = self.gen(node.test)
        self.e("if (!py_truth(%s)) {" % c)
        self.e("    py_release(%s);" % c)
        if node.msg:
            a = self.gen(node.msg)
            e = self.tmp()
            self.e("    PyVal *%s = py_make_exc_val(PY_EXC_ASSERTION, %s);" % (e, a))
            self.e("    py_release(%s);" % a)
            self.e("    py_raise_val(%s);" % e)
        else:
            self.e("    py_raise(PY_EXC_ASSERTION, \"assertion failed\");")
        self.e("}")
        self.e("py_release(%s);" % c)

    def stmt_try(self, node):
        if node.finalbody and not node.handlers:
            self.gen_try_finally(node)
            return
        if node.finalbody and node.handlers:
            inner = ast.Try(body=node.body, handlers=node.handlers,
                            orelse=node.orelse, finalbody=[])
            outer = ast.Try(body=[], handlers=[], orelse=[], finalbody=node.finalbody)
            # emit inner inside a finally guard manually
            self.gen_finally_wrapper(inner, node.finalbody)
            return

        h = "h%d" % self.lc
        self.lc += 1
        self.e("PyJmp *%s = py_push_handler();" % h)
        self.e("if (setjmp(%s->buf) == 0) {" % h)
        self.indent += 1
        self.handlers.append((h, len(self.loops)))
        self.stmts(node.body)
        self.handlers.pop()
        self.e("py_pop_handler(%s);" % h)
        if node.orelse:
            self.stmts(node.orelse)
        self.indent -= 1
        self.e("} else {")
        self.indent += 1
        self.e("py_pop_handler(%s);" % h)
        ev = "e%d" % self.lc
        self.lc += 1
        self.e("PyVal *%s = py_exc_current();" % ev)
        self.e("py_retain(%s);" % ev)
        self.e("py_exc_clear();")
        self.exc_vars.append(ev)
        first = True
        bare = False
        for handler in node.handlers:
            if handler.type is None:
                if not first:
                    self.e("else {")
                self.indent += 1
                if handler.name:
                    self.e("py_assign(&%s, %s);" % (self.slot_for(handler.name), ev))
                self.stmts(handler.body)
                self.indent -= 1
                if not first:
                    self.e("}")
                bare = True
                break
            cond = self.exc_condition(handler.type)
            if first:
                self.e("if (%s) {" % cond)
                first = False
            else:
                self.e("else if (%s) {" % cond)
            self.indent += 1
            if handler.name:
                self.e("py_assign(&%s, %s);" % (self.slot_for(handler.name), ev))
            self.stmts(handler.body)
            self.indent -= 1
            self.e("}")
        if not bare:
            if first:
                self.e("py_raise_val(%s);" % ev)
            else:
                self.e("else { py_raise_val(%s); }" % ev)
        self.exc_vars.pop()
        self.e("py_release(%s);" % ev)
        self.indent -= 1
        self.e("}")

    def exc_condition(self, node):
        if isinstance(node, ast.Name):
            return "py_exc_matches_name(%s, %s)" % (self.exc_vars[-1], cstr(node.id))
        if isinstance(node, ast.Attribute):
            return "py_exc_matches_name(%s, %s)" % (self.exc_vars[-1], cstr(node.attr))
        if isinstance(node, ast.Tuple):
            parts = [self.exc_condition(e) for e in node.elts]
            return " || ".join(parts) if parts else "1"
        return "1"

    def gen_finally_wrapper(self, protected, finalbody):
        h = "h%d" % self.lc
        self.lc += 1
        self.e("PyJmp *%s = py_push_handler();" % h)
        self.e("if (setjmp(%s->buf) == 0) {" % h)
        self.indent += 1
        self.handlers.append((h, len(self.loops)))
        if isinstance(protected, ast.Try):
            self.stmt_try(protected)
        else:
            self.stmts(protected)
        self.handlers.pop()
        self.e("py_pop_handler(%s);" % h)
        self.stmts(finalbody)
        self.indent -= 1
        self.e("} else {")
        self.indent += 1
        self.e("py_pop_handler(%s);" % h)
        ev = "e%d" % self.lc
        self.lc += 1
        self.e("PyVal *%s = py_exc_current();" % ev)
        self.e("py_retain(%s);" % ev)
        self.e("py_exc_clear();")
        self.stmts(finalbody)
        self.e("py_raise_val(%s);" % ev)
        self.indent -= 1
        self.e("}")

    def gen_try_finally(self, node):
        self.gen_finally_wrapper(node.body, node.finalbody)

    def stmt_with(self, node):
        entered = []
        for item in node.items:
            mgr = self.gen(item.context_expr)
            ent = self.tmp()
            self.e("PyVal *%s = py_enter(%s);" % (ent, mgr))
            self.e("py_release(%s);" % mgr)
            entered.append(ent)
            if item.optional_vars is not None:
                self.assign(item.optional_vars, ent)
        h = "h%d" % self.lc
        self.lc += 1
        self.e("PyJmp *%s = py_push_handler();" % h)
        self.e("if (setjmp(%s->buf) == 0) {" % h)
        self.indent += 1
        self.handlers.append((h, len(self.loops)))
        for ent in entered:
            self.withs.append((ent, len(self.loops)))
        self.stmts(node.body)
        for _ in entered:
            self.withs.pop()
        self.handlers.pop()
        self.e("py_pop_handler(%s);" % h)
        for ent in reversed(entered):
            self.e("py_exit(%s);" % ent)
        self.indent -= 1
        self.e("} else {")
        self.indent += 1
        self.e("py_pop_handler(%s);" % h)
        ev = "e%d" % self.lc
        self.lc += 1
        self.e("PyVal *%s = py_exc_current();" % ev)
        self.e("py_retain(%s);" % ev)
        self.e("py_exc_clear();")
        for ent in reversed(entered):
            self.e("py_exit(%s);" % ent)
        self.e("py_raise_val(%s);" % ev)
        self.indent -= 1
        self.e("}")
        for ent in reversed(entered):
            self.e("py_release(%s);" % ent)

    def stmt_delete(self, node):
        for target in node.targets:
            if isinstance(target, ast.Name):
                slot = self.slot_for(target.id)
                self.e("py_release(%s);" % slot)
                self.e("%s = NULL;" % slot)
            elif isinstance(target, ast.Attribute):
                obj = self.gen(target.value)
                self.e("py_delattr(%s, %s);" % (obj, cstr(target.attr)))
                self.e("py_release(%s);" % obj)
            elif isinstance(target, ast.Subscript):
                obj = self.gen(target.value)
                key = self.gen_slice(target.slice)
                self.e("py_delitem(%s, %s);" % (obj, key))
                self.e("py_release(%s); py_release(%s);" % (obj, key))
            else:
                warn("unsupported delete target", node)

    # -------------------------------------------------------- name handling
    def slot_for(self, name):
        if name in self.local_names:
            return self.local(name)
        return self.m.global_c(name)

    def load_name(self, name):
        t = self.tmp()
        if name in self.free:
            self.e("PyVal *%s = py_env_get(%s, %s);" % (t, self.env_name, cstr(name)))
            return t
        if name in self.cells:
            self.e("PyVal *%s = py_env_get(%s, %s);" % (t, self.cell_env, cstr(name)))
            return t
        if name in self.local_names:
            self.e("PyVal *%s = py_load(&%s, %s);" % (t, self.local(name), cstr(name)))
            return t
        if name in self.globals or name in self.m.global_decls:
            self.e("PyVal *%s = py_load(&%s, %s);" % (t, self.m.global_c(name), cstr(name)))
            return t
        if name == "__name__":
            self.e("PyVal *%s = py_str(\"__main__\");" % t)
            return t
        if name == "__file__":
            self.e("PyVal *%s = py_str(%s);" % (t, cstr(self.m.filename)))
            return t
        if name == "__doc__":
            self.e("PyVal *%s = py_none(); py_retain(%s);" % (t, t))
            return t
        if name in self.m.module_aliases:
            module = self.m.module_aliases[name]
            self.e("PyVal *%s = py_str(%s);" % (t, cstr(module)))
            return t
        if name in self.m.direct:
            module, member = self.m.direct[name]
            self.e("PyVal *%s = py_module_attr(%s, %s);" % (t, cstr(module), cstr(member)))
            return t
        if name in TK_CONSTANTS:
            self.e("PyVal *%s = py_str(%s);" % (t, cstr(TK_CONSTANTS[name])))
            return t
        if name in self.m.star_modules:
            module = self.m.star_modules[0]
            self.e("PyVal *%s = py_module_attr(%s, %s);" % (t, cstr(module), cstr(name)))
            return t
        if name in BUILTIN_NAMES:
            self.e("PyVal *%s = py_builtin_lookup(%s);" % (t, cstr(name)))
            return t
        if name in EXC_KINDS:
            self.e("PyVal *%s = py_str(%s);" % (t, cstr(name)))
            return t
        self.e("PyVal *%s = NULL;" % t)
        self.e("py_raise(PY_EXC_NAME, %s);" % cstr("name '%s' is not defined" % name))
        self.e("%s = py_none(); py_retain(%s);" % (t, t))
        return t

    def store_name(self, name, value):
        if name in self.nonlocals and name in self.free:
            self.e("py_dict_put(%s, %s, %s);" % (self.env_name, cstr(name), value))
        elif name in self.cells:
            self.e("py_dict_put(%s, %s, %s);" % (self.cell_env, cstr(name), value))
        elif name in self.local_names:
            self.e("py_assign(&%s, %s);" % (self.local(name), value))
        else:
            self.e("py_assign(&%s, %s);" % (self.m.global_c(name), value))

    # ------------------------------------------------------------- targets
    def assign(self, target, value):
        if isinstance(target, ast.Name):
            self.store_name(target.id, value)
        elif isinstance(target, ast.Attribute):
            obj = self.gen(target.value)
            self.e("py_setattr(%s, %s, %s);" % (obj, cstr(target.attr), value))
            self.e("py_release(%s);" % obj)
        elif isinstance(target, ast.Subscript):
            obj = self.gen(target.value)
            key = self.gen_slice(target.slice)
            self.e("py_setitem(%s, %s, %s);" % (obj, key, value))
            self.e("py_release(%s); py_release(%s);" % (obj, key))
        elif isinstance(target, (ast.Tuple, ast.List)):
            self.assign_unpack(target.elts, value)
        elif isinstance(target, ast.Starred):
            self.assign(target.value, value)
        else:
            warn("unsupported assignment target", target)

    def assign_unpack(self, elts, value):
        if any(isinstance(e, ast.Starred) for e in elts):
            # a, *b, c = value
            items = self.tmp()
            star = next(i for i, e in enumerate(elts) if isinstance(e, ast.Starred))
            suffix = len(elts) - star - 1
            min_n = len(elts) - 1
            self.e("PyVal *%s = py_builtin_list((PyVal *[]){%s}, 1, NULL);" % (items, value))
            self.e("if (%s->u.seq.len < (size_t)%d) py_raise(PY_EXC_VALUE, \"not enough values to unpack\");" % (items, min_n))
            for i in range(star):
                tmp = self.tmp()
                self.e("PyVal *%s = %s->u.seq.items[%d];" % (tmp, items, i))
                self.e("py_retain(%s);" % tmp)
                self.assign(elts[i], tmp)
                self.e("py_release(%s);" % tmp)
            mid = self.tmp()
            self.e("PyVal *%s = py_list_new();" % mid)
            idxv = self.tmp()
            self.e("for (size_t %s = (size_t)%d; %s + (size_t)%d < %s->u.seq.len; %s++) {" % (idxv, star, idxv, suffix, items, idxv))
            self.indent += 1
            self.e("py_list_append(%s, %s->u.seq.items[%s]);" % (mid, items, idxv))
            self.indent -= 1
            self.e("}")
            self.assign(elts[star].value, mid)
            self.e("py_release(%s);" % mid)
            for j in range(suffix):
                tmp = self.tmp()
                off = suffix - j
                self.e("PyVal *%s = %s->u.seq.items[%s->u.seq.len - (size_t)%d];" % (tmp, items, items, off))
                self.e("py_retain(%s);" % tmp)
                self.assign(elts[star + 1 + j], tmp)
                self.e("py_release(%s);" % tmp)
            self.e("py_release(%s);" % items)
            return
        n = len(elts)
        slots = []
        for i, elt in enumerate(elts):
            if not isinstance(elt, ast.Name):
                # nested tuple/list
                tmp = "u%d_%s" % (i, self.tmp())
                self.e("PyVal *%s = NULL;" % tmp)
                slots.append((elt, tmp))
            else:
                tmp = "u%d_%s" % (i, self.tmp())
                self.e("PyVal *%s = NULL;" % tmp)
                slots.append((elt, tmp))
        self.e("{")
        self.indent += 1
        self.e("PyVal **slot_arr[] = {%s};" % ", ".join("&%s" % s[1] for s in slots))
        self.e("py_unpack(%s, slot_arr, %d);" % (value, n))
        for elt, tmp in slots:
            self.assign(elt, tmp)
            self.e("py_release(%s);" % tmp)
        self.indent -= 1
        self.e("}")

    # ----------------------------------------------------------- expressions
    def gen(self, node):
        if isinstance(node, ast.Constant):
            return self.gen_const(node)
        if isinstance(node, ast.Name):
            return self.load_name(node.id)
        if isinstance(node, ast.Attribute):
            return self.gen_attr(node)
        if isinstance(node, ast.Subscript):
            return self.gen_subscript(node.value, node.slice)
        if isinstance(node, ast.Call):
            return self.gen_call(node)
        if isinstance(node, ast.BinOp):
            folded = self.const_expr(node)
            if folded is not None:
                return folded
            op = BIN_OPS.get(type(node.op))
            if not op:
                warn("unsupported binary operator", node)
                return self.const_none()
            a = self.gen(node.left)
            b = self.gen(node.right)
            t = self.tmp()
            self.e("PyVal *%s = py_binop(%s, %s, %s);" % (t, op, a, b))
            self.e("py_release(%s); py_release(%s);" % (a, b))
            return t
        if isinstance(node, ast.UnaryOp):
            return self.gen_unary(node)
        if isinstance(node, ast.BoolOp):
            return self.gen_boolop(node)
        if isinstance(node, ast.Compare):
            return self.gen_compare(node)
        if isinstance(node, ast.IfExp):
            return self.gen_ifexp(node)
        if isinstance(node, ast.List):
            return self.gen_seq(node.elts, "py_list_from", allow_star=True)
        if isinstance(node, ast.Tuple):
            return self.gen_seq(node.elts, "py_tuple_from", allow_star=True)
        if isinstance(node, ast.Set):
            return self.gen_seq(node.elts, "py_set_from", allow_star=False)
        if isinstance(node, ast.Dict):
            return self.gen_dict(node)
        if isinstance(node, ast.ListComp):
            return self.gen_comp(node, "list")
        if isinstance(node, ast.SetComp):
            return self.gen_comp(node, "set")
        if isinstance(node, ast.DictComp):
            return self.gen_comp(node, "dict")
        if isinstance(node, ast.GeneratorExp):
            lst = self.gen_comp(node, "list")
            t = self.tmp()
            self.e("PyVal *%s = py_iter(%s);" % (t, lst))
            self.e("py_release(%s);" % lst)
            return t
        if isinstance(node, ast.JoinedStr):
            return self.gen_fstring(node)
        if isinstance(node, ast.FormattedValue):
            spec = ""
            if isinstance(node.format_spec, ast.JoinedStr):
                spec = self.static_string(node.format_spec)
            v = self.gen(node.value)
            t = self.tmp()
            conv = 0
            if node.conversion == ord("r"):
                conv = 1
            elif node.conversion == ord("s"):
                conv = 2
            self.e("PyVal *%s = py_format_value(%s, %s, %d);" % (t, v, cstr(spec), conv))
            self.e("py_release(%s);" % v)
            return t
        if isinstance(node, ast.Lambda):
            return self.gen_lambda(node)
        if isinstance(node, ast.NamedExpr):
            v = self.gen(node.value)
            self.assign(node.target, v)
            self.e("py_retain(%s);" % v)
            return v
        if isinstance(node, ast.Slice):
            warn("bare slice is not a value", node)
            return self.const_none()
        if isinstance(node, ast.Starred):
            return self.gen(node.value)
        warn("unsupported expression %s" % type(node).__name__, node)
        return self.const_none()

    def const_none(self):
        t = self.tmp()
        self.e("PyVal *%s = py_none();" % t)
        self.e("py_retain(%s);" % t)
        return t

    def gen_const(self, node):
        t = self.tmp()
        v = node.value
        if v is None:
            self.e("PyVal *%s = py_none();" % t)
            self.e("py_retain(%s);" % t)
        elif v is True or v is False:
            self.e("PyVal *%s = py_bool(%d);" % (t, 1 if v else 0))
            self.e("py_retain(%s);" % t)
        elif isinstance(v, int):
            if v > (1 << 63) - 1 or v < -(1 << 63):
                warn("integer constant does not fit in 64 bits", node)
            self.e("PyVal *%s = py_int(INT64_C(%d));" % (t, v))
        elif isinstance(v, float):
            if v != v:
                self.e("PyVal *%s = py_float(NAN);" % t)
            elif v == float("inf"):
                self.e("PyVal *%s = py_float(INFINITY);" % t)
            elif v == float("-inf"):
                self.e("PyVal *%s = py_float(-INFINITY);" % t)
            else:
                self.e("PyVal *%s = py_float(%s);" % (t, repr(v)))
        elif isinstance(v, str):
            self.e("PyVal *%s = py_str(%s);" % (t, cstr(v)))
        elif isinstance(v, bytes):
            self.e("PyVal *%s = py_str(%s);" % (t, cstr(v.decode("utf-8", "replace"))))
        else:
            warn("unsupported constant %r" % (v,), node)
            self.e("PyVal *%s = py_none();" % t)
            self.e("py_retain(%s);" % t)
        return t

    def gen_unary(self, node):
        folded = self.const_expr(node)
        if folded is not None:
            return folded
        a = self.gen(node.operand)
        if isinstance(node.op, ast.Not):
            t = self.tmp()
            self.e("PyVal *%s = py_bool(!py_truth(%s));" % (t, a))
            self.e("py_retain(%s);" % t)
            self.e("py_release(%s);" % a)
            return t
        op = 0
        if isinstance(node.op, ast.USub):
            op = 0
        elif isinstance(node.op, ast.UAdd):
            op = 1
        elif isinstance(node.op, ast.Invert):
            op = 2
        t = self.tmp()
        self.e("PyVal *%s = py_unaryop(%d, %s);" % (t, op, a))
        self.e("py_release(%s);" % a)
        return t

    def gen_boolop(self, node):
        is_and = isinstance(node.op, ast.And)
        t = "b%d" % self.tc
        self.tc += 1
        self.e("PyVal *%s = NULL;" % t)
        self.e("{")
        self.indent += 1

        def rec(i):
            v = self.gen(node.values[i])
            if i == len(node.values) - 1:
                self.e("%s = %s;" % (t, v))
                return
            if is_and:
                self.e("if (py_truth(%s)) {" % v)
                self.indent += 1
                self.e("py_release(%s);" % v)
                rec(i + 1)
                self.indent -= 1
                self.e("} else {")
                self.indent += 1
                self.e("%s = %s;" % (t, v))
                self.indent -= 1
                self.e("}")
            else:
                self.e("if (py_truth(%s)) {" % v)
                self.indent += 1
                self.e("%s = %s;" % (t, v))
                self.indent -= 1
                self.e("} else {")
                self.indent += 1
                self.e("py_release(%s);" % v)
                rec(i + 1)
                self.indent -= 1
                self.e("}")

        rec(0)
        self.indent -= 1
        self.e("}")
        return t

    def gen_compare(self, node):
        if len(node.ops) == 1:
            ok, a = self.const_value(node.left)
            ok2, b = self.const_value(node.comparators[0])
            if ok and ok2:
                try:
                    op = node.ops[0]
                    if isinstance(op, ast.Eq): r = a == b
                    elif isinstance(op, ast.NotEq): r = a != b
                    elif isinstance(op, ast.Lt): r = a < b
                    elif isinstance(op, ast.LtE): r = a <= b
                    elif isinstance(op, ast.Gt): r = a > b
                    elif isinstance(op, ast.GtE): r = a >= b
                    else: r = None
                    if r is not None:
                        return self.gen_const(ast.Constant(value=bool(r)))
                except Exception:
                    pass
        vals = [self.gen(node.left)]
        for c in node.comparators:
            vals.append(self.gen(c))
        conds = []
        for i, op in enumerate(node.ops):
            conds.append("py_cmp(%s, %s, %s)" %
                         (CMP_OPS.get(type(op), "PY_CMP_EQ"), vals[i], vals[i + 1]))
        t = self.tmp()
        self.e("PyVal *%s = py_bool(%s);" % (t, " && ".join(conds) if conds else "1"))
        self.e("py_retain(%s);" % t)
        for v in vals:
            self.e("py_release(%s);" % v)
        return t

    def gen_ifexp(self, node):
        ok, cv = self.const_value(node.test)
        if ok:
            return self.gen(node.body if bool(cv) else node.orelse)
        c = self.gen(node.test)
        t = self.tmp()
        self.e("PyVal *%s = NULL;" % t)
        self.e("if (py_truth(%s)) {" % c)
        self.indent += 1
        self.e("py_release(%s);" % c)
        a = self.gen(node.body)
        self.e("%s = %s;" % (t, a))
        self.indent -= 1
        self.e("} else {")
        self.indent += 1
        self.e("py_release(%s);" % c)
        b = self.gen(node.orelse)
        self.e("%s = %s;" % (t, b))
        self.indent -= 1
        self.e("}")
        return t

    def gen_seq(self, elts, func, allow_star=False):
        parts = []
        temps = []
        for elt in elts:
            if isinstance(elt, ast.Starred):
                if not allow_star:
                    warn("starred expression not supported here", elt)
                    continue
                # build dynamically
                return self.gen_seq_star(elts, func)
            v = self.gen(elt)
            temps.append(v)
            parts.append(v)
        t = self.tmp()
        if parts:
            self.e("PyVal *%s = %s((PyVal *[]){%s}, %d);" % (t, func, ", ".join(parts), len(parts)))
        else:
            if func == "py_set_from":
                self.e("PyVal *%s = py_set_new();" % t)
            elif func == "py_tuple_from":
                self.e("PyVal *%s = py_tuple_from(NULL, 0);" % t)
            else:
                self.e("PyVal *%s = py_list_new();" % t)
        for v in temps:
            self.e("py_release(%s);" % v)
        return t

    def gen_seq_star(self, elts, func):
        lst = self.tmp()
        self.e("PyVal *%s = py_list_new();" % lst)
        for elt in elts:
            if isinstance(elt, ast.Starred):
                v = self.gen(elt.value)
                it = self.tmp()
                item = self.tmp()
                self.e("PyVal *%s = py_iter(%s);" % (it, v))
                self.e("py_release(%s);" % v)
                self.e("PyVal *%s = NULL;" % item)
                self.e("while (py_iter_next(%s, &%s)) py_list_append(%s, %s);" % (it, item, lst, item))
                self.e("py_release(%s); py_release(%s);" % (item, it))
            else:
                v = self.gen(elt)
                self.e("py_list_append(%s, %s);" % (lst, v))
                self.e("py_release(%s);" % v)
        t = self.tmp()
        if func == "py_tuple_from":
            self.e("PyVal *%s = py_tuple_from(%s->u.seq.items, %s->u.seq.len);" % (t, lst, lst))
            self.e("py_release(%s);" % lst)
        elif func == "py_set_from":
            self.e("PyVal *%s = py_set_from(%s->u.seq.items, %s->u.seq.len);" % (t, lst, lst))
            self.e("py_release(%s);" % lst)
        else:
            self.e("PyVal *%s = %s;" % (t, lst))
        return t

    def gen_dict(self, node):
        d = self.tmp()
        self.e("PyVal *%s = py_dict_new();" % d)
        for k, v in zip(node.keys, node.values):
            if k is None:
                src = self.gen(v)
                self.e("py_dict_update(%s, %s);" % (d, src))
                self.e("py_release(%s);" % src)
                continue
            kv = self.gen(k)
            vv = self.gen(v)
            if isinstance(k, ast.Constant) and isinstance(k.value, str):
                self.e("py_dict_put(%s, %s, %s);" % (d, cstr(k.value), vv))
            else:
                self.e("py_dict_strkey(%s, %s, %s);" % (d, kv, vv))
            self.e("py_release(%s); py_release(%s);" % (kv, vv))
        return d

    # comprehension -------------------------------------------------------
    def gen_comp(self, node, kind):
        acc = self.tmp()
        if kind == "list":
            self.e("PyVal *%s = py_list_new();" % acc)
        elif kind == "set":
            self.e("PyVal *%s = py_set_new();" % acc)
        else:
            self.e("PyVal *%s = py_dict_new();" % acc)

        def loops(i):
            if i >= len(node.generators):
                if kind == "dict":
                    k = self.gen(node.key)
                    v = self.gen(node.value)
                    self.e("py_dict_strkey(%s, %s, %s);" % (acc, k, v))
                    self.e("py_release(%s); py_release(%s);" % (k, v))
                else:
                    v = self.gen(node.elt)
                    if kind == "list":
                        self.e("py_list_append(%s, %s);" % (acc, v))
                    else:
                        self.e("py_set_add(%s, %s);" % (acc, v))
                    self.e("py_release(%s);" % v)
                return
            gen = node.generators[i]
            iterable = self.gen(gen.iter)
            it = self.tmp()
            item = self.tmp()
            self.e("PyVal *%s = py_iter(%s);" % (it, iterable))
            self.e("py_release(%s);" % iterable)
            self.e("PyVal *%s = NULL;" % item)
            self.e("while (py_iter_next(%s, &%s)) {" % (it, item))
            self.indent += 1
            self.assign(gen.target, item)
            for cond in gen.ifs:
                c = self.gen(cond)
                self.e("if (!py_truth(%s)) { py_release(%s); continue; }" % (c, c))
                self.e("py_release(%s);" % c)
            loops(i + 1)
            self.indent -= 1
            self.e("}")
            self.e("py_release(%s); py_release(%s);" % (item, it))

        loops(0)
        return acc

    # f-strings -----------------------------------------------------------
    def static_string(self, joined):
        parts = []
        for v in joined.values:
            if isinstance(v, ast.Constant) and isinstance(v.value, str):
                parts.append(v.value)
            else:
                return ""
        return "".join(parts)

    def gen_fstring(self, node):
        parts = []
        for part in node.values:
            if isinstance(part, ast.Constant):
                s = self.tmp()
                self.e("PyVal *%s = py_str(%s);" % (s, cstr(str(part.value))))
            else:
                s = self.gen(part)
            parts.append(s)
        pa = self.tmp()
        if parts:
            self.e("PyVal *%s[] = {%s};" % (pa, ", ".join(parts)))
        else:
            self.e("PyVal *%s[] = {NULL};" % pa)
        r = self.tmp()
        self.e("PyVal *%s = py_str_join(%s, %d);" % (r, pa, len(parts)))
        for s in parts:
            self.e("py_release(%s);" % s)
        return r

    # attributes ----------------------------------------------------------
    def module_of(self, node):
        if isinstance(node, ast.Name) and node.id in self.m.module_aliases:
            return self.m.module_aliases[node.id]
        if isinstance(node, ast.Attribute):
            base = self.module_of(node.value)
            if base:
                return base + "." + node.attr
        return None

    def gen_attr(self, node):
        module = self.module_of(node.value)
        if module:
            if (node.attr in WIDGET_KINDS or node.attr in VAR_CTORS or
                    node.attr == "Tk"):
                return self.gen_widget_ctor(node.attr, [], [], node)
            t = self.tmp()
            self.e("PyVal *%s = py_module_attr(%s, %s);" % (t, cstr(module), cstr(node.attr)))
            return t
        obj = self.gen(node.value)
        t = self.tmp()
        self.e("PyVal *%s = py_getattr(%s, %s);" % (t, obj, cstr(node.attr)))
        self.e("py_release(%s);" % obj)
        return t

    # calls ---------------------------------------------------------------
    def gen_call(self, node):
        func = node.func
        # tkinter constructors
        if isinstance(func, ast.Name):
            if func.id in ("Tk", "Toplevel") and func.id not in self.local_names \
                    and func.id not in self.globals:
                return self.gen_tk_root(node)
            if func.id in WIDGET_KINDS and func.id not in self.local_names \
                    and func.id not in self.globals:
                return self.gen_widget_ctor(func.id, node.args, node.keywords, node)
            if func.id in VAR_CTORS and func.id not in self.local_names \
                    and func.id not in self.globals:
                return self.gen_var_ctor(func.id, node)
            if func.id == "isinstance":
                return self.gen_isinstance(node)
            if func.id == "super":
                warn("super() is not supported", node)
                return self.const_none()
        if isinstance(func, ast.Attribute):
            if isinstance(func.value, ast.Call) and \
                    isinstance(func.value.func, ast.Name) and \
                    func.value.func.id == "super" and not func.value.args:
                r = self.gen_super_call(node)
                if r is not None:
                    return r
            module = self.module_of(func.value)
            if module:
                if module.split(".")[-1] == "messagebox" and func.attr in MSGBOX_FUNCS:
                    return self.gen_msgbox(func.attr, node)
                if func.attr in ("Tk", "Toplevel") and module.startswith("tkinter"):
                    return self.gen_tk_root(node)
                if func.attr in WIDGET_KINDS and module.startswith("tkinter"):
                    return self.gen_widget_ctor(func.attr, node.args, node.keywords, node)
                if func.attr in VAR_CTORS and module.startswith("tkinter"):
                    return self.gen_var_ctor(func.attr, node)
                argv, argc, kw, owned = self.gen_call_args(node)
                t = self.tmp()
                self.e("PyVal *%s = py_module_call(%s, %s, %s, %s, %s);"
                       % (t, cstr(module), cstr(func.attr), argv, argc, kw))
                self.release_call_args(owned, kw)
                return t
            # normal method call
            obj = self.gen(func.value)
            argv, argc, kw, owned = self.gen_call_args(node)
            t = self.tmp()
            self.e("PyVal *%s = py_method(%s, %s, %s, %s, %s);"
                   % (t, obj, cstr(func.attr), argv, argc, kw))
            self.e("py_release(%s);" % obj)
            self.release_call_args(owned, kw)
            return t
        fn = self.gen(func)
        argv, argc, kw, owned = self.gen_call_args(node)
        t = self.tmp()
        self.e("PyVal *%s = py_call(%s, %s, %s, %s);" % (t, fn, argv, argc, kw))
        self.e("py_release(%s);" % fn)
        self.release_call_args(owned, kw)
        return t

    def release_call_args(self, owned, kw):
        for v in owned:
            self.e("py_release(%s);" % v)
        if kw != "NULL":
            self.e("py_release(%s);" % kw)

    def gen_call_args(self, node):
        has_star = any(isinstance(a, ast.Starred) for a in node.args)
        kw_temps = []
        has_kwstar = any(k.arg is None for k in node.keywords)
        kw = "NULL"
        owned_args = []
        if has_star:
            lst = self.tmp()
            self.e("PyVal *%s = py_list_new();" % lst)
            for a in node.args:
                if isinstance(a, ast.Starred):
                    v = self.gen(a.value)
                    it = self.tmp()
                    item = self.tmp()
                    self.e("PyVal *%s = py_iter(%s);" % (it, v))
                    self.e("py_release(%s);" % v)
                    self.e("PyVal *%s = NULL;" % item)
                    self.e("while (py_iter_next(%s, &%s)) py_list_append(%s, %s);"
                           % (it, item, lst, item))
                    self.e("py_release(%s); py_release(%s);" % (item, it))
                else:
                    v = self.gen(a)
                    self.e("py_list_append(%s, %s);" % (lst, v))
                    self.e("py_release(%s);" % v)
            argv = "%s->u.seq.items" % lst
            argc = "%s->u.seq.len" % lst
            owned_args.append(lst)
        else:
            argv = self.tmp()
            for a in node.args:
                owned_args.append(self.gen(a))
            if owned_args:
                self.e("PyVal *%s[] = {%s};" % (argv, ", ".join(owned_args)))
                argc = str(len(owned_args))
            else:
                self.e("PyVal **%s = NULL;" % argv)
                argc = "0"
        if node.keywords:
            kw = self.tmp()
            if has_kwstar:
                self.e("PyVal *%s = py_dict_new();" % kw)
                for k in node.keywords:
                    v = self.gen(k.value)
                    if k.arg is None:
                        self.e("py_dict_update(%s, %s);" % (kw, v))
                    else:
                        self.e("py_dict_put(%s, %s, %s);" % (kw, cstr(k.arg), v))
                    self.e("py_release(%s);" % v)
            else:
                keys = ", ".join(cstr(k.arg) for k in node.keywords)
                vals = []
                for k in node.keywords:
                    vals.append(self.gen(k.value))
                self.e("PyVal *%s = py_dict_lit((const char *[]){%s}, (PyVal *[]){%s}, %d);"
                       % (kw, keys, ", ".join(vals), len(vals)))
                owned_args.extend(vals)
        return argv, argc, kw, owned_args

    def gen_super_call(self, node):
        base = self.spec.bases[0] if self.spec.bases else None
        attr = node.func.attr
        if not isinstance(base, ast.Name) or not self.self_arg or \
                any(isinstance(a, ast.Starred) for a in node.args):
            warn("unsupported super() call", node)
            return None
        m = self.tmp()
        self.e("PyVal *%s = py_getattr(%s, %s);" %
               (m, self.m.global_c(base.id), cstr(attr)))
        fake = ast.Call(
            func=ast.Name(id="__super__", ctx=ast.Load()),
            args=[ast.Name(id=self.self_arg, ctx=ast.Load())] + list(node.args),
            keywords=list(node.keywords))
        argv, argc, kw, owned = self.gen_call_args(fake)
        t = self.tmp()
        self.e("PyVal *%s = py_call(%s, %s, %s, %s);" % (t, m, argv, argc, kw))
        self.e("py_release(%s);" % m)
        self.release_call_args(owned, kw)
        return t

    def gen_isinstance(self, node):
        if len(node.args) >= 2 and isinstance(node.args[1], ast.Name) \
                and node.args[1].id in TYPE_NAMES:
            values = [self.gen(node.args[0])]
            tname = self.tmp()
            self.e("PyVal *%s = py_str(%s);" % (tname, cstr(TYPE_NAMES[node.args[1].id])))
            values.append(tname)
            t = self.tmp()
            self.e("PyVal *%s = py_builtin_isinstance((PyVal *[]){%s, %s}, 2, NULL);"
                   % (t, values[0], values[1]))
            for v in values:
                self.e("py_release(%s);" % v)
            return t
        fn = self.tmp()
        self.e("PyVal *%s = py_builtin_lookup(\"isinstance\");" % fn)
        argv, argc, kw, owned = self.gen_call_args(node)
        t = self.tmp()
        self.e("PyVal *%s = py_call(%s, %s, %s, %s);" % (t, fn, argv, argc, kw))
        self.e("py_release(%s);" % fn)
        self.release_call_args(owned, kw)
        return t

    def gen_tk_root(self, node):
        t = self.tmp()
        self.e("PyVal *%s = py_tk_root();" % t)
        return t

    def gen_var_ctor(self, name, node):
        val = None
        for a in node.args:
            if val is None:
                val = a
            else:
                break
        for k in node.keywords:
            if k.arg == "value":
                val = k.value
        if val is None:
            v = self.tmp()
            self.e("PyVal *%s = py_none();" % v)
            self.e("py_retain(%s);" % v)
        else:
            v = self.gen(val)
        t = self.tmp()
        self.e("PyVal *%s = py_var(%s, %s);" % (t, v, cstr(name)))
        self.e("py_release(%s);" % v)
        return t

    def gen_msgbox(self, name, node):
        title = "NULL"
        message = "NULL"
        pos = list(node.args)
        for k in node.keywords:
            if k.arg == "title":
                title = self.gen(k.value)
            elif k.arg == "message":
                message = self.gen(k.value)
        if title == "NULL" and pos:
            title = self.gen(pos.pop(0))
        if message == "NULL" and pos:
            message = self.gen(pos.pop(0))
        tv = self.tmp() if title == "NULL" else title
        mv = self.tmp() if message == "NULL" else message
        if title == "NULL":
            self.e("PyVal *%s = py_str(\"\");" % tv)
        if message == "NULL":
            self.e("PyVal *%s = py_str(\"\");" % mv)
        t = self.tmp()
        self.e("PyVal *%s = py_msgbox(%d, %s, %s);" % (t, MSGBOX_FUNCS[name], mv, tv))
        self.e("py_release(%s); py_release(%s);" % (tv, mv))
        return t

    def gen_widget_ctor(self, name, args, keywords, node):
        parent = None
        # first positional argument is the master (except for Tk)
        if args:
            parent = self.gen(args[0])
        else:
            parent = self.tmp()
            self.e("PyVal *%s = py_none();" % parent)
            self.e("py_retain(%s);" % parent)
        kw = "NULL"
        owned = []
        if keywords:
            kw = self.tmp()
            keys = ", ".join(cstr(k.arg) for k in keywords if k.arg)
            vals = []
            for k in keywords:
                if k.arg is None:
                    continue
                vals.append(self.gen(k.value))
            if vals:
                self.e("PyVal *%s = py_dict_lit((const char *[]){%s}, (PyVal *[]){%s}, %d);"
                       % (kw, keys, ", ".join(vals), len(vals)))
                owned = vals
            else:
                self.e("PyVal *%s = NULL;" % kw)
                kw = "NULL"
        t = self.tmp()
        self.e("PyVal *%s = py_tk_widget(%s, %s, %s);" % (t, WIDGET_KINDS[name], parent, kw))
        self.e("py_release(%s);" % parent)
        for v in owned:
            self.e("py_release(%s);" % v)
        if kw != "NULL":
            self.e("py_release(%s);" % kw)
        return t

    def gen_slice(self, node):
        if isinstance(node, ast.Slice):
            warn("slice is not a value here", node)
            return self.const_none()
        return self.gen(node)

    def gen_subscript(self, value, sl):
        if isinstance(sl, ast.Slice):
            obj = self.gen(value)

            def part(p):
                if p is None:
                    t = self.tmp()
                    self.e("PyVal *%s = py_none(); py_retain(%s);" % (t, t))
                    return t
                return self.gen(p)

            lo = part(sl.lower)
            hi = part(sl.upper)
            st = part(sl.step)
            t = self.tmp()
            self.e("PyVal *%s = py_getslice(%s, %s, %s, %s);" % (t, obj, lo, hi, st))
            self.e("py_release(%s); py_release(%s); py_release(%s); py_release(%s);"
                   % (obj, lo, hi, st))
            return t
        obj = self.gen(value)
        key = self.gen(sl)
        t = self.tmp()
        self.e("PyVal *%s = py_getitem(%s, %s);" % (t, obj, key))
        self.e("py_release(%s); py_release(%s);" % (obj, key))
        return t

    # lambdas -------------------------------------------------------------
    def gen_lambda(self, node):
        params = list(node.args.posonlyargs) + list(node.args.args)
        bound = {a.arg for a in params}
        lc = LoadCollector(bound)
        lc.visit(node.body)
        free = {n for n in lc.loads
                if n in self.local_names or n in self.free}
        env_name = "env_" + self.tmp()
        spec = self.m.new_lambda(node, free, env_name)
        env = self.m.make_closure_env(spec, self, free)
        self.m.emit_closure_defaults(spec, self, env)
        t = self.tmp()
        self.e("PyVal *%s = py_func(%s, \"<lambda>\");" % (t, spec.cname))
        r = self.tmp()
        self.e("PyVal *%s = py_bound(%s, %s);" % (r, t, env))
        self.e("py_release(%s); py_release(%s);" % (t, env))
        return r


class ModuleEmitter:
    def __init__(self, tree, filename, out_c):
        self.tree = tree
        self.filename = filename
        self.out_c = out_c
        self.module_globals = set()
        self.global_decls = set()
        self.global_names = NamePool()
        self.global_map = {}
        self.fn_names = NamePool()
        self.specs = []
        self.class_specs = []
        self.module_aliases = {}
        self.direct = {}
        self.star_modules = []
        self.imports_seen = set()
        self.global_funcs = []
        self.func_map = {}
        self.class_map = {}
        self._scan()

    # ------------------------------------------------------------- scanning
    def _scan(self):
        for stmt in self.tree.body:
            self.scan_stmt(stmt)
            self.collect_stores(stmt)
        for stmt in ast.walk(self.tree):
            if isinstance(stmt, (ast.Import, ast.ImportFrom)):
                self.note_import(stmt)
        for stmt in self.tree.body:
            if isinstance(stmt, ast.FunctionDef):
                self.add_function(stmt, stmt.name, False)
            elif isinstance(stmt, ast.ClassDef):
                self.add_class(stmt)
        self._register_nested_functions()

    def _register_nested_functions(self):
        """Register ``def`` statements nested inside function bodies."""
        pending = list(self.specs)
        seen = set()
        while pending:
            spec = pending.pop(0)
            if id(spec.node) in seen:
                continue
            seen.add(id(spec.node))
            enclosing = (collect_locals(spec.node) | set(spec.cells) |
                         set(getattr(spec, "free", ())))
            for node in ast.walk(spec.node):
                if node is spec.node or id(node) in seen:
                    continue
                if not isinstance(node, ast.FunctionDef):
                    continue
                display = spec.display + "__" + node.name
                nspec = self.add_function(node, display, False)
                nspec.env_name = "env_" + self.fn_names.take("cell")
                nspec.free = collect_free(node) & enclosing
                pending.append(nspec)

    def collect_stores(self, node):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef,
                             ast.ClassDef, ast.Lambda)):
            return
        if isinstance(node, ast.Name) and isinstance(node.ctx, ast.Store):
            self.module_globals.add(node.id)
        elif isinstance(node, ast.ExceptHandler) and node.name:
            self.module_globals.add(node.name)
        for child in ast.iter_child_nodes(node):
            self.collect_stores(child)

    def scan_stmt(self, stmt):
        if isinstance(stmt, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            self.module_globals.add(stmt.name)
            return
        if isinstance(stmt, ast.Assign):
            for t in stmt.targets:
                self.module_globals.update(target_names(t))
        elif isinstance(stmt, ast.AnnAssign):
            self.module_globals.update(target_names(stmt.target))
        elif isinstance(stmt, ast.AugAssign):
            self.module_globals.update(target_names(stmt.target))
        elif isinstance(stmt, ast.For):
            self.module_globals.update(target_names(stmt.target))
        elif isinstance(stmt, ast.While):
            pass
        elif isinstance(stmt, ast.Import):
            pass
        elif isinstance(stmt, ast.ImportFrom):
            pass
        for child in ast.iter_child_nodes(stmt):
            if isinstance(child, (ast.FunctionDef, ast.ClassDef)):
                continue
            if isinstance(child, ast.stmt) and not isinstance(child, ast.comprehension):
                self.scan_stmt(child)
            elif isinstance(child, (ast.With, ast.Try)):
                pass

    def add_function(self, node, display, is_method):
        cname = self.fn_names.take("pyf_" + display)
        static = any(isinstance(d, ast.Name) and d.id == "staticmethod"
                     for d in node.decorator_list)
        spec = FuncSpec(node, cname, display, is_method=False, is_static=static)
        spec.cells = collect_cells(node)
        self.add_defaults(spec, node)
        self.func_map[id(node)] = spec
        self.specs.append(spec)
        return spec

    def add_method(self, node, display):
        cname = self.fn_names.take("pyf_" + display)
        static = any(isinstance(d, ast.Name) and d.id == "staticmethod"
                     for d in node.decorator_list)
        spec = FuncSpec(node, cname, display, is_method=not static, is_static=static)
        spec.cells = collect_cells(node)
        self.add_defaults(spec, node)
        self.specs.append(spec)
        return spec

    def add_defaults(self, spec, node):
        args = list(node.args.posonlyargs) + list(node.args.args)
        if spec.is_method and not spec.is_static and not spec.is_lambda:
            pass
        ndef = len(node.args.defaults)
        defaults = [None] * len(args)
        if ndef:
            for i, d in enumerate(node.args.defaults):
                defaults[len(args) - ndef + i] = d
        spec.params = args
        spec.defaults = defaults
        spec.vararg = node.args.vararg
        spec.kwarg = node.args.kwarg
        spec.kwonly = list(node.args.kwonlyargs)
        spec.kw_defaults = list(node.args.kw_defaults)

    def add_class(self, node):
        cname = self.fn_names.take("cls_" + node.name)
        cs = ClassSpec(node, cname)
        for stmt in node.body:
            if isinstance(stmt, ast.FunctionDef):
                m = self.add_method(stmt, node.name + "_" + stmt.name)
                m.owner_class = node.name
                m.bases = list(node.bases)
                cs.methods.append((stmt.name, m))
        self.class_map[id(node)] = cs
        self.class_specs.append(cs)

    def new_lambda(self, node, free, env_name):
        cname = self.fn_names.take("pyf_lambda")
        spec = FuncSpec(node, cname, "<lambda>", is_method=False)

        self.add_defaults(spec, node)
        spec.env_name = env_name
        spec.is_lambda = True
        spec.free = free
        self.specs.append(spec)
        return spec

    # --------------------------------------------------------------- names
    def global_c(self, name):
        if name not in self.global_map:
            self.global_map[name] = "g_" + self.global_names.take(name)
        return self.global_map[name]

    # ------------------------------------------------------------- imports
    def note_import(self, node):
        if isinstance(node, ast.Import):
            for a in node.names:
                if a.asname:
                    self.module_aliases[a.asname] = a.name
                else:
                    root = a.name.split(".")[0]
                    self.module_aliases[root] = root
        else:
            module = node.module or ""
            for a in node.names:
                if a.name == "*":
                    if module not in self.star_modules:
                        self.star_modules.append(module)
                    continue
                local = a.asname or a.name
                if (module, a.name) in MODULE_MEMBERS:
                    self.module_aliases[local] = module + "." + a.name
                else:
                    self.direct[local] = (module, a.name)

    # ------------------------------------------------------------- assembly
    def defaults_decl(self, spec):
        out = []
        if any(d is not None for d in spec.defaults):
            n = len(spec.params)
            out.append("static PyVal *py_defs_%s[%d];" % (spec.cname, n))
        kwonly = getattr(spec, "kwonly", [])
        kwdefs = getattr(spec, "kw_defaults", [])
        if kwonly and any(d is not None for d in kwdefs):
            out.append("static PyVal *py_kwdefs_%s[%d];" % (spec.cname, len(kwonly)))
        return ("\n".join(out) + "\n") if out else ""

    def emit_defaults(self, spec, em):
        offset = 1 if (spec.is_method and not spec.is_static) else 0
        for i, d in enumerate(spec.defaults):
            if d is None or i < offset:
                continue
            v = em.gen(d)
            em.e("py_assign(&py_defs_%s[%d], %s);" % (spec.cname, i - offset, v))
            em.e("py_release(%s);" % v)
        kwdefs = getattr(spec, "kw_defaults", [])
        for i, d in enumerate(kwdefs):
            if d is None:
                continue
            v = em.gen(d)
            em.e("py_assign(&py_kwdefs_%s[%d], %s);" % (spec.cname, i, v))
            em.e("py_release(%s);" % v)

    def make_closure_env(self, spec, em, free):
        """Build (or reuse) the environment dict for a nested function."""
        if free and em.cell_env and free <= em.cells:
            env = em.tmp()
            em.e("PyVal *%s = %s;" % (env, em.cell_env))
            em.e("py_retain(%s);" % env)
            return env
        if free and em.env_name and free <= em.free:
            env = em.tmp()
            em.e("PyVal *%s = %s;" % (env, em.env_name))
            em.e("py_retain(%s);" % env)
            return env
        env = em.tmp()
        em.e("PyVal *%s = py_dict_new();" % env)
        for name in sorted(free):
            v = em.gen(ast.Name(id=name, ctx=ast.Load()))
            em.e("py_dict_put(%s, %s, %s);" % (env, cstr(name), v))
            em.e("py_release(%s);" % v)
        return env

    def emit_closure_defaults(self, spec, em, env):
        """Evaluate defaults once per closure and keep them in the env dict."""
        offset = 1 if (spec.is_method and not spec.is_static) else 0
        for i, d in enumerate(spec.defaults):
            if d is None or i < offset:
                continue
            v = em.gen(d)
            key = "__def_%s_%d" % (spec.cname, i - offset)
            em.e("py_dict_put(%s, %s, %s);" % (env, cstr(key), v))
            em.e("py_release(%s);" % v)
        for i, d in enumerate(getattr(spec, "kw_defaults", [])):
            if d is None:
                continue
            v = em.gen(d)
            key = "__kwdef_%s_%d" % (spec.cname, i)
            em.e("py_dict_put(%s, %s, %s);" % (env, cstr(key), v))
            em.e("py_release(%s);" % v)

    def apply_decorators(self, node, em, slot):
        for dec in reversed(getattr(node, "decorator_list", [])):
            if isinstance(dec, ast.Name) and dec.id in ("staticmethod", "classmethod"):
                continue
            f = em.gen(dec)
            cur = em.tmp()
            em.e("PyVal *%s = py_load(&%s, %s);" % (cur, slot, cstr(node.name)))
            args = em.tmp()
            em.e("PyVal *%s[] = {%s};" % (args, cur))
            res = em.tmp()
            em.e("PyVal *%s = py_call(%s, %s, 1, NULL);" % (res, f, args))
            em.e("py_assign(&%s, %s);" % (slot, res))
            em.e("py_release(%s);" % res)
            em.e("py_release(%s);" % cur)
            em.e("py_release(%s);" % f)

    def define_function(self, node, em):
        spec = self.func_map.get(id(node))
        if spec is None:
            warn("cannot find definition of '%s'" % node.name, node)
            return
        self.emit_defaults(spec, em)
        t = em.tmp()
        em.e("PyVal *%s = py_func(%s, %s);" % (t, spec.cname, cstr(node.name)))
        em.e("py_assign(&%s, %s);" % (self.global_c(node.name), t))
        em.e("py_release(%s);" % t)
        self.apply_decorators(node, em, self.global_c(node.name))

    def define_nested_function(self, node, em):
        spec = self.func_map.get(id(node))
        if spec is None:
            warn("cannot find definition of nested '%s'" % node.name, node)
            return
        free = spec.free
        env = self.make_closure_env(spec, em, free)
        self.emit_closure_defaults(spec, em, env)
        t = em.tmp()
        em.e("PyVal *%s = py_func(%s, %s);" % (t, spec.cname, cstr(node.name)))
        r = em.tmp()
        em.e("PyVal *%s = py_bound(%s, %s);" % (r, t, env))
        em.e("py_release(%s); py_release(%s);" % (t, env))
        slot = em.local(node.name) if node.name in em.local_names else self.global_c(node.name)
        em.e("py_assign(&%s, %s);" % (slot, r))
        self.apply_decorators(node, em, slot)
        em.e("py_release(%s);" % r)

    def define_class(self, node, em):
        cs = self.class_map.get(id(node))
        if cs is None:
            warn("cannot find definition of class '%s'" % node.name, node)
            return
        d = em.tmp()
        em.e("PyVal *%s = py_dict_new();" % d)
        for base in node.bases:
            b = em.gen(base)
            em.e("py_class_inherit(%s, %s);" % (d, b))
            em.e("py_release(%s);" % b)
        method_specs = {id(spec.node): spec for _, spec in cs.methods}
        for stmt in node.body:
            if isinstance(stmt, ast.FunctionDef):
                m = method_specs.get(id(stmt))
                if m is None:
                    continue
                self.emit_defaults(m, em)
                mt = em.tmp()
                em.e("PyVal *%s = py_func(%s, %s);" % (mt, m.cname, cstr(stmt.name)))
                em.e("py_dict_put(%s, %s, %s);" % (d, cstr(stmt.name), mt))
                em.e("py_release(%s);" % mt)
            elif isinstance(stmt, ast.Assign):
                val = em.gen(stmt.value)
                for tgt in stmt.targets:
                    if isinstance(tgt, ast.Name):
                        em.e("py_dict_put(%s, %s, %s);" % (d, cstr(tgt.id), val))
                em.e("py_release(%s);" % val)
            elif isinstance(stmt, ast.AnnAssign) and stmt.value is not None \
                    and isinstance(stmt.target, ast.Name):
                val = em.gen(stmt.value)
                em.e("py_dict_put(%s, %s, %s);" % (d, cstr(stmt.target.id), val))
                em.e("py_release(%s);" % val)
        c = em.tmp()
        em.e("PyVal *%s = py_class(%s, %s);" % (c, d, cstr(node.name)))
        em.e("py_release(%s);" % d)
        em.e("py_assign(&%s, %s);" % (self.global_c(node.name), c))
        em.e("py_release(%s);" % c)
        self.apply_decorators(node, em, self.global_c(node.name))

    def generate(self):
        # Emit the module body first: lambdas are discovered while generating
        # code, and their prototypes must be emitted with everything else.
        module_lines = self.emit_module()
        i = 0
        compiled = set()
        while i < len(self.specs):
            spec = self.specs[i]
            if spec.cname not in compiled:
                compiled.add(spec.cname)
                fn = FnEmitter(self, spec)
                spec.body_c = fn.render()
            i += 1

        out = []
        out.append("/* generated by py2c99 - do not edit */")
        out.append("#define _CRT_SECURE_NO_WARNINGS")
        out.append("#ifndef UNICODE")
        out.append("#define UNICODE")
        out.append("#define _UNICODE")
        out.append("#endif")
        out.append("#include \"pyrt.h\"")
        out.append("#include <math.h>")
        out.append("#include <string.h>")
        out.append("#include <stdlib.h>")
        out.append("#include <wchar.h>")
        out.append("#include <windows.h>")
        out.append("")
        if self.specs:
            for spec in self.specs:
                out.append("static PyVal *%s(PyVal **args, size_t argc, PyVal *kwargs);" % spec.cname)
            out.append("")
        for name in sorted(self.module_globals):
            out.append("static PyVal *%s = NULL;" % self.global_c(name))
        for spec in self.specs:
            decl = self.defaults_decl(spec)
            if decl:
                out.append(decl.rstrip())
        out.append("")
        for spec in self.specs:
            out.append(spec.body_c)
        out.extend(module_lines)
        return "\n".join(out)

    def emit_module(self):
        lines = []
        lines.append("static int py_run_module(void) {")
        lines.append("    PyJmp *h = py_push_handler();")
        lines.append("    if (setjmp(h->buf) == 0) {")
        fake = ast.FunctionDef(
            name="__module__",
            args=ast.arguments(posonlyargs=[], args=[], vararg=None,
                               kwonlyargs=[], kw_defaults=[], kwarg=None,
                               defaults=[]),
            body=[], decorator_list=[], returns=None, type_comment=None)
        em = FnEmitter(self, FuncSpec(fake, "unused", "<module>"))
        em.in_function = False
        em.indent = 2
        em.lines = lines
        for stmt in self.tree.body:
            em.stmt(stmt)
        lines.append("        py_pop_handler(h);")
        lines.append("        return 0;")
        lines.append("    } else {")
        lines.append("        py_pop_handler(h);")
        lines.append("        py_print_exc(py_exc_current());")
        lines.append("        py_exc_clear();")
        lines.append("        return 1;")
        lines.append("    }")
        lines.append("}")
        lines.append("")
        lines.append("static void py_cleanup(void) {")
        for name in sorted(self.module_globals):
            lines.append("    py_release(%s);" % self.global_c(name))
            lines.append("    %s = NULL;" % self.global_c(name))
        lines.append("}")
        lines.append("")
        lines.append("int wmain(int argc, wchar_t **wargv) {")
        lines.append("    char **argv = (char **)malloc((size_t)(argc + 1) * sizeof(char *));")
        lines.append("    int i, code;")
        lines.append("    for (i = 0; i < argc; i++) argv[i] = py_utf8_from_utf16(wargv[i]);")
        lines.append("    argv[argc] = NULL;")
        lines.append("    py_init(argc, argv);")
        lines.append("    code = py_run_module();")
        lines.append("    py_cleanup();")
        lines.append("    py_shutdown();")
        lines.append("    for (i = 0; i < argc; i++) free(argv[i]);")
        lines.append("    free(argv);")
        lines.append("    return code;")
        lines.append("}")
        lines.append("")
        return lines


# --------------------------------------------------------------------------
# driver


def build(source, output, keep, verbose, run, extra_cflags, lto=False,
          quiet=False):
    try:
        with open(source, "r", encoding="utf-8") as f:
            text = f.read()
    except OSError as exc:
        die("cannot read %s: %s" % (source, exc))
    try:
        tree = ast.parse(text, filename=source)
    except SyntaxError as exc:
        die("%s:%s: %s" % (source, exc.lineno, exc.msg))

    work = tempfile.mkdtemp(prefix="py2c99_")
    gen_path = os.path.join(work, "py2c99_gen.c")
    mod = ModuleEmitter(tree, os.path.abspath(source), gen_path)
    code = mod.generate()
    with open(gen_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(code)
    if keep:
        kept = os.path.splitext(os.path.abspath(output))[0] + ".c"
        shutil.copyfile(gen_path, kept)
        if not quiet:
            sys.stderr.write("py2c99: kept generated C at %s\n" % kept)

    gcc = os.environ.get("CC", "gcc")
    cmd = [gcc, "-std=c99", "-O3", "-s",
           "-ffunction-sections", "-fdata-sections",
           "-Wno-discarded-qualifiers",
           "-municode", "-o", output,
           gen_path,
           os.path.join(RUNTIME, "pyrt.c"),
           os.path.join(RUNTIME, "pyrt_stdlib.c"),
           os.path.join(RUNTIME, "pyrt_win.c"),
           "-I", RUNTIME,
           "-Wl,--gc-sections",
           "-luser32", "-lgdi32", "-lcomctl32", "-lm"]
    if lto:
        cmd.insert(3, "-flto")
    cmd.extend(extra_cflags)
    if verbose:
        sys.stderr.write(" ".join(cmd) + "\n")
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True)
    except FileNotFoundError:
        die("gcc not found; install MinGW-w64 or set CC")
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout)
        sys.stderr.write(proc.stderr)
        if not keep:
            kept = os.path.splitext(os.path.abspath(output))[0] + ".c"
            shutil.copyfile(gen_path, kept)
            if not quiet:
                sys.stderr.write("py2c99: generated C kept at %s\n" % kept)
        die("gcc failed")
    if proc.stderr.strip() and not quiet:
        sys.stderr.write(proc.stderr)
    shutil.rmtree(work, ignore_errors=True)
    if not quiet:
        sys.stderr.write("py2c99: built %s\n" % output)
    if run:
        sys.exit(subprocess.call([os.path.abspath(output)]))


def repl():
    """Interactive compile-and-run loop with persistent setup state."""
    setup_types = (ast.Import, ast.ImportFrom, ast.Assign, ast.AnnAssign,
                   ast.FunctionDef, ast.ClassDef)
    history = []
    work = tempfile.mkdtemp(prefix="py2c99_repl_")
    serial = 0

    def is_setup(tree):
        return bool(tree.body) and all(isinstance(stmt, setup_types)
                                       for stmt in tree.body)

    print("py2c99 REPL - type exit() or press Ctrl+Z/Ctrl+D to leave")
    print("State is kept by recompiling earlier imports, assignments and defs.")
    try:
        while True:
            try:
                line = input(">>> ")
            except (EOFError, KeyboardInterrupt):
                print()
                break
            stripped = line.strip()
            if not stripped:
                continue
            if stripped.lower() in ("exit", "quit", "exit()", "quit()"):
                break

            source = line
            if line.rstrip().endswith(":"):
                while True:
                    try:
                        more = input("... ")
                    except (EOFError, KeyboardInterrupt):
                        print()
                        return
                    if not more.strip():
                        break
                    source += "\n" + more

            try:
                tree = ast.parse(source, filename="<repl>")
            except SyntaxError as exc:
                sys.stderr.write("py2c99: error: %s\n" % exc.msg)
                continue

            serial += 1
            source_path = os.path.join(work, "repl.py")
            exe_path = os.path.join(work, "repl_%d.exe" % serial)
            try:
                with open(source_path, "w", encoding="utf-8") as f:
                    f.write("\n".join(history + [source]) + "\n")
            except OSError as exc:
                sys.stderr.write("py2c99: error: %s\n" % exc)
                continue

            try:
                build(source_path, exe_path, False, False, False, [],
                      quiet=True)
            except SystemExit:
                # die() already printed the diagnostic; keep the session alive.
                continue

            try:
                subprocess.call([os.path.abspath(exe_path)], cwd=os.getcwd())
            except OSError as exc:
                sys.stderr.write("py2c99: error: cannot run REPL program: %s\n"
                                 % exc)

            if is_setup(tree):
                history.append(source)
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main(argv=None):
    ap = argparse.ArgumentParser(description="transpile Python 3 to C99")
    ap.add_argument("source", nargs="?", help="input .py file (omit for REPL)")
    ap.add_argument("-o", "--output", help="output executable")
    ap.add_argument("--keep", action="store_true", help="keep generated C")
    ap.add_argument("--run", action="store_true", help="run after building")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("-C", "--cflag", action="append", default=[],
                    help="extra flag passed to gcc")
    ap.add_argument("--lto", action="store_true",
                    help="enable link-time optimization (experimental)")
    args = ap.parse_args(argv)
    if not args.source:
        return repl()
    out = args.output
    if not out:
        out = os.path.splitext(os.path.basename(args.source))[0] + ".exe"
    build(args.source, out, args.keep, args.verbose, args.run, args.cflag,
          lto=args.lto)
    return 0


if __name__ == "__main__":
    sys.exit(main())
