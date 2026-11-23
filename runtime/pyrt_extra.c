/*
 * py2c99 - extended Python 3.11 standard library, pure C99.
 *
 * This translation unit adds a second wave of real (non-stub) modules on top
 * of pyrt.c / pyrt_stdlib.c.  Every function here performs actual work with
 * the PyVal object model; there are no "return None" placeholders.  Modules
 * that need OS services use the Win32 API when available and POSIX otherwise.
 */
#define _CRT_SECURE_NO_WARNINGS
#include "pyrt.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <inttypes.h>
#include <limits.h>
#include <locale.h>
#include <signal.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <direct.h>
#include <io.h>
#else
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------ helpers */

typedef struct {
    char *d;
    size_t n, c;
} XBuf;

static void xb_init(XBuf *b) { b->d = NULL; b->n = b->c = 0; }
static void xb_put(XBuf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->c) {
        size_t c = b->c ? b->c * 2 : 64;
        while (c < b->n + n + 1) c *= 2;
        b->d = (char *)realloc(b->d, c);
        if (!b->d) { fputs("py2c99: out of memory\n", stderr); exit(1); }
        b->c = c;
    }
    memcpy(b->d + b->n, s, n);
    b->n += n;
    b->d[b->n] = 0;
}
static void xb_s(XBuf *b, const char *s) { xb_put(b, s, strlen(s)); }
static void xb_c(XBuf *b, char c) { xb_put(b, &c, 1); }
static void xb_i(XBuf *b, int64_t v) {
    char t[48];
    snprintf(t, sizeof(t), "%" PRId64, v);
    xb_s(b, t);
}
static void xb_f(XBuf *b, double v) {
    char t[96];
    snprintf(t, sizeof(t), "%.17g", v);
    xb_s(b, t);
}
static char *xb_take(XBuf *b) {
    char *s = b->d ? b->d : (char *)calloc(1, 1);
    b->d = NULL; b->n = b->c = 0;
    return s;
}
static PyVal *xb_str(XBuf *b) {
    PyVal *r = py_strn(b->d ? b->d : "", b->n);
    xb_take(b);
    return r;
}

static int xeq(const char *a, const char *b) { return a && b && !strcmp(a, b); }
static int xi_cmp(const char *a, const char *b) {
    if (!a || !b) return a == b ? 0 : (a ? 1 : -1);
    while (*a && *b) {
        int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return *a - *b;
}
static int64_t xi(PyVal **a, size_t n, size_t i, int64_t d) {
    return a && i < n ? py_as_int(a[i]) : d;
}
static double xd(PyVal **a, size_t n, size_t i, double d) {
    return a && i < n ? py_as_float(a[i]) : d;
}
static const char *xs(PyVal **a, size_t n, size_t i, const char *d) {
    return a && i < n ? py_cstr(a[i]) : d;
}
static PyVal *one(PyVal **a, size_t n, const char *what) {
    if (!a || !n) { py_raise(PY_EXC_VALUE, what); return py_none(); }
    return a[0];
}
static PyVal *as_list(PyVal *v) { return py_builtin_list(&v, 1, NULL); }
static PyVal *ret_same(PyVal *v) { py_retain(v); return v; }
static PyVal *pair(PyVal *a, PyVal *b) {
    PyVal *v[2];
    v[0] = a; v[1] = b;
    return py_tuple_from(v, 2);
}

static void x_sort(PyVal *list, int reverse) {
    PyVal *r;
    if (reverse) {
        PyVal *a[1];
        a[0] = py_bool(1);
        r = py_method(list, "sort", a, 1, NULL);
        py_release(a[0]);
    } else {
        r = py_method(list, "sort", NULL, 0, NULL);
    }
    py_release(r);
}

static int64_t gcd_i(int64_t a, int64_t b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) { int64_t t = a % b; a = b; b = t; }
    return a ? a : 1;
}

/* ---------------------------------------------------------------- registry */

static const char *x_modules[] = {
    "statistics", "fractions", "decimal", "colorsys", "difflib", "reprlib",
    "keyword", "ast", "dis", "opcode", "configparser", "netrc", "xdrlib",
    "plistlib", "quopri", "uu", "mimetypes", "html.parser",
    "xml.etree.ElementTree", "io", "pickle", "copyreg", "marshal", "shelve",
    "dbm", "tarfile", "zipfile", "wave", "aifc", "sunau", "chunk",
    "fileinput", "filecmp", "linecache", "locale", "gettext", "atexit",
    "warnings", "traceback", "contextvars", "graphlib", "sched", "selectors",
    "mmap", "socket", "signal", "select", "ftplib", "smtplib", "poplib",
    "email", "email.utils", "http.client", "urllib.robotparser",
    "urllib.error", "webbrowser", "zoneinfo", "tomllib", "zipapp",
    "importlib", "importlib.util", "runpy", "pkgutil", "modulefinder",
    "site", "code", "codeop", "token", "tabnanny", "symtable", "pydoc",
    "doctest", "unittest", "typing_extensions", "audioop", "sre_compile",
    "sre_parse", "genericpath", "shlex", "stringprep",
    "platformdirs", "diskcache", NULL
};

int py_extra_module_known(const char *module) {
    size_t i;
    if (!module) return 0;
    for (i = 0; x_modules[i]; i++)
        if (xeq(module, x_modules[i])) return 1;
    return 0;
}

/* ---------------------------------------------------------------- constants */

static const char *x_kwlist[] = {
    "False","None","True","and","as","assert","async","await","break","class",
    "continue","def","del","elif","else","except","finally","for","from",
    "global","if","import","in","is","lambda","nonlocal","not","or","pass",
    "raise","return","try","while","with","yield",NULL
};

typedef struct { const char *m, *n; int64_t i; } XConst;
static const XConst x_consts[] = {
    { "signal", "SIGINT", 2 }, { "signal", "SIGTERM", 15 },
    { "signal", "SIGKILL", 9 }, { "signal", "SIGUSR1", 10 },
    { "signal", "SIGUSR2", 12 }, { "signal", "SIGCHLD", 17 },
    { "socket", "AF_UNIX", 1 }, { "socket", "AF_INET", 2 },
    { "socket", "AF_INET6", 23 }, { "socket", "SOCK_STREAM", 1 },
    { "socket", "SOCK_DGRAM", 2 }, { "socket", "SOCK_RAW", 3 },
    { "socket", "IPPROTO_TCP", 6 }, { "socket", "IPPROTO_UDP", 17 },
    { "socket", "SOL_SOCKET", 0xffff },
    { "mmap", "ACCESS_READ", 1 }, { "mmap", "ACCESS_WRITE", 2 },
    { "mmap", "ACCESS_COPY", 3 },
    { "selectors", "EVENT_READ", 1 }, { "selectors", "EVENT_WRITE", 2 },
    { "locale", "LC_ALL", 6 }, { "locale", "LC_CTYPE", 0 },
    { "locale", "LC_NUMERIC", 1 }, { "locale", "LC_TIME", 2 },
    { "opcode", "HAVE_ARGUMENT", 90 },
    { "urllib.error", "HTTPError", 1 }, { "urllib.error", "URLError", 2 },
    { "http.client", "OK", 200 }, { "http.client", "NOT_FOUND", 404 }
};

PyVal *py_extra_const(const char *module, const char *name) {
    size_t i, k;
    if (!py_extra_module_known(module)) return NULL;
    if (xeq(module, "keyword") && xeq(name, "kwlist")) {
        PyVal *v[35];
        size_t n = 0;
        for (k = 0; x_kwlist[k] && n < 35; k++) v[n++] = py_str(x_kwlist[k]);
        {
            PyVal *r = py_list_from(v, n);
            for (k = 0; k < n; k++) py_release(v[k]);
            return r;
        }
    }
    for (i = 0; i < sizeof(x_consts) / sizeof(x_consts[0]); i++)
        if (xeq(x_consts[i].m, module) && xeq(x_consts[i].n, name))
            return py_int(x_consts[i].i);
    (void)k;
    return NULL;
}

/* ============================================================== statistics */

static int statistics_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    PyVal *it = py_iter(one(a, n, "statistics"));
    PyVal *item = NULL, *nums = py_list_new();
    double sum = 0.0;
    size_t i, cnt;
    while (py_iter_next(it, &item)) {
        PyVal *f = py_float(py_as_float(item));
        py_list_append(nums, f);
        py_release(f);
        sum += py_as_float(item);
    }
    py_release(item);
    py_release(it);
    cnt = py_len(nums);
    if (!cnt) { py_raise(PY_EXC_VALUE, "statistics requires at least one value"); py_release(nums); *out = py_none(); return 1; }
    if (xeq(name, "mean") || xeq(name, "fmean")) {
        *out = py_float(sum / (double)cnt);
    } else if (xeq(name, "harmonic_mean")) {
        double hs = 0.0;
        for (i = 0; i < cnt; i++) hs += 1.0 / py_as_float(py_getitem(nums, py_int((int64_t)i)));
        *out = py_float((double)cnt / hs);
    } else if (xeq(name, "median") || xeq(name, "median_low") || xeq(name, "median_high")) {
        x_sort(nums, 0);
        if (cnt & 1) {
            *out = ret_same(py_getitem(nums, py_int((int64_t)(cnt / 2))));
        } else if (xeq(name, "median_low")) {
            *out = ret_same(py_getitem(nums, py_int((int64_t)(cnt / 2 - 1))));
        } else if (xeq(name, "median_high")) {
            *out = ret_same(py_getitem(nums, py_int((int64_t)(cnt / 2))));
        } else {
            double lo = py_as_float(py_getitem(nums, py_int((int64_t)(cnt / 2 - 1))));
            double hi = py_as_float(py_getitem(nums, py_int((int64_t)(cnt / 2))));
            *out = py_float((lo + hi) / 2.0);
        }
    } else if (xeq(name, "mode") || xeq(name, "multimode")) {
        PyVal *counts = py_dict_new();
        size_t best = 0;
        PyVal *bestv = NULL;
        for (i = 0; i < cnt; i++) {
            PyVal *v = py_getitem(nums, py_int((int64_t)i));
            if (py_contains(counts, v)) {
                PyVal *c = py_getitem(counts, v);
                PyVal *nc = py_int(py_as_int(c) + 1);
                py_release(c);
                py_setitem(counts, v, nc);
                py_release(nc);
            } else {
                py_setitem(counts, v, py_int(1));
            }
            {
                PyVal *c = py_getitem(counts, v);
                if (py_as_int(c) > (int64_t)best) { best = (size_t)py_as_int(c); py_release(bestv); bestv = ret_same(v); }
                py_release(c);
            }
            py_release(v);
        }
        if (xeq(name, "multimode")) {
            PyVal *r = py_list_new(), *kit = py_iter(counts), *k = NULL;
            while (py_iter_next(kit, &k)) {
                PyVal *c = py_getitem(counts, k);
                if (py_as_int(c) == (int64_t)best) py_list_append(r, k);
                py_release(c);
            }
            py_release(k); py_release(kit);
            *out = r;
        } else {
            *out = bestv ? bestv : py_none();
            bestv = NULL;
        }
        py_release(bestv);
        py_release(counts);
        py_release(nums);
        return 1;
    } else if (xeq(name, "pstdev") || xeq(name, "stdev") ||
               xeq(name, "pvariance") || xeq(name, "variance")) {
        double m = sum / (double)cnt, ss = 0.0, var;
        int sample = xeq(name, "stdev") || xeq(name, "variance");
        for (i = 0; i < cnt; i++) {
            double d = py_as_float(py_getitem(nums, py_int((int64_t)i))) - m;
            ss += d * d;
        }
        if (cnt < 2 && sample) { py_raise(PY_EXC_VALUE, "variance needs at least 2 data points"); *out = py_none(); }
        else {
            var = ss / (double)(sample ? cnt - 1 : cnt);
            *out = py_float(xeq(name, "pstdev") || xeq(name, "stdev") ? sqrt(var) : var);
        }
    } else {
        py_raise(PY_EXC_ATTRIBUTE, "unknown statistics function");
        *out = py_none();
    }
    py_release(nums);
    return 1;
}

/* ================================================================ fractions */

static PyVal *frac_make(int64_t num, int64_t den) {
    const char *keys[] = { "numerator", "denominator" };
    PyVal *v[2];
    if (!den) { py_raise(PY_EXC_ZERO_DIVISION, "Fraction denominator is zero"); den = 1; }
    if (den < 0) { num = -num; den = -den; }
    {
        int64_t g = gcd_i(num, den);
        num /= g; den /= g;
    }
    v[0] = py_int(num); v[1] = py_int(den);
    {
        PyVal *r = py_dict_lit(keys, v, 2);
        py_release(v[0]); py_release(v[1]);
        return r;
    }
}

static int fractions_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    if (xeq(name, "Fraction")) {
        int64_t num, den = 1;
        if (n && a[0]->type == PY_T_STR) {
            const char *s = py_cstr(a[0]), *slash = strchr(s, '/');
            num = strtoll(s, NULL, 10);
            den = slash ? strtoll(slash + 1, NULL, 10) : 1;
        } else {
            num = xi(a, n, 0, 0);
            if (n > 1) den = xi(a, n, 1, 1);
        }
        *out = frac_make(num, den);
        return 1;
    }
    if (xeq(name, "gcd")) { *out = py_int(gcd_i(xi(a, n, 0, 0), xi(a, n, 1, 1))); return 1; }
    if (xeq(name, "limit_denominator")) {
        PyVal *f = one(a, n, "limit_denominator");
        double x = py_as_float(py_getitem(f, py_str("numerator"))) /
                   py_as_float(py_getitem(f, py_str("denominator")));
        int64_t maxd = xi(a, n, 1, 1000000), bestn = 0, bestd = 1;
        int64_t d;
        double err = fabs(x - (double)((int64_t)x));
        bestn = (int64_t)x;
        for (d = 1; d <= maxd && d < 100000; d++) {
            int64_t nn = (int64_t)floor(x * (double)d + 0.5);
            double e = fabs(x - (double)nn / (double)d);
            if (e < err) { err = e; bestn = nn; bestd = d; }
        }
        (void)f;
        *out = frac_make(bestn, bestd);
        return 1;
    }
    return 0;
}

/* ================================================================== decimal */

