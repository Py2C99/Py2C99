/*
 * py2c99 - additional Python 3.11 builtins implemented in pure C99.
 *
 * These fill the gaps in pyrt.c's builtin table: descriptor helpers,
 * introspection helpers and the remaining one-argument conversions.  They
 * use only the public runtime API, so they live in their own translation unit.
 */
#define _CRT_SECURE_NO_WARNINGS
#include "pyrt.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

static int b_eq(const char *a, const char *b) { return a && b && !strcmp(a, b); }

static PyVal *b_one(PyVal **a, size_t n) { return a && n ? a[0] : py_none(); }

/* ASCII-only repr: escape non-ASCII code points, not UTF-8 bytes. */
static PyVal *bi_ascii(PyVal **a, size_t n, PyVal *kw) {
    (void)kw;
    return py_ascii_of(b_one(a, n));
}

static PyVal *bi_issubclass(PyVal **a, size_t n, PyVal *kw) {
    PyVal *cls = b_one(a, n);
    PyVal *base = n > 1 ? a[1] : py_none();
    (void)kw;
    return py_bool(py_class_is_subclass(cls, base));
}

static PyVal *bi_delattr(PyVal **a, size_t n, PyVal *kw) {
    (void)kw;
    if (n > 1) py_delattr(a[0], py_cstr(a[1]));
    return py_none();
}

static PyVal *bi_aiter(PyVal **a, size_t n, PyVal *kw) {
    (void)kw;
    return py_iter(b_one(a, n));
}

static PyVal *bi_anext(PyVal **a, size_t n, PyVal *kw) {
    PyVal *out = NULL;
    (void)kw;
    if (!py_iter_next(b_one(a, n), &out)) py_raise(PY_EXC_STOP_ITERATION, "anext()");
    return out ? out : py_none();
}

static PyVal *bi_breakpoint(PyVal **a, size_t n, PyVal *kw) {
    (void)a; (void)n; (void)kw;
    fputs("py2c99: breakpoint() reached (no debugger attached)\n", stderr);
    return py_none();
}

/* The compiler emits a live snapshot for globals()/locals(); this is the
   fallback used when they are reached through the builtins module. */
static PyVal *bi_globals(PyVal **a, size_t n, PyVal *kw) {
    (void)a; (void)n; (void)kw;
    return py_dict_new();
}

static PyVal *bi_locals(PyVal **a, size_t n, PyVal *kw) {
    (void)a; (void)n; (void)kw;
    return py_dict_new();
}

static PyVal *bi_debug(PyVal **a, size_t n, PyVal *kw) {
    (void)a; (void)n; (void)kw;
    return py_bool(1);
}

static void dir_add(PyVal *out, PyVal *name) {
    PyVal *s = py_str_of(name);
    size_t i;
    for (i = 0; i < py_len(out); i++) {
        PyVal *cur = py_getitem(out, py_int((int64_t)i));
        if (py_cstr(cur) && !strcmp(py_cstr(cur), py_cstr(s))) {
            py_release(cur);
            py_release(s);
            return;
        }
        py_release(cur);
    }
    py_list_append(out, s);
    py_release(s);
}

static void dir_from_dict(PyVal *out, PyVal *d) {
    size_t i;
    if (!d || d->type != PY_T_DICT) return;
    for (i = 0; i < d->u.map.len; i++) dir_add(out, d->u.map.keys[i]);
}

static void dir_add_strings(PyVal *out, const char *const *names) {
    size_t i;
    for (i = 0; names[i]; i++) {
        PyVal *s = py_str(names[i]);
        dir_add(out, s);
        py_release(s);
    }
}

