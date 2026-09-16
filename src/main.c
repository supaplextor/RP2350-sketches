#include <ctype.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"

#define LINE_BUF_SIZE 96

#define MAX_SCALE 9
#define MAX_IDENT 16
#define MAX_VARS 64

typedef struct {
    int64_t mag;
    int scale;
} Value;

typedef struct {
    const char *s;
    int ibase;
    char error[80];
} Parser;

typedef struct {
    char name[MAX_IDENT];
    Value value;
    bool used;
} Variable;

static Variable g_vars[MAX_VARS];
static int g_scale = 0;
static int g_ibase = 10;
static int g_obase = 10;

static void set_error(Parser *p, const char *msg) {
    if (p->error[0] == '\0') {
        strncpy(p->error, msg, sizeof(p->error) - 1);
        p->error[sizeof(p->error) - 1] = '\0';
    }
}

static void skip_ws(Parser *p) {
    while (isspace((unsigned char)*p->s)) {
        p->s++;
    }
}

static int64_t pow10_i(int n, bool *ok) {
    int64_t out = 1;
    *ok = true;
    for (int i = 0; i < n; ++i) {
        if (out > INT64_MAX / 10) {
            *ok = false;
            return 0;
        }
        out *= 10;
    }
    return out;
}

static Value make_value(int64_t mag, int scale) {
    Value v = {mag, scale};
    while (v.scale > 0 && (v.mag % 10) == 0) {
        v.mag /= 10;
        v.scale--;
    }
    return v;
}

static bool get_ident_at(const char *s, char *ident_out, const char **end_out) {
    if (!(isalpha((unsigned char)*s) || *s == '_')) {
        return false;
    }

    size_t n = 0;
    while (isalnum((unsigned char)*s) || *s == '_') {
        if (n + 1 >= MAX_IDENT) {
            return false;
        }
        ident_out[n++] = *s;
        s++;
    }
    ident_out[n] = '\0';
    *end_out = s;
    return true;
}

static bool find_var_index(const char *name, int *index_out) {
    for (int i = 0; i < MAX_VARS; ++i) {
        if (g_vars[i].used && strcmp(g_vars[i].name, name) == 0) {
            *index_out = i;
            return true;
        }
    }
    return false;
}

static bool set_variable(const char *name, Value value) {
    int idx = -1;
    if (find_var_index(name, &idx)) {
        g_vars[idx].value = value;
        return true;
    }

    for (int i = 0; i < MAX_VARS; ++i) {
        if (!g_vars[i].used) {
            g_vars[i].used = true;
            strncpy(g_vars[i].name, name, sizeof(g_vars[i].name) - 1);
            g_vars[i].name[sizeof(g_vars[i].name) - 1] = '\0';
            g_vars[i].value = value;
            return true;
        }
    }
    return false;
}

static bool get_variable(const char *name, Value *out) {
    int idx = -1;
    if (!find_var_index(name, &idx)) {
        return false;
    }
    *out = g_vars[idx].value;
    return true;
}

static int digit_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = (char)toupper((unsigned char)c);
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

static bool parse_int_in_base(const char *s, int base, int64_t *value_out, const char **end_out) {
    bool neg = false;
    if (*s == '+' || *s == '-') {
        neg = (*s == '-');
        s++;
    }

    int64_t v = 0;
    bool saw = false;
    while (*s) {
        int d = digit_value(*s);
        if (d < 0 || d >= base) {
            break;
        }
        saw = true;
        if (v > (INT64_MAX - d) / base) {
            return false;
        }
        v = v * base + d;
        s++;
    }

    if (!saw) {
        return false;
    }

    *value_out = neg ? -v : v;
    *end_out = s;
    return true;
}

static bool to_int(Value v, int64_t *out) {
    if (v.scale == 0) {
        *out = v.mag;
        return true;
    }

    bool ok = false;
    int64_t denom = pow10_i(v.scale, &ok);
    if (!ok || denom == 0 || (v.mag % denom) != 0) {
        return false;
    }

    *out = v.mag / denom;
    return true;
}

static bool checked_add64(int64_t a, int64_t b, int64_t *out) {
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) {
        return false;
    }
    *out = a + b;
    return true;
}

