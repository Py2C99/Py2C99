/*
 * py2c99 native tkinter shim.
 *
 * The public API intentionally mirrors the small part of tkinter that most
 * scripts use.  Widgets are real Win32 windows (user32.dll), layout is done by
 * us, and callbacks are ordinary transpiled Python functions.
 */
#define _CRT_SECURE_NO_WARNINGS
#ifndef UNICODE
#define UNICODE
#endif
#define _UNICODE
#include "pyrt.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <wchar.h>

#ifdef _WIN32
#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>

#ifndef TBM_SETPOS
#define TBM_SETPOS (WM_USER + 5)
#endif
#ifndef TBM_GETPOS
#define TBM_GETPOS (WM_USER)
#endif

typedef struct CanvasItem {
    int id;
    int type; /* 0 line, 1 rect, 2 oval, 3 text */
    int x1, y1, x2, y2;
    char *text;
    COLORREF color;
    COLORREF fill;
    int width;
    struct CanvasItem *next;
} CanvasItem;

struct PyWidget {
    HWND hwnd;
    int kind;
    struct PyWidget *parent;
    struct PyWidget *first_child;
    struct PyWidget *next_sibling;
    struct PyWidget *next_global;
    PyVal *self; /* weak back-reference to the PyVal wrapper */
    PyVal *opts;
    PyVal *command;
    PyVal *bindings[16];
    PyVal *text;
    PyVal *var;
    int id;
    WNDPROC oldproc;
    int req_w, req_h;
    int manager; /* 0 none, 1 pack, 2 grid, 3 place */
    int side;    /* 0 top, 1 left, 2 right, 3 bottom */
    int fill;    /* bit 1 = x, bit 2 = y */
    int expand;
    int padx, pady;
    int row, column, rowspan, columnspan;
    char sticky[16];
    int place_x, place_y, place_w, place_h;
    COLORREF bg, fg;
    int bg_set, fg_set, state;
    HBRUSH brush;
    int px, py, pw, ph;
    CanvasItem *items;
    int next_item;
    PyVal *close_fn;
};

typedef struct {
    UINT_PTR id;
    HWND hwnd;
    PyVal *fn;
} Timer;

static char *dupstr(const char *s) {
    size_t n = strlen(s ? s : "") + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s ? s : "", n);
    return p;
}
static Timer g_timers[64];
static int g_timer_count = 0;
static UINT_PTR g_timer_next = 1;

static PyWidget *g_widgets = NULL;
static PyWidget *g_root = NULL;
static int g_next_id = 100;
static HFONT g_font = NULL;
static int g_gui_ready = 0;

enum {
    EV_CLICK = 0, EV_DOUBLE, EV_RIGHT, EV_MOTION, EV_KEY,
    EV_RETURN, EV_ESCAPE, EV_CONFIGURE, EV_FOCUSIN, EV_FOCUSOUT, EV_WHEEL
};

/* --------------------------------------------------------------- utilities */
static PyVal *copy_dict(PyVal *src) {
    PyVal *d = py_dict_new();
    size_t i;
    if (src && src->type == PY_T_DICT) {
        for (i = 0; i < src->u.map.len; i++)
            py_dict_strkey(d, src->u.map.keys[i], src->u.map.vals[i]);
    }
    return d;
}

static void put_new(PyVal *d, const char *k, PyVal *v) {
    py_dict_put(d, k, v);
    py_release(v);
}

static const char *opt_str(PyVal *kw, const char *name) {
    PyVal *v = py_kw_get(kw, name);
    return (v && v->type == PY_T_STR) ? v->u.s.data : NULL;
}

static int opt_int(PyVal *kw, const char *name, int def) {
    PyVal *v = py_kw_get(kw, name);
    if (!v || v->type == PY_T_NONE) return def;
    return (int)py_as_int(v);
}

static int opt_flag(PyVal *kw, const char *name, int def) {
    PyVal *v = py_kw_get(kw, name);
    if (!v) return def;
    if (v->type == PY_T_STR) {
        if (!strcmp(v->u.s.data, "both")) return 3;
        if (!strcmp(v->u.s.data, "x")) return 1;
        if (!strcmp(v->u.s.data, "y")) return 2;
        if (!strcmp(v->u.s.data, "none")) return 0;
    }
    return py_truth(v);
}

static COLORREF named_color(const char *s) {
    if (!s) return CLR_INVALID;
    if (s[0] == '#') {
        unsigned int r = 0, g = 0, b = 0;
        if (strlen(s) >= 7) {
            sscanf(s + 1, "%2x%2x%2x", &r, &g, &b);
        } else if (strlen(s) >= 4) {
            unsigned int rr, gg, bb;
            sscanf(s + 1, "%1x%1x%1x", &rr, &gg, &bb);
            r = rr * 17; g = gg * 17; b = bb * 17;
        }
        return RGB(r, g, b);
    }
    if (!strcmp(s, "white")) return RGB(255, 255, 255);
    if (!strcmp(s, "black")) return RGB(0, 0, 0);
    if (!strcmp(s, "red")) return RGB(220, 30, 30);
    if (!strcmp(s, "green")) return RGB(30, 160, 30);
    if (!strcmp(s, "blue")) return RGB(30, 60, 220);
    if (!strcmp(s, "yellow")) return RGB(250, 230, 60);
    if (!strcmp(s, "orange")) return RGB(245, 150, 30);
    if (!strcmp(s, "purple")) return RGB(130, 40, 170);
    if (!strcmp(s, "cyan")) return RGB(40, 200, 220);
    if (!strcmp(s, "magenta")) return RGB(220, 40, 200);
    if (!strcmp(s, "gray") || !strcmp(s, "grey")) return RGB(128, 128, 128);
    if (!strcmp(s, "lightgray") || !strcmp(s, "lightgrey")) return RGB(211, 211, 211);
    if (!strcmp(s, "darkgray") || !strcmp(s, "darkgrey")) return RGB(80, 80, 80);
    if (!strcmp(s, "navy")) return RGB(20, 20, 100);
    if (!strcmp(s, "gold")) return RGB(230, 190, 40);
    return CLR_INVALID;
}

static PyWidget *find_hwnd(HWND h) {
    PyWidget *w;
    if (!h) return NULL;
    for (w = g_widgets; w; w = w->next_global)
        if (w->hwnd == h) return w;
    return NULL;
}

static PyWidget *find_id(int id) {
    PyWidget *w;
    for (w = g_widgets; w; w = w->next_global)
        if (w->id == id) return w;
    return NULL;
}

static void safe_call(PyVal *fn, PyVal **args, size_t argc) {
    PyJmp *h;
    if (!fn || (fn->type != PY_T_FUNC && fn->type != PY_T_BOUND &&
                fn->type != PY_T_CLASS && fn->type != PY_T_GUIMETHOD))
        return;
    h = py_push_handler();
    if (setjmp(h->buf) == 0) {
        PyVal *r = py_call(fn, args, argc, NULL);
        py_release(r);
    } else {
        py_print_exc(py_exc_current());
        py_exc_clear();
    }
    py_pop_handler(h);
}

static PyVal *make_event(PyWidget *w, int x, int y, const char *keysym,
                         const char *ch, int num, int delta) {
    PyVal *d = py_dict_new();
    put_new(d, "x", py_int(x));
    put_new(d, "y", py_int(y));
    put_new(d, "num", py_int(num));
    put_new(d, "delta", py_int(delta));
    put_new(d, "keysym", py_str(keysym ? keysym : ""));
    put_new(d, "char", py_str(ch ? ch : ""));
    put_new(d, "widget", w && w->self ? w->self : py_none());
    return d;
}

static void fire_event(PyWidget *w, int idx, int x, int y, const char *keysym,
                       const char *ch, int num, int delta) {
    PyVal *ev;
    if (!w || idx < 0 || idx >= 16 || !w->bindings[idx]) return;
    ev = make_event(w, x, y, keysym, ch, num, delta);
    safe_call(w->bindings[idx], &ev, 1);
    py_release(ev);
}

static int bind_index(const char *seq) {
    char buf[64];
    size_t i, n;
    if (!seq) return -1;
    while (*seq == '<') seq++;
    n = strlen(seq);
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    for (i = 0; i < n; i++) buf[i] = (char)(seq[i] >= 'A' && seq[i] <= 'Z' ? seq[i] + 32 : seq[i]);
    buf[n] = 0;
    while (n && buf[n - 1] == '>') buf[--n] = 0;
    if (!strcmp(buf, "button-1") || !strcmp(buf, "buttonpress-1")) return EV_CLICK;
    if (!strcmp(buf, "double-button-1") || !strcmp(buf, "double-1")) return EV_DOUBLE;
    if (!strcmp(buf, "button-3")) return EV_RIGHT;
    if (!strcmp(buf, "motion") || !strcmp(buf, "b1-motion")) return EV_MOTION;
    if (!strcmp(buf, "key") || !strcmp(buf, "keypress")) return EV_KEY;
    if (!strcmp(buf, "return") || !strcmp(buf, "kp-enter")) return EV_RETURN;
    if (!strcmp(buf, "escape")) return EV_ESCAPE;
    if (!strcmp(buf, "configure")) return EV_CONFIGURE;
    if (!strcmp(buf, "focusin")) return EV_FOCUSIN;
    if (!strcmp(buf, "focusout")) return EV_FOCUSOUT;
    if (!strcmp(buf, "mousewheel")) return EV_WHEEL;
    return -1;
}