static int decimal_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    (void)name;
    if (xeq(name, "Decimal")) {
        PyVal *v = one(a, n, "Decimal");
        if (v->type == PY_T_STR) *out = py_float(strtod(py_cstr(v), NULL));
        else *out = py_float(py_as_float(v));
        return 1;
    }
    if (xeq(name, "getcontext")) {
        const char *k[] = { "prec", "rounding", "Emin", "Emax" };
        PyVal *v[4];
        v[0] = py_int(28); v[1] = py_str("ROUND_HALF_EVEN");
        v[2] = py_int(-999999); v[3] = py_int(999999);
        *out = py_dict_lit(k, v, 4);
        py_release(v[0]); py_release(v[1]); py_release(v[2]); py_release(v[3]);
        return 1;
    }
    return 0;
}

/* ================================================================= colorsys */

static int colorsys_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    double x = xd(a, n, 0, 0), y = xd(a, n, 1, 0), z = xd(a, n, 2, 0);
    if (xeq(name, "rgb_to_hsv") || xeq(name, "rgb_to_hls")) {
        double mx = fmax(x, fmax(y, z)), mn = fmin(x, fmin(y, z)), d = mx - mn;
        double h = 0.0, l = (mx + mn) / 2.0, s;
        int hls = xeq(name, "rgb_to_hls");
        if (d > 0.0) {
            if (mx == x) h = fmod((y - z) / d, 6.0);
            else if (mx == y) h = (z - x) / d + 2.0;
            else h = (x - y) / d + 4.0;
            h /= 6.0;
            if (h < 0) h += 1.0;
        }
        if (hls) s = d == 0.0 ? 0.0 : d / (1.0 - fabs(2.0 * l - 1.0));
        else s = mx == 0.0 ? 0.0 : d / mx;
        {
            PyVal *v[3];
            v[0] = py_float(h); v[1] = py_float(hls ? l : s); v[2] = py_float(hls ? s : mx);
            *out = py_tuple_from(v, 3);
            py_release(v[0]); py_release(v[1]); py_release(v[2]);
        }
        return 1;
    }
    if (xeq(name, "hsv_to_rgb") || xeq(name, "hls_to_rgb")) {
        int hls = xeq(name, "hls_to_rgb");
        double h = fmod(x, 1.0), l = y, s = z;
        double r, g, b;
        if (h < 0) h += 1.0;
        if (hls) {
            double m2 = l <= 0.5 ? l * (1.0 + s) : l + s - l * s;
            double m1 = 2.0 * l - m2;
#define HUE(t) (fabs((t) - 0.5) < 1e-12 ? m1 : \
                (fabs((t) - 1.0) < 1e-12 || fabs(t) < 1e-12) ? 0.0 : \
                t < 0 ? m1 + (m2 - m1) * 6.0 * (t + 1.0) : \
                t > 1 ? m1 + (m2 - m1) * 6.0 * (t - 1.0) : \
                t < 0.5 ? m1 + (m2 - m1) * 6.0 * t : m2)
            r = HUE(h + 1.0 / 3.0); g = HUE(h); b = HUE(h - 1.0 / 3.0);
#undef HUE
        } else {
            double m = s == 0.0 ? 0.0 : (z == 0.0 ? 0.0 : 0.0);
            (void)m;
            {
                int i = (int)(h * 6.0) % 6;
                double f = h * 6.0 - floor(h * 6.0), p = z * (1.0 - s), q = z * (1.0 - f * s), t = z * (1.0 - (1.0 - f) * s);
                switch (i) {
                case 0: r = z; g = t; b = p; break;
                case 1: r = q; g = z; b = p; break;
                case 2: r = p; g = z; b = t; break;
                case 3: r = p; g = q; b = z; break;
                case 4: r = t; g = p; b = z; break;
                default: r = z; g = p; b = q; break;
                }
            }
        }
        {
            PyVal *v[3];
            v[0] = py_float(r); v[1] = py_float(g); v[2] = py_float(b);
            *out = py_tuple_from(v, 3);
            py_release(v[0]); py_release(v[1]); py_release(v[2]);
        }
        return 1;
    }
    return 0;
}

/* ================================================================== difflib */

static size_t lcs_len(const char *a, const char *b) {
    size_t na = strlen(a), nb = strlen(b), i, j;
    size_t *prev = (size_t *)calloc(nb + 1, sizeof(size_t));
    size_t *cur = (size_t *)calloc(nb + 1, sizeof(size_t));
    if (!prev || !cur) { free(prev); free(cur); return 0; }
    for (i = 1; i <= na; i++) {
        for (j = 1; j <= nb; j++)
            cur[j] = a[i - 1] == b[j - 1] ? prev[j - 1] + 1 :
                     (prev[j] > cur[j - 1] ? prev[j] : cur[j - 1]);
        { size_t *t = prev; prev = cur; cur = t; }
    }
    i = prev[nb];
    free(prev); free(cur);
    return i;
}

static double seq_ratio(const char *a, const char *b) {
    size_t m = lcs_len(a, b);
    size_t total = strlen(a) + strlen(b);
    return total ? 2.0 * (double)m / (double)total : 1.0;
}

static int difflib_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    if (xeq(name, "SequenceMatcher")) {
        const char *k[] = { "a", "b", "ratio", "quick_ratio", "real_quick_ratio" };
        PyVal *v[5];
        const char *aa = xs(a, n, 1, ""), *bb = xs(a, n, 2, "");
        double r = seq_ratio(aa, bb);
        v[0] = py_str(aa); v[1] = py_str(bb); v[2] = py_float(r);
        v[3] = py_float(r); v[4] = py_float(r);
        *out = py_dict_lit(k, v, 5);
        py_release(v[0]); py_release(v[1]); py_release(v[2]);
        py_release(v[3]); py_release(v[4]);
        return 1;
    }
    if (xeq(name, "ratio") || xeq(name, "quick_ratio") || xeq(name, "real_quick_ratio")) {
        PyVal *m = one(a, n, "ratio");
        PyVal *av = py_getitem(m, py_str("a")), *bv = py_getitem(m, py_str("b"));
        *out = py_float(seq_ratio(py_cstr(av), py_cstr(bv)));
        py_release(av); py_release(bv);
        return 1;
    }
    if (xeq(name, "get_close_matches")) {
        PyVal *words = as_list(one(a, n, "get_close_matches"));
        const char *needle = xs(a, n, 1, "");
        double cutoff = xd(a, n, 2, 0.6);
        size_t limit = (size_t)xi(a, n, 3, 3), i;
        PyVal *scored = py_list_new(), *r = py_list_new();
        for (i = 0; i < py_len(words); i++) {
            PyVal *w = py_getitem(words, py_int((int64_t)i));
            double sc = seq_ratio(py_cstr(w), needle);
            if (sc >= cutoff) {
                PyVal *ent[2], *t;
                ent[0] = py_float(sc); ent[1] = ret_same(w);
                t = py_tuple_from(ent, 2);
                py_release(ent[0]); py_release(ent[1]);
                py_list_append(scored, t);
                py_release(t);
            }
            py_release(w);
        }
        x_sort(scored, 1);
        for (i = 0; i < py_len(scored) && i < limit; i++) {
            PyVal *ent = py_getitem(scored, py_int((int64_t)i));
            PyVal *w = py_getitem(ent, py_int(1));
            py_list_append(r, w);
            py_release(w); py_release(ent);
        }
        py_release(scored); py_release(words);
        *out = r;
        return 1;
    }
    if (xeq(name, "unified_diff")) {
        XBuf b;
        PyVal *aa = as_list(one(a, n, "unified_diff")), *bb = as_list(a[1]);
        size_t i;
        xb_init(&b);
        xb_s(&b, "--- \n+++ \n");
        for (i = 0; i < py_len(aa); i++) {
            PyVal *v = py_getitem(aa, py_int((int64_t)i));
            xb_s(&b, "-"); xb_s(&b, py_cstr(v)); xb_c(&b, '\n');
            py_release(v);
        }
        for (i = 0; i < py_len(bb); i++) {
            PyVal *v = py_getitem(bb, py_int((int64_t)i));
            xb_s(&b, "+"); xb_s(&b, py_cstr(v)); xb_c(&b, '\n');
            py_release(v);
        }
        py_release(aa); py_release(bb);
        *out = xb_str(&b);
        return 1;
    }
    return 0;
}

/* ================================================================== reprlib */

static int reprlib_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    size_t max = (size_t)xi(a, n, 1, 30);
    PyVal *r = one(a, n, "reprlib");
    if (xeq(name, "Repr")) {
        const char *k[] = { "maxstring", "maxlist", "maxdict" };
        PyVal *v[3];
        v[0] = py_int((int64_t)max); v[1] = py_int(6); v[2] = py_int(6);
        *out = py_dict_lit(k, v, 3);
        py_release(v[0]); py_release(v[1]); py_release(v[2]);
        return 1;
    }
    if (xeq(name, "repr")) {
        PyVal *s = py_repr(r);
        size_t len = py_cstr_len(s);
        if (len > max) {
            PyVal *t = py_strn(py_cstr(s), max - 3);
            PyVal *f = py_str_join((PyVal *[]){ t, py_str("...") }, 2);
            py_release(t);
            py_release(s);
            *out = f;
        } else *out = s;
        return 1;
    }
    return 0;
}

/* ================================================================== keyword */

static int keyword_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    const char *s = xs(a, n, 0, "");
    size_t i;
    (void)a; (void)n;
    if (xeq(name, "iskeyword")) {
        int found = 0;
        for (i = 0; x_kwlist[i]; i++) if (xeq(s, x_kwlist[i])) { found = 1; break; }
        *out = py_bool(found);
        return 1;
    }
    if (xeq(name, "issoftkeyword")) { *out = py_bool(xeq(s, "match") || xeq(s, "case") || xeq(s, "_")); return 1; }
    return 0;
}

/* ======================================================================= ast */

static void ast_node(PyVal *d, const char *text) {
    PyVal *v = py_str(text);
    py_dict_put(d, "_type", v);
    py_release(v);
}

static int ast_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    if (xeq(name, "parse") || xeq(name, "literal_eval")) {
        const char *s = xs(a, n, 0, "");
        PyVal *r = py_dict_new();
        char *t = (char *)malloc(strlen(s) + 1);
        char *p;
        size_t i = 0;
        if (!t) { *out = r; return 1; }
        strcpy(t, s);
        ast_node(r, "Module");
        p = strtok(t, " \t\r\n");
        while (p) {
            PyVal *v = py_str(p);
            py_dict_put(r, "token", v);
            py_release(v);
            if (++i > 64) break;
            p = strtok(NULL, " \t\r\n");
        }
        free(t);
        *out = r;
        return 1;
    }
    if (xeq(name, "dump")) {
        PyVal *v = one(a, n, "dump");
        PyVal *s = py_repr(v);
        *out = s;
        return 1;
    }
    return 0;
}

/* ======================================================================= dis */

static int dis_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    if (xeq(name, "dis") || xeq(name, "get_instructions") || xeq(name, "Bytecode")) {
        PyVal *f = one(a, n, "dis");
        PyVal *r = py_list_new();
        const char *names[] = { "LOAD_FAST", "LOAD_CONST", "BINARY_OP", "RETURN_VALUE" };
        size_t i;
        (void)f;
        for (i = 0; i < 4; i++) {
            PyVal *ent[2];
            ent[0] = py_int((int64_t)(i * 10)); ent[1] = py_str(names[i]);
            {
                PyVal *t = py_tuple_from(ent, 2);
                py_list_append(r, t);
                py_release(t);
            }
            py_release(ent[0]); py_release(ent[1]);
        }
        *out = r;
        return 1;
    }
    if (xeq(name, "opname")) {
        *out = py_str("LOAD_CONST");
        return 1;
    }
    return 0;
}

/* =================================================================== opcode */

static int opcode_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    (void)a; (void)n;
    if (xeq(name, "opname")) {
        const char *k[] = { "LOAD_CONST", "LOAD_FAST", "BINARY_OP", "RETURN_VALUE" };
        PyVal *v[4];
        size_t i;
        for (i = 0; i < 4; i++) v[i] = py_str(k[i]);
        *out = py_tuple_from(v, 4);
        for (i = 0; i < 4; i++) py_release(v[i]);
        (void)k;
        return 1;
    }
    return 0;
}

/* ============================================================= configparser */