static bool checked_mul64(int64_t a, int64_t b, int64_t *out) {
    if (a == 0 || b == 0) {
        *out = 0;
        return true;
    }

    if (a > 0) {
        if (b > 0) {
            if (a > INT64_MAX / b) {
                return false;
            }
        } else {
            if (b < INT64_MIN / a) {
                return false;
            }
        }
    } else {
        if (b > 0) {
            if (a < INT64_MIN / b) {
                return false;
            }
        } else {
            if (a != 0 && b < INT64_MAX / a) {
                return false;
            }
        }
    }

    *out = a * b;
    return true;
}

static bool checked_scale10(int64_t value, int exp, int64_t *out) {
    int64_t v = value;
    while (exp-- > 0) {
        if (!checked_mul64(v, 10, &v)) {
            return false;
        }
    }
    *out = v;
    return true;
}

static bool value_add(Value a, Value b, Value *out) {
    int common = (a.scale > b.scale) ? a.scale : b.scale;
    int64_t am = 0;
    int64_t bm = 0;
    if (!checked_scale10(a.mag, common - a.scale, &am) ||
        !checked_scale10(b.mag, common - b.scale, &bm)) {
        return false;
    }

    int64_t rm = 0;
    if (!checked_add64(am, bm, &rm)) {
        return false;
    }
    *out = make_value(rm, common);
    return true;
}

static bool value_sub(Value a, Value b, Value *out) {
    if (b.mag == INT64_MIN) {
        return false;
    }
    b.mag = -b.mag;
    return value_add(a, b, out);
}

static bool value_mul(Value a, Value b, Value *out) {
    int64_t rm = 0;
    if (!checked_mul64(a.mag, b.mag, &rm)) {
        return false;
    }
    int rs = a.scale + b.scale;
    while (rs > MAX_SCALE) {
        rm /= 10;
        rs--;
    }
    *out = make_value(rm, rs);
    return true;
}

static bool value_div(Value a, Value b, int out_scale, Value *out) {
    if (b.mag == 0) {
        return false;
    }
    if (out_scale < 0 || out_scale > MAX_SCALE) {
        return false;
    }

    int exp = b.scale + out_scale - a.scale;
    int64_t num = a.mag;
    int64_t den = b.mag;

    while (exp > 0 && (den % 10) == 0) {
        den /= 10;
        exp--;
    }

    while (exp < 0 && (num % 10) == 0) {
        num /= 10;
        exp++;
    }

    if (exp > 0) {
        if (!checked_scale10(num, exp, &num)) {
            return false;
        }
    } else if (exp < 0) {
        if (!checked_scale10(den, -exp, &den)) {
            return false;
        }
    }

    if (den == 0) {
        return false;
    }

    *out = make_value(num / den, out_scale);
    return true;
}

static bool value_mod(Value a, Value b, Value *out) {
    int64_t ai = 0;
    int64_t bi = 0;
    if (!to_int(a, &ai) || !to_int(b, &bi) || bi == 0) {
        return false;
    }
    *out = make_value(ai % bi, 0);
    return true;
}

static bool value_pow(Value base, Value exp, Value *out) {
    int64_t e = 0;
    if (!to_int(exp, &e) || e < 0) {
        return false;
    }
    Value result = make_value(1, 0);
    Value cur = base;
    while (e > 0) {
        if (e & 1) {
            if (!value_mul(result, cur, &result)) {
                return false;
            }
        }
        e >>= 1;
        if (e > 0) {
            if (!value_mul(cur, cur, &cur)) {
                return false;
            }
        }
    }
    *out = result;
    return true;
}

static void print_int_base(int64_t value, int base) {
    if (base < 2 || base > 16) {
        printf("error: obase out of range\\n");
        return;
    }

    if (value == 0) {
        printf("0\\n");
        return;
    }

    char buf[80];
    size_t n = 0;
    bool neg = value < 0;
    uint64_t u = neg ? (uint64_t)(-value) : (uint64_t)value;
    while (u > 0 && n + 1 < sizeof(buf)) {
        int d = (int)(u % (uint64_t)base);
        buf[n++] = (char)(d < 10 ? ('0' + d) : ('A' + (d - 10)));
        u /= (uint64_t)base;
    }
    if (neg && n + 1 < sizeof(buf)) {
        buf[n++] = '-';
    }

    while (n > 0) {
        putchar(buf[--n]);
    }
    putchar('\n');
}