static int key_name(WPARAM vk, char *out, size_t n) {
    if (vk >= 'A' && vk <= 'Z') { snprintf(out, n, "%c", (char)vk); return 1; }
    if (vk >= '0' && vk <= '9') { snprintf(out, n, "%c", (char)vk); return 1; }
    switch (vk) {
    case VK_RETURN: snprintf(out, n, "Return"); return 1;
    case VK_ESCAPE: snprintf(out, n, "Escape"); return 1;
    case VK_SPACE: snprintf(out, n, "space"); return 1;
    case VK_BACK: snprintf(out, n, "BackSpace"); return 1;
    case VK_TAB: snprintf(out, n, "Tab"); return 1;
    case VK_LEFT: snprintf(out, n, "Left"); return 1;
    case VK_RIGHT: snprintf(out, n, "Right"); return 1;
    case VK_UP: snprintf(out, n, "Up"); return 1;
    case VK_DOWN: snprintf(out, n, "Down"); return 1;
    case VK_DELETE: snprintf(out, n, "Delete"); return 1;
    default: snprintf(out, n, "??"); return 0;
    }
}

static void handle_timer(UINT_PTR id) {
    int i;
    for (i = 0; i < g_timer_count; i++) {
        if (g_timers[i].id == id) {
            PyVal *fn = g_timers[i].fn;
            HWND hwnd = g_timers[i].hwnd;
            g_timers[i].fn = NULL;
            g_timers[i].id = 0;
            g_timers[i].hwnd = NULL;
            KillTimer(hwnd, id);
            safe_call(fn, NULL, 0);
            py_release(fn);
            return;
        }
    }
}

static void handle_command(int id, int code) {
    PyWidget *w = find_id(id);
    if (!w) return;
    if (w->kind == PY_W_BUTTON && code == BN_CLICKED) {
        safe_call(w->command, NULL, 0);
    } else if ((w->kind == PY_W_CHECKBUTTON || w->kind == PY_W_RADIOBUTTON) && code == BN_CLICKED) {
        if (w->var) {
            LRESULT st = SendMessageW(w->hwnd, BM_GETCHECK, 0, 0);
            PyVal *v = py_int(st == BST_CHECKED ? 1 : 0);
            py_assign(&w->var->u.var.value, v);
            py_release(v);
        }
        safe_call(w->command, NULL, 0);
    } else if (w->kind == PY_W_ENTRY && code == EN_CHANGE) {
        if (w->var) {
            int n = GetWindowTextLengthW(w->hwnd);
            wchar_t *buf = (wchar_t *)malloc((size_t)(n + 1) * sizeof(wchar_t));
            char *u8;
            if (!buf) return;
            GetWindowTextW(w->hwnd, buf, n + 1);
            u8 = py_utf8_from_utf16(buf);
            free(buf);
            if (u8) {
                PyVal *v = py_str(u8);
                py_assign(&w->var->u.var.value, v);
                py_release(v);
                free(u8);
            }
        }
    }
}

static void layout_parent(PyWidget *p);
static HBRUSH GetSolidBrushCache(PyWidget *c);

static LRESULT CALLBACK subclass_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    PyWidget *w = (PyWidget *)GetWindowLongPtrW(h, GWLP_USERDATA);
    WNDPROC old = w ? w->oldproc : NULL;
    switch (msg) {
    case WM_LBUTTONDOWN:
        fire_event(w, EV_CLICK, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), "", "", 1, 0);
        break;
    case WM_LBUTTONDBLCLK:
        fire_event(w, EV_DOUBLE, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), "", "", 1, 0);
        break;
    case WM_RBUTTONDOWN:
        fire_event(w, EV_RIGHT, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), "", "", 3, 0);
        break;
    case WM_MOUSEMOVE:
        fire_event(w, EV_MOTION, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), "", "", 0, 0);
        break;
    case WM_KEYDOWN: {
        char name[32];
        int idx;
        key_name(wp, name, sizeof(name));
        fire_event(w, EV_KEY, 0, 0, name, name, 0, 0);
        idx = bind_index(name);
        if (idx == EV_RETURN || idx == EV_ESCAPE) fire_event(w, idx, 0, 0, name, "", 0, 0);
        break;
    }
    case WM_MOUSEWHEEL:
        fire_event(w, EV_WHEEL, 0, 0, "MouseWheel", "", 0, GET_WHEEL_DELTA_WPARAM(wp));
        break;
    case WM_TIMER:
        handle_timer(wp);
        break;
    }
    if (old) return CallWindowProcW(old, h, msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

/* --------------------------------------------------------------- painting */
static void paint_canvas(PyWidget *w, HDC dc) {
    RECT rc;
    CanvasItem *it;
    GetClientRect(w->hwnd, &rc);
    if (w->bg_set) {
        HBRUSH b = CreateSolidBrush(w->bg);
        FillRect(dc, &rc, b);
        DeleteObject(b);
    } else {
        FillRect(dc, &rc, (HBRUSH)(COLOR_WINDOW + 1));
    }
    for (it = w->items; it; it = it->next) {
        HPEN pen = CreatePen(PS_SOLID, it->width > 0 ? it->width : 1, it->color);
        HGDIOBJ oldpen = SelectObject(dc, pen);
        if (it->type == 1) {
            HBRUSH br = CreateSolidBrush(it->fill);
            FillRect(dc, &(RECT){it->x1, it->y1, it->x2, it->y2}, br);
            DeleteObject(br);
            SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, it->x1, it->y1, it->x2, it->y2);
        } else if (it->type == 2) {
            HBRUSH br = CreateSolidBrush(it->fill);
            HGDIOBJ oldbr = SelectObject(dc, br);
            Ellipse(dc, it->x1, it->y1, it->x2, it->y2);
            SelectObject(dc, oldbr);
            DeleteObject(br);
        } else if (it->type == 3) {
            wchar_t *wt = py_utf16_from_utf8(it->text ? it->text : "");
            SetTextColor(dc, it->color);
            SetBkMode(dc, TRANSPARENT);
            TextOutW(dc, it->x1, it->y1, wt, (int)wcslen(wt));
            free(wt);
        } else {
            MoveToEx(dc, it->x1, it->y1, NULL);
            LineTo(dc, it->x2, it->y2);
        }
        SelectObject(dc, oldpen);
        DeleteObject(pen);
    }
}