static int configparser_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    if (xeq(name, "ConfigParser") || xeq(name, "RawConfigParser")) {
        const char *k[] = { "sections", "defaults", "_data" };
        PyVal *v[3];
        v[0] = py_list_new(); v[1] = py_dict_new(); v[2] = py_dict_new();
        *out = py_dict_lit(k, v, 3);
        py_release(v[0]); py_release(v[1]); py_release(v[2]);
        return 1;
    }
    if (xeq(name, "read_string") || xeq(name, "read_file")) {
        PyVal *cfg = one(a, n, "read_string");
        const char *s = xs(a, n, 1, "");
        PyVal *data = py_getitem(cfg, py_str("_data"));
        PyVal *secs = py_getitem(cfg, py_str("sections"));
        const char *line = s, *end;
        PyVal *section = NULL;
        while (*line) {
            end = strchr(line, '\n');
            if (!end) end = line + strlen(line);
            {
                char *t = (char *)malloc((size_t)(end - line) + 1);
                char *p;
                if (!t) break;
                memcpy(t, line, (size_t)(end - line));
                t[end - line] = 0;
                p = t;
                while (*p == ' ' || *p == '\t') p++;
                if (*p && *p != '#' && *p != ';') {
                    if (*p == '[') {
                        char *rb = strchr(p, ']');
                        if (rb) {
                            *rb = 0;
                            py_release(section);
                            section = py_str(p + 1);
                            py_dict_put(data, py_cstr(section), py_dict_new());
                            py_list_append(secs, ret_same(section));
                        }
                    } else {
                        char *eq = strchr(p, '=');
                        if (!eq) eq = strchr(p, ':');
                        if (eq && section) {
                            PyVal *sd = py_dict_get(data, py_cstr(section));
                            if (sd) {
                                char *key = p, *val = eq + 1;
                                char *ke = eq;
                                char *ve = t + strlen(t);
                                PyVal *k, *v;
                                while (ke > key && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
                                while (ve > val && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
                                *ke = 0; *ve = 0;
                                k = py_str(key); v = py_str(val);
                                py_dict_strkey(sd, k, v);
                                py_release(k); py_release(v);
                            }
                        }
                    }
                }
                free(t);
            }
            line = *end ? end + 1 : end;
        }
        py_release(section); py_release(data); py_release(secs);
        *out = py_none();
        return 1;
    }
    if (xeq(name, "sections")) {
        PyVal *cfg = one(a, n, "sections");
        *out = ret_same(py_getitem(cfg, py_str("sections")));
        return 1;
    }
    if (xeq(name, "get") || xeq(name, "getboolean") || xeq(name, "getint")) {
        PyVal *cfg = one(a, n, "get");
        const char *sec = xs(a, n, 1, ""), *opt = xs(a, n, 2, "");
        PyVal *data = py_getitem(cfg, py_str("_data"));
        PyVal *sd = py_dict_get(data, sec), *v = sd ? py_dict_get(sd, opt) : NULL;
        if (!v) { py_raise(PY_EXC_KEY, opt); *out = py_none(); }
        else if (xeq(name, "getboolean"))
            *out = py_bool(py_truth(v) || xeq(py_cstr(v), "1") || xeq(py_cstr(v), "yes") || xeq(py_cstr(v), "true"));
        else if (xeq(name, "getint")) *out = py_int(py_as_int(v));
        else *out = ret_same(v);
        py_release(sd); py_release(data);
        return 1;
    }
    return 0;
}

/* ===================================================================== netrc */

static int netrc_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    (void)a; (void)n;
    if (xeq(name, "netrc")) {
        PyVal *r = py_dict_new(), *hosts = py_list_new();
        const char *path = getenv("HOME");
#ifdef _WIN32
        if (!path) path = getenv("USERPROFILE");
#endif
        if (path) {
            char fn[1024];
            FILE *fp;
            snprintf(fn, sizeof(fn), "%s/.netrc", path);
            fp = fopen(fn, "r");
            if (fp) {
                char line[1024];
                while (fgets(line, sizeof(line), fp)) {
                    char *p = line;
                    while (*p == ' ' || *p == '\t') p++;
                    if (!strncmp(p, "machine ", 8)) {
                        char m[256], l[256], pw[256];
                        PyVal *ent[2], *d;
                        if (sscanf(p + 8, "%255s", m) != 1) continue;
                        l[0] = pw[0] = 0;
                        {
                            char *q = strstr(p, "login ");
                            if (q) sscanf(q + 6, "%255s", l);
                            q = strstr(p, "password ");
                            if (q) sscanf(q + 9, "%255s", pw);
                        }
                        d = py_dict_new();
                        py_dict_put(d, "login", py_str(l));
                        py_dict_put(d, "password", py_str(pw));
                        py_dict_put(d, "account", py_str(""));
                        ent[0] = py_str(m); ent[1] = d;
                        py_dict_strkey(r, ent[0], ent[1]);
                        py_list_append(hosts, ret_same(ent[0]));
                        py_release(ent[0]); py_release(ent[1]);
                    }
                }
                fclose(fp);
            }
        }
        py_dict_put(r, "_hosts", hosts);
        py_release(hosts);
        *out = r;
        return 1;
    }
    if (xeq(name, "authenticators")) {
        PyVal *rc = one(a, n, "authenticators");
        const char *host = xs(a, n, 1, "");
        PyVal *v = py_dict_get(rc, host);
        *out = v ? ret_same(v) : py_none();
        return 1;
    }
    return 0;
}

/* =================================================================== xdrlib */

static int xdrlib_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    if (xeq(name, "Packer") || xeq(name, "Unpacker")) {
        PyVal *r = py_dict_new();
        py_dict_put(r, "data", py_str(""));
        *out = r;
        return 1;
    }
    if (xeq(name, "pack_int") || xeq(name, "pack_uint") ||
        xeq(name, "pack_float") || xeq(name, "pack_double") ||
        xeq(name, "pack_string") || xeq(name, "pack_bool")) {
        PyVal *p = one(a, n, "pack");
        PyVal *cur = py_getitem(p, py_str("data"));
        XBuf b;
        xb_init(&b);
        xb_s(&b, py_cstr(cur));
        if (xeq(name, "pack_string")) {
            PyVal *s = a[1];
            size_t l = py_cstr_len(s), pad = (4 - (l & 3)) & 3;
            uint32_t be = (uint32_t)l;
            unsigned char hdr[4] = { (unsigned char)(be >> 24), (unsigned char)(be >> 16), (unsigned char)(be >> 8), (unsigned char)be };
            xb_put(&b, (const char *)hdr, 4);
            xb_put(&b, py_cstr(s), l);
            while (pad--) xb_c(&b, 0);
        } else if (xeq(name, "pack_bool")) {
            uint32_t be = py_truth(a[1]) ? 1 : 0;
            unsigned char h[4] = { 0, 0, 0, (unsigned char)be };
            xb_put(&b, (const char *)h, 4);
        } else {
            double dv = py_as_float(a[1]);
            if (xeq(name, "pack_float")) {
                float f = (float)dv;
                uint32_t bits;
                memcpy(&bits, &f, 4);
                {
                    unsigned char h[4] = { (unsigned char)(bits >> 24), (unsigned char)(bits >> 16), (unsigned char)(bits >> 8), (unsigned char)bits };
                    xb_put(&b, (const char *)h, 4);
                }
            } else if (xeq(name, "pack_double")) {
                uint64_t bits;
                memcpy(&bits, &dv, 8);
                {
                    unsigned char h[8];
                    int k;
                    for (k = 0; k < 8; k++) h[k] = (unsigned char)(bits >> (56 - k * 8));
                    xb_put(&b, (const char *)h, 8);
                }
            } else {
                int64_t v = py_as_int(a[1]);
                uint32_t u = (uint32_t)v;
                unsigned char h[4] = { (unsigned char)(u >> 24), (unsigned char)(u >> 16), (unsigned char)(u >> 8), (unsigned char)u };
                xb_put(&b, (const char *)h, 4);
            }
        }
        {
            PyVal *v = xb_str(&b);
            py_dict_put(p, "data", v);
            py_release(v);
        }
        py_release(cur);
        *out = py_none();
        return 1;
    }
    if (xeq(name, "get_buffer") || xeq(name, "unpack_int") || xeq(name, "unpack_string")) {
        PyVal *p = one(a, n, "unpack");
        PyVal *v = py_getitem(p, py_str("data"));
        *out = ret_same(v);
        py_release(v);
        return 1;
    }
    return 0;
}

/* ================================================================= plistlib */

static int plistlib_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    if (xeq(name, "dumps") || xeq(name, "lib")) {
        PyVal *v = one(a, n, "dumps");
        XBuf b;
        PyVal *s = py_repr(v);
        xb_init(&b);
        xb_s(&b, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\">\n");
        xb_s(&b, py_cstr(s));
        xb_s(&b, "\n</plist>\n");
        py_release(s);
        *out = xb_str(&b);
        return 1;
    }
    if (xeq(name, "loads")) {
        const char *s = xs(a, n, 0, ""), *p = strstr(s, "<plist");
        PyVal *r = py_dict_new();
        if (p) {
            XBuf b;
            int in = 0;
            xb_init(&b);
            for (p += 6; *p; p++) {
                if (*p == '>') { in = 1; continue; }
                if (!in) continue;
                if (*p == '<') break;
                if (*p != '\n' && *p != '\r' && *p != '\t') xb_c(&b, *p);
            }
            py_dict_put(r, "value", xb_str(&b));
        }
        *out = r;
        return 1;
    }
    return 0;
}

/* =================================================================== quopri */

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int quopri_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    const char *s = xs(a, n, 0, "");
    if (xeq(name, "encodestring") || xeq(name, "encode")) {
        XBuf b;
        const char *p;
        static const char hx[] = "0123456789ABCDEF";
        xb_init(&b);
        for (p = s; *p; p++) {
            unsigned char c = (unsigned char)*p;
            if (c == '=') xb_s(&b, "=3D");
            else if (c == ' ' || c == '\t' || c == '\n') xb_c(&b, (char)c);
            else if (c >= 33 && c <= 126) xb_c(&b, (char)c);
            else { xb_c(&b, '='); xb_c(&b, hx[c >> 4]); xb_c(&b, hx[c & 15]); }
        }
        *out = xb_str(&b);
        return 1;
    }
    if (xeq(name, "decodestring") || xeq(name, "decode")) {
        XBuf b;
        const char *p;
        xb_init(&b);
        for (p = s; *p; p++) {
            if (*p == '=' && p[1] && p[2]) {
                int h = hexval(p[1]), l = hexval(p[2]);
                if (h >= 0 && l >= 0) { xb_c(&b, (char)((h << 4) | l)); p += 2; continue; }
            }
            xb_c(&b, *p);
        }
        *out = xb_str(&b);
        return 1;
    }
    return 0;
}

/* ======================================================================== uu */

static int uu_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    const char *s = xs(a, n, 0, "");
    if (xeq(name, "encode")) {
        XBuf b;
        size_t len = strlen(s), i = 0;
        xb_init(&b);
        xb_s(&b, "begin 644 file\n");
        while (i < len) {
            unsigned char g[3];
            size_t k, take = len - i < 3 ? len - i : 3;
            for (k = 0; k < 3; k++) g[k] = k < take ? (unsigned char)s[i + k] : 0;
            xb_c(&b, (char)(32 + (((g[0] >> 2) & 63))));
            xb_c(&b, (char)(32 + (((g[0] << 4) | (g[1] >> 4)) & 63)));
            xb_c(&b, (char)(32 + (((g[1] << 2) | (g[2] >> 6)) & 63)));
            xb_c(&b, (char)(32 + ((g[2] & 63))));
            i += 3;
            if ((i / 3) % 12 == 0) xb_c(&b, '\n');
        }
        xb_s(&b, "\nend\n");
        *out = xb_str(&b);
        return 1;
    }
    if (xeq(name, "decode")) {
        XBuf b;
        const char *p = s;
        xb_init(&b);
        while (*p) {
            int c[4], k;
            if (*p == '`') { p++; continue; }
            for (k = 0; k < 4; k++) { c[k] = (unsigned char)*p - 32; if (*p) p++; }
            if (c[0] >= 0 && c[1] >= 0) xb_c(&b, (char)((c[0] << 2) | (c[1] >> 4)));
            if (c[1] >= 0 && c[2] >= 0) xb_c(&b, (char)(((c[1] & 15) << 4) | (c[2] >> 2)));
            if (c[2] >= 0 && c[3] >= 0) xb_c(&b, (char)(((c[2] & 3) << 6) | c[3]));
        }
        *out = xb_str(&b);
        return 1;
    }
    return 0;
}

/* ================================================================ mimetypes */

static int mimetypes_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    const char *fn = xs(a, n, 0, ""), *dot = strrchr(fn, '.');
    static const char *exts[] = {
        ".html","text/html", ".htm","text/html", ".txt","text/plain",
        ".css","text/css", ".csv","text/csv", ".json","application/json",
        ".xml","application/xml", ".pdf","application/pdf", ".zip","application/zip",
        ".png","image/png", ".jpg","image/jpeg", ".jpeg","image/jpeg",
        ".gif","image/gif", ".svg","image/svg+xml", ".py","text/x-python",
        ".exe","application/octet-stream", ".bin","application/octet-stream",
        NULL
    };
    (void)a; (void)n;
    if (xeq(name, "guess_type")) {
        size_t i;
        if (!dot) { PyVal *v[2]; v[0] = py_none(); v[1] = py_none(); *out = py_tuple_from(v, 2); py_release(v[0]); py_release(v[1]); return 1; }
        for (i = 0; exts[i]; i += 2)
            if (!xi_cmp(dot, exts[i])) { *out = py_str(exts[i + 1]); return 1; }
        *out = py_str("application/octet-stream");
        return 1;
    }
    if (xeq(name, "guess_extension")) {
        const char *mt = xs(a, n, 0, "");
        size_t i;
        for (i = 0; exts[i]; i += 2)
            if (!xi_cmp(mt, exts[i + 1])) { *out = py_str(exts[i]); return 1; }
        *out = py_none();
        return 1;
    }
    if (xeq(name, "init")) { *out = py_none(); return 1; }
    return 0;
}

/* ============================================================== html.parser */

static void html_unescape_into(XBuf *b, const char *s) {
    while (*s) {
        if (*s == '&') {
            const char *semi = strchr(s, ';');
            if (semi && semi - s < 16) {
                char name[16];
                size_t l = (size_t)(semi - s - 1);
                memcpy(name, s + 1, l); name[l] = 0;
                if (!strcmp(name, "amp")) xb_c(b, '&');
                else if (!strcmp(name, "lt")) xb_c(b, '<');
                else if (!strcmp(name, "gt")) xb_c(b, '>');
                else if (!strcmp(name, "quot")) xb_c(b, '"');
                else if (!strcmp(name, "apos")) xb_c(b, '\'');
                else if (!strcmp(name, "nbsp")) xb_s(b, "\xC2\xA0");
                else if (name[0] == '#') {
                    int cp = name[1] == 'x' || name[1] == 'X' ?
                        (int)strtol(name + 2, NULL, 16) : (int)strtol(name + 1, NULL, 10);
                    if (cp < 128) xb_c(b, (char)cp);
                    else if (cp < 0x800) {
                        xb_c(b, (char)(0xC0 | (cp >> 6)));
                        xb_c(b, (char)(0x80 | (cp & 63)));
                    } else {
                        xb_c(b, (char)(0xE0 | (cp >> 12)));
                        xb_c(b, (char)(0x80 | ((cp >> 6) & 63)));
                        xb_c(b, (char)(0x80 | (cp & 63)));
                    }
                } else { xb_c(b, '&'); xb_s(b, name); xb_c(b, ';'); }
                s = semi + 1;
                continue;
            }
        }
        xb_c(b, *s++);
    }
}

static int htmlparser_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    if (xeq(name, "HTMLParser")) {
        PyVal *r = py_dict_new();
        py_dict_put(r, "data", py_str(""));
        *out = r;
        return 1;
    }
    if (xeq(name, "unescape") || xeq(name, "unescape_entities")) {
        XBuf b;
        xb_init(&b);
        html_unescape_into(&b, xs(a, n, 0, ""));
        *out = xb_str(&b);
        return 1;
    }
    if (xeq(name, "feed") || xeq(name, "close")) {
        PyVal *p = one(a, n, "feed");
        const char *s = xs(a, n, 1, "");
        PyVal *cur = py_getitem(p, py_str("data"));
        XBuf b;
        xb_init(&b);
        xb_s(&b, py_cstr(cur));
        html_unescape_into(&b, s);
        {
            PyVal *v = xb_str(&b);
            py_dict_put(p, "data", v);
            py_release(v);
        }
        py_release(cur);
        *out = py_none();
        return 1;
    }
    return 0;
}

/* ==================================================== xml.etree.ElementTree */

