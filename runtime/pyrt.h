/*
 * py2c99 runtime - a small, pure C99 Python object model.
 *
 * Everything is reference counted.  A PyVal is never NULL when it has been
 * returned by the public API: the singleton None/True/False values are real
 * objects.  NULL is only used internally as "no value" / "end of iterator".
 */
#ifndef PY2C99_PYRT_H
#define PY2C99_PYRT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PyVal PyVal;
typedef struct PyIter PyIter;
typedef struct PyWidget PyWidget;

typedef PyVal *(*PyCFunc)(PyVal **args, size_t argc, PyVal *kwargs);

typedef enum {
    PY_T_NONE = 0,
    PY_T_BOOL,
    PY_T_INT,
    PY_T_FLOAT,
    PY_T_STR,
    PY_T_LIST,
    PY_T_TUPLE,
    PY_T_DICT,
    PY_T_SET,
    PY_T_FUNC,
    PY_T_BOUND,
    PY_T_RANGE,
    PY_T_ITER,
    PY_T_WIDGET,
    PY_T_FILE,
    PY_T_EXC,
    PY_T_VAR,
    PY_T_CLASS,
    PY_T_MODFUNC,
    PY_T_GUIMETHOD,
    PY_T_DICTVIEW,
    PY_T_SLICE
} PyType;

/* Widget kinds understood by py_method / py_tk_widget (pyrt_win.c). */
enum {
    PY_W_ROOT = 0,
    PY_W_FRAME,
    PY_W_LABEL,
    PY_W_BUTTON,
    PY_W_ENTRY,
    PY_W_TEXT,
    PY_W_CANVAS,
    PY_W_LISTBOX,
    PY_W_CHECKBUTTON,
    PY_W_RADIOBUTTON,
    PY_W_SCALE,
    PY_W_MENU,
    PY_W_MENUBUTTON,
    PY_W_SPINBOX,
    PY_W_PROGRESSBAR,
    PY_W_SEPARATOR,
    PY_W_TREEVIEW,
    PY_W_NOTEBOOK
};

/* Binary operators. */
enum {
    PY_OP_ADD = 0,
    PY_OP_SUB,
    PY_OP_MUL,
    PY_OP_DIV,
    PY_OP_FLOORDIV,
    PY_OP_MOD,
    PY_OP_POW,
    PY_OP_LSHIFT,
    PY_OP_RSHIFT,
    PY_OP_BITAND,
    PY_OP_BITOR,
    PY_OP_BITXOR,
    PY_OP_MATMUL
};

/* Comparison operators. */
enum {
    PY_CMP_EQ = 0,
    PY_CMP_NE,
    PY_CMP_LT,
    PY_CMP_LE,
    PY_CMP_GT,
    PY_CMP_GE,
    PY_CMP_IN,
    PY_CMP_NOTIN,
    PY_CMP_IS,
    PY_CMP_ISNOT
};

/* Exception kinds (names deliberately mirror Python). */
enum {
    PY_EXC_BASE = 0,
    PY_EXC_EXCEPTION,
    PY_EXC_ARITHMETIC,
    PY_EXC_ASSERTION,
    PY_EXC_ATTRIBUTE,
    PY_EXC_INDEX,
    PY_EXC_KEY,
    PY_EXC_NAME,
    PY_EXC_OS,
    PY_EXC_RUNTIME,
    PY_EXC_STOP_ITERATION,
    PY_EXC_TYPE,
    PY_EXC_VALUE,
    PY_EXC_ZERO_DIVISION,
    PY_EXC_IMPORT,
    PY_EXC_UNSUPPORTED,
    PY_EXC_EOF,
    PY_EXC_OVERFLOW,
    PY_EXC_MEMORY,
    PY_EXC_FILE
};

typedef struct PyVal {
    PyType type;
    int refs;
    union {
        int b;
        int64_t i;
        double f;
        struct {
            char *data;
            size_t len;
        } s;
        struct {
            PyVal **items;
            size_t len, cap;
        } seq; /* list, tuple */
        struct {
            PyVal **keys;
            PyVal **vals;
            size_t len, cap;
        } map; /* dict, set (vals are PY_TRUE) */
        struct {
            PyVal *fn;
            PyVal *self;
        } bound;
        struct {
            PyCFunc fn;
            PyVal *ctx;
            const char *name;
        } func;
        struct {
            int64_t start, stop, step, idx;
        } range;
        struct {
            PyIter *it;
        } iter;
        struct {
            PyWidget *w;
        } widget;
        struct {
            FILE *fp;
            int mode; /* 0 = read, 1 = write, 2 = append */
        } file;
        struct {
            int kind;
            char *msg;
        } exc;
        struct {
            PyVal *value;
            PyVal *name;
        } var;
        struct {
            PyVal *dict;
            PyVal *name;
        } klass;
        struct {
            char *module;
            char *name;
        } modfunc;
        struct {
            PyWidget *w;
            char *name;
        } gm;
        struct {
            PyVal *dict;
            int kind; /* 0 = keys, 1 = values, 2 = items */
        } view;
        struct {
            PyVal *start, *stop, *step;
        } slice;
    } u;
} PyVal;

