/*
 * py2c99 runtime core.  Pure C99, no libpython.
 */
#define _CRT_SECURE_NO_WARNINGS
#include "pyrt.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_E
#define M_E 2.71828182845904523536
#endif

/* ------------------------------------------------------------------ state */
static PyVal g_none;
static PyVal g_true;
static PyVal g_false;
static int g_initialized = 0;

static PyJmp *g_handler = NULL;
static PyVal *g_exc = NULL;

static PyVal *g_argv = NULL;

static PyVal *file_method(PyVal *f, const char *name, PyVal **args, size_t argc,
                          PyVal *kwargs);
static PyVal *file_readline(PyVal *f);

/* ---------------------------------------------------------------- buffers */
typedef struct {
    char *data;
    size_t len, cap;
} Buf;

static void buf_init(Buf *b) {
    b->data = NULL;
    b->len = b->cap = 0;
}

static void buf_reserve(Buf *b, size_t extra) {
    if (b->len + extra + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 64;
        while (nc < b->len + extra + 1) nc *= 2;
        b->data = (char *)realloc(b->data, nc);
        if (!b->data) {
            fputs("py2c99: out of memory\n", stderr);
            exit(1);
        }
        b->cap = nc;
    }
}

static void buf_add(Buf *b, const char *s, size_t n) {
    buf_reserve(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}

static void buf_adds(Buf *b, const char *s) { buf_add(b, s, strlen(s)); }
static void buf_addc(Buf *b, char c) { buf_add(b, &c, 1); }

static void buf_printf(Buf *b, const char *fmt, ...) {
    va_list ap, ap2;
    int n;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return; }
    buf_reserve(b, (size_t)n);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
}

/* -------------------------------------------------------------- utf8 utils */
static int u8_iscont(unsigned char c) { return (c & 0xC0) == 0x80; }

static size_t u8_len(const char *s, size_t n) {
    size_t i, count = 0;
    for (i = 0; i < n; i++)
        if (!u8_iscont((unsigned char)s[i])) count++;
    return count;
}

/* byte offset of codepoint index (0..count) */
static size_t u8_offset(const char *s, size_t n, size_t index) {
    size_t i, count = 0;
    if (index == 0) return 0;
    for (i = 0; i < n; i++) {
        if (!u8_iscont((unsigned char)s[i])) {
            if (count == index) return i;
            count++;
        }
    }
    return n;
}

static uint32_t u8_decode(const char *s, size_t n, size_t *adv) {
    const unsigned char *p = (const unsigned char *)s;
    uint32_t cp;
    size_t i = 1;
    if (n == 0) { *adv = 0; return 0; }
    if (p[0] < 0x80) { *adv = 1; return p[0]; }
    if ((p[0] & 0xE0) == 0xC0) { cp = p[0] & 0x1F; i = 2; }
    else if ((p[0] & 0xF0) == 0xE0) { cp = p[0] & 0x0F; i = 3; }
    else if ((p[0] & 0xF8) == 0xF0) { cp = p[0] & 0x07; i = 4; }
    else { *adv = 1; return p[0]; }
    if (i > n) { *adv = 1; return p[0]; }
    {
        size_t k;
        for (k = 1; k < i; k++) {
            if (!u8_iscont(p[k])) { *adv = 1; return p[0]; }
            cp = (cp << 6) | (p[k] & 0x3F);
        }
    }
    *adv = i;
    return cp;
}

static void u8_encode(uint32_t cp, char *out, size_t *len) {
    if (cp < 0x80) { out[0] = (char)cp; *len = 1; }
    else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        *len = 2;
    } else if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        *len = 3;
    } else {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        *len = 4;
    }
}

/* ------------------------------------------------------------- allocation */
static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) {
        fputs("py2c99: out of memory\n", stderr);
        exit(1);
    }
    return p;
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fputs("py2c99: out of memory\n", stderr);
        exit(1);
    }
    return q;
}

static PyVal *val_new(PyType t) {
    PyVal *v = (PyVal *)xmalloc(sizeof(PyVal));
    memset(v, 0, sizeof(*v));
    v->type = t;
    v->refs = 1;
    return v;
}

static void reserve_seq(PyVal *v, size_t extra) {
    if (v->u.seq.len + extra > v->u.seq.cap) {
        size_t nc = v->u.seq.cap ? v->u.seq.cap * 2 : 8;
        while (nc < v->u.seq.len + extra) nc *= 2;
        v->u.seq.items = (PyVal **)xrealloc(v->u.seq.items, nc * sizeof(PyVal *));
        v->u.seq.cap = nc;
    }
}

static void seq_push(PyVal *v, PyVal *item) {
    reserve_seq(v, 1);
    py_retain(item);
    v->u.seq.items[v->u.seq.len++] = item;
}

void py_list_append(PyVal *list, PyVal *item) {
    if (!list) return;
    if (list->type != PY_T_LIST && list->type != PY_T_TUPLE) return;
    seq_push(list, item);
}

static void map_reserve(PyVal *v, size_t extra) {
    if (v->u.map.len + extra > v->u.map.cap) {
        size_t nc = v->u.map.cap ? v->u.map.cap * 2 : 8;
        while (nc < v->u.map.len + extra) nc *= 2;
        v->u.map.keys = (PyVal **)xrealloc(v->u.map.keys, nc * sizeof(PyVal *));
        v->u.map.vals = (PyVal **)xrealloc(v->u.map.vals, nc * sizeof(PyVal *));
        v->u.map.cap = nc;
    }
}

/* -------------------------------------------------------------- singletons */
PyVal *py_none(void) { return &g_none; }
PyVal *py_bool(int b) { return b ? &g_true : &g_false; }
PyVal *py_int(int64_t i) {
    PyVal *v = val_new(PY_T_INT);
    v->u.i = i;
    return v;
}
PyVal *py_float(double f) {
    PyVal *v = val_new(PY_T_FLOAT);
    v->u.f = f;
    return v;
}
PyVal *py_strn(const char *s, size_t n) {
    PyVal *v = val_new(PY_T_STR);
    v->u.s.data = (char *)xmalloc(n + 1);
    if (n && s) memcpy(v->u.s.data, s, n);
    v->u.s.data[n] = 0;
    v->u.s.len = n;
    return v;
}
PyVal *py_str(const char *s) { return py_strn(s ? s : "", s ? strlen(s) : 0); }

PyVal *py_list_new(void) { return val_new(PY_T_LIST); }
PyVal *py_dict_view(PyVal *dict, int kind) {
    PyVal *v = val_new(PY_T_DICTVIEW);
    v->u.view.dict = dict;
    v->u.view.kind = kind;
    py_retain(dict);
    return v;
}

PyVal *py_str_join(PyVal **parts, size_t n) {
    Buf b;
    size_t i;
    PyVal *r;
    buf_init(&b);
    for (i = 0; i < n; i++) {
        if (!parts[i]) continue;
        if (parts[i]->type != PY_T_STR) py_raise(PY_EXC_TYPE, "can only join str parts");
        buf_add(&b, parts[i]->u.s.data, parts[i]->u.s.len);
    }
    r = py_strn(b.data ? b.data : "", b.len);
    free(b.data);
    return r;
}

PyVal *py_slice_new(PyVal *start, PyVal *stop, PyVal *step) {
    PyVal *v = val_new(PY_T_SLICE);
    v->u.slice.start = start; py_retain(start);
    v->u.slice.stop = stop; py_retain(stop);
    v->u.slice.step = step; py_retain(step);
    return v;
}

PyVal *py_set_new(void) { return val_new(PY_T_SET); }
PyVal *py_dict_new(void) { return val_new(PY_T_DICT); }

PyVal *py_list_from(PyVal **items, size_t n) {
    PyVal *v = py_list_new();
    size_t i;
    for (i = 0; i < n; i++) seq_push(v, items[i]);
    return v;
}
PyVal *py_tuple_from(PyVal **items, size_t n) {
    PyVal *v = val_new(PY_T_TUPLE);
    size_t i;
    for (i = 0; i < n; i++) seq_push(v, items[i]);
    return v;
}
PyVal *py_range(int64_t start, int64_t stop, int64_t step) {
    PyVal *v;
    if (step == 0) py_raise(PY_EXC_VALUE, "range() arg 3 must not be zero");
    v = val_new(PY_T_RANGE);
    v->u.range.start = start;
    v->u.range.stop = stop;
    v->u.range.step = step;
    v->u.range.idx = 0;
    return v;
}
PyVal *py_func(PyCFunc fn, const char *name) { return py_func_ctx(fn, name, NULL); }
PyVal *py_func_ctx(PyCFunc fn, const char *name, PyVal *ctx) {
    PyVal *v = val_new(PY_T_FUNC);
    v->u.func.fn = fn;
    v->u.func.name = name ? name : "<function>";
    v->u.func.ctx = ctx;
    py_retain(ctx);
    return v;
}
PyVal *py_bound(PyVal *fn, PyVal *self) {
    PyVal *v = val_new(PY_T_BOUND);
    v->u.bound.fn = fn;
    v->u.bound.self = self;
    py_retain(fn);
    py_retain(self);
    return v;
}
PyVal *py_var(PyVal *value, const char *name) {
    PyVal *v = val_new(PY_T_VAR);
    v->u.var.value = value;
    v->u.var.name = py_str(name ? name : "");
    py_retain(value);
    return v;
}
PyVal *py_class(PyVal *dict, const char *name) {
    PyVal *v = val_new(PY_T_CLASS);
    v->u.klass.dict = dict;
    v->u.klass.name = py_str(name ? name : "?");
    py_retain(dict);
    return v;
}
void py_class_inherit(PyVal *dict, PyVal *base) {
    size_t i;
    if (!dict || dict->type != PY_T_DICT) return;
    if (!base || base->type != PY_T_CLASS) return;
    for (i = 0; i < base->u.klass.dict->u.map.len; i++) {
        PyVal *k = base->u.klass.dict->u.map.keys[i];
        PyVal *v = base->u.klass.dict->u.map.vals[i];
        if (k && k->type == PY_T_STR)
            py_dict_put(dict, k->u.s.data, v);
    }
}
PyVal *py_widget_value(PyWidget *w) {
    PyVal *v = val_new(PY_T_WIDGET);
    v->u.widget.w = w;
    return v;
}
PyVal *py_guimethod(PyWidget *w, const char *name) {
    PyVal *v = val_new(PY_T_GUIMETHOD);
    size_t n = strlen(name ? name : "");
    v->u.gm.w = w;
    v->u.gm.name = (char *)xmalloc(n + 1);
    memcpy(v->u.gm.name, name ? name : "", n + 1);
    return v;
}

/* ---------------------------------------------------------------- release */
void py_release_impl(PyVal *v) {
    size_t i;
    if (!v || v == &g_none || v == &g_true || v == &g_false) return;
    switch (v->type) {
    case PY_T_STR:
        free(v->u.s.data);
        break;
    case PY_T_LIST:
    case PY_T_TUPLE:
        for (i = 0; i < v->u.seq.len; i++) py_release(v->u.seq.items[i]);
        free(v->u.seq.items);
        break;
    case PY_T_DICT:
    case PY_T_SET:
        for (i = 0; i < v->u.map.len; i++) {
            py_release(v->u.map.keys[i]);
            py_release(v->u.map.vals[i]);
        }
        free(v->u.map.keys);
        free(v->u.map.vals);
        break;
    case PY_T_BOUND:
        py_release(v->u.bound.fn);
        py_release(v->u.bound.self);
        break;
    case PY_T_FUNC:
        py_release(v->u.func.ctx);
        break;
    case PY_T_ITER:
        if (v->u.iter.it) {
            py_release(v->u.iter.it->src);
            py_release(v->u.iter.it->extra);
            free(v->u.iter.it);
        }
        break;
    case PY_T_WIDGET:
        if (v->u.widget.w) py_gui_free(v->u.widget.w);
        break;
    case PY_T_FILE:
        if (v->u.file.fp) fclose(v->u.file.fp);
        break;
    case PY_T_EXC:
        free(v->u.exc.msg);
        break;
    case PY_T_VAR:
        py_release(v->u.var.value);
        py_release(v->u.var.name);
        break;
    case PY_T_CLASS:
        py_release(v->u.klass.dict);
        py_release(v->u.klass.name);
        break;
    case PY_T_MODFUNC:
        free(v->u.modfunc.module);
        free(v->u.modfunc.name);
        break;
    case PY_T_GUIMETHOD:
        free(v->u.gm.name);
        break;
    case PY_T_DICTVIEW:
        py_release(v->u.view.dict);
        break;
    case PY_T_SLICE:
        py_release(v->u.slice.start);
        py_release(v->u.slice.stop);
        py_release(v->u.slice.step);
        break;
    default:
        break;
    }
    free(v);
}

void py_retain_impl(PyVal *v) { if (v) v->refs++; }

/* ------------------------------------------------------------------ assign */
void py_assign(PyVal **slot, PyVal *value) {
    PyVal *old;
    if (!slot) return;
    py_retain(value);
    old = *slot;
    *slot = value;
    py_release(old);
}

/* --------------------------------------------------------------- accessors */
size_t py_len(PyVal *v) {
    int64_t n;
    if (!v) return 0;
    switch (v->type) {
    case PY_T_STR: return u8_len(v->u.s.data, v->u.s.len);
    case PY_T_LIST: case PY_T_TUPLE: return v->u.seq.len;
    case PY_T_DICT: case PY_T_SET: return v->u.map.len;
    case PY_T_DICTVIEW: return v->u.view.dict->u.map.len;
    case PY_T_RANGE:
        n = v->u.range.stop - v->u.range.start;
        if (v->u.range.step > 0) {
            if (n <= 0) return 0;
            return (size_t)((n + v->u.range.step - 1) / v->u.range.step);
        } else {
            if (n >= 0) return 0;
            return (size_t)((-n + (-v->u.range.step) - 1) / (-v->u.range.step));
        }
    default:
        py_raise(PY_EXC_TYPE, "object has no len()");
    }
    return 0;
}

int py_truth(PyVal *v) {
    if (!v || v->type == PY_T_NONE) return 0;
    switch (v->type) {
    case PY_T_BOOL: return v->u.b != 0;
    case PY_T_INT: return v->u.i != 0;
    case PY_T_FLOAT: return v->u.f != 0.0;
    case PY_T_STR: return v->u.s.len != 0;
    case PY_T_LIST: case PY_T_TUPLE: return v->u.seq.len != 0;
    case PY_T_DICT: case PY_T_SET: return v->u.map.len != 0;
    case PY_T_DICTVIEW: return py_len(v) != 0;
    case PY_T_RANGE: return py_len(v) != 0;
    case PY_T_VAR: return py_truth(v->u.var.value);
    default: return 1;
    }
}

int py_is_none(PyVal *v) { return !v || v->type == PY_T_NONE; }
int py_is_widget(PyVal *v) { return v && v->type == PY_T_WIDGET; }

/* -------------------------------------------------------------- dict core */
static int str_eq(PyVal *a, PyVal *b) {
    return a->u.s.len == b->u.s.len &&
           memcmp(a->u.s.data, b->u.s.data, a->u.s.len) == 0;
}

int py_cmp(int op, PyVal *a, PyVal *b); /* forward */

static ptrdiff_t map_index(PyVal *d, PyVal *key) {
    size_t i;
    for (i = 0; i < d->u.map.len; i++) {
        PyVal *k = d->u.map.keys[i];
        if (key->type == PY_T_STR && k->type == PY_T_STR) {
            if (str_eq(key, k)) return (ptrdiff_t)i;
        } else if (py_cmp(PY_CMP_EQ, key, k)) {
            return (ptrdiff_t)i;
        }
    }
    return -1;
}

static void map_put(PyVal *d, PyVal *key, PyVal *value) {
    ptrdiff_t at = map_index(d, key);
    if (at >= 0) {
        py_retain(value);
        py_release(d->u.map.vals[at]);
        d->u.map.vals[at] = value;
        return;
    }
    map_reserve(d, 1);
    py_retain(key);
    py_retain(value);
    d->u.map.keys[d->u.map.len] = key;
    d->u.map.vals[d->u.map.len] = value;
    d->u.map.len++;
}

static PyVal *map_get(PyVal *d, PyVal *key) {
    ptrdiff_t at = map_index(d, key);
    return at >= 0 ? d->u.map.vals[at] : NULL;
}

static int map_del(PyVal *d, PyVal *key) {
    ptrdiff_t at = map_index(d, key);
    size_t i;
    if (at < 0) return 0;
    py_release(d->u.map.keys[at]);
    py_release(d->u.map.vals[at]);
    for (i = (size_t)at; i + 1 < d->u.map.len; i++) {
        d->u.map.keys[i] = d->u.map.keys[i + 1];
        d->u.map.vals[i] = d->u.map.vals[i + 1];
    }
    d->u.map.len--;
    return 1;
}

void py_dict_strkey(PyVal *d, PyVal *key, PyVal *value) {
    map_put(d, key, value);
}

void py_dict_put(PyVal *d, const char *key, PyVal *value) {
    PyVal *k = py_str(key);
    map_put(d, k, value);
    py_release(k);
}

PyVal *py_dict_get(PyVal *d, const char *key) {
    PyVal *k, *r;
    if (!d || (d->type != PY_T_DICT && d->type != PY_T_SET)) return NULL;
    k = py_str(key);
    r = map_get(d, k);
    py_release(k);
    return r;
}

PyVal *py_kw_get(PyVal *kwargs, const char *name) {
    size_t i, n;
    if (!kwargs || kwargs->type != PY_T_DICT) return NULL;
    n = strlen(name);
    for (i = 0; i < kwargs->u.map.len; i++) {
        PyVal *k = kwargs->u.map.keys[i];
        if (k->type == PY_T_STR && k->u.s.len == n && memcmp(k->u.s.data, name, n) == 0)
            return kwargs->u.map.vals[i];
    }
    return NULL;
}

PyVal *py_dict_lit(const char **keys, PyVal **vals, size_t n) {
    PyVal *d = py_dict_new();
    size_t i;
    for (i = 0; i < n; i++) py_dict_put(d, keys[i], vals[i]);
    return d;
}

void py_set_add(PyVal *s, PyVal *value) { map_put(s, value, py_bool(1)); }
int py_set_has(PyVal *s, PyVal *value) { return map_index(s, value) >= 0; }
int py_set_discard(PyVal *s, PyVal *value) { return map_del(s, value); }

/* ------------------------------------------------------------- get/set item */
PyVal *py_getitem(PyVal *obj, PyVal *key) {
    if (!obj) py_raise(PY_EXC_TYPE, "NoneType is not subscriptable");
    if (key && key->type == PY_T_SLICE)
        return py_getslice(obj, key->u.slice.start, key->u.slice.stop, key->u.slice.step);
    switch (obj->type) {
    case PY_T_DICT: {
        PyVal *r = map_get(obj, key);
        if (!r) py_raise(PY_EXC_KEY, "key not found");
        py_retain(r);
        return r;
    }
    case PY_T_LIST:
    case PY_T_TUPLE: {
        int64_t i = py_as_int(key);
        int64_t n = (int64_t)obj->u.seq.len;
        if (i < 0) i += n;
        if (i < 0 || i >= n) py_raise(PY_EXC_INDEX, "list index out of range");
        py_retain(obj->u.seq.items[i]);
        return obj->u.seq.items[i];
    }
    case PY_T_STR: {
        int64_t i = py_as_int(key);
        int64_t n = (int64_t)py_len(obj);
        size_t off, adv;
        char enc[4];
        if (i < 0) i += n;
        if (i < 0 || i >= n) py_raise(PY_EXC_INDEX, "string index out of range");
        off = u8_offset(obj->u.s.data, obj->u.s.len, (size_t)i);
        (void)u8_decode(obj->u.s.data + off, obj->u.s.len - off, &adv);
        return py_strn(obj->u.s.data + off, adv);
    }
    case PY_T_RANGE: {
        int64_t i = py_as_int(key);
        int64_t n = (int64_t)py_len(obj);
        if (i < 0) i += n;
        if (i < 0 || i >= n) py_raise(PY_EXC_INDEX, "range object index out of range");
        return py_int(obj->u.range.start + i * obj->u.range.step);
    }
    default:
        (void)key;
        py_raise(PY_EXC_TYPE, "object is not subscriptable");
    }
    return py_none();
}

void py_setitem(PyVal *obj, PyVal *key, PyVal *value) {
    if (!obj) py_raise(PY_EXC_TYPE, "NoneType does not support item assignment");
    if (obj->type == PY_T_DICT) {
        map_put(obj, key, value);
        return;
    }
    if (obj->type == PY_T_LIST) {
        int64_t i = py_as_int(key);
        int64_t n = (int64_t)obj->u.seq.len;
        if (i < 0) i += n;
        if (i < 0 || i >= n) py_raise(PY_EXC_INDEX, "list assignment index out of range");
        py_assign(&obj->u.seq.items[i], value);
        return;
    }
    py_raise(PY_EXC_TYPE, "object does not support item assignment");
}

int py_delitem(PyVal *obj, PyVal *key) {
    size_t i;
    if (obj && obj->type == PY_T_DICT)
        return map_del(obj, key);
    if (obj && obj->type == PY_T_LIST) {
        int64_t at = py_as_int(key);
        int64_t n = (int64_t)obj->u.seq.len;
        if (at < 0) at += n;
        if (at < 0 || at >= n) py_raise(PY_EXC_INDEX, "list assignment index out of range");
        py_release(obj->u.seq.items[at]);
        for (i = (size_t)at; i + 1 < obj->u.seq.len; i++)
            obj->u.seq.items[i] = obj->u.seq.items[i + 1];
        obj->u.seq.len--;
        return 1;
    }
    return 0;
}