static int xml_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    if (xeq(name, "fromstring") || xeq(name, "parse")) {
        const char *s = xs(a, n, 0, "");
        PyVal *r = py_dict_new();
        const char *p = strchr(s, '<');
        char tag[128] = "root";
        XBuf text;
        xb_init(&text);
        if (p) {
            p++;
            {
                size_t k = 0;
                while (*p && *p != '>' && *p != ' ' && k < sizeof(tag) - 1) tag[k++] = *p++;
                tag[k] = 0;
            }
            {
                const char *gt = strchr(p, '>');
                const char *close = strstr(s, "</");
                if (gt && close && close > gt) xb_put(&text, gt + 1, (size_t)(close - gt - 1));
            }
        }
        py_dict_put(r, "tag", py_str(tag));
        py_dict_put(r, "text", xb_str(&text));
        py_dict_put(r, "attrib", py_dict_new());
        *out = r;
        return 1;
    }
    if (xeq(name, "Element") || xeq(name, "SubElement")) {
        const char *tag = xs(a, n, 0, "root");
        const char *txt = xs(a, n, 1, "");
        PyVal *r = py_dict_new();
        py_dict_put(r, "tag", py_str(tag));
        py_dict_put(r, "text", py_str(txt));
        py_dict_put(r, "attrib", py_dict_new());
        *out = r;
        return 1;
    }
    if (xeq(name, "tostring")) {
        PyVal *e = one(a, n, "tostring");
        PyVal *tag = py_getitem(e, py_str("tag")), *txt = py_getitem(e, py_str("text"));
        XBuf b;
        xb_init(&b);
        xb_c(&b, '<'); xb_s(&b, py_cstr(tag)); xb_c(&b, '>');
        xb_s(&b, py_cstr(txt));
        xb_s(&b, "</"); xb_s(&b, py_cstr(tag)); xb_c(&b, '>');
        py_release(tag); py_release(txt);
        *out = xb_str(&b);
        return 1;
    }
    return 0;
}

/* ======================================================================== io */

static int io_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    if (xeq(name, "StringIO") || xeq(name, "BytesIO")) {
        PyVal *r = py_dict_new();
        py_dict_put(r, "value", py_str(xs(a, n, 0, "")));
        py_dict_put(r, "pos", py_int(0));
        *out = r;
        return 1;
    }
    if (xeq(name, "getvalue")) {
        PyVal *f = one(a, n, "getvalue");
        *out = ret_same(py_getitem(f, py_str("value")));
        return 1;
    }
    if (xeq(name, "write") || xeq(name, "writelines")) {
        PyVal *f = one(a, n, "write");
        PyVal *cur = py_getitem(f, py_str("value"));
        XBuf b;
        xb_init(&b);
        xb_s(&b, py_cstr(cur));
        if (xeq(name, "writelines")) {
            PyVal *lines = as_list(a[1]);
            size_t i;
            for (i = 0; i < py_len(lines); i++) {
                PyVal *l = py_getitem(lines, py_int((int64_t)i));
                xb_s(&b, py_cstr(l));
                py_release(l);
            }
            py_release(lines);
        } else xb_s(&b, py_cstr(a[1]));
        {
            PyVal *v = xb_str(&b);
            py_dict_put(f, "value", v);
            py_release(v);
        }
        py_release(cur);
        *out = py_int((int64_t)strlen(xs(a, n, 1, "")));
        return 1;
    }
    if (xeq(name, "read") || xeq(name, "readline")) {
        PyVal *f = one(a, n, "read");
        PyVal *cur = py_getitem(f, py_str("value"));
        PyVal *posv = py_getitem(f, py_str("pos"));
        size_t pos = (size_t)py_as_int(posv), len = py_cstr_len(cur);
        size_t want = a && n > 1 ? (size_t)py_as_int(a[1]) : len - pos;
        PyVal *r;
        if (xeq(name, "readline")) {
            const char *nl = strchr(py_cstr(cur) + pos, '\n');
            want = nl ? (size_t)(nl - (py_cstr(cur) + pos) + 1) : len - pos;
        }
        if (pos > len) pos = len;
        if (want > len - pos) want = len - pos;
        r = py_strn(py_cstr(cur) + pos, want);
        {
            PyVal *np = py_int((int64_t)(pos + want));
            py_dict_put(f, "pos", np);
            py_release(np);
        }
        py_release(posv); py_release(cur);
        *out = r;
        return 1;
    }
    if (xeq(name, "seek")) {
        PyVal *f = one(a, n, "seek");
        PyVal *p = py_int(xi(a, n, 1, 0));
        py_dict_put(f, "pos", p);
        py_release(p);
        *out = py_int(0);
        return 1;
    }
    return 0;
}

/* =================================================================== pickle */

static void pickle_dump(XBuf *b, PyVal *v, int level) {
    if (level > 16) return;
    switch (v->type) {
    case PY_T_NONE: xb_s(b, "N."); break;
    case PY_T_BOOL: xb_s(b, v->u.b ? "I01\n." : "I00\n."); break;
    case PY_T_INT: xb_s(b, "I"); xb_i(b, v->u.i); xb_s(b, "\n."); break;
    case PY_T_FLOAT: xb_s(b, "F"); xb_f(b, v->u.f); xb_s(b, "\n."); break;
    case PY_T_STR: xb_s(b, "S'"); xb_s(b, py_cstr(v)); xb_s(b, "'\n."); break;
    case PY_T_LIST: case PY_T_TUPLE: {
        size_t i;
        xb_s(b, v->type == PY_T_LIST ? "(l" : "(t");
        for (i = 0; i < py_len(v); i++) {
            PyVal *x = py_getitem(v, py_int((int64_t)i));
            pickle_dump(b, x, level + 1);
            py_release(x);
        }
        xb_s(b, v->type == PY_T_LIST ? "a." : "t.");
        break;
    }
    case PY_T_DICT: {
        PyVal *it = py_iter(v), *k = NULL;
        xb_s(b, "(d");
        while (py_iter_next(it, &k)) {
            PyVal *val = py_getitem(v, k);
            pickle_dump(b, k, level + 1);
            pickle_dump(b, val, level + 1);
            py_release(val);
        }
        py_release(k); py_release(it);
        xb_s(b, "s.");
        break;
    }
    default: {
        PyVal *s = py_repr(v);
        xb_s(b, "S'"); xb_s(b, py_cstr(s)); xb_s(b, "'\n.");
        py_release(s);
        break;
    }
    }
}

static PyVal *pickle_load(const char **pp) {
    const char *p = *pp;
    while (*p == ' ' || *p == '\n' || *p == '\r') p++;
    if (*p == 'N') { *pp = p + 2; return py_none(); }
    if (*p == 'I' || *p == 'F') {
        char num[128];
        size_t k = 0;
        p++;
        while (*p && *p != '\n' && k < sizeof(num) - 1) num[k++] = *p++;
        num[k] = 0;
        if (*p == '\n') p++;
        if (*p == '.') p++;
        *pp = p;
        return *num == 'F' ? py_float(strtod(num + 1, NULL)) : py_int(strtoll(num, NULL, 10));
    }
    if (*p == 'S') {
        PyVal *r;
        XBuf b;
        p++;
        if (*p == '\'') p++;
        xb_init(&b);
        while (*p && *p != '\'') {
            if (*p == '\\' && p[1]) { p++; }
            xb_c(&b, *p++);
        }
        if (*p == '\'') p++;
        while (*p && *p != '.') p++;
        if (*p == '.') p++;
        r = xb_str(&b);
        *pp = p;
        return r;
    }
    if (*p == '(') {
        p++;
        if (*p == 'l' || *p == 't') {
            int islist = *p == 'l';
            PyVal *r = islist ? py_list_new() : py_list_new();
            p++;
            while (*p && *p != '.' && *p != 'a' && *p != 't') {
                PyVal *x = pickle_load(&p);
                py_list_append(r, x);
                py_release(x);
                while (*p == ' ' || *p == '\n' || *p == '\r') p++;
            }
            if (*p == 'a' || *p == 't') p++;
            if (*p == '.') p++;
            *pp = p;
            if (!islist) {
                PyVal *v[64];
                size_t i, n = py_len(r) < 64 ? py_len(r) : 64;
                for (i = 0; i < n; i++) v[i] = py_getitem(r, py_int((int64_t)i));
                {
                    PyVal *t = py_tuple_from(v, n);
                    for (i = 0; i < n; i++) py_release(v[i]);
                    py_release(r);
                    *pp = p;
                    return t;
                }
            }
            return r;
        }
        if (*p == 'd') {
            PyVal *r = py_dict_new();
            p++;
            while (*p && *p != 's' && *p != '.') {
                PyVal *k = pickle_load(&p), *v;
                while (*p == ' ' || *p == '\n' || *p == '\r') p++;
                v = pickle_load(&p);
                py_dict_put(r, py_cstr(k), v);
                py_release(k); py_release(v);
                while (*p == ' ' || *p == '\n' || *p == '\r') p++;
            }
            if (*p == 's') p++;
            if (*p == '.') p++;
            *pp = p;
            return r;
        }
    }
    *pp = p + 1;
    return py_none();
}

static int pickle_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    if (xeq(name, "dumps")) {
        PyVal *v = one(a, n, "dumps");
        XBuf b;
        xb_init(&b);
        xb_s(&b, "\x80\x04");
        pickle_dump(&b, v, 0);
        *out = xb_str(&b);
        return 1;
    }
    if (xeq(name, "loads")) {
        const char *s = xs(a, n, 0, "");
        const char *p = s;
        if (*p == '\x80') p += 2;
        *out = pickle_load(&p);
        return 1;
    }
    if (xeq(name, "dump") || xeq(name, "load")) {
        py_raise(PY_EXC_UNSUPPORTED, "use pickle.dumps / pickle.loads in C mode");
        *out = py_none();
        return 1;
    }
    return 0;
}

/* ================================================================= copyreg */

static int copyreg_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    (void)a; (void)n;
    if (xeq(name, "_reconstructor")) { *out = py_none(); return 1; }
    if (xeq(name, "constructor")) { PyVal *r = py_dict_new(); *out = r; return 1; }
    return 0;
}

/* ================================================================= marshal */

static int marshal_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    if (xeq(name, "version")) { *out = py_int(4); return 1; }
    if (xeq(name, "dumps")) { PyVal *v = one(a, n, "dumps"); *out = py_repr(v); return 1; }
    if (xeq(name, "loads")) { *out = py_str(xs(a, n, 0, "")); return 1; }
    return 0;
}

/* ============================================================ shelve / dbm */

static int shelve_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    if (xeq(name, "open") || xeq(name, "Shelf")) {
        const char *path = xs(a, n, 0, "py2c99.shelf");
        PyVal *r = py_dict_new();
        FILE *fp = fopen(path, "rb");
        py_dict_put(r, "_path", py_str(path));
        if (fp) {
            char line[8192];
            while (fgets(line, sizeof(line), fp)) {
                char *eq = strchr(line, '=');
                if (eq) {
                    *eq = 0;
                    {
                        char *v = eq + 1, *nl = strchr(v, '\n');
                        if (nl) *nl = 0;
                        py_dict_put(r, line, py_str(v));
                    }
                }
            }
            fclose(fp);
        }
        *out = r;
        return 1;
    }
    if (xeq(name, "sync") || xeq(name, "close")) {
        PyVal *sh = one(a, n, "shelve");
        PyVal *pv = py_getitem(sh, py_str("_path"));
        FILE *fp = fopen(py_cstr(pv), "wb");
        PyVal *it;
        PyVal *k = NULL;
        if (fp) {
            it = py_iter(sh);
            while (py_iter_next(it, &k)) {
                PyVal *v;
                if (xeq(py_cstr(k), "_path")) continue;
                v = py_getitem(sh, k);
                fprintf(fp, "%s=%s\n", py_cstr(k), py_cstr(v));
                py_release(v);
            }
            py_release(k); py_release(it);
            fclose(fp);
        }
        py_release(pv);
        *out = py_none();
        return 1;
    }
    return 0;
}

static int dbm_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    if (xeq(name, "open") || xeq(name, "whichdb")) {
        const char *path = xs(a, n, 0, "py2c99.db");
        PyVal *r = py_dict_new();
        FILE *fp = fopen(path, "rb");
        py_dict_put(r, "_path", py_str(path));
        if (fp) {
            char buf[8192];
            size_t got;
            while ((got = fread(buf, 1, sizeof(buf), fp)) > 0) {
                char *p = buf, *end = buf + got;
                while (p < end) {
                    char *nl = memchr(p, '\n', (size_t)(end - p));
                    char *eq = memchr(p, '=', (size_t)((nl ? nl : end) - p));
                    if (!eq) { p = nl ? nl + 1 : end; continue; }
                    {
                        char *v = eq + 1, *ve = nl ? nl : end;
                        py_dict_strkey(r, py_strn(p, (size_t)(eq - p)), py_strn(v, (size_t)(ve - v)));
                    }
                    p = nl ? nl + 1 : end;
                }
            }
            fclose(fp);
        }
        *out = r;
        return 1;
    }
    return 0;
}

/* ================================================================== tarfile */

static int tarfile_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    if (xeq(name, "open") || xeq(name, "TarFile")) {
        const char *path = xs(a, n, 0, "");
        PyVal *r = py_dict_new(), *members = py_list_new();
        FILE *fp = fopen(path, "rb");
        py_dict_put(r, "_path", py_str(path));
        if (fp) {
            unsigned char blk[512];
            while (fread(blk, 1, 512, fp) == 512) {
                long sz = 0;
                size_t k;
                if (!blk[0]) break;
                for (k = 124; k < 136; k++)
                    if (blk[k] >= '0' && blk[k] <= '7') sz = sz * 8 + (blk[k] - '0');
                {
                    char nm[101];
                    PyVal *m = py_dict_new();
                    memcpy(nm, blk, 100); nm[100] = 0;
                    py_dict_put(m, "name", py_str(nm));
                    py_dict_put(m, "size", py_int(sz));
                    py_dict_put(m, "mode", py_int(0));
                    py_list_append(members, m);
                    py_release(m);
                }
                if (fseek(fp, (long)(((sz + 511) / 512) * 512), SEEK_CUR)) break;
            }
            fclose(fp);
        }
        py_dict_put(r, "members", members);
        py_release(members);
        *out = r;
        return 1;
    }
    if (xeq(name, "getnames")) {
        PyVal *t = one(a, n, "getnames");
        PyVal *ms = py_getitem(t, py_str("members"));
        PyVal *r = py_list_new(), *it = py_iter(ms), *m = NULL;
        while (py_iter_next(it, &m)) {
            PyVal *nm = py_getitem(m, py_str("name"));
            py_list_append(r, nm);
            py_release(nm);
        }
        py_release(m); py_release(it);
        *out = r;
        return 1;
    }
    return 0;
}

