/* Calculator, after MoonCalc (a Nintendo DS calculator for MoonShell). Keys are drawn as text cells; the
 * pointer / mouse hovers and clicks them, the D-Pad moves between them and A presses. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <wiiuse/wpad.h>

#include "calc.h"
#include "input.h"
#include "pointer.h"
#include "sfx.h"
#include "ui.h"

/* ---------------------------------------------------------------- the maths */

static bool degrees = true;
static double ans;
static bool bad;
static const char *s;                       /* the expression being read */

static double parse_expr(void);

static void skip(void) { while (*s == ' ') s++; }

static double to_rad(double x) { return degrees ? x * M_PI / 180.0 : x; }
static double from_rad(double x) { return degrees ? x * 180.0 / M_PI : x; }

static double call(const char *fn, double x)
{
    if (!strcmp(fn, "sin")) return sin(to_rad(x));
    if (!strcmp(fn, "cos")) return cos(to_rad(x));
    if (!strcmp(fn, "tan")) return tan(to_rad(x));
    if (!strcmp(fn, "asin")) return from_rad(asin(x));
    if (!strcmp(fn, "acos")) return from_rad(acos(x));
    if (!strcmp(fn, "atan")) return from_rad(atan(x));
    if (!strcmp(fn, "ln")) return log(x);
    if (!strcmp(fn, "log")) return log10(x);
    if (!strcmp(fn, "sqrt")) return sqrt(x);
    if (!strcmp(fn, "abs")) return fabs(x);
    bad = true;
    return 0;
}

static double primary(void)
{
    skip();
    if (isdigit((unsigned char)*s) || *s == '.') {
        char *end;
        double v = strtod(s, &end);
        s = end;
        return v;
    }
    if (*s == '(') {
        s++;
        double v = parse_expr();
        skip();
        if (*s == ')') s++;                 /* a missing ) at the end is let go */
        return v;
    }
    if (isalpha((unsigned char)*s)) {
        char name[8];
        int n = 0;
        while (isalpha((unsigned char)*s) && n < 7) name[n++] = *s++;
        name[n] = '\0';
        if (!strcmp(name, "pi")) return M_PI;
        if (!strcmp(name, "e")) return M_E;
        if (!strcmp(name, "ans")) return ans;
        skip();
        if (*s == '(') {
            s++;
            double v = parse_expr();
            skip();
            if (*s == ')') s++;
            return call(name, v);
        }
    }
    bad = true;
    return 0;
}

static double unary(void);

static double power(void)
{
    double v = primary();
    skip();
    while (*s == '%') {                     /* a percentage */
        v /= 100.0;
        s++;
        skip();
    }
    if (*s == '^') {
        s++;
        v = pow(v, unary());
    }
    return v;
}

static double unary(void)
{
    skip();
    if (*s == '-') { s++; return -unary(); }
    if (*s == '+') { s++; return unary(); }
    return power();
}

static double term(void)
{
    double v = unary();
    for (;;) {
        skip();
        if (*s == '*') { s++; v *= unary(); }
        else if (*s == '/') { s++; v /= unary(); }
        else return v;
    }
}

static double parse_expr(void)
{
    double v = term();
    for (;;) {
        skip();
        if (*s == '+') { s++; v += term(); }
        else if (*s == '-') { s++; v -= term(); }
        else return v;
    }
}

static bool evaluate(const char *text, double *out)
{
    s = text;
    bad = false;
    double v = parse_expr();
    skip();
    if (*s || bad || isnan(v) || isinf(v)) return false;
    *out = v;
    return true;
}

/* ---------------------------------------------------------------- the keys */

#define KCOLS 6
#define KROWS 6
#define KEY_W 10
#define KEY_H 2
#define KEY_ROW0 9
#define KEY_COL0 2

typedef struct { const char *label, *insert; } key_t_;
static const key_t_ KEYS[KROWS][KCOLS] = {
    { { "sin", "sin(" }, { "cos", "cos(" }, { "tan", "tan(" }, { "ln", "ln(" }, { "log", "log(" }, { "DEL", "\b" } },
    { { "asin", "asin(" }, { "acos", "acos(" }, { "atan", "atan(" }, { "sqrt", "sqrt(" }, { "^", "^" }, { "AC", "\f" } },
    { { "7", "7" }, { "8", "8" }, { "9", "9" }, { "(", "(" }, { ")", ")" }, { "/", "/" } },
    { { "4", "4" }, { "5", "5" }, { "6", "6" }, { "pi", "pi" }, { "e", "e" }, { "*", "*" } },
    { { "1", "1" }, { "2", "2" }, { "3", "3" }, { "Ans", "ans" }, { "%", "%" }, { "-", "-" } },
    { { "0", "0" }, { ".", "." }, { "x^2", "^2" }, { "1/x", "1/(" }, { "=", "=" }, { "+", "+" } },
};

static char expr[96];
static char result[48];
static char hist[3][96];
static int kx = 4, ky = 5;
static bool fresh;                                  /* the expression is the last answer */

static void show(void)
{
    int cols = ui_cols();
    ui_color(C_CYAN, true);
    ui_field(0, 1, cols - 2, "Calculator");
    ui_color(C_WHITE, false);
    ui_field(1, cols - 12, 10, "%s", degrees ? "DEG" : "RAD");
    ui_reset();
    for (int i = 0; i < 3; i++) {
        ui_color(C_WHITE, false);
        ui_field(2 + i, 2, cols - 4, "%s", hist[i]);
    }
    ui_reset();
    ui_color(C_WHITE, true);
    ui_field(5, 2, cols - 4, "> %s_", expr);
    ui_color(C_GREEN, true);
    ui_field(7, 2, cols - 4, "%s", result);
    ui_reset();
}