struct PyIter {
    PyVal *src;   /* retained */
    PyVal *extra; /* retained (zip/enumerate state) */
    size_t idx;
    int done;
    PyVal *(*next)(PyIter *it); /* new reference, or NULL when exhausted */
};

typedef struct PyJmp {
    jmp_buf buf;
    struct PyJmp *prev;
} PyJmp;

/* ---------------------------------------------------------------- lifetime */
void py_init(int argc, char **argv_utf8);
void py_shutdown(void);
void py_retain_impl(PyVal *v);
void py_release_impl(PyVal *v);

/* Keep these as functions: a macro would evaluate an expression argument
   twice, which silently duplicates side effects such as py_method(...). */
static inline void py_retain(PyVal *v) {
    if (v) v->refs++;
}
static inline void py_release(PyVal *v) {
    if (v && --v->refs == 0) py_release_impl(v);
}

PyVal *py_none(void);
PyVal *py_bool(int b);
PyVal *py_int(int64_t i);
PyVal *py_float(double f);
PyVal *py_str(const char *s);
PyVal *py_strn(const char *s, size_t n);

/* Constructors.  All of these retain the values they are given. */
PyVal *py_list_new(void);
void py_list_append(PyVal *list, PyVal *item);
PyVal *py_set_from(PyVal **items, size_t n);
void py_dict_update(PyVal *dst, PyVal *src);
PyVal *py_list_from(PyVal **items, size_t n);
PyVal *py_tuple_from(PyVal **items, size_t n);
PyVal *py_set_new(void);
PyVal *py_dict_new(void);
PyVal *py_dict_view(PyVal *dict, int kind);  /* 0 keys, 1 values, 2 items */
PyVal *py_slice_new(PyVal *start, PyVal *stop, PyVal *step);
PyVal *py_str_join(PyVal **parts, size_t n);       /* build a string in one pass */
PyVal *py_dict_lit(const char **keys, PyVal **vals, size_t n);
PyVal *py_range(int64_t start, int64_t stop, int64_t step);
PyVal *py_func(PyCFunc fn, const char *name);
PyVal *py_func_ctx(PyCFunc fn, const char *name, PyVal *ctx);
PyVal *py_bound(PyVal *fn, PyVal *self);
PyVal *py_var(PyVal *value, const char *name);
PyVal *py_class(PyVal *dict, const char *name);
void py_class_inherit(PyVal *dict, PyVal *base);
PyVal *py_widget_value(PyWidget *w);
PyVal *py_guimethod(PyWidget *w, const char *name);

/* ------------------------------------------------------------- conversion */
int py_truth(PyVal *v);
int64_t py_as_int(PyVal *v);
double py_as_float(PyVal *v);
const char *py_cstr(PyVal *v); /* borrowed UTF-8 view */
size_t py_cstr_len(PyVal *v);
PyVal *py_str_of(PyVal *v);   /* str(v) */
PyVal *py_repr(PyVal *v);     /* repr(v) */
PyVal *py_type(PyVal *v);     /* type object (a string-like descriptor) */

/* Assignment helper: retains new value, releases old one. */
void py_assign(PyVal **slot, PyVal *value);

/* Context-manager helpers used by generated `with` statements. */
PyVal *py_enter(PyVal *mgr);
void py_exit(PyVal *mgr);

/* --------------------------------------------------------------- containers */
size_t py_len(PyVal *v);
PyVal *py_getitem(PyVal *obj, PyVal *key);
void py_setitem(PyVal *obj, PyVal *key, PyVal *value);
int py_delitem(PyVal *obj, PyVal *key);
int py_contains(PyVal *obj, PyVal *key);
PyVal *py_getslice(PyVal *obj, PyVal *lo, PyVal *hi, PyVal *step);
PyVal *py_getattr(PyVal *obj, const char *name);
void py_setattr(PyVal *obj, const char *name, PyVal *value);
int py_delattr(PyVal *obj, const char *name);
int py_unpack(PyVal *obj, PyVal ***slots, size_t n);

/* dict helpers (used by generated keyword-argument code) */
void py_dict_put(PyVal *d, const char *key, PyVal *value);
PyVal *py_dict_get(PyVal *d, const char *key);
void py_dict_strkey(PyVal *d, PyVal *key, PyVal *value);
PyVal *py_kw_get(PyVal *kwargs, const char *name);
void py_set_add(PyVal *s, PyVal *value);
int py_set_has(PyVal *s, PyVal *value);
int py_set_discard(PyVal *s, PyVal *value);