static void print_value(Value v) {
    if (g_obase != 10) {
        int64_t i = 0;
        if (!to_int(v, &i)) {
            printf("error: non-integer cannot be printed with obase != 10\\n");
            return;
        }
        print_int_base(i, g_obase);
        return;
    }

    bool ok = false;
    int64_t denom = pow10_i(v.scale, &ok);
    if (!ok || denom == 0) {
        printf("error: print overflow\\n");
        return;
    }

    int64_t mag = v.mag;
    bool neg = mag < 0;
    if (neg) {
        mag = -mag;
    }

    int64_t whole = (v.scale == 0) ? mag : (mag / denom);
    int64_t frac = (v.scale == 0) ? 0 : (mag % denom);

    if (v.scale == 0) {
        printf("%s%lld\\n", neg ? "-" : "", (long long)whole);
        return;
    }

    char frac_buf[16];
    for (int i = v.scale - 1; i >= 0; --i) {
        frac_buf[i] = (char)('0' + (frac % 10));
        frac /= 10;
    }
    frac_buf[v.scale] = '\0';
    printf("%s%lld.%s\\n", neg ? "-" : "", (long long)whole, frac_buf);
}

static Value parse_expr(Parser *p);

static Value parse_primary(Parser *p) {
    skip_ws(p);
    if (p->error[0] != '\0') {
        return make_value(0, 0);
    }

    if (*p->s == '(') {
        p->s++;
        Value v = parse_expr(p);
        skip_ws(p);
        if (*p->s != ')') {
            set_error(p, "missing ')' ");
            return make_value(0, 0);
        }
        p->s++;
        return v;
    }

    if (isalpha((unsigned char)*p->s) || *p->s == '_') {
        char ident[MAX_IDENT];
        const char *end = NULL;
        if (!get_ident_at(p->s, ident, &end)) {
            set_error(p, "invalid identifier");
            return make_value(0, 0);
        }
        p->s = end;

        if (strcmp(ident, "scale") == 0) {
            return make_value(g_scale, 0);
        }
        if (strcmp(ident, "ibase") == 0) {
            return make_value(g_ibase, 0);
        }
        if (strcmp(ident, "obase") == 0) {
            return make_value(g_obase, 0);
        }

        Value v;
        if (!get_variable(ident, &v)) {
            set_error(p, "undefined variable");
            return make_value(0, 0);
        }
        return v;
    }

    int64_t iv = 0;
    const char *end = NULL;
    if (!parse_int_in_base(p->s, p->ibase, &iv, &end)) {
        set_error(p, "expected number/identifier/'(' ");
        return make_value(0, 0);
    }
    p->s = end;
    return make_value(iv, 0);
}

static Value parse_unary(Parser *p) {
    skip_ws(p);
    if (*p->s == '+') {
        p->s++;
        return parse_unary(p);
    }
    if (*p->s == '-') {
        p->s++;
        Value v = parse_unary(p);
        v.mag = -v.mag;
        return v;
    }
    return parse_primary(p);
}

static Value parse_power(Parser *p) {
    Value left = parse_unary(p);
    skip_ws(p);
    if (*p->s == '^') {
        p->s++;
        Value right = parse_power(p);
        Value out;
        if (!value_pow(left, right, &out)) {
            set_error(p, "invalid/overflow power operation");
            return make_value(0, 0);
        }
        return out;
    }
    return left;
}

static Value parse_term(Parser *p) {
    Value v = parse_power(p);
    while (p->error[0] == '\0') {
        skip_ws(p);
        char op = *p->s;
        if (op != '*' && op != '/' && op != '%') {
            break;
        }
        p->s++;
        Value rhs = parse_power(p);
        Value out;
        bool ok = false;
        if (op == '*') {
            ok = value_mul(v, rhs, &out);
        } else if (op == '/') {
            ok = value_div(v, rhs, g_scale, &out);
        } else {
            ok = value_mod(v, rhs, &out);
        }
        if (!ok) {
            set_error(p, "invalid/overflow term operation");
            return make_value(0, 0);
        }
        v = out;
    }
    return v;
}

static Value parse_expr(Parser *p) {
    Value v = parse_term(p);
    while (p->error[0] == '\0') {
        skip_ws(p);
        char op = *p->s;
        if (op != '+' && op != '-') {
            break;
        }
        p->s++;
        Value rhs = parse_term(p);
        Value out;
        bool ok = (op == '+') ? value_add(v, rhs, &out) : value_sub(v, rhs, &out);
        if (!ok) {
            set_error(p, "invalid/overflow expression operation");
            return make_value(0, 0);
        }
        v = out;
    }
    return v;
}