static void draw_key(int r, int c, bool sel)
{
    const char *l = KEYS[r][c].label;
    int len = (int)strlen(l), pad = (KEY_W - 2 - len) / 2;
    bool op = c == KCOLS - 1 || r < 2;
    for (int row = 0; row < KEY_H; row++) {
        if (sel) {
            ui_bg(C_CYAN);
            ui_color(C_BLACK, false);
        } else {
            ui_bg(C_BLUE);
            ui_color(C_WHITE, op);
        }
        if (row == 0)
            ui_field(KEY_ROW0 + r * KEY_H + row, KEY_COL0 + c * KEY_W, KEY_W - 1, "%*s%s", pad + 1, "", l);
        else
            ui_field(KEY_ROW0 + r * KEY_H + row, KEY_COL0 + c * KEY_W, KEY_W - 1, "");
        ui_reset();
    }
}

static void draw_all(void)
{
    ui_clear();
    show();
    for (int r = 0; r < KROWS; r++)
        for (int c = 0; c < KCOLS; c++) draw_key(r, c, r == ky && c == kx);
    ui_color(C_CYAN, true);
    ui_field(ui_rows() - 2, 1, ui_cols() - 2, "A Press  B Back  1 DEG/RAD  2 Delete  + =  - Clear");
    ui_reset();
}

static void press(const char *ins)
{
    size_t n = strlen(expr);
    if (!strcmp(ins, "\f")) {                           /* AC */
        expr[0] = result[0] = '\0';
        fresh = false;
        return;
    }
    if (!strcmp(ins, "\b")) {                           /* DEL: a function name goes whole */
        if (!n) return;
        if (expr[n - 1] == '(') {
            n--;
            while (n && isalpha((unsigned char)expr[n - 1])) n--;
        } else if (isalpha((unsigned char)expr[n - 1])) {
            while (n && isalpha((unsigned char)expr[n - 1])) n--;
        } else {
            n--;
        }
        expr[n] = '\0';
        result[0] = '\0';
        fresh = false;
        return;
    }
    if (!strcmp(ins, "=")) {
        double v;
        if (!expr[0]) return;
        if (evaluate(expr, &v)) {
            snprintf(result, sizeof(result), "= %.10g", v);
            memmove(hist[0], hist[1], sizeof(hist[0]));
            memmove(hist[1], hist[2], sizeof(hist[0]));
            snprintf(hist[2], sizeof(hist[2]), "%s %s", expr, result);
            ans = v;
            snprintf(expr, sizeof(expr), "%.10g", v);   /* go on from the answer */
            fresh = true;
        } else {
            snprintf(result, sizeof(result), "Error");
            sfx_play(SFX_LONGTAP);
        }
        return;
    }
    if (fresh && !strchr("+-*/^%", ins[0])) {            /* not an operator: a new sum */
        expr[0] = '\0';
        n = 0;
    }
    fresh = false;
    if (n + strlen(ins) < sizeof(expr)) strcat(expr, ins);
    result[0] = '\0';
}

void calc_run(void)
{
    expr[0] = result[0] = '\0';
    memset(hist, 0, sizeof(hist));
    draw_all();
    int last_r = -1, last_c = -1;
    for (;;) {
        u32 p = input_pressed() | ptr_ui_buttons();
        int pr, pc, ok = ptr_cell(&pr, &pc);
        int hr = ok ? (pr - KEY_ROW0) / KEY_H : -1, hc = ok ? (pc - KEY_COL0) / KEY_W : -1;
        bool over = ok && pr >= KEY_ROW0 && pc >= KEY_COL0 && hr < KROWS && hc < KCOLS &&
                    (pc - KEY_COL0) % KEY_W < KEY_W - 1;
        int ox = kx, oy = ky;
        if (ok && (pr != last_r || pc != last_c)) {         /* moved: the key under it is the cursor */
            last_r = pr;
            last_c = pc;
            if (over) { kx = hc; ky = hr; }
        } else if (!ok) {
            last_r = last_c = -1;
        }
        if (p & WPAD_BUTTON_UP)    ky = (ky + KROWS - 1) % KROWS;
        if (p & WPAD_BUTTON_DOWN)  ky = (ky + 1) % KROWS;
        if (p & WPAD_BUTTON_LEFT)  kx = (kx + KCOLS - 1) % KCOLS;
        if (p & WPAD_BUTTON_RIGHT) kx = (kx + 1) % KCOLS;
        /* the wheel (UP / DOWN from ptr_ui_buttons) and a click on a key */
        if ((p & WPAD_BUTTON_A) && over) { kx = hc; ky = hr; }

        bool changed = false;
        if (ox != kx || oy != ky) {
            draw_key(oy, ox, false);
            draw_key(ky, kx, true);
        }
        if (p & WPAD_BUTTON_A) { press(KEYS[ky][kx].insert); changed = true; }
        if (p & WPAD_BUTTON_2) { press("\b"); changed = true; }
        if (p & WPAD_BUTTON_PLUS) { press("="); changed = true; }
        if (p & WPAD_BUTTON_MINUS) { press("\f"); changed = true; }
        if (p & WPAD_BUTTON_1) { degrees = !degrees; changed = true; }
        if (changed) show();
        if (p & (WPAD_BUTTON_B | WPAD_BUTTON_HOME)) break;
    }
    ui_clear();
}
