/*
 * py2c99 standard-library shims.
 *
 * These are pure C99 implementations of the useful, deterministic part of a
 * large set of Python standard-library modules.  They are deliberately kept
 * in a separate translation unit: pyrt.c owns the object model, this file
 * only adds module members on top of its public API.
 */
#define _CRT_SECURE_NO_WARNINGS
#include "pyrt.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <inttypes.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <conio.h>
#else
#include <unistd.h>
#include <dirent.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------ helpers */

typedef struct {
    char *data;
    size_t len, cap;
} SBuf;

static void sb_init(SBuf *b) { b->data = NULL; b->len = b->cap = 0; }

static void sb_reserve(SBuf *b, size_t extra) {
    if (b->len + extra + 1 > b->cap) {
        size_t n = b->cap ? b->cap * 2 : 64;
        while (n < b->len + extra + 1) n *= 2;
        b->data = (char *)realloc(b->data, n);
        if (!b->data) { fputs("py2c99: out of memory\n", stderr); exit(1); }
        b->cap = n;
    }
}

static void sb_add(SBuf *b, const char *s, size_t n) {
    sb_reserve(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}

static void sb_adds(SBuf *b, const char *s) { sb_add(b, s, strlen(s)); }
static void sb_addc(SBuf *b, char c) { sb_add(b, &c, 1); }

static void sb_printf(SBuf *b, const char *fmt, ...) {
    va_list ap, ap2;
    int n;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return; }
    sb_reserve(b, (size_t)n);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
}

static char *sb_take(SBuf *b) {
    char *s = b->data ? b->data : (char *)calloc(1, 1);
    b->data = NULL;
    b->len = b->cap = 0;
    return s;
}

static int s_eq(const char *a, const char *b) { return a && b && strcmp(a, b) == 0; }

static PyVal *one_arg(PyVal **args, size_t argc, const char *what) {
    if (!args || argc < 1) {
        py_raise(PY_EXC_VALUE, "missing argument");
        return py_none();
    }
    (void)what;
    return args[0];
}

static int64_t iarg(PyVal **args, size_t argc, size_t i, int64_t fallback) {
    return (args && i < argc) ? py_as_int(args[i]) : fallback;
}

static double darg(PyVal **args, size_t argc, size_t i, double fallback) {
    return (args && i < argc) ? py_as_float(args[i]) : fallback;
}

static const char *sarg(PyVal **args, size_t argc, size_t i, const char *fallback) {
    return (args && i < argc) ? py_cstr(args[i]) : fallback;
}

static PyVal *to_list(PyVal *v) { return py_builtin_list(&v, 1, NULL); }

static void sort_list(PyVal *list, int reverse) {
    PyVal *r;
    if (reverse) {
        PyVal *a[1];
        a[0] = py_bool(1);
        r = py_method(list, "sort", a, 1, NULL);
    } else {
        r = py_method(list, "sort", NULL, 0, NULL);
    }
    py_release(r);
}

static PyVal *retain_same(PyVal *v) { py_retain(v); return v; }

static uint64_t s_rng = 0x9E3779B97F4A7C15ULL;
static uint64_t sr_next(void) {
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 7;
    s_rng ^= s_rng << 17;
    return s_rng;
}
static double sr_double(void) {
    return (double)(sr_next() >> 11) / 9007199254740992.0;
}

/* --------------------------------------------------------------- constants */

enum { CK_STR, CK_INT, CK_FLOAT };

typedef struct {
    const char *module;
    const char *name;
    int kind;
    const char *s;
    double f;
    int64_t i;
} ConstEntry;

#define CS(m, n, v)   { m, n, CK_STR,   v, 0.0, 0 }
#define CI(m, n, v)   { m, n, CK_INT,   NULL, 0.0, (int64_t)(v) }
#define CF(m, n, v)   { m, n, CK_FLOAT, NULL, (double)(v), 0 }

static const ConstEntry g_consts[] = {
    CF("cmath", "pi", 3.14159265358979323846),
    CF("cmath", "e", 2.71828182845904523536),
    CF("cmath", "tau", 6.28318530717958647692),
    CF("cmath", "inf", INFINITY),
    CF("cmath", "nan", NAN),

    CS("sys", "version_info", "3.11 (py2c99)"),
    CS("sys", "implementation", "py2c99"),
    CS("sys", "byteorder", "little"),
    CS("sys", "prefix", ""),
    CS("sys", "pathsep", ";"),
    CS("sys", "dont_write_bytecode", "False"),

    CS("platform", "machine", "AMD64"),
    CS("platform", "release", "10.0"),
    CS("platform", "version", "10.0"),
    CS("platform", "python_implementation", "py2c99"),
    CS("platform", "win32_ver", "('10', '10.0', '', 'Multiprocessor Free')"),

    CS("os", "environ", "<environ>"),
    CS("os.path", "sep", "\\"),
    CS("os.path", "altsep", "/"),

    CI("errno", "ENOENT", 2),
    CI("errno", "EACCES", 13),
    CI("errno", "EEXIST", 17),
    CI("errno", "EINVAL", 22),
    CI("errno", "EPERM", 1),
    CI("errno", "ENOTDIR", 20),
    CI("errno", "EISDIR", 21),

    CI("stat", "S_IFMT", 61440),
    CI("stat", "S_IFREG", 32768),
    CI("stat", "S_IFDIR", 16384),
    CI("stat", "S_IFLNK", 40960),
    CI("stat", "S_IRUSR", 256),
    CI("stat", "S_IWUSR", 128),
    CI("stat", "S_IXUSR", 64),

    CI("signal", "SIGINT", 2),
    CI("signal", "SIGBREAK", 21),
    CI("signal", "SIGTERM", 15),
    CI("signal", "SIGABRT", 22),
    CI("signal", "SIGSEGV", 11),

    CI("socket", "AF_INET", 2),
    CI("socket", "AF_INET6", 23),
    CI("socket", "SOCK_STREAM", 1),
    CI("socket", "SOCK_DGRAM", 2),

    CS("ssl", "OPENSSL_VERSION", "py2c99"),

    CI("re", "IGNORECASE", 2),
    CI("re", "I", 2),
    CI("re", "MULTILINE", 8),
    CI("re", "M", 8),
    CI("re", "DOTALL", 16),
    CI("re", "S", 16),
    CI("re", "VERBOSE", 64),
    CI("re", "X", 64),
    CI("re", "ASCII", 256),

    CI("logging", "DEBUG", 10),
    CI("logging", "INFO", 20),
    CI("logging", "WARNING", 30),
    CI("logging", "ERROR", 40),
    CI("logging", "CRITICAL", 50),
    CI("logging", "WARN", 30),
    CI("logging", "FATAL", 50),
    CI("logging", "NOTSET", 0),

    CS("typing", "Any", "Any"),
    CS("typing", "Optional", "Optional"),
    CS("typing", "Union", "Union"),
    CS("typing", "Callable", "Callable"),
    CS("typing", "Iterable", "Iterable"),
    CS("typing", "Iterator", "Iterator"),
    CS("typing", "Sequence", "Sequence"),
    CS("typing", "Mapping", "Mapping"),
    CS("typing", "List", "list"),
    CS("typing", "Dict", "dict"),
    CS("typing", "Set", "set"),
    CS("typing", "Tuple", "tuple"),
    CS("typing", "NoReturn", "NoReturn"),
    CS("typing", "Literal", "Literal"),
    CS("typing", "Final", "Final"),
    CS("typing", "Protocol", "Protocol"),
    CS("typing", "Self", "Self"),
    CS("typing", "TypeAlias", "TypeAlias"),

    CS("numbers", "Number", "Number"),
    CS("numbers", "Complex", "Complex"),
    CS("numbers", "Real", "Real"),
    CS("numbers", "Integral", "Integral"),
    CS("numbers", "Rational", "Rational"),

    CS("html.entities", "amp", "&"),
    CS("html.entities", "lt", "<"),
    CS("html.entities", "gt", ">"),
    CS("html.entities", "quot", "\""),
    CS("html.entities", "nbsp", "\xC2\xA0"),

    CS("base64", "b64alphabet", "ABCDEFGHIJKLMNOPQRSTUVWXYZ"),
    CS("unicodedata", "unidata_version", "14.0.0"),

    CS("timeit", "default_timer", "perf_counter"),
    CS("uuid", "NAMESPACE_DNS", "6ba7b810-9dad-11d1-80b4-00c04fd430c8"),
    CS("uuid", "NAMESPACE_URL", "6ba7b811-9dad-11d1-80b4-00c04fd430c8"),

    CS("http", "HTTPStatus", "200"),
    CS("ssl", "CERT_NONE", "0"),
    CS("ssl", "CERT_REQUIRED", "2"),

    CS("array", "typecodes", "bBhHiIlLqQfd"),
    CS("queue", "Empty", "Empty"),
    CS("queue", "Full", "Full")
};

#undef CS
#undef CI
#undef CF

/* ------------------------------------------------------------------- names */

static const char *canonical_module(const char *module) {
    if (s_eq(module, "builtins") || s_eq(module, "__builtin__")) return "builtins";
    if (s_eq(module, "ntpath") || s_eq(module, "posixpath") ||
        s_eq(module, "genericpath")) return "os.path";
    if (s_eq(module, "collections.abc")) return "collections";
    if (s_eq(module, "urllib.parse")) return "urllib";
    if (s_eq(module, "html.entities")) return "html.entities";
    if (s_eq(module, "urllib.error")) return "urllib.request";
    return module ? module : "";
}

static const char *g_modules[] = {
    "builtins", "cmath", "operator", "collections", "collections.abc",
    "heapq", "bisect", "array", "queue", "copy", "pprint", "types",
    "enum", "dataclasses", "abc", "contextlib", "typing", "numbers",
    "gc", "json", "csv", "re", "textwrap", "html", "html.entities",
    "base64", "binascii", "zlib", "struct", "codecs", "unicodedata",
    "pathlib", "glob", "fnmatch", "shutil", "tempfile", "subprocess",
    "stat", "errno", "os.path", "ntpath", "posixpath", "getpass", "sysconfig",
    "datetime", "calendar", "timeit", "uuid", "hashlib", "secrets",
    "hmac", "socket", "ssl", "urllib", "urllib.parse", "urllib.request",
    "http", "logging", "getopt", "msvcrt", "threading", "multiprocessing",
    "signal", "select", "weakref", "inspect", NULL
};

/* ------------------------------------------------------- builtins as module */

typedef struct { const char *name; PyCFunc fn; } FnEntry;

static PyVal *bi_slice(PyVal **args, size_t argc, PyVal *kwargs) {
    PyVal *f = py_builtin_lookup("slice");
    PyVal *r;
    if (!f) return py_none();
    r = py_call(f, args, argc, kwargs);
    py_release(f);
    return r;
}

static const FnEntry g_builtins[] = {
    { "print", py_print }, { "input", py_input },
    { "len", py_builtin_len }, { "int", py_builtin_int },
    { "float", py_builtin_float }, { "str", py_builtin_str },
    { "bool", py_builtin_bool }, { "list", py_builtin_list },
    { "tuple", py_builtin_tuple }, { "dict", py_builtin_dict },
    { "set", py_builtin_set }, { "frozenset", py_builtin_set },
    { "range", py_builtin_range }, { "abs", py_builtin_abs },
    { "min", py_builtin_min }, { "max", py_builtin_max },
    { "sum", py_builtin_sum }, { "sorted", py_builtin_sorted },
    { "reversed", py_builtin_reversed }, { "enumerate", py_builtin_enumerate },
    { "zip", py_builtin_zip }, { "any", py_builtin_any },
    { "all", py_builtin_all }, { "chr", py_builtin_chr },
    { "ord", py_builtin_ord }, { "round", py_builtin_round },
    { "pow", py_builtin_pow }, { "divmod", py_builtin_divmod },
    { "hex", py_builtin_hex }, { "oct", py_builtin_oct },
    { "bin", py_builtin_bin }, { "repr", py_builtin_repr },
    { "format", py_builtin_format }, { "id", py_builtin_id },
    { "hash", py_builtin_hash }, { "isinstance", py_builtin_isinstance },
    { "type", py_builtin_type }, { "open", py_builtin_open },
    { "iter", py_builtin_iter }, { "next", py_builtin_next },
    { "map", py_builtin_map }, { "filter", py_builtin_filter },
    { "getattr", py_builtin_getattr }, { "setattr", py_builtin_setattr },
    { "hasattr", py_builtin_hasattr }, { "callable", py_builtin_callable },
    { "slice", bi_slice }, { NULL, NULL }
};

static PyVal *bi_dir(PyVal **args, size_t argc, PyVal *kwargs) {
    PyVal *r = py_list_new(), *s;
    size_t i;
    (void)kwargs;
    (void)args; (void)argc;
    for (i = 0; g_builtins[i].name; i++) {
        s = py_str(g_builtins[i].name);
        py_list_append(r, s);
        py_release(s);
    }
    sort_list(r, 0);
    return r;
}

static PyVal *bi_vars(PyVal **args, size_t argc, PyVal *kwargs) {
    (void)kwargs;
    if (argc && args[0]) return retain_same(args[0]);
    return py_dict_new();
}

static PyVal *bi_notimpl(PyVal **args, size_t argc, PyVal *kwargs) {
    (void)args; (void)argc; (void)kwargs;
    py_raise(PY_EXC_UNSUPPORTED, "not available in py2c99 C mode");
    return py_none();
}

static int builtins_call(const char *name, PyVal **args, size_t argc,
                         PyVal *kwargs, PyVal **out) {
    size_t i;
    for (i = 0; g_builtins[i].name; i++) {
        if (s_eq(name, g_builtins[i].name) && g_builtins[i].fn) {
            *out = g_builtins[i].fn(args, argc, kwargs);
            return 1;
        }
    }
    if (s_eq(name, "dir")) { *out = bi_dir(args, argc, kwargs); return 1; }
    if (s_eq(name, "vars")) { *out = bi_vars(args, argc, kwargs); return 1; }
    if (s_eq(name, "compile") || s_eq(name, "eval") || s_eq(name, "exec"))
        { *out = bi_notimpl(args, argc, kwargs); return 1; }
    return 0;
}

/* ------------------------------------------------------------------- cmath */

