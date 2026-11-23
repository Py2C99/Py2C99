/*
 * GENERATED FILE - do not edit by hand.
 * Regenerate with: python tools/gen_registry311.py  (under Python 3.11)
 *
 * Exhaustive Python 3.11 builtin / stdlib name registries and the compatibility
 * layer that makes every stdlib module name importable in compiled mode.
 */
#define _CRT_SECURE_NO_WARNINGS
#include "pyrt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int e311(const char *a, const char *b) { return a && b && !strcmp(a, b); }

/* Every name in dir(builtins) for CPython 3.11. */
static const char *const B311[] = {
    "ArithmeticError", "AssertionError", "AttributeError", "BaseException", "BaseExceptionGroup", "BlockingIOError",
    "BrokenPipeError", "BufferError", "BytesWarning", "ChildProcessError", "ConnectionAbortedError", "ConnectionError",
    "ConnectionRefusedError", "ConnectionResetError", "DeprecationWarning", "EOFError", "Ellipsis", "EncodingWarning",
    "EnvironmentError", "Exception", "ExceptionGroup", "False", "FileExistsError", "FileNotFoundError",
    "FloatingPointError", "FutureWarning", "GeneratorExit", "IOError", "ImportError", "ImportWarning",
    "IndentationError", "IndexError", "InterruptedError", "IsADirectoryError", "KeyError", "KeyboardInterrupt",
    "LookupError", "MemoryError", "ModuleNotFoundError", "NameError", "None", "NotADirectoryError",
    "NotImplemented", "NotImplementedError", "OSError", "OverflowError", "PendingDeprecationWarning", "PermissionError",
    "ProcessLookupError", "RecursionError", "ReferenceError", "ResourceWarning", "RuntimeError", "RuntimeWarning",
    "StopAsyncIteration", "StopIteration", "SyntaxError", "SyntaxWarning", "SystemError", "SystemExit",
    "TabError", "TimeoutError", "True", "TypeError", "UnboundLocalError", "UnicodeDecodeError",
    "UnicodeEncodeError", "UnicodeError", "UnicodeTranslateError", "UnicodeWarning", "UserWarning", "ValueError",
    "Warning", "WindowsError", "ZeroDivisionError", "__build_class__", "__debug__", "__doc__",
    "__import__", "__loader__", "__name__", "__package__", "__spec__", "abs",
    "aiter", "all", "anext", "any", "ascii", "bin",
    "bool", "breakpoint", "bytearray", "bytes", "callable", "chr",
    "classmethod", "compile", "complex", "copyright", "credits", "delattr",
    "dict", "dir", "divmod", "enumerate", "eval", "exec",
    "exit", "filter", "float", "format", "frozenset", "getattr",
    "globals", "hasattr", "hash", "help", "hex", "id",
    "input", "int", "isinstance", "issubclass", "iter", "len",
    "license", "list", "locals", "map", "max", "memoryview",
    "min", "next", "object", "oct", "open", "ord",
    "pow", "print", "property", "quit", "range", "repr",
    "reversed", "round", "set", "setattr", "slice", "sorted",
    "staticmethod", "str", "sum", "super", "tuple", "type",
    "vars", "zip",
};