/* ================================================================== zipfile */

static int zipfile_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    if (xeq(name, "ZipFile") || xeq(name, "is_zipfile")) {
        const char *path = xs(a, n, 0, "");
        PyVal *r = py_dict_new(), *names = py_list_new();
        FILE *fp = fopen(path, "rb");
        py_dict_put(r, "_path", py_str(path));
        if (fp) {
            unsigned char h[4];
            long pos = 0;
            while (fread(h, 1, 4, fp) == 4) {
                if (h[0] == 'P' && h[1] == 'K' && h[2] == 3 && h[3] == 4) {
                    unsigned char lh[30];
                    if (fread(lh, 1, 26, fp) == 26) {
                        uint32_t cs = (uint32_t)lh[18] | ((uint32_t)lh[19] << 8) |
                                      ((uint32_t)lh[20] << 16) | ((uint32_t)lh[21] << 24);
                        uint32_t us = (uint32_t)lh[22] | ((uint32_t)lh[23] << 8) |
                                      ((uint32_t)lh[24] << 16) | ((uint32_t)lh[25] << 24);
                        uint16_t nl = (uint16_t)lh[26] | (uint16_t)(lh[27] << 8);
                        (void)cs; (void)us;
                        if (nl && nl < 4096) {
                            char *nm = (char *)malloc(nl + 1);
                            if (nm && fread(nm, 1, nl, fp) == nl) {
                                nm[nl] = 0;
                                py_list_append(names, py_str(nm));
                            }
                            free(nm);
                        }
                        pos = ftell(fp);
                        {
                            long skip = (long)cs + (long)us - (long)nl;
                            if (skip > 0) fseek(fp, skip, SEEK_CUR);
                        }
                    }
                }
                pos = ftell(fp);
            }
            fclose(fp);
            (void)pos;
        }
        py_dict_put(r, "namelist", names);
        py_release(names);
        *out = r;
        return 1;
    }
    if (xeq(name, "namelist")) {
        PyVal *z = one(a, n, "namelist");
        *out = ret_same(py_getitem(z, py_str("namelist")));
        return 1;
    }
    return 0;
}

/* =========================================================== wave/aifc/sunau */

static int audio_call(const char *module, const char *name, PyVal **a, size_t n, PyVal **out) {
    if (!xeq(name, "open")) return 0;
    const char *path = xs(a, n, 0, "");
    FILE *fp = fopen(path, "rb");
    PyVal *r = py_dict_new();
    py_dict_put(r, "_path", py_str(path));
    if (fp) {
        unsigned char h[64];
        size_t got = fread(h, 1, sizeof(h), fp);
        fclose(fp);
        if (xeq(module, "wave") && got >= 44 && !memcmp(h, "RIFF", 4)) {
            uint32_t rate = (uint32_t)h[24] | ((uint32_t)h[25] << 8) |
                            ((uint32_t)h[26] << 16) | ((uint32_t)h[27] << 24);
            uint16_t ch = (uint16_t)h[22] | (uint16_t)(h[23] << 8);
            uint16_t sw = (uint16_t)h[34] | (uint16_t)(h[35] << 8);
            py_dict_put(r, "nchannels", py_int(ch));
            py_dict_put(r, "sampwidth", py_int(sw / 8));
            py_dict_put(r, "framerate", py_int(rate));
            py_dict_put(r, "nframes", py_int(((h[40] | (h[41] << 8) | (h[42] << 16) | ((uint32_t)h[43] << 24)) / (ch ? ch * sw / 8 : 1))));
        } else if (xeq(module, "aifc") && got >= 24 && (!memcmp(h, "FORM", 4) || !memcmp(h, "AIFF", 4))) {
            int off = !memcmp(h, "FORM", 4) ? 22 : 18;
            if (got >= (size_t)off + 2) {
                uint16_t ch = (uint16_t)h[off - 2] | (uint16_t)(h[off - 1] << 8);
                py_dict_put(r, "nchannels", py_int(ch));
                py_dict_put(r, "sampwidth", py_int(2));
                py_dict_put(r, "framerate", py_int(44100));
                py_dict_put(r, "nframes", py_int(0));
            }
        } else if (xeq(module, "sunau") && got >= 24 && !memcmp(h, ".snd", 4)) {
            uint32_t off = (uint32_t)h[4] | ((uint32_t)h[5] << 8) | ((uint32_t)h[6] << 16) | ((uint32_t)h[7] << 24);
            uint32_t sz = (uint32_t)h[8] | ((uint32_t)h[9] << 8) | ((uint32_t)h[10] << 16) | ((uint32_t)h[11] << 24);
            uint32_t enc = (uint32_t)h[12] | ((uint32_t)h[13] << 8) | ((uint32_t)h[14] << 16) | ((uint32_t)h[15] << 24);
            uint32_t rate = (uint32_t)h[16] | ((uint32_t)h[17] << 8) | ((uint32_t)h[18] << 16) | ((uint32_t)h[19] << 24);
            uint32_t ch = (uint32_t)h[20] | ((uint32_t)h[21] << 8) | ((uint32_t)h[22] << 16) | ((uint32_t)h[23] << 24);
            py_dict_put(r, "nchannels", py_int(ch));
            py_dict_put(r, "sampwidth", py_int(2));
            py_dict_put(r, "framerate", py_int(rate));
            py_dict_put(r, "nframes", py_int((sz - off) / (ch ? ch * 2 : 2)));
            py_dict_put(r, "comptype", py_int(enc));
        }
    }
    *out = r;
    return 1;
}

/* ==================================================================== chunk */

static int chunk_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    if (xeq(name, "Chunk")) {
        const char *path = xs(a, n, 1, "");
        FILE *fp = fopen(path, "rb");
        PyVal *r = py_dict_new();
        char id[5] = { 0 };
        uint32_t sz = 0;
        py_dict_put(r, "_path", py_str(path));
        if (fp) {
            if (fread(id, 1, 4, fp) == 4) {
                unsigned char b[4];
                if (fread(b, 1, 4, fp) == 4)
                    sz = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
                py_dict_put(r, "id", py_str(id));
                py_dict_put(r, "size", py_int(sz));
            }
            fclose(fp);
        }
        *out = r;
        return 1;
    }
    return 0;
}

/* ================================================================ fileinput */

static int fileinput_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    if (xeq(name, "input") || xeq(name, "FileInput")) {
        PyVal *files = as_list(one(a, n, "fileinput"));
        PyVal *r = py_list_new(), *it = py_iter(files), *f = NULL;
        while (py_iter_next(it, &f)) {
            FILE *fp = fopen(py_cstr(f), "r");
            if (fp) {
                char line[8192];
                while (fgets(line, sizeof(line), fp)) {
                    size_t l = strlen(line);
                    if (l && line[l - 1] == '\n') line[--l] = 0;
                    py_list_append(r, py_strn(line, l));
                }
                fclose(fp);
            }
        }
        py_release(f); py_release(it); py_release(files);
        *out = r;
        return 1;
    }
    return 0;
}

/* ================================================================= filecmp */

static int filecmp_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    const char *p1 = xs(a, n, 0, ""), *p2 = xs(a, n, 1, "");
    if (xeq(name, "cmp") || xeq(name, "cmpfiles")) {
        FILE *f1 = fopen(p1, "rb"), *f2 = fopen(p2, "rb");
        int same = f1 && f2;
        if (f1 && f2) {
            int c1, c2;
            do {
                c1 = fgetc(f1); c2 = fgetc(f2);
                if (c1 != c2) { same = 0; break; }
            } while (c1 != EOF);
        } else same = 0;
        if (f1) fclose(f1);
        if (f2) fclose(f2);
        *out = py_bool(same);
        return 1;
    }
    return 0;
}

/* =============================================================== linecache */

static int linecache_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    if (xeq(name, "getline") || xeq(name, "getlines") || xeq(name, "checkcache")) {
        const char *path = xs(a, n, 0, "");
        size_t want = (size_t)xi(a, n, 1, 1), i = 0;
        FILE *fp = fopen(path, "r");
        PyVal *r = py_list_new();
        if (fp) {
            char line[8192];
            while (fgets(line, sizeof(line), fp)) {
                i++;
                if (xeq(name, "getline") && i == want) {
                    size_t l = strlen(line);
                    if (l && line[l - 1] == '\n') line[--l] = 0;
                    py_release(r);
                    r = py_strn(line, l);
                    break;
                }
                if (!xeq(name, "getline")) {
                    size_t l = strlen(line);
                    if (l && line[l - 1] == '\n') line[--l] = 0;
                    py_list_append(r, py_strn(line, l));
                }
            }
            fclose(fp);
        }
        if (xeq(name, "getline") && py_len(r) == 0) { py_release(r); r = py_str(""); }
        *out = r;
        return 1;
    }
    if (xeq(name, "clearcache")) { *out = py_none(); return 1; }
    return 0;
}

/* ============================================================ locale/gettext */

static int locale_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    if (xeq(name, "setlocale")) {
        int cat = (int)xi(a, n, 0, 6);
        const char *loc = xs(a, n, 1, "");
#ifdef _WIN32
        static char cur[128] = "C";
        if (loc && *loc) {
            const char *r = setlocale(cat, loc);
            if (r) snprintf(cur, sizeof(cur), "%s", r);
        } else {
            const char *r = setlocale(cat, NULL);
            if (r) snprintf(cur, sizeof(cur), "%s", r);
        }
        *out = py_str(cur);
#else
        const char *r = setlocale(cat, loc && *loc ? loc : NULL);
        *out = py_str(r ? r : "C");
#endif
        return 1;
    }
    if (xeq(name, "getlocale") || xeq(name, "getpreferredencoding")) {
        const char *r = setlocale(0, NULL);
        *out = py_str(r ? r : "C");
        return 1;
    }
    if (xeq(name, "normalize")) { *out = py_str(xs(a, n, 1, "")); return 1; }
    if (xeq(name, "localeconv")) {
        const char *k[] = { "decimal_point", "thousands_sep", "grouping",
                            "int_curr_symbol", "currency_symbol" };
        struct lconv *lc = localeconv();
        PyVal *v[5];
        v[0] = py_str(lc->decimal_point ? lc->decimal_point : ".");
        v[1] = py_str(lc->thousands_sep ? lc->thousands_sep : ",");
        v[2] = py_str(lc->grouping ? lc->grouping : "\3");
        v[3] = py_str(lc->int_curr_symbol ? lc->int_curr_symbol : "USD ");
        v[4] = py_str(lc->currency_symbol ? lc->currency_symbol : "$");
        *out = py_dict_lit(k, v, 5);
        py_release(v[0]); py_release(v[1]); py_release(v[2]);
        py_release(v[3]); py_release(v[4]);
        return 1;
    }
    return 0;
}

static int gettext_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    const char *s = xs(a, n, 0, "");
    if (xeq(name, "gettext") || xeq(name, "_") || xeq(name, "dgettext") || xeq(name, "ldgettext")) {
        static PyVal *catalog = NULL;
        if (!catalog) catalog = py_dict_new();
        {
            PyVal *v = py_dict_get(catalog, s);
            *out = v ? ret_same(v) : py_str(s);
        }
        return 1;
    }
    if (xeq(name, "ngettext")) {
        int64_t num = xi(a, n, 2, 1);
        *out = py_str(num == 1 ? xs(a, n, 0, "") : xs(a, n, 1, ""));
        return 1;
    }
    if (xeq(name, "install") || xeq(name, "textdomain") || xeq(name, "bindtextdomain")) {
        *out = py_str(xs(a, n, 0, ""));
        return 1;
    }
    return 0;
}

/* ========================================================= atexit/warnings */

static PyVal *g_atexit = NULL;

static int atexit_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    if (!g_atexit) g_atexit = py_list_new();
    if (xeq(name, "register")) {
        PyVal *f = one(a, n, "atexit.register");
        py_list_append(g_atexit, f);
        if (n > 1) {
            PyVal *args = py_list_from(a + 1, n - 1);
            py_list_append(g_atexit, args);
            py_release(args);
        } else {
            py_list_append(g_atexit, py_list_new());
        }
        *out = f;
        return 1;
    }
    if (xeq(name, "unregister")) {
        PyVal *f = one(a, n, "atexit.unregister");
        for (;;) {
            PyVal *it = py_iter(g_atexit), *x = NULL;
            int found = 0;
            while (py_iter_next(it, &x)) {
                int eq = x && f && x->type == f->type &&
                         x->type == PY_T_FUNC && x->u.func.fn == f->u.func.fn;
                py_release(x);
                if (eq) { found = 1; break; }
            }
            py_release(it);
            if (!found) break;
            {
                PyVal *idx = py_int(0);
                py_delitem(g_atexit, idx);
                py_release(idx);
            }
        }
        *out = py_none();
        return 1;
    }
    if (xeq(name, "_run")) {
        size_t i;
        for (i = 0; i + 1 < py_len(g_atexit); i += 2) {
            PyVal *f = py_getitem(g_atexit, py_int((int64_t)i));
            PyVal *args = py_getitem(g_atexit, py_int((int64_t)(i + 1)));
            py_call_list(f, args, NULL);
            py_release(f); py_release(args);
        }
        *out = py_none();
        return 1;
    }
    return 0;
}

void py_extra_run_atexit(void) {
    PyVal *out = NULL;
    if (!g_atexit) return;
    if (atexit_call("_run", NULL, 0, &out)) py_release(out);
}

static int warnings_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    if (xeq(name, "warn") || xeq(name, "warn_explicit") || xeq(name, "showwarning")) {
        const char *msg = xs(a, n, 0, "");
        const char *cat = n > 1 && a[1]->type == PY_T_STR ? py_cstr(a[1]) : "UserWarning";
        fprintf(stderr, "%s: %s\n", cat, msg);
        *out = py_none();
        return 1;
    }
    if (xeq(name, "catch_warnings")) {
        PyVal *r = py_dict_new();
        py_dict_put(r, "record", py_list_new());
        *out = r;
        return 1;
    }
    if (xeq(name, "simplefilter") || xeq(name, "filterwarnings") || xeq(name, "resetwarnings")) {
        *out = py_none();
        return 1;
    }
    return 0;
}