/* ----------------------------------------------------------------- dynamics */
PyVal *py_binop(int op, PyVal *a, PyVal *b);
PyVal *py_unaryop(int op, PyVal *a); /* op: 0=neg 1=pos 2=invert 3=not */
int py_cmp(int op, PyVal *a, PyVal *b);
PyVal *py_call(PyVal *fn, PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_method(PyVal *obj, const char *name, PyVal **args, size_t argc,
                 PyVal *kwargs);
PyVal *py_call_attr(PyVal *obj, const char *name, PyVal **args, size_t argc,
                    PyVal *kwargs);

/* builtins that are convenient to have as real C entry points */
PyVal *py_print(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_input(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_len(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_int(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_float(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_str(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_bool(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_list(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_tuple(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_dict(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_set(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_range(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_abs(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_min(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_max(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_sum(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_sorted(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_reversed(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_enumerate(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_zip(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_any(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_all(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_chr(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_ord(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_round(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_pow(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_divmod(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_hex(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_oct(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_bin(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_repr(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_format(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_id(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_hash(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_isinstance(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_type(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_open(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_iter(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_next(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_map(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_filter(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_getattr(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_setattr(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_hasattr(PyVal **args, size_t argc, PyVal *kwargs);
PyVal *py_builtin_callable(PyVal **args, size_t argc, PyVal *kwargs);

/* --------------------------------------------------------------- iteration */
PyVal *py_iter(PyVal *obj);
PyVal *py_iter_next(PyVal *it, PyVal **out); /* returns *out or NULL */
PyVal *py_iter_from_array(PyVal **items, size_t n); /* list iterator */

/* -------------------------------------------------------------- exceptions */
PyJmp *py_push_handler(void);
void py_pop_handler(PyJmp *h);
void py_raise(int exctype, const char *msg);
void py_raise_val(PyVal *exc);
void py_reraise(void);
PyVal *py_exc_current(void);
void py_exc_clear(void);
int py_exc_matches(PyVal *exc, int exctype);
int py_exc_matches_name(PyVal *exc, const char *name);
const char *py_exc_name(int exctype);
PyVal *py_make_exc(int exctype, const char *msg);
PyVal *py_make_exc_val(int exctype, PyVal *msg);

/* ------------------------------------------------------- calling conventions */
PyVal *py_args_tail(PyVal **args, size_t argc, size_t nfixed);
PyVal *py_kwargs_tail(PyVal *kwargs, const char **names, size_t n);
void py_bind_args(PyVal **args, size_t argc, PyVal *kwargs,
                  const char **names, PyVal **defaults, size_t nparams,
                  PyVal ***slots);
void py_bind_args_ex(PyVal **args, size_t argc, PyVal *kwargs,
                     const char **names, PyVal **defaults, size_t nparams,
                     PyVal ***slots, int allow_extra);

/* ------------------------------------------------------------------- files */
PyVal *py_open_file(const char *path, const char *mode);
PyVal *py_file_read(PyVal *f, PyVal **args, size_t n);
PyVal *py_file_write(PyVal *f, PyVal *data);
void py_file_close(PyVal *f);

/* --------------------------------------------------------------- modules */
PyVal *py_math_sqrt(PyVal *x);PyVal *py_module_call(const char *module, const char *name, PyVal **args,
                      size_t argc, PyVal *kwargs);
PyVal *py_module_get(const char *module, const char *name); /* NULL if absent */
PyVal *py_module_attr(const char *module, const char *name);
int py_module_known(const char *module);
PyVal *py_sys_argv(void);

/* Additional pure-C standard library shims (runtime/pyrt_stdlib.c). */
int py_stdlib_module_known(const char *module);
PyVal *py_stdlib_const(const char *module, const char *name);
PyVal *py_stdlib_call(const char *module, const char *name, PyVal **args,
                      size_t argc, PyVal *kwargs, int *handled);

/* --------------------------------------------------------- generated helpers */
PyVal *py_load(PyVal **slot, const char *name); /* retained or NameError */
PyVal *py_env_get(PyVal *env, const char *name); /* retained or NameError */
PyVal *py_builtin_lookup(const char *name);     /* retained function or NULL */
PyVal *py_call_list(PyVal *fn, PyVal *poslist, PyVal *kwargs);
PyVal *py_format_value(PyVal *value, const char *spec, int conv);

/* -------------------------------------------------------------------- misc */
void py_print_exc(PyVal *exc);
int py_is_none(PyVal *v);
size_t py_hash(PyVal *v);
char *py_utf8_from_utf16(const wchar_t *w);
wchar_t *py_utf16_from_utf8(const char *s);
int py_is_widget(PyVal *v);

/* ---------------------------------------------------------- Win32 tkinter */
PyVal *py_tk_root(void);
PyVal *py_tk_widget(int kind, PyVal *parent, PyVal *kwargs);
PyVal *py_tk_mainloop(void);
PyVal *py_tk_update(void);
PyVal *py_tk_destroy(PyVal *w);
PyVal *py_msgbox(int kind, PyVal *message, PyVal *title);
void py_gui_init(void);
void py_gui_shutdown(void);

/* Implemented by pyrt_win.c and called from the object model. */
PyVal *py_gui_method(PyWidget *w, const char *name, PyVal **args, size_t argc,
                     PyVal *kwargs);
PyVal *py_gui_getattr(PyWidget *w, const char *name);
void py_gui_setattr(PyWidget *w, const char *name, PyVal *value);
void py_gui_free(PyWidget *w);

#ifdef __cplusplus
}
#endif

#endif /* PY2C99_PYRT_H */