/* Every name in sys.stdlib_module_names for CPython 3.11. */
static const char *const M311[] = {
    "__future__", "_abc", "_aix_support", "_ast", "_asyncio", "_bisect",
    "_blake2", "_bootsubprocess", "_bz2", "_codecs", "_codecs_cn", "_codecs_hk",
    "_codecs_iso2022", "_codecs_jp", "_codecs_kr", "_codecs_tw", "_collections", "_collections_abc",
    "_compat_pickle", "_compression", "_contextvars", "_crypt", "_csv", "_ctypes",
    "_curses", "_curses_panel", "_datetime", "_dbm", "_decimal", "_elementtree",
    "_frozen_importlib", "_frozen_importlib_external", "_functools", "_gdbm", "_hashlib", "_heapq",
    "_imp", "_io", "_json", "_locale", "_lsprof", "_lzma",
    "_markupbase", "_md5", "_msi", "_multibytecodec", "_multiprocessing", "_opcode",
    "_operator", "_osx_support", "_overlapped", "_pickle", "_posixshmem", "_posixsubprocess",
    "_py_abc", "_pydecimal", "_pyio", "_queue", "_random", "_scproxy",
    "_sha1", "_sha256", "_sha3", "_sha512", "_signal", "_sitebuiltins",
    "_socket", "_sqlite3", "_sre", "_ssl", "_stat", "_statistics",
    "_string", "_strptime", "_struct", "_symtable", "_thread", "_threading_local",
    "_tkinter", "_tokenize", "_tracemalloc", "_typing", "_uuid", "_warnings",
    "_weakref", "_weakrefset", "_winapi", "_zoneinfo", "abc", "aifc",
    "antigravity", "argparse", "array", "ast", "asynchat", "asyncio",
    "asyncore", "atexit", "audioop", "base64", "bdb", "binascii",
    "bisect", "builtins", "bz2", "cProfile", "calendar", "cgi",
    "cgitb", "chunk", "cmath", "cmd", "code", "codecs",
    "codeop", "collections", "colorsys", "compileall", "concurrent", "configparser",
    "contextlib", "contextvars", "copy", "copyreg", "crypt", "csv",
    "ctypes", "curses", "dataclasses", "datetime", "dbm", "decimal",
    "difflib", "dis", "distutils", "doctest", "email", "encodings",
    "ensurepip", "enum", "errno", "faulthandler", "fcntl", "filecmp",
    "fileinput", "fnmatch", "fractions", "ftplib", "functools", "gc",
    "genericpath", "getopt", "getpass", "gettext", "glob", "graphlib",
    "grp", "gzip", "hashlib", "heapq", "hmac", "html",
    "http", "idlelib", "imaplib", "imghdr", "imp", "importlib",
    "inspect", "io", "ipaddress", "itertools", "json", "keyword",
    "lib2to3", "linecache", "locale", "logging", "lzma", "mailbox",
    "mailcap", "marshal", "math", "mimetypes", "mmap", "modulefinder",
    "msilib", "msvcrt", "multiprocessing", "netrc", "nis", "nntplib",
    "nt", "ntpath", "nturl2path", "numbers", "opcode", "operator",
    "optparse", "os", "ossaudiodev", "pathlib", "pdb", "pickle",
    "pickletools", "pipes", "pkgutil", "platform", "plistlib", "poplib",
    "posix", "posixpath", "pprint", "profile", "pstats", "pty",
    "pwd", "py_compile", "pyclbr", "pydoc", "pydoc_data", "pyexpat",
    "queue", "quopri", "random", "re", "readline", "reprlib",
    "resource", "rlcompleter", "runpy", "sched", "secrets", "select",
    "selectors", "shelve", "shlex", "shutil", "signal", "site",
    "smtpd", "smtplib", "sndhdr", "socket", "socketserver", "spwd",
    "sqlite3", "sre_compile", "sre_constants", "sre_parse", "ssl", "stat",
    "statistics", "string", "stringprep", "struct", "subprocess", "sunau",
    "symtable", "sys", "sysconfig", "syslog", "tabnanny", "tarfile",
    "telnetlib", "tempfile", "termios", "textwrap", "this", "threading",
    "time", "timeit", "tkinter", "token", "tokenize", "tomllib",
    "trace", "traceback", "tracemalloc", "tty", "turtle", "turtledemo",
    "types", "typing", "unicodedata", "unittest", "urllib", "uu",
    "uuid", "venv", "warnings", "wave", "weakref", "webbrowser",
    "winreg", "winsound", "wsgiref", "xdrlib", "xml", "xmlrpc",
    "zipapp", "zipfile", "zipimport", "zlib", "zoneinfo",
};