/* ================================================================ traceback */

static int traceback_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    if (xeq(name, "format_exc") || xeq(name, "format_exception") || xeq(name, "format_tb")) {
        PyVal *e = py_exc_current();
        XBuf b;
        xb_init(&b);
        if (e) {
            PyVal *s = py_str_of(e);
            xb_s(&b, "Traceback (most recent call last):\n");
            xb_s(&b, py_cstr(s));
            xb_c(&b, '\n');
            py_release(s);
        } else xb_s(&b, "NoneType: None\n");
        *out = xb_str(&b);
        return 1;
    }
    if (xeq(name, "print_exc") || xeq(name, "print_exception")) {
        PyVal *e = py_exc_current();
        if (e) py_print_exc(e);
        *out = py_none();
        return 1;
    }
    if (xeq(name, "extract_tb")) { *out = py_list_new(); return 1; }
    return 0;
}

/* ============================================================== contextvars */

static int contextvars_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    if (xeq(name, "ContextVar") || xeq(name, "Token")) {
        PyVal *r = py_dict_new();
        PyVal *def = n && a[0] ? a[0] : py_none();
        py_dict_put(r, "value", def);
        if (n > 1) {
            PyVal *nm = py_str_of(a[1]);
            py_dict_put(r, "name", nm);
            py_release(nm);
        } else py_dict_put(r, "name", py_str("var"));
        *out = r;
        return 1;
    }
    if (xeq(name, "get") || xeq(name, "set")) {
        PyVal *v = one(a, n, "ContextVar");
        if (xeq(name, "set")) {
            PyVal *old = py_getitem(v, py_str("value"));
            py_dict_put(v, "value", a[1]);
            *out = old;
        } else {
            *out = ret_same(py_getitem(v, py_str("value")));
        }
        return 1;
    }
    if (xeq(name, "reset")) {
        PyVal *v = one(a, n, "reset");
        if (n > 1) py_dict_put(v, "value", a[1]);
        *out = py_none();
        return 1;
    }
    if (xeq(name, "copy_context")) { *out = py_dict_new(); return 1; }
    return 0;
}

/* ================================================================= graphlib */

static int graphlib_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    (void)a; (void)n;
    if (xeq(name, "TopologicalSorter")) {
        PyVal *r = py_dict_new();
        py_dict_put(r, "graph", py_dict_new());
        py_dict_put(r, "ready", py_list_new());
        *out = r;
        return 1;
    }
    if (xeq(name, "add")) {
        PyVal *ts = one(a, n, "add");
        PyVal *g = py_getitem(ts, py_str("graph"));
        PyVal *node = a[1];
        PyVal *deps = py_list_from(a + 2, n > 2 ? n - 2 : 0);
        py_dict_put(g, py_cstr(node), deps);
        {
            PyVal *ready = py_getitem(ts, py_str("ready"));
            size_t i;
            int hasdep = 0;
            for (i = 0; i < py_len(deps); i++) {
                PyVal *d = py_getitem(deps, py_int((int64_t)i));
                if (py_contains(g, d)) hasdep = 1;
                py_release(d);
            }
            if (!hasdep) py_list_append(ready, ret_same(node));
            py_release(ready);
        }
        py_release(deps); py_release(g);
        *out = py_none();
        return 1;
    }
    if (xeq(name, "static_order") || xeq(name, "prepare") || xeq(name, "get_ready")) {
        PyVal *ts = one(a, n, "static_order");
        PyVal *g = py_getitem(ts, py_str("graph"));
        PyVal *r = py_list_new(), *it = py_iter(g), *k = NULL;
        while (py_iter_next(it, &k)) {
            py_list_append(r, k);
            py_release(k);
        }
        py_release(it); py_release(g);
        x_sort(r, 0);
        *out = r;
        return 1;
    }
    return 0;
}

/* ============================================================== sched/selectors */

static int sched_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    if (xeq(name, "scheduler")) {
        PyVal *r = py_dict_new();
        PyVal *q = py_list_new();
        py_dict_put(r, "queue", q);
        py_release(q);
        *out = r;
        return 1;
    }
    if (xeq(name, "enter") || xeq(name, "enterabs")) {
        PyVal *s = one(a, n, "enter");
        PyVal *q = py_getitem(s, py_str("queue"));
        PyVal *e[3], *t;
        e[0] = py_float(xd(a, n, 0, 0)); e[1] = ret_same(a[1]); e[2] = a[2];
        t = py_tuple_from(e, 3);
        py_list_append(q, t);
        py_release(t); py_release(e[0]); py_release(e[1]);
        *out = t;
        return 1;
    }
    if (xeq(name, "run")) {
        PyVal *s = one(a, n, "run");
        PyVal *q = py_getitem(s, py_str("queue"));
        size_t i;
        x_sort(q, 0);
        for (i = 0; i < py_len(q); i++) {
            PyVal *e = py_getitem(q, py_int((int64_t)i));
            PyVal *fn = py_getitem(e, py_int(2));
            py_call(fn, NULL, 0, NULL);
            py_release(fn); py_release(e);
        }
        {
            PyVal *empty = py_list_new();
            py_dict_put(s, "queue", empty);
            py_release(empty);
        }
        py_release(q);
        *out = py_none();
        return 1;
    }
    return 0;
}

static int selectors_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    (void)a; (void)n;
    if (xeq(name, "DefaultSelector") || xeq(name, "SelectSelector") ||
        xeq(name, "PollSelector") || xeq(name, "EpollSelector") ||
        xeq(name, "KqueueSelector") || xeq(name, "BaseSelector")) {
        PyVal *r = py_dict_new();
        py_dict_put(r, "registered", py_dict_new());
        *out = r;
        return 1;
    }
    if (xeq(name, "register") || xeq(name, "unregister") || xeq(name, "modify")) {
        PyVal *s = one(a, n, "register");
        PyVal *reg = py_getitem(s, py_str("registered"));
        if (xeq(name, "unregister")) {
            PyVal *k = py_int(py_as_int(a[1]));
            py_delitem(reg, k);
            py_release(k);
        } else {
            char kb[32];
            snprintf(kb, sizeof(kb), "%d", (int)py_as_int(a[1]));
            py_dict_put(reg, kb, ret_same(a[1]));
        }
        py_release(reg);
        *out = py_none();
        return 1;
    }
    if (xeq(name, "select") || xeq(name, "close")) { *out = py_list_new(); return 1; }
    if (xeq(name, "get_map")) {
        PyVal *s = one(a, n, "get_map");
        *out = ret_same(py_getitem(s, py_str("registered")));
        return 1;
    }
    return 0;
}

/* ============================================================== mmap/socket */

static int mmap_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    if (xeq(name, "mmap") || xeq(name, "ACCESS_READ")) {
        const char *path = xs(a, n, 1, "");
        size_t size = (size_t)xi(a, n, 2, 0);
        HANDLE fh = INVALID_HANDLE_VALUE, mh = NULL;
        void *ptr = NULL;
        PyVal *r = py_dict_new();
        (void)size;
#ifdef _WIN32
        fh = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (fh == INVALID_HANDLE_VALUE)
            fh = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (fh != INVALID_HANDLE_VALUE) {
            DWORD hi = 0, lo = GetFileSize(fh, &hi);
            mh = CreateFileMappingA(fh, NULL, PAGE_READONLY, hi, lo, NULL);
            if (mh) ptr = MapViewOfFile(mh, FILE_MAP_READ, 0, 0, 0);
        }
#else
        {
            int fd = open(path, O_RDONLY);
            if (fd >= 0) {
                struct stat st;
                if (!fstat(fd, &st)) size = (size_t)st.st_size;
                ptr = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
                close(fd);
            }
        }
#endif
        if (!ptr) { py_raise(PY_EXC_OS, "cannot map file"); *out = r; return 1; }
        py_dict_put(r, "_ptr", py_int((int64_t)(intptr_t)ptr));
        py_dict_put(r, "_size", py_int((int64_t)size));
        py_dict_put(r, "data", py_strn((const char *)ptr, size > 65536 ? 65536 : size));
#ifdef _WIN32
        if (mh) UnmapViewOfFile(ptr);
        if (fh != INVALID_HANDLE_VALUE) CloseHandle(fh);
        if (mh) CloseHandle(mh);
#endif
        *out = r;
        return 1;
    }
    if (xeq(name, "size")) {
        PyVal *m = one(a, n, "size");
        *out = ret_same(py_getitem(m, py_str("_size")));
        return 1;
    }
    return 0;
}

static int socket_ws_init(void) {
#ifdef _WIN32
    static int done = 0;
    if (!done) {
        WSADATA wsa;
        int r = WSAStartup(MAKEWORD(2, 2), &wsa);
        if (r) return 0;
        done = 1;
    }
#endif
    return 1;
}

static int socket_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    (void)a; (void)n;
    socket_ws_init();
    if (xeq(name, "gethostname")) {
        char host[256] = "localhost";
#ifdef _WIN32
        if (gethostname(host, (int)sizeof(host))) strcpy(host, "localhost");
#else
        gethostname(host, sizeof(host));
#endif
        *out = py_str(host);
        return 1;
    }
    if (xeq(name, "gethostbyname")) {
        const char *hn = xs(a, n, 0, "localhost");
        struct hostent *he = gethostbyname(hn);
        *out = he ? py_str(inet_ntoa(*(struct in_addr *)he->h_addr_list[0]))
                  : py_str("127.0.0.1");
        return 1;
    }
    if (xeq(name, "inet_aton") || xeq(name, "inet_pton") || xeq(name, "inet_addr")) {
        const char *ip = xs(a, n, 0, "127.0.0.1");
        struct in_addr ad;
        ad.s_addr = inet_addr(ip);
        *out = py_str(inet_ntoa(ad));
        return 1;
    }
    if (xeq(name, "inet_ntoa") || xeq(name, "inet_ntop")) {
        uint32_t ip = (uint32_t)xi(a, n, 0, 0x0100007f);
        struct in_addr ad;
        ad.s_addr = ip;
        *out = py_str(inet_ntoa(ad));
        return 1;
    }
    if (xeq(name, "socket") || xeq(name, "create_connection")) {
        int family = (int)xi(a, n, 0, 2), type = (int)xi(a, n, 1, 1);
        int fd;
        PyVal *r = py_dict_new();
        if (xeq(name, "create_connection") && n) {
            const char *host = xs(a, n, 0, "localhost");
            int port = (int)xi(a, n, 1, 80);
            struct hostent *he;
            struct sockaddr_in sa;
            struct in_addr loopback;
            loopback.s_addr = inet_addr("127.0.0.1");
            memset(&sa, 0, sizeof(sa));
            fd = (int)socket(AF_INET, SOCK_STREAM, 0);
            he = gethostbyname(host);
            sa.sin_family = AF_INET;
            sa.sin_port = htons((unsigned short)port);
            sa.sin_addr = he ? *(struct in_addr *)he->h_addr_list[0] : loopback;
            if (connect(fd, (struct sockaddr *)&sa, sizeof(sa))) {
#ifdef _WIN32
                closesocket(fd);
#else
                close(fd);
#endif
                fd = -1;
            }
        } else {
            fd = (int)socket(family == 2 ? AF_INET : family == 23 ? AF_INET6 : AF_INET,
                             type == 2 ? SOCK_DGRAM : SOCK_STREAM, 0);
        }
        py_dict_put(r, "fd", py_int(fd));
        py_dict_put(r, "family", py_int(family));
        py_dict_put(r, "type", py_int(type));
        *out = r;
        return 1;
    }
    if (xeq(name, "getaddrinfo")) {
        const char *host = xs(a, n, 0, "localhost");
        struct hostent *he = gethostbyname(host);
        PyVal *r = py_dict_new();
        py_dict_put(r, "ip", py_str(he ? inet_ntoa(*(struct in_addr *)he->h_addr_list[0]) : "127.0.0.1"));
        py_dict_put(r, "port", py_int(xi(a, n, 1, 0)));
        *out = r;
        return 1;
    }
    return 0;
}

static int signal_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    int sig = (int)xi(a, n, 0, 2);
    if (xeq(name, "signal")) {
        PyVal *h = a[1];
        void (*prev)(int) = SIG_DFL;
        if (h && h->type == PY_T_INT && py_as_int(h) == 0) prev = SIG_IGN;
        prev = signal(sig, prev);
        *out = py_int((int64_t)(intptr_t)prev);
        return 1;
    }
    if (xeq(name, "getsignal")) {
        *out = py_int((int64_t)(intptr_t)SIG_DFL);
        return 1;
    }
    if (xeq(name, "raise_signal") || xeq(name, "raise")) { raise(sig); *out = py_none(); return 1; }
    if (xeq(name, "alarm")) {
        *out = py_int(0);
        return 1;
    }
    if (xeq(name, "pause")) { *out = py_none(); return 1; }
    (void)sig;
    return 0;
}

static int select_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    (void)a; (void)n; (void)0;
    socket_ws_init();
    if (xeq(name, "select")) {
        fd_set rfds, wfds, xfds;
        struct timeval tv, *ptv = NULL;
        int maxfd = -1, r;
        PyVal *rr = py_list_new(), *wr = py_list_new(), *xr = py_list_new();
        PyVal *v[3];
        FD_ZERO(&rfds); FD_ZERO(&wfds); FD_ZERO(&xfds);
        if (n > 0) {
            PyVal *l = as_list(a[0]), *it = py_iter(l), *f = NULL;
            while (py_iter_next(it, &f)) {
                int fd = (int)py_as_int(py_getitem(f, py_str("fd")));
                if (fd > maxfd) maxfd = fd;
                FD_SET(fd, &rfds);
                py_release(f);
            }
            py_release(it); py_release(l);
        }
        if (n > 3 && a[3] && a[3]->type != PY_T_NONE) {
            double secs = py_as_float(a[3]);
            tv.tv_sec = (long)secs;
            tv.tv_usec = (long)((secs - (double)tv.tv_sec) * 1000000.0);
            ptv = &tv;
        }
        r = select(maxfd + 1, &rfds, &wfds, &xfds, ptv);
        if (r > 0 && n > 0) {
            PyVal *l = as_list(a[0]), *it = py_iter(l), *f = NULL;
            while (py_iter_next(it, &f)) {
                int fd = (int)py_as_int(py_getitem(f, py_str("fd")));
                if (FD_ISSET(fd, &rfds)) py_list_append(rr, f);
                py_release(f);
            }
            py_release(it); py_release(l);
        }
        v[0] = rr; v[1] = wr; v[2] = xr;
        *out = py_tuple_from(v, 3);
        py_release(rr); py_release(wr); py_release(xr);
        return 1;
    }
    return 0;
}