static PyVal *dictview_list(PyVal *v);

int py_contains(PyVal *obj, PyVal *key) {
    size_t i;
    if (!obj) return 0;
    if (obj->type == PY_T_DICTVIEW) {
        PyVal *l = dictview_list(obj);
        int r = py_contains(l, key);
        py_release(l);
        return r;
    }
    if (obj->type == PY_T_STR) {
        if (key->type != PY_T_STR) py_raise(PY_EXC_TYPE, "'in <string>' requires string");
        if (key->u.s.len == 0) return 1;
        if (obj->u.s.len < key->u.s.len) return 0;
        for (i = 0; i + key->u.s.len <= obj->u.s.len; i++)
            if (memcmp(obj->u.s.data + i, key->u.s.data, key->u.s.len) == 0)
                return 1;
        return 0;
    }
    if (obj->type == PY_T_DICT || obj->type == PY_T_SET)
        return map_index(obj, key) >= 0;
    if (obj->type == PY_T_RANGE) {
        int64_t x = py_as_int(key);
        int64_t s = obj->u.range.start, e = obj->u.range.stop, st = obj->u.range.step;
        if (st > 0) { if (x < s || x >= e) return 0; }
        else { if (x > s || x <= e) return 0; }
        return ((x - s) % st) == 0;
    }
    if (obj->type == PY_T_LIST || obj->type == PY_T_TUPLE) {
        for (i = 0; i < obj->u.seq.len; i++)
            if (py_cmp(PY_CMP_EQ, obj->u.seq.items[i], key)) return 1;
        return 0;
    }
    {
        PyVal *it = py_iter(obj), *item = NULL;
        int found = 0;
        while (py_iter_next(it, &item)) {
            if (py_cmp(PY_CMP_EQ, item, key)) { found = 1; break; }
        }
        py_release(it);
        py_release(item);
        return found;
    }
}

/* ------------------------------------------------------------------ slices */
static void slice_bounds(PyVal *lo, PyVal *hi, PyVal *stepv, int64_t n,
                         int64_t *s, int64_t *e, int64_t *st) {
    int64_t step = stepv && stepv->type != PY_T_NONE ? py_as_int(stepv) : 1;
    int64_t start, stop;
    if (step == 0) py_raise(PY_EXC_VALUE, "slice step cannot be zero");
    if (step > 0) {
        start = (lo && lo->type != PY_T_NONE) ? py_as_int(lo) : 0;
        stop = (hi && hi->type != PY_T_NONE) ? py_as_int(hi) : n;
        if (start < 0) { start += n; if (start < 0) start = 0; }
        else if (start > n) start = n;
        if (stop < 0) { stop += n; if (stop < 0) stop = 0; }
        else if (stop > n) stop = n;
        if (stop < start) stop = start;
    } else {
        if (lo && lo->type != PY_T_NONE) {
            start = py_as_int(lo);
            if (start < 0) start += n;
            if (start < 0) start = -1;
            else if (start >= n) start = n - 1;
        } else {
            start = n - 1;
        }
        if (hi && hi->type != PY_T_NONE) {
            stop = py_as_int(hi);
            if (stop < 0) stop += n;
            if (stop < 0) stop = -1;
            else if (stop >= n) stop = n - 1;
        } else {
            stop = -1;
        }
        if (stop > start) stop = start;
    }
    *s = start; *e = stop; *st = step;
}

PyVal *py_getslice(PyVal *obj, PyVal *lo, PyVal *hi, PyVal *stepv) {
    int64_t s, e, st, i;
    if (!obj) py_raise(PY_EXC_TYPE, "NoneType is not subscriptable");
    slice_bounds(lo, hi, stepv, (int64_t)py_len(obj), &s, &e, &st);
    if (obj->type == PY_T_STR) {
        Buf b;
        buf_init(&b);
        for (i = s; st > 0 ? i < e : i > e; i += st) {
            size_t off = u8_offset(obj->u.s.data, obj->u.s.len, (size_t)i);
            size_t adv;
            (void)u8_decode(obj->u.s.data + off, obj->u.s.len - off, &adv);
            buf_add(&b, obj->u.s.data + off, adv);
        }
        {
            PyVal *r = py_strn(b.data ? b.data : "", b.len);
            free(b.data);
            return r;
        }
    }
    if (obj->type == PY_T_LIST || obj->type == PY_T_TUPLE) {
        PyVal *r = obj->type == PY_T_LIST ? py_list_new() : val_new(PY_T_TUPLE);
        for (i = s; st > 0 ? i < e : i > e; i += st)
            seq_push(r, obj->u.seq.items[i]);
        return r;
    }
    if (obj->type == PY_T_RANGE) {
        int64_t ns = obj->u.range.start + s * obj->u.range.step;
        int64_t ne = obj->u.range.start + e * obj->u.range.step;
        return py_range(ns, ne, obj->u.range.step * st);
    }
    py_raise(PY_EXC_TYPE, "object is not sliceable");
    return py_none();
}

/* ------------------------------------------------------------------- unpack */
int py_unpack(PyVal *obj, PyVal ***slots, size_t n) {
    size_t i;
    if (obj && (obj->type == PY_T_LIST || obj->type == PY_T_TUPLE)) {
        if (obj->u.seq.len != n)
            py_raise(PY_EXC_VALUE, "too many or too few values to unpack");
        for (i = 0; i < n; i++) py_assign(slots[i], obj->u.seq.items[i]);
        return 1;
    }
    {
        PyVal *it = py_iter(obj), *item = NULL;
        size_t got = 0;
        while (py_iter_next(it, &item)) {
            if (got >= n) {
                py_release(it); py_release(item);
                py_raise(PY_EXC_VALUE, "too many values to unpack");
            }
            py_assign(slots[got], item);
            got++;
        }
        py_release(it);
        py_release(item);
        if (got != n) py_raise(PY_EXC_VALUE, "not enough values to unpack");
    }
    return 1;
}

/* --------------------------------------------------------------- arguments */
PyVal *py_args_tail(PyVal **args, size_t argc, size_t nfixed) {
    if (nfixed >= argc) return py_tuple_from(NULL, 0);
    return py_tuple_from(args + nfixed, argc - nfixed);
}

PyVal *py_kwargs_tail(PyVal *kwargs, const char **names, size_t n) {
    PyVal *d = py_dict_new();
    size_t i, j;
    if (!kwargs || kwargs->type != PY_T_DICT) return d;
    for (i = 0; i < kwargs->u.map.len; i++) {
        PyVal *k = kwargs->u.map.keys[i];
        int used = 0;
        for (j = 0; j < n; j++) {
            size_t kl = strlen(names[j]);
            if (k->type == PY_T_STR && k->u.s.len == kl &&
                memcmp(k->u.s.data, names[j], kl) == 0) { used = 1; break; }
        }
        if (!used) map_put(d, k, kwargs->u.map.vals[i]);
    }
    return d;
}

void py_bind_args(PyVal **args, size_t argc, PyVal *kwargs,
                  const char **names, PyVal **defaults, size_t nparams,
                  PyVal ***slots) {
    py_bind_args_ex(args, argc, kwargs, names, defaults, nparams, slots, 0);
}
void py_bind_args_ex(PyVal **args, size_t argc, PyVal *kwargs,
                     const char **names, PyVal **defaults, size_t nparams,
                     PyVal ***slots, int allow_extra) {
    size_t i;
    if (argc > nparams && !allow_extra)
        py_raise(PY_EXC_TYPE, "too many positional arguments");
    for (i = 0; i < nparams; i++) {
        if (i < argc) {
            py_assign(slots[i], args[i]);
        } else {
            PyVal *kw = py_kw_get(kwargs, names[i]);
            if (kw) {
                py_assign(slots[i], kw);
            } else if (defaults && defaults[i]) {
                py_assign(slots[i], defaults[i]);
            } else {
                char msg[256];
                snprintf(msg, sizeof(msg), "missing required argument: %s", names[i]);
                py_raise(PY_EXC_TYPE, msg);
            }
        }
    }
}
/* --------------------------------------------------------------- operators */
static int is_num(PyVal *v) {
    return v && (v->type == PY_T_INT || v->type == PY_T_BOOL || v->type == PY_T_FLOAT);
}
static int is_intlike(PyVal *v) {
    return v && (v->type == PY_T_INT || v->type == PY_T_BOOL);
}
static double as_double(PyVal *v) {
    if (v->type == PY_T_FLOAT) return v->u.f;
    if (v->type == PY_T_BOOL) return (double)v->u.b;
    if (v->type == PY_T_INT) return (double)v->u.i;
    py_raise(PY_EXC_TYPE, "a number is required");
    return 0;
}
static int64_t as_int64(PyVal *v) {
    if (v->type == PY_T_BOOL) return v->u.b;
    if (v->type == PY_T_INT) return v->u.i;
    py_raise(PY_EXC_TYPE, "an integer is required");
    return 0;
}

/* printf-style % formatting for str % args */
static PyVal *format_percent(PyVal *fmt, PyVal *arg) {
    Buf b;
    const char *p = fmt->u.s.data;
    size_t n = fmt->u.s.len, i = 0;
    size_t ai = 0;
    int tuple = arg && arg->type == PY_T_TUPLE;
    buf_init(&b);
    while (i < n) {
        if (p[i] != '%') { buf_addc(&b, p[i++]); continue; }
        i++;
        if (i < n && p[i] == '%') { buf_addc(&b, '%'); i++; continue; }
        {
            char spec[64];
            size_t sl = 0;
            char conv;
            PyVal *a = tuple ? (ai < arg->u.seq.len ? arg->u.seq.items[ai] : py_none()) : arg;
            spec[sl++] = '%';
            while (i < n && strchr("-+ #0", p[i]) && sl < sizeof(spec) - 2) spec[sl++] = p[i++];
            while (i < n && isdigit((unsigned char)p[i]) && sl < sizeof(spec) - 2) spec[sl++] = p[i++];
            if (i < n && p[i] == '.') {
                spec[sl++] = p[i++];
                while (i < n && isdigit((unsigned char)p[i]) && sl < sizeof(spec) - 2) spec[sl++] = p[i++];
            }
            if (i >= n) break;
            conv = p[i++];
            if (tuple) ai++;
            switch (conv) {
            case 's': case 'r': {
                PyVal *s = conv == 's' ? py_str_of(a) : py_repr(a);
                char fs[80];
                snprintf(fs, sizeof(fs), "%.*ss", (int)sl, spec);
                buf_printf(&b, fs, py_cstr(s));
                py_release(s);
                break;
            }
            case 'd': case 'i': case 'x': case 'X': case 'o': case 'u': {
                char fs[80];
                snprintf(fs, sizeof(fs), "%.*sll%c", (int)sl, spec, conv == 'u' ? 'u' : conv);
                buf_printf(&b, fs, (long long)py_as_int(a));
                break;
            }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
                char fs[80];
                snprintf(fs, sizeof(fs), "%.*s%c", (int)sl, spec, conv);
                buf_printf(&b, fs, py_as_float(a));
                break;
            }
            case 'c':
                buf_printf(&b, "%c", (char)py_as_int(a));
                break;
            default:
                buf_addc(&b, conv);
                break;
            }
        }
    }
    {
        PyVal *r = py_strn(b.data ? b.data : "", b.len);
        free(b.data);
        return r;
    }
}

static PyVal *seq_repeat(PyVal *seq, int64_t times) {
    PyVal *r;
    int64_t k;
    size_t i;
    if (times <= 0) return seq->type == PY_T_LIST ? py_list_new() : val_new(PY_T_TUPLE);
    if ((uint64_t)times > (uint64_t)(SIZE_MAX / (seq->u.seq.len + 1)))
        py_raise(PY_EXC_MEMORY, "repeated sequence is too long");
    r = seq->type == PY_T_LIST ? py_list_new() : val_new(PY_T_TUPLE);
    for (k = 0; k < times; k++)
        for (i = 0; i < seq->u.seq.len; i++) seq_push(r, seq->u.seq.items[i]);
    return r;
}

PyVal *py_binop(int op, PyVal *a, PyVal *b) {
    if (op == PY_OP_ADD) {
        if (a->type == PY_T_STR && b->type == PY_T_STR) {
            PyVal *r = py_strn(NULL, 0);
            free(r->u.s.data);
            r->u.s.len = a->u.s.len + b->u.s.len;
            r->u.s.data = (char *)xmalloc(r->u.s.len + 1);
            memcpy(r->u.s.data, a->u.s.data, a->u.s.len);
            memcpy(r->u.s.data + a->u.s.len, b->u.s.data, b->u.s.len);
            r->u.s.data[r->u.s.len] = 0;
            return r;
        }
        if ((a->type == PY_T_LIST && b->type == PY_T_LIST) ||
            (a->type == PY_T_TUPLE && b->type == PY_T_TUPLE)) {
            PyVal *r = a->type == PY_T_LIST ? py_list_new() : val_new(PY_T_TUPLE);
            size_t i;
            for (i = 0; i < a->u.seq.len; i++) seq_push(r, a->u.seq.items[i]);
            for (i = 0; i < b->u.seq.len; i++) seq_push(r, b->u.seq.items[i]);
            return r;
        }
    }
    if (op == PY_OP_MUL && b->type == PY_T_STR && is_intlike(a))
        return py_binop(PY_OP_MUL, b, a);
    if (op == PY_OP_MUL && a->type == PY_T_STR && is_intlike(b)) {
        PyVal *r;
        int64_t times = as_int64(b), k;
        if (times <= 0) return py_str("");
        if ((uint64_t)times > (uint64_t)(SIZE_MAX / (a->u.s.len + 1)))
            py_raise(PY_EXC_MEMORY, "repeated string is too long");
        r = py_strn(NULL, 0);
        free(r->u.s.data);
        r->u.s.len = a->u.s.len * (size_t)times;
        r->u.s.data = (char *)xmalloc(r->u.s.len + 1);
        for (k = 0; k < times; k++)
            memcpy(r->u.s.data + (size_t)k * a->u.s.len, a->u.s.data, a->u.s.len);
        r->u.s.data[r->u.s.len] = 0;
        return r;
    }
    if (op == PY_OP_MUL && b->type != PY_T_STR && is_intlike(a) &&
        (b->type == PY_T_LIST || b->type == PY_T_TUPLE))
        return seq_repeat(b, as_int64(a));
    if (op == PY_OP_MUL && is_intlike(b) && (a->type == PY_T_LIST || a->type == PY_T_TUPLE))
        return seq_repeat(a, as_int64(b));
    if (op == PY_OP_MOD && a->type == PY_T_STR)
        return format_percent(a, b);

    if (!is_num(a) || !is_num(b)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "unsupported operand types for %s",
                 op == PY_OP_ADD ? "+" : op == PY_OP_SUB ? "-" :
                 op == PY_OP_MUL ? "*" : "operator");
        py_raise(PY_EXC_TYPE, msg);
    }
    switch (op) {
    case PY_OP_ADD:
        if (a->type == PY_T_FLOAT || b->type == PY_T_FLOAT)
            return py_float(as_double(a) + as_double(b));
        return py_int(as_int64(a) + as_int64(b));
    case PY_OP_SUB:
        if (a->type == PY_T_FLOAT || b->type == PY_T_FLOAT)
            return py_float(as_double(a) - as_double(b));
        return py_int(as_int64(a) - as_int64(b));
    case PY_OP_MUL:
        if (a->type == PY_T_FLOAT || b->type == PY_T_FLOAT)
            return py_float(as_double(a) * as_double(b));
        return py_int(as_int64(a) * as_int64(b));
    case PY_OP_DIV: {
        double x = as_double(a), y = as_double(b);
        if (y == 0.0) py_raise(PY_EXC_ZERO_DIVISION, "division by zero");
        return py_float(x / y);
    }
    case PY_OP_FLOORDIV:
        if (a->type == PY_T_FLOAT || b->type == PY_T_FLOAT) {
            double x = as_double(a), y = as_double(b);
            if (y == 0.0) py_raise(PY_EXC_ZERO_DIVISION, "division by zero");
            return py_float(floor(x / y));
        } else {
            int64_t x = as_int64(a), y = as_int64(b), q, r;
            if (y == 0) py_raise(PY_EXC_ZERO_DIVISION, "integer division by zero");
            q = x / y; r = x % y;
            if (r != 0 && ((r < 0) != (y < 0))) q--;
            return py_int(q);
        }
    case PY_OP_MOD:
        if (a->type == PY_T_FLOAT || b->type == PY_T_FLOAT) {
            double x = as_double(a), y = as_double(b), r;
            if (y == 0.0) py_raise(PY_EXC_ZERO_DIVISION, "division by zero");
            r = fmod(x, y);
            if (r != 0.0 && ((r < 0) != (y < 0))) r += y;
            return py_float(r);
        } else {
            int64_t x = as_int64(a), y = as_int64(b), r;
            if (y == 0) py_raise(PY_EXC_ZERO_DIVISION, "integer division by zero");
            r = x % y;
            if (r != 0 && ((r < 0) != (y < 0))) r += y;
            return py_int(r);
        }
    case PY_OP_POW: {
        double x = as_double(a), y = as_double(b);
        if (is_intlike(a) && is_intlike(b) && as_int64(b) >= 0) {
            int64_t base = as_int64(a), e = as_int64(b), r = 1;
            while (e > 0) {
                if (e & 1) r *= base;
                e >>= 1;
                if (e) base *= base;
            }
            return py_int(r);
        }
        return py_float(pow(x, y));
    }
    case PY_OP_LSHIFT: return py_int(as_int64(a) << as_int64(b));
    case PY_OP_RSHIFT: return py_int(as_int64(a) >> as_int64(b));
    case PY_OP_BITAND: return py_int(as_int64(a) & as_int64(b));
    case PY_OP_BITOR:  return py_int(as_int64(a) | as_int64(b));
    case PY_OP_BITXOR: return py_int(as_int64(a) ^ as_int64(b));
    default:
        py_raise(PY_EXC_TYPE, "unsupported binary operator");
    }
    return py_none();
}

PyVal *py_unaryop(int op, PyVal *a) {
    if (op == 3) return py_bool(!py_truth(a));
    if (!is_num(a)) py_raise(PY_EXC_TYPE, "bad operand type for unary operator");
    if (op == 0) return a->type == PY_T_FLOAT ? py_float(-a->u.f) : py_int(-as_int64(a));
    if (op == 1) return a->type == PY_T_FLOAT ? py_float(a->u.f) : py_int(as_int64(a));
    if (op == 2) return py_int(~as_int64(a));
    py_raise(PY_EXC_TYPE, "bad unary operator");
    return py_none();
}

int py_cmp(int op, PyVal *a, PyVal *b) {
    if (op == PY_CMP_IS) return a == b;
    if (op == PY_CMP_ISNOT) return a != b;
    if (op == PY_CMP_IN) return py_contains(b, a);
    if (op == PY_CMP_NOTIN) return !py_contains(b, a);
    if (op == PY_CMP_EQ || op == PY_CMP_NE) {
        int eq = 0;
        if (a == b) eq = 1;
        else if (!a || !b) eq = 0;
        else if (a->type == PY_T_NONE || b->type == PY_T_NONE) eq = (a->type == b->type);
        else if (is_num(a) && is_num(b)) eq = as_double(a) == as_double(b);
        else if (a->type == PY_T_STR && b->type == PY_T_STR) eq = str_eq(a, b);
        else if ((a->type == PY_T_LIST || a->type == PY_T_TUPLE) &&
                 (b->type == PY_T_LIST || b->type == PY_T_TUPLE)) {
            size_t i;
            eq = a->u.seq.len == b->u.seq.len;
            for (i = 0; eq && i < a->u.seq.len; i++)
                eq = py_cmp(PY_CMP_EQ, a->u.seq.items[i], b->u.seq.items[i]);
        } else if (a->type == PY_T_DICT && b->type == PY_T_DICT) {
            size_t i;
            eq = a->u.map.len == b->u.map.len;
            for (i = 0; eq && i < a->u.map.len; i++) {
                PyVal *bv = map_get(b, a->u.map.keys[i]);
                eq = bv && py_cmp(PY_CMP_EQ, a->u.map.vals[i], bv);
            }
        } else if (a->type == PY_T_SET && b->type == PY_T_SET) {
            size_t i;
            eq = a->u.map.len == b->u.map.len;
            for (i = 0; eq && i < a->u.map.len; i++)
                eq = map_index(b, a->u.map.keys[i]) >= 0;
        } else eq = 0;
        return op == PY_CMP_EQ ? eq : !eq;
    }
    if (is_num(a) && is_num(b)) {
        double x = as_double(a), y = as_double(b);
        switch (op) {
        case PY_CMP_LT: return x < y;
        case PY_CMP_LE: return x <= y;
        case PY_CMP_GT: return x > y;
        case PY_CMP_GE: return x >= y;
        }
    }
    if (a->type == PY_T_STR && b->type == PY_T_STR) {
        int c = strcmp(a->u.s.data, b->u.s.data);
        switch (op) {
        case PY_CMP_LT: return c < 0;
        case PY_CMP_LE: return c <= 0;
        case PY_CMP_GT: return c > 0;
        case PY_CMP_GE: return c >= 0;
        }
    }
    if ((a->type == PY_T_LIST || a->type == PY_T_TUPLE) &&
        (b->type == PY_T_LIST || b->type == PY_T_TUPLE)) {
        size_t i, n = a->u.seq.len < b->u.seq.len ? a->u.seq.len : b->u.seq.len;
        for (i = 0; i < n; i++) {
            if (py_cmp(PY_CMP_EQ, a->u.seq.items[i], b->u.seq.items[i])) continue;
            return py_cmp(op, a->u.seq.items[i], b->u.seq.items[i]);
        }
        switch (op) {
        case PY_CMP_LT: return a->u.seq.len < b->u.seq.len;
        case PY_CMP_LE: return a->u.seq.len <= b->u.seq.len;
        case PY_CMP_GT: return a->u.seq.len > b->u.seq.len;
        case PY_CMP_GE: return a->u.seq.len >= b->u.seq.len;
        }
    }
    py_raise(PY_EXC_TYPE, "objects are not orderable");
    return 0;
}