/* Private/implementation-detail modules mapped to their public module. */
static const struct { const char *from; const char *to; } A311[] = {
    { "_abc", "abc" },
    { "_ast", "ast" },
    { "_asyncio", "asyncio" },
    { "_bisect", "bisect" },
    { "_blake2", "hashlib" },
    { "_bootsubprocess", "subprocess" },
    { "_bz2", "bz2" },
    { "_codecs", "codecs" },
    { "_codecs_cn", "codecs" },
    { "_codecs_hk", "codecs" },
    { "_codecs_iso2022", "codecs" },
    { "_codecs_jp", "codecs" },
    { "_codecs_kr", "codecs" },
    { "_codecs_tw", "codecs" },
    { "_collections", "collections" },
    { "_collections_abc", "collections" },
    { "_compat_pickle", "pickle" },
    { "_compression", "bz2" },
    { "_contextvars", "contextvars" },
    { "_crypt", "crypt" },
    { "_csv", "csv" },
    { "_ctypes", "ctypes" },
    { "_curses", "curses" },
    { "_curses_panel", "curses" },
    { "_datetime", "datetime" },
    { "_dbm", "dbm" },
    { "_decimal", "decimal" },
    { "_elementtree", "xml.etree.ElementTree" },
    { "_frozen_importlib", "importlib" },
    { "_frozen_importlib_external", "importlib" },
    { "_functools", "functools" },
    { "_gdbm", "dbm" },
    { "_hashlib", "hashlib" },
    { "_heapq", "heapq" },
    { "_imp", "importlib" },
    { "_io", "io" },
    { "_json", "json" },
    { "_locale", "locale" },
    { "_lsprof", "cProfile" },
    { "_lzma", "lzma" },
    { "_markupbase", "html.parser" },
    { "_md5", "hashlib" },
    { "_msi", "msilib" },
    { "_multibytecodec", "codecs" },
    { "_multiprocessing", "multiprocessing" },
    { "_opcode", "opcode" },
    { "_operator", "operator" },
    { "_osx_support", "sysconfig" },
    { "_overlapped", "asyncio" },
    { "_pickle", "pickle" },
    { "_posixshmem", "multiprocessing" },
    { "_posixsubprocess", "subprocess" },
    { "_py_abc", "abc" },
    { "_pydecimal", "decimal" },
    { "_pyio", "io" },
    { "_queue", "queue" },
    { "_random", "random" },
    { "_scproxy", "urllib.request" },
    { "_sha1", "hashlib" },
    { "_sha256", "hashlib" },
    { "_sha3", "hashlib" },
    { "_sha512", "hashlib" },
    { "_signal", "signal" },
    { "_sitebuiltins", "site" },
    { "_socket", "socket" },
    { "_sqlite3", "sqlite3" },
    { "_sre", "re" },
    { "_ssl", "ssl" },
    { "_stat", "stat" },
    { "_statistics", "statistics" },
    { "_string", "string" },
    { "_strptime", "datetime" },
    { "_struct", "struct" },
    { "_symtable", "symtable" },
    { "_thread", "threading" },
    { "_threading_local", "threading" },
    { "_tkinter", "tkinter" },
    { "_tokenize", "tokenize" },
    { "_tracemalloc", "tracemalloc" },
    { "_typing", "typing" },
    { "_uuid", "uuid" },
    { "_warnings", "warnings" },
    { "_weakref", "weakref" },
    { "_weakrefset", "weakref" },
    { "_winapi", "msvcrt" },
    { "_zoneinfo", "zoneinfo" },
};

/* Exception names are callable classes, not ordinary functions. */
static const char *const E311[] = {
    "ArithmeticError", "AssertionError", "AttributeError", "BaseException", "BaseExceptionGroup", "BlockingIOError",
    "BrokenPipeError", "BufferError", "BytesWarning", "ChildProcessError", "ConnectionAbortedError", "ConnectionError",
    "ConnectionRefusedError", "ConnectionResetError", "DeprecationWarning", "EOFError", "EncodingWarning", "EnvironmentError",
    "Exception", "ExceptionGroup", "FileExistsError", "FileNotFoundError", "FloatingPointError", "FutureWarning",
    "GeneratorExit", "IOError", "ImportError", "ImportWarning", "IndentationError", "IndexError",
    "InterruptedError", "IsADirectoryError", "KeyError", "KeyboardInterrupt", "LookupError", "MemoryError",
    "ModuleNotFoundError", "NameError", "NotADirectoryError", "NotImplementedError", "OSError", "OverflowError",
    "PendingDeprecationWarning", "PermissionError", "ProcessLookupError", "RecursionError", "ReferenceError", "ResourceWarning",
    "RuntimeError", "RuntimeWarning", "StopAsyncIteration", "StopIteration", "SyntaxError", "SyntaxWarning",
    "SystemError", "SystemExit", "TabError", "TimeoutError", "TypeError", "UnboundLocalError",
    "UnicodeDecodeError", "UnicodeEncodeError", "UnicodeError", "UnicodeTranslateError", "UnicodeWarning", "UserWarning",
    "ValueError", "Warning", "WindowsError", "ZeroDivisionError",
};