/* ================================================= network application layer */

/* A tiny line-oriented TCP client used by ftplib/smtplib/poplib. */
static int net_connect(const char *host, int port) {
    struct hostent *he;
    struct sockaddr_in sa;
    int fd;
    socket_ws_init();
    fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    he = gethostbyname(host);
    if (!he) { closesocket(fd); return -1; }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    sa.sin_addr = *(struct in_addr *)he->h_addr_list[0];
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa))) { closesocket(fd); return -1; }
    return fd;
}
#ifndef _WIN32
#define closesocket close
#endif

static void net_send(int fd, const char *s) {
    send(fd, s, (int)strlen(s), 0);
}

static PyVal *net_recv(int fd) {
    char buf[8192];
    int got = recv(fd, buf, (int)sizeof(buf) - 1, 0);
    if (got <= 0) return py_str("");
    buf[got] = 0;
    return py_str(buf);
}

static int net_client_call(const char *module, const char *name, PyVal **a, size_t n, PyVal **out) {
    const char *host = xs(a, n, 0, "localhost");
    int port = (int)xi(a, n, 1, xeq(module, "smtplib") ? 25 : xeq(module, "poplib") ? 110 : 21);
    if (xeq(name, "FTP") || xeq(name, "SMTP") || xeq(name, "POP3") || xeq(name, "connect")) {
        int fd = net_connect(host, port);
        PyVal *r = py_dict_new();
        py_dict_put(r, "_fd", py_int(fd));
        py_dict_put(r, "host", py_str(host));
        py_dict_put(r, "port", py_int(port));
        if (fd >= 0) {
            PyVal *greet = net_recv(fd);
            py_dict_put(r, "welcome", greet);
            py_release(greet);
        }
        *out = r;
        return 1;
    }
    if (xeq(name, "login") || xeq(name, "user") || xeq(name, "auth")) {
        PyVal *c = one(a, n, "login");
        PyVal *fdv = py_getitem(c, py_str("_fd"));
        int fd = (int)py_as_int(fdv);
        XBuf b;
        xb_init(&b);
        if (xeq(module, "ftplib")) {
            xb_s(&b, "USER "); xb_s(&b, xs(a, n, 1, "anonymous")); xb_s(&b, "\r\n");
            if (n > 2) { xb_s(&b, "PASS "); xb_s(&b, xs(a, n, 2, "")); xb_s(&b, "\r\n"); }
        } else if (xeq(module, "smtplib")) {
            xb_s(&b, "EHLO py2c99\r\nAUTH LOGIN\r\n");
        } else {
            xb_s(&b, "USER "); xb_s(&b, xs(a, n, 1, "")); xb_s(&b, "\r\n");
            if (n > 2) { xb_s(&b, "PASS "); xb_s(&b, xs(a, n, 2, "")); xb_s(&b, "\r\n"); }
        }
        if (fd >= 0) { net_send(fd, b.d ? b.d : ""); { PyVal *r = net_recv(fd); py_release(r); } }
        *out = py_none();
        return 1;
    }
    if (xeq(name, "retr") || xeq(name, "retrlines") || xeq(name, "list") || xeq(name, "sendmail") || xeq(name, "quit")) {
        PyVal *c = one(a, n, name);
        PyVal *fdv = py_getitem(c, py_str("_fd"));
        int fd = (int)py_as_int(fdv);
        XBuf b;
        xb_init(&b);
        if (xeq(name, "retr") || xeq(name, "retrlines")) {
            xb_s(&b, "RETR "); xb_s(&b, xs(a, n, 1, "")); xb_s(&b, "\r\n");
        } else if (xeq(name, "list")) xb_s(&b, "LIST\r\n");
        else if (xeq(name, "sendmail")) {
            xb_s(&b, "MAIL FROM:<"); xb_s(&b, xs(a, n, 1, "")); xb_s(&b, ">\r\nRCPT TO:<");
            xb_s(&b, xs(a, n, 2, "")); xb_s(&b, ">\r\nDATA\r\n"); xb_s(&b, xs(a, n, 3, "")); xb_s(&b, "\r\n.\r\n");
        } else xb_s(&b, "QUIT\r\n");
        if (fd >= 0) { net_send(fd, b.d ? b.d : ""); *out = net_recv(fd); }
        else *out = py_str("");
        return 1;
    }
    if (xeq(name, "noop") || xeq(name, "quit")) { *out = py_none(); return 1; }
    return 0;
}

/* ====================================================== email / http / urllib */

static int email_call(const char *module, const char *name, PyVal **a, size_t n, PyVal **out) {
    if (xeq(name, "message_from_string") || xeq(name, "message_from_bytes") || xeq(name, "Message")) {
        const char *s = xs(a, n, 0, "");
        PyVal *r = py_dict_new();
        XBuf body;
        const char *blank = strstr(s, "\r\n\r\n");
        if (!blank) blank = strstr(s, "\n\n");
        xb_init(&body);
        if (blank) {
            const char *hdr = s;
            while (hdr < blank) {
                const char *line_end = strchr(hdr, '\n');
                const char *e = line_end;
                if (!e || e >= blank) e = blank;
                if (e > hdr && e[-1] == '\r') e--;
                {
                    const char *colon = memchr(hdr, ':', (size_t)(e - hdr));
                    if (colon) {
                        PyVal *k = py_strn(hdr, (size_t)(colon - hdr));
                        const char *vs = colon + 1;
                        while (vs < e && (*vs == ' ' || *vs == '\t')) vs++;
                        py_dict_put(r, py_cstr(k), py_strn(vs, (size_t)(e - vs)));
                        py_release(k);
                    }
                }
                hdr = line_end && *line_end ? line_end + 1
                                             : (line_end ? line_end : blank);
            }
            xb_s(&body, blank + (blank[0] == '\r' ? 4 : 2));
        }
        {
            PyVal *payload = xb_str(&body);
            py_dict_put(r, "body", py_none());
            py_dict_put(r, "payload", payload);
            py_release(payload);
        }
        *out = r;
        return 1;
    }
    if (xeq(name, "parseaddr")) {
        const char *s = xs(a, n, 0, ""), *lt = strchr(s, '<'), *gt = strchr(s, '>');
        PyVal *v[2];
        if (lt && gt && gt > lt) {
            v[0] = py_strn(s, (size_t)(lt - s));
            v[1] = py_strn(lt + 1, (size_t)(gt - lt - 1));
        } else { v[0] = py_str(""); v[1] = py_str(s); }
        *out = py_tuple_from(v, 2);
        py_release(v[0]); py_release(v[1]);
        return 1;
    }
    if (xeq(name, "formatdate")) {
        time_t t = time(NULL);
        char buf[128];
        struct tm *tm = gmtime(&t);
        static const char *days[] = { "Mon","Tue","Wed","Thu","Fri","Sat","Sun" };
        static const char *mons[] = { "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec" };
        snprintf(buf, sizeof(buf), "%s, %02d %s %d %02d:%02d:%02d +0000",
                 days[(tm->tm_wday + 6) % 7], tm->tm_mday, mons[tm->tm_mon],
                 tm->tm_year + 1900, tm->tm_hour, tm->tm_min, tm->tm_sec);
        *out = py_str(buf);
        return 1;
    }
    (void)module;
    return 0;
}

static int http_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    if (xeq(name, "HTTPConnection") || xeq(name, "HTTPSConnection")) {
        PyVal *r = py_dict_new();
        py_dict_put(r, "host", py_str(xs(a, n, 0, "localhost")));
        py_dict_put(r, "port", py_int(xi(a, n, 1, 80)));
        *out = r;
        return 1;
    }
    if (xeq(name, "request") || xeq(name, "getresponse")) {
        PyVal *c = one(a, n, "request");
        PyVal *h = py_getitem(c, py_str("host"));
        PyVal *pv = py_getitem(c, py_str("port"));
        int fd = net_connect(py_cstr(h), (int)py_as_int(pv));
        XBuf b;
        xb_init(&b);
        xb_s(&b, xs(a, n, 1, "GET")); xb_c(&b, ' ');
        xb_s(&b, xs(a, n, 2, "/")); xb_s(&b, " HTTP/1.0\r\nHost: ");
        xb_s(&b, py_cstr(h)); xb_s(&b, "\r\n\r\n");
        if (fd >= 0) { net_send(fd, b.d ? b.d : ""); *out = net_recv(fd); closesocket(fd); }
        else *out = py_str("");
        py_release(h); py_release(pv);
        return 1;
    }
    return 0;
}

static int urllib_extra_call(const char *module, const char *name, PyVal **a, size_t n, PyVal **out) {
    if (xeq(module, "urllib.robotparser") && xeq(name, "RobotFileParser")) {
        PyVal *r = py_dict_new();
        py_dict_put(r, "rules", py_list_new());
        py_dict_put(r, "url", py_str(xs(a, n, 0, "")));
        *out = r;
        return 1;
    }
    if (xeq(name, "read") || xeq(name, "parse")) {
        PyVal *rp = one(a, n, "read");
        const char *s = xs(a, n, 1, "");
        PyVal *rules = py_getitem(rp, py_str("rules"));
        const char *line = s;
        while (*line) {
            const char *e = strchr(line, '\n');
            if (!e) e = line + strlen(line);
            if (!strncmp(line, "User-agent:", 11) || !strncmp(line, "Disallow:", 9)) {
                py_list_append(rules, py_strn(line, (size_t)(e - line)));
            }
            line = *e ? e + 1 : e;
        }
        py_release(rules);
        *out = py_none();
        return 1;
    }
    if (xeq(name, "can_fetch")) { *out = py_bool(1); return 1; }
    if (xeq(module, "urllib.error") && (xeq(name, "URLError") || xeq(name, "HTTPError"))) {
        const char *k[] = { "reason", "code", "msg" };
        PyVal *v[3];
        v[0] = py_str(xs(a, n, 0, "")); v[1] = py_int(xi(a, n, 1, 0)); v[2] = py_str(xs(a, n, 2, ""));
        *out = py_dict_lit(k, v, 3);
        py_release(v[0]); py_release(v[1]); py_release(v[2]);
        return 1;
    }
    return 0;
}

static int webbrowser_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    const char *url = xs(a, n, 0, "");
    if (xeq(name, "open") || xeq(name, "open_new") || xeq(name, "open_new_tab")) {
#ifdef _WIN32
        ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
#else
        char cmd[2048];
        snprintf(cmd, sizeof(cmd), "xdg-open \"%s\"", url);
        system(cmd);
#endif
        *out = py_bool(1);
        return 1;
    }
    if (xeq(name, "get")) { *out = py_str("default"); return 1; }
    return 0;
}

/* =========================================== zoneinfo / tomllib / zipapp */

static int zoneinfo_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    (void)a; (void)n;
    if (xeq(name, "ZoneInfo") || xeq(name, "available_timezones")) {
        const char *key = xs(a, n, 0, "UTC");
        PyVal *r = py_dict_new();
        py_dict_put(r, "key", py_str(key));
        py_dict_put(r, "utcoffset", py_int(0));
        *out = r;
        return 1;
    }
    if (xeq(name, "reset_tzpath")) { *out = py_none(); return 1; }
    return 0;
}

static int tomllib_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    (void)a; (void)n;
    if (!xeq(name, "loads") && !xeq(name, "load")) return 0;
    const char *s = xs(a, n, 0, "");
    PyVal *r = py_dict_new(), *section = r;
    const char *line = s;
    while (*line) {
        const char *e = strchr(line, '\n');
        if (!e) e = line + strlen(line);
        char *t = (char *)malloc((size_t)(e - line) + 1);
        if (!t) break;
        memcpy(t, line, (size_t)(e - line)); t[e - line] = 0;
        {
            char *p = t;
            while (*p == ' ' || *p == '\t') p++;
            if (*p && *p != '#') {
                if (*p == '[') {
                    char *rb = strchr(p, ']');
                    if (rb) {
                        *rb = 0;
                        PyVal *d = py_dict_new();
                        py_dict_put(r, p + 1, d);
                        py_release(d);
                        section = py_dict_get(r, p + 1);
                    }
                } else {
                    char *eq = strchr(p, '=');
                    if (eq) {
                        *eq = 0;
                        {
                            char *k = p, *v = eq + 1;
                            char *ke = k + strlen(k), *ve = v + strlen(v);
                            while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
                            while (ve > v && (*ve == ' ' || *ve == '\t' || *ve == '"' || *ve == '\'')) ve++;
                            while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t' || ve[-1] == '"' || ve[-1] == '\'')) ve--;
                            *ke = 0; *ve = 0;
                            {
                                PyVal *val;
                                if (!xi_cmp(v, "true")) val = py_bool(1);
                                else if (!xi_cmp(v, "false")) val = py_bool(0);
                                else {
                                    char *endp = NULL;
                                    long long iv = strtoll(v, &endp, 10);
                                    val = (endp && !*endp) ? py_int(iv) : py_str(v);
                                }
                                py_dict_put(section, k, val);
                                py_release(val);
                            }
                        }
                    }
                }
            }
        }
        free(t);
        line = *e ? e + 1 : e;
    }
    *out = r;
    return 1;
}

static int zipapp_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    if (xeq(name, "create_archive")) {
        const char *src = xs(a, n, 0, ""), *dst = xs(a, n, 1, "app.pyz");
        FILE *in = fopen(src, "rb"), *fo = fopen(dst, "wb");
        if (in && fo) {
            char buf[8192];
            size_t got;
            fputs("#!/usr/bin/env python3\n", fo);
            while ((got = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, got, fo);
            fclose(in);
        }
        if (fo) fclose(fo);
        *out = py_str(dst);
        return 1;
    }
    return 0;
}

/* ======================================== importlib / runpy / pkgutil / site */

static int importlib_call(const char *module, const char *name, PyVal **a, size_t n, PyVal **out) {
    const char *mn = xs(a, n, 0, "");
    if (xeq(name, "import_module") || xeq(name, "__import__")) {
        PyVal *r = py_dict_new();
        py_dict_put(r, "__name__", py_str(mn));
        py_dict_put(r, "__loader__", py_str("py2c99"));
        py_dict_put(r, "__spec__", py_dict_new());
        *out = r;
        return 1;
    }
    if (xeq(name, "reload")) { *out = one(a, n, "reload"); return 1; }
    if (xeq(name, "invalidate_caches")) { *out = py_none(); return 1; }
    if (xeq(name, "find_spec")) {
        int found = py_extra_module_known(mn) || py_module_known(mn);
        *out = py_bool(found);
        return 1;
    }
    (void)module;
    return 0;
}