/* ------------------------------------------------------------- repr / str */
static void repr_string(Buf *b, PyVal *s) {
    size_t i;
    int use_single = 1;
    for (i = 0; i < s->u.s.len; i++)
        if (s->u.s.data[i] == '\'') { use_single = 0; break; }
    buf_addc(b, use_single ? '\'' : '"');
    for (i = 0; i < s->u.s.len; i++) {
        unsigned char c = (unsigned char)s->u.s.data[i];
        switch (c) {
        case '\\': buf_adds(b, "\\\\"); break;
        case '\n': buf_adds(b, "\\n"); break;
        case '\r': buf_adds(b, "\\r"); break;
        case '\t': buf_adds(b, "\\t"); break;
        default:
            if (c == (use_single ? '\'' : '"')) { buf_addc(b, '\\'); buf_addc(b, (char)c); }
            else if (c < 32 || c == 127) buf_printf(b, "\\x%02x", c);
            else buf_addc(b, (char)c);
        }
    }
    buf_addc(b, use_single ? '\'' : '"');
}

static uint64_t py_hash_of(PyVal *v) {
    uint64_t h;
    size_t i;
    if (!v) return 0;
    switch (v->type) {
    case PY_T_BOOL: return (uint64_t)v->u.b;
    case PY_T_INT: return (uint64_t)v->u.i;
    case PY_T_FLOAT: return (uint64_t)(int64_t)v->u.f;
    case PY_T_STR:
        h = 1469598103934665603ULL;
        for (i = 0; i < v->u.s.len; i++) {
            h ^= (unsigned char)v->u.s.data[i];
            h *= 1099511628211ULL;
        }
        return h;
    case PY_T_TUPLE:
        h = 1469598103934665603ULL;
        for (i = 0; i < v->u.seq.len; i++) {
            h ^= py_hash_of(v->u.seq.items[i]);
            h *= 1099511628211ULL;
        }
        return h;
    default: return (uint64_t)(uintptr_t)v;
    }
}

static void repr_into(Buf *b, PyVal *v, int depth);

static PyVal *dictview_list(PyVal *v) {
    PyVal *d = v->u.view.dict, *r = py_list_new();
    size_t i;
    for (i = 0; i < d->u.map.len; i++) {
        if (v->u.view.kind == 0) seq_push(r, d->u.map.keys[i]);
        else if (v->u.view.kind == 1) seq_push(r, d->u.map.vals[i]);
        else {
            PyVal *pair[2] = {d->u.map.keys[i], d->u.map.vals[i]};
            seq_push(r, py_tuple_from(pair, 2));
        }
    }
    return r;
}

static PyVal *number_str(PyVal *v) {
    char tmp[64];
    if (v->type == PY_T_INT) snprintf(tmp, sizeof(tmp), "%" PRId64, v->u.i);
    else if (v->type == PY_T_BOOL) snprintf(tmp, sizeof(tmp), "%s", v->u.b ? "True" : "False");
    else {
        double f = v->u.f;
        int prec;
        if (isnan(f)) return py_str("nan");
        if (isinf(f)) return py_str(f < 0 ? "-inf" : "inf");
        /* Python's repr uses the shortest representation that round-trips. */
        for (prec = 15; prec < 17; prec++) {
            snprintf(tmp, sizeof(tmp), "%.*g", prec, f);
            if (strtod(tmp, NULL) == f) break;
        }
        if (prec == 17) snprintf(tmp, sizeof(tmp), "%.17g", f);
        if (!strchr(tmp, '.') && !strchr(tmp, 'e') && !strchr(tmp, 'E'))
            strncat(tmp, ".0", sizeof(tmp) - strlen(tmp) - 1);
    }
    return py_str(tmp);
}

static void repr_into(Buf *b, PyVal *v, int depth) {
    size_t i;
    if (!v) { buf_adds(b, "None"); return; }
    if (depth > 16) { buf_adds(b, "..."); return; }
    switch (v->type) {
    case PY_T_NONE: buf_adds(b, "None"); break;
    case PY_T_BOOL: buf_adds(b, v->u.b ? "True" : "False"); break;
    case PY_T_INT: buf_printf(b, "%" PRId64, v->u.i); break;
    case PY_T_FLOAT: {
        PyVal *s = number_str(v);
        buf_adds(b, s->u.s.data);
        py_release(s);
        break;
    }
    case PY_T_STR: repr_string(b, v); break;
    case PY_T_LIST: case PY_T_TUPLE: {
        int tup = v->type == PY_T_TUPLE;
        buf_addc(b, tup ? '(' : '[');
        for (i = 0; i < v->u.seq.len; i++) {
            if (i) buf_adds(b, ", ");
            repr_into(b, v->u.seq.items[i], depth + 1);
        }
        if (tup && v->u.seq.len == 1) buf_addc(b, ',');
        buf_addc(b, tup ? ')' : ']');
        break;
    }
    case PY_T_DICT: {
        buf_addc(b, '{');
        for (i = 0; i < v->u.map.len; i++) {
            if (i) buf_adds(b, ", ");
            repr_into(b, v->u.map.keys[i], depth + 1);
            buf_adds(b, ": ");
            repr_into(b, v->u.map.vals[i], depth + 1);
        }
        buf_addc(b, '}');
        break;
    }
    case PY_T_SET: {
        size_t *order, a, b2;
        uint64_t mask;
        if (v->u.map.len == 0) { buf_adds(b, "set()"); break; }
        order = (size_t *)xmalloc(v->u.map.len * sizeof(size_t));
        for (i = 0; i < v->u.map.len; i++) order[i] = i;
        mask = v->u.map.len <= 4 ? 7u : 31u;
        for (a = 1; a < v->u.map.len; a++) {
            size_t cur = order[a], j = a;
            uint64_t hc = py_hash_of(v->u.map.keys[cur]) & mask;
            while (j > 0) {
                size_t prev = order[j - 1];
                if ((py_hash_of(v->u.map.keys[prev]) & mask) <= hc) break;
                order[j] = prev;
                j--;
            }
            order[j] = cur;
        }
        buf_addc(b, '{');
        for (a = 0; a < v->u.map.len; a++) {
            if (a) buf_adds(b, ", ");
            b2 = order[a];
            repr_into(b, v->u.map.keys[b2], depth + 1);
        }
        free(order);
        buf_addc(b, '}');
        break;
    }
    case PY_T_RANGE:
        if (v->u.range.step == 1)
            buf_printf(b, "range(%" PRId64 ", %" PRId64 ")", v->u.range.start, v->u.range.stop);
        else
            buf_printf(b, "range(%" PRId64 ", %" PRId64 ", %" PRId64 ")",
                       v->u.range.start, v->u.range.stop, v->u.range.step);
        break;
    case PY_T_FUNC: buf_printf(b, "<function %s>", v->u.func.name); break;
    case PY_T_BOUND: buf_adds(b, "<bound method>"); break;
    case PY_T_CLASS: buf_printf(b, "<class '%s'>", v->u.klass.name->u.s.data); break;
    case PY_T_WIDGET: buf_adds(b, "<tkinter widget>"); break;
    case PY_T_FILE: buf_adds(b, "<file>"); break;
    case PY_T_VAR: buf_adds(b, "<tkinter variable>"); break;
    case PY_T_EXC: buf_printf(b, "%s(%s)", py_exc_name(v->u.exc.kind), v->u.exc.msg); break;
    case PY_T_ITER: buf_adds(b, "<iterator>"); break;
    case PY_T_SLICE:
        buf_adds(b, "slice(");
        repr_into(b, v->u.slice.start ? v->u.slice.start : py_none(), depth + 1);
        buf_adds(b, ", ");
        repr_into(b, v->u.slice.stop ? v->u.slice.stop : py_none(), depth + 1);
        if (v->u.slice.step) {
            buf_adds(b, ", ");
            repr_into(b, v->u.slice.step, depth + 1);
        }
        buf_addc(b, ')');
        break;
    case PY_T_DICTVIEW: {
        PyVal *l = dictview_list(v);
        buf_adds(b, v->u.view.kind == 0 ? "dict_keys(" :
                    v->u.view.kind == 1 ? "dict_values(" : "dict_items(");
        repr_into(b, l, depth + 1);
        buf_addc(b, ')');
        py_release(l);
        break;
    }
    default: buf_adds(b, "<object>"); break;
    }
}

PyVal *py_repr(PyVal *v) {
    Buf b;
    PyVal *r;
    buf_init(&b);
    repr_into(&b, v, 0);
    r = py_strn(b.data ? b.data : "", b.len);
    free(b.data);
    return r;
}

PyVal *py_str_of(PyVal *v) {
    if (!v) return py_str("None");
    switch (v->type) {
    case PY_T_STR: py_retain(v); return v;
    case PY_T_NONE: return py_str("None");
    case PY_T_BOOL: return py_str(v->u.b ? "True" : "False");
    case PY_T_INT: case PY_T_FLOAT: return number_str(v);
    case PY_T_CLASS: {
        char tmp[256];
        snprintf(tmp, sizeof(tmp), "<class '%s'>", v->u.klass.name->u.s.data);
        return py_str(tmp);
    }
    case PY_T_EXC:
        if (v->u.exc.kind == PY_EXC_KEY) {
            PyVal *s = py_str(v->u.exc.msg);
            PyVal *r = py_repr(s);
            py_release(s);
            return r;
        }
        return py_str(v->u.exc.msg);
    default: return py_repr(v);
    }
}

const char *py_cstr(PyVal *v) {
    if (v && v->type == PY_T_STR) return v->u.s.data;
    py_raise(PY_EXC_TYPE, "expected a string");
    return "";
}
size_t py_cstr_len(PyVal *v) {
    if (v && v->type == PY_T_STR) return v->u.s.len;
    return 0;
}

int64_t py_as_int(PyVal *v) {
    if (!v) return 0;
    if (v->type == PY_T_INT) return v->u.i;
    if (v->type == PY_T_BOOL) return v->u.b;
    if (v->type == PY_T_FLOAT) return (int64_t)v->u.f;
    if (v->type == PY_T_STR) {
        char *end = NULL;
        long long x = strtoll(v->u.s.data, &end, 10);
        if (end == v->u.s.data) py_raise(PY_EXC_VALUE, "invalid literal for int()");
        return (int64_t)x;
    }
    py_raise(PY_EXC_TYPE, "cannot convert to int");
    return 0;
}
double py_as_float(PyVal *v) {
    if (!v) return 0.0;
    if (v->type == PY_T_FLOAT) return v->u.f;
    if (v->type == PY_T_INT) return (double)v->u.i;
    if (v->type == PY_T_BOOL) return (double)v->u.b;
    if (v->type == PY_T_STR) {
        char *end = NULL;
        double x = strtod(v->u.s.data, &end);
        if (end == v->u.s.data) py_raise(PY_EXC_VALUE, "could not convert string to float");
        return x;
    }
    py_raise(PY_EXC_TYPE, "cannot convert to float");
    return 0.0;
}

static const char *type_name(PyVal *v) {
    if (!v) return "NoneType";
    switch (v->type) {
    case PY_T_NONE: return "NoneType";
    case PY_T_BOOL: return "bool";
    case PY_T_INT: return "int";
    case PY_T_FLOAT: return "float";
    case PY_T_STR: return "str";
    case PY_T_LIST: return "list";
    case PY_T_TUPLE: return "tuple";
    case PY_T_DICT: return "dict";
    case PY_T_SET: return "set";
    case PY_T_FUNC: case PY_T_BOUND: return "function";
    case PY_T_RANGE: return "range";
    case PY_T_WIDGET: return "tkinter.Widget";
    case PY_T_FILE: return "file";
    case PY_T_EXC: return "Exception";
    case PY_T_VAR: return "tkinter.Variable";
    case PY_T_CLASS: return v->u.klass.name ? v->u.klass.name->u.s.data : "type";
    case PY_T_DICTVIEW: return v->u.view.kind == 0 ? "dict_keys" :
                                v->u.view.kind == 1 ? "dict_values" : "dict_items";
    case PY_T_SLICE: return "slice";
    default: return "object";
    }
}

PyVal *py_type(PyVal *v) {
    PyVal *cls = val_new(PY_T_CLASS);
    cls->u.klass.name = py_str(type_name(v));
    cls->u.klass.dict = py_dict_new();
    return cls;
}

/* --------------------------------------------------------------- attributes */
static PyVal *map_get_cstr(PyVal *d, const char *name) {
    size_t i, n = strlen(name);
    if (!d || (d->type != PY_T_DICT && d->type != PY_T_SET)) return NULL;
    for (i = 0; i < d->u.map.len; i++) {
        PyVal *k = d->u.map.keys[i];
        if (k->type == PY_T_STR && k->u.s.len == n && memcmp(k->u.s.data, name, n) == 0)
            return d->u.map.vals[i];
    }
    return NULL;
}

static void map_put_cstr(PyVal *d, const char *name, PyVal *value) {
    PyVal *k = py_str(name);
    map_put(d, k, value);
    py_release(k);
}

static void map_del_cstr(PyVal *d, const char *name) {
    PyVal *k = py_str(name);
    map_del(d, k);
    py_release(k);
}

PyVal *py_getattr(PyVal *obj, const char *name) {
    if (!obj) py_raise(PY_EXC_ATTRIBUTE, "'NoneType' object has no attribute");
    if (obj->type == PY_T_WIDGET)
        return py_gui_getattr(obj->u.widget.w, name);
    if (obj->type == PY_T_DICT) {
        PyVal *r = map_get_cstr(obj, name);
        PyVal *cls;
        if (r) { py_retain(r); return r; }
        cls = map_get_cstr(obj, "__class__");
        if (cls && cls->type == PY_T_CLASS) {
            r = map_get_cstr(cls->u.klass.dict, name);
            if (r) {
                if (r->type == PY_T_FUNC) return py_bound(r, obj);
                py_retain(r);
                return r;
            }
        }
    }
    if (obj->type == PY_T_CLASS) {
        PyVal *r = map_get_cstr(obj->u.klass.dict, name);
        if (r) { py_retain(r); return r; }
    }
    if (obj->type == PY_T_RANGE) {
        if (strcmp(name, "start") == 0) return py_int(obj->u.range.start);
        if (strcmp(name, "stop") == 0) return py_int(obj->u.range.stop);
        if (strcmp(name, "step") == 0) return py_int(obj->u.range.step);
    }
    if (obj->type == PY_T_VAR) {
        if (strcmp(name, "value") == 0) { py_retain(obj->u.var.value); return obj->u.var.value; }
    }
    {
        char msg[256];
        snprintf(msg, sizeof(msg), "object has no attribute '%s'", name);
        py_raise(PY_EXC_ATTRIBUTE, msg);
    }
    return py_none();
}

void py_setattr(PyVal *obj, const char *name, PyVal *value) {
    if (!obj) py_raise(PY_EXC_ATTRIBUTE, "cannot set attribute on None");
    if (obj->type == PY_T_WIDGET) { py_gui_setattr(obj->u.widget.w, name, value); return; }
    if (obj->type == PY_T_DICT) { map_put_cstr(obj, name, value); return; }
    if (obj->type == PY_T_CLASS) { map_put_cstr(obj->u.klass.dict, name, value); return; }
    if (obj->type == PY_T_VAR) {
        if (strcmp(name, "value") == 0) { py_assign(&obj->u.var.value, value); return; }
    }
    py_raise(PY_EXC_ATTRIBUTE, "object does not support attribute assignment");
}

int py_delattr(PyVal *obj, const char *name) {
    if (obj && obj->type == PY_T_DICT) { map_del_cstr(obj, name); return 1; }
    if (obj && obj->type == PY_T_CLASS) { map_del_cstr(obj->u.klass.dict, name); return 1; }
    return 0;
}

size_t py_hash(PyVal *v) {
    PyVal *r = py_repr(v);
    size_t h = 2166136261u, i;
    for (i = 0; i < r->u.s.len; i++) {
        h ^= (unsigned char)r->u.s.data[i];
        h *= 16777619u;
    }
    py_release(r);
    return h;
}
/* --------------------------------------------------------------- iteration */
static PyVal *iter_next_seq(PyIter *it) {
    PyVal *src = it->src;
    if (it->idx >= src->u.seq.len) return NULL;
    py_retain(src->u.seq.items[it->idx]);
    return src->u.seq.items[it->idx++];
}
static PyVal *iter_next_str(PyIter *it) {
    PyVal *src = it->src;
    size_t adv;
    char enc[4];
    if (it->idx >= src->u.s.len) return NULL;
    (void)u8_decode(src->u.s.data + it->idx, src->u.s.len - it->idx, &adv);
    memcpy(enc, src->u.s.data + it->idx, adv);
    it->idx += adv;
    return py_strn(enc, adv);
}
static PyVal *iter_next_range(PyIter *it) {
    int64_t v;
    int64_t n = (int64_t)py_len(it->src);
    if ((int64_t)it->idx >= n) return NULL;
    v = it->src->u.range.start + (int64_t)it->idx * it->src->u.range.step;
    it->idx++;
    return py_int(v);
}
static PyVal *iter_next_dict(PyIter *it) {
    PyVal *src = it->src;
    if (it->idx >= src->u.map.len) return NULL;
    py_retain(src->u.map.keys[it->idx]);
    return src->u.map.keys[it->idx++];
}
static PyVal *iter_next_enumerate(PyIter *it) {
    PyVal *item = NULL, *t, *arr[2];
    if (!py_iter_next(it->src, &item)) return NULL;
    t = py_int((int64_t)it->idx++);
    arr[0] = t;
    arr[1] = item;
    {
        PyVal *r = py_tuple_from(arr, 2);
        py_release(t);
        py_release(item);
        return r;
    }
}
static PyVal *iter_next_zip(PyIter *it) {
    PyVal *result;
    size_t i, n;
    PyVal **vals;
    if (!it->extra || it->extra->type != PY_T_LIST) return NULL;
    n = it->extra->u.seq.len;
    if (n == 0) return NULL;
    vals = (PyVal **)xmalloc(n * sizeof(PyVal *));
    for (i = 0; i < n; i++) {
        vals[i] = NULL;
        if (!py_iter_next(it->extra->u.seq.items[i], &vals[i])) {
            size_t j;
            for (j = 0; j < i; j++) py_release(vals[j]);
            free(vals);
            return NULL;
        }
    }
    result = py_tuple_from(vals, n);
    for (i = 0; i < n; i++) py_release(vals[i]);
    free(vals);
    return result;
}
static PyVal *iter_next_map(PyIter *it) {
    PyVal *item = NULL, *fn, *r;
    if (!it->extra || it->extra->type != PY_T_LIST || it->extra->u.seq.len < 2) return NULL;
    fn = it->extra->u.seq.items[0];
    if (!py_iter_next(it->extra->u.seq.items[1], &item)) return NULL;
    r = py_call(fn, &item, 1, NULL);
    py_release(item);
    return r;
}
static PyVal *iter_next_filter(PyIter *it) {
    PyVal *item = NULL, *fn;
    if (!it->extra || it->extra->type != PY_T_LIST || it->extra->u.seq.len < 2) return NULL;
    fn = it->extra->u.seq.items[0];
    while (py_iter_next(it->extra->u.seq.items[1], &item)) {
        int keep = fn->type == PY_T_NONE ? py_truth(item) :
                   py_truth(py_call(fn, &item, 1, NULL));
        if (keep) return item;
        py_release(item);
        item = NULL;
    }
    return NULL;
}
static PyVal *iter_next_file(PyIter *it) {
    PyVal *line;
    if (!it->src || it->src->type != PY_T_FILE || !it->src->u.file.fp) return NULL;
    line = file_readline(it->src);
    if (line->u.s.len == 0) { py_release(line); return NULL; }
    return line;
}