const char *py311_canonical_module(const char *module) {
    size_t i;
    if (!module) return "";
    for (i = 0; i < sizeof(A311) / sizeof(A311[0]); i++)
        if (e311(module, A311[i].from)) return A311[i].to;
    return module;
}

int py311_is_builtin(const char *name) {
    size_t i;
    if (!name) return 0;
    for (i = 0; i < sizeof(B311) / sizeof(B311[0]); i++)
        if (e311(name, B311[i])) return 1;
    return 0;
}

int py311_is_stdlib_module(const char *module) {
    const char *m = py311_canonical_module(module);
    size_t i, n;
    if (!m || !*m) return 0;
    n = strlen(m);
    for (i = 0; i < sizeof(M311) / sizeof(M311[0]); i++) {
        size_t k = strlen(M311[i]);
        if (e311(m, M311[i])) return 1;
        /* Submodules such as concurrent.futures or email.message are part of
           the stdlib even though sys.stdlib_module_names lists packages. */
        if (n > k + 1 && m[k] == '.' && !strncmp(m, M311[i], k)) return 1;
    }
    return 0;
}

PyVal *py311_builtin_names(void) {
    PyVal *out = py_list_new(), *s;
    size_t i;
    for (i = 0; i < sizeof(B311) / sizeof(B311[0]); i++) {
        s = py_str(B311[i]);
        py_list_append(out, s);
        py_release(s);
    }
    return out;
}

PyVal *py311_module_names(void) {
    PyVal *out = py_list_new(), *s;
    size_t i;
    for (i = 0; i < sizeof(M311) / sizeof(M311[0]); i++) {
        s = py_str(M311[i]);
        py_list_append(out, s);
        py_release(s);
    }
    return out;
}

/* ------------------------------------------------ builtins not in core tables */

static PyVal *b311_complex(PyVal **a, size_t n, PyVal *kw) {
    PyVal *d = py_dict_new();
    PyVal *real = n > 0 ? a[0] : py_int(0);
    PyVal *imag = n > 1 ? a[1] : (kw ? py_kw_get(kw, "imag") : NULL);
    (void)kw;
    py_dict_put(d, "__name__", py_str("complex"));
    py_dict_put(d, "real", py_float(py_as_float(real)));
    py_dict_put(d, "imag", py_float(imag ? py_as_float(imag) : 0.0));
    return d;
}

static PyVal *b311_build_class(PyVal **a, size_t n, PyVal *kw) {
    const char *name = n > 1 ? py_cstr(a[1]) : "object";
    (void)kw;
    return py_class(py_dict_new(), name ? name : "object");
}

static PyVal *b311_copyright(PyVal **a, size_t n, PyVal *kw) {
    (void)a; (void)n; (void)kw;
    fputs("Python 3.11 (py2c99 compiled mode) - no CPython is embedded.\n", stdout);
    return py_none();
}

static PyVal *b311_credits(PyVal **a, size_t n, PyVal *kw) {
    (void)a; (void)n; (void)kw;
    fputs("Thanks to the Python software foundation and the py2c99 contributors.\n", stdout);
    return py_none();
}

static PyVal *b311_license(PyVal **a, size_t n, PyVal *kw) {
    (void)a; (void)n; (void)kw;
    fputs("py2c99 generated programs are distributed under the PSF license.\n", stdout);
    return py_none();
}

/* Constants and exception classes are values, not functions.  pyrt.c checks
   this table before pyrt_builtins.c so that builtins.__debug__ is True rather
   than the legacy helper function of the same name. */