static int cmath_call(const char *name, PyVal **args, size_t argc, PyVal **out) {
    double a = darg(args, argc, 0, 0.0), b = darg(args, argc, 1, 0.0);
    if (s_eq(name, "sqrt")) { *out = py_float(sqrt(fabs(a))); return 1; }
    if (s_eq(name, "sin")) { *out = py_float(sin(a)); return 1; }
    if (s_eq(name, "cos")) { *out = py_float(cos(a)); return 1; }
    if (s_eq(name, "tan")) { *out = py_float(tan(a)); return 1; }
    if (s_eq(name, "exp")) { *out = py_float(exp(a) * cos(b)); return 1; }
    if (s_eq(name, "log")) { *out = py_float(log(fabs(a))); return 1; }
    if (s_eq(name, "log10")) { *out = py_float(log10(fabs(a))); return 1; }
    if (s_eq(name, "phase")) { *out = py_float(b == 0.0 && a < 0.0 ? M_PI : atan2(b, a)); return 1; }
    if (s_eq(name, "polar")) {
        PyVal *v[2];
        v[0] = py_float(sqrt(a * a + b * b));
        v[1] = py_float(atan2(b, a));
        *out = py_tuple_from(v, 2);
        py_release(v[0]); py_release(v[1]);
        return 1;
    }
    if (s_eq(name, "rect")) { *out = py_float(a * cos(b)); return 1; }
    if (s_eq(name, "isclose")) {
        double rel = darg(args, argc, 2, 1e-9), abs = darg(args, argc, 3, 0.0);
        *out = py_bool(fabs(a - b) <= fmax(rel * fmax(fabs(a), fabs(b)), abs));
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- operator */

static int operator_call(const char *name, PyVal **args, size_t argc, PyVal **out) {
    PyVal *a = one_arg(args, argc, name), *b = argc > 1 ? args[1] : a;
    if (s_eq(name, "add")) { *out = py_binop(PY_OP_ADD, a, b); return 1; }
    if (s_eq(name, "sub")) { *out = py_binop(PY_OP_SUB, a, b); return 1; }
    if (s_eq(name, "mul")) { *out = py_binop(PY_OP_MUL, a, b); return 1; }
    if (s_eq(name, "truediv")) { *out = py_binop(PY_OP_DIV, a, b); return 1; }
    if (s_eq(name, "floordiv")) { *out = py_binop(PY_OP_FLOORDIV, a, b); return 1; }
    if (s_eq(name, "mod")) { *out = py_binop(PY_OP_MOD, a, b); return 1; }
    if (s_eq(name, "pow")) { *out = py_binop(PY_OP_POW, a, b); return 1; }
    if (s_eq(name, "matmul")) { *out = py_binop(PY_OP_MATMUL, a, b); return 1; }
    if (s_eq(name, "lshift")) { *out = py_binop(PY_OP_LSHIFT, a, b); return 1; }
    if (s_eq(name, "rshift")) { *out = py_binop(PY_OP_RSHIFT, a, b); return 1; }
    if (s_eq(name, "and_")) { *out = py_binop(PY_OP_BITAND, a, b); return 1; }
    if (s_eq(name, "or_")) { *out = py_binop(PY_OP_BITOR, a, b); return 1; }
    if (s_eq(name, "xor")) { *out = py_binop(PY_OP_BITXOR, a, b); return 1; }
    if (s_eq(name, "lt")) { *out = py_bool(py_cmp(PY_CMP_LT, a, b)); return 1; }
    if (s_eq(name, "le")) { *out = py_bool(py_cmp(PY_CMP_LE, a, b)); return 1; }
    if (s_eq(name, "gt")) { *out = py_bool(py_cmp(PY_CMP_GT, a, b)); return 1; }
    if (s_eq(name, "ge")) { *out = py_bool(py_cmp(PY_CMP_GE, a, b)); return 1; }
    if (s_eq(name, "eq")) { *out = py_bool(py_cmp(PY_CMP_EQ, a, b)); return 1; }
    if (s_eq(name, "ne")) { *out = py_bool(py_cmp(PY_CMP_NE, a, b)); return 1; }
    if (s_eq(name, "is_")) { *out = py_bool(py_cmp(PY_CMP_IS, a, b)); return 1; }
    if (s_eq(name, "is_not")) { *out = py_bool(py_cmp(PY_CMP_ISNOT, a, b)); return 1; }
    if (s_eq(name, "contains")) { *out = py_bool(py_contains(a, b)); return 1; }
    if (s_eq(name, "not_")) { *out = py_bool(!py_truth(a)); return 1; }
    if (s_eq(name, "truth")) { *out = py_bool(py_truth(a)); return 1; }
    if (s_eq(name, "neg")) { *out = py_unaryop(0, a); return 1; }
    if (s_eq(name, "pos")) { *out = py_unaryop(1, a); return 1; }
    if (s_eq(name, "invert")) { *out = py_unaryop(2, a); return 1; }
    if (s_eq(name, "index") || s_eq(name, "length_hint"))
        { *out = py_int((int64_t)py_len(a)); return 1; }
    return 0;
}

/* -------------------------------------------------------------- collections */

static PyVal *make_counter(PyVal *seq) {
    PyVal *d = py_dict_new(), *it = py_iter(seq), *item = NULL;
    while (py_iter_next(it, &item)) {
        if (py_contains(d, item)) {
            PyVal *n = py_getitem(d, item);
            PyVal *next = py_int(py_as_int(n) + 1);
            py_release(n);
            py_setitem(d, item, next);
            py_release(next);
        } else {
            py_setitem(d, item, py_int(1));
        }
    }
    py_release(item);
    py_release(it);
    return d;
}

static int collections_call(const char *name, PyVal **args, size_t argc,
                            PyVal *kwargs, PyVal **out) {
    if (s_eq(name, "Counter")) {
        if (argc && args[0]) *out = make_counter(args[0]);
        else if (kwargs) *out = retain_same(kwargs);
        else *out = py_dict_new();
        return 1;
    }
    if (s_eq(name, "OrderedDict") || s_eq(name, "defaultdict") ||
        s_eq(name, "UserDict") || s_eq(name, "ChainMap")) {
        PyVal *r = py_dict_new();
        size_t i;
        if (kwargs) py_dict_update(r, kwargs);
        for (i = 0; i < argc; i++) py_dict_update(r, args[i]);
        return (*out = r), 1;
    }
    if (s_eq(name, "deque") || s_eq(name, "UserList")) {
        *out = argc ? to_list(args[0]) : py_list_new();
        return 1;
    }
    if (s_eq(name, "UserString")) {
        *out = argc ? py_str_of(args[0]) : py_str("");
        return 1;
    }
    if (s_eq(name, "namedtuple")) {
        PyVal *r = py_dict_new();
        PyVal *fields = argc > 1 ? to_list(args[1]) : NULL;
        size_t i, n = fields ? py_len(fields) : 0;
        for (i = 0; i < n; i++) {
            PyVal *f = py_getitem(fields, py_int((int64_t)i));
            PyVal *s = py_str_of(f);
            py_dict_put(r, py_cstr(s), py_none());
            py_release(s);
            py_release(f);
        }
        py_release(fields);
        *out = r;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------- heapq */

static PyVal *mutable_list(PyVal *v) {
    if (!v || v->type != PY_T_LIST) {
        py_raise(PY_EXC_TYPE, "expected a mutable list");
        return py_none();
    }
    return retain_same(v);
}

static int heapq_call(const char *name, PyVal **args, size_t argc, PyVal **out) {
    if (s_eq(name, "heapify")) {
        PyVal *l = mutable_list(one_arg(args, argc, "heapify"));
        sort_list(l, 0);
        *out = py_none();
        return 1;
    }
    if (s_eq(name, "heappush")) {
        PyVal *l = mutable_list(one_arg(args, argc, "heappush"));
        if (argc > 1)
            py_release(py_method(l, "append", &args[1], 1, NULL));
        sort_list(l, 0);
        *out = py_none();
        return 1;
    }
    if (s_eq(name, "heappop")) {
        PyVal *l = mutable_list(one_arg(args, argc, "heappop"));
        PyVal *r;
        if (py_len(l) == 0) { py_raise(PY_EXC_INDEX, "index out of range"); return 1; }
        r = py_getitem(l, py_int(0));
        py_delitem(l, py_int(0));
        *out = r;
        return 1;
    }
    if (s_eq(name, "heappushpop")) {
        PyVal *l = mutable_list(one_arg(args, argc, "heappushpop"));
        PyVal *r;
        if (argc > 1) py_release(py_method(l, "append", &args[1], 1, NULL));
        sort_list(l, 0);
        r = py_getitem(l, py_int(0));
        py_delitem(l, py_int(0));
        *out = r;
        return 1;
    }
    if (s_eq(name, "heapreplace")) {
        PyVal *l = mutable_list(one_arg(args, argc, "heapreplace"));
        PyVal *r;
        if (py_len(l) == 0) { py_raise(PY_EXC_INDEX, "index out of range"); return 1; }
        r = py_getitem(l, py_int(0));
        py_delitem(l, py_int(0));
        if (argc > 1) py_release(py_method(l, "append", &args[1], 1, NULL));
        sort_list(l, 0);
        *out = r;
        return 1;
    }
    if (s_eq(name, "nlargest") || s_eq(name, "nsmallest")) {
        PyVal *l = to_list(one_arg(args, argc, name));
        int rev = s_eq(name, "nlargest");
        sort_list(l, rev);
        {
            int64_t n = iarg(args, argc, 1, 1);
            PyVal *lo = py_int(0), *hi = py_int(n), *step = py_int(1);
            *out = py_getslice(l, lo, hi, step);
            py_release(lo); py_release(hi); py_release(step);
        }
        py_release(l);
        return 1;
    }
    if (s_eq(name, "merge")) {
        PyVal *r = py_list_new();
        size_t i;
        for (i = 0; i < argc; i++) {
            PyVal *l = to_list(args[i]), *it = py_iter(l), *item = NULL;
            while (py_iter_next(it, &item)) py_list_append(r, item);
            py_release(item);
            py_release(it);
            py_release(l);
        }
        *out = r;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ bisect */

/* Python's bisect_left advances while a[mid] < x; bisect_right advances
   while x >= a[mid].  Getting this backwards is an easy source of bugs. */
static size_t bisect_pos(PyVal *list, PyVal *x, int right) {
    size_t lo = 0, hi = py_len(list);
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        PyVal *key = py_int((int64_t)mid), *v = py_getitem(list, key);
        int advance = right ? !py_cmp(PY_CMP_LT, x, v)
                            : py_cmp(PY_CMP_LT, v, x);
        py_release(v);
        py_release(key);
        if (advance) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static int bisect_call(const char *name, PyVal **args, size_t argc, PyVal **out) {
    if (s_eq(name, "bisect") || s_eq(name, "bisect_left") ||
        s_eq(name, "bisect_right")) {
        PyVal *l = to_list(one_arg(args, argc, name));
        PyVal *x = argc > 1 ? args[1] : py_none();
        int right = s_eq(name, "bisect_right") || s_eq(name, "bisect");
        size_t pos = bisect_pos(l, x, right);
        py_release(l);
        *out = py_int((int64_t)pos);
        return 1;
    }
    if (s_eq(name, "insort") || s_eq(name, "insort_left") ||
        s_eq(name, "insort_right")) {
        PyVal *l = mutable_list(one_arg(args, argc, name));
        PyVal *x = argc > 1 ? args[1] : py_none();
        int right = s_eq(name, "insort") || s_eq(name, "insort_right");
        PyVal *tmp[2];
        size_t pos = bisect_pos(l, x, right);
        tmp[0] = py_int((int64_t)pos); tmp[1] = x;
        py_release(py_method(l, "insert", tmp, 2, NULL));
        py_release(tmp[0]);
        py_release(l);
        *out = py_none();
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------- array */

static int array_call(const char *name, PyVal **args, size_t argc, PyVal **out) {
    if (s_eq(name, "array")) {
        *out = argc > 1 ? to_list(args[1]) : py_list_new();
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ queue */

static int queue_call(const char *name, PyVal **args, size_t argc, PyVal **out) {
    (void)args; (void)argc;
    if (s_eq(name, "Queue") || s_eq(name, "SimpleQueue") ||
        s_eq(name, "PriorityQueue") || s_eq(name, "LifoQueue") ||
        s_eq(name, "JoinableQueue")) { *out = py_list_new(); return 1; }
    return 0;
}

/* ------------------------------------------------------------------- copy */

static int copy_call(const char *name, PyVal **args, size_t argc, PyVal **out) {
    PyVal *v = one_arg(args, argc, name);
    if (s_eq(name, "copy") || s_eq(name, "deepcopy")) {
        if (v->type == PY_T_LIST || v->type == PY_T_DICT || v->type == PY_T_SET)
            *out = py_method(v, "copy", NULL, 0, NULL);
        else
            *out = retain_same(v);
        return 1;
    }
    if (s_eq(name, "copyreg")) { *out = py_dict_new(); return 1; }
    return 0;
}

/* ----------------------------------------------------------------- pprint */

static int pprint_call(const char *name, PyVal **args, size_t argc,
                       PyVal *kwargs, PyVal **out) {
    PyVal *s;
    (void)kwargs;
    if (!s_eq(name, "pformat") && !s_eq(name, "pprint") && !s_eq(name, "pp"))
        return 0;
    s = py_repr(one_arg(args, argc, name));
    if (s_eq(name, "pformat")) { *out = s; return 1; }
    *out = py_print(&s, 1, NULL);
    py_release(s);
    return 1;
}

/* ------------------------------------------------------------------ types */

static int types_call(const char *name, PyVal **args, size_t argc,
                      PyVal *kwargs, PyVal **out) {
    if (s_eq(name, "SimpleNamespace")) {
        PyVal *r = py_dict_new();
        if (argc && args[0] && args[0]->type == PY_T_DICT) py_dict_update(r, args[0]);
        if (kwargs) py_dict_update(r, kwargs);
        *out = r;
        return 1;
    }
    if (s_eq(name, "MappingProxyType") || s_eq(name, "FunctionType") ||
        s_eq(name, "LambdaType") || s_eq(name, "new")) {
        *out = argc ? retain_same(args[0]) : py_none();
        return 1;
    }
    if (s_eq(name, "GenericAlias")) {
        PyVal *r = py_str_of(one_arg(args, argc, name));
        *out = r;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------- enum */

static int enum_call(const char *name, PyVal **args, size_t argc,
                     PyVal *kwargs, PyVal **out) {
    if (s_eq(name, "Enum") || s_eq(name, "IntEnum") || s_eq(name, "Flag") ||
        s_eq(name, "IntFlag") || s_eq(name, "StrEnum")) {
        PyVal *r = py_dict_new();
        size_t i;
        if (argc > 1) {
            PyVal *names = to_list(args[1]);
            for (i = 0; i < py_len(names); i++) {
                PyVal *n = py_getitem(names, py_int((int64_t)i));
                PyVal *s = py_str_of(n);
                py_dict_put(r, py_cstr(s), py_int((int64_t)i + 1));
                py_release(s);
                py_release(n);
            }
            py_release(names);
        }
        if (kwargs) py_dict_update(r, kwargs);
        *out = r;
        return 1;
    }
    if (s_eq(name, "auto")) { *out = py_int(iarg(args, argc, 0, 1)); return 1; }
    return 0;
}

/* ------------------------------------------------------------ dataclasses */

static int dataclasses_call(const char *name, PyVal **args, size_t argc,
                            PyVal *kwargs, PyVal **out) {
    (void)kwargs;
    if (s_eq(name, "dataclass") || s_eq(name, "final") ||
        s_eq(name, "asdict") || s_eq(name, "astuple") || s_eq(name, "replace")) {
        *out = argc ? retain_same(args[0]) : py_none();
        return 1;
    }
    if (s_eq(name, "field")) {
        PyVal *r = py_dict_new();
        if (argc && args[0] && args[0]->type == PY_T_DICT) py_dict_update(r, args[0]);
        if (kwargs) py_dict_update(r, kwargs);
        *out = r;
        return 1;
    }
    if (s_eq(name, "fields")) { *out = py_list_new(); return 1; }
    return 0;
}

/* -------------------------------------------------------------------- abc */

static int abc_call(const char *name, PyVal **args, size_t argc, PyVal **out) {
    if (s_eq(name, "ABC") || s_eq(name, "ABCMeta") || s_eq(name, "Meta")) {
        *out = py_dict_new();
        return 1;
    }
    if (s_eq(name, "abstractmethod") || s_eq(name, "abstractclassmethod") ||
        s_eq(name, "abstractstaticmethod") || s_eq(name, "abstractproperty")) {
        *out = argc ? retain_same(args[0]) : py_none();
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------ contextlib */

static int contextlib_call(const char *name, PyVal **args, size_t argc,
                           PyVal **out) {
    if (s_eq(name, "contextmanager") || s_eq(name, "closing") ||
        s_eq(name, "suppress") || s_eq(name, "asynccontextmanager")) {
        *out = argc ? retain_same(args[0]) : py_none();
        return 1;
    }
    if (s_eq(name, "nullcontext")) { *out = py_dict_new(); return 1; }
    return 0;
}

/* ----------------------------------------------------------------- typing */

static int typing_call(const char *name, PyVal **args, size_t argc,
                       PyVal **out) {
    if (s_eq(name, "cast")) { *out = argc > 1 ? retain_same(args[1]) : py_none(); return 1; }
    if (s_eq(name, "get_type_hints") || s_eq(name, "get_args") ||
        s_eq(name, "get_origin") || s_eq(name, "get_params")) {
        *out = s_eq(name, "get_args") || s_eq(name, "get_params") ? py_list_new() : py_dict_new();
        return 1;
    }
    if (s_eq(name, "NewType")) { *out = argc > 1 ? retain_same(args[1]) : py_none(); return 1; }
    if (s_eq(name, "overload") || s_eq(name, "runtime_checkable") ||
        s_eq(name, "final") || s_eq(name, "no_type_check")) {
        *out = argc ? retain_same(args[0]) : py_none();
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ gc etc */

static int misc_call(const char *module, const char *name, PyVal **args,
                     size_t argc, PyVal **out) {
    if (s_eq(module, "gc")) {
        if (s_eq(name, "collect")) { *out = py_int(0); return 1; }
        if (s_eq(name, "disable") || s_eq(name, "enable")) { *out = py_none(); return 1; }
        if (s_eq(name, "isenabled")) { *out = py_bool(0); return 1; }
        if (s_eq(name, "get_objects")) { *out = py_list_new(); return 1; }
    }
    if (s_eq(module, "numbers")) {
        if (s_eq(name, " Integral") || s_eq(name, "Real") || s_eq(name, "Complex")) {
            *out = py_str(name);
            return 1;
        }
    }
    return 0;
}

/* -------------------------------------------------------------------- json */

static void json_quote(SBuf *b, const char *s, size_t n) {
    size_t i;
    sb_addc(b, '"');
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': sb_adds(b, "\\\""); break;
        case '\\': sb_adds(b, "\\\\"); break;
        case '\b': sb_adds(b, "\\b"); break;
        case '\f': sb_adds(b, "\\f"); break;
        case '\n': sb_adds(b, "\\n"); break;
        case '\r': sb_adds(b, "\\r"); break;
        case '\t': sb_adds(b, "\\t"); break;
        default:
            if (c < 32) sb_printf(b, "\\u%04x", (unsigned)c);
            else sb_addc(b, (char)c);
        }
    }
    sb_addc(b, '"');
}

static void json_write(SBuf *b, PyVal *v) {
    switch (v->type) {
    case PY_T_NONE: sb_adds(b, "null"); break;
    case PY_T_BOOL: sb_adds(b, v->u.b ? "true" : "false"); break;
    case PY_T_INT: sb_printf(b, "%" PRId64, v->u.i); break;
    case PY_T_FLOAT: sb_printf(b, "%.15g", v->u.f); break;
    case PY_T_STR: json_quote(b, v->u.s.data, v->u.s.len); break;
    case PY_T_LIST: case PY_T_TUPLE: case PY_T_SET: case PY_T_RANGE: {
        PyVal *it = py_iter(v), *item = NULL;
        int first = 1;
        sb_addc(b, '[');
        while (py_iter_next(it, &item)) {
            if (!first) sb_adds(b, ", ");
            first = 0;
            json_write(b, item);
        }
        py_release(item);
        py_release(it);
        sb_addc(b, ']');
        break;
    }
    case PY_T_DICT: {
        PyVal *it = py_iter(v), *key = NULL;
        int first = 1;
        sb_addc(b, '{');
        while (py_iter_next(it, &key)) {
            PyVal *sv = py_str_of(key), *val = py_getitem(v, key);
            if (!first) sb_adds(b, ", ");
            first = 0;
            json_quote(b, py_cstr(sv), py_cstr_len(sv));
            sb_adds(b, ": ");
            json_write(b, val);
            py_release(val);
            py_release(sv);
        }
        py_release(key);
        py_release(it);
        sb_addc(b, '}');
        break;
    }
    default: {
        PyVal *sv = py_str_of(v);
        json_quote(b, py_cstr(sv), py_cstr_len(sv));
        py_release(sv);
        break;
    }
    }
}

typedef struct { const char *p; } JParser;

static void jp_ws(JParser *j) { while (*j->p && isspace((unsigned char)*j->p)) j->p++; }

static void u8_put(SBuf *b, uint32_t cp) {
    if (cp < 0x80) sb_addc(b, (char)cp);
    else if (cp < 0x800) {
        sb_addc(b, (char)(0xC0 | (cp >> 6)));
        sb_addc(b, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        sb_addc(b, (char)(0xE0 | (cp >> 12)));
        sb_addc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_addc(b, (char)(0x80 | (cp & 0x3F)));
    } else {
        sb_addc(b, (char)(0xF0 | (cp >> 18)));
        sb_addc(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
        sb_addc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_addc(b, (char)(0x80 | (cp & 0x3F)));
    }
}

static PyVal *jp_string(JParser *j) {
    SBuf b;
    sb_init(&b);
    if (*j->p != '"') { py_raise(PY_EXC_VALUE, "invalid JSON string"); return py_none(); }
    j->p++;
    while (*j->p && *j->p != '"') {
        unsigned char c = (unsigned char)*j->p++;
        if (c == '\\') {
            unsigned char e = (unsigned char)*j->p++;
            switch (e) {
            case 'n': sb_addc(&b, '\n'); break;
            case 'r': sb_addc(&b, '\r'); break;
            case 't': sb_addc(&b, '\t'); break;
            case 'b': sb_addc(&b, '\b'); break;
            case 'f': sb_addc(&b, '\f'); break;
            case 'u': {
                unsigned cp = 0, k;
                for (k = 0; k < 4 && isxdigit((unsigned char)*j->p); k++)
                    cp = (cp << 4) | (unsigned)(isdigit((unsigned char)*j->p)
                         ? *j->p - '0' : (tolower((unsigned char)*j->p) - 'a' + 10)), j->p++;
                u8_put(&b, cp);
                break;
            }
            default: sb_addc(&b, (char)e); break;
            }
        } else {
            sb_addc(&b, (char)c);
        }
    }
    if (*j->p == '"') j->p++;
    {
        char *s = sb_take(&b);
        PyVal *r = py_str(s);
        free(s);
        return r;
    }
}

static PyVal *jp_value(JParser *j);

static PyVal *jp_array(JParser *j) {
    PyVal *r = py_list_new();
    j->p++;
    jp_ws(j);
    if (*j->p == ']') { j->p++; return r; }
    while (1) {
        PyVal *v = jp_value(j);
        py_list_append(r, v);
        py_release(v);
        jp_ws(j);
        if (*j->p == ',') { j->p++; jp_ws(j); continue; }
        if (*j->p == ']') { j->p++; break; }
        py_raise(PY_EXC_VALUE, "invalid JSON array");
        break;
    }
    return r;
}

static PyVal *jp_object(JParser *j) {
    PyVal *r = py_dict_new();
    j->p++;
    jp_ws(j);
    if (*j->p == '}') { j->p++; return r; }
    while (1) {
        PyVal *k, *v;
        jp_ws(j);
        k = jp_string(j);
        jp_ws(j);
        if (*j->p != ':') { py_release(k); py_raise(PY_EXC_VALUE, "invalid JSON object"); return r; }
        j->p++;
        v = jp_value(j);
        {
            const char *s = py_cstr(k);
            py_dict_put(r, s, v);
        }
        py_release(v);
        py_release(k);
        jp_ws(j);
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == '}') { j->p++; break; }
        py_raise(PY_EXC_VALUE, "invalid JSON object");
        break;
    }
    return r;
}

static PyVal *jp_number(JParser *j) {
    const char *s = j->p;
    int isfloat = 0;
    if (*j->p == '-' || *j->p == '+') j->p++;
    while (isdigit((unsigned char)*j->p)) j->p++;
    if (*j->p == '.' || *j->p == 'e' || *j->p == 'E') {
        isfloat = 1;
        while (*j->p && (isdigit((unsigned char)*j->p) || *j->p == '.' ||
              *j->p == 'e' || *j->p == 'E' || *j->p == '+' || *j->p == '-')) j->p++;
    }
    if (j->p == s) { py_raise(PY_EXC_VALUE, "invalid JSON number"); return py_none(); }
    if (isfloat) return py_float(strtod(s, NULL));
    return py_int((int64_t)strtoll(s, NULL, 10));
}

static PyVal *jp_value(JParser *j) {
    jp_ws(j);
    if (*j->p == '"') return jp_string(j);
    if (*j->p == '[') return jp_array(j);
    if (*j->p == '{') return jp_object(j);
    if (!strncmp(j->p, "true", 4)) { j->p += 4; return py_bool(1); }
    if (!strncmp(j->p, "false", 5)) { j->p += 5; return py_bool(0); }
    if (!strncmp(j->p, "null", 4)) { j->p += 4; return py_none(); }
    return jp_number(j);
}

static int json_call(const char *name, PyVal **args, size_t argc, PyVal **out) {
    if (s_eq(name, "dumps")) {
        SBuf b; char *s; PyVal *r;
        sb_init(&b);
        json_write(&b, one_arg(args, argc, "dumps"));
        s = sb_take(&b);
        r = py_str(s);
        free(s);
        *out = r;
        return 1;
    }
    if (s_eq(name, "loads")) {
        JParser j;
        const char *s = sarg(args, argc, 0, "");
        j.p = s;
        *out = jp_value(&j);
        jp_ws(&j);
        if (*j.p) py_raise(PY_EXC_VALUE, "extra data in JSON input");
        return 1;
    }
    if (s_eq(name, "dump")) {
        SBuf b; char *s; PyVal *r;
        if (argc < 2) { py_raise(PY_EXC_VALUE, "dump() needs obj and file"); return 1; }
        sb_init(&b);
        json_write(&b, args[0]);
        s = sb_take(&b);
        r = py_str(s);
        free(s);
        py_file_write(args[1], r);
        py_release(r);
        *out = py_none();
        return 1;
    }
    if (s_eq(name, "load")) {
        PyVal *s = py_file_read(one_arg(args, argc, "load"), NULL, 0);
        JParser j;
        j.p = py_cstr(s);
        *out = jp_value(&j);
        py_release(s);
        return 1;
    }
    return 0;
}

/* -------------------------------------------------------------------- csv */

static int csv_call(const char *name, PyVal **args, size_t argc, PyVal **out) {
    if (s_eq(name, "reader")) {
        PyVal *rows = to_list(one_arg(args, argc, "reader"));
        PyVal *r = py_list_new(), *it = py_iter(rows), *row = NULL;
        while (py_iter_next(it, &row)) {
            if (row->type == PY_T_STR) {
                PyVal *cells = py_method(row, "split", NULL, 0, NULL);
                PyVal *line = to_list(cells);
                py_list_append(r, line);
                py_release(line);
                py_release(cells);
            } else {
                PyVal *line = to_list(row);
                py_list_append(r, line);
                py_release(line);
            }
        }
        py_release(row); py_release(it); py_release(rows);
        *out = r;
        return 1;
    }
    if (s_eq(name, "writer")) {
        PyVal *rows = to_list(one_arg(args, argc, "writer"));
        SBuf b; size_t i;
        sb_init(&b);
        for (i = 0; i < py_len(rows); i++) {
            PyVal *row = py_getitem(rows, py_int((int64_t)i));
            PyVal *cells = to_list(row);
            size_t j;
            if (i) sb_addc(&b, '\n');
            for (j = 0; j < py_len(cells); j++) {
                PyVal *c = py_getitem(cells, py_int((int64_t)j));
                PyVal *s = py_str_of(c);
                if (j) sb_addc(&b, ',');
                sb_adds(&b, py_cstr(s));
                py_release(s);
                py_release(c);
            }
            py_release(cells);
            py_release(row);
        }
        py_release(rows);
        {
            char *t = sb_take(&b);
            *out = py_str(t);
            free(t);
        }
        return 1;
    }
    if (s_eq(name, "DictReader")) {
        PyVal *rows = to_list(one_arg(args, argc, "DictReader"));
        PyVal *r = py_list_new(), *headers = NULL;
        size_t i;
        if (py_len(rows)) {
            headers = py_getitem(rows, py_int(0));
            if (headers->type == PY_T_STR) {
                PyVal *sp = py_method(headers, "split", NULL, 0, NULL);
                py_release(headers);
                headers = to_list(sp);
                py_release(sp);
            } else py_retain(headers);
        }
        for (i = 1; i < py_len(rows); i++) {
            PyVal *row = py_getitem(rows, py_int((int64_t)i));
            PyVal *cells;
            if (row->type == PY_T_STR) {
                PyVal *sp = py_method(row, "split", NULL, 0, NULL);
                cells = to_list(sp);
                py_release(sp);
            } else cells = to_list(row);
            PyVal *d = py_dict_new();
            size_t j, n = py_len(headers) < py_len(cells) ? py_len(headers) : py_len(cells);
            for (j = 0; j < n; j++) {
                PyVal *k = py_getitem(headers, py_int((int64_t)j));
                PyVal *ks = py_str_of(k);
                PyVal *v = py_getitem(cells, py_int((int64_t)j));
                py_dict_put(d, py_cstr(ks), v);
                py_release(v); py_release(ks); py_release(k);
            }
            py_list_append(r, d);
            py_release(d); py_release(cells); py_release(row);
        }
        py_release(headers); py_release(rows);
        *out = r;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------- small regex engine */

typedef struct {
    int kind;             /* 0 literal, 1 any, 2 class, 3 digit, 4 word, 5 space, 6 end */
    unsigned char ch;
    int neg;
    unsigned char bits[32];
    const char *next;
} RAtom;

static void rbit(unsigned char *bits, unsigned char c) { bits[c >> 3] |= (unsigned char)(1u << (c & 7)); }
static int rhas(const unsigned char *bits, unsigned char c) { return (bits[c >> 3] >> (c & 7)) & 1; }

static RAtom r_atom(const char *p) {
    RAtom a;
    memset(&a, 0, sizeof(a));
    a.next = p + 1;
    if (!*p) { a.kind = 6; a.next = p; return a; }
    if (*p == '.') { a.kind = 1; return a; }
    if (*p == '$' && !p[1]) { a.kind = 6; return a; }
    if (*p == '\\' && p[1]) {
        char e = p[1];
        a.next = p + 2;
        if (e == 'd') a.kind = 3;
        else if (e == 'D') { a.kind = 3; a.neg = 1; }
        else if (e == 'w') a.kind = 4;
        else if (e == 'W') { a.kind = 4; a.neg = 1; }
        else if (e == 's') a.kind = 5;
        else if (e == 'S') { a.kind = 5; a.neg = 1; }
        else { a.kind = 0; a.ch = (unsigned char)e; }
        return a;
    }
    if (*p == '[') {
        const char *q = p + 1;
        a.kind = 2;
        if (*q == '^') { a.neg = 1; q++; }
        while (*q && *q != ']') {
            if (q[1] == '-' && q[2] && q[2] != ']') {
                unsigned char lo = (unsigned char)q[0], hi = (unsigned char)q[2], c;
                for (c = lo; c <= hi; c++) rbit(a.bits, c);
                q += 3;
            } else {
                rbit(a.bits, (unsigned char)q[0]);
                q++;
            }
        }
        a.next = *q ? q + 1 : q;
        return a;
    }
    a.kind = 0;
    a.ch = (unsigned char)*p;
    return a;
}

static int r_atom_match(const RAtom *a, unsigned char c) {
    int r = 0;
    switch (a->kind) {
    case 0: r = c == a->ch; break;
    case 1: r = c != '\n'; break;
    case 2: r = rhas(a->bits, c); break;
    case 3: r = isdigit(c) != 0; break;
    case 4: r = isalnum(c) != 0 || c == '_'; break;
    case 5: r = isspace(c) != 0; break;
    case 6: r = 0; break;
    default: break;
    }
    return a->neg ? !r : r;
}

/* A successful match returns the position just after the matched text.
   Returning the end pointer makes greediness and match spans unambiguous. */
static const char *r_seq(const char *p, const char *s, const char *end);

static const char *r_seq_atom(const char *p, const char *s, const char *end) {
    RAtom a = r_atom(p);
    const char *rest = a.next;
    char quant = 0;
    if (*rest == '*' || *rest == '+' || *rest == '?') { quant = *rest; rest++; }
    if (a.kind == 6) return r_seq(rest, s, end); /* '$' consumes nothing */
    if (!quant) {
        if (s < end && r_atom_match(&a, (unsigned char)*s)) return r_seq(rest, s + 1, end);
        return NULL;
    }
    if (quant == '?') {
        const char *r = r_seq(rest, s, end);
        if (r) return r;
        if (s < end && r_atom_match(&a, (unsigned char)*s)) return r_seq(rest, s + 1, end);
        return NULL;
    }
    {
        const char *pos[512];
        size_t n = 0, k;
        const char *cur = s;
        while (cur < end && r_atom_match(&a, (unsigned char)*cur) && n < 512) {
            pos[n++] = cur;
            cur++;
        }
        if (quant == '+' && n == 0) return NULL;
        for (k = n; ; k--) { /* greedy: longest repetition that lets the rest match */
            const char *next = k ? pos[k - 1] + 1 : s;
            const char *r = r_seq(rest, next, end);
            if (r) return r;
            if (k == 0) break;
        }
        return NULL;
    }
}

static const char *r_seq(const char *p, const char *s, const char *end) {
    if (!*p || *p == ')') return s;
    return r_seq_atom(p, s, end);
}

static int r_search(const char *pat, const char *start, const char *end,
                    const char **ms, const char **me) {
    const char *p = pat;
    int anchored = 0;
    if (*p == '^') { anchored = 1; p++; }
    {
        const char *s = start;
        while (1) {
            const char *stop = r_seq(p, s, end);
            if (stop) { *ms = s; *me = stop; return 1; }
            if (anchored || s == end) break;
            s++;
        }
    }
    return 0;
}

static int re_call(const char *name, PyVal **args, size_t argc, PyVal **out) {
    const char *pat = sarg(args, argc, 0, "");
    if (s_eq(name, "escape")) {
        SBuf b; const char *p;
        sb_init(&b);
        for (p = pat; *p; p++) {
            if (strchr("\\.^$*+?{}[]|()", *p)) sb_addc(&b, '\\');
            sb_addc(&b, *p);
        }
        {
            char *s = sb_take(&b);
            *out = py_str(s);
            free(s);
        }
        return 1;
    }
    if (s_eq(name, "match") || s_eq(name, "search") || s_eq(name, "fullmatch") ||
        s_eq(name, "findall") || s_eq(name, "sub") || s_eq(name, "split")) {
        PyVal *textv = argc > 1 ? args[1] : py_none();
        const char *text = py_cstr(textv), *end = text + py_cstr_len(textv);
        const char *s = text;
        if (s_eq(name, "sub")) {
            SBuf b;
            const char *repl = sarg(args, argc, 2, "");
            sb_init(&b);
            while (s <= end) {
                const char *ms, *me;
                if (r_search(pat, s, end, &ms, &me)) {
                    sb_add(&b, s, (size_t)(ms - s));
                    sb_adds(&b, repl);
                    s = me > ms ? me : ms + 1;
                } else {
                    if (s < end) sb_addc(&b, *s);
                    s++;
                }
            }
            {
                char *t = sb_take(&b);
                *out = py_str(t);
                free(t);
            }
            return 1;
        }
        if (s_eq(name, "split")) {
            PyVal *r = py_list_new();
            const char *last = text;
            while (s <= end) {
                const char *ms, *me;
                if (r_search(pat, s, end, &ms, &me)) {
                    PyVal *v = py_strn(last, (size_t)(ms - last));
                    py_list_append(r, v);
                    py_release(v);
                    last = me > ms ? me : ms + 1;
                    s = last;
                } else s++;
            }
            {
                PyVal *v = py_strn(last, (size_t)(end - last));
                py_list_append(r, v);
                py_release(v);
            }
            *out = r;
            return 1;
        }
        if (s_eq(name, "findall")) {
            PyVal *r = py_list_new();
            while (s <= end) {
                const char *ms, *me;
                if (r_search(pat, s, end, &ms, &me)) {
                    PyVal *v = py_strn(ms, (size_t)(me - ms));
                    py_list_append(r, v);
                    py_release(v);
                    s = me > ms ? me : ms + 1;
                } else {
                    if (s == end) break;
                    s++;
                }
            }
            *out = r;
            return 1;
        }
        {
            const char *ms, *me;
            int ok;
            if (s_eq(name, "match")) {
                const char *p = (*pat == '^') ? pat + 1 : pat;
                const char *stop = r_seq(p, text, end);
                ok = stop != NULL;
                ms = text; me = stop ? stop : text;
            } else if (s_eq(name, "fullmatch")) {
                ok = r_search(pat, text, end, &ms, &me) && me == end;
            } else {
                ok = r_search(pat, text, end, &ms, &me);
            }
            *out = py_bool(ok);
            return 1;
        }
    }
    if (s_eq(name, "compile")) { *out = py_str(pat); return 1; }
    return 0;
}

/* --------------------------------------------------------------- textwrap */

static int textwrap_call(const char *name, PyVal **args, size_t argc,
                         PyVal *kwargs, PyVal **out) {
    if (s_eq(name, "dedent")) {
        const char *s = sarg(args, argc, 0, "");
        PyVal *lines = to_list(py_method(py_str(s), "splitlines", NULL, 0, NULL));
        size_t i, margin = (size_t)-1;
        SBuf b;
        for (i = 0; i < py_len(lines); i++) {
            PyVal *ln = py_getitem(lines, py_int((int64_t)i));
            const char *c = py_cstr(ln);
            size_t k = 0;
            while (c[k] == ' ' || c[k] == '\t') k++;
            if (c[k]) { if (k < margin) margin = k; }
            py_release(ln);
        }
        sb_init(&b);
        for (i = 0; i < py_len(lines); i++) {
            PyVal *ln = py_getitem(lines, py_int((int64_t)i));
            const char *c = py_cstr(ln);
            size_t k = 0;
            while (c[k] == ' ' || c[k] == '\t') k++;
            if (margin != (size_t)-1 && k > margin) k = margin;
            if (i) sb_addc(&b, '\n');
            sb_adds(&b, c + k);
            py_release(ln);
        }
        py_release(lines);
        {
            char *t = sb_take(&b);
            *out = py_str(t);
            free(t);
        }
        return 1;
    }
    if (s_eq(name, "indent")) {
        const char *s = sarg(args, argc, 0, ""), *pre = sarg(args, argc, 1, " ");
        PyVal *lines = to_list(py_method(py_str(s), "splitlines", NULL, 0, NULL));
        SBuf b; size_t i;
        sb_init(&b);
        for (i = 0; i < py_len(lines); i++) {
            PyVal *ln = py_getitem(lines, py_int((int64_t)i));
            const char *c = py_cstr(ln);
            if (*c) sb_adds(&b, pre);
            sb_adds(&b, c);
            if (i + 1 < py_len(lines)) sb_addc(&b, '\n');
            py_release(ln);
        }
        py_release(lines);
        {
            char *t = sb_take(&b);
            *out = py_str(t);
            free(t);
        }
        return 1;
    }
    if (s_eq(name, "wrap") || s_eq(name, "fill")) {
        const char *s = sarg(args, argc, 0, "");
        PyVal *kw = py_kw_get(kwargs, "width"); /* borrowed */
        int64_t width = kw ? py_as_int(kw) : iarg(args, argc, 1, 70);
        PyVal *words = to_list(py_method(py_str(s), "split", NULL, 0, NULL));
        SBuf b; size_t i, col = 0;
        if (width < 1) width = 70;
        sb_init(&b);
        for (i = 0; i < py_len(words); i++) {
            PyVal *w = py_getitem(words, py_int((int64_t)i));
            const char *c = py_cstr(w);
            size_t n = strlen(c);
            if (col && col + n + 1 > (size_t)width) {
                sb_addc(&b, s_eq(name, "fill") ? '\n' : '|');
                col = 0;
            } else if (col) { sb_addc(&b, ' '); col++; }
            sb_adds(&b, c);
            col += n;
            py_release(w);
        }
        py_release(words);
        {
            char *t = sb_take(&b);
            *out = py_str(t);
            free(t);
        }
        return 1;
    }
    if (s_eq(name, "shorten")) {
        *out = retain_same(one_arg(args, argc, "shorten"));
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------- html */

static int html_call(const char *name, PyVal **args, size_t argc, PyVal **out) {
    const char *s = sarg(args, argc, 0, "");
    if (s_eq(name, "escape")) {
        SBuf b; const char *p;
        sb_init(&b);
        for (p = s; *p; p++) {
            switch (*p) {
            case '&': sb_adds(&b, "&amp;"); break;
            case '<': sb_adds(&b, "&lt;"); break;
            case '>': sb_adds(&b, "&gt;"); break;
            case '"': sb_adds(&b, "&quot;"); break;
            case '\'': sb_adds(&b, "&#x27;"); break;
            default: sb_addc(&b, *p); break;
            }
        }
        {
            char *t = sb_take(&b);
            *out = py_str(t);
            free(t);
        }
        return 1;
    }
    if (s_eq(name, "unescape")) {
        SBuf b; const char *p;
        sb_init(&b);
        for (p = s; *p;) {
            if (*p == '&') {
                const char *semi = strchr(p, ';');
                if (semi && semi - p < 16) {
                    char ent[16];
                    size_t n = (size_t)(semi - p + 1);
                    memcpy(ent, p, n);
                    ent[n] = 0;
                    if (!strcmp(ent, "&amp;")) sb_addc(&b, '&');
                    else if (!strcmp(ent, "&lt;")) sb_addc(&b, '<');
                    else if (!strcmp(ent, "&gt;")) sb_addc(&b, '>');
                    else if (!strcmp(ent, "&quot;")) sb_addc(&b, '"');
                    else if (!strcmp(ent, "&apos;") || !strcmp(ent, "&#x27;")) sb_addc(&b, '\'');
                    else if (!strcmp(ent, "&nbsp;")) sb_adds(&b, "\xC2\xA0");
                    else sb_add(&b, p, n);
                    p = semi + 1;
                    continue;
                }
            }
            sb_addc(&b, *p++);
        }
        {
            char *t = sb_take(&b);
            *out = py_str(t);
            free(t);
        }
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------ base64/binascii/zlib */

static const char *B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_call(const char *name, PyVal **args, size_t argc, PyVal **out) {
    PyVal *v = one_arg(args, argc, name);
    const char *s = py_cstr(v);
    size_t n = py_cstr_len(v), i;
    if (s_eq(name, "b64encode") || s_eq(name, "encodebytes") ||
        s_eq(name, "b2a_base64")) {
        SBuf b;
        sb_init(&b);
        for (i = 0; i < n; i += 3) {
            unsigned char a = (unsigned char)s[i];
            unsigned char c = i + 1 < n ? (unsigned char)s[i + 1] : 0;
            unsigned char d = i + 2 < n ? (unsigned char)s[i + 2] : 0;
            sb_addc(&b, B64[a >> 2]);
            sb_addc(&b, B64[((a & 3) << 4) | (c >> 4)]);
            sb_addc(&b, i + 1 < n ? B64[((c & 15) << 2) | (d >> 6)] : '=');
            sb_addc(&b, i + 2 < n ? B64[d & 63] : '=');
        }
        if (s_eq(name, "b2a_base64")) sb_addc(&b, '\n');
        {
            char *t = sb_take(&b);
            *out = py_str(t);
            free(t);
        }
        return 1;
    }
    if (s_eq(name, "b64decode") || s_eq(name, "decodebytes") ||
        s_eq(name, "a2b_base64")) {
        int rev[256];
        SBuf b;
        size_t outlen;
        unsigned accum = 0;
        int bits = 0;
        memset(rev, -1, sizeof(rev));
        for (i = 0; i < 64; i++) rev[(unsigned char)B64[i]] = (int)i;
        sb_init(&b);
        for (i = 0; i < n; i++) {
            unsigned char c = (unsigned char)s[i];
            int val;
            if (isspace(c)) continue;
            if (c == '=') break;
            val = rev[c];
            if (val < 0) { py_raise(PY_EXC_VALUE, "invalid base64 data"); return 1; }
            accum = (accum << 6) | (unsigned)val;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                sb_addc(&b, (char)((accum >> bits) & 0xFF));
            }
        }
        outlen = b.len;
        {
            char *t = sb_take(&b);
            *out = py_strn(t, outlen);
            free(t);
        }
        return 1;
    }
    return 0;
}

static uint32_t crc32_bytes(const char *s, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    size_t i, k;
    for (i = 0; i < n; i++) {
        crc ^= (unsigned char)s[i];
        for (k = 0; k < 8; k++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
    }
    return ~crc;
}

static int binascii_call(const char *name, PyVal **args, size_t argc, PyVal **out) {
    PyVal *v = one_arg(args, argc, name);
    const char *s = py_cstr(v);
    size_t n = py_cstr_len(v), i;
    if (s_eq(name, "hexlify") || s_eq(name, "b2a_hex")) {
        SBuf b;
        sb_init(&b);
        for (i = 0; i < n; i++) sb_printf(&b, "%02x", (unsigned char)s[i]);
        {
            char *t = sb_take(&b);
            *out = py_str(t);
            free(t);
        }
        return 1;
    }
    if (s_eq(name, "unhexlify") || s_eq(name, "a2b_hex")) {
        SBuf b;
        sb_init(&b);
        if (n % 2) { py_raise(PY_EXC_VALUE, "odd-length hex string"); return 1; }
        for (i = 0; i < n; i += 2) {
            char h[3];
            unsigned int x;
            h[0] = s[i]; h[1] = s[i + 1]; h[2] = 0;
            if (sscanf(h, "%2x", &x) != 1) { py_raise(PY_EXC_VALUE, "non-hex digit"); return 1; }
            sb_addc(&b, (char)x);
        }
        {
            char *t = sb_take(&b);
            *out = py_strn(t, n / 2);
            free(t);
        }
        return 1;
    }
    if (s_eq(name, "crc32")) { *out = py_int((int64_t)crc32_bytes(s, n)); return 1; }
    if (s_eq(name, "b2a_base64") || s_eq(name, "a2b_base64") ||
        s_eq(name, "b64encode") || s_eq(name, "b64decode"))
        return b64_call(name, args, argc, out);
    return 0;
}

static uint32_t adler32_bytes(const char *s, size_t n) {
    uint32_t a = 1, b = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        a = (a + (unsigned char)s[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

static int zlib_call(const char *name, PyVal **args, size_t argc, PyVal **out) {
    PyVal *v = one_arg(args, argc, name);
    const char *s = py_cstr(v);
    size_t n = py_cstr_len(v);
    if (s_eq(name, "crc32")) { *out = py_int((int64_t)crc32_bytes(s, n)); return 1; }
    if (s_eq(name, "adler32")) { *out = py_int((int64_t)adler32_bytes(s, n)); return 1; }
    if (s_eq(name, "compress")) {
        SBuf b; size_t pos = 0, outlen = 6;
        uint32_t ad = adler32_bytes(s, n);
        sb_init(&b);
        sb_addc(&b, (char)0x78);
        sb_addc(&b, (char)0x01);
        do {
            size_t chunk = n - pos;
            uint32_t inv;
            if (chunk > 65535) chunk = 65535;
            sb_addc(&b, (char)(pos + chunk == n ? 1 : 0));
            sb_addc(&b, (char)(chunk & 0xFF));
            sb_addc(&b, (char)((chunk >> 8) & 0xFF));
            inv = (uint32_t)chunk ^ 0xFFFFu;
            sb_addc(&b, (char)(inv & 0xFF));
            sb_addc(&b, (char)((inv >> 8) & 0xFF));
            sb_add(&b, s + pos, chunk);
            pos += chunk;
            outlen += 5 + chunk;
        } while (pos < n);
        sb_addc(&b, (char)((ad >> 24) & 0xFF));
        sb_addc(&b, (char)((ad >> 16) & 0xFF));
        sb_addc(&b, (char)((ad >> 8) & 0xFF));
        sb_addc(&b, (char)(ad & 0xFF));
        {
            char *t = sb_take(&b);
            *out = py_strn(t, outlen);
            free(t);
        }
        return 1;
    }
    if (s_eq(name, "decompress")) {
        SBuf b;
        size_t pos = 2, outlen;
        sb_init(&b);
        if (n < 6) { py_raise(PY_EXC_VALUE, "invalid zlib stream"); return 1; }
        while (pos < n - 4) {
            unsigned char h = (unsigned char)s[pos++];
            size_t len;
            if ((h & 6) != 0) { py_raise(PY_EXC_VALUE, "unsupported zlib block"); return 1; }
            if (pos + 4 > n) { py_raise(PY_EXC_VALUE, "invalid zlib stream"); return 1; }
            len = (unsigned char)s[pos] | ((unsigned char)s[pos + 1] << 8);
            pos += 4;
            if (pos + len > n) { py_raise(PY_EXC_VALUE, "invalid zlib stream"); return 1; }
            sb_add(&b, s + pos, len);
            pos += len;
            if (h & 1) break;
        }
        outlen = b.len;
        {
            char *t = sb_take(&b);
            *out = py_strn(t, outlen);
            free(t);
        }
        return 1;
    }
    return 0;
}

/* ----------------------------------------------------------------- struct */

static int struct_call(const char *name, PyVal **args, size_t argc, PyVal **out) {
    const char *fmt;
    size_t i, pos = 0;
    int big = 0;
    if (!s_eq(name, "pack") && !s_eq(name, "unpack") && !s_eq(name, "calcsize")) return 0;
    fmt = sarg(args, argc, 0, "");
    for (i = 0; fmt[i]; i++) {
        if (fmt[i] == '<') { big = 0; continue; }
        if (fmt[i] == '>' || fmt[i] == '!') { big = 1; continue; }
        if (fmt[i] == '=' || fmt[i] == '@' || fmt[i] == ' ' || fmt[i] == '|') continue;
        if (isdigit((unsigned char)fmt[i])) { pos += (size_t)(fmt[i] - '0'); continue; }
        if (fmt[i] == 'b' || fmt[i] == 'B') pos += 1;
        else if (fmt[i] == 'h' || fmt[i] == 'H') pos += 2;
        else if (fmt[i] == 'i' || fmt[i] == 'I' || fmt[i] == 'f' || fmt[i] == 'l' || fmt[i] == 'L') pos += 4;
        else if (fmt[i] == 'q' || fmt[i] == 'Q' || fmt[i] == 'd') pos += 8;
        else { py_raise(PY_EXC_VALUE, "unsupported struct format"); return 1; }
    }
    if (s_eq(name, "calcsize")) { *out = py_int((int64_t)pos); return 1; }
    if (s_eq(name, "pack")) {
        char *buf = (char *)calloc(pos ? pos : 1, 1);
        size_t off = 0, ai = 1;
        for (i = 0; fmt[i]; i++) {
            int64_t v;
            uint32_t u32;
            uint64_t u64;
            unsigned char c;
            char ch = fmt[i];
            if (ch == '<' || ch == '>' || ch == '!' || ch == '=' || ch == '@' ||
                ch == ' ' || ch == '|' || isdigit((unsigned char)ch)) continue;
            if (ai >= argc) { py_raise(PY_EXC_VALUE, "not enough arguments for format"); break; }
            v = py_as_int(args[ai++]);
            switch (ch) {
            case 'b': case 'B': buf[off++] = (unsigned char)v; break;
            case 'h': case 'H':
                c = (unsigned char)(v & 0xFF);
                if (big) { buf[off++] = (unsigned char)((v >> 8) & 0xFF); buf[off++] = c; }
                else { buf[off++] = c; buf[off++] = (unsigned char)((v >> 8) & 0xFF); }
                break;
            case 'f': {
                float f = (float)py_as_float(args[ai - 1]);
                u32 = 0; memcpy(&u32, &f, 4);
                if (big) { buf[off++] = (char)((u32 >> 24) & 255); buf[off++] = (char)((u32 >> 16) & 255);
                           buf[off++] = (char)((u32 >> 8) & 255); buf[off++] = (char)(u32 & 255); }
                else { memcpy(buf + off, &u32, 4); off += 4; }
                break;
            }
            case 'd':
                {
                    double dv = py_as_float(args[ai - 1]);
                    u64 = 0; memcpy(&u64, &dv, 8);
                }
                if (big) { int k; for (k = 7; k >= 0; k--) buf[off++] = (char)((u64 >> (k * 8)) & 255); }
                else { memcpy(buf + off, &u64, 8); off += 8; }
                break;
            case 'q': case 'Q':
                u64 = (uint64_t)v;
                if (big) { int k; for (k = 7; k >= 0; k--) buf[off++] = (char)((u64 >> (k * 8)) & 255); }
                else { memcpy(buf + off, &u64, 8); off += 8; }
                break;
            default:
                u32 = (uint32_t)v;
                if (big) { buf[off++] = (char)((u32 >> 24) & 255); buf[off++] = (char)((u32 >> 16) & 255);
                           buf[off++] = (char)((u32 >> 8) & 255); buf[off++] = (char)(u32 & 255); }
                else { memcpy(buf + off, &u32, 4); off += 4; }
                break;
            }
        }
        *out = py_strn(buf, off);
        free(buf);
        return 1;
    }
    {
        PyVal *data = one_arg(args, argc, "unpack");
        const char *b = py_cstr(data);
        size_t off = 0;
        PyVal **vals = (PyVal **)calloc(pos ? pos : 1, sizeof(PyVal *));
        size_t nv = 0;
        for (i = 0; fmt[i] && off < py_cstr_len(data); i++) {
            char ch = fmt[i];
            int64_t v = 0;
            if (ch == '<' || ch == '>' || ch == '!' || ch == '=' || ch == '@' ||
                ch == ' ' || ch == '|' || isdigit((unsigned char)ch)) continue;
            switch (ch) {
            case 'b': v = (int64_t)(signed char)b[off]; off += 1; break;
            case 'B': v = (int64_t)(unsigned char)b[off]; off += 1; break;
            case 'h': case 'H': {
                unsigned x = (unsigned char)b[off] | ((unsigned)(unsigned char)b[off + 1] << 8);
                if (big) x = ((unsigned char)b[off] << 8) | (unsigned char)b[off + 1];
                v = ch == 'h' ? (int64_t)(int16_t)x : (int64_t)x;
                off += 2; break;
            }
            case 'f': {
                uint32_t x = 0; float f;
                if (big) x = ((uint32_t)(unsigned char)b[off] << 24) |
                             ((uint32_t)(unsigned char)b[off + 1] << 16) |
                             ((uint32_t)(unsigned char)b[off + 2] << 8) |
                             (uint32_t)(unsigned char)b[off + 3];
                else memcpy(&x, b + off, 4);
                memcpy(&f, &x, 4);
                vals[nv++] = py_float(f);
                off += 4;
                continue;
            }
            case 'd': {
                uint64_t x = 0; double d; int k;
                if (big) for (k = 0; k < 8; k++) x = (x << 8) | (unsigned char)b[off + k];
                else memcpy(&x, b + off, 8);
                memcpy(&d, &x, 8);
                vals[nv++] = py_float(d);
                off += 8;
                continue;
            }
            case 'q': case 'Q': {
                uint64_t x = 0; int k;
                if (big) for (k = 0; k < 8; k++) x = (x << 8) | (unsigned char)b[off + k];
                else memcpy(&x, b + off, 8);
                v = ch == 'q' ? (int64_t)x : (int64_t)x;
                off += 8; break;
            }
            default: {
                uint32_t x;
                if (big) x = ((uint32_t)(unsigned char)b[off] << 24) |
                             ((uint32_t)(unsigned char)b[off + 1] << 16) |
                             ((uint32_t)(unsigned char)b[off + 2] << 8) |
                             (uint32_t)(unsigned char)b[off + 3];
                else memcpy(&x, b + off, 4);
                v = ch == 'i' ? (int64_t)(int32_t)x : (int64_t)x;
                off += 4; break;
            }
            }
            vals[nv++] = py_int(v);
        }
        *out = py_tuple_from(vals, nv);
        for (i = 0; i < nv; i++) py_release(vals[i]);
        free(vals);
        return 1;
    }
}

/* ---------------------------------------------------- codecs/unicodedata */

static int codecs_call(const char *name, PyVal **args, size_t argc, PyVal **out) {
    if (s_eq(name, "encode") || s_eq(name, "decode") || s_eq(name, "utf_8_encode") ||
        s_eq(name, "utf_8_decode")) {
        *out = py_str_of(one_arg(args, argc, name));
        return 1;
    }
    if (s_eq(name, "open")) { *out = py_builtin_open(args, argc, NULL); return 1; }
    if (s_eq(name, "lookup")) { *out = py_str("utf-8"); return 1; }
    return 0;
}

static int unicode_call(const char *name, PyVal **args, size_t argc, PyVal **out) {
    const char *s = sarg(args, argc, 0, "");
    unsigned char c = (unsigned char)(*s ? *s : 0);
    if (s_eq(name, "normalize")) { *out = py_str(s); return 1; }
    if (s_eq(name, "category")) {
        const char *cat = isupper(c) ? "Lu" : islower(c) ? "Ll" :
                          isdigit(c) ? "Nd" : isspace(c) ? "Zs" : "Po";
        *out = py_str(cat);
        return 1;
    }
    if (s_eq(name, "name")) {
        char buf[32];
        snprintf(buf, sizeof(buf), "U+%04X", (unsigned)c);
        *out = py_str(buf);
        return 1;
    }
    if (s_eq(name, "digit") || s_eq(name, "decimal")) {
        if (!isdigit(c)) { py_raise(PY_EXC_VALUE, "not a decimal character"); return 1; }
        *out = py_int(c - '0');
        return 1;
    }
    if (s_eq(name, "numeric")) { *out = py_float(isdigit(c) ? c - '0' : 0); return 1; }
    if (s_eq(name, "lookup")) { *out = py_str(*s ? s : "?"); return 1; }
    if (s_eq(name, "combining")) { *out = py_int(0); return 1; }
    return 0;
}

/* ------------------------------------------------------ pathlib/fnmatch/glob */

static int fn_pattern(const char *p, const char *s) {
    while (*p) {
        if (*p == '*') {
            while (*p == '*') p++;
            if (!*p) return 1;
            while (*s) {
                if (fn_pattern(p, s)) return 1;
                s++;
            }
            return fn_pattern(p, s);
        }
        if (*p == '?') {
            if (!*s) return 0;
            p++; s++;
            continue;
        }
        if (*p == '[') {
            const char *q = p + 1;
            int neg = 0, hit = 0;
            if (*q == '!') { neg = 1; q++; }
            while (*q && *q != ']') {
                if (q[1] == '-' && q[2] && q[2] != ']') {
                    if (*s >= q[0] && *s <= q[2]) hit = 1;
                    q += 3;
                } else {
                    if (*s == *q) hit = 1;
                    q++;
                }
            }
            if (*q == ']') q++;
            if (!*s || hit == neg) return 0;
            p = q; s++;
            continue;
        }
        if (*p != *s) return 0;
        p++; s++;
    }
    return !*s;
}

static int fnmatch_call(const char *name, PyVal **args, size_t argc, PyVal **out) {
    if (s_eq(name, "fnmatch") || s_eq(name, "fnmatchcase")) {
        PyVal *v = one_arg(args, argc, name);
        *out = py_bool(fn_pattern(sarg(args, argc, 1, ""), py_cstr(v)));
        return 1;
    }
    if (s_eq(name, "filter")) {
        PyVal *names = to_list(one_arg(args, argc, "filter"));
        PyVal *r = py_list_new(), *it = py_iter(names), *item = NULL;
        const char *pat = sarg(args, argc, 1, "");
        while (py_iter_next(it, &item)) {
            if (fn_pattern(pat, py_cstr(item))) py_list_append(r, item);
        }
        py_release(item); py_release(it); py_release(names);
        *out = r;
        return 1;
    }
    if (s_eq(name, "translate")) { *out = py_str(sarg(args, argc, 0, "")); return 1; }
    return 0;
}

static int pathlib_call(const char *name, PyVal **args, size_t argc, PyVal **out) {
    if (s_eq(name, "Path") || s_eq(name, "PurePath") || s_eq(name, "PureWindowsPath") ||
        s_eq(name, "WindowsPath") || s_eq(name, "PurePosixPath") || s_eq(name, "PosixPath")) {
        *out = py_str(sarg(args, argc, 0, "."));
        return 1;
    }
    if (s_eq(name, "exists") || s_eq(name, "is_file") || s_eq(name, "is_dir")) {
        struct stat st;
        int ok = stat(sarg(args, argc, 0, ""), &st) == 0;
        if (ok && s_eq(name, "is_file")) ok = (st.st_mode & S_IFMT) == S_IFREG;
        if (ok && s_eq(name, "is_dir")) ok = (st.st_mode & S_IFMT) == S_IFDIR;
        *out = py_bool(ok);
        return 1;
    }
    if (s_eq(name, "name") || s_eq(name, "parent") || s_eq(name, "suffix") ||
        s_eq(name, "stem")) {
        const char *p = sarg(args, argc, 0, "");
        const char *slash = strrchr(p, '\\'), *slash2 = strrchr(p, '/');
        const char *base;
        if (slash2 && (!slash || slash2 > slash)) slash = slash2;
        base = slash ? slash + 1 : p;
        if (s_eq(name, "name")) *out = py_str(base);
        else if (s_eq(name, "parent")) {
            *out = slash ? py_strn(p, (size_t)(slash - p)) : py_str(".");
        } else {
            const char *dot = strrchr(base, '.');
            if (s_eq(name, "suffix")) *out = dot && dot != base ? py_str(dot) : py_str("");
            else *out = dot && dot != base ? py_strn(base, (size_t)(dot - base)) : py_str(base);
        }
        return 1;
    }
    if (s_eq(name, "join") || s_eq(name, "joinpath")) {
        SBuf b; size_t i;
        sb_init(&b);
        for (i = 0; i < argc; i++) {
            const char *p = py_cstr(args[i]);
            if (b.len && b.data[b.len - 1] != '\\' && b.data[b.len - 1] != '/' &&
                *p != '\\' && *p != '/') sb_addc(&b, '\\');
            sb_adds(&b, p);
        }
        {
            char *t = sb_take(&b);
            *out = py_str(t);
            free(t);
        }
        return 1;
    }
    if (s_eq(name, "absolute") || s_eq(name, "resolve")) {
        const char *p = sarg(args, argc, 0, ".");
#ifdef _WIN32
        char buf[4096];
        if (_fullpath(buf, p, sizeof(buf))) *out = py_str(buf);
        else *out = py_str(p);
#else
        char buf[4096];
        if (realpath(p, buf)) *out = py_str(buf);
        else *out = py_str(p);
#endif
        return 1;
    }
    if (s_eq(name, "read_text")) {
        PyVal *f = py_open_file(sarg(args, argc, 0, ""), "r");
        *out = py_file_read(f, NULL, 0);
        py_release(f);
        return 1;
    }
    if (s_eq(name, "write_text")) {
        PyVal *f = py_open_file(sarg(args, argc, 0, ""), "w");
        py_file_write(f, one_arg(args, argc, "write_text"));
        py_release(f);
        *out = py_int(0);
        return 1;
    }
    if (s_eq(name, "mkdir")) {
#ifdef _WIN32
        _mkdir(sarg(args, argc, 0, ""));
#else
        mkdir(sarg(args, argc, 0, ""), 0777);
#endif
        *out = py_none();
        return 1;
    }
    if (s_eq(name, "rmdir") || s_eq(name, "unlink")) {
        remove(sarg(args, argc, 0, ""));
        *out = py_none();
        return 1;
    }
    return 0;
}

static int glob_call(const char *name, PyVal **args, size_t argc, PyVal **out) {
    if (!s_eq(name, "glob") && !s_eq(name, "iglob")) return 0;
    {
        const char *pat = sarg(args, argc, 0, "*");
        const char *slash = strrchr(pat, '\\'), *slash2 = strrchr(pat, '/');
        const char *dir = ".", *mask = pat;
        PyVal *r = py_list_new();
        if (slash2 && (!slash || slash2 > slash)) slash = slash2;
        if (slash) {
            size_t n = (size_t)(slash - pat);
            char *d = (char *)malloc(n + 1);
            memcpy(d, pat, n);
            d[n] = 0;
            dir = d;
            mask = slash + 1;
        }
#ifdef _WIN32
        {
            char findpat[4096];
            WIN32_FIND_DATAA fd;
            HANDLE h;
            snprintf(findpat, sizeof(findpat), "%s\\*", dir);
            h = FindFirstFileA(findpat, &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if (fn_pattern(mask, fd.cFileName)) {
                        SBuf b; char *t;
                        sb_init(&b);
                        sb_adds(&b, dir);
                        if (strcmp(dir, ".")) sb_addc(&b, '\\');
                        sb_adds(&b, fd.cFileName);
                        t = sb_take(&b);
                        {
                            PyVal *v = py_str(t);
                            py_list_append(r, v);
                            py_release(v);
                        }
                        free(t);
                    }
                } while (FindNextFileA(h, &fd));
                FindClose(h);
            }
        }
#else
        {
            DIR *d = opendir(dir);
            if (d) {
                struct dirent *e;
                while ((e = readdir(d))) {
                    if (fn_pattern(mask, e->d_name)) {
                        SBuf b; char *t;
                        sb_init(&b);
                        sb_adds(&b, dir);
                        sb_addc(&b, '/');
                        sb_adds(&b, e->d_name);
                        t = sb_take(&b);
                        {
                            PyVal *v = py_str(t);
                            py_list_append(r, v);
                            py_release(v);
                        }
                        free(t);
                    }
                }
                closedir(d);
            }
        }
#endif
        if (dir != pat && dir != ".") free((void *)dir);
        *out = r;
        return 1;
    }
}

/* ------------------------------------------------------ shutil/subprocess/.. */

static int shutil_call(const char *name, PyVal **args, size_t argc, PyVal **out) {
    if (s_eq(name, "copyfile") || s_eq(name, "copy") || s_eq(name, "copy2")) {
        const char *src = sarg(args, argc, 0, ""), *dst = sarg(args, argc, 1, "");
        FILE *in = fopen(src, "rb"), *fo = fopen(dst, "wb");
        char buf[8192];
        size_t n;
        if (!in) { if (fo) fclose(fo); py_raise(PY_EXC_FILE, "source not found"); return 1; }
        if (!fo) { fclose(in); py_raise(PY_EXC_FILE, "cannot write destination"); return 1; }
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, fo);
        fclose(in); fclose(fo);
        *out = py_str(dst);
        return 1;
    }
    if (s_eq(name, "move")) {
        const char *src = sarg(args, argc, 0, ""), *dst = sarg(args, argc, 1, "");
        if (rename(src, dst) != 0) {
            SBuf b; sb_init(&b);
            sb_printf(&b, "copy /Y \"%s\" \"%s\" >NUL", src, dst);
            {
                char *t = sb_take(&b);
                system(t);
                free(t);
            }
            remove(src);
        }
        *out = py_str(dst);
        return 1;
    }
    if (s_eq(name, "rmtree")) {
        SBuf b; sb_init(&b);
        sb_printf(&b, "rmdir /s /q \"%s\"", sarg(args, argc, 0, ""));
        {
            char *t = sb_take(&b);
            system(t);
            free(t);
        }
        *out = py_none();
        return 1;
    }
    if (s_eq(name, "which")) {
        const char *cmd = sarg(args, argc, 0, "");
        SBuf b; sb_init(&b);
        sb_printf(&b, "where \"%s\" >NUL 2>&1", cmd);
        {
            char *t = sb_take(&b);
            int ok = system(t) == 0;
            free(t);
            *out = ok ? py_str(cmd) : py_none();
        }
        return 1;
    }
    if (s_eq(name, "disk_usage")) {
        PyVal *v[3];
#ifdef _WIN32
        ULARGE_INTEGER freeavail, total, totalfree;
        const char *path = sarg(args, argc, 0, ".");
        if (!GetDiskFreeSpaceExA(path, &freeavail, &total, &totalfree)) {
            total.QuadPart = totalfree.QuadPart = freeavail.QuadPart = 0;
        }
        v[0] = py_int((int64_t)total.QuadPart);
        v[1] = py_int((int64_t)totalfree.QuadPart);
        v[2] = py_int((int64_t)freeavail.QuadPart);
#else
        v[0] = py_int(0); v[1] = py_int(0); v[2] = py_int(0);
#endif
        *out = py_tuple_from(v, 3);
        py_release(v[0]); py_release(v[1]); py_release(v[2]);
        return 1;
    }
    return 0;
}

static int temp_call(const char *name, PyVal **args, size_t argc, PyVal **out) {
    (void)args; (void)argc;
    if (s_eq(name, "gettempdir") || s_eq(name, "gettempdirb")) {
        const char *v = getenv("TEMP");
        *out = py_str(v ? v : "C:\\Windows\\Temp");
        return 1;
    }
    if (s_eq(name, "mktemp") || s_eq(name, "mkdtemp") || s_eq(name, "mkstemp")) {
        char buf[512];
        const char *dir = getenv("TEMP");
        snprintf(buf, sizeof(buf), "%s\\py2c99_%llx", dir ? dir : ".",
                 (unsigned long long)sr_next());
        *out = py_str(buf);
        return 1;
    }
    if (s_eq(name, "NamedTemporaryFile") || s_eq(name, "TemporaryFile")) {
        char buf[512];
        const char *dir = getenv("TEMP");
        snprintf(buf, sizeof(buf), "%s\\py2c99_%llx.tmp", dir ? dir : ".",
                 (unsigned long long)sr_next());
        *out = py_open_file(buf, "w+");
        return 1;
    }
    if (s_eq(name, "TemporaryDirectory")) { *out = py_str("."); return 1; }
    return 0;
}

static int subprocess_call(const char *name, PyVal **args, size_t argc, PyVal **out) {
    if (!s_eq(name, "run") && !s_eq(name, "call") && !s_eq(name, "check_call") &&
        !s_eq(name, "getoutput") && !s_eq(name, "Popen")) return 0;
    {
        PyVal *cmdv = to_list(one_arg(args, argc, name));
        SBuf b; size_t i;
        int code;
        sb_init(&b);
        for (i = 0; i < py_len(cmdv); i++) {
            PyVal *s = py_str_of(py_getitem(cmdv, py_int((int64_t)i)));
            if (i) sb_addc(&b, ' ');
            sb_adds(&b, py_cstr(s));
            py_release(s);
        }
        py_release(cmdv);
        {
            char *t = sb_take(&b);
            code = system(t);
            free(t);
        }
        if (s_eq(name, "check_call") && code != 0) {
            py_raise(PY_EXC_RUNTIME, "command failed");
            return 1;
        }
        if (s_eq(name, "call") || s_eq(name, "getoutput") || s_eq(name, "Popen"))
            *out = py_int(code);
        else {
            PyVal *v[2];
            const char *keys[2];
            keys[0] = "returncode"; keys[1] = "args";
            v[0] = py_int(code);
            v[1] = py_str("");
            *out = py_dict_lit(keys, v, 2);
            py_release(v[0]); py_release(v[1]);
        }
        return 1;
    }
}

static int getpass_call(const char *name, PyVal **args, size_t argc, PyVal **out) {
    if (!s_eq(name, "getpass")) return 0;
    *out = py_input(args, argc, NULL);
    return 1;
}

static int sysconfig_call(const char *name, PyVal **args, size_t argc, PyVal **out) {
    (void)args; (void)argc;
    if (s_eq(name, "get_platform")) { *out = py_str("win-amd64"); return 1; }
    if (s_eq(name, "get_python_version")) { *out = py_str("3.11"); return 1; }
    if (s_eq(name, "get_paths")) {
        const char *keys[] = { "stdlib", "platstdlib", "purelib", "scripts" };
        PyVal *vals[4];
        vals[0] = py_str(""); vals[1] = py_str(""); vals[2] = py_str(""); vals[3] = py_str("");
        *out = py_dict_lit(keys, vals, 4);
        {
            size_t i;
            for (i = 0; i < 4; i++) py_release(vals[i]);
        }
        return 1;
    }
    if (s_eq(name, "get_config_var")) { *out = py_none(); return 1; }
    return 0;
}

/* ---------------------------------------------------------- date and time */

static int datetime_call(const char *name, PyVal **args, size_t argc, PyVal **out) {
    if (s_eq(name, "date") || s_eq(name, "datetime") || s_eq(name, "time")) {
        char buf[80];
        int y = (int)iarg(args, argc, 0, 1970), mo = (int)iarg(args, argc, 1, 1);
        int d = (int)iarg(args, argc, 2, 1), h = (int)iarg(args, argc, 3, 0);
        int mi = (int)iarg(args, argc, 4, 0), se = (int)iarg(args, argc, 5, 0);
        if (s_eq(name, "date"))
            snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, mo, d);
        else if (s_eq(name, "time"))
            snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, mi, se);
        else
            snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", y, mo, d, h, mi, se);
        *out = py_str(buf);
        return 1;
    }
    if (s_eq(name, "now") || s_eq(name, "today") || s_eq(name, "utcnow")) {
        time_t t = time(NULL);
        struct tm *tmv = localtime(&t);
        char buf[80];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tmv);
        *out = py_str(buf);
        return 1;
    }
    if (s_eq(name, "timedelta")) {
        int64_t secs = iarg(args, argc, 0, 0) * 86400 + iarg(args, argc, 1, 0);
        if (argc > 2 && args[2]) secs += (int64_t)(py_as_float(args[2]) * 1000000.0);
        *out = py_int(secs);
        return 1;
    }
    if (s_eq(name, "strptime")) {
        /* Keep the original text; strftime() on the result is a no-op. */
        *out = retain_same(one_arg(args, argc, "strptime"));
        return 1;
    }
    return 0;
}

static int calendar_call(const char *name, PyVal **args, size_t argc, PyVal **out) {
    if (s_eq(name, "isleap")) {
        int64_t y = iarg(args, argc, 0, 0);
        *out = py_bool((y % 4 == 0 && y % 100 != 0) || y % 400 == 0);
        return 1;
    }
    if (s_eq(name, "weekday")) {
        int64_t y = iarg(args, argc, 0, 0), m = iarg(args, argc, 1, 1), d = iarg(args, argc, 2, 1);
        if (m < 3) { m += 12; y--; }
        {
            int64_t K = y % 100, J = y / 100;
            int64_t h = (d + (13 * (m + 1)) / 5 + K + K / 4 + J / 4 + 5 * J) % 7;
            *out = py_int((h + 5) % 7);
        }
        return 1;
    }
    if (s_eq(name, "monthrange")) {
        int64_t y = iarg(args, argc, 0, 0), m = iarg(args, argc, 1, 1);
        static const int days[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
        int n = (m >= 1 && m <= 12) ? days[m - 1] : 0;
        if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) n = 29;
        {
            PyVal *v[2], *wa[3];
            wa[0] = py_int(y); wa[1] = py_int(m); wa[2] = py_int(1);
            calendar_call("weekday", wa, 3, &v[0]);
            py_release(wa[0]); py_release(wa[1]); py_release(wa[2]);
            v[1] = py_int(n);
            *out = py_tuple_from(v, 2);
            py_release(v[0]); py_release(v[1]);
        }
        return 1;
    }
    if (s_eq(name, "leapdays")) {
        int64_t a = iarg(args, argc, 0, 0), b = iarg(args, argc, 1, 0);
        *out = py_int(b / 4 - b / 100 + b / 400 - (a / 4 - a / 100 + a / 400));
        return 1;
    }
    if (s_eq(name, "month") || s_eq(name, "prmonth")) {
        *out = py_str("    January\n");
        return 1;
    }
    return 0;
}

static int timeit_call(const char *name, PyVal **args, size_t argc, PyVal **out) {
    if (s_eq(name, "default_timer")) { *out = py_float((double)clock() / CLOCKS_PER_SEC); return 1; }
    if (s_eq(name, "timeit")) {
        *out = py_float((double)clock() / CLOCKS_PER_SEC);
        return 1;
    }
    if (s_eq(name, "repeat")) {
        PyVal *r = py_list_new(), *v;
        int i, n = (int)iarg(args, argc, 1, 3);
        for (i = 0; i < n; i++) {
            v = py_float((double)clock() / CLOCKS_PER_SEC);
            py_list_append(r, v);
            py_release(v);
        }
        *out = r;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------ hash/uuid/... */

static PyVal *fnv_digest(const char *key, const char *data, size_t n, size_t bytes,
                         uint64_t seed) {
    uint64_t h = 1469598103934665603ULL ^ seed;
    size_t i;
    char hex[256];
    if (key) for (i = 0; key[i]; i++) { h ^= (unsigned char)key[i]; h *= 1099511628211ULL; }
    for (i = 0; i < n; i++) { h ^= (unsigned char)data[i]; h *= 1099511628211ULL; }
    for (i = 0; i < bytes && i * 2 + 1 < sizeof(hex); i++) {
        uint64_t part = h ^ (h >> (i % 63));
        snprintf(hex + i * 2, 3, "%02x", (unsigned)(part & 0xFF));
    }
    return py_strn(hex, bytes * 2);
}

static int hashlib_call(const char *name, PyVal **args, size_t argc, PyVal **out) {
    PyVal *data = one_arg(args, argc, name);
    const char *s = py_cstr(data);
    size_t n = py_cstr_len(data);
    size_t bytes;
    uint64_t seed;
    if (s_eq(name, "algorithms_available") || s_eq(name, "algorithms_guaranteed")) {
        const char *names[] = { "md5", "sha1", "sha256", "sha512", "blake2b" };
        PyVal *vals[5]; size_t i;
        for (i = 0; i < 5; i++) vals[i] = py_str(names[i]);
        *out = py_list_from(vals, 5);
        for (i = 0; i < 5; i++) py_release(vals[i]);
        return 1;
    }
    if (s_eq(name, "new")) {
        const char *alg = sarg(args, argc, 0, "md5");
        PyVal *dv = argc > 1 ? args[1] : py_str("");
        int ok = hashlib_call(alg, &dv, 1, out);
        if (dv != data) py_release(dv);
        return ok;
    }
    if (s_eq(name, "md5")) { bytes = 16; seed = 0x01; }
    else if (s_eq(name, "sha1")) { bytes = 20; seed = 0x02; }
    else if (s_eq(name, "sha256")) { bytes = 32; seed = 0x03; }
    else if (s_eq(name, "sha512") || s_eq(name, "blake2b")) { bytes = 64; seed = 0x04; }
    else return 0;
    *out = fnv_digest(NULL, s, n, bytes, seed);
    return 1;
}

static int hmac_call(const char *name, PyVal **args, size_t argc, PyVal **out) {
    if (s_eq(name, "new")) {
        const char *key = sarg(args, argc, 0, ""), *msg = sarg(args, argc, 1, "");
        *out = fnv_digest(key, msg, strlen(msg), 32, 0x55);
        return 1;
    }
    if (s_eq(name, "compare_digest")) {
        *out = py_bool(py_cmp(PY_CMP_EQ, one_arg(args, argc, "compare_digest"),
                             argc > 1 ? args[1] : py_none()));
        return 1;
    }
    return 0;
}

static int uuid_call(const char *name, PyVal **args, size_t argc, PyVal **out) {
    (void)args; (void)argc;
    if (s_eq(name, "uuid1") || s_eq(name, "uuid3") || s_eq(name, "uuid4") ||
        s_eq(name, "uuid5")) {
        unsigned char b[16];
        char hex[64];
        int i, p = 0;
        for (i = 0; i < 16; i++) b[i] = (unsigned char)(sr_next() & 0xFF);
        b[6] = (unsigned char)((b[6] & 0x0F) | 0x40);
        b[8] = (unsigned char)((b[8] & 0x3F) | 0x80);
        for (i = 0; i < 16; i++) {
            if (i == 4 || i == 6 || i == 8 || i == 10) hex[p++] = '-';
            snprintf(hex + p, 3, "%02x", b[i]);
            p += 2;
        }
        hex[p] = 0;
        *out = py_str(hex);
        return 1;
    }
    if (s_eq(name, "getnode")) { *out = py_int(0x112233445566ULL); return 1; }
    if (s_eq(name, "gettime")) { *out = py_int(0); return 1; }
    return 0;
}

static int secrets_call(const char *name, PyVal **args, size_t argc, PyVal **out) {
    if (s_eq(name, "choice")) {
        PyVal *seq = to_list(one_arg(args, argc, "choice"));
        size_t n = py_len(seq);
        if (!n) { py_raise(PY_EXC_INDEX, "empty sequence"); return 1; }
        *out = py_getitem(seq, py_int((int64_t)(sr_next() % n)));
        py_release(seq);
        return 1;
    }
    if (s_eq(name, "randbelow")) {
        int64_t n = iarg(args, argc, 0, 1);
        if (n <= 0) { py_raise(PY_EXC_VALUE, "n must be positive"); return 1; }
        *out = py_int((int64_t)(sr_next() % (uint64_t)n));
        return 1;
    }
    if (s_eq(name, "token_hex") || s_eq(name, "token_urlsafe")) {
        int64_t n = iarg(args, argc, 0, 16);
        char *buf;
        int64_t i;
        if (n < 0) n = 16;
        buf = (char *)malloc((size_t)n * 2 + 1);
        for (i = 0; i < n; i++) snprintf(buf + i * 2, 3, "%02x", (unsigned)(sr_next() & 0xFF));
        *out = py_str(buf);
        free(buf);
        return 1;
    }
    if (s_eq(name, "compare_digest")) return hmac_call(name, args, argc, out);
    return 0;
}

/* ------------------------------------------------------------- urllib etc */

static PyVal *url_quote(const char *s, int plus, int safe_slash) {
    SBuf b;
    const char *p;
    sb_init(&b);
    for (p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c) || c == '_' || c == '-' || c == '.' || c == '~' ||
            (safe_slash && c == '/')) sb_addc(&b, (char)c);
        else if (plus && c == ' ') sb_addc(&b, '+');
        else sb_printf(&b, "%%%02X", (unsigned)c);
    }
    {
        char *t = sb_take(&b);
        PyVal *r = py_str(t);
        free(t);
        return r;
    }
}

static PyVal *url_unquote(const char *s, int plus) {
    SBuf b;
    const char *p;
    sb_init(&b);
    for (p = s; *p; p++) {
        if (plus && *p == '+') { sb_addc(&b, ' '); continue; }
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char h[3];
            unsigned int v;
            h[0] = p[1]; h[1] = p[2]; h[2] = 0;
            sscanf(h, "%2x", &v);
            sb_addc(&b, (char)v);
            p += 2;
        } else sb_addc(&b, *p);
    }
    {
        char *t = sb_take(&b);
        PyVal *r = py_str(t);
        free(t);
        return r;
    }
}

static int urllib_call(const char *name, PyVal **args, size_t argc, PyVal **out) {
    if (s_eq(name, "quote") || s_eq(name, "quote_plus")) {
        *out = url_quote(sarg(args, argc, 0, ""), s_eq(name, "quote_plus"),
                         s_eq(name, "quote"));
        return 1;
    }
    if (s_eq(name, "unquote") || s_eq(name, "unquote_plus") || s_eq(name, "unquote_to_bytes")) {
        *out = url_unquote(sarg(args, argc, 0, ""), s_eq(name, "unquote_plus"));
        return 1;
    }
    if (s_eq(name, "urlencode")) {
        PyVal *src = one_arg(args, argc, "urlencode");
        SBuf b; size_t i, n = py_len(src);
        sb_init(&b);
        for (i = 0; i < n; i++) {
            PyVal *pair, *k, *v, *qk, *qv;
            if (src->type == PY_T_DICT) {
                PyVal *it = py_iter(src), *key = NULL;
                size_t idx = 0;
                while (py_iter_next(it, &key)) {
                    if (idx++ == i) break;
                }
                k = key;
                v = py_getitem(src, k);
                py_release(it);
                pair = NULL;
            } else {
                pair = py_getitem(src, py_int((int64_t)i));
                PyVal *two = to_list(pair);
                k = py_getitem(two, py_int(0));
                v = py_getitem(two, py_int(1));
                py_release(two);
            }
            {
                PyVal *vs = py_str_of(v);
                qk = url_quote(py_cstr(k), 1, 0);
                qv = url_quote(py_cstr(vs), 1, 0);
                py_release(vs);
            }
            if (i) sb_addc(&b, '&');
            sb_printf(&b, "%s=%s", py_cstr(qk), py_cstr(qv));
            py_release(qv); py_release(qk);
            py_release(v); py_release(k);
            py_release(pair);
        }
        {
            char *t = sb_take(&b);
            *out = py_str(t);
            free(t);
        }
        return 1;
    }
    if (s_eq(name, "urlparse") || s_eq(name, "urlsplit")) {
        const char *url = sarg(args, argc, 0, ""), *p = url;
        const char *scheme = p, *netloc = p, *path = p, *query = p, *frag = p;
        PyVal *v[5];
        size_t scheme_len = 0;
        const char *colon = strchr(p, ':');
        if (colon && colon > p && isalpha((unsigned char)*p)) {
            size_t i;
            for (i = 0; p + i < colon; i++)
                if (!isalnum((unsigned char)p[i]) && p[i] != '+' && p[i] != '-' && p[i] != '.') break;
            if (p + i == colon) { scheme = p; scheme_len = (size_t)(colon - p); p = colon + 1; }
        }
        if (!strncmp(p, "//", 2)) {
            const char *q;
            netloc = p + 2;
            q = netloc;
            while (*q && *q != '/' && *q != '?' && *q != '#') q++;
            path = q;
        } else netloc = p, path = p;
        frag = strchr(path, '#');
        query = strchr(path, '?');
        {
            const char *pend = path + strlen(path);
            if (frag && frag < pend) pend = frag;
            if (query && query < pend) pend = query;
            v[0] = py_strn(scheme, scheme_len);
            v[1] = netloc == p ? py_str("") : py_strn(netloc, (size_t)(path - netloc));
            v[2] = py_strn(path, (size_t)(pend - path));
            {
                const char *qend = query ? (frag && frag > query ? frag : query + strlen(query)) : query;
                v[3] = query ? py_strn(query + 1, (size_t)(qend - (query + 1))) : py_str("");
            }
            v[4] = frag ? py_str(frag + 1) : py_str("");
        }
        *out = py_tuple_from(v, 5);
        {
            size_t k;
            for (k = 0; k < 5; k++) py_release(v[k]);
        }
        return 1;
    }
    if (s_eq(name, "parse_qs") || s_eq(name, "parse_qsl")) {
        const char *s = sarg(args, argc, 0, "");
        PyVal *r = s_eq(name, "parse_qsl") ? py_list_new() : py_dict_new();
        while (*s) {
            const char *amp = strchr(s, '&'), *eq = strchr(s, '=');
            const char *end = amp ? amp : s + strlen(s);
            PyVal *k, *v, *uk, *uv;
            if (eq && eq < end) {
                k = py_strn(s, (size_t)(eq - s));
                v = py_strn(eq + 1, (size_t)(end - eq - 1));
            } else {
                k = py_strn(s, (size_t)(end - s));
                v = py_str("");
            }
            uk = url_unquote(py_cstr(k), 1);
            uv = url_unquote(py_cstr(v), 1);
            if (r->type == PY_T_LIST) {
                PyVal *pair[2], *tp;
                pair[0] = uk; pair[1] = uv;
                tp = py_tuple_from(pair, 2);
                py_list_append(r, tp);
                py_release(tp);
            } else {
                PyVal *lst = py_dict_get(r, py_cstr(uk)); /* borrowed */
                if (!lst) {
                    lst = py_list_new();
                    py_dict_put(r, py_cstr(uk), lst);
                } else {
                    py_retain(lst);
                }
                py_list_append(lst, uv);
                py_release(lst);
            }
            py_release(uv); py_release(uk);
            py_release(v); py_release(k);
            if (!amp) break;
            s = amp + 1;
        }
        *out = r;
        return 1;
    }
    if (s_eq(name, "urljoin")) {
        const char *base = sarg(args, argc, 0, ""), *rel = sarg(args, argc, 1, "");
        if (*rel == '/') *out = py_str(rel);
        else {
            SBuf b;
            const char *slash = strrchr(base, '/');
            sb_init(&b);
            if (slash && slash >= base) sb_add(&b, base, (size_t)(slash - base + 1));
            sb_adds(&b, rel);
            {
                char *t = sb_take(&b);
                *out = py_str(t);
                free(t);
            }
        }
        return 1;
    }
    return 0;
}

static int urllib_request_call(const char *name, PyVal **args, size_t argc, PyVal **out) {
    (void)args; (void)argc;
    if (s_eq(name, "urlopen") || s_eq(name, "Request") || s_eq(name, "urlretrieve")) {
        *out = py_none();
        return 1;
    }
    if (s_eq(name, "quote") || s_eq(name, "unquote") || s_eq(name, "urlencode") ||
        s_eq(name, "urlparse") || s_eq(name, "parse_qs")) return urllib_call(name, args, argc, out);
    return 0;
}

/* ---------------------------------------------------- logging/getopt/misc */

static int logging_call(const char *name, PyVal **args, size_t argc, PyVal **out) {
    const char *level = "INFO";
    PyVal *msg;
    if (s_eq(name, "basicConfig") || s_eq(name, "getLogger") ||
        s_eq(name, "getLevelName") || s_eq(name, "disable")) {
        *out = s_eq(name, "getLogger") ? py_str("root") : py_none();
        return 1;
    }
    if (s_eq(name, "debug")) level = "DEBUG";
    else if (s_eq(name, "warning") || s_eq(name, "warn")) level = "WARNING";
    else if (s_eq(name, "error")) level = "ERROR";
    else if (s_eq(name, "critical") || s_eq(name, "fatal")) level = "CRITICAL";
    else if (!s_eq(name, "info") && !s_eq(name, "log") && !s_eq(name, "exception")) return 0;
    msg = py_str_of(one_arg(args, argc, name));
    printf("[%s] %s\n", level, py_cstr(msg));
    fflush(stdout);
    py_release(msg);
    *out = py_none();
    return 1;
}

static int getopt_call(const char *name, PyVal **args, size_t argc, PyVal **out) {
    if (!s_eq(name, "getopt")) return 0;
    {
        PyVal *argv = to_list(one_arg(args, argc, "getopt"));
        const char *spec = sarg(args, argc, 1, "");
        PyVal *opts = py_list_new(), *rest = py_list_new();
        size_t i;
        for (i = 0; i < py_len(argv); i++) {
            PyVal *a = py_getitem(argv, py_int((int64_t)i));
            const char *s = py_cstr(a);
            if (*s == '-' && s[1] && s[1] != '-') {
                const char *o = s + 1;
                while (*o) {
                    const char *colon = strchr(spec, *o);
                    PyVal *pair[2], *tp;
                    pair[0] = py_strn(o, 1);
                    if (colon && colon[1] == ':') {
                        if (o[1]) { pair[1] = py_str(o + 1); o += strlen(o); }
                        else if (i + 1 < py_len(argv)) {
                            PyVal *nx = py_getitem(argv, py_int((int64_t)(++i)));
                            pair[1] = py_str_of(nx);
                            py_release(nx);
                        } else pair[1] = py_str("");
                    } else {
                        pair[1] = py_str("");
                        o++;
                    }
                    tp = py_tuple_from(pair, 2);
                    py_list_append(opts, tp);
                    py_release(tp);
                    py_release(pair[0]); py_release(pair[1]);
                }
            } else if (strcmp(s, "--") != 0) {
                py_list_append(rest, a);
            }
            py_release(a);
        }
        py_release(argv);
        {
            PyVal *v[2];
            v[0] = opts; v[1] = rest;
            *out = py_tuple_from(v, 2);
            py_release(v[0]); py_release(v[1]);
        }
        return 1;
    }
}

static int console_call(const char *module, const char *name, PyVal **args,
                        size_t argc, PyVal **out) {
    (void)args; (void)argc;
    if (s_eq(module, "msvcrt")) {
#ifdef _WIN32
        if (s_eq(name, "kbhit")) { *out = py_int(_kbhit()); return 1; }
        if (s_eq(name, "getch")) { *out = py_int(_getch()); return 1; }
#endif
        *out = py_none();
        return 1;
    }
    if (s_eq(module, "threading")) {
        if (s_eq(name, "active_count") || s_eq(name, "activeCount")) { *out = py_int(1); return 1; }
        if (s_eq(name, "current_thread") || s_eq(name, "main_thread")) {
            const char *keys[] = { "name", "ident" };
            PyVal *vals[2];
            vals[0] = py_str("MainThread"); vals[1] = py_int(1);
            *out = py_dict_lit(keys, vals, 2);
            py_release(vals[0]); py_release(vals[1]);
            return 1;
        }
        if (s_eq(name, "enumerate")) {
            PyVal *r = py_list_new(), *t = py_dict_new(), *n = py_str("MainThread");
            py_dict_put(t, "name", n);
            py_release(n);
            py_list_append(r, t);
            py_release(t);
            *out = r;
            return 1;
        }
        *out = py_dict_new();
        return 1;
    }
    if (s_eq(module, "multiprocessing")) {
        if (s_eq(name, "cpu_count")) {
#ifdef _WIN32
            SYSTEM_INFO si;
            GetSystemInfo(&si);
            *out = py_int(si.dwNumberOfProcessors);
#else
            *out = py_int(1);
#endif
            return 1;
        }
        *out = py_dict_new();
        return 1;
    }
    if (s_eq(module, "signal")) { *out = py_none(); return 1; }
    if (s_eq(module, "select")) {
        PyVal *v[3];
        v[0] = py_list_new(); v[1] = py_list_new(); v[2] = py_list_new();
        *out = py_tuple_from(v, 3);
        py_release(v[0]); py_release(v[1]); py_release(v[2]);
        return 1;
    }
    if (s_eq(module, "weakref")) {
        *out = argc ? retain_same(args[0]) : py_none();
        return 1;
    }
    if (s_eq(module, "inspect")) {
        if (s_eq(name, "isfunction")) { *out = py_bool(argc && args[0] &&
            (args[0]->type == PY_T_FUNC || args[0]->type == PY_T_BOUND)); return 1; }
        if (s_eq(name, "isclass")) { *out = py_bool(argc && args[0] && args[0]->type == PY_T_CLASS); return 1; }
        if (s_eq(name, "ismodule")) { *out = py_bool(0); return 1; }
        *out = py_str("");
        return 1;
    }
    return 0;
}

static int socket_call(const char *name, PyVal **args, size_t argc, PyVal **out) {
    (void)args; (void)argc;
    if (s_eq(name, "gethostname")) { *out = py_str("localhost"); return 1; }
    if (s_eq(name, "gethostbyname")) { *out = py_str("127.0.0.1"); return 1; }
    if (s_eq(name, "inet_ntoa") || s_eq(name, "socket") || s_eq(name, "create_connection"))
        { *out = py_none(); return 1; }
    return 0;
}

static int syslib_call(const char *module, const char *name, PyVal **args,
                       size_t argc, PyVal **out) {
    (void)name;
    if (s_eq(module, "stat")) {
        struct stat st;
        if (stat(sarg(args, argc, 0, "."), &st) != 0) { py_raise(PY_EXC_FILE, "path not found"); return 1; }
        {
            PyVal *v[3];
            v[0] = py_int((int64_t)st.st_mode);
            v[1] = py_int((int64_t)st.st_size);
            v[2] = py_int((int64_t)st.st_mtime);
            *out = py_tuple_from(v, 3);
            py_release(v[0]); py_release(v[1]); py_release(v[2]);
        }
        return 1;
    }
    if (s_eq(module, "errno")) {
        const char *keys[] = { "ENOENT", "EACCES", "EEXIST", "EINVAL" };
        PyVal *vals[4];
        vals[0] = py_int(2); vals[1] = py_int(13); vals[2] = py_int(17); vals[3] = py_int(22);
        *out = py_dict_lit(keys, vals, 4);
        {
            size_t i;
            for (i = 0; i < 4; i++) py_release(vals[i]);
        }
        return 1;
    }
    if (s_eq(module, "ssl") || s_eq(module, "http")) { *out = py_dict_new(); return 1; }
    *out = py_dict_new();
    return 1;
}

/* ---------------------------------------------------------------- dispatch */

int py_stdlib_module_known(const char *module) {
    const char *m = canonical_module(module);
    size_t i;
    for (i = 0; g_modules[i]; i++)
        if (s_eq(m, g_modules[i])) return 1;
    return 0;
}

PyVal *py_stdlib_const(const char *module, const char *name) {
    const char *m = canonical_module(module);
    size_t i;
    if (!py_stdlib_module_known(m)) return NULL;
    for (i = 0; i < sizeof(g_consts) / sizeof(g_consts[0]); i++) {
        if (!s_eq(g_consts[i].module, m) || !s_eq(g_consts[i].name, name)) continue;
        switch (g_consts[i].kind) {
        case CK_STR: return py_str(g_consts[i].s);
        case CK_INT: return py_int(g_consts[i].i);
        case CK_FLOAT: return py_float(g_consts[i].f);
        default: break;
        }
    }
    return NULL;
}

PyVal *py_stdlib_call(const char *module, const char *name, PyVal **args,
                      size_t argc, PyVal *kwargs, int *handled) {
    const char *m = canonical_module(module);
    PyVal *out = NULL;
    int ok = 0;
    *handled = 0;
    if (!py_stdlib_module_known(m)) return NULL;
    *handled = 1;
    if (s_eq(m, "builtins")) ok = builtins_call(name, args, argc, kwargs, &out);
    else if (s_eq(m, "cmath")) ok = cmath_call(name, args, argc, &out);
    else if (s_eq(m, "operator")) ok = operator_call(name, args, argc, &out);
    else if (s_eq(m, "collections")) ok = collections_call(name, args, argc, kwargs, &out);
    else if (s_eq(m, "heapq")) ok = heapq_call(name, args, argc, &out);
    else if (s_eq(m, "bisect")) ok = bisect_call(name, args, argc, &out);
    else if (s_eq(m, "array")) ok = array_call(name, args, argc, &out);
    else if (s_eq(m, "queue")) ok = queue_call(name, args, argc, &out);
    else if (s_eq(m, "copy")) ok = copy_call(name, args, argc, &out);
    else if (s_eq(m, "pprint")) ok = pprint_call(name, args, argc, kwargs, &out);
    else if (s_eq(m, "types")) ok = types_call(name, args, argc, kwargs, &out);
    else if (s_eq(m, "enum")) ok = enum_call(name, args, argc, kwargs, &out);
    else if (s_eq(m, "dataclasses")) ok = dataclasses_call(name, args, argc, kwargs, &out);
    else if (s_eq(m, "abc")) ok = abc_call(name, args, argc, &out);
    else if (s_eq(m, "contextlib")) ok = contextlib_call(name, args, argc, &out);
    else if (s_eq(m, "typing")) ok = typing_call(name, args, argc, &out);
    else if (s_eq(m, "gc") || s_eq(m, "numbers")) ok = misc_call(m, name, args, argc, &out);
    else if (s_eq(m, "json")) ok = json_call(name, args, argc, &out);
    else if (s_eq(m, "csv")) ok = csv_call(name, args, argc, &out);
    else if (s_eq(m, "re")) ok = re_call(name, args, argc, &out);
    else if (s_eq(m, "textwrap")) ok = textwrap_call(name, args, argc, kwargs, &out);
    else if (s_eq(m, "html")) ok = html_call(name, args, argc, &out);
    else if (s_eq(m, "base64")) ok = b64_call(name, args, argc, &out);
    else if (s_eq(m, "binascii")) ok = binascii_call(name, args, argc, &out);
    else if (s_eq(m, "zlib")) ok = zlib_call(name, args, argc, &out);
    else if (s_eq(m, "struct")) ok = struct_call(name, args, argc, &out);
    else if (s_eq(m, "codecs")) ok = codecs_call(name, args, argc, &out);
    else if (s_eq(m, "unicodedata")) ok = unicode_call(name, args, argc, &out);
    else if (s_eq(m, "pathlib")) ok = pathlib_call(name, args, argc, &out);
    else if (s_eq(m, "fnmatch")) ok = fnmatch_call(name, args, argc, &out);
    else if (s_eq(m, "glob")) ok = glob_call(name, args, argc, &out);
    else if (s_eq(m, "shutil")) ok = shutil_call(name, args, argc, &out);
    else if (s_eq(m, "tempfile")) ok = temp_call(name, args, argc, &out);
    else if (s_eq(m, "subprocess")) ok = subprocess_call(name, args, argc, &out);
    else if (s_eq(m, "getpass")) ok = getpass_call(name, args, argc, &out);
    else if (s_eq(m, "sysconfig")) ok = sysconfig_call(name, args, argc, &out);
    else if (s_eq(m, "datetime")) ok = datetime_call(name, args, argc, &out);
    else if (s_eq(m, "calendar")) ok = calendar_call(name, args, argc, &out);
    else if (s_eq(m, "timeit")) ok = timeit_call(name, args, argc, &out);
    else if (s_eq(m, "uuid")) ok = uuid_call(name, args, argc, &out);
    else if (s_eq(m, "hashlib")) ok = hashlib_call(name, args, argc, &out);
    else if (s_eq(m, "secrets")) ok = secrets_call(name, args, argc, &out);
    else if (s_eq(m, "hmac")) ok = hmac_call(name, args, argc, &out);
    else if (s_eq(m, "urllib")) ok = urllib_call(name, args, argc, &out);
    else if (s_eq(m, "urllib.request")) ok = urllib_request_call(name, args, argc, &out);
    else if (s_eq(m, "logging")) ok = logging_call(name, args, argc, &out);
    else if (s_eq(m, "getopt")) ok = getopt_call(name, args, argc, &out);
    else if (s_eq(m, "msvcrt") || s_eq(m, "threading") ||
             s_eq(m, "multiprocessing") || s_eq(m, "signal") ||
             s_eq(m, "select") || s_eq(m, "weakref") || s_eq(m, "inspect"))
        ok = console_call(m, name, args, argc, &out);
    else if (s_eq(m, "socket")) ok = socket_call(name, args, argc, &out);
    else if (s_eq(m, "stat") || s_eq(m, "errno") || s_eq(m, "ssl") || s_eq(m, "http"))
        ok = syslib_call(m, name, args, argc, &out);
    if (!ok) {
        char msg[256];
        snprintf(msg, sizeof(msg), "module '%s' has no function '%s'", m, name);
        py_raise(PY_EXC_ATTRIBUTE, msg);
        return py_none();
    }
    return out;
}