PyVal *py_iter(PyVal *obj) {
    PyVal *v;
    PyIter *it;
    if (!obj) py_raise(PY_EXC_TYPE, "'NoneType' object is not iterable");
    if (obj->type == PY_T_DICTVIEW) {
        PyVal *l = dictview_list(obj);
        PyVal *v2 = py_iter(l);
        py_release(l);
        return v2;
    }
    if (obj->type == PY_T_ITER) { py_retain(obj); return obj; }
    if (obj->type == PY_T_DICT) {
        PyVal *cls = map_get_cstr(obj, "__class__");
        if (cls) {
            PyVal *m = py_getattr(obj, "__iter__");
            PyVal *r = py_call(m, NULL, 0, NULL);
            py_release(m);
            v = py_iter(r);
            py_release(r);
            return v;
        }
    }
    v = val_new(PY_T_ITER);
    it = (PyIter *)xmalloc(sizeof(PyIter));
    memset(it, 0, sizeof(*it));
    it->src = obj;
    py_retain(obj);
    switch (obj->type) {
    case PY_T_LIST: case PY_T_TUPLE: it->next = iter_next_seq; break;
    case PY_T_STR: it->next = iter_next_str; break;
    case PY_T_RANGE: it->next = iter_next_range; break;
    case PY_T_DICT: case PY_T_SET: it->next = iter_next_dict; break;
    case PY_T_FILE: it->next = iter_next_file; break;
    default: {
        PyVal *m;
        py_release(v);
        py_release(obj);
        m = py_getattr(obj, "__iter__");
        v = py_call(m, NULL, 0, NULL);
        py_release(m);
        {
            PyVal *r = py_iter(v);
            py_release(v);
            return r;
        }
    }
    }
    v->u.iter.it = it;
    return v;
}

PyVal *py_iter_from_array(PyVal **items, size_t n) {
    PyVal *list = py_list_from(items, n);
    PyVal *it = py_iter(list);
    py_release(list);
    return it;
}

PyVal *py_iter_next(PyVal *itv, PyVal **out) {
    PyVal *r;
    if (!itv || itv->type != PY_T_ITER || !itv->u.iter.it) return NULL;
    r = itv->u.iter.it->next(itv->u.iter.it);
    if (!r) return NULL;
    py_assign(out, r);
    py_release(r);
    return *out;
}

/* --------------------------------------------------------------- callables */
PyVal *py_call(PyVal *fn, PyVal **args, size_t argc, PyVal *kwargs) {
    if (!fn) py_raise(PY_EXC_TYPE, "'NoneType' object is not callable");
    if (fn->type == PY_T_FUNC) return fn->u.func.fn(args, argc, kwargs);
    if (fn->type == PY_T_BOUND) {
        PyVal **a = (PyVal **)xmalloc((argc + 1) * sizeof(PyVal *));
        PyVal *r;
        size_t i;
        a[0] = fn->u.bound.self;
        for (i = 0; i < argc; i++) a[i + 1] = args[i];
        r = py_call(fn->u.bound.fn, a, argc + 1, kwargs);
        free(a);
        return r;
    }
    if (fn->type == PY_T_MODFUNC)
        return py_module_call(fn->u.modfunc.module, fn->u.modfunc.name, args, argc, kwargs);
    if (fn->type == PY_T_GUIMETHOD)
        return py_gui_method(fn->u.gm.w, fn->u.gm.name, args, argc, kwargs);
    if (fn->type == PY_T_CLASS) {        PyVal *inst = py_dict_new();
        PyVal *init;
        PyVal *k = py_str("__class__");
        map_put(inst, k, fn);
        py_release(k);
        init = map_get_cstr(fn->u.klass.dict, "__init__");
        if (init) {
            PyVal *b = py_bound(init, inst);
            PyVal *r = py_call(b, args, argc, kwargs);
            py_release(r);
            py_release(b);
        }
        return inst;
    }
    if (fn->type == PY_T_DICT) {
        PyVal *c = map_get_cstr(fn, "__call__");
        if (c) {
            PyVal *b = py_bound(c, fn);
            PyVal *r = py_call(b, args, argc, kwargs);
            py_release(b);
            return r;
        }
    }
    py_raise(PY_EXC_TYPE, "object is not callable");
    return py_none();
}

PyVal *py_call_attr(PyVal *obj, const char *name, PyVal **args, size_t argc,
                    PyVal *kwargs) {
    return py_method(obj, name, args, argc, kwargs);
}

/* ------------------------------------------------------------ methods: str */
static PyVal *str_slice(PyVal *s, size_t a, size_t b) { return py_strn(s->u.s.data + a, b - a); }

static int str_is_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

static PyVal *str_split(PyVal *s, PyVal *sepv, int64_t maxsplit) {
    PyVal *r = py_list_new();
    int64_t done = 0;
    if (!sepv || sepv->type == PY_T_NONE) {
        size_t i = 0, n = s->u.s.len;
        while (i < n) {
            size_t a;
            while (i < n && str_is_space((unsigned char)s->u.s.data[i])) i++;
            if (i >= n) break;
            if (maxsplit >= 0 && done >= maxsplit) {
                size_t b = n;
                while (b > i && str_is_space((unsigned char)s->u.s.data[b - 1])) b--;
                seq_push(r, py_strn(s->u.s.data + i, b - i));
                break;
            }
            a = i;
            while (i < n && !str_is_space((unsigned char)s->u.s.data[i])) i++;
            seq_push(r, py_strn(s->u.s.data + a, i - a));
            done++;
        }
    } else {
        const char *sep = sepv->u.s.data;
        size_t sl = sepv->u.s.len, i = 0;
        if (sl == 0) py_raise(PY_EXC_VALUE, "empty separator");
        while (i <= s->u.s.len) {
            size_t a = i;
            if (maxsplit >= 0 && done >= maxsplit) {
                seq_push(r, py_strn(s->u.s.data + i, s->u.s.len - i));
                break;
            }
            while (i + sl <= s->u.s.len && memcmp(s->u.s.data + i, sep, sl) != 0) i++;
            if (i + sl > s->u.s.len) {
                seq_push(r, py_strn(s->u.s.data + a, s->u.s.len - a));
                break;
            }
            seq_push(r, py_strn(s->u.s.data + a, i - a));
            i += sl;
            done++;
        }
    }
    return r;
}

static PyVal *str_format_impl(PyVal *s, PyVal **args, size_t argc, PyVal *kwargs) {
    Buf b;
    size_t i = 0, auto_idx = 0;
    buf_init(&b);
    while (i < s->u.s.len) {
        char c = s->u.s.data[i];
        if (c == '{') {
            size_t j, k, field_start, name_len;
            PyVal *val = NULL;
            PyVal *specv = NULL;
            PyVal *out;
            char conv = 0;
            int all_digits = 1;
            char name[256];
            if (i + 1 < s->u.s.len && s->u.s.data[i + 1] == '{') {
                buf_addc(&b, '{');
                i += 2;
                continue;
            }
            j = i + 1;
            while (j < s->u.s.len && s->u.s.data[j] != '}') j++;
            if (j >= s->u.s.len) {
                buf_addc(&b, '{');
                i++;
                continue;
            }
            field_start = i + 1;
            k = field_start;
            while (k < j && s->u.s.data[k] != ':' && s->u.s.data[k] != '!') k++;
            name_len = k - field_start;
            if (name_len == 0) {
                if (auto_idx >= argc)
                    py_raise(PY_EXC_INDEX, "not enough arguments for format string");
                val = args[auto_idx++];
            } else {
                size_t q;
                long idx;
                if (name_len >= sizeof(name)) name_len = sizeof(name) - 1;
                memcpy(name, s->u.s.data + field_start, name_len);
                name[name_len] = 0;
                for (q = 0; q < name_len; q++)
                    if (!isdigit((unsigned char)name[q])) all_digits = 0;
                if (all_digits) {
                    idx = strtol(name, NULL, 10);
                    if (idx < 0 || (size_t)idx >= argc)
                        py_raise(PY_EXC_INDEX, "format index out of range");
                    val = args[idx];
                } else {
                    val = kwargs ? py_dict_get(kwargs, name) : NULL;
                    if (!val) py_raise(PY_EXC_KEY, "format key not found");
                }
            }
            if (k < j && s->u.s.data[k] == '!') {
                if (k + 1 < j) conv = s->u.s.data[k + 1];
                k += 2;
                if (k < j && s->u.s.data[k] == ':') {
                    specv = py_strn(s->u.s.data + k + 1, j - k - 1);
                }
            } else if (k < j && s->u.s.data[k] == ':') {
                specv = py_strn(s->u.s.data + k + 1, j - k - 1);
            }
            if (conv == 'r') out = py_repr(val);
            else if (specv) {
                PyVal *fa[2];
                fa[0] = val;
                fa[1] = specv;
                out = py_builtin_format(fa, 2, NULL);
            } else out = py_str_of(val);
            buf_add(&b, out->u.s.data, out->u.s.len);
            py_release(out);
            py_release(specv);
            i = j + 1;
        } else if (c == '}') {
            if (i + 1 < s->u.s.len && s->u.s.data[i + 1] == '}') {
                buf_addc(&b, '}');
                i += 2;
                continue;
            }
            py_raise(PY_EXC_VALUE, "single '}' encountered in format string");
        } else {
            buf_addc(&b, c);
            i++;
        }
    }
    {
        PyVal *r = py_strn(b.data ? b.data : "", b.len);
        free(b.data);
        return r;
    }
}

static PyVal *str_method(PyVal *s, const char *name, PyVal **args, size_t argc,
                         PyVal *kwargs) {
    (void)kwargs;
    if (!strcmp(name, "upper") || !strcmp(name, "lower") || !strcmp(name, "casefold") ||
        !strcmp(name, "swapcase") || !strcmp(name, "capitalize") || !strcmp(name, "title")) {
        PyVal *r = py_strn(s->u.s.data, s->u.s.len);
        size_t i;
        int up = !strcmp(name, "upper");
        int title = !strcmp(name, "title");
        int start = 1;
        for (i = 0; i < r->u.s.len; i++) {
            unsigned char c = (unsigned char)r->u.s.data[i];
            if (c < 128) {
                if (title) { r->u.s.data[i] = (char)(start ? toupper(c) : tolower(c)); start = !isalnum(c); }
                else if (up) r->u.s.data[i] = (char)toupper(c);
                else if (!strcmp(name, "swapcase")) r->u.s.data[i] = (char)(isupper(c) ? tolower(c) : toupper(c));
                else if (!strcmp(name, "capitalize")) r->u.s.data[i] = (char)(i == 0 ? toupper(c) : tolower(c));
                else r->u.s.data[i] = (char)tolower(c);
            }
        }
        return r;
    }
    if (!strcmp(name, "strip") || !strcmp(name, "lstrip") || !strcmp(name, "rstrip")) {
        size_t a = 0, b = s->u.s.len;
        PyVal *chars = argc > 0 ? args[0] : NULL;
        int left = strcmp(name, "rstrip") != 0;
        int right = strcmp(name, "lstrip") != 0;
        if (chars && chars->type == PY_T_STR) {
            if (left) while (a < b && memchr(chars->u.s.data, s->u.s.data[a], chars->u.s.len)) a++;
            if (right) while (b > a && memchr(chars->u.s.data, s->u.s.data[b - 1], chars->u.s.len)) b--;
        } else {
            if (left) while (a < b && str_is_space((unsigned char)s->u.s.data[a])) a++;
            if (right) while (b > a && str_is_space((unsigned char)s->u.s.data[b - 1])) b--;
        }
        return str_slice(s, a, b);
    }
    if (!strcmp(name, "split") || !strcmp(name, "rsplit")) {
        PyVal *sep = argc > 0 ? args[0] : NULL;
        int64_t maxsplit = argc > 1 ? py_as_int(args[1]) : -1;
        if (maxsplit < 0) maxsplit = -1;
        return str_split(s, sep, maxsplit);
    }
    if (!strcmp(name, "splitlines")) {
        PyVal *r = py_list_new();
        size_t i, a = 0;
        int keep = argc > 0 && py_truth(args[0]);
        for (i = 0; i < s->u.s.len; i++) {
            if (s->u.s.data[i] == '\n' || s->u.s.data[i] == '\r') {
                size_t b = i;
                if (s->u.s.data[i] == '\r' && i + 1 < s->u.s.len && s->u.s.data[i + 1] == '\n') i++;
                seq_push(r, py_strn(s->u.s.data + a, b - a + (keep ? 1 : 0)));
                a = i + 1;
            }
        }
        if (a < s->u.s.len) seq_push(r, py_strn(s->u.s.data + a, s->u.s.len - a));
        return r;
    }
    if (!strcmp(name, "join")) {
        PyVal *r = py_strn(NULL, 0);
        Buf b;
        size_t i;
        buf_init(&b);
        if (argc > 0) {
            PyVal *it = py_iter(args[0]), *item = NULL;
            int first = 1;
            while (py_iter_next(it, &item)) {
                PyVal *str;
                if (item->type != PY_T_STR) py_raise(PY_EXC_TYPE, "sequence item is not a string");
                if (!first) buf_add(&b, s->u.s.data, s->u.s.len);
                first = 0;
                str = item;
                buf_add(&b, str->u.s.data, str->u.s.len);
            }
            py_release(it);
            py_release(item);
        }
        free(r->u.s.data);
        r->u.s.len = b.len;
        r->u.s.data = b.data ? b.data : (char *)xmalloc(1);
        if (!b.data) r->u.s.data[0] = 0;
        return r;
    }
    if (!strcmp(name, "replace")) {
        if (argc < 2) py_raise(PY_EXC_TYPE, "replace() takes at least 2 arguments");
        {
            PyVal *oldv = args[0], *newv = args[1];
            int64_t count = argc > 2 ? py_as_int(args[2]) : -1;
            Buf b;
            size_t i = 0;
            int64_t done = 0;
            if (oldv->type != PY_T_STR || newv->type != PY_T_STR)
                py_raise(PY_EXC_TYPE, "replace() arguments must be strings");
            buf_init(&b);
            while (i < s->u.s.len) {
                if ((count < 0 || done < count) && oldv->u.s.len > 0 &&
                    i + oldv->u.s.len <= s->u.s.len &&
                    memcmp(s->u.s.data + i, oldv->u.s.data, oldv->u.s.len) == 0) {
                    buf_add(&b, newv->u.s.data, newv->u.s.len);
                    i += oldv->u.s.len;
                    done++;
                } else {
                    buf_addc(&b, s->u.s.data[i++]);
                }
            }
            {
                PyVal *r = py_strn(b.data ? b.data : "", b.len);
                free(b.data);
                return r;
            }
        }
    }
    if (!strcmp(name, "find") || !strcmp(name, "rfind") || !strcmp(name, "index") ||
        !strcmp(name, "rindex") || !strcmp(name, "count")) {
        PyVal *needle = argc > 0 ? args[0] : py_none();
        size_t i, start = 0, end = s->u.s.len;
        ptrdiff_t found = -1;
        int rev = name[0] == 'r';
        int is_count = !strcmp(name, "count");
        int64_t count = 0;
        if (needle->type != PY_T_STR) py_raise(PY_EXC_TYPE, "expected a string");
        if (argc > 1) start = (size_t)py_as_int(args[1]);
        if (argc > 2) end = (size_t)py_as_int(args[2]);
        if (end > s->u.s.len) end = s->u.s.len;
        if (start > end) start = end;
        if (needle->u.s.len == 0) {
            if (is_count) return py_int((int64_t)(end - start + 1));
            found = (ptrdiff_t)(rev ? end : start);
        } else {
            for (i = start; i + needle->u.s.len <= end; i++) {
                if (memcmp(s->u.s.data + i, needle->u.s.data, needle->u.s.len) == 0) {
                    if (is_count) count++;
                    else if (!rev) { found = (ptrdiff_t)i; break; }
                    else found = (ptrdiff_t)i;
                }
            }
        }
        if (is_count) return py_int(count);
        if (found >= 0) {
            size_t off = (size_t)found;
            return py_int((int64_t)u8_len(s->u.s.data, off));
        }
        if (!strcmp(name, "index") || !strcmp(name, "rindex"))
            py_raise(PY_EXC_VALUE, "substring not found");
        return py_int(-1);
    }
    if (!strcmp(name, "startswith") || !strcmp(name, "endswith")) {
        int ends = name[0] == 'e';
        PyVal *prefix = argc > 0 ? args[0] : py_none();
        int ok = 0;
        if (prefix->type == PY_T_TUPLE) {
            size_t i;
            for (i = 0; i < prefix->u.seq.len && !ok; i++) {
                PyVal *tmp[2];
                tmp[0] = prefix->u.seq.items[i];
                ok = py_truth(str_method(s, name, tmp, 1, NULL));
            }
        } else {
            const char *p;
            size_t pl;
            if (prefix->type != PY_T_STR) py_raise(PY_EXC_TYPE, "expected str");
            p = prefix->u.s.data;
            pl = prefix->u.s.len;
            if (ends) ok = s->u.s.len >= pl && memcmp(s->u.s.data + s->u.s.len - pl, p, pl) == 0;
            else ok = s->u.s.len >= pl && memcmp(s->u.s.data, p, pl) == 0;
        }
        return py_bool(ok);
    }
    if (!strcmp(name, "removeprefix") || !strcmp(name, "removesuffix")) {
        PyVal *fix = argc > 0 ? args[0] : py_none();
        int suffix = name[0] == 'r';
        if (fix->type != PY_T_STR) py_raise(PY_EXC_TYPE, "expected str");
        if (suffix) {
            if (s->u.s.len >= fix->u.s.len &&
                memcmp(s->u.s.data + s->u.s.len - fix->u.s.len, fix->u.s.data, fix->u.s.len) == 0)
                return str_slice(s, 0, s->u.s.len - fix->u.s.len);
        } else {
            if (s->u.s.len >= fix->u.s.len &&
                memcmp(s->u.s.data, fix->u.s.data, fix->u.s.len) == 0)
                return str_slice(s, fix->u.s.len, s->u.s.len);
        }
        py_retain(s);
        return s;
    }
    if (!strcmp(name, "partition") || !strcmp(name, "rpartition")) {
        PyVal *sep = argc > 0 ? args[0] : py_none();
        size_t i;
        ptrdiff_t found = -1;
        int rev = name[0] == 'r';
        PyVal *arr[3];
        if (sep->type != PY_T_STR || sep->u.s.len == 0)
            py_raise(PY_EXC_VALUE, "empty separator");
        for (i = 0; i + sep->u.s.len <= s->u.s.len; i++)
            if (memcmp(s->u.s.data + i, sep->u.s.data, sep->u.s.len) == 0) {
                found = (ptrdiff_t)i;
                if (!rev) break;
            }
        if (found < 0) {
            if (rev) { arr[0] = py_str(""); arr[1] = py_str(""); arr[2] = s; }
            else { arr[0] = s; arr[1] = py_str(""); arr[2] = py_str(""); }
        } else {
            arr[0] = py_strn(s->u.s.data, (size_t)found);
            arr[1] = py_strn(s->u.s.data + found, sep->u.s.len);
            arr[2] = py_strn(s->u.s.data + found + sep->u.s.len,
                             s->u.s.len - (size_t)found - sep->u.s.len);
        }
        {
            PyVal *r = py_tuple_from(arr, 3);
            if (found >= 0) { py_release(arr[0]); py_release(arr[1]); py_release(arr[2]); }
            else if (rev) { py_release(arr[0]); py_release(arr[1]); }
            else { py_release(arr[1]); py_release(arr[2]); }
            return r;
        }
    }
    if (!strcmp(name, "ljust") || !strcmp(name, "rjust") || !strcmp(name, "center") ||
        !strcmp(name, "zfill")) {
        int64_t width = argc > 0 ? py_as_int(args[0]) : 0;
        char fill = ' ';
        size_t pad, left;
        Buf b;
        if (!strcmp(name, "zfill")) fill = '0';
        else if (argc > 1) {
            if (args[1]->type != PY_T_STR || args[1]->u.s.len == 0)
                py_raise(PY_EXC_TYPE, "fill character must be a single character");
            fill = args[1]->u.s.data[0];
        }
        if (width <= (int64_t)s->u.s.len) { py_retain(s); return s; }
        pad = (size_t)width - s->u.s.len;
        buf_init(&b);
        if (!strcmp(name, "zfill")) {
            size_t i;
            size_t sign = (s->u.s.len && (s->u.s.data[0] == '-' || s->u.s.data[0] == '+')) ? 1 : 0;
            if (sign) buf_addc(&b, s->u.s.data[0]);
            for (i = 0; i < pad; i++) buf_addc(&b, '0');
            buf_add(&b, s->u.s.data + sign, s->u.s.len - sign);
        } else {
            left = !strcmp(name, "ljust") ? 0 : !strcmp(name, "rjust") ? pad : pad / 2;
            { size_t i; for (i = 0; i < left; i++) buf_addc(&b, fill); }
            buf_add(&b, s->u.s.data, s->u.s.len);
            { size_t i; for (i = 0; i < pad - left; i++) buf_addc(&b, fill); }
        }
        {
            PyVal *r = py_strn(b.data, b.len);
            free(b.data);
            return r;
        }
    }
    if (!strcmp(name, "isdigit") || !strcmp(name, "isalpha") || !strcmp(name, "isalnum") ||
        !strcmp(name, "isspace") || !strcmp(name, "islower") || !strcmp(name, "isupper") ||
        !strcmp(name, "isnumeric") || !strcmp(name, "isidentifier")) {
        size_t i;
        int ok = s->u.s.len > 0;
        for (i = 0; i < s->u.s.len && ok; i++) {
            unsigned char c = (unsigned char)s->u.s.data[i];
            if (!strcmp(name, "isdigit") || !strcmp(name, "isnumeric")) ok = isdigit(c) != 0;
            else if (!strcmp(name, "isalpha")) ok = isalpha(c) != 0;
            else if (!strcmp(name, "isalnum")) ok = isalnum(c) != 0;
            else if (!strcmp(name, "isspace")) ok = str_is_space(c);
            else if (!strcmp(name, "islower")) ok = !isupper(c);
            else if (!strcmp(name, "isupper")) ok = !islower(c);
            else ok = isalnum(c) || c == '_';
        }
        return py_bool(ok);
    }
    if (!strcmp(name, "encode") || !strcmp(name, "decode")) {
        py_retain(s);
        return s;
    }
    if (!strcmp(name, "format")) return str_format_impl(s, args, argc, kwargs);
    if (!strcmp(name, "__len__")) return py_int((int64_t)py_len(s));
    if (!strcmp(name, "__str__")) { py_retain(s); return s; }
    if (!strcmp(name, "__contains__")) return py_bool(argc > 0 && py_contains(s, args[0]));
    py_raise(PY_EXC_ATTRIBUTE, "str object has no such method");
    return py_none();
}