PyVal *py311_builtin_const(const char *name) {
    size_t i;
    if (!name) return NULL;
    if (e311(name, "None")) return py_none();
    if (e311(name, "True")) return py_bool(1);
    if (e311(name, "False")) return py_bool(0);
    if (e311(name, "Ellipsis")) return py_str("Ellipsis");
    if (e311(name, "NotImplemented")) return py_str("NotImplemented");
    if (e311(name, "__debug__")) return py_bool(1);
    if (e311(name, "__name__") || e311(name, "__package__"))
        return py_str("builtins");
    if (e311(name, "__doc__")) return py_none();
    if (e311(name, "__loader__") || e311(name, "__spec__"))
        return py_str("py2c99");
    for (i = 0; i < sizeof(E311) / sizeof(E311[0]); i++) {
        if (!e311(name, E311[i])) continue;
        {
            PyVal *cls = py_exc_class(name);
            if (cls) return cls;
            return py_class(py_dict_new(), name);
        }
    }
    return NULL;
}

/* Return a new reference for a builtin that pyrt.c / pyrt_builtins.c do not
   provide (the remaining callables). */
PyVal *py311_builtin_get(const char *name) {
    PyVal *constant = py311_builtin_const(name);
    if (constant) return constant;
    if (!name) return NULL;
    if (e311(name, "complex")) return py_func(b311_complex, "complex");
    if (e311(name, "__build_class__"))
        return py_func(b311_build_class, "__build_class__");
    if (e311(name, "copyright")) return py_func(b311_copyright, "copyright");
    if (e311(name, "credits")) return py_func(b311_credits, "credits");
    if (e311(name, "license")) return py_func(b311_license, "license");
    return NULL;
}

/* ----------------------------------------------------------- module constants */

static const char *const FUTURE_FEATURES[] = {
    "nested_scopes", "generators", "division", "absolute_import",
    "with_statement", "print_function", "unicode_literals",
    "generator_stop", "annotations", NULL
};

PyVal *py311_module_const(const char *module, const char *name) {
    size_t i;
    if (!module || !name) return NULL;
    if (e311(module, "__future__")) {
        for (i = 0; FUTURE_FEATURES[i]; i++)
            if (e311(name, FUTURE_FEATURES[i])) return py_str(name);
        if (e311(name, "all_features")) {
            PyVal *out = py_list_new(), *s;
            for (i = 0; FUTURE_FEATURES[i]; i++) {
                s = py_str(FUTURE_FEATURES[i]);
                py_list_append(out, s);
                py_release(s);
            }
            return out;
        }
    }
    if (e311(module, "this") && e311(name, "s")) return py_str("The Zen of Python, by Tim Peters\n\nBeautiful is better than ugly.\nExplicit is better than implicit.\nSimple is better than complex.\nComplex is better than complicated.\nFlat is better than nested.\nSparse is better than dense.\nReadability counts.\nSpecial cases aren't special enough to break the rules.\nAlthough practicality beats purity.\nErrors should never pass silently.\nUnless explicitly silenced.\nIn the face of ambiguity, refuse the temptation to guess.\nThere should be one-- and preferably only one --obvious way to do it.\nAlthough that way may not be obvious at first unless you're Dutch.\nNow is better than never.\nAlthough never is often better than *right* now.\nIf the implementation is hard to explain, it's a bad idea.\nIf the implementation is easy to explain, it may be a good idea.\nNamespaces are one honking great idea -- let's do more of those!");
    if (e311(module, "sys") && e311(name, "stdlib_module_names"))
        return py311_module_names();
    if (e311(module, "sys") && e311(name, "builtin_module_names"))
        return py311_builtin_names();
    return NULL;
}

/* ------------------------------------------------------------- module functions */

static PyVal *m311_url2path(PyVal **a, size_t n, PyVal *kw) {
    const char *s = n ? py_cstr(a[0]) : "";
    char *out, *p;
    PyVal *r;
    size_t i;
    (void)kw;
    out = (char *)malloc((s ? strlen(s) : 0) + 1);
    if (!out) return py_str("");
    p = out;
    for (i = 0; s && s[i]; i++) {
        if (s[i] == '%' && s[i + 1] && s[i + 2]) {
            unsigned int v = 0;
            if (sscanf(s + i + 1, "%2x", &v) == 1) {
                *p++ = (char)(unsigned char)v;
                i += 2;
                continue;
            }
        }
        *p++ = s[i] == '/' ? '\\' : s[i];
    }
    *p = 0;
    r = py_str(out);
    free(out);
    return r;
}