static PyVal *bi_dir(PyVal **a, size_t n, PyVal *kw) {
    PyVal *obj = b_one(a, n), *out;
    static const char *const common[] = {
        "__class__", "__delattr__", "__dict__", "__dir__", "__doc__",
        "__eq__", "__format__", "__ge__", "__getattr__", "__gt__",
        "__hash__", "__init__", "__le__", "__lt__", "__ne__", "__new__",
        "__reduce__", "__repr__", "__setattr__", "__sizeof__", "__str__", NULL
    };
    static const char *const dict_methods[] = {
        "clear", "copy", "fromkeys", "get", "items", "keys", "pop",
        "popitem", "setdefault", "update", "values", NULL
    };
    static const char *const list_methods[] = {
        "append", "clear", "copy", "count", "extend", "index", "insert",
        "pop", "remove", "reverse", "sort", NULL
    };
    static const char *const str_methods[] = {
        "capitalize", "casefold", "center", "count", "encode", "endswith",
        "find", "format", "index", "isalpha", "isdigit", "islower",
        "isspace", "istitle", "isupper", "join", "ljust", "lower",
        "lstrip", "partition", "replace", "rfind", "rindex", "rjust",
        "rstrip", "split", "splitlines", "startswith", "strip", "swapcase",
        "title", "upper", "zfill", NULL
    };
    size_t i;
    (void)kw;
    /* `dir(builtins)` should expose the complete generated 3.11 registry. */
    if (obj->type == PY_T_STR && obj->u.s.data &&
        (!strcmp(obj->u.s.data, "builtins") ||
         !strcmp(obj->u.s.data, "__builtin__")))
        return py311_builtin_names();
    out = py_list_new();
    if (obj->type == PY_T_CLASS && obj->u.klass.dict)
        dir_from_dict(out, obj->u.klass.dict);
    if (obj->type == PY_T_DICT) dir_add_strings(out, dict_methods);
    else if (obj->type == PY_T_LIST || obj->type == PY_T_TUPLE)
        dir_add_strings(out, list_methods);
    else if (obj->type == PY_T_STR) dir_add_strings(out, str_methods);
    if (obj->type == PY_T_EXC) {
        static const char *const extra[] = {
            "args", "with_traceback", "__traceback__", NULL
        };
        dir_add_strings(out, extra);
    }
    dir_add_strings(out, common);
    for (i = 1; i < py_len(out); i++) {
        PyVal *item = py_getitem(out, py_int((int64_t)i));
        size_t j = i;
        while (j > 0) {
            PyVal *prev = py_getitem(out, py_int((int64_t)(j - 1)));
            if (strcmp(py_cstr(prev), py_cstr(item)) <= 0) {
                py_release(prev);
                break;
            }
            py_setitem(out, py_int((int64_t)j), prev);
            py_release(prev);
            j--;
        }
        py_setitem(out, py_int((int64_t)j), item);
        py_release(item);
    }
    return out;
}

static PyVal *bi_vars(PyVal **a, size_t n, PyVal *kw) {
    PyVal *obj;
    (void)kw;
    if (!n) return py_dict_new();
    obj = a[0];
    if (obj->type == PY_T_CLASS && obj->u.klass.dict) {
        py_retain(obj->u.klass.dict);
        return obj->u.klass.dict;
    }
    if (obj->type == PY_T_DICT) {
        PyVal *d = py_dict_get(obj, "__dict__");
        if (d) return d;
        py_retain(obj);
        return obj;
    }
    {
        PyVal *d = py_getattr(obj, "__dict__");
        if (d->type == PY_T_DICT) return d;
        py_release(d);
    }
    return py_dict_new();
}

static PyVal *bi_object(PyVal **a, size_t n, PyVal *kw) {
    (void)a; (void)n; (void)kw;
    return py_class(py_dict_new(), "object");
}

static PyVal *bi_bytes_impl(PyVal **a, size_t n, PyVal *kw, int mutable) {
    PyVal *v, *out;
    (void)kw; (void)mutable;
    if (!n) return py_str("");
    v = a[0];
    if (v->type == PY_T_STR || v->type == PY_T_INT || v->type == PY_T_BOOL)
        return py_builtin_str(a, n, kw);
    if (v->type == PY_T_LIST || v->type == PY_T_TUPLE) {
        size_t i, len = py_len(v);
        char *buf = (char *)malloc(len + 1);
        if (!buf) return py_str("");
        for (i = 0; i < len; i++) {
            PyVal *item = py_getitem(v, py_int((int64_t)i));
            int c = (int)py_as_int(item);
            buf[i] = (char)(unsigned char)c;
            py_release(item);
        }
        buf[len] = 0;
        out = py_strn(buf, len);
        free(buf);
        return out;
    }
    out = py_str_of(v);
    return out;
}

static PyVal *bi_bytes(PyVal **a, size_t n, PyVal *kw) {
    return bi_bytes_impl(a, n, kw, 0);
}

static PyVal *bi_bytearray(PyVal **a, size_t n, PyVal *kw) {
    return bi_bytes_impl(a, n, kw, 1);
}

static PyVal *bi_import(PyVal **a, size_t n, PyVal *kw) {
    PyVal *name = py_str_of(b_one(a, n)), *r = py_dict_new();
    (void)kw;
    py_dict_put(r, "__name__", name);
    py_dict_put(r, "__py_module__", name);
    py_release(name);
    py_dict_put(r, "__package__", py_str(""));
    py_dict_put(r, "__loader__", py_str("py2c99"));
    return r;
}