/* ----------------------------------------------------------- methods: list */
static void sort_array(PyVal **items, size_t n, PyVal *key, int reverse) {
    size_t i, j;
    PyVal **keys = NULL;
    if (key && key->type != PY_T_NONE) {
        keys = (PyVal **)xmalloc(n * sizeof(PyVal *));
        for (i = 0; i < n; i++) keys[i] = py_call(key, &items[i], 1, NULL);
    }
    for (i = 1; i < n; i++) {
        PyVal *item = items[i];
        PyVal *kitem = keys ? keys[i] : item;
        j = i;
        while (j > 0) {
            PyVal *prev = keys ? keys[j - 1] : items[j - 1];
            int c = py_cmp(PY_CMP_LT, reverse ? prev : kitem, reverse ? kitem : prev);
            if (!c) break;
            items[j] = items[j - 1];
            if (keys) keys[j] = keys[j - 1];
            j--;
        }
        items[j] = item;
        if (keys) keys[j] = kitem;
    }
    if (keys) {
        for (i = 0; i < n; i++) py_release(keys[i]);
        free(keys);
    }
}

static PyVal *list_method(PyVal *l, const char *name, PyVal **args, size_t argc,
                          PyVal *kwargs) {
    if (!strcmp(name, "append")) {
        if (argc < 1) py_raise(PY_EXC_TYPE, "append() takes exactly one argument");
        seq_push(l, args[0]);
        return py_none();
    }
    if (!strcmp(name, "extend")) {
        PyVal *it, *item = NULL;
        if (argc < 1) py_raise(PY_EXC_TYPE, "extend() takes exactly one argument");
        if (args[0] == l) {
            size_t i;
            for (i = 0; i < l->u.seq.len; i++) seq_push(l, l->u.seq.items[i]);
            return py_none();
        }
        it = py_iter(args[0]);
        while (py_iter_next(it, &item)) seq_push(l, item);
        py_release(it);
        py_release(item);
        return py_none();
    }
    if (!strcmp(name, "insert")) {
        int64_t at;
        size_t i;
        if (argc < 2) py_raise(PY_EXC_TYPE, "insert() takes 2 arguments");
        at = py_as_int(args[0]);
        if (at < 0) at += (int64_t)l->u.seq.len;
        if (at < 0) at = 0;
        if (at > (int64_t)l->u.seq.len) at = (int64_t)l->u.seq.len;
        seq_push(l, args[1]);
        for (i = l->u.seq.len - 1; i > (size_t)at; i--)
            l->u.seq.items[i] = l->u.seq.items[i - 1];
        l->u.seq.items[at] = args[1];
        return py_none();
    }
    if (!strcmp(name, "pop")) {
        int64_t at = argc > 0 ? py_as_int(args[0]) : (int64_t)l->u.seq.len - 1;
        size_t i;
        PyVal *r;
        if (l->u.seq.len == 0) py_raise(PY_EXC_INDEX, "pop from empty list");
        if (at < 0) at += (int64_t)l->u.seq.len;
        if (at < 0 || at >= (int64_t)l->u.seq.len) py_raise(PY_EXC_INDEX, "pop index out of range");
        r = l->u.seq.items[at];
        for (i = (size_t)at; i + 1 < l->u.seq.len; i++)
            l->u.seq.items[i] = l->u.seq.items[i + 1];
        l->u.seq.len--;
        return r;
    }
    if (!strcmp(name, "remove")) {
        size_t i;
        if (argc < 1) py_raise(PY_EXC_TYPE, "remove() takes one argument");
        for (i = 0; i < l->u.seq.len; i++) {
            if (py_cmp(PY_CMP_EQ, l->u.seq.items[i], args[0])) {
                py_release(l->u.seq.items[i]);
                for (; i + 1 < l->u.seq.len; i++)
                    l->u.seq.items[i] = l->u.seq.items[i + 1];
                l->u.seq.len--;
                return py_none();
            }
        }
        py_raise(PY_EXC_VALUE, "list.remove(x): x not in list");
    }
    if (!strcmp(name, "clear")) { 
        size_t i;
        for (i = 0; i < l->u.seq.len; i++) py_release(l->u.seq.items[i]);
        l->u.seq.len = 0;
        return py_none();
    }
    if (!strcmp(name, "reverse")) {
        size_t i, j;
        for (i = 0, j = l->u.seq.len; i + 1 < j; i++, j--) {
            PyVal *t = l->u.seq.items[i];
            l->u.seq.items[i] = l->u.seq.items[j - 1];
            l->u.seq.items[j - 1] = t;
        }
        return py_none();
    }
    if (!strcmp(name, "sort")) {
        PyVal *key = py_kw_get(kwargs, "key");
        PyVal *rev = py_kw_get(kwargs, "reverse");
        sort_array(l->u.seq.items, l->u.seq.len, key, rev && py_truth(rev));
        return py_none();
    }
    if (!strcmp(name, "count") || !strcmp(name, "index")) {
        size_t i, count = 0;
        for (i = 0; i < l->u.seq.len; i++) {
            if (py_cmp(PY_CMP_EQ, l->u.seq.items[i], args[0])) {
                if (!strcmp(name, "index")) return py_int((int64_t)i);
                count++;
            }
        }
        if (!strcmp(name, "index")) py_raise(PY_EXC_VALUE, "value is not in list");
        return py_int((int64_t)count);
    }
    if (!strcmp(name, "copy")) {
        PyVal *r = py_list_new();
        size_t i;
        for (i = 0; i < l->u.seq.len; i++) seq_push(r, l->u.seq.items[i]);
        return r;
    }
    if (!strcmp(name, "__len__")) return py_int((int64_t)l->u.seq.len);
    py_raise(PY_EXC_ATTRIBUTE, "list object has no such method");
    return py_none();
}

/* ----------------------------------------------------------- methods: dict */
static PyVal *dict_method(PyVal *d, const char *name, PyVal **args, size_t argc,
                          PyVal *kwargs) {
    if (!strcmp(name, "get")) {
        PyVal *r = argc > 0 ? map_get(d, args[0]) : NULL;
        if (r) { py_retain(r); return r; }
        if (argc > 1) { py_retain(args[1]); return args[1]; }
        return py_none();
    }
    if (!strcmp(name, "keys") || !strcmp(name, "values") || !strcmp(name, "items")) {
        return py_dict_view(d, !strcmp(name, "keys") ? 0 :
                               !strcmp(name, "values") ? 1 : 2);
    }
    if (!strcmp(name, "update")) {
        size_t i;
        if (argc > 0) {
            if (args[0]->type == PY_T_DICT) {
                for (i = 0; i < args[0]->u.map.len; i++)
                    map_put(d, args[0]->u.map.keys[i], args[0]->u.map.vals[i]);
            } else {
                PyVal *it = py_iter(args[0]), *item = NULL;
                while (py_iter_next(it, &item)) {
                    if (item->type != PY_T_LIST && item->type != PY_T_TUPLE)
                        py_raise(PY_EXC_TYPE, "cannot convert sequence element to pair");
                    if (item->u.seq.len != 2) py_raise(PY_EXC_VALUE, "dictionary update sequence has wrong length");
                    map_put(d, item->u.seq.items[0], item->u.seq.items[1]);
                }
                py_release(it);
                py_release(item);
            }
        }
        if (kwargs && kwargs->type == PY_T_DICT)
            for (i = 0; i < kwargs->u.map.len; i++)
                map_put(d, kwargs->u.map.keys[i], kwargs->u.map.vals[i]);
        return py_none();
    }
    if (!strcmp(name, "setdefault")) {
        PyVal *r;
        if (argc < 1) py_raise(PY_EXC_TYPE, "setdefault() takes at least one argument");
        r = map_get(d, args[0]);
        if (r) { py_retain(r); return r; }
        map_put(d, args[0], argc > 1 ? args[1] : py_none());
        r = map_get(d, args[0]);
        py_retain(r);
        return r;
    }
    if (!strcmp(name, "pop")) {
        PyVal *r;
        if (argc < 1) py_raise(PY_EXC_TYPE, "pop() takes at least one argument");
        r = map_get(d, args[0]);
        if (!r) {
            if (argc > 1) { py_retain(args[1]); return args[1]; }
            py_raise(PY_EXC_KEY, "key not found");
        }
        py_retain(r);
        map_del(d, args[0]);
        return r;
    }
    if (!strcmp(name, "popitem")) {
        PyVal *k, *v, *arr[2], *r;
        if (d->u.map.len == 0) py_raise(PY_EXC_KEY, "popitem(): dictionary is empty");
        k = d->u.map.keys[d->u.map.len - 1];
        v = d->u.map.vals[d->u.map.len - 1];
        py_retain(k);
        py_retain(v);
        map_del(d, k);
        arr[0] = k;
        arr[1] = v;
        r = py_tuple_from(arr, 2);
        py_release(k);
        py_release(v);
        return r;
    }
    if (!strcmp(name, "clear")) {
        size_t i;
        for (i = 0; i < d->u.map.len; i++) {
            py_release(d->u.map.keys[i]);
            py_release(d->u.map.vals[i]);
        }
        d->u.map.len = 0;
        return py_none();
    }
    if (!strcmp(name, "copy")) {
        PyVal *r = py_dict_new();
        size_t i;
        for (i = 0; i < d->u.map.len; i++) map_put(r, d->u.map.keys[i], d->u.map.vals[i]);
        return r;
    }
    if (!strcmp(name, "fromkeys")) {
        PyVal *r = py_dict_new();
        PyVal *it, *item = NULL;
        PyVal *val = argc > 1 ? args[1] : py_none();
        if (argc < 1) py_raise(PY_EXC_TYPE, "fromkeys() takes an iterable");
        it = py_iter(args[0]);
        while (py_iter_next(it, &item)) map_put(r, item, val);
        py_release(it);
        py_release(item);
        return r;
    }
    if (!strcmp(name, "__len__")) return py_int((int64_t)d->u.map.len);
    py_raise(PY_EXC_ATTRIBUTE, "dict object has no such method");
    return py_none();
}

/* ------------------------------------------------------------ methods: set */
static PyVal *set_method(PyVal *s, const char *name, PyVal **args, size_t argc,
                         PyVal *kwargs) {
    size_t i;
    (void)kwargs;
    if (!strcmp(name, "add")) {
        if (argc < 1) py_raise(PY_EXC_TYPE, "add() takes one argument");
        map_put(s, args[0], py_bool(1));
        return py_none();
    }
    if (!strcmp(name, "discard") || !strcmp(name, "remove")) {
        if (argc < 1) py_raise(PY_EXC_TYPE, "remove() takes one argument");
        if (!map_del(s, args[0]) && !strcmp(name, "remove"))
            py_raise(PY_EXC_KEY, "set.remove(x): x not in set");
        return py_none();
    }
    if (!strcmp(name, "pop")) {
        PyVal *k, *r;
        if (s->u.map.len == 0) py_raise(PY_EXC_KEY, "pop from an empty set");
        k = s->u.map.keys[0];
        py_retain(k);
        map_del(s, k);
        r = k;
        return r;
    }
    if (!strcmp(name, "clear")) {
        for (i = 0; i < s->u.map.len; i++) {
            py_release(s->u.map.keys[i]);
            py_release(s->u.map.vals[i]);
        }
        s->u.map.len = 0;
        return py_none();
    }
    if (!strcmp(name, "update")) {
        size_t a;
        for (a = 0; a < argc; a++) {
            PyVal *it = py_iter(args[a]), *item = NULL;
            while (py_iter_next(it, &item)) map_put(s, item, py_bool(1));
            py_release(it);
            py_release(item);
        }
        return py_none();
    }
    if (!strcmp(name, "union") || !strcmp(name, "intersection") || !strcmp(name, "difference") ||
        !strcmp(name, "symmetric_difference")) {
        PyVal *r = py_set_new();
        size_t a;
        if (!strcmp(name, "union")) {
            for (i = 0; i < s->u.map.len; i++) map_put(r, s->u.map.keys[i], py_bool(1));
            for (a = 0; a < argc; a++) {
                PyVal *it = py_iter(args[a]), *item = NULL;
                while (py_iter_next(it, &item)) map_put(r, item, py_bool(1));
                py_release(it);
                py_release(item);
            }
        } else if (!strcmp(name, "intersection")) {
            for (i = 0; i < s->u.map.len; i++) {
                int in_all = 1;
                for (a = 0; a < argc && in_all; a++) in_all = py_contains(args[a], s->u.map.keys[i]);
                if (in_all || argc == 0) map_put(r, s->u.map.keys[i], py_bool(1));
            }
        } else if (!strcmp(name, "difference")) {
            for (i = 0; i < s->u.map.len; i++) {
                int in_any = 0;
                for (a = 0; a < argc && !in_any; a++) in_any = py_contains(args[a], s->u.map.keys[i]);
                if (!in_any) map_put(r, s->u.map.keys[i], py_bool(1));
            }
        } else {
            for (i = 0; i < s->u.map.len; i++) {
                int in_any = 0;
                for (a = 0; a < argc && !in_any; a++) in_any = py_contains(args[a], s->u.map.keys[i]);
                if (!in_any) map_put(r, s->u.map.keys[i], py_bool(1));
            }
            for (a = 0; a < argc; a++) {
                PyVal *it = py_iter(args[a]), *item = NULL;
                while (py_iter_next(it, &item)) {
                    if (map_index(s, item) < 0) map_put(r, item, py_bool(1));
                }
                py_release(it);
                py_release(item);
            }
        }
        return r;
    }
    if (!strcmp(name, "issubset") || !strcmp(name, "issuperset") || !strcmp(name, "isdisjoint")) {
        PyVal *other = argc > 0 ? args[0] : py_none();
        int ok = 1;
        if (!strcmp(name, "issubset")) {
            for (i = 0; i < s->u.map.len && ok; i++) ok = py_contains(other, s->u.map.keys[i]);
        } else if (!strcmp(name, "issuperset")) {
            PyVal *it = py_iter(other), *item = NULL;
            while (py_iter_next(it, &item) && ok) ok = map_index(s, item) >= 0;
            py_release(it);
            py_release(item);
        } else {
            PyVal *it = py_iter(other), *item = NULL;
            while (py_iter_next(it, &item) && ok) ok = map_index(s, item) < 0;
            py_release(it);
            py_release(item);
        }
        return py_bool(ok);
    }
    if (!strcmp(name, "copy")) {
        PyVal *r = py_set_new();
        for (i = 0; i < s->u.map.len; i++) map_put(r, s->u.map.keys[i], py_bool(1));
        return r;
    }
    py_raise(PY_EXC_ATTRIBUTE, "set object has no such method");
    return py_none();
}

/* ---------------------------------------------------------- methods: misc */
static PyVal *var_method(PyVal *v, const char *name, PyVal **args, size_t argc,
                          PyVal *kwargs) {
    (void)kwargs;
    if (!strcmp(name, "get")) {
        PyVal *r = v->u.var.value;
        if (!r) r = py_none();
        py_retain(r);
        return r;
    }
    if (!strcmp(name, "set")) {
        if (argc > 0) py_assign(&v->u.var.value, args[0]);
        return py_none();
    }
    if (!strcmp(name, "trace") || !strcmp(name, "trace_add")) return py_none();
    py_raise(PY_EXC_ATTRIBUTE, "variable has no such method");
    return py_none();
}

PyVal *py_method(PyVal *obj, const char *name, PyVal **args, size_t argc,
                 PyVal *kwargs) {
    if (!obj) py_raise(PY_EXC_ATTRIBUTE, "'NoneType' object has no attribute");
    if (obj->type == PY_T_DICTVIEW) {
        if (!strcmp(name, "__len__")) return py_int((int64_t)py_len(obj));
        if (!strcmp(name, "__iter__")) return py_iter(obj);
        if (!strcmp(name, "__str__")) return py_repr(obj);
    }
    if (obj->type == PY_T_WIDGET)
        return py_gui_method(obj->u.widget.w, name, args, argc, kwargs);
    if (obj->type == PY_T_VAR)
        return var_method(obj, name, args, argc, kwargs);
    if (obj->type == PY_T_FILE)
        return file_method(obj, name, args, argc, kwargs);
    if (obj->type == PY_T_DICT) {
        PyVal *cls = map_get_cstr(obj, "__class__");
        if (cls) {
            PyVal *m = py_getattr(obj, name);
            PyVal *r = py_call(m, args, argc, kwargs);
            py_release(m);
            return r;
        }
        return dict_method(obj, name, args, argc, kwargs);
    }
    if (obj->type == PY_T_CLASS) {
        PyVal *m = py_getattr(obj, name);
        PyVal *r = py_call(m, args, argc, kwargs);
        py_release(m);
        return r;
    }
    switch (obj->type) {
    case PY_T_STR: return str_method(obj, name, args, argc, kwargs);
    case PY_T_LIST: return list_method(obj, name, args, argc, kwargs);
    case PY_T_TUPLE:
        if (!strcmp(name, "count") || !strcmp(name, "index")) {
            size_t i, count = 0;
            for (i = 0; i < obj->u.seq.len; i++) {
                if (py_cmp(PY_CMP_EQ, obj->u.seq.items[i], args[0])) {
                    if (!strcmp(name, "index")) return py_int((int64_t)i);
                    count++;
                }
            }
            if (!strcmp(name, "index")) py_raise(PY_EXC_VALUE, "tuple.index(x): x not in tuple");
            return py_int((int64_t)count);
        }
        break;
    case PY_T_SET: return set_method(obj, name, args, argc, kwargs);
    case PY_T_RANGE:
        if (!strcmp(name, "count")) {
            return py_int(py_contains(obj, args[0]) ? 1 : 0);
        }
        if (!strcmp(name, "index")) {
            if (!py_contains(obj, args[0])) py_raise(PY_EXC_VALUE, "not in range");
            return py_int((py_as_int(args[0]) - obj->u.range.start) / obj->u.range.step);
        }
        break;
    default:
        break;
    }
    {
        PyVal *m = py_getattr(obj, name);
        PyVal *r = py_call(m, args, argc, kwargs);
        py_release(m);
        return r;
    }
}
/* ---------------------------------------------------------------- builtins */
static PyVal *arg_at(PyVal **args, size_t argc, size_t i) {
    return i < argc ? args[i] : py_none();
}

PyVal *py_print(PyVal **args, size_t argc, PyVal *kwargs) {
    PyVal *sep = py_kw_get(kwargs, "sep");
    PyVal *end = py_kw_get(kwargs, "end");
    PyVal *file = py_kw_get(kwargs, "file");
    size_t i;
    FILE *fp = stdout;
    if (!sep || sep->type == PY_T_NONE) sep = NULL;
    if (!end || end->type == PY_T_NONE) end = NULL;
    if (file && file->type == PY_T_FILE && file->u.file.fp) fp = file->u.file.fp;
    for (i = 0; i < argc; i++) {
        PyVal *s = py_str_of(args[i]);
        if (i) {
            if (sep) fwrite(sep->u.s.data, 1, sep->u.s.len, fp);
            else fputc(' ', fp);
        }
        fwrite(s->u.s.data, 1, s->u.s.len, fp);
        py_release(s);
    }
    if (end) fwrite(end->u.s.data, 1, end->u.s.len, fp);
    else fputc('\n', fp);
    fflush(fp);
    return py_none();
}

PyVal *py_input(PyVal **args, size_t argc, PyVal *kwargs) {
    Buf b;
    int c, got = 0;
    PyVal *r;
    (void)kwargs;
    buf_init(&b);
    if (argc > 0) {
        PyVal *p = py_str_of(args[0]);
        fwrite(p->u.s.data, 1, p->u.s.len, stdout);
        py_release(p);
        fflush(stdout);
    }
    while ((c = fgetc(stdin)) != EOF) {
        got = 1;
        if (c == '\n') break;
        if (c != '\r') buf_addc(&b, (char)c);
    }
    if (!got && b.len == 0) {
        free(b.data);
        return py_str("");
    }
    r = py_strn(b.data ? b.data : "", b.len);
    free(b.data);
    return r;
}

PyVal *py_builtin_len(PyVal **args, size_t argc, PyVal *kwargs) {
    (void)kwargs;
    if (argc < 1) py_raise(PY_EXC_TYPE, "len() takes exactly one argument");
    return py_int((int64_t)py_len(args[0]));
}