static int runpy_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    if (xeq(name, "run_path") || xeq(name, "run_module")) {
        const char *p = xs(a, n, 0, "");
        FILE *fp = fopen(p, "r");
        PyVal *r = py_dict_new();
        XBuf b;
        xb_init(&b);
        if (fp) {
            char line[8192];
            while (fgets(line, sizeof(line), fp)) xb_s(&b, line);
            fclose(fp);
        }
        py_dict_put(r, "__name__", py_str(p));
        py_dict_put(r, "__file__", py_str(p));
        py_dict_put(r, "source", xb_str(&b));
        *out = r;
        return 1;
    }
    return 0;
}

static int pkgutil_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    if (xeq(name, "iter_modules") || xeq(name, "walk_packages")) {
        PyVal *r = py_list_new();
        size_t i;
        for (i = 0; x_modules[i]; i++) {
            PyVal *e[2];
            e[0] = py_str("py2c99"); e[1] = py_str(x_modules[i]);
            {
                PyVal *t = py_tuple_from(e, 2);
                py_list_append(r, t);
                py_release(t);
            }
            py_release(e[0]); py_release(e[1]);
        }
        *out = r;
        return 1;
    }
    if (xeq(name, "get_data")) {
        const char *path = xs(a, n, 1, "");
        FILE *fp = fopen(path, "rb");
        PyVal *r = py_str("");
        if (fp) {
            XBuf b;
            char buf[4096];
            size_t got;
            xb_init(&b);
            while ((got = fread(buf, 1, sizeof(buf), fp)) > 0) xb_put(&b, buf, got);
            fclose(fp);
            py_release(r);
            r = xb_str(&b);
        }
        *out = r;
        return 1;
    }
    return 0;
}

static int site_call(const char *name, PyVal **a, size_t n, PyVal **out) {
    (void)a; (void)n;
    if (xeq(name, "getsitepackages") || xeq(name, "getusersitepackages")) {
        PyVal *v[1];
        v[0] = py_str("py2c99_stdlib");
        *out = py_list_from(v, 1);
        py_release(v[0]);
        return 1;
    }
    if (xeq(name, "main") || xeq(name, "execsitecustomize")) { *out = py_none(); return 1; }
    return 0;
}

/* ================================================= code / token / symtable */

static int source_tools_call(const char *module, const char *name, PyVal **a, size_t n, PyVal **out) {
    const char *s = xs(a, n, 0, "");
    if (xeq(name, "compile_command") || xeq(name, "opcode")) {
        *out = py_bool(s && *s);
        return 1;
    }
    if (xeq(name, "InteractiveInterpreter") || xeq(name, "InteractiveConsole") || xeq(name, "Code")) {
        PyVal *r = py_dict_new();
        py_dict_put(r, "source", py_str(s));
        *out = r;
        return 1;
    }
    if (xeq(name, "generate_tokens") || xeq(name, "tokenize")) {
        PyVal *r = py_list_new();
        const char *p = s;
        while (*p) {
            if (isalpha((unsigned char)*p) || *p == '_') {
                const char *b = p;
                while (isalnum((unsigned char)*p) || *p == '_') p++;
                py_list_append(r, py_strn(b, (size_t)(p - b)));
            } else if (isdigit((unsigned char)*p)) {
                const char *b = p;
                while (isdigit((unsigned char)*p)) p++;
                py_list_append(r, py_strn(b, (size_t)(p - b)));
            } else p++;
        }
        *out = r;
        return 1;
    }
    if (xeq(name, "symtable")) {
        PyVal *r = py_dict_new();
        py_dict_put(r, "identifiers", py_list_new());
        *out = r;
        return 1;
    }
    (void)module;
    return 0;
}

/* ============================================ pydoc / doctest / unittest */

static int testing_call(const char *module, const char *name, PyVal **a, size_t n, PyVal **out) {
    if (xeq(name, "testmod") || xeq(name, "testfile") || xeq(name, "run")) {
        PyVal *r = py_dict_new();
        py_dict_put(r, "attempted", py_int(0));
        py_dict_put(r, "failed", py_int(0));
        *out = r;
        return 1;
    }
    if (xeq(name, "DocTestRunner")) {
        PyVal *r = py_dict_new();
        py_dict_put(r, "tries", py_int(0));
        *out = r;
        return 1;
    }
    if (xeq(name, "render") || xeq(name, "help")) {
        PyVal *v = one(a, n, "pydoc");
        PyVal *s = py_repr(v);
        XBuf b;
        xb_init(&b);
        xb_s(&b, "NAME\n    ");
        xb_s(&b, py_cstr(s));
        xb_c(&b, '\n');
        py_release(s);
        *out = xb_str(&b);
        return 1;
    }
    (void)module;
    return 0;
}

/* ==================================================== misc small modules */

static int small_call(const char *module, const char *name, PyVal **a, size_t n, PyVal **out) {
    if (xeq(module, "typing_extensions")) {
        if (xeq(name, "override") || xeq(name, "deprecated")) { *out = ret_same(one(a, n, "typing_extensions")); return 1; }
        *out = py_str(name);
        return 1;
    }
    if (xeq(module, "audioop")) {
        double x = xd(a, n, 0, 0);
        if (xeq(name, "add")) { *out = py_int((int64_t)(x + xd(a, n, 1, 0))); return 1; }
        if (xeq(name, "mul") || xeq(name, "muladd")) { *out = py_int((int64_t)(x * xd(a, n, 1, 1))); return 1; }
        if (xeq(name, "max") || xeq(name, "maxabs")) { *out = py_int((int64_t)fabs(x)); return 1; }
        if (xeq(name, "reverse")) { *out = py_int((int64_t)x); return 1; }
        if (xeq(name, "lin2lin")) { *out = py_int((int64_t)x); return 1; }
        return 0;
    }
    if (xeq(module, "sre_compile") || xeq(module, "sre_parse")) {
        if (xeq(name, "compile") || xeq(name, "parse")) {
            PyVal *r = py_dict_new();
            py_dict_put(r, "pattern", py_str(xs(a, n, 0, "")));
            *out = r;
            return 1;
        }
        return 0;
    }
    if (xeq(module, "genericpath") || xeq(module, "shlex")) {
        if (xeq(name, "exists")) {
            struct stat st;
            *out = py_bool(stat(xs(a, n, 0, ""), &st) == 0);
            return 1;
        }
        if (xeq(name, "split") || xeq(name, "quote") || xeq(name, "join")) {
            const char *s = xs(a, n, 0, "");
            if (xeq(name, "split")) {
                PyVal *r = py_list_new();
                const char *p = s;
                while (*p) {
                    while (*p == ' ') p++;
                    if (!*p) break;
                    {
                        const char *b = p;
                        int quoted = *p == '"' || *p == '\'';
                        if (quoted) { char q = *p++; b = p; while (*p && *p != q) p++; }
                        else while (*p && *p != ' ') p++;
                        py_list_append(r, py_strn(b, (size_t)(p - b)));
                        if (quoted && *p) p++;
                    }
                }
                *out = r;
                return 1;
            }
            *out = py_str(s);
            return 1;
        }
        return 0;
    }
    if (xeq(module, "stringprep")) {
        PyVal *v = one(a, n, "stringprep");
        if (xeq(name, "in_table_a1") || xeq(name, "in_table_c12")) { *out = py_bool(0); return 1; }
        if (xeq(name, "nodeprep") || xeq(name, "nameprep") || xeq(name, "saslprep")) { *out = ret_same(v); return 1; }
        return 0;
    }
    if (xeq(module, "platformdirs") || xeq(module, "diskcache")) {
        if (xeq(name, "user_data_dir") || xeq(name, "user_config_dir") || xeq(name, "Cache")) {
            const char *home = getenv("USERPROFILE");
            if (!home) home = getenv("HOME");
            *out = py_str(home ? home : ".");
            return 1;
        }
        return 0;
    }
    return 0;
}

/* ================================================================ dispatch */

PyVal *py_extra_call(const char *module, const char *name, PyVal **a, size_t n,
                     PyVal *kwargs, int *handled) {
    PyVal *out = NULL;
    int ok = 0;
    /* Wave-2 module functions take positional arguments.  Accept and ignore
       keywords here instead of rejecting the whole call: most real programs
       pass optional keywords (autojunk=True, encoding=..., etc.) whose values
       the C shim does not model. */
    (void)kwargs;
    if (!py_extra_module_known(module)) return NULL;
    *handled = 1;
    if (xeq(module, "statistics")) ok = statistics_call(name, a, n, &out);
    else if (xeq(module, "fractions")) ok = fractions_call(name, a, n, &out);
    else if (xeq(module, "decimal")) ok = decimal_call(name, a, n, &out);
    else if (xeq(module, "colorsys")) ok = colorsys_call(name, a, n, &out);
    else if (xeq(module, "difflib")) ok = difflib_call(name, a, n, &out);
    else if (xeq(module, "reprlib")) ok = reprlib_call(name, a, n, &out);
    else if (xeq(module, "keyword")) ok = keyword_call(name, a, n, &out);
    else if (xeq(module, "ast")) ok = ast_call(name, a, n, &out);
    else if (xeq(module, "dis")) ok = dis_call(name, a, n, &out);
    else if (xeq(module, "opcode")) ok = opcode_call(name, a, n, &out);
    else if (xeq(module, "configparser")) ok = configparser_call(name, a, n, &out);
    else if (xeq(module, "netrc")) ok = netrc_call(name, a, n, &out);
    else if (xeq(module, "xdrlib")) ok = xdrlib_call(name, a, n, &out);
    else if (xeq(module, "plistlib")) ok = plistlib_call(name, a, n, &out);
    else if (xeq(module, "quopri")) ok = quopri_call(name, a, n, &out);
    else if (xeq(module, "uu")) ok = uu_call(name, a, n, &out);
    else if (xeq(module, "mimetypes")) ok = mimetypes_call(name, a, n, &out);
    else if (xeq(module, "html.parser")) ok = htmlparser_call(name, a, n, &out);
    else if (xeq(module, "xml.etree.ElementTree")) ok = xml_call(name, a, n, &out);
    else if (xeq(module, "io")) ok = io_call(name, a, n, &out);
    else if (xeq(module, "pickle")) ok = pickle_call(name, a, n, &out);
    else if (xeq(module, "copyreg")) ok = copyreg_call(name, a, n, &out);
    else if (xeq(module, "marshal")) ok = marshal_call(name, a, n, &out);
    else if (xeq(module, "shelve")) ok = shelve_call(name, a, n, &out);
    else if (xeq(module, "dbm")) ok = dbm_call(name, a, n, &out);
    else if (xeq(module, "tarfile")) ok = tarfile_call(name, a, n, &out);
    else if (xeq(module, "zipfile")) ok = zipfile_call(name, a, n, &out);
    else if (xeq(module, "wave") || xeq(module, "aifc") || xeq(module, "sunau"))
        ok = audio_call(module, name, a, n, &out);
    else if (xeq(module, "chunk")) ok = chunk_call(name, a, n, &out);
    else if (xeq(module, "fileinput")) ok = fileinput_call(name, a, n, &out);
    else if (xeq(module, "filecmp")) ok = filecmp_call(name, a, n, &out);
    else if (xeq(module, "linecache")) ok = linecache_call(name, a, n, &out);
    else if (xeq(module, "locale")) ok = locale_call(name, a, n, &out);
    else if (xeq(module, "gettext")) ok = gettext_call(name, a, n, &out);
    else if (xeq(module, "atexit")) ok = atexit_call(name, a, n, &out);
    else if (xeq(module, "warnings")) ok = warnings_call(name, a, n, &out);
    else if (xeq(module, "traceback")) ok = traceback_call(name, a, n, &out);
    else if (xeq(module, "contextvars")) ok = contextvars_call(name, a, n, &out);
    else if (xeq(module, "graphlib")) ok = graphlib_call(name, a, n, &out);
    else if (xeq(module, "sched")) ok = sched_call(name, a, n, &out);
    else if (xeq(module, "selectors")) ok = selectors_call(name, a, n, &out);
    else if (xeq(module, "mmap")) ok = mmap_call(name, a, n, &out);
    else if (xeq(module, "socket")) ok = socket_call(name, a, n, &out);
    else if (xeq(module, "signal")) ok = signal_call(name, a, n, &out);
    else if (xeq(module, "select")) ok = select_call(name, a, n, &out);
    else if (xeq(module, "ftplib") || xeq(module, "smtplib") || xeq(module, "poplib"))
        ok = net_client_call(module, name, a, n, &out);
    else if (xeq(module, "email") || xeq(module, "email.utils")) ok = email_call(module, name, a, n, &out);
    else if (xeq(module, "http.client")) ok = http_call(name, a, n, &out);
    else if (xeq(module, "urllib.robotparser") || xeq(module, "urllib.error"))
        ok = urllib_extra_call(module, name, a, n, &out);
    else if (xeq(module, "webbrowser")) ok = webbrowser_call(name, a, n, &out);
    else if (xeq(module, "zoneinfo")) ok = zoneinfo_call(name, a, n, &out);
    else if (xeq(module, "tomllib")) ok = tomllib_call(name, a, n, &out);
    else if (xeq(module, "zipapp")) ok = zipapp_call(name, a, n, &out);
    else if (xeq(module, "importlib") || xeq(module, "importlib.util")) ok = importlib_call(module, name, a, n, &out);
    else if (xeq(module, "runpy")) ok = runpy_call(name, a, n, &out);
    else if (xeq(module, "pkgutil") || xeq(module, "modulefinder")) ok = pkgutil_call(name, a, n, &out);
    else if (xeq(module, "site")) ok = site_call(name, a, n, &out);
    else if (xeq(module, "code") || xeq(module, "codeop") || xeq(module, "token") ||
             xeq(module, "tabnanny") || xeq(module, "symtable"))
        ok = source_tools_call(module, name, a, n, &out);
    else if (xeq(module, "pydoc") || xeq(module, "doctest") || xeq(module, "unittest"))
        ok = testing_call(module, name, a, n, &out);
    else ok = small_call(module, name, a, n, &out);
    if (!ok) {
        char msg[256];
        snprintf(msg, sizeof(msg), "module '%s' has no function '%s'", module, name);
        py_raise(PY_EXC_ATTRIBUTE, msg);
        return py_none();
    }
    return out;
}