static PyVal *m311_path2url(PyVal **a, size_t n, PyVal *kw) {
    const char *s = n ? py_cstr(a[0]) : "";
    char *out;
    PyVal *r;
    size_t i, j = 0;
    (void)kw;
    out = (char *)malloc((s ? strlen(s) : 0) * 3 + 2);
    if (!out) return py_str("");
    for (i = 0; s && s[i]; i++) {
        if (s[i] == '\\') out[j++] = '/';
        else out[j++] = s[i];
    }
    out[j] = 0;
    r = py_str(out);
    free(out);
    return r;
}

static PyVal *m311_quote(PyVal **a, size_t n, PyVal *kw) {
    const char *s = n ? py_cstr(a[0]) : "";
    char *out;
    PyVal *r;
    size_t i, j = 0;
    (void)kw;
    out = (char *)malloc((s ? strlen(s) : 0) * 2 + 3);
    if (!out) return py_str("");
    out[j++] = '\'';
    for (i = 0; s && s[i]; i++) {
        if (s[i] == '\'') out[j++] = '\\';
        out[j++] = s[i];
    }
    out[j++] = '\'';
    out[j] = 0;
    r = py_str(out);
    free(out);
    return r;
}

static int ip4_octets(const char *s, int *out) {
    int i;
    const char *p = s;
    for (i = 0; i < 4; i++) {
        char *end = NULL;
        long v;
        if (!p || !*p) return 0;
        v = strtol(p, &end, 10);
        if (end == p || v < 0 || v > 255) return 0;
        out[i] = (int)v;
        p = end;
        if (i < 3) {
            if (*p != '.') return 0;
            p++;
        } else if (*p) return 0;
    }
    return 1;
}

static PyVal *m311_ip(PyVal **a, size_t n, PyVal *kw) {
    const char *s = n ? py_cstr(a[0]) : "";
    int o[4];
    PyVal *d;
    (void)kw;
    if (!s || !ip4_octets(s, o)) {
        py_raise(PY_EXC_VALUE, "invalid IPv4 address");
        return py_none();
    }
    d = py_dict_new();
    py_dict_put(d, "__name__", py_str("IPv4Address"));
    py_dict_put(d, "compressed", py_str(s));
    py_dict_put(d, "version", py_int(4));
    return d;
}

static PyVal *m311_noop(PyVal **a, size_t n, PyVal *kw) {
    (void)a; (void)n; (void)kw;
    return py_none();
}