PyVal *py_builtin_int(PyVal **args, size_t argc, PyVal *kwargs) {
    (void)kwargs;
    if (argc == 0) return py_int(0);
    if (args[0]->type == PY_T_STR) {
        int base = argc > 1 ? (int)py_as_int(args[1]) : 10;
        char *end = NULL;
        long long v;
        const char *s = args[0]->u.s.data;
        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
        v = strtoll(s, &end, base);
        if (end == s) py_raise(PY_EXC_VALUE, "invalid literal for int()");
        return py_int((int64_t)v);
    }
    return py_int(py_as_int(args[0]));
}

PyVal *py_builtin_float(PyVal **args, size_t argc, PyVal *kwargs) {
    (void)kwargs;
    if (argc == 0) return py_float(0.0);
    return py_float(py_as_float(args[0]));
}

PyVal *py_builtin_str(PyVal **args, size_t argc, PyVal *kwargs) {
    (void)kwargs;
    if (argc == 0) return py_str("");
    return py_str_of(args[0]);
}

PyVal *py_builtin_bool(PyVal **args, size_t argc, PyVal *kwargs) {
    (void)kwargs;
    return py_bool(argc > 0 && py_truth(args[0]));
}

PyVal *py_builtin_list(PyVal **args, size_t argc, PyVal *kwargs) {
    PyVal *r = py_list_new();
    (void)kwargs;
    if (argc > 0) {
        PyVal *it = py_iter(args[0]), *item = NULL;
        while (py_iter_next(it, &item)) seq_push(r, item);
        py_release(it);
        py_release(item);
    }
    return r;
}

PyVal *py_builtin_tuple(PyVal **args, size_t argc, PyVal *kwargs) {
    PyVal *l = py_builtin_list(args, argc, kwargs);
    PyVal *r = py_tuple_from(l->u.seq.items, l->u.seq.len);
    py_release(l);
    return r;
}

PyVal *py_builtin_dict(PyVal **args, size_t argc, PyVal *kwargs) {
    PyVal *d = py_dict_new();
    size_t i;
    if (argc > 0) {
        if (args[0]->type == PY_T_DICT) {
            for (i = 0; i < args[0]->u.map.len; i++)
                map_put(d, args[0]->u.map.keys[i], args[0]->u.map.vals[i]);
        } else {
            PyVal *it = py_iter(args[0]), *item = NULL;
            while (py_iter_next(it, &item)) {
                if ((item->type != PY_T_LIST && item->type != PY_T_TUPLE) || item->u.seq.len != 2)
                    py_raise(PY_EXC_VALUE, "dictionary update sequence has wrong length");
                map_put(d, item->u.seq.items[0], item->u.seq.items[1]);
            }
            py_release(it);
            py_release(item);
        }
    }
    if (kwargs) {
        for (i = 0; i < kwargs->u.map.len; i++)
            map_put(d, kwargs->u.map.keys[i], kwargs->u.map.vals[i]);
    }
    return d;
}

PyVal *py_builtin_set(PyVal **args, size_t argc, PyVal *kwargs) {
    PyVal *s = py_set_new();
    if (argc > 0) {
        PyVal *it = py_iter(args[0]), *item = NULL;
        while (py_iter_next(it, &item)) map_put(s, item, py_bool(1));
        py_release(it);
        py_release(item);
    }
    (void)kwargs;
    return s;
}

PyVal *py_builtin_range(PyVal **args, size_t argc, PyVal *kwargs) {
    int64_t a = 0, b = 0, c = 1;
    (void)kwargs;
    if (argc == 1) { b = py_as_int(args[0]); }
    else if (argc == 2) { a = py_as_int(args[0]); b = py_as_int(args[1]); }
    else if (argc >= 3) { a = py_as_int(args[0]); b = py_as_int(args[1]); c = py_as_int(args[2]); }
    else py_raise(PY_EXC_TYPE, "range expected at least 1 argument");
    return py_range(a, b, c);
}

PyVal *py_builtin_abs(PyVal **args, size_t argc, PyVal *kwargs) {
    PyVal *v;
    (void)kwargs;
    if (argc < 1) py_raise(PY_EXC_TYPE, "abs() takes one argument");
    v = args[0];
    if (v->type == PY_T_INT) return py_int(v->u.i < 0 ? -v->u.i : v->u.i);
    if (v->type == PY_T_BOOL) return py_int(v->u.b);
    if (v->type == PY_T_FLOAT) return py_float(fabs(v->u.f));
    py_raise(PY_EXC_TYPE, "bad operand type for abs()");
    return py_none();
}

static PyVal *minmax(PyVal **args, size_t argc, PyVal *kwargs, int want_max) {
    PyVal *best = NULL, *it = NULL, *item = NULL;
    PyVal *key = py_kw_get(kwargs, "key");
    int have = 0;
    if (argc == 0) py_raise(PY_EXC_TYPE, "expected at least one argument");
    if (argc == 1) { it = py_iter(args[0]); }
    else {
        size_t i;
        for (i = 0; i < argc; i++) {
            PyVal *k = key && key->type != PY_T_NONE ? py_call(key, &args[i], 1, NULL) : args[i];
            if (!have || (want_max ? py_cmp(PY_CMP_GT, k, best) : py_cmp(PY_CMP_LT, k, best))) {
                py_assign(&best, args[i]);
                have = 1;
            }
            if (k != args[i]) py_release(k);
        }
        py_retain(best);
        return best;
    }
    while (py_iter_next(it, &item)) {
        PyVal *k = key && key->type != PY_T_NONE ? py_call(key, &item, 1, NULL) : item;
        if (!have || (want_max ? py_cmp(PY_CMP_GT, k, best) : py_cmp(PY_CMP_LT, k, best))) {
            py_assign(&best, item);
            have = 1;
        }
        if (k != item) py_release(k);
    }
    py_release(it);
    py_release(item);
    if (!have) py_raise(PY_EXC_VALUE, "min()/max() arg is an empty sequence");
    py_retain(best);
    return best;
}
PyVal *py_builtin_min(PyVal **args, size_t argc, PyVal *kwargs) { return minmax(args, argc, kwargs, 0); }
PyVal *py_builtin_max(PyVal **args, size_t argc, PyVal *kwargs) { return minmax(args, argc, kwargs, 1); }

PyVal *py_builtin_sum(PyVal **args, size_t argc, PyVal *kwargs) {
    PyVal *acc = argc > 1 ? args[1] : py_int(0);
    PyVal *it, *item = NULL;
    py_retain(acc);
    if (argc < 1) py_raise(PY_EXC_TYPE, "sum() takes at least one argument");
    it = py_iter(args[0]);
    while (py_iter_next(it, &item)) {
        PyVal *n = py_binop(PY_OP_ADD, acc, item);
        py_release(acc);
        acc = n;
    }
    py_release(it);
    py_release(item);
    (void)kwargs;
    return acc;
}

PyVal *py_builtin_sorted(PyVal **args, size_t argc, PyVal *kwargs) {
    PyVal *l;
    PyVal *key = py_kw_get(kwargs, "key");
    PyVal *rev = py_kw_get(kwargs, "reverse");
    if (argc < 1) py_raise(PY_EXC_TYPE, "sorted() takes one argument");
    l = py_builtin_list(args, 1, NULL);
    sort_array(l->u.seq.items, l->u.seq.len, key, rev && py_truth(rev));
    return l;
}

PyVal *py_builtin_reversed(PyVal **args, size_t argc, PyVal *kwargs) {
    PyVal *l, *r;
    size_t i;
    (void)kwargs;
    if (argc < 1) py_raise(PY_EXC_TYPE, "reversed() takes one argument");
    l = py_builtin_list(args, 1, NULL);
    r = py_list_new();
    for (i = l->u.seq.len; i > 0; i--) seq_push(r, l->u.seq.items[i - 1]);
    py_release(l);
    return r;
}

PyVal *py_builtin_enumerate(PyVal **args, size_t argc, PyVal *kwargs) {
    int64_t start;
    PyVal *r = py_list_new();
    PyVal *it, *item = NULL;
    PyVal *startv = py_kw_get(kwargs, "start");
    start = startv ? py_as_int(startv) : (argc > 1 ? py_as_int(args[1]) : 0);
    if (argc < 1) py_raise(PY_EXC_TYPE, "enumerate() takes an iterable");
    it = py_iter(args[0]);
    while (py_iter_next(it, &item)) {
        PyVal *pair[2];
        pair[0] = py_int(start++);
        pair[1] = item;
        seq_push(r, py_tuple_from(pair, 2));
        py_release(pair[0]);
    }
    py_release(it);
    py_release(item);
    return r;
}

PyVal *py_builtin_slice(PyVal **args, size_t argc, PyVal *kwargs) {
    (void)kwargs;
    return py_slice_new(arg_at(args, argc, 0), arg_at(args, argc, 1),
                        arg_at(args, argc, 2));
}

PyVal *py_builtin_frozenset(PyVal **args, size_t argc, PyVal *kwargs) {
    return py_builtin_set(args, argc, kwargs);
}

PyVal *py_builtin_zip(PyVal **args, size_t argc, PyVal *kwargs) {
    PyVal *r = py_list_new();
    size_t i, n = argc;
    PyVal **iters;
    int done = 0;
    (void)kwargs;
    if (n == 0) return r;
    iters = (PyVal **)xmalloc(n * sizeof(PyVal *));
    for (i = 0; i < n; i++) iters[i] = py_iter(args[i]);
    while (!done) {
        PyVal **vals = (PyVal **)xmalloc(n * sizeof(PyVal *));
        for (i = 0; i < n; i++) vals[i] = NULL;
        for (i = 0; i < n; i++) {
            if (!py_iter_next(iters[i], &vals[i])) {
                size_t j;
                for (j = 0; j < i; j++) py_release(vals[j]);
                free(vals);
                done = 1;
                break;
            }
        }
        if (!done) {
            PyVal *t = py_tuple_from(vals, n);
            seq_push(r, t);
            py_release(t);
            for (i = 0; i < n; i++) py_release(vals[i]);
            free(vals);
        }
    }
    for (i = 0; i < n; i++) py_release(iters[i]);
    free(iters);
    return r;
}

PyVal *py_builtin_any(PyVal **args, size_t argc, PyVal *kwargs) {
    PyVal *it, *item = NULL;
    int r = 0;
    (void)kwargs;
    if (argc < 1) py_raise(PY_EXC_TYPE, "any() takes an iterable");
    it = py_iter(args[0]);
    while (py_iter_next(it, &item)) if (py_truth(item)) { r = 1; break; }
    py_release(it);
    py_release(item);
    return py_bool(r);
}
PyVal *py_builtin_all(PyVal **args, size_t argc, PyVal *kwargs) {
    PyVal *it, *item = NULL;
    int r = 1;
    (void)kwargs;
    if (argc < 1) py_raise(PY_EXC_TYPE, "all() takes an iterable");
    it = py_iter(args[0]);
    while (py_iter_next(it, &item)) if (!py_truth(item)) { r = 0; break; }
    py_release(it);
    py_release(item);
    return py_bool(r);
}

PyVal *py_builtin_chr(PyVal **args, size_t argc, PyVal *kwargs) {
    char buf[4];
    size_t n;
    (void)kwargs;
    if (argc < 1) py_raise(PY_EXC_TYPE, "chr() takes one argument");
    u8_encode((uint32_t)py_as_int(args[0]), buf, &n);
    return py_strn(buf, n);
}
PyVal *py_builtin_ord(PyVal **args, size_t argc, PyVal *kwargs) {
    size_t adv;
    uint32_t cp;
    (void)kwargs;
    if (argc < 1 || args[0]->type != PY_T_STR || args[0]->u.s.len == 0)
        py_raise(PY_EXC_TYPE, "ord() expected a character");
    cp = u8_decode(args[0]->u.s.data, args[0]->u.s.len, &adv);
    return py_int((int64_t)cp);
}

PyVal *py_builtin_round(PyVal **args, size_t argc, PyVal *kwargs) {
    double x;
    int64_t nd = 0;
    (void)kwargs;
    if (argc < 1) py_raise(PY_EXC_TYPE, "round() takes one argument");
    x = py_as_float(args[0]);
    if (argc > 1) nd = py_as_int(args[1]);
    {
        double scale = pow(10.0, (double)nd);
        double r = nearbyint(x * scale) / scale;
        if (nd <= 0) return py_int((int64_t)r);
        return py_float(r);
    }
}

PyVal *py_builtin_pow(PyVal **args, size_t argc, PyVal *kwargs) {
    (void)kwargs;
    if (argc >= 3) {
        int64_t b = py_as_int(args[0]), e = py_as_int(args[1]), m = py_as_int(args[2]);
        int64_t r = 1;
        b %= m;
        while (e > 0) {
            if (e & 1) r = (r * b) % m;
            e >>= 1;
            b = (b * b) % m;
        }
        return py_int(r);
    }
    return py_binop(PY_OP_POW, arg_at(args, argc, 0), arg_at(args, argc, 1));
}

PyVal *py_builtin_divmod(PyVal **args, size_t argc, PyVal *kwargs) {
    PyVal *arr[2], *r;
    (void)kwargs;
    arr[0] = py_binop(PY_OP_FLOORDIV, args[0], args[1]);
    arr[1] = py_binop(PY_OP_MOD, args[0], args[1]);
    r = py_tuple_from(arr, 2);
    py_release(arr[0]);
    py_release(arr[1]);
    return r;
}

static PyVal *int_to_base(int64_t v, int base, const char *prefix, int upper) {
    char buf[80];
    char digits[36];
    int neg = v < 0;
    uint64_t u = neg ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
    int i = 0;
    const char *d = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    memcpy(digits, d, 16);
    do {
        buf[i++] = digits[u % (uint64_t)base];
        u /= (uint64_t)base;
    } while (u && i < 70);
    {
        Buf b;
        buf_init(&b);
        if (neg) buf_addc(&b, '-');
        buf_adds(&b, prefix);
        while (i > 0) buf_addc(&b, buf[--i]);
        {
            PyVal *r = py_strn(b.data ? b.data : "", b.len);
            free(b.data);
            return r;
        }
    }
}

PyVal *py_builtin_hex(PyVal **args, size_t argc, PyVal *kwargs) {
    (void)kwargs;
    return int_to_base(py_as_int(arg_at(args, argc, 0)), 16, "0x", 0);
}
PyVal *py_builtin_oct(PyVal **args, size_t argc, PyVal *kwargs) {
    (void)kwargs;
    return int_to_base(py_as_int(arg_at(args, argc, 0)), 8, "0o", 0);
}
PyVal *py_builtin_bin(PyVal **args, size_t argc, PyVal *kwargs) {
    (void)kwargs;
    return int_to_base(py_as_int(arg_at(args, argc, 0)), 2, "0b", 0);
}
PyVal *py_builtin_repr(PyVal **args, size_t argc, PyVal *kwargs) {
    (void)kwargs;
    return py_repr(arg_at(args, argc, 0));
}

PyVal *py_builtin_format(PyVal **args, size_t argc, PyVal *kwargs) {
    PyVal *v, *spec;
    int align = 0, fill = ' ', width = 0, prec = -1, zero = 0, alt = 0;
    char type = 0;
    const char *sp;
    size_t i = 0, sl;
    (void)kwargs;
    if (argc < 1) py_raise(PY_EXC_TYPE, "format() takes at least one argument");
    v = args[0];
    if (argc < 2 || args[1]->type != PY_T_STR) return py_str_of(v);
    spec = args[1];
    sp = spec->u.s.data;
    sl = spec->u.s.len;
    if (sl == 0) return py_str_of(v);
    if (sl >= 2 && (sp[1] == '<' || sp[1] == '>' || sp[1] == '^' || sp[1] == '=')) {
        fill = sp[0];
        align = sp[1];
        i = 2;
    } else if (sl >= 1 && (sp[0] == '<' || sp[0] == '>' || sp[0] == '^' || sp[0] == '=')) {
        align = sp[0];
        i = 1;
    }
    if (i < sl && (sp[i] == '+' || sp[i] == '-' || sp[i] == ' ')) i++;
    if (i < sl && sp[i] == '#') { alt = 1; i++; }
    if (i < sl && sp[i] == '0') { zero = 1; i++; }
    while (i < sl && isdigit((unsigned char)sp[i])) { width = width * 10 + (sp[i] - '0'); i++; }
    if (i < sl && sp[i] == '.') {
        i++; prec = 0;
        while (i < sl && isdigit((unsigned char)sp[i])) { prec = prec * 10 + (sp[i] - '0'); i++; }
    }
    if (i < sl) type = sp[i];
    if (!type) type = (v->type == PY_T_FLOAT) ? 'g' : (v->type == PY_T_STR ? 's' : 'd');
    if (type == 's' || v->type == PY_T_STR) {
        PyVal *s = py_str_of(v);
        PyVal *r;
        size_t len = s->u.s.len, pad = width > (int)len ? (size_t)width - len : 0;
        size_t left = align == '<' ? 0 : align == '^' ? pad / 2 : pad;
        Buf b;
        buf_init(&b);
        for (i = 0; i < left; i++) buf_addc(&b, (char)fill);
        buf_add(&b, s->u.s.data, s->u.s.len);
        for (i = 0; i < pad - left; i++) buf_addc(&b, (char)fill);
        py_release(s);
        r = py_strn(b.data, b.len);
        free(b.data);
        return r;
    }
    {
        char fmt[64], out[256];
        int n;
        size_t fl = 0;
        if (align && fill != ' ') { fmt[fl++] = '%'; fmt[fl++] = '-'; }
        else fmt[fl++] = '%';
        if (alt) fmt[fl++] = '#';
        if (zero) fmt[fl++] = '0';
        if (width > 0) fl += (size_t)snprintf(fmt + fl, sizeof(fmt) - fl, "%d", width);
        if (prec >= 0) fl += (size_t)snprintf(fmt + fl, sizeof(fmt) - fl, ".%d", prec);
        if (type == 'd' || type == 'i') fmt[fl++] = 'l', fmt[fl++] = 'l', fmt[fl++] = 'd';
        else if (type == 'x' || type == 'X' || type == 'o') {
            fmt[fl++] = 'l'; fmt[fl++] = 'l';
            if (type == 'X') alt = 0;
            fmt[fl++] = type;
        }
        else fmt[fl++] = type;
        fmt[fl] = 0;
        if (type == 'd' || type == 'i' || type == 'x' || type == 'X' || type == 'o')
            n = snprintf(out, sizeof(out), fmt, (long long)py_as_int(v));
        else
            n = snprintf(out, sizeof(out), fmt, py_as_float(v));
        if (n < 0) n = 0;
        if ((size_t)n >= sizeof(out)) n = (int)sizeof(out) - 1;
        return py_strn(out, (size_t)n);
    }
}

PyVal *py_builtin_id(PyVal **args, size_t argc, PyVal *kwargs) {
    (void)kwargs;
    return py_int((int64_t)(intptr_t)arg_at(args, argc, 0));
}
PyVal *py_builtin_hash(PyVal **args, size_t argc, PyVal *kwargs) {
    (void)kwargs;
    return py_int((int64_t)py_hash(arg_at(args, argc, 0)));
}

static int class_matches(PyVal *obj, PyVal *cls) {
    const char *want = NULL;
    if (cls->type == PY_T_CLASS) {
        PyVal *k;
        if (obj->type == PY_T_DICT) {
            k = map_get_cstr(obj, "__class__");
            if (k == cls) return 1;
        }
        want = cls->u.klass.name ? cls->u.klass.name->u.s.data : "object";
    } else if (cls->type == PY_T_FUNC) {
        want = cls->u.func.name;
    } else if (cls->type == PY_T_STR) {
        want = cls->u.s.data;
    } else if (cls->type == PY_T_TUPLE) {
        size_t i;
        for (i = 0; i < cls->u.seq.len; i++)
            if (class_matches(obj, cls->u.seq.items[i])) return 1;
        return 0;
    } else {
        return 0;
    }
    if (!want) return 0;
    if (!strcmp(want, "object")) return 1;
    {
        const char *got = type_name(obj);
        if (!strcmp(want, got)) return 1;
        if (!strcmp(want, "int") && !strcmp(got, "bool")) return 1;
        if (!strcmp(want, "complex") && (!strcmp(got, "int") || !strcmp(got, "float"))) return 1;
    }
    return 0;
}

PyVal *py_builtin_isinstance(PyVal **args, size_t argc, PyVal *kwargs) {
    (void)kwargs;
    if (argc < 2) py_raise(PY_EXC_TYPE, "isinstance() takes two arguments");
    return py_bool(class_matches(args[0], args[1]));
}
PyVal *py_builtin_type(PyVal **args, size_t argc, PyVal *kwargs) {
    (void)kwargs;
    if (argc < 1) py_raise(PY_EXC_TYPE, "type() takes one argument");
    return py_type(args[0]);
}

PyVal *py_builtin_open(PyVal **args, size_t argc, PyVal *kwargs) {
    const char *mode = "r";
    (void)kwargs;
    if (argc < 1) py_raise(PY_EXC_TYPE, "open() takes a filename");
    if (argc > 1) mode = py_cstr(args[1]);
    return py_open_file(py_cstr(args[0]), mode);
}