static PyVal *bi_compile(PyVal **a, size_t n, PyVal *kw) {
    PyVal *r = py_dict_new();
    PyVal *src = py_str_of(b_one(a, n));
    PyVal *file = n > 1 ? py_str_of(a[1]) : py_str("<string>");
    PyVal *mode = n > 2 ? py_str_of(a[2]) : py_str("exec");
    (void)kw;
    py_dict_put(r, "source", src);
    py_dict_put(r, "filename", file);
    py_dict_put(r, "mode", mode);
    py_release(src);
    py_release(file);
    py_release(mode);
    return r;
}

static PyVal *bi_descriptor_kind(PyVal **a, size_t n, const char *kind) {
    const char *keys[] = { "__descriptor__", "__func__", "fget", "fset" };
    PyVal *v[4];
    v[0] = py_str(kind);
    v[1] = b_one(a, n);
    py_retain(v[1]);
    v[2] = n > 1 ? a[1] : py_none();
    py_retain(v[2]);
    v[3] = n > 2 ? a[2] : py_none();
    py_retain(v[3]);
    {
        PyVal *r = py_dict_lit(keys, v, 4);
        size_t i;
        for (i = 0; i < 4; i++) py_release(v[i]);
        return r;
    }
}

static PyVal *bi_property(PyVal **a, size_t n, PyVal *kw) {
    (void)kw;
    return bi_descriptor_kind(a, n, "property");
}

static PyVal *bi_staticmethod(PyVal **a, size_t n, PyVal *kw) {
    (void)kw;
    return bi_descriptor_kind(a, n, "staticmethod");
}

static PyVal *bi_classmethod(PyVal **a, size_t n, PyVal *kw) {
    (void)kw;
    return bi_descriptor_kind(a, n, "classmethod");
}

static PyVal *bi_super(PyVal **a, size_t n, PyVal *kw) {
    PyVal *r = py_dict_new();
    (void)kw;
    if (n > 1 && a[1]) {
        py_dict_put(r, "__thisclass__", a[1]);
        py_dict_put(r, "__self__", a[0]);
    } else if (n && a[0]) {
        py_dict_put(r, "__self__", a[0]);
    }
    return r;
}

static PyVal *bi_memoryview(PyVal **a, size_t n, PyVal *kw) {
    PyVal *v = b_one(a, n), *r = py_dict_new();
    (void)kw;
    py_dict_put(r, "obj", v);
    py_dict_put(r, "nbytes", py_int((int64_t)py_cstr_len(v)));
    py_dict_put(r, "format", py_str("B"));
    return r;
}

static PyVal *bi_help(PyVal **a, size_t n, PyVal *kw) {
    PyVal *v = b_one(a, n), *s = py_repr(v);
    (void)kw;
    printf("Help on object:\n\n%s\n", py_cstr(s));
    py_release(s);
    return py_none();
}

static PyVal *bi_exit(PyVal **a, size_t n, PyVal *kw) {
    int code = n ? (int)py_as_int(a[0]) : 0;
    (void)kw;
    exit(code);
}

static PyVal *bi_notimpl(PyVal **a, size_t n, PyVal *kw) {
    (void)a; (void)n; (void)kw;
    py_raise(PY_EXC_UNSUPPORTED, "not available in compiled C mode");
    return py_none();
}

PyVal *py_builtin_extra_lookup(const char *name) {
    static const struct { const char *name; PyCFunc fn; } table[] = {
        { "ascii", bi_ascii },
        { "issubclass", bi_issubclass },
        { "delattr", bi_delattr },
        { "aiter", bi_aiter },
        { "anext", bi_anext },
        { "breakpoint", bi_breakpoint },
        { "globals", bi_globals },
        { "locals", bi_locals },
        { "__debug__", bi_debug },
        { "dir", bi_dir },
        { "vars", bi_vars },
        { "object", bi_object },
        { "bytes", bi_bytes },
        { "bytearray", bi_bytearray },
        { "__import__", bi_import },
        { "property", bi_property },
        { "staticmethod", bi_staticmethod },
        { "classmethod", bi_classmethod },
        { "super", bi_super },
        { "memoryview", bi_memoryview },
        { "help", bi_help },
        { "exit", bi_exit },
        { "quit", bi_exit },
        { "compile", bi_compile },
        { "eval", bi_notimpl },
        { "exec", bi_notimpl },
        { NULL, NULL }
    };
    size_t i;
    for (i = 0; table[i].name; i++)
        if (b_eq(table[i].name, name)) return py_func(table[i].fn, table[i].name);
    return NULL;
}