static bool starts_with_ident_assignment(const char *line, char *ident_out, const char **rhs_out) {
    const char *p = line;
    while (isspace((unsigned char)*p)) {
        p++;
    }

    const char *end = NULL;
    if (!get_ident_at(p, ident_out, &end)) {
        return false;
    }

    p = end;
    while (isspace((unsigned char)*p)) {
        p++;
    }

    if (*p != '=') {
        return false;
    }
    p++;
    *rhs_out = p;
    return true;
}

static void print_help(void) {
    printf("serial-bc style calculator\\n");
    printf("Supported: + - * / %% ^, parentheses, unary +/-\\n");
    printf("Variables: name = expr, then use name in expressions\\n");
    printf("Settings: scale=0..9, ibase=2..16, obase=2..16\\n");
    printf("Commands: help, quit, exit\\n");
}

static bool eval_line(const char *line, Value *out, bool *has_out, bool *should_quit) {
    *has_out = false;
    *should_quit = false;

    while (isspace((unsigned char)*line)) {
        line++;
    }
    if (*line == '\0') {
        return true;
    }

    if (strcmp(line, "help") == 0) {
        print_help();
        return true;
    }
    if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) {
        *should_quit = true;
        return true;
    }

    char ident[MAX_IDENT];
    const char *rhs = NULL;
    if (starts_with_ident_assignment(line, ident, &rhs)) {
        Parser p = {.s = rhs, .ibase = g_ibase, .error = {0}};
        Value v = parse_expr(&p);
        skip_ws(&p);
        if (p.error[0] != '\0' || *p.s != '\0') {
            printf("error: %s\\n", p.error[0] ? p.error : "trailing input");
            return false;
        }

        if (strcmp(ident, "scale") == 0) {
            int64_t iv = 0;
            if (!to_int(v, &iv) || iv < 0 || iv > MAX_SCALE) {
                printf("error: scale must be integer in 0..%d\\n", MAX_SCALE);
                return false;
            }
            g_scale = (int)iv;
            *out = make_value(g_scale, 0);
            *has_out = true;
            return true;
        }
        if (strcmp(ident, "ibase") == 0) {
            int64_t iv = 0;
            if (!to_int(v, &iv) || iv < 2 || iv > 16) {
                printf("error: ibase must be integer in 2..16\\n");
                return false;
            }
            g_ibase = (int)iv;
            *out = make_value(g_ibase, 0);
            *has_out = true;
            return true;
        }
        if (strcmp(ident, "obase") == 0) {
            int64_t iv = 0;
            if (!to_int(v, &iv) || iv < 2 || iv > 16) {
                printf("error: obase must be integer in 2..16\\n");
                return false;
            }
            g_obase = (int)iv;
            *out = make_value(g_obase, 0);
            *has_out = true;
            return true;
        }

        if (!set_variable(ident, v)) {
            printf("error: variable table full\\n");
            return false;
        }
        *out = v;
        *has_out = true;
        return true;
    }

    Parser p = {.s = line, .ibase = g_ibase, .error = {0}};
    Value v = parse_expr(&p);
    skip_ws(&p);
    if (p.error[0] != '\0' || *p.s != '\0') {
        printf("error: %s\\n", p.error[0] ? p.error : "trailing input");
        return false;
    }

    *out = v;
    *has_out = true;
    return true;
}

int main(void) {
    stdio_init_all();

    sleep_ms(1500);
    printf("serial-calc (Pico SDK, RP2350)\\n");
    print_help();
    printf("> ");

    char line[LINE_BUF_SIZE];
    size_t len = 0;

    while (true) {
        int ch = getchar_timeout_us(0);
        if (ch == PICO_ERROR_TIMEOUT) {
            tight_loop_contents();
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            if (len == 0) {
                printf("> ");
                continue;
            }

            line[len] = '\0';
            Value result = make_value(0, 0);
            bool has_out = false;
            bool should_quit = false;
            if (eval_line(line, &result, &has_out, &should_quit) && has_out) {
                print_value(result);
            }
            if (should_quit) {
                printf("bye\\n");
                break;
            }
            len = 0;
            printf("> ");
            continue;
        }

        if (ch == 0x7F || ch == '\b') {
            if (len > 0) {
                len--;
            }
            continue;
        }

        if (!isprint((unsigned char)ch)) {
            continue;
        }

        if (len + 1 < LINE_BUF_SIZE) {
            line[len++] = (char)ch;
        } else {
            printf("error: line too long\\n");
            len = 0;
        }
    }

    return 0;
}