PyVal *py_builtin_iter(PyVal **args, size_t argc, PyVal *kwargs) {
    (void)kwargs;
    return py_iter(arg_at(args, argc, 0));
}
PyVal *py_builtin_next(PyVal **args, size_t argc, PyVal *kwargs) {
    PyVal *v = NULL;
    (void)kwargs;
    if (argc < 1) py_raise(PY_EXC_TYPE, "next() takes an iterator");
    if (!py_iter_next(args[0], &v)) {
        if (argc > 1) { py_retain(args[1]); return args[1]; }
        py_raise(PY_EXC_STOP_ITERATION, "StopIteration");
    }
    return v;
}

PyVal *py_builtin_map(PyVal **args, size_t argc, PyVal *kwargs) {
    PyVal *r = py_list_new();
    size_t i, n;
    PyVal **iters;
    int done = 0;
    (void)kwargs;
    if (argc < 2) py_raise(PY_EXC_TYPE, "map() requires at least two arguments");
    n = argc - 1;
    iters = (PyVal **)xmalloc(n * sizeof(PyVal *));
    for (i = 0; i < n; i++) iters[i] = py_iter(args[i + 1]);
    while (!done) {
        PyVal **vals = (PyVal **)xmalloc(n * sizeof(PyVal *));
        PyVal *res;
        for (i = 0; i < n; i++) vals[i] = NULL;
        for (i = 0; i < n; i++) {
            if (!py_iter_next(iters[i], &vals[i])) {
                size_t j;
                for (j = 0; j < i; j++) py_release(vals[j]);
                free(vals);
                done = 1;
                break;
            }
        }
        if (done) break;
        res = py_call(args[0], vals, n, NULL);
        seq_push(r, res);
        py_release(res);
        for (i = 0; i < n; i++) py_release(vals[i]);
        free(vals);
    }
    for (i = 0; i < n; i++) py_release(iters[i]);
    free(iters);
    return r;
}

PyVal *py_builtin_filter(PyVal **args, size_t argc, PyVal *kwargs) {
    PyVal *r = py_list_new();
    PyVal *it, *item = NULL;
    (void)kwargs;
    if (argc < 2) py_raise(PY_EXC_TYPE, "filter() requires two arguments");
    it = py_iter(args[1]);
    while (py_iter_next(it, &item)) {
        int keep = args[0]->type == PY_T_NONE ? py_truth(item) :
                   py_truth(py_call(args[0], &item, 1, NULL));
        if (keep) seq_push(r, item);
    }
    py_release(it);
    py_release(item);
    return r;
}

PyVal *py_builtin_getattr(PyVal **args, size_t argc, PyVal *kwargs) {
    (void)kwargs;
    if (argc < 2) py_raise(PY_EXC_TYPE, "getattr() takes two arguments");
    return py_getattr(args[0], py_cstr(args[1]));
}
PyVal *py_builtin_setattr(PyVal **args, size_t argc, PyVal *kwargs) {
    (void)kwargs;
    if (argc < 3) py_raise(PY_EXC_TYPE, "setattr() takes three arguments");
    py_setattr(args[0], py_cstr(args[1]), args[2]);
    return py_none();
}
PyVal *py_builtin_hasattr(PyVal **args, size_t argc, PyVal *kwargs) {
    PyJmp *h;
    int ok = 0;
    (void)kwargs;
    if (argc < 2) py_raise(PY_EXC_TYPE, "hasattr() takes two arguments");
    h = py_push_handler();
    if (setjmp(h->buf) == 0) {
        PyVal *v = py_getattr(args[0], py_cstr(args[1]));
        py_release(v);
        ok = 1;
    } else {
        py_exc_clear();
    }
    py_pop_handler(h);
    return py_bool(ok);
}
PyVal *py_builtin_callable(PyVal **args, size_t argc, PyVal *kwargs) {
    (void)kwargs;
    if (argc < 1) return py_bool(0);
    return py_bool(args[0]->type == PY_T_FUNC || args[0]->type == PY_T_BOUND ||
                   args[0]->type == PY_T_CLASS);
}
/* -------------------------------------------------------------- exceptions */
const char *py_exc_name(int kind) {
    switch (kind) {
    case PY_EXC_BASE: return "BaseException";
    case PY_EXC_EXCEPTION: return "Exception";
    case PY_EXC_ARITHMETIC: return "ArithmeticError";
    case PY_EXC_ASSERTION: return "AssertionError";
    case PY_EXC_ATTRIBUTE: return "AttributeError";
    case PY_EXC_INDEX: return "IndexError";
    case PY_EXC_KEY: return "KeyError";
    case PY_EXC_NAME: return "NameError";
    case PY_EXC_OS: return "OSError";
    case PY_EXC_RUNTIME: return "RuntimeError";
    case PY_EXC_STOP_ITERATION: return "StopIteration";
    case PY_EXC_TYPE: return "TypeError";
    case PY_EXC_VALUE: return "ValueError";
    case PY_EXC_ZERO_DIVISION: return "ZeroDivisionError";
    case PY_EXC_IMPORT: return "ImportError";
    case PY_EXC_UNSUPPORTED: return "NotImplementedError";
    case PY_EXC_EOF: return "EOFError";
    case PY_EXC_OVERFLOW: return "OverflowError";
    case PY_EXC_MEMORY: return "MemoryError";
    case PY_EXC_FILE: return "FileNotFoundError";
    default: return "Exception";
    }
}

int py_exc_matches(PyVal *exc, int exctype) {
    if (!exc || exc->type != PY_T_EXC) return 0;
    if (exctype == PY_EXC_BASE || exctype == PY_EXC_EXCEPTION) return 1;
    if (exc->u.exc.kind == exctype) return 1;
    if (exctype == PY_EXC_OS && exc->u.exc.kind == PY_EXC_FILE) return 1;
    if (exctype == PY_EXC_ARITHMETIC &&
        (exc->u.exc.kind == PY_EXC_ZERO_DIVISION || exc->u.exc.kind == PY_EXC_OVERFLOW))
        return 1;
    return 0;
}

int py_exc_matches_name(PyVal *exc, const char *name) {
    if (!exc || exc->type != PY_T_EXC) return 0;
    if (!strcmp(name, "BaseException") || !strcmp(name, "Exception")) return 1;
    if (!strcmp(name, py_exc_name(exc->u.exc.kind))) return 1;
    if (!strcmp(name, "OSError") && exc->u.exc.kind == PY_EXC_FILE) return 1;
    if (!strcmp(name, "ArithmeticError") &&
        (exc->u.exc.kind == PY_EXC_ZERO_DIVISION || exc->u.exc.kind == PY_EXC_OVERFLOW))
        return 1;
    if (!strcmp(name, "StopIteration") && exc->u.exc.kind == PY_EXC_STOP_ITERATION) return 1;
    return 0;
}

PyVal *py_make_exc(int exctype, const char *msg) {
    PyVal *v = val_new(PY_T_EXC);
    const char *m = msg ? msg : "";
    size_t n = strlen(m) + 1;
    v->u.exc.kind = exctype;
    v->u.exc.msg = (char *)xmalloc(n);
    memcpy(v->u.exc.msg, m, n);
    return v;
}

PyVal *py_make_exc_val(int exctype, PyVal *msg) {
    PyVal *s = py_str_of(msg);
    PyVal *v = py_make_exc(exctype, s ? s->u.s.data : "");
    py_release(s);
    return v;
}

void py_print_exc(PyVal *exc) {
    if (!exc || exc->type != PY_T_EXC) {
        fputs("py2c99: unknown exception\n", stderr);
        return;
    }
    fprintf(stderr, "Traceback (most recent call last):\n%s: %s\n",
            py_exc_name(exc->u.exc.kind), exc->u.exc.msg);
    fflush(stderr);
}

void py_raise_val(PyVal *exc) {
    if (!exc) exc = py_make_exc(PY_EXC_RUNTIME, "no active exception");
    py_retain(exc);
    py_release(g_exc);
    g_exc = exc;
    if (!g_handler) {
        py_print_exc(exc);
        exit(1);
    }
    longjmp(g_handler->buf, 1);
}

void py_raise(int exctype, const char *msg) {
    py_raise_val(py_make_exc(exctype, msg));
}

void py_reraise(void) {
    if (g_exc) py_raise_val(g_exc);
    py_raise(PY_EXC_RUNTIME, "No active exception to reraise");
}

PyVal *py_exc_current(void) { return g_exc; }
void py_exc_clear(void) {
    PyVal *e = g_exc;
    g_exc = NULL;
    py_release(e);
}

PyJmp *py_push_handler(void) {
    PyJmp *h = (PyJmp *)xmalloc(sizeof(PyJmp));
    h->prev = g_handler;
    g_handler = h;
    return h;
}
void py_pop_handler(PyJmp *h) {
    PyJmp *p;
    if (!h) return;
    if (g_handler == h) { g_handler = h->prev; free(h); return; }
    p = g_handler;
    while (p) {
        if (p->prev == h) { p->prev = h->prev; free(h); return; }
        p = p->prev;
    }
}

/* ------------------------------------------------------------------- files */
PyVal *py_open_file(const char *path, const char *mode) {
    PyVal *v;
    FILE *fp;
    int m = 0;
    if (!mode || !*mode) mode = "r";
    if (strchr(mode, 'w')) m = 1;
    else if (strchr(mode, 'a')) m = 2;
    fp = fopen(path, mode);
    if (!fp) {
        char msg[512];
        snprintf(msg, sizeof(msg), "%s: %s", path, strerror(errno));
        py_raise(PY_EXC_FILE, msg);
    }
    v = val_new(PY_T_FILE);
    v->u.file.fp = fp;
    v->u.file.mode = m;
    return v;
}

static PyVal *file_readline(PyVal *f) {
    Buf b;
    int c;
    buf_init(&b);
    if (!f->u.file.fp) py_raise(PY_EXC_VALUE, "I/O operation on closed file");
    while ((c = fgetc(f->u.file.fp)) != EOF) {
        buf_addc(&b, (char)c);
        if (c == '\n') break;
    }
    {
        PyVal *r = py_strn(b.data ? b.data : "", b.len);
        free(b.data);
        return r;
    }
}

PyVal *py_file_read(PyVal *f, PyVal **args, size_t n) {
    long sz;
    size_t want, got;
    char *buf;
    if (!f || f->type != PY_T_FILE || !f->u.file.fp)
        py_raise(PY_EXC_VALUE, "I/O operation on closed file");
    if (n > 0 && args[0]->type != PY_T_NONE) {
        want = (size_t)py_as_int(args[0]);
        buf = (char *)xmalloc(want + 1);
        got = fread(buf, 1, want, f->u.file.fp);
        buf[got] = 0;
        {
            PyVal *r = py_strn(buf, got);
            free(buf);
            return r;
        }
    }
    {
        long cur = ftell(f->u.file.fp);
        if (cur >= 0 && fseek(f->u.file.fp, 0, SEEK_END) == 0) {
            sz = ftell(f->u.file.fp);
            if (sz < cur) sz = cur;
            fseek(f->u.file.fp, cur, SEEK_SET);
            want = (size_t)(sz - cur);
            buf = (char *)xmalloc(want + 1);
            got = fread(buf, 1, want, f->u.file.fp);
            buf[got] = 0;
        } else {
            Buf b;
            int c;
            buf_init(&b);
            clearerr(f->u.file.fp);
            while ((c = fgetc(f->u.file.fp)) != EOF) buf_addc(&b, (char)c);
            got = b.len;
            buf = b.data ? b.data : (char *)xmalloc(1);
            if (!b.data) buf[0] = 0;
        }
    }
    {
        PyVal *r = py_strn(buf, got);
        free(buf);
        return r;
    }
}

PyVal *py_file_write(PyVal *f, PyVal *data) {
    PyVal *s = py_str_of(data);
    size_t n;
    if (!f || f->type != PY_T_FILE || !f->u.file.fp)
        py_raise(PY_EXC_VALUE, "I/O operation on closed file");
    n = fwrite(s->u.s.data, 1, s->u.s.len, f->u.file.fp);
    py_release(s);
    return py_int((int64_t)n);
}

void py_file_close(PyVal *f) {
    if (f && f->type == PY_T_FILE && f->u.file.fp) {
        fclose(f->u.file.fp);
        f->u.file.fp = NULL;
    }
}

static PyVal *file_method(PyVal *f, const char *name, PyVal **args, size_t argc,
                          PyVal *kwargs) {
    (void)kwargs;
    if (!strcmp(name, "read")) return py_file_read(f, args, argc);
    if (!strcmp(name, "readline")) return file_readline(f);
    if (!strcmp(name, "readlines")) {
        PyVal *r = py_list_new();
        for (;;) {
            PyVal *line = file_readline(f);
            if (line->u.s.len == 0) { py_release(line); break; }
            seq_push(r, line);
            py_release(line);
        }
        return r;
    }
    if (!strcmp(name, "write")) {
        if (argc < 1) py_raise(PY_EXC_TYPE, "write() takes one argument");
        return py_file_write(f, args[0]);
    }
    if (!strcmp(name, "writelines")) {
        PyVal *it, *item = NULL;
        if (argc < 1) py_raise(PY_EXC_TYPE, "writelines() takes one argument");
        it = py_iter(args[0]);
        while (py_iter_next(it, &item)) py_file_write(f, item);
        py_release(it);
        py_release(item);
        return py_none();
    }
    if (!strcmp(name, "close")) { py_file_close(f); return py_none(); }
    if (!strcmp(name, "flush")) { if (f->u.file.fp) fflush(f->u.file.fp); return py_none(); }
    if (!strcmp(name, "seek")) {
        if (argc > 0) fseek(f->u.file.fp, (long)py_as_int(args[0]),
                            argc > 1 ? (int)py_as_int(args[1]) : SEEK_SET);
        return py_none();
    }
    if (!strcmp(name, "tell")) return py_int(ftell(f->u.file.fp));
    if (!strcmp(name, "fileno")) return py_int(_fileno(f->u.file.fp));
    if (!strcmp(name, "__enter__")) { py_retain(f); return f; }
    if (!strcmp(name, "__exit__")) { py_file_close(f); return py_bool(0); }
    py_raise(PY_EXC_ATTRIBUTE, "file object has no such method");
    return py_none();
}

PyVal *py_enter(PyVal *mgr) {
    if (mgr && mgr->type == PY_T_FILE) { py_retain(mgr); return mgr; }
    return py_method(mgr, "__enter__", NULL, 0, NULL);
}
void py_exit(PyVal *mgr) {
    if (mgr && mgr->type == PY_T_FILE) { py_file_close(mgr); return; }
    {
        PyVal *r = py_method(mgr, "__exit__", NULL, 0, NULL);
        py_release(r);
    }
}

/* ----------------------------------------------------------------- modules */
static int streq(const char *a, const char *b) { return a && b && strcmp(a, b) == 0; }

PyVal *py_sys_argv(void) {
    if (!g_argv) return py_list_new();
    py_retain(g_argv);
    return g_argv;
}

static PyVal *itertools_call(const char *name, PyVal **args, size_t argc) {
    size_t i, j;
    if (streq(name, "chain")) {
        PyVal *r = py_list_new();
        for (i = 0; i < argc; i++) {
            PyVal *it = py_iter(args[i]), *item = NULL;
            while (py_iter_next(it, &item)) py_list_append(r, item);
            py_release(item);
            py_release(it);
        }
        return r;
    }
    if (streq(name, "repeat")) {
        PyVal *r = py_list_new();
        int64_t n = argc > 1 ? py_as_int(args[1]) : 0;
        for (i = 0; i < (size_t)(n > 0 ? n : 0); i++) py_list_append(r, args[0]);
        return r;
    }
    if (streq(name, "product")) {
        PyVal *r = py_list_new();
        PyVal **seqs;
        size_t *idx, *lens, k = argc, i, j;
        if (k == 0) { py_list_append(r, py_tuple_from(NULL, 0)); return r; }
        seqs = (PyVal **)xmalloc(k * sizeof(PyVal *));
        lens = (size_t *)xmalloc(k * sizeof(size_t));
        idx = (size_t *)xmalloc(k * sizeof(size_t));
        for (i = 0; i < k; i++) {
            seqs[i] = py_builtin_list(&args[i], 1, NULL);
            lens[i] = seqs[i]->u.seq.len;
            idx[i] = 0;
            if (lens[i] == 0) { /* empty input: no products */
                for (j = 0; j <= i; j++) py_release(seqs[j]);
                free(idx); free(lens); free(seqs);
                return r;
            }
        }
        while (1) {
            PyVal *t = py_list_new(), *tv;
            for (j = 0; j < k; j++)
                py_list_append(t, seqs[j]->u.seq.items[idx[j]]);
            tv = py_tuple_from(t->u.seq.items, t->u.seq.len);
            py_release(t);
            py_list_append(r, tv);
            py_release(tv);
            for (j = k; j > 0; j--) {
                idx[j - 1]++;
                if (idx[j - 1] < lens[j - 1]) break;
                idx[j - 1] = 0;
            }
            if (j == 0) break;
        }
        for (j = 0; j < k; j++) py_release(seqs[j]);
        free(idx); free(lens); free(seqs);
        return r;
    }
    if (streq(name, "islice")) {
        PyVal *r = py_list_new();
        int64_t start = argc > 1 ? py_as_int(args[1]) : 0;
        int64_t stop = argc > 2 ? py_as_int(args[2]) : INT64_MAX;
        int64_t step = argc > 3 ? py_as_int(args[3]) : 1;
        PyVal *it = py_iter(args[0]), *item = NULL;
        int64_t pos = 0, out = 0;
        if (step <= 0) step = 1;
        while (py_iter_next(it, &item)) {
            if (pos >= start && pos < stop && ((pos - start) % step) == 0)
                py_list_append(r, item);
            pos++;
            out++;
            if (pos >= stop) break;
            (void)out;
        }
        py_release(item);
        py_release(it);
        return r;
    }
    return NULL;
}

static PyVal *functools_call(const char *name, PyVal **args, size_t argc) {
    if (streq(name, "reduce") && argc >= 2) {
        PyVal *it, *item = NULL, *acc;
        int have = 0;
        if (argc >= 3) {
            acc = args[2]; py_retain(acc); have = 1;
        }
        it = py_iter(args[1]);
        while (py_iter_next(it, &item)) {
            if (!have) { acc = item; item = NULL; have = 1; }
            else {
                PyVal *av[2];
                PyVal *n;
                av[0] = acc; av[1] = item;
                n = py_call(args[0], av, 2, NULL);
                py_release(acc);
                acc = n;
            }
            py_release(item);
            item = NULL;
        }
        py_release(item);
        py_release(it);
        if (!have) py_raise(PY_EXC_VALUE, "reduce() of empty sequence");
        return acc;
    }
    return NULL;
}

int py_module_known(const char *module) {
    return streq(module, "math") || streq(module, "random") || streq(module, "time") ||
           streq(module, "sys") || streq(module, "os") || streq(module, "os.path") ||
           streq(module, "tkinter") || streq(module, "tkinter.ttk") ||
           streq(module, "tkinter.messagebox") || streq(module, "tkinter.constants") ||
           streq(module, "string") || streq(module, "platform") ||
           streq(module, "functools") || streq(module, "itertools") ||
           py_stdlib_module_known(module);
}

PyVal *py_module_get(const char *module, const char *name) {
    if (streq(module, "math")) {
        if (streq(name, "pi")) return py_float(3.14159265358979323846);
        if (streq(name, "e")) return py_float(2.71828182845904523536);
        if (streq(name, "tau")) return py_float(6.28318530717958647692);
        if (streq(name, "inf")) return py_float(INFINITY);
        if (streq(name, "nan")) return py_float(NAN);
    }
    if (streq(module, "sys")) {
        if (streq(name, "argv")) return py_sys_argv();
        if (streq(name, "platform")) return py_str("win32");
        if (streq(name, "version")) return py_str("3.11 (py2c99)");
        if (streq(name, "maxsize")) return py_int(INT64_MAX);
        if (streq(name, "executable")) return py_str("py2c99.exe");
    }
    if (streq(module, "os")) {
        if (streq(name, "sep")) return py_str("\\");
        if (streq(name, "linesep")) return py_str("\r\n");
        if (streq(name, "name")) return py_str("nt");
    }
    if (streq(module, "string")) {
        if (streq(name, "ascii_lowercase")) return py_str("abcdefghijklmnopqrstuvwxyz");
        if (streq(name, "ascii_uppercase")) return py_str("ABCDEFGHIJKLMNOPQRSTUVWXYZ");
        if (streq(name, "ascii_letters")) return py_str("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ");
        if (streq(name, "digits")) return py_str("0123456789");
        if (streq(name, "hexdigits")) return py_str("0123456789abcdefABCDEF");
        if (streq(name, "whitespace")) return py_str(" \t\n\r\v\f");
        if (streq(name, "punctuation")) return py_str("!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~");
    }
    if (streq(module, "platform") && streq(name, "system")) return py_str("Windows");
    if (streq(module, "tkinter") || streq(module, "tkinter.constants") ||
        streq(module, "tkinter.ttk")) {
        if (streq(name, "LEFT")) return py_str("left");
        if (streq(name, "RIGHT")) return py_str("right");
        if (streq(name, "TOP")) return py_str("top");
        if (streq(name, "BOTTOM")) return py_str("bottom");
        if (streq(name, "BOTH")) return py_str("both");
        if (streq(name, "X")) return py_str("x");
        if (streq(name, "Y")) return py_str("y");
        if (streq(name, "NONE")) return py_str("none");
        if (streq(name, "N")) return py_str("n");
        if (streq(name, "S")) return py_str("s");
        if (streq(name, "E")) return py_str("e");
        if (streq(name, "W")) return py_str("w");
        if (streq(name, "NW")) return py_str("nw");
        if (streq(name, "NE")) return py_str("ne");
        if (streq(name, "SW")) return py_str("sw");
        if (streq(name, "SE")) return py_str("se");
        if (streq(name, "NSEW")) return py_str("nsew");
        if (streq(name, "END")) return py_str("end");
        if (streq(name, "CENTER")) return py_str("center");
        if (streq(name, "HORIZONTAL")) return py_str("horizontal");
        if (streq(name, "VERTICAL")) return py_str("vertical");
        if (streq(name, "DISABLED")) return py_str("disabled");
        if (streq(name, "NORMAL")) return py_str("normal");
        if (streq(name, "ACTIVE")) return py_str("active");
    }
    {
        PyVal *extra = py_stdlib_const(module, name);
        if (extra) return extra;
    }
    return NULL;
}