static LRESULT CALLBACK canvas_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    PyWidget *w = (PyWidget *)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (w) paint_canvas(w, dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
        if (w) fire_event(w, EV_CLICK, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), "", "", 1, 0);
        return 0;
    case WM_LBUTTONDBLCLK:
        if (w) fire_event(w, EV_DOUBLE, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), "", "", 1, 0);
        return 0;
    case WM_RBUTTONDOWN:
        if (w) fire_event(w, EV_RIGHT, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), "", "", 3, 0);
        return 0;
    case WM_MOUSEMOVE:
        if (w) fire_event(w, EV_MOTION, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), "", "", 0, 0);
        return 0;
    case WM_KEYDOWN:
        if (w) {
            char name[32];
            key_name(wp, name, sizeof(name));
            fire_event(w, EV_KEY, 0, 0, name, name, 0, 0);
        }
        return 0;
    case WM_TIMER:
        handle_timer(wp);
        return 0;
    case WM_COMMAND:
        handle_command(LOWORD(wp), HIWORD(wp));
        return 0;
    case WM_SIZE:
        if (w) InvalidateRect(h, NULL, TRUE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        if (w) w->hwnd = NULL;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static LRESULT CALLBACK frame_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    PyWidget *w = (PyWidget *)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (w) {
            RECT rc;
            GetClientRect(h, &rc);
            if (w->bg_set) {
                HBRUSH b = CreateSolidBrush(w->bg);
                FillRect(dc, &rc, b);
                DeleteObject(b);
            } else {
                FillRect(dc, &rc, (HBRUSH)(COLOR_BTNFACE + 1));
            }
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_COMMAND:
        handle_command(LOWORD(wp), HIWORD(wp));
        return 0;
    case WM_TIMER:
        handle_timer(wp);
        return 0;
    case WM_SIZE:
        if (w) layout_parent(w);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT: {
        PyWidget *c = find_hwnd((HWND)lp);
        if (c) {
            if (c->fg_set) SetTextColor((HDC)wp, c->fg);
            if (c->bg_set) {
                SetBkColor((HDC)wp, c->bg);
                return (LRESULT)GetSolidBrushCache(c);
            }
        }
        break;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static LRESULT CALLBACK root_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    PyWidget *w = find_hwnd(h);
    switch (msg) {
    case WM_COMMAND:
        handle_command(LOWORD(wp), HIWORD(wp));
        return 0;
    case WM_TIMER:
        handle_timer(wp);
        return 0;
    case WM_SIZE:
        if (w) layout_parent(w);
        return 0;
    case WM_CLOSE:
        if (w && w->close_fn) safe_call(w->close_fn, NULL, 0);
        if (w && w->hwnd) DestroyWindow(w->hwnd);
        else DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        if (w) w->hwnd = NULL;
        PostQuitMessage(0);
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT: {
        PyWidget *c = find_hwnd((HWND)lp);
        if (c) {
            if (c->fg_set) SetTextColor((HDC)wp, c->fg);
            if (c->bg_set) {
                SetBkColor((HDC)wp, c->bg);
                return (LRESULT)GetSolidBrushCache(c);
            }
        }
        break;
    }
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* --------------------------------------------------------------- sizing */
static void compute_request(PyWidget *w, PyVal *kw) {
    int width = opt_int(kw, "width", 0);
    int height = opt_int(kw, "height", 0);
    int pad = opt_int(kw, "padx", 8);
    int pady = opt_int(kw, "pady", 4);
    const char *text = opt_str(kw, "text");
    if (!text) text = opt_str(kw, "label");
    switch (w->kind) {
    case PY_W_ROOT:
        w->req_w = width > 0 ? width : 640;
        w->req_h = height > 0 ? height : 480;
        break;
    case PY_W_LABEL: case PY_W_BUTTON: case PY_W_CHECKBUTTON:
    case PY_W_RADIOBUTTON: {
        HDC dc = GetDC(NULL);
        SIZE sz = {0, 0};
        HGDIOBJ old = SelectObject(dc, g_font);
        wchar_t *wt = py_utf16_from_utf8(text ? text : "");
        GetTextExtentPoint32W(dc, wt, (int)wcslen(wt), &sz);
        free(wt);
        SelectObject(dc, old);
        ReleaseDC(NULL, dc);
        w->req_w = width > 0 ? width * 8 : sz.cx + pad * 2 + 8;
        w->req_h = height > 0 ? height * 16 : sz.cy + pady * 2 + 6;
        if (w->req_w < 44) w->req_w = 44;
        if (w->req_h < 24) w->req_h = 24;
        break;
    }
    case PY_W_ENTRY:
        w->req_w = width > 0 ? width * 8 : 180;
        w->req_h = height > 0 ? height * 16 : 26;
        break;
    case PY_W_TEXT:
        w->req_w = width > 0 ? width * 8 : 360;
        w->req_h = height > 0 ? height * 16 : 200;
        break;
    case PY_W_LISTBOX:
        w->req_w = width > 0 ? width * 8 : 220;
        w->req_h = height > 0 ? height * 16 : 160;
        break;
    case PY_W_CANVAS:
        w->req_w = width > 0 ? width : 320;
        w->req_h = height > 0 ? height : 220;
        break;
    case PY_W_FRAME:
        w->req_w = width > 0 ? width : 240;
        w->req_h = height > 0 ? height : 140;
        break;
    case PY_W_SCALE:
        w->req_w = width > 0 ? width : 220;
        w->req_h = 32;
        break;
    case PY_W_PROGRESSBAR:
        w->req_w = width > 0 ? width : 220;
        w->req_h = 22;
        break;
    default:
        w->req_w = width > 0 ? width : 200;
        w->req_h = height > 0 ? height : 80;
        break;
    }
}

/* --------------------------------------------------------------- layout */
static void layout_parent(PyWidget *p) {
    RECT rc;
    int W, H, x = 0, y = 0, rx, by;
    PyWidget *c;
    if (!p || !p->hwnd) return;
    GetClientRect(p->hwnd, &rc);
    W = rc.right;
    H = rc.bottom;
    rx = W;
    by = H;
    for (c = p->first_child; c; c = c->next_sibling) {
        if (!c->hwnd || c->manager != 1) continue;
        if (c->side == 0) {
            int cw = (c->fill & 1) || c->expand ? W : c->req_w;
            int ch = c->req_h;
            SetWindowPos(c->hwnd, HWND_TOP, c->padx, y + c->pady,
                         cw > 0 ? cw : 1, ch, SWP_NOACTIVATE);
            y += ch + c->pady * 2;
        } else if (c->side == 3) {
            int cw = (c->fill & 1) || c->expand ? W : c->req_w;
            int ch = c->req_h;
            by -= ch + c->pady * 2;
            SetWindowPos(c->hwnd, HWND_TOP, c->padx, by + c->pady,
                         cw > 0 ? cw : 1, ch, SWP_NOACTIVATE);
        } else if (c->side == 1) {
            int cw = c->req_w;
            int ch = (c->fill & 2) || c->expand ? H : c->req_h;
            SetWindowPos(c->hwnd, HWND_TOP, x + c->padx, c->pady,
                         cw, ch > 0 ? ch : 1, SWP_NOACTIVATE);
            x += cw + c->padx * 2;
        } else {
            int cw = c->req_w;
            int ch = (c->fill & 2) || c->expand ? H : c->req_h;
            rx -= cw + c->padx * 2;
            SetWindowPos(c->hwnd, HWND_TOP, rx + c->padx, c->pady,
                         cw, ch > 0 ? ch : 1, SWP_NOACTIVATE);
        }
    }
    {
        int colw[64], rowh[64];
        int maxc = 0, maxr = 0, i, j;
        memset(colw, 0, sizeof(colw));
        memset(rowh, 0, sizeof(rowh));
        for (c = p->first_child; c; c = c->next_sibling) {
            if (!c->hwnd || c->manager != 2) continue;
            if (c->column > 63) c->column = 63;
            if (c->row > 63) c->row = 63;
            if (c->column + 1 > maxc) maxc = c->column + 1;
            if (c->row + 1 > maxr) maxr = c->row + 1;
            if (c->req_w + c->padx * 2 > colw[c->column]) colw[c->column] = c->req_w + c->padx * 2;
            if (c->req_h + c->pady * 2 > rowh[c->row]) rowh[c->row] = c->req_h + c->pady * 2;
        }
        {
            int cx[65], cy[65];
            cx[0] = 0; cy[0] = 0;
            for (i = 0; i < maxc; i++) cx[i + 1] = cx[i] + colw[i];
            for (j = 0; j < maxr; j++) cy[j + 1] = cy[j] + rowh[j];
            for (c = p->first_child; c; c = c->next_sibling) {
                int cw, ch, px, py;
                if (!c->hwnd || c->manager != 2) continue;
                cw = c->req_w;
                ch = c->req_h;
                if (c->columnspan > 1) cw = cx[c->column + c->columnspan] - cx[c->column] - c->padx * 2;
                if (c->rowspan > 1) ch = cy[c->row + c->rowspan] - cy[c->row] - c->pady * 2;
                if (strchr(c->sticky, 'e') && strchr(c->sticky, 'w'))
                    cw = (c->columnspan > 1 ? cx[c->column + c->columnspan] - cx[c->column] : colw[c->column]) - c->padx * 2;
                if (strchr(c->sticky, 'n') && strchr(c->sticky, 's'))
                    ch = (c->rowspan > 1 ? cy[c->row + c->rowspan] - cy[c->row] : rowh[c->row]) - c->pady * 2;
                px = cx[c->column] + c->padx;
                py = cy[c->row] + c->pady;
                if (strchr(c->sticky, 'e') && !strchr(c->sticky, 'w'))
                    px = cx[c->column + (c->columnspan > 1 ? c->columnspan : 1)] - cw - c->padx;
                if (strchr(c->sticky, 's') && !strchr(c->sticky, 'n'))
                    py = cy[c->row + (c->rowspan > 1 ? c->rowspan : 1)] - ch - c->pady;
                if (cw < 1) cw = 1;
                if (ch < 1) ch = 1;
                SetWindowPos(c->hwnd, HWND_TOP, px, py, cw, ch, SWP_NOACTIVATE);
            }
        }
    }
    for (c = p->first_child; c; c = c->next_sibling) {
        if (!c->hwnd || c->manager != 3) continue;
        SetWindowPos(c->hwnd, HWND_TOP, c->place_x, c->place_y,
                     c->place_w > 0 ? c->place_w : c->req_w,
                     c->place_h > 0 ? c->place_h : c->req_h, SWP_NOACTIVATE);
    }
    InvalidateRect(p->hwnd, NULL, TRUE);
}

static HBRUSH GetSolidBrushCache(PyWidget *c) {
    if (!c) return (HBRUSH)(COLOR_WINDOW + 1);
    if (!c->bg_set) return (HBRUSH)(COLOR_WINDOW + 1);
    if (!c->brush) c->brush = CreateSolidBrush(c->bg);
    return c->brush;
}

/* --------------------------------------------------------------- creation */
static void attach_child(PyWidget *parent, PyWidget *child) {
    child->parent = parent;
    child->next_sibling = NULL;
    if (!parent->first_child) parent->first_child = child;
    else {
        PyWidget *t = parent->first_child;
        while (t->next_sibling) t = t->next_sibling;
        t->next_sibling = child;
    }
}

static PyWidget *new_widget(int kind, PyWidget *parent) {
    PyWidget *w = (PyWidget *)calloc(1, sizeof(PyWidget));
    if (!w) py_raise(PY_EXC_MEMORY, "out of memory creating widget");
    w->kind = kind;
    w->id = g_next_id++;
    w->next_item = 1;
    w->state = 1;
    w->next_global = g_widgets;
    g_widgets = w;
    if (parent) attach_child(parent, w);
    return w;
}

static void apply_options(PyWidget *w, PyVal *kw);

static PyWidget *create_widget(int kind, PyWidget *parent, PyVal *kw) {
    const wchar_t *cls = L"STATIC";
    DWORD style = WS_CHILD | WS_VISIBLE;
    DWORD ex = 0;
    const char *text = opt_str(kw, "text");
    PyWidget *w;
    wchar_t *wtext;
    if (!text) text = opt_str(kw, "label");
    if (!text && kind == PY_W_ROOT) text = opt_str(kw, "title");
    if (!text) text = "";
    switch (kind) {
    case PY_W_ROOT:
        cls = L"Py2C99Root";
        style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
        break;
    case PY_W_FRAME:
        cls = L"Py2C99Frame";
        style = WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN;
        break;
    case PY_W_CANVAS:
        cls = L"Py2C99Canvas";
        style = WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN;
        break;
    case PY_W_LABEL:
        cls = L"STATIC";
        style |= SS_LEFT;
        break;
    case PY_W_BUTTON:
        cls = L"BUTTON";
        style |= BS_PUSHBUTTON | WS_TABSTOP;
        break;
    case PY_W_ENTRY:
        cls = L"EDIT";
        style |= WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP;
        break;
    case PY_W_TEXT:
        cls = L"EDIT";
        style |= WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP;
        break;
    case PY_W_LISTBOX:
        cls = L"LISTBOX";
        style |= WS_BORDER | WS_VSCROLL | LBS_NOTIFY | WS_TABSTOP;
        break;
    case PY_W_CHECKBUTTON:
        cls = L"BUTTON";
        style |= BS_AUTOCHECKBOX | WS_TABSTOP;
        break;
    case PY_W_RADIOBUTTON:
        cls = L"BUTTON";
        style |= BS_AUTORADIOBUTTON | WS_TABSTOP;
        break;
    case PY_W_SCALE:
        cls = TRACKBAR_CLASSW;
        style |= TBS_HORZ | WS_TABSTOP;
        break;
    case PY_W_PROGRESSBAR:
        cls = PROGRESS_CLASSW;
        break;
    case PY_W_SEPARATOR:
        cls = L"STATIC";
        style |= SS_ETCHEDHORZ;
        break;
    default:
        cls = L"STATIC";
        style |= SS_LEFT;
        break;
    }
    w = new_widget(kind, parent);
    w->req_w = 100;
    w->req_h = 30;
    compute_request(w, kw);
    wtext = py_utf16_from_utf8(text);
    w->hwnd = CreateWindowExW(ex, cls, wtext, style,
                              CW_USEDEFAULT, CW_USEDEFAULT,
                              w->req_w, w->req_h,
                              kind == PY_W_ROOT ? NULL : (parent ? parent->hwnd : NULL),
                              kind == PY_W_ROOT ? NULL : (HMENU)(INT_PTR)w->id,
                              GetModuleHandleW(NULL), NULL);
    free(wtext);
    if (!w->hwnd) {
        py_raise(PY_EXC_OS, "CreateWindowEx failed");
    }
    SetWindowLongPtrW(w->hwnd, GWLP_USERDATA, (LONG_PTR)w);
    if (kind == PY_W_ROOT || kind == PY_W_FRAME || kind == PY_W_CANVAS) {
        w->oldproc = NULL;
    } else {
        w->oldproc = (WNDPROC)SetWindowLongPtrW(w->hwnd, GWLP_WNDPROC, (LONG_PTR)subclass_proc);
    }
    if (kind == PY_W_ROOT) {
        w->oldproc = NULL;
    }
    SendMessageW(w->hwnd, WM_SETFONT, (WPARAM)g_font, TRUE);
    w->opts = copy_dict(kw);
    apply_options(w, kw);
    if (kind == PY_W_ROOT) {
        g_root = w;
        layout_parent(w);
    } else if (parent) {
        layout_parent(parent);
    }
    if (kind == PY_W_CANVAS) InvalidateRect(w->hwnd, NULL, TRUE);
    return w;
}

/* --------------------------------------------------------- option handling */
static void set_control_text(PyWidget *w, const char *utf8) {
    wchar_t *wt = py_utf16_from_utf8(utf8 ? utf8 : "");
    if (w->kind == PY_W_LISTBOX) {
        SendMessageW(w->hwnd, LB_ADDSTRING, 0, (LPARAM)wt);
    } else {
        SetWindowTextW(w->hwnd, wt);
    }
    free(wt);
}

static void geometry_apply(PyWidget *w, const char *geo) {
    int x = 0, y = 0, ww = 0, hh = 0;
    char *p;
    if (!geo) return;
    ww = atoi(geo);
    p = strchr(geo, 'x');
    if (p) {
        hh = atoi(p + 1);
        p = strchr(p + 1, '+');
        if (p) { x = atoi(p + 1); p = strchr(p + 1, '+'); if (p) y = atoi(p + 1); }
    }
    if (ww > 0 && hh > 0)
        SetWindowPos(w->hwnd, NULL, x, y, ww, hh, SWP_NOZORDER | SWP_NOACTIVATE);
}

static void apply_options(PyWidget *w, PyVal *kw) {
    const char *s;
    int i;
    if (!kw) return;
    s = opt_str(kw, "text");
    if (!s) s = opt_str(kw, "label");
    if (s && w->hwnd) {
        if (w->kind != PY_W_LISTBOX) set_control_text(w, s);
    }
    s = opt_str(kw, "title");
    if (s && w->kind == PY_W_ROOT) {
        wchar_t *wt = py_utf16_from_utf8(s);
        SetWindowTextW(w->hwnd, wt);
        free(wt);
    }
    s = opt_str(kw, "bg");
    if (!s) s = opt_str(kw, "background");
    if (s) {
        COLORREF c = named_color(s);
        if (c != CLR_INVALID) { w->bg = c; w->bg_set = 1; if (w->brush) { DeleteObject(w->brush); w->brush = NULL; } }
    }
    s = opt_str(kw, "fg");
    if (!s) s = opt_str(kw, "foreground");
    if (s) {
        COLORREF c = named_color(s);
        if (c != CLR_INVALID) { w->fg = c; w->fg_set = 1; }
    }
    s = opt_str(kw, "state");
    if (s) {
        w->state = !strcmp(s, "disabled") ? 0 : 1;
        EnableWindow(w->hwnd, w->state);
    }
    s = opt_str(kw, "geometry");
    if (s && w->kind == PY_W_ROOT) geometry_apply(w, s);
    {
        PyVal *cmd = py_kw_get(kw, "command");
        if (cmd) py_assign(&w->command, cmd);
    }
    {
        PyVal *var = py_kw_get(kw, "textvariable");
        if (var && var->type == PY_T_VAR) {
            py_assign(&w->var, var);
            if (var->u.var.value) {
                PyVal *sv = py_str_of(var->u.var.value);
                if (w->kind != PY_W_LISTBOX) set_control_text(w, sv->u.s.data);
                py_release(sv);
            }
        }
    }
    {
        PyVal *vals = py_kw_get(kw, "values");
        if (vals && w->kind == PY_W_LISTBOX) {
            PyVal *it = py_iter(vals), *item = NULL;
            while (py_iter_next(it, &item)) {
                PyVal *sv = py_str_of(item);
                wchar_t *wt = py_utf16_from_utf8(sv->u.s.data);
                SendMessageW(w->hwnd, LB_ADDSTRING, 0, (LPARAM)wt);
                free(wt);
                py_release(sv);
            }
            py_release(it);
            py_release(item);
        }
    }
    {
        PyVal *val = py_kw_get(kw, "value");
        if (val && w->kind == PY_W_SCALE) {
            SendMessageW(w->hwnd, TBM_SETPOS, TRUE, (LPARAM)py_as_int(val));
        }
    }
    if (opt_int(kw, "width", -1) >= 0 || opt_int(kw, "height", -1) >= 0) {
        compute_request(w, kw);
    }
    {
        PyVal *side = py_kw_get(kw, "side");
        if (side && side->type == PY_T_STR) {
            if (!strcmp(side->u.s.data, "left")) w->side = 1;
            else if (!strcmp(side->u.s.data, "right")) w->side = 2;
            else if (!strcmp(side->u.s.data, "bottom")) w->side = 3;
            else w->side = 0;
        }
    }
    if (py_kw_get(kw, "fill")) w->fill = opt_flag(kw, "fill", 0);
    if (py_kw_get(kw, "expand")) w->expand = opt_flag(kw, "expand", 0);
    if (py_kw_get(kw, "padx")) w->padx = opt_int(kw, "padx", 0);
    if (py_kw_get(kw, "pady")) w->pady = opt_int(kw, "pady", 0);
    if (py_kw_get(kw, "row")) { w->manager = 2; w->row = opt_int(kw, "row", 0); }
    if (py_kw_get(kw, "column")) { w->manager = 2; w->column = opt_int(kw, "column", 0); }
    if (py_kw_get(kw, "rowspan")) w->rowspan = opt_int(kw, "rowspan", 1);
    if (py_kw_get(kw, "columnspan")) w->columnspan = opt_int(kw, "columnspan", 1);
    s = opt_str(kw, "sticky");
    if (s) { strncpy(w->sticky, s, sizeof(w->sticky) - 1); w->sticky[sizeof(w->sticky) - 1] = 0; }
    if (py_kw_get(kw, "ipadx") || py_kw_get(kw, "ipady")) {
        w->req_w += opt_int(kw, "ipadx", 0) * 2;
        w->req_h += opt_int(kw, "ipady", 0) * 2;
    }
    if (py_kw_get(kw, "x")) { w->manager = 3; w->place_x = opt_int(kw, "x", 0); }
    if (py_kw_get(kw, "y")) { w->manager = 3; w->place_y = opt_int(kw, "y", 0); }
    if (py_kw_get(kw, "relx") || py_kw_get(kw, "rely")) {
        RECT rc = {0, 0, 0, 0};
        if (w->parent && w->parent->hwnd) GetClientRect(w->parent->hwnd, &rc);
        w->manager = 3;
        if (py_kw_get(kw, "relx")) w->place_x = (int)(rc.right * (float)py_as_float(py_kw_get(kw, "relx")));
        if (py_kw_get(kw, "rely")) w->place_y = (int)(rc.bottom * (float)py_as_float(py_kw_get(kw, "rely")));
    }
    (void)i;
}

/* ------------------------------------------------------------------ init */
static void register_classes(HINSTANCE inst) {
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = L"Py2C99Root";
    wc.lpfnWndProc = root_proc;
    RegisterClassExW(&wc);
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = L"Py2C99Frame";
    wc.lpfnWndProc = frame_proc;
    RegisterClassExW(&wc);
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = L"Py2C99Canvas";
    wc.lpfnWndProc = canvas_proc;
    RegisterClassExW(&wc);
}

void py_gui_init(void) {
    INITCOMMONCONTROLSEX icc;
    if (g_gui_ready) return;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);
    register_classes(GetModuleHandleW(NULL));
    g_font = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    g_gui_ready = 1;
}

void py_gui_shutdown(void) {
    PyWidget *w, *n;
    int i;
    for (i = 0; i < g_timer_count; i++) {
        if (g_timers[i].id) KillTimer(NULL, g_timers[i].id);
        py_release(g_timers[i].fn);
        g_timers[i].fn = NULL;
    }
    g_timer_count = 0;
    for (w = g_widgets; w; w = n) {
        CanvasItem *it, *inx;
        n = w->next_global;
        if (w->hwnd) DestroyWindow(w->hwnd);
        for (it = w->items; it; it = inx) {
            inx = it->next;
            free(it->text);
            free(it);
        }
        py_release(w->opts);
        py_release(w->command);
        for (i = 0; i < 16; i++) py_release(w->bindings[i]);
        py_release(w->text);
        py_release(w->var);
        py_release(w->close_fn);
        if (w->brush) DeleteObject(w->brush);
        free(w);
    }
    g_widgets = NULL;
    g_root = NULL;
    if (g_font) { DeleteObject(g_font); g_font = NULL; }
    g_gui_ready = 0;
}
/* ---------------------------------------------------------- public widgets */
static PyVal *wrap_widget(PyWidget *w) {
    PyVal *v;
    if (!w) return py_none();
    if (w->self) { py_retain(w->self); return w->self; }
    v = py_widget_value(w);
    w->self = v;
    return v;
}

PyVal *py_tk_root(void) {
    PyWidget *w;
    PyVal *v;
    if (!g_gui_ready) py_gui_init();
    w = create_widget(PY_W_ROOT, NULL, NULL);
    v = py_widget_value(w);
    w->self = v;
    SetWindowTextW(w->hwnd, L"tk");
    ShowWindow(w->hwnd, SW_SHOW);
    UpdateWindow(w->hwnd);
    return v;
}

PyVal *py_tk_widget(int kind, PyVal *parent, PyVal *kwargs) {
    PyWidget *p = NULL, *w;
    PyVal *v;
    if (!g_gui_ready) py_gui_init();
    if (kind == PY_W_ROOT) return py_tk_root();
    if (parent && parent->type == PY_T_WIDGET) p = parent->u.widget.w;
    if (!p) p = g_root;
    if (!p) {
        PyVal *root = py_tk_root();
        p = root->u.widget.w;
        py_release(root);
    }
    w = create_widget(kind, p, kwargs);
    v = py_widget_value(w);
    w->self = v;
    if (kind == PY_W_CANVAS) InvalidateRect(w->hwnd, NULL, TRUE);
    return v;
}

PyVal *py_tk_mainloop(void) {
    MSG msg;
    if (!g_root) return py_none();
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return py_none();
}

PyVal *py_tk_update(void) {
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return py_none();
}

PyVal *py_tk_destroy(PyVal *w) {
    PyWidget *p = NULL;
    if (w && w->type == PY_T_WIDGET) p = w->u.widget.w;
    if (!p) return py_none();
    if (p->hwnd) DestroyWindow(p->hwnd);
    p->hwnd = NULL;
    if (p == g_root) g_root = NULL;
    return py_none();
}

PyVal *py_msgbox(int kind, PyVal *message, PyVal *title) {
    PyVal *ms = py_str_of(message);
    PyVal *ts = py_str_of(title);
    wchar_t *wm = py_utf16_from_utf8(ms->u.s.data);
    wchar_t *wt = py_utf16_from_utf8(ts->u.s.data);
    UINT flags = MB_OK;
    int r;
    switch (kind) {
    case 1: flags = MB_ICONWARNING | MB_OK; break;
    case 2: flags = MB_ICONERROR | MB_OK; break;
    case 3: flags = MB_ICONQUESTION | MB_OK; break;
    case 4: flags = MB_ICONQUESTION | MB_YESNO; break;
    case 5: flags = MB_ICONQUESTION | MB_YESNOCANCEL; break;
    case 6: flags = MB_ICONQUESTION | MB_OKCANCEL; break;
    case 7: flags = MB_ICONWARNING | MB_RETRYCANCEL; break;
    default: flags = MB_ICONINFORMATION | MB_OK; break;
    }
    r = MessageBoxW(g_root ? g_root->hwnd : NULL, wm, wt, flags);
    free(wm);
    free(wt);
    py_release(ms);
    py_release(ts);
    if (kind == 4) return py_bool(r == IDYES);
    if (kind == 5) return r == IDYES ? py_bool(1) : r == IDNO ? py_bool(0) : py_none();
    if (kind == 6) return py_bool(r == IDOK);
    if (kind == 7) return py_bool(r == IDRETRY);
    return py_none();
}

/* ---------------------------------------------------------- widget helpers */
static PyVal *wtext(PyWidget *w) {
    int n;
    wchar_t *buf;
    char *u8;
    PyVal *r;
    if (!w || !w->hwnd) return py_str("");
    n = GetWindowTextLengthW(w->hwnd);
    buf = (wchar_t *)malloc((size_t)(n + 1) * sizeof(wchar_t));
    if (!buf) return py_str("");
    GetWindowTextW(w->hwnd, buf, n + 1);
    u8 = py_utf8_from_utf16(buf);
    free(buf);
    if (!u8) return py_str("");
    r = py_str(u8);
    free(u8);
    return r;
}

static void append_control_text(PyWidget *w, const char *utf8) {
    wchar_t *wt = py_utf16_from_utf8(utf8);
    SendMessageW(w->hwnd, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    SendMessageW(w->hwnd, EM_REPLACESEL, FALSE, (LPARAM)wt);
    free(wt);
}

static void set_text_plain(PyWidget *w, const char *utf8) {
    wchar_t *wt = py_utf16_from_utf8(utf8);
    SetWindowTextW(w->hwnd, wt);
    free(wt);
}

static PyVal *listbox_get(PyWidget *w, int index) {
    int n = (int)SendMessageW(w->hwnd, LB_GETCOUNT, 0, 0);
    int len;
    wchar_t *buf;
    char *u8;
    PyVal *r;
    if (index < 0) index += n;
    if (index < 0 || index >= n) py_raise(PY_EXC_INDEX, "listbox index out of range");
    len = (int)SendMessageW(w->hwnd, LB_GETTEXTLEN, (WPARAM)index, 0);
    buf = (wchar_t *)malloc((size_t)(len + 1) * sizeof(wchar_t));
    SendMessageW(w->hwnd, LB_GETTEXT, (WPARAM)index, (LPARAM)buf);
    u8 = py_utf8_from_utf16(buf);
    free(buf);
    r = py_str(u8 ? u8 : "");
    free(u8);
    return r;
}

static CanvasItem *canvas_find(PyWidget *w, int id) {
    CanvasItem *it;
    for (it = w->items; it; it = it->next) if (it->id == id) return it;
    return NULL;
}

static PyVal *canvas_create(PyWidget *w, int type, PyVal **args, size_t argc,
                            PyVal *kwargs) {
    CanvasItem *it = (CanvasItem *)calloc(1, sizeof(CanvasItem));
    const char *s;
    it->id = w->next_item++;
    it->type = type;
    it->color = RGB(0, 0, 0);
    it->fill = RGB(200, 200, 200);
    it->width = 1;
    if (argc > 0) it->x1 = (int)py_as_int(args[0]);
    if (argc > 1) it->y1 = (int)py_as_int(args[1]);
    if (type != 3) {
        if (argc > 2) it->x2 = (int)py_as_int(args[2]);
        if (argc > 3) it->y2 = (int)py_as_int(args[3]);
    }
    if (type == 3) {
        s = opt_str(kwargs, "text");
        if (!s && argc > 2) {
            PyVal *sv = py_str_of(args[2]);
            it->text = dupstr(sv->u.s.data);
            py_release(sv);
        } else {
            it->text = dupstr(s ? s : "");
        }
    }
    s = opt_str(kwargs, "fill");
    if (s) { COLORREF c = named_color(s); if (c != CLR_INVALID) it->fill = c; }
    s = opt_str(kwargs, "outline");
    if (s) { COLORREF c = named_color(s); if (c != CLR_INVALID) it->color = c; }
    else {
        s = opt_str(kwargs, "fill");
        if (s && type == 0) { COLORREF c = named_color(s); if (c != CLR_INVALID) it->color = c; }
    }
    if (py_kw_get(kwargs, "width")) it->width = opt_int(kwargs, "width", 1);
    it->next = w->items;
    w->items = it;
    InvalidateRect(w->hwnd, NULL, TRUE);
    return py_int(it->id);
}

static PyVal *canvas_method(PyWidget *w, const char *name, PyVal **args, size_t argc,
                            PyVal *kwargs) {
    if (!strcmp(name, "create_line") || !strcmp(name, "create_polygon"))
        return canvas_create(w, 0, args, argc, kwargs);
    if (!strcmp(name, "create_rectangle")) return canvas_create(w, 1, args, argc, kwargs);
    if (!strcmp(name, "create_oval") || !strcmp(name, "create_arc"))
        return canvas_create(w, 2, args, argc, kwargs);
    if (!strcmp(name, "create_text")) return canvas_create(w, 3, args, argc, kwargs);
    if (!strcmp(name, "delete")) {
        if (argc > 0 && args[0]->type == PY_T_STR && !strcmp(args[0]->u.s.data, "all")) {
            CanvasItem *it = w->items, *nx;
            while (it) { nx = it->next; free(it->text); free(it); it = nx; }
            w->items = NULL;
        } else {
            int id = argc > 0 ? (int)py_as_int(args[0]) : -1;
            CanvasItem **pp = &w->items;
            while (*pp) {
                if ((*pp)->id == id) {
                    CanvasItem *nx = (*pp)->next;
                    free((*pp)->text);
                    free(*pp);
                    *pp = nx;
                    break;
                }
                pp = &(*pp)->next;
            }
        }
        InvalidateRect(w->hwnd, NULL, TRUE);
        return py_none();
    }
    if (!strcmp(name, "coords")) {
        int id = argc > 0 ? (int)py_as_int(args[0]) : -1;
        CanvasItem *it = canvas_find(w, id);
        if (it) {
            if (argc > 1) it->x1 = (int)py_as_int(args[1]);
            if (argc > 2) it->y1 = (int)py_as_int(args[2]);
            if (argc > 3) it->x2 = (int)py_as_int(args[3]);
            if (argc > 4) it->y2 = (int)py_as_int(args[4]);
            InvalidateRect(w->hwnd, NULL, TRUE);
        }
        return py_none();
    }
    if (!strcmp(name, "itemconfig") || !strcmp(name, "itemconfigure")) {
        int id = argc > 0 ? (int)py_as_int(args[0]) : -1;
        CanvasItem *it = canvas_find(w, id);
        if (it) {
            const char *s = opt_str(kwargs, "fill");
            if (s) { COLORREF c = named_color(s); if (c != CLR_INVALID) it->fill = c; }
            s = opt_str(kwargs, "outline");
            if (s) { COLORREF c = named_color(s); if (c != CLR_INVALID) it->color = c; }
            s = opt_str(kwargs, "text");
            if (s) { free(it->text); it->text = dupstr(s); }
            if (py_kw_get(kwargs, "width")) it->width = opt_int(kwargs, "width", it->width);
            InvalidateRect(w->hwnd, NULL, TRUE);
        }
        return py_none();
    }
    if (!strcmp(name, "itemcget")) {
        int id = argc > 0 ? (int)py_as_int(args[0]) : -1;
        const char *opt = argc > 1 ? py_cstr(args[1]) : "";
        CanvasItem *it = canvas_find(w, id);
        if (!it) return py_str("");
        if (!strcmp(opt, "text")) return py_str(it->text ? it->text : "");
        return py_str("");
    }
    if (!strcmp(name, "find_all")) {
        PyVal *r = py_list_new();
        CanvasItem *it;
        for (it = w->items; it; it = it->next) {
            PyVal *v = py_int(it->id);
            py_list_append(r, v);
            py_release(v);
        }
        return r;
    }
    if (!strcmp(name, "find_closest") || !strcmp(name, "find_overlapping")) return py_list_new();
    if (!strcmp(name, "itemcget")) return py_str("");
    if (!strcmp(name, "focus") || !strcmp(name, "bind")) return py_none();
    py_raise(PY_EXC_ATTRIBUTE, "canvas object has no such method");
    return py_none();
}

static int as_index(PyVal *v, int n, int def) {
    if (!v || v->type == PY_T_NONE) return def;
    if (v->type == PY_T_STR) {
        if (!strcmp(v->u.s.data, "end")) return n;
        if (!strcmp(v->u.s.data, "active")) return 0;
    }
    return (int)py_as_int(v);
}

static PyVal *listbox_method(PyWidget *w, const char *name, PyVal **args, size_t argc,
                             PyVal *kwargs) {
    (void)kwargs;
    if (!strcmp(name, "insert")) {
        size_t i;
        for (i = 1; i < argc; i++) {
            PyVal *sv = py_str_of(args[i]);
            wchar_t *wt = py_utf16_from_utf8(sv->u.s.data);
            SendMessageW(w->hwnd, LB_ADDSTRING, 0, (LPARAM)wt);
            free(wt);
            py_release(sv);
        }
        return py_none();
    }
    if (!strcmp(name, "delete")) {
        int n = (int)SendMessageW(w->hwnd, LB_GETCOUNT, 0, 0);
        int first = as_index(argc > 0 ? args[0] : NULL, n, 0);
        int last = argc > 1 ? as_index(args[1], n, first) : first;
        int i;
        if (last < 0) last += n;
        if (last >= n) last = n - 1;
        if (first <= 0 && last >= n - 1) {
            SendMessageW(w->hwnd, LB_RESETCONTENT, 0, 0);
        } else {
            for (i = last; i >= first; i--) SendMessageW(w->hwnd, LB_DELETESTRING, (WPARAM)i, 0);
        }
        return py_none();
    }
    if (!strcmp(name, "get")) {
        int n = (int)SendMessageW(w->hwnd, LB_GETCOUNT, 0, 0);
        int first = as_index(argc > 0 ? args[0] : NULL, n, 0);
        int last = argc > 1 ? as_index(args[1], n, first) : first;
        if (last < 0) last += n;
        if (argc <= 1) return listbox_get(w, first);
        else {
            PyVal *r = py_list_new();
            int i;
            for (i = first; i <= last && i < n; i++) {
                PyVal *v = listbox_get(w, i);
                py_list_append(r, v);
                py_release(v);
            }
            return r;
        }
    }
    if (!strcmp(name, "size")) return py_int((int64_t)SendMessageW(w->hwnd, LB_GETCOUNT, 0, 0));
    if (!strcmp(name, "curselection")) {
        PyVal *r = py_list_new();
        int n = (int)SendMessageW(w->hwnd, LB_GETCOUNT, 0, 0);
        int i;
        for (i = 0; i < n; i++) {
            if (SendMessageW(w->hwnd, LB_GETSEL, (WPARAM)i, 0) > 0) {
                PyVal *v = py_int(i);
                py_list_append(r, v);
                py_release(v);
            }
        }
        return r;
    }
    if (!strcmp(name, "selection_clear")) {
        SendMessageW(w->hwnd, LB_SETSEL, FALSE, (LPARAM)-1);
        return py_none();
    }
    if (!strcmp(name, "selection_set")) {
        int first = argc > 0 ? (int)py_as_int(args[0]) : 0;
        int last = argc > 1 ? (int)py_as_int(args[1]) : first;
        int i;
        for (i = first; i <= last; i++) SendMessageW(w->hwnd, LB_SETSEL, TRUE, (LPARAM)i);
        return py_none();
    }
    if (!strcmp(name, "activate") || !strcmp(name, "see")) return py_none();
    py_raise(PY_EXC_ATTRIBUTE, "listbox object has no such method");
    return py_none();
}

static PyVal *entry_text_method(PyWidget *w, const char *name, PyVal **args, size_t argc,
                                PyVal *kwargs) {
    (void)kwargs;
    if (!strcmp(name, "get")) return wtext(w);
    if (!strcmp(name, "insert")) {
        if (argc > 1) {
            PyVal *sv = py_str_of(args[1]);
            append_control_text(w, sv->u.s.data);
            py_release(sv);
        }
        return py_none();
    }
    if (!strcmp(name, "delete")) {
        int last_is_end = 1;
        if (argc > 1) {
            if (args[1]->type == PY_T_STR) {
                last_is_end = !strcmp(args[1]->u.s.data, "end");
            } else {
                last_is_end = py_as_int(args[1]) >= 1;
            }
        }
        if (argc <= 1 || last_is_end) set_text_plain(w, "");
        return py_none();
    }
    if (!strcmp(name, "icursor") || !strcmp(name, "see") || !strcmp(name, "xview") ||
        !strcmp(name, "yview") || !strcmp(name, "focus") || !strcmp(name, "tag_add") ||
        !strcmp(name, "edit_undo") || !strcmp(name, "edit_reset"))
        return py_none();
    if (!strcmp(name, "index")) return py_str("0");
    if (!strcmp(name, "select_range")) {
        int a = argc > 0 ? (int)py_as_int(args[0]) : 0;
        int b = argc > 1 ? (int)py_as_int(args[1]) : a;
        SendMessageW(w->hwnd, EM_SETSEL, (WPARAM)a, (LPARAM)b);
        return py_none();
    }
    py_raise(PY_EXC_ATTRIBUTE, "text widget has no such method");
    return py_none();
}

static void set_option_one(PyWidget *w, const char *name, PyVal *value) {
    PyVal *kw = py_dict_new();
    py_dict_put(kw, name, value);
    apply_options(w, kw);
    py_release(kw);
    if (w->opts) py_dict_put(w->opts, name, value);
    if (w->parent) layout_parent(w->parent);
}

PyVal *py_gui_method(PyWidget *w, const char *name, PyVal **args, size_t argc,
                     PyVal *kwargs) {
    if (!w) py_raise(PY_EXC_ATTRIBUTE, "invalid widget");
    if (!strcmp(name, "pack") || !strcmp(name, "pack_forget")) {
        if (!strcmp(name, "pack_forget")) { ShowWindow(w->hwnd, SW_HIDE); return py_none(); }
        w->manager = 1;
        if (kwargs) {
            const char *s = opt_str(kwargs, "side");
            if (s) {
                if (!strcmp(s, "left")) w->side = 1;
                else if (!strcmp(s, "right")) w->side = 2;
                else if (!strcmp(s, "bottom")) w->side = 3;
                else w->side = 0;
            }
            w->fill = opt_flag(kwargs, "fill", w->fill);
            w->expand = opt_flag(kwargs, "expand", w->expand);
            w->padx = opt_int(kwargs, "padx", w->padx);
            w->pady = opt_int(kwargs, "pady", w->pady);
        }
        ShowWindow(w->hwnd, SW_SHOW);
        if (w->parent) layout_parent(w->parent);
        return py_none();
    }
    if (!strcmp(name, "grid") || !strcmp(name, "grid_forget")) {
        if (!strcmp(name, "grid_forget")) { ShowWindow(w->hwnd, SW_HIDE); return py_none(); }
        w->manager = 2;
        if (kwargs) {
            w->row = opt_int(kwargs, "row", w->row);
            w->column = opt_int(kwargs, "column", w->column);
            w->rowspan = opt_int(kwargs, "rowspan", w->rowspan ? w->rowspan : 1);
            w->columnspan = opt_int(kwargs, "columnspan", w->columnspan ? w->columnspan : 1);
            {
                const char *s = opt_str(kwargs, "sticky");
                if (s) { strncpy(w->sticky, s, sizeof(w->sticky) - 1); w->sticky[sizeof(w->sticky) - 1] = 0; }
            }
            w->padx = opt_int(kwargs, "padx", w->padx);
            w->pady = opt_int(kwargs, "pady", w->pady);
        }
        ShowWindow(w->hwnd, SW_SHOW);
        if (w->parent) layout_parent(w->parent);
        return py_none();
    }
    if (!strcmp(name, "place") || !strcmp(name, "place_forget")) {
        if (!strcmp(name, "place_forget")) { ShowWindow(w->hwnd, SW_HIDE); return py_none(); }
        w->manager = 3;
        if (kwargs) {
            w->place_x = opt_int(kwargs, "x", w->place_x);
            w->place_y = opt_int(kwargs, "y", w->place_y);
            w->place_w = opt_int(kwargs, "width", w->place_w);
            w->place_h = opt_int(kwargs, "height", w->place_h);
            if (py_kw_get(kwargs, "relx") || py_kw_get(kwargs, "rely")) {
                RECT rc = {0, 0, 0, 0};
                if (w->parent && w->parent->hwnd) GetClientRect(w->parent->hwnd, &rc);
                if (py_kw_get(kwargs, "relx")) w->place_x = (int)(rc.right * (float)py_as_float(py_kw_get(kwargs, "relx")));
                if (py_kw_get(kwargs, "rely")) w->place_y = (int)(rc.bottom * (float)py_as_float(py_kw_get(kwargs, "rely")));
            }
        }
        ShowWindow(w->hwnd, SW_SHOW);
        if (w->parent) layout_parent(w->parent);
        return py_none();
    }
    if (!strcmp(name, "config") || !strcmp(name, "configure") || !strcmp(name, "__setitem__")) {
        if (!strcmp(name, "__setitem__")) {
            if (argc >= 2) set_option_one(w, py_cstr(args[0]), args[1]);
            return py_none();
        }
        if (!kwargs || kwargs->u.map.len == 0) return copy_dict(w->opts);
        apply_options(w, kwargs);
        {
            size_t i;
            for (i = 0; i < kwargs->u.map.len; i++)
                if (w->opts) py_dict_strkey(w->opts, kwargs->u.map.keys[i], kwargs->u.map.vals[i]);
        }
        if (w->parent) layout_parent(w->parent);
        return py_none();
    }
    if (!strcmp(name, "cget")) {
        PyVal *v;
        if (argc < 1) py_raise(PY_EXC_TYPE, "cget() takes one argument");
        v = w->opts ? py_dict_get(w->opts, py_cstr(args[0])) : NULL;
        if (v) { py_retain(v); return v; }
        if (!strcmp(py_cstr(args[0]), "text")) return wtext(w);
        return py_none();
    }
    if (!strcmp(name, "bind")) {
        int idx;
        if (argc < 2) py_raise(PY_EXC_TYPE, "bind() takes two arguments");
        idx = bind_index(py_cstr(args[0]));
        if (idx >= 0) py_assign(&w->bindings[idx], args[1]);
        return py_none();
    }
    if (!strcmp(name, "unbind")) {
        int idx;
        if (argc < 1) return py_none();
        idx = bind_index(py_cstr(args[0]));
        if (idx >= 0) { py_release(w->bindings[idx]); w->bindings[idx] = NULL; }
        return py_none();
    }
    if (!strcmp(name, "mainloop")) return py_tk_mainloop();
    if (!strcmp(name, "quit")) { PostQuitMessage(0); return py_none(); }
    if (!strcmp(name, "destroy")) {
        if (w->hwnd) DestroyWindow(w->hwnd);
        w->hwnd = NULL;
        if (w == g_root) g_root = NULL;
        return py_none();
    }
    if (!strcmp(name, "update") || !strcmp(name, "update_idletasks")) return py_tk_update();
    if (!strcmp(name, "after") || !strcmp(name, "after_idle")) {
        int ms = !strcmp(name, "after_idle") ? 1 : (argc > 0 ? (int)py_as_int(args[0]) : 1);
        PyVal *fn = argc > 1 ? args[1] : NULL;
        int slot = -1, i;
        UINT_PTR id;
        for (i = 0; i < g_timer_count; i++) if (!g_timers[i].id) { slot = i; break; }
        if (slot < 0 && g_timer_count < 64) slot = g_timer_count++;
        if (slot < 0 || !fn) return py_int(0);
        id = g_timer_next++;
        g_timers[slot].id = id;
        g_timers[slot].hwnd = w->hwnd;
        g_timers[slot].fn = fn;
        py_retain(fn);
        SetTimer(w->hwnd, id, (UINT)(ms > 0 ? ms : 1), NULL);
        return py_int((int64_t)id);
    }
    if (!strcmp(name, "after_cancel")) {
        if (argc > 0) {
            UINT_PTR id = (UINT_PTR)py_as_int(args[0]);
            int i;
            for (i = 0; i < g_timer_count; i++)
                if (g_timers[i].id == id) {
                    KillTimer(g_timers[i].hwnd, id);
                    g_timers[i].id = 0;
                    g_timers[i].hwnd = NULL;
                    py_release(g_timers[i].fn);
                    g_timers[i].fn = NULL;
                }
        }
        return py_none();
    }
    if (!strcmp(name, "winfo_width") || !strcmp(name, "winfo_height")) {
        RECT rc;
        GetClientRect(w->hwnd, &rc);
        return py_int(!strcmp(name, "winfo_width") ? rc.right : rc.bottom);
    }
    if (!strcmp(name, "winfo_reqwidth")) return py_int(w->req_w);
    if (!strcmp(name, "winfo_reqheight")) return py_int(w->req_h);
    if (!strcmp(name, "winfo_x") || !strcmp(name, "winfo_y")) {
        RECT rc;
        POINT pt;
        GetWindowRect(w->hwnd, &rc);
        pt.x = rc.left; pt.y = rc.top;
        if (w->parent && w->parent->hwnd) ScreenToClient(w->parent->hwnd, &pt);
        return py_int(!strcmp(name, "winfo_x") ? pt.x : pt.y);
    }
    if (!strcmp(name, "winfo_rootx") || !strcmp(name, "winfo_rooty")) {
        RECT rc;
        GetWindowRect(w->hwnd, &rc);
        return py_int(!strcmp(name, "winfo_rootx") ? rc.left : rc.top);
    }
    if (!strcmp(name, "winfo_screenwidth")) return py_int(GetSystemMetrics(SM_CXSCREEN));
    if (!strcmp(name, "winfo_screenheight")) return py_int(GetSystemMetrics(SM_CYSCREEN));
    if (!strcmp(name, "winfo_exists")) return py_bool(w->hwnd != NULL);
    if (!strcmp(name, "winfo_ismapped")) return py_bool(w->hwnd && IsWindowVisible(w->hwnd));
    if (!strcmp(name, "winfo_children")) {
        PyVal *r = py_list_new();
        PyWidget *c;
        for (c = w->first_child; c; c = c->next_sibling) {
            PyVal *v = wrap_widget(c);
            py_list_append(r, v);
            py_release(v);
        }
        return r;
    }
    if (!strcmp(name, "winfo_toplevel")) return wrap_widget(g_root ? g_root : w);
    if (!strcmp(name, "winfo_name")) return py_str("!widget");
    if (!strcmp(name, "title")) {
        if (argc > 0) { set_text_plain(w, py_cstr(args[0])); return py_none(); }
        return wtext(w);
    }
    if (!strcmp(name, "geometry")) {
        if (argc > 0) { geometry_apply(w, py_cstr(args[0])); return py_none(); }
        {
            RECT rc;
            char buf[64];
            GetWindowRect(w->hwnd, &rc);
            snprintf(buf, sizeof(buf), "%dx%d+%d+%d", rc.right - rc.left, rc.bottom - rc.top, rc.left, rc.top);
            return py_str(buf);
        }
    }
    if (!strcmp(name, "resizable") || !strcmp(name, "minsize") || !strcmp(name, "maxsize") ||
        !strcmp(name, "grid_columnconfigure") || !strcmp(name, "grid_rowconfigure") ||
        !strcmp(name, "grid_propagate") || !strcmp(name, "pack_propagate") ||
        !strcmp(name, "columnconfigure") || !strcmp(name, "rowconfigure") ||
        !strcmp(name, "iconphoto") || !strcmp(name, "wm_iconbitmap") ||
        !strcmp(name, "attributes") || !strcmp(name, "transient") ||
        !strcmp(name, "overrideredirect") || !strcmp(name, "focus_force") ||
        !strcmp(name, "grab_set") || !strcmp(name, "grab_release") ||
        !strcmp(name, "wait_window") || !strcmp(name, "wait_visibility") ||
        !strcmp(name, "tag_configure") || !strcmp(name, "tag_bind") ||
        !strcmp(name, "tag_remove") || !strcmp(name, "mark_set") ||
        !strcmp(name, "mark_unset") || !strcmp(name, "edit_modified") ||
        !strcmp(name, "edit_separator") || !strcmp(name, "edit_undo") ||
        !strcmp(name, "edit_redo") || !strcmp(name, "yview_scroll") ||
        !strcmp(name, "xview_scroll") || !strcmp(name, "yview_moveto") ||
        !strcmp(name, "xview_moveto") || !strcmp(name, "start") ||
        !strcmp(name, "stop") || !strcmp(name, "step") ||
        !strcmp(name, "selection") || !strcmp(name, "clipboard_clear") ||
        !strcmp(name, "clipboard_append") || !strcmp(name, "bell"))
        return py_none();
    if (!strcmp(name, "protocol")) {
        if (argc >= 2) { py_assign(&w->close_fn, args[1]); return py_none(); }
        return py_none();
    }
    if (!strcmp(name, "iconify")) { ShowWindow(w->hwnd, SW_MINIMIZE); return py_none(); }
    if (!strcmp(name, "deiconify")) { ShowWindow(w->hwnd, SW_RESTORE); return py_none(); }
    if (!strcmp(name, "state")) {
        if (argc > 0) {
            const char *s = py_cstr(args[0]);
            if (!strcmp(s, "iconic")) ShowWindow(w->hwnd, SW_MINIMIZE);
            else if (!strcmp(s, "withdrawn")) ShowWindow(w->hwnd, SW_HIDE);
            else if (!strcmp(s, "zoomed")) ShowWindow(w->hwnd, SW_MAXIMIZE);
            else ShowWindow(w->hwnd, SW_RESTORE);
            return py_none();
        }
        return py_str(IsIconic(w->hwnd) ? "iconic" : IsWindowVisible(w->hwnd) ? "normal" : "withdrawn");
    }
    if (!strcmp(name, "focus_set") || !strcmp(name, "focus")) { SetFocus(w->hwnd); return py_none(); }
    if (!strcmp(name, "lift")) { SetWindowPos(w->hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE); return py_none(); }
    if (!strcmp(name, "lower")) { SetWindowPos(w->hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE); return py_none(); }
    if (!strcmp(name, "winfo_id")) return py_int((int64_t)(intptr_t)w->hwnd);

    if (w->kind == PY_W_CANVAS) return canvas_method(w, name, args, argc, kwargs);
    if (w->kind == PY_W_LISTBOX) return listbox_method(w, name, args, argc, kwargs);
    if (w->kind == PY_W_ENTRY || w->kind == PY_W_TEXT)
        return entry_text_method(w, name, args, argc, kwargs);
    if (w->kind == PY_W_BUTTON && !strcmp(name, "invoke")) {
        SendMessageW(w->hwnd, BM_CLICK, 0, 0);
        return py_none();
    }
    if (w->kind == PY_W_CHECKBUTTON || w->kind == PY_W_RADIOBUTTON) {
        if (!strcmp(name, "get")) {
            LRESULT st = SendMessageW(w->hwnd, BM_GETCHECK, 0, 0);
            return py_int(st == BST_CHECKED ? 1 : 0);
        }
        if (!strcmp(name, "set") || !strcmp(name, "select")) {
            SendMessageW(w->hwnd, BM_SETCHECK, BST_CHECKED, 0);
            return py_none();
        }
        if (!strcmp(name, "deselect")) { SendMessageW(w->hwnd, BM_SETCHECK, BST_UNCHECKED, 0); return py_none(); }
        if (!strcmp(name, "toggle")) {
            LRESULT st = SendMessageW(w->hwnd, BM_GETCHECK, 0, 0);
            SendMessageW(w->hwnd, BM_SETCHECK, st == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED, 0);
            return py_none();
        }
        if (!strcmp(name, "invoke")) { SendMessageW(w->hwnd, BM_CLICK, 0, 0); return py_none(); }
    }
    if (w->kind == PY_W_SCALE) {
        if (!strcmp(name, "get")) return py_int((int64_t)SendMessageW(w->hwnd, TBM_GETPOS, 0, 0));
        if (!strcmp(name, "set")) {
            SendMessageW(w->hwnd, TBM_SETPOS, TRUE, (LPARAM)py_as_int(args[0]));
            return py_none();
        }
    }
    if (w->kind == PY_W_PROGRESSBAR) {
        if (!strcmp(name, "value")) {
            if (argc > 0) {
                int v = (int)py_as_int(args[0]);
                SendMessageW(w->hwnd, PBM_SETPOS, (WPARAM)v, 0);
                return py_none();
            }
            return py_int((int64_t)SendMessageW(w->hwnd, PBM_GETPOS, 0, 0));
        }
        return py_none();
    }
    py_raise(PY_EXC_ATTRIBUTE, "widget has no such method");
    return py_none();
}

PyVal *py_gui_getattr(PyWidget *w, const char *name) {
    if (!w) py_raise(PY_EXC_ATTRIBUTE, "invalid widget");
    if (!strcmp(name, "master") || !strcmp(name, "parent") || !strcmp(name, "tk"))
        return wrap_widget(w->parent ? w->parent : (g_root ? g_root : w));
    if (!strcmp(name, "children")) {
        PyVal *r = py_list_new();
        PyWidget *c;
        for (c = w->first_child; c; c = c->next_sibling) {
            PyVal *v = wrap_widget(c);
            py_list_append(r, v);
            py_release(v);
        }
        return r;
    }
    if (!strcmp(name, "widgetName")) return py_str("!widget");
    if (!strcmp(name, "winfo")) return py_none();
    return py_guimethod(w, name);
}

void py_gui_setattr(PyWidget *w, const char *name, PyVal *value) {
    if (!w) return;
    set_option_one(w, name, value);
}

void py_gui_free(PyWidget *w) {
    if (w) w->self = NULL;
}

#else
void py_gui_init(void) {}
void py_gui_shutdown(void) {}
PyVal *py_tk_root(void) { py_raise(PY_EXC_OS, "tkinter is only available on Windows"); return py_none(); }
PyVal *py_tk_widget(int kind, PyVal *parent, PyVal *kwargs) { (void)kind; (void)parent; (void)kwargs; return py_none(); }
PyVal *py_tk_mainloop(void) { return py_none(); }
PyVal *py_tk_update(void) { return py_none(); }
PyVal *py_tk_destroy(PyVal *w) { (void)w; return py_none(); }
PyVal *py_gui_method(PyWidget *w, const char *n, PyVal **a, size_t c, PyVal *k) { (void)w;(void)n;(void)a;(void)c;(void)k; return py_none(); }
PyVal *py_gui_getattr(PyWidget *w, const char *n) { (void)w;(void)n; return py_none(); }
void py_gui_setattr(PyWidget *w, const char *n, PyVal *v) { (void)w;(void)n;(void)v; }
void py_gui_free(PyWidget *w) { (void)w; }
PyVal *py_msgbox(int kind, PyVal *m, PyVal *t) { (void)kind;(void)m;(void)t; return py_none(); }
#endif