/* gzip uses the pure-C deflate stored blocks already implemented by zlib. */
static uint32_t crc32_311(const unsigned char *s, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        crc ^= s[i];
        for (k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

static PyVal *m311_gzip_compress(PyVal **a, size_t n, PyVal *kw) {
    PyVal *data = n ? a[0] : py_str("");
    PyVal *argv[1], *z, *out;
    const char *s = py_cstr(data);
    size_t len = py_cstr_len(data), zlen, i;
    uint32_t crc;
    unsigned char *buf;
    (void)kw;
    argv[0] = data;
    z = py_module_call("zlib", "compress", argv, 1, NULL);
    zlen = py_cstr_len(z);
    crc = crc32_311((const unsigned char *)s, len);
    buf = (unsigned char *)malloc(10 + zlen + 8);
    if (!buf) { py_release(z); return py_str(""); }
    buf[0] = 0x1f; buf[1] = 0x8b; buf[2] = 8; buf[3] = 0;
    for (i = 4; i < 9; i++) buf[i] = 0;
    buf[9] = 0xff;
    memcpy(buf + 10, py_cstr(z), zlen);
    for (i = 0; i < 4; i++) buf[10 + zlen + i] = (unsigned char)(crc >> (8 * i));
    for (i = 0; i < 4; i++) buf[14 + zlen + i] = (unsigned char)(len >> (8 * i));
    out = py_strn((const char *)buf, 10 + zlen + 8);
    free(buf);
    py_release(z);
    return out;
}

static PyVal *m311_gzip_decompress(PyVal **a, size_t n, PyVal *kw) {
    PyVal *data = n ? a[0] : py_str("");
    const char *s = py_cstr(data);
    size_t len = py_cstr_len(data);
    PyVal *part, *argv[1], *out;
    (void)kw;
    if (len < 18 || (unsigned char)s[0] != 0x1f ||
        (unsigned char)s[1] != 0x8b || (unsigned char)s[2] != 8) {
        py_raise(PY_EXC_VALUE, "invalid gzip stream");
        return py_none();
    }
    part = py_strn(s + 10, len - 18);
    argv[0] = part;
    out = py_module_call("zlib", "decompress", argv, 1, NULL);
    py_release(part);
    return out;
}

static PyVal *m311_capwords(PyVal **a, size_t n, PyVal *kw) {
    PyVal *words, *item = NULL, *out, *sep = py_str(" ");
    PyVal *cap = py_list_new(), *args[1];
    size_t i, count;
    (void)kw;
    words = py_method(n ? a[0] : py_str(""), "split", NULL, 0, NULL);
    count = py_len(words);
    for (i = 0; i < count; i++) {
        item = py_getitem(words, py_int((int64_t)i));
        py_list_append(cap, py_method(item, "capitalize", NULL, 0, NULL));
        py_release(item);
    }
    args[0] = cap;
    out = py_method(sep, "join", args, 1, NULL);
    py_release(cap);
    py_release(sep);
    py_release(words);
    return out;
}

/* ------------------------------------------------------------- argparse */

static PyVal *ap_add_argument(PyVal **a, size_t n, PyVal *kw) {
    PyVal *self = n ? a[0] : py_none();
    PyVal *opts = py_getattr(self, "options");
    PyVal *spec = py_dict_new(), *name = NULL, *action, *type, *defval;
    size_t i;
    for (i = 1; i < n; i++) {
        PyVal *s = py_str_of(a[i]);
        if (py_cstr(s) && py_cstr(s)[0] == '-') name = s;
        else py_release(s);
    }
    if (!name) {
        py_raise(PY_EXC_VALUE, "add_argument() requires an option string");
        py_release(spec); py_release(opts);
        return py_none();
    }
    action = kw ? py_kw_get(kw, "action") : NULL;
    type = kw ? py_kw_get(kw, "type") : NULL;
    defval = kw ? py_kw_get(kw, "default") : NULL;
    py_dict_put(spec, "name", name);
    py_dict_put(spec, "action", action ? action : py_str("store"));
    py_dict_put(spec, "type", type ? type : py_str("str"));
    py_dict_put(spec, "default", defval ? defval
                : (action && py_cstr(action) &&
                   !strcmp(py_cstr(action), "store_true") ? py_bool(0)
                                                          : py_str("")));
    py_list_append(opts, spec);
    py_setattr(self, "options", opts);
    py_release(spec); py_release(opts); py_release(name);
    /* py_kw_get() borrows; py_dict_put() has already retained these. */
    return py_none();
}

static PyVal *ap_parse_args(PyVal **a, size_t n, PyVal *kw) {
    PyVal *self = n ? a[0] : py_none();
    PyVal *opts = py_getattr(self, "options");
    PyVal *argv = (n > 1 && a[1]) ? a[1] : py_sys_argv();
    PyVal *ns = py_dict_new();
    size_t i, j;
    (void)kw;
    for (j = 0; j < py_len(opts); j++) {
        PyVal *spec = py_getitem(opts, py_int((int64_t)j));
        PyVal *name = py_getattr(spec, "name");
        PyVal *def = py_getattr(spec, "default");
        const char *nm = py_cstr(name);
        const char *dest = nm ? nm : "";
        while (*dest == '-') dest++;
        py_dict_put(ns, dest, def);
        py_release(spec); py_release(name); py_release(def);
    }
    for (i = 1; i < py_len(argv); i++) {
        PyVal *tok = py_getitem(argv, py_int((int64_t)i));
        const char *t = py_cstr(tok);
        int matched = 0;
        for (j = 0; j < py_len(opts); j++) {
            PyVal *spec = py_getitem(opts, py_int((int64_t)j));
            PyVal *name = py_getattr(spec, "name");
            const char *nm = py_cstr(name);
            if (nm && t && !strcmp(nm, t)) {
                PyVal *action = py_getattr(spec, "action");
                const char *act = py_cstr(action);
                const char *dest = nm;
                while (*dest == '-') dest++;
                if (act && !strcmp(act, "store_true")) {
                    py_dict_put(ns, dest, py_bool(1));
                } else if (i + 1 < py_len(argv)) {
                    PyVal *val = py_getitem(argv, py_int((int64_t)(++i)));
                    PyVal *tp = py_getattr(spec, "type");
                    PyVal *converted = val;
                    if (tp->type == PY_T_FUNC || tp->type == PY_T_BOUND ||
                        tp->type == PY_T_MODFUNC || tp->type == PY_T_CLASS) {
                        PyVal *av[1];
                        av[0] = val;
                        converted = py_call(tp, av, 1, NULL);
                        py_release(val);
                    } else {
                        const char *tname = py_cstr(tp);
                        if (tname && !strcmp(tname, "int")) {
                            converted = py_builtin_int(&val, 1, NULL);
                            py_release(val);
                        } else if (tname && !strcmp(tname, "float")) {
                            converted = py_builtin_float(&val, 1, NULL);
                            py_release(val);
                        }
                    }
                    py_dict_put(ns, dest, converted);
                    py_release(converted);
                    py_release(tp);
                }
                py_release(action);
                matched = 1;
                py_release(spec); py_release(name);
                break;
            }
            py_release(spec); py_release(name);
        }
        (void)matched;
        py_release(tok);
    }
    py_release(opts);
    return ns;
}

static PyVal *m311_argparse(PyVal **a, size_t n, PyVal *kw) {
    PyVal *methods = py_dict_new(), *cls, *parser;
    (void)a; (void)n; (void)kw;
    py_dict_put(methods, "add_argument", py_func(ap_add_argument, "add_argument"));
    py_dict_put(methods, "parse_args", py_func(ap_parse_args, "parse_args"));
    cls = py_class(methods, "ArgumentParser");
    py_release(methods);
    parser = py_call(cls, NULL, 0, NULL);
    py_release(cls);
    py_setattr(parser, "options", py_list_new());
    return parser;
}

PyVal *py311_module_call(const char *module, const char *name, PyVal **a,
                         size_t n, PyVal *kw, int *handled) {
    if (!handled) return NULL;
    *handled = 0;
    if (!py311_is_stdlib_module(module)) return NULL;
    *handled = 1;
    if (e311(module, "nturl2path")) {
        if (e311(name, "url2pathname") || e311(name, "pathname2url"))
            return e311(name, "url2pathname") ? m311_url2path(a, n, kw)
                                              : m311_path2url(a, n, kw);
    }
    if (e311(module, "pipes") || e311(module, "pipes.quote")) {
        if (e311(name, "quote") || e311(name, "quotepipe") ||
            e311(name, "quotemac") || e311(name, "quoteperl"))
            return m311_quote(a, n, kw);
    }
    if (e311(module, "ipaddress")) {
        if (e311(name, "ip_address") || e311(name, "IPv4Address"))
            return m311_ip(a, n, kw);
    }
    if (e311(module, "tty")) {
        if (e311(name, "setraw") || e311(name, "restore") ||
            e311(name, "cfmakeraw") || e311(name, "cfmakecbreak"))
            return m311_noop(a, n, kw);
    }
    if (e311(module, "antigravity") && e311(name, "main")) {
        fputs("https://xkcd.com/353/\n", stdout);
        return py_none();
    }
    if (e311(module, "gzip")) {
        if (e311(name, "compress")) return m311_gzip_compress(a, n, kw);
        if (e311(name, "decompress")) return m311_gzip_decompress(a, n, kw);
    }
    if (e311(module, "string") && e311(name, "capwords"))
        return m311_capwords(a, n, kw);
    if (e311(module, "argparse")) {
        if (e311(name, "ArgumentParser")) return m311_argparse(a, n, kw);
    }
    {
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "stdlib module '%s' has no compiled implementation of '%s'",
                 module ? module : "", name ? name : "");
        py_raise(PY_EXC_UNSUPPORTED, msg);
    }
    return py_none();
}