PyVal *py_module_attr(const char *module, const char *name) {
    PyVal *v = py_module_get(module, name);
    PyVal *m;
    char *md, *nd;
    if (v) return v;
    m = val_new(PY_T_MODFUNC);
    {
        const char *ms = module ? module : "";
        const char *ns = name ? name : "";
        size_t mlen = strlen(ms) + 1;
        size_t nlen = strlen(ns) + 1;
        md = (char *)xmalloc(mlen);
        nd = (char *)xmalloc(nlen);
        memcpy(md, ms, mlen);
        memcpy(nd, ns, nlen);
    }
    m->u.modfunc.module = md;
    m->u.modfunc.name = nd;
    return m;
}

static uint64_t g_rng = 88172645463325252ULL;
static uint64_t rng_next(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}
static double rng_double(void) { return (double)(rng_next() >> 11) / 9007199254740992.0; }

static PyVal *math_call(const char *name, PyVal **args, size_t argc) {
    double a = argc > 0 ? py_as_float(args[0]) : 0.0;
    double b = argc > 1 ? py_as_float(args[1]) : 0.0;
    if (streq(name, "sqrt")) return py_float(sqrt(a));
    if (streq(name, "sin")) return py_float(sin(a));
    if (streq(name, "cos")) return py_float(cos(a));
    if (streq(name, "tan")) return py_float(tan(a));
    if (streq(name, "asin")) return py_float(asin(a));
    if (streq(name, "acos")) return py_float(acos(a));
    if (streq(name, "atan")) return py_float(atan(a));
    if (streq(name, "atan2")) return py_float(atan2(a, b));
    if (streq(name, "sinh")) return py_float(sinh(a));
    if (streq(name, "cosh")) return py_float(cosh(a));
    if (streq(name, "tanh")) return py_float(tanh(a));
    if (streq(name, "exp")) return py_float(exp(a));
    if (streq(name, "log")) return py_float(argc > 1 ? log(a) / log(b) : log(a));
    if (streq(name, "log10")) return py_float(log10(a));
    if (streq(name, "log2")) return py_float(log2(a));
    if (streq(name, "pow")) return py_float(pow(a, b));
    if (streq(name, "fabs")) return py_float(fabs(a));
    if (streq(name, "floor")) return py_int((int64_t)floor(a));
    if (streq(name, "ceil")) return py_int((int64_t)ceil(a));
    if (streq(name, "trunc")) return py_int((int64_t)trunc(a));
    if (streq(name, "fmod")) return py_float(fmod(a, b));
    if (streq(name, "hypot")) return py_float(hypot(a, b));
    if (streq(name, "degrees")) return py_float(a * 180.0 / M_PI);
    if (streq(name, "radians")) return py_float(a * M_PI / 180.0);
    if (streq(name, "isfinite")) return py_bool(isfinite(a) != 0);
    if (streq(name, "isnan")) return py_bool(isnan(a) != 0);
    if (streq(name, "isinf")) return py_bool(isinf(a) != 0);
    if (streq(name, "factorial")) {
        int64_t n = py_as_int(args[0]), r = 1;
        while (n > 1) r *= n--;
        return py_int(r);
    }
    if (streq(name, "gcd")) {
        int64_t x = py_as_int(args[0]), y = py_as_int(args[1]);
        if (x < 0) x = -x;
        if (y < 0) y = -y;
        while (y) { int64_t t = x % y; x = y; y = t; }
        return py_int(x);
    }
    if (streq(name, "copysign")) return py_float(copysign(a, b));
    if (streq(name, "remainder")) return py_float(remainder(a, b));
    return NULL;
}

static PyVal *random_call(const char *name, PyVal **args, size_t argc) {
    if (streq(name, "random")) return py_float(rng_double());
    if (streq(name, "uniform")) {
        double a = py_as_float(args[0]), b = py_as_float(args[1]);
        return py_float(a + (b - a) * rng_double());
    }
    if (streq(name, "randint")) {
        int64_t a = py_as_int(args[0]), b = py_as_int(args[1]);
        if (b < a) { int64_t t = a; a = b; b = t; }
        return py_int(a + (int64_t)(rng_next() % (uint64_t)(b - a + 1)));
    }
    if (streq(name, "randrange")) {
        int64_t a, b, step = 1;
        if (argc == 1) { a = 0; b = py_as_int(args[0]); }
        else { a = py_as_int(args[0]); b = py_as_int(args[1]); if (argc > 2) step = py_as_int(args[2]); }
        if (step == 0) py_raise(PY_EXC_VALUE, "zero step for randrange()");
        {
            int64_t n = (b - a + step - 1) / step;
            if (n <= 0) py_raise(PY_EXC_VALUE, "empty range for randrange()");
            return py_int(a + (int64_t)(rng_next() % (uint64_t)n) * step);
        }
    }
    if (streq(name, "choice")) {
        PyVal *seq = args[0];
        size_t n = py_len(seq);
        if (n == 0) py_raise(PY_EXC_INDEX, "cannot choose from an empty sequence");
        {
            PyVal *idx = py_int((int64_t)(rng_next() % n));
            PyVal *r = py_getitem(seq, idx);
            py_release(idx);
            return r;
        }
    }
    if (streq(name, "shuffle")) {
        PyVal *l = args[0];
        size_t i;
        if (l->type != PY_T_LIST) py_raise(PY_EXC_TYPE, "shuffle() requires a list");
        for (i = l->u.seq.len; i > 1; i--) {
            size_t j = (size_t)(rng_next() % i);
            PyVal *t = l->u.seq.items[i - 1];
            l->u.seq.items[i - 1] = l->u.seq.items[j];
            l->u.seq.items[j] = t;
        }
        return py_none();
    }
    if (streq(name, "seed")) {
        if (argc > 0) g_rng = (uint64_t)py_as_int(args[0]) ^ 88172645463325252ULL;
        else g_rng = (uint64_t)time(NULL) ^ 88172645463325252ULL;
        if (!g_rng) g_rng = 1;
        return py_none();
    }
    return NULL;
}

static PyVal *time_call(const char *name, PyVal **args, size_t argc) {
    if (streq(name, "time")) {
        return py_float((double)clock() / (double)CLOCKS_PER_SEC);
    }
    if (streq(name, "perf_counter") || streq(name, "monotonic")) {
        return py_float((double)clock() / (double)CLOCKS_PER_SEC);
    }
    if (streq(name, "sleep")) {
        double secs = py_as_float(args[0]);
#ifdef _WIN32
        Sleep((DWORD)(secs * 1000.0));
#else
        {
            struct timespec ts;
            ts.tv_sec = (time_t)secs;
            ts.tv_nsec = (long)((secs - (double)ts.tv_sec) * 1e9);
            nanosleep(&ts, NULL);
        }
#endif
        return py_none();
    }
    if (streq(name, "strftime")) {
        char buf[512];
        time_t t = time(NULL);
        struct tm *tmv = localtime(&t);
        const char *fmt = argc > 0 ? py_cstr(args[0]) : "%Y-%m-%d %H:%M:%S";
        (void)argc;
        if (!strftime(buf, sizeof(buf), fmt, tmv)) buf[0] = 0;
        return py_str(buf);
    }
    if (streq(name, "ctime")) {
        char buf[64];
        time_t t = time(NULL);
        snprintf(buf, sizeof(buf), "%s", ctime(&t));
        {
            size_t n = strlen(buf);
            while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
            return py_strn(buf, n);
        }
    }
    return NULL;
}

static PyVal *os_call(const char *name, PyVal **args, size_t argc) {
    if (streq(name, "getcwd")) {
        char buf[4096];
#ifdef _WIN32
        if (_getcwd(buf, sizeof(buf))) return py_str(buf);
#else
        if (getcwd(buf, sizeof(buf))) return py_str(buf);
#endif
        return py_str(".");
    }
    if (streq(name, "chdir")) {
#ifdef _WIN32
        _chdir(py_cstr(args[0]));
#else
        chdir(py_cstr(args[0]));
#endif
        return py_none();
    }
    if (streq(name, "mkdir")) {
#ifdef _WIN32
        _mkdir(py_cstr(args[0]));
#else
        mkdir(py_cstr(args[0]), 0777);
#endif
        return py_none();
    }
    if (streq(name, "remove") || streq(name, "unlink")) { remove(py_cstr(args[0])); return py_none(); }
    if (streq(name, "rename")) { rename(py_cstr(args[0]), py_cstr(args[1])); return py_none(); }
    if (streq(name, "getenv")) {
        const char *v = getenv(py_cstr(args[0]));
        return v ? py_str(v) : py_none();
    }
    if (streq(name, "system")) {
        return py_int(system(py_cstr(args[0])));
    }
    if (streq(name, "listdir")) {
        PyVal *r = py_list_new();
        const char *path = argc > 0 ? py_cstr(args[0]) : ".";
#ifdef _WIN32
        char pattern[4096];
        WIN32_FIND_DATAA fd;
        HANDLE h;
        snprintf(pattern, sizeof(pattern), "%s\\*", path);
        h = FindFirstFileA(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, ".."))
                    seq_push(r, py_str(fd.cFileName));
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
#else
        DIR *d = opendir(path);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
                    seq_push(r, py_str(e->d_name));
            }
            closedir(d);
        }
#endif
        return r;
    }
    if (streq(name, "join") || streq(name, "path_join")) {
        Buf b;
        size_t i;
        buf_init(&b);
        for (i = 0; i < argc; i++) {
            const char *p = py_cstr(args[i]);
            if (i && b.len && b.data[b.len - 1] != '\\' && b.data[b.len - 1] != '/')
                buf_addc(&b, '\\');
            buf_adds(&b, p);
        }
        {
            PyVal *r = py_strn(b.data ? b.data : "", b.len);
            free(b.data);
            return r;
        }
    }
    if (streq(name, "exists") || streq(name, "isfile") || streq(name, "isdir")) {
        const char *p = py_cstr(args[0]);
        struct stat st;
        if (stat(p, &st) != 0) return py_bool(0);
        if (streq(name, "isfile")) return py_bool((st.st_mode & S_IFMT) == S_IFREG);
        if (streq(name, "isdir")) return py_bool((st.st_mode & S_IFMT) == S_IFDIR);
        return py_bool(1);
    }
    if (streq(name, "basename") || streq(name, "path_basename")) {
        const char *p = py_cstr(args[0]);
        const char *s = strrchr(p, '\\');
        const char *s2 = strrchr(p, '/');
        if (s2 && (!s || s2 > s)) s = s2;
        return py_str(s ? s + 1 : p);
    }
    if (streq(name, "dirname") || streq(name, "path_dirname")) {
        const char *p = py_cstr(args[0]);
        const char *s = strrchr(p, '\\');
        const char *s2 = strrchr(p, '/');
        if (s2 && (!s || s2 > s)) s = s2;
        if (!s) return py_str(".");
        return py_strn(p, (size_t)(s - p));
    }
    if (streq(name, "splitext") || streq(name, "path_splitext")) {
        const char *p = py_cstr(args[0]);
        const char *dot = strrchr(p, '.');
        const char *slash = strrchr(p, '\\');
        PyVal *arr[2], *r;
        if (!dot || (slash && dot < slash)) dot = p + strlen(p);
        arr[0] = py_strn(p, (size_t)(dot - p));
        arr[1] = py_str(dot);
        r = py_tuple_from(arr, 2);
        py_release(arr[0]);
        py_release(arr[1]);
        return r;
    }
    return NULL;
}

PyVal *py_module_call(const char *module, const char *name, PyVal **args,
                      size_t argc, PyVal *kwargs) {
    PyVal *r = NULL;
    (void)kwargs;
    if (streq(module, "math")) {
        r = math_call(name, args, argc);
        if (!r) py_raise(PY_EXC_ATTRIBUTE, "math function not supported");
        return r;
    }
    if (streq(module, "random")) {
        r = random_call(name, args, argc);
        if (!r) py_raise(PY_EXC_ATTRIBUTE, "random function not supported");
        return r;
    }
    if (streq(module, "time")) {
        r = time_call(name, args, argc);
        if (!r) py_raise(PY_EXC_ATTRIBUTE, "time function not supported");
        return r;
    }
    if (streq(module, "os") || streq(module, "os.path")) {
        r = os_call(name, args, argc);
        if (!r) py_raise(PY_EXC_ATTRIBUTE, "os function not supported");
        return r;
    }
    if (streq(module, "sys")) {
        if (streq(name, "exit")) {
            int code = argc > 0 ? (int)py_as_int(args[0]) : 0;
            py_gui_shutdown();
            exit(code);
        }
        if (streq(name, "getdefaultencoding")) return py_str("utf-8");
        if (streq(name, "getrecursionlimit")) return py_int(1000);
        if (streq(name, "setrecursionlimit")) return py_none();
        if (streq(name, "stdout") || streq(name, "stderr")) return py_none();
    }
    if (streq(module, "platform")) {
        if (streq(name, "system")) return py_str("Windows");
        if (streq(name, "python_version")) return py_str("3.11");
        if (streq(name, "node")) return py_str("py2c99");
    }
    if (streq(module, "itertools")) {
        r = itertools_call(name, args, argc);
        if (!r) py_raise(PY_EXC_ATTRIBUTE, "itertools function not supported");
        return r;
    }
    if (streq(module, "functools")) {
        r = functools_call(name, args, argc);
        if (!r) py_raise(PY_EXC_ATTRIBUTE, "functools function not supported");
        return r;
    }
    {
        int handled = 0;
        PyVal *extra = py_stdlib_call(module, name, args, argc, kwargs, &handled);
        if (handled) return extra;
    }
    py_raise(PY_EXC_IMPORT, "module function not available in pure C mode");
    return py_none();
}

PyVal *py_math_sqrt(PyVal *x) { return py_float(sqrt(py_as_float(x))); }

/* --------------------------------------------------------- generated helpers */
PyVal *py_load(PyVal **slot, const char *name) {
    PyVal *v;
    if (!slot || !*slot) {
        char msg[256];
        snprintf(msg, sizeof(msg), "name '%s' is not defined", name ? name : "");
        py_raise(PY_EXC_NAME, msg);
    }
    v = *slot;
    py_retain(v);
    return v;
}

PyVal *py_env_get(PyVal *env, const char *name) {
    PyVal *v = py_dict_get(env, name);
    if (!v) {
        char msg[256];
        snprintf(msg, sizeof(msg), "name '%s' is not defined", name ? name : "");
        py_raise(PY_EXC_NAME, msg);
    }
    py_retain(v);
    return v;
}

PyVal *py_call_list(PyVal *fn, PyVal *poslist, PyVal *kwargs) {
    if (!poslist) return py_call(fn, NULL, 0, kwargs);
    if (poslist->type != PY_T_LIST && poslist->type != PY_T_TUPLE)
        py_raise(PY_EXC_TYPE, "argument list is not a sequence");
    return py_call(fn, poslist->u.seq.items, poslist->u.seq.len, kwargs);
}

PyVal *py_format_value(PyVal *value, const char *spec, int conv) {
    PyVal *v = NULL, *s, *r, *args[2];
    if (!value) value = py_none();
    if (conv == 1) v = py_repr(value);
    else if (conv == 2) v = py_str_of(value);
    else { v = value; py_retain(v); }
    if (!spec || !*spec) {
        if (conv == 0 && value->type != PY_T_STR) {
            PyVal *t = py_str_of(v);
            py_release(v);
            return t;
        }
        return v;
    }
    s = py_str(spec ? spec : "");
    args[0] = v;
    args[1] = s;
    r = py_builtin_format(args, 2, NULL);
    py_release(s);
    py_release(v);
    return r;
}

PyVal *py_set_from(PyVal **items, size_t n) {
    PyVal *s = py_set_new();
    size_t i;
    for (i = 0; i < n; i++) py_set_add(s, items[i]);
    return s;
}

void py_dict_update(PyVal *dst, PyVal *src) {
    size_t i;
    if (!dst || dst->type != PY_T_DICT || !src || src->type != PY_T_DICT) return;
    for (i = 0; i < src->u.map.len; i++)
        map_put(dst, src->u.map.keys[i], src->u.map.vals[i]);
}

PyVal *py_builtin_lookup(const char *name) {
    static const struct { const char *name; PyCFunc fn; } table[] = {
        {"print", py_print}, {"input", py_input}, {"len", py_builtin_len},
        {"int", py_builtin_int}, {"float", py_builtin_float}, {"str", py_builtin_str},
        {"bool", py_builtin_bool}, {"list", py_builtin_list}, {"tuple", py_builtin_tuple},
        {"dict", py_builtin_dict}, {"set", py_builtin_set}, {"range", py_builtin_range},
        {"abs", py_builtin_abs}, {"min", py_builtin_min}, {"max", py_builtin_max},
        {"sum", py_builtin_sum}, {"sorted", py_builtin_sorted},
        {"reversed", py_builtin_reversed}, {"enumerate", py_builtin_enumerate},
        {"zip", py_builtin_zip}, {"any", py_builtin_any}, {"all", py_builtin_all},
        {"chr", py_builtin_chr}, {"ord", py_builtin_ord}, {"round", py_builtin_round},
        {"pow", py_builtin_pow}, {"divmod", py_builtin_divmod}, {"hex", py_builtin_hex},
        {"oct", py_builtin_oct}, {"bin", py_builtin_bin}, {"repr", py_builtin_repr},
        {"format", py_builtin_format}, {"id", py_builtin_id}, {"hash", py_builtin_hash},
        {"isinstance", py_builtin_isinstance}, {"type", py_builtin_type},
        {"open", py_builtin_open}, {"iter", py_builtin_iter}, {"next", py_builtin_next},
        {"map", py_builtin_map}, {"filter", py_builtin_filter},
        {"getattr", py_builtin_getattr}, {"setattr", py_builtin_setattr},
        {"hasattr", py_builtin_hasattr}, {"callable", py_builtin_callable},
        {"slice", py_builtin_slice}, {"frozenset", py_builtin_frozenset},
        {NULL, NULL}
    };
    size_t i;
    for (i = 0; table[i].name; i++)
        if (!strcmp(table[i].name, name)) return py_func(table[i].fn, table[i].name);
    return NULL;
}

/* ------------------------------------------------------------ utf helpers */char *py_utf8_from_utf16(const wchar_t *w) {
    if (!w) return NULL;
#ifdef _WIN32
    {
        int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
        char *s = (char *)xmalloc((size_t)(n > 0 ? n : 1));
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
        else s[0] = 0;
        return s;
    }
#else
    {
        size_t n = wcstombs(NULL, w, 0);
        char *s = (char *)xmalloc(n + 1);
        wcstombs(s, w, n + 1);
        return s;
    }
#endif
}

wchar_t *py_utf16_from_utf8(const char *s) {
    if (!s) return NULL;
#ifdef _WIN32
    {
        int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
        wchar_t *w = (wchar_t *)xmalloc((size_t)(n > 0 ? n : 1) * sizeof(wchar_t));
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
        else w[0] = 0;
        return w;
    }
#else
    {
        size_t n = mbstowcs(NULL, s, 0);
        wchar_t *w = (wchar_t *)xmalloc((n + 1) * sizeof(wchar_t));
        mbstowcs(w, s, n + 1);
        return w;
    }
#endif
}

/* ---------------------------------------------------------------- lifecycle */
void py_init(int argc, char **argv_utf8) {
    int i;
    if (g_initialized) return;
    memset(&g_none, 0, sizeof(g_none));
    memset(&g_true, 0, sizeof(g_true));
    memset(&g_false, 0, sizeof(g_false));
    g_none.type = PY_T_NONE; g_none.refs = 1000000;
    g_true.type = PY_T_BOOL; g_true.refs = 1000000; g_true.u.b = 1;
    g_false.type = PY_T_BOOL; g_false.refs = 1000000; g_false.u.b = 0;
    g_argv = py_list_new();
    for (i = 0; i < argc; i++) {
        PyVal *s = py_str(argv_utf8[i]);
        seq_push(g_argv, s);
        py_release(s);
    }
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    g_rng = ((uint64_t)time(NULL) << 16) ^ (uint64_t)(uintptr_t)&g_rng;
    if (!g_rng) g_rng = 1;
    setvbuf(stdout, NULL, _IONBF, 0);
    py_gui_init();
    g_initialized = 1;
}

void py_shutdown(void) {
    py_gui_shutdown();
    py_exc_clear();
    py_release(g_argv);
    g_argv = NULL;
}
/*@@END@@*/
