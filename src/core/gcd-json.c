/*
 * gcd-json.c — minimal JSON DOM parser + builder.
 * SPDX-License-Identifier: MIT
 */

#include <string.h>
#include <stdlib.h>
#include <gio/gio.h>
#include "gcd/gcd-json.h"

struct GcdJson {
  GcdJsonType type;
  union {
    gboolean boolean;
    gdouble  num;
    gchar   *str;
    GPtrArray *arr;  /* GcdJson* */
    GHashTable *obj; /* key -> GcdJson*; member order kept in `keys` */
  } u;
  GPtrArray *keys;   /* member order for objects (gchar*, owned) */
};

/* ============================================================== parser */

typedef struct {
  const gchar *p, *end;
  GError **error;
} Parser;

static void parse_fail(Parser *ps, const gchar *msg)
{
  if (ps->error && !*ps->error)
    g_set_error(ps->error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "JSON parse error near byte %ld: %s",
                (long)(ps->p - (ps->p - (ps->end - ps->p))), msg);
  /* offset printed crudely; message is what matters */
}

static void skip_ws(Parser *ps)
{
  while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' ||
                             *ps->p == '\n' || *ps->p == '\r'))
    ps->p++;
}

static gboolean at_end(Parser *ps) { skip_ws(ps); return ps->p >= ps->end; }

static GcdJson *parse_value(Parser *ps);

static gboolean parse_literal(Parser *ps, const gchar *lit)
{
  gsize n = strlen(lit);
  if ((gsize)(ps->end - ps->p) >= n && memcmp(ps->p, lit, n) == 0) {
    ps->p += n;
    return TRUE;
  }
  return FALSE;
}

static gboolean parse_hex4(Parser *ps, gunichar *out)
{
  if (ps->end - ps->p < 4) return FALSE;
  gunichar c = 0;
  for (int i = 0; i < 4; i++) {
    gchar ch = *ps->p++;
    c <<= 4;
    if (ch >= '0' && ch <= '9') c |= ch - '0';
    else if (ch >= 'a' && ch <= 'f') c |= ch - 'a' + 10;
    else if (ch >= 'A' && ch <= 'F') c |= ch - 'A' + 10;
    else return FALSE;
  }
  *out = c;
  return TRUE;
}

static gchar *parse_string_raw(Parser *ps)
{
  GString *s;
  if (ps->p >= ps->end || *ps->p != '"') {
    parse_fail(ps, "expected string");
    return NULL;
  }
  ps->p++;
  s = g_string_new(NULL);
  while (ps->p < ps->end) {
    gchar c = *ps->p++;
    if (c == '"')
      return g_string_free(s, FALSE);
    if (c == '\\') {
      if (ps->p >= ps->end) break;
      switch (*ps->p++) {
        case '"':  g_string_append_c(s, '"');  break;
        case '\\': g_string_append_c(s, '\\'); break;
        case '/':  g_string_append_c(s, '/');  break;
        case 'b':  g_string_append_c(s, '\b'); break;
        case 'f':  g_string_append_c(s, '\f'); break;
        case 'n':  g_string_append_c(s, '\n'); break;
        case 'r':  g_string_append_c(s, '\r'); break;
        case 't':  g_string_append_c(s, '\t'); break;
        case 'u': {
          gunichar w1;
          if (!parse_hex4(ps, &w1)) goto bad;
          /* \uD800-\uDBFF surrogate pair */
          if (w1 >= 0xD800 && w1 <= 0xDBFF &&
              ps->end - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u') {
            gunichar w2;
            ps->p += 2;
            if (parse_hex4(ps, &w2) && w2 >= 0xDC00 && w2 <= 0xDFFF)
              w1 = 0x10000 + ((w1 - 0xD800) << 10) + (w2 - 0xDC00);
            else
              ps->p -= 2;
          }
          g_string_append_unichar(s, w1);
          break;
        }
        default: goto bad;
      }
    } else {
      g_string_append_c(s, c);
    }
  }
bad:
  g_string_free(s, TRUE);
  parse_fail(ps, "unterminated or invalid string");
  return NULL;
}

static GcdJson *json_new(GcdJsonType t)
{
  GcdJson *j = g_new0(GcdJson, 1);
  j->type = t;
  return j;
}

static GcdJson *parse_number(Parser *ps)
{
  const gchar *start = ps->p;
  if (ps->p < ps->end && *ps->p == '-') ps->p++;
  while (ps->p < ps->end &&
         ((*ps->p >= '0' && *ps->p <= '9') || *ps->p == '.' ||
          *ps->p == 'e' || *ps->p == 'E' || *ps->p == '+' || *ps->p == '-'))
    ps->p++;
  if (ps->p == start || (ps->p - start == 1 && *start == '-')) {
    parse_fail(ps, "invalid number");
    return NULL;
  }
  gchar *numstr = g_strndup(start, ps->p - start);
  GcdJson *j = json_new(GCD_JSON_NUM);
  j->u.num = g_ascii_strtod(numstr, NULL);
  g_free(numstr);
  return j;
}

static GcdJson *parse_array(Parser *ps)
{
  GcdJson *arr = json_new(GCD_JSON_ARR);
  arr->u.arr = g_ptr_array_new_with_free_func((GDestroyNotify)gcd_json_free);
  ps->p++; /* '[' */
  skip_ws(ps);
  if (ps->p < ps->end && *ps->p == ']') { ps->p++; return arr; }
  for (;;) {
    GcdJson *v = parse_value(ps);
    if (!v) { gcd_json_free(arr); return NULL; }
    g_ptr_array_add(arr->u.arr, v);
    skip_ws(ps);
    if (ps->p >= ps->end) break;
    if (*ps->p == ',') { ps->p++; continue; }
    if (*ps->p == ']') { ps->p++; return arr; }
    break;
  }
  gcd_json_free(arr);
  parse_fail(ps, "unterminated array");
  return NULL;
}

static GcdJson *parse_object(Parser *ps)
{
  GcdJson *obj = json_new(GCD_JSON_OBJ);
  obj->u.obj = g_hash_table_new_full(g_str_hash, g_str_equal,
                                   g_free, (GDestroyNotify)gcd_json_free);
  obj->keys = g_ptr_array_new_with_free_func(g_free);
  ps->p++; /* '{' */
  skip_ws(ps);
  if (ps->p < ps->end && *ps->p == '}') { ps->p++; return obj; }
  for (;;) {
    skip_ws(ps);
    gchar *key = parse_string_raw(ps);
    if (!key) { gcd_json_free(obj); return NULL; }
    skip_ws(ps);
    if (ps->p >= ps->end || *ps->p != ':') {
      g_free(key); gcd_json_free(obj);
      parse_fail(ps, "expected ':'");
      return NULL;
    }
    ps->p++;
    GcdJson *v = parse_value(ps);
    if (!v) { g_free(key); gcd_json_free(obj); return NULL; }
    if (g_hash_table_contains(obj->u.obj, key)) {
      g_hash_table_replace(obj->u.obj, key, v); /* replaces in place */
    } else {
      g_hash_table_insert(obj->u.obj, key, v);
      g_ptr_array_add(obj->keys, g_strdup(key));
    }
    skip_ws(ps);
    if (ps->p >= ps->end) break;
    if (*ps->p == ',') { ps->p++; continue; }
    if (*ps->p == '}') { ps->p++; return obj; }
    break;
  }
  gcd_json_free(obj);
  parse_fail(ps, "unterminated object");
  return NULL;
}

static GcdJson *parse_value(Parser *ps)
{
  skip_ws(ps);
  if (ps->p >= ps->end) { parse_fail(ps, "unexpected end"); return NULL; }
  switch (*ps->p) {
    case '{': return parse_object(ps);
    case '[': return parse_array(ps);
    case '"': {
      gchar *s = parse_string_raw(ps);
      if (!s) return NULL;
      GcdJson *j = json_new(GCD_JSON_STR);
      j->u.str = s;
      return j;
    }
    case 't': if (parse_literal(ps, "true"))  { GcdJson *j = json_new(GCD_JSON_BOOL); j->u.boolean = TRUE;  return j; } break;
    case 'f': if (parse_literal(ps, "false")) { GcdJson *j = json_new(GCD_JSON_BOOL); j->u.boolean = FALSE; return j; } break;
    case 'n': if (parse_literal(ps, "null"))  return json_new(GCD_JSON_NULL); break;
    default:  return parse_number(ps);
  }
  parse_fail(ps, "invalid literal");
  return NULL;
}

GcdJson *gcd_json_parse(const gchar *text, gssize len, GError **error)
{
  Parser ps;
  GcdJson *root;
  g_return_val_if_fail(text != NULL, NULL);
  if (len < 0) len = strlen(text);
  ps.p = text;
  ps.end = text + len;
  ps.error = error;
  root = parse_value(&ps);
  if (!root) return NULL;
  if (!at_end(&ps)) {
    gcd_json_free(root);
    parse_fail(&ps, "trailing data after value");
    return NULL;
  }
  return root;
}

/* ============================================================= free/get */

void gcd_json_free(GcdJson *j)
{
  if (!j) return;
  switch (j->type) {
    case GCD_JSON_STR: g_free(j->u.str); break;
    case GCD_JSON_ARR: g_ptr_array_unref(j->u.arr); break;
    case GCD_JSON_OBJ:
      g_hash_table_unref(j->u.obj);
      g_ptr_array_unref(j->keys);
      break;
    default: break;
  }
  g_free(j);
}

GcdJsonType gcd_json_type(const GcdJson *j)
{ return j ? j->type : GCD_JSON_NULL; }

gboolean gcd_json_bool(const GcdJson *j)
{ return j && j->type == GCD_JSON_BOOL ? j->u.boolean : FALSE; }

gdouble gcd_json_num(const GcdJson *j)
{ return j && j->type == GCD_JSON_NUM ? j->u.num : 0.0; }

const gchar *gcd_json_str(const GcdJson *j)
{ return j && j->type == GCD_JSON_STR ? j->u.str : NULL; }

const GcdJson *gcd_json_obj_get(const GcdJson *j, const gchar *key)
{
  if (!j || j->type != GCD_JSON_OBJ) return NULL;
  return g_hash_table_lookup(j->u.obj, key);
}

const gchar *gcd_json_obj_str(const GcdJson *j, const gchar *key)
{ return gcd_json_str(gcd_json_obj_get(j, key)); }

gdouble gcd_json_obj_num(const GcdJson *j, const gchar *key, gdouble fb)
{
  const GcdJson *v = gcd_json_obj_get(j, key);
  return v && v->type == GCD_JSON_NUM ? v->u.num : fb;
}

gboolean gcd_json_obj_bool(const GcdJson *j, const gchar *key, gboolean fb)
{
  const GcdJson *v = gcd_json_obj_get(j, key);
  return v && v->type == GCD_JSON_BOOL ? v->u.boolean : fb;
}

gchar **gcd_json_obj_keys(const GcdJson *j)
{
  GPtrArray *out;
  if (!j || j->type != GCD_JSON_OBJ) return g_new0(gchar *, 1);
  out = g_ptr_array_new();
  for (guint i = 0; i < j->keys->len; i++)
    g_ptr_array_add(out, g_strdup(g_ptr_array_index(j->keys, i)));
  g_ptr_array_add(out, NULL);
  return (gchar **)g_ptr_array_free(out, FALSE);
}

gsize gcd_json_arr_len(const GcdJson *j)
{ return j && j->type == GCD_JSON_ARR ? j->u.arr->len : 0; }

const GcdJson *gcd_json_arr_get(const GcdJson *j, gsize i)
{
  if (!j || j->type != GCD_JSON_ARR || i >= j->u.arr->len) return NULL;
  return g_ptr_array_index(j->u.arr, i);
}

/* ============================================================= builder */

GcdJson *gcd_json_obj_new(void)
{
  GcdJson *j = json_new(GCD_JSON_OBJ);
  j->u.obj = g_hash_table_new_full(g_str_hash, g_str_equal,
                                   g_free, (GDestroyNotify)gcd_json_free);
  j->keys = g_ptr_array_new_with_free_func(g_free);
  return j;
}

GcdJson *gcd_json_arr_new(void)
{
  GcdJson *j = json_new(GCD_JSON_ARR);
  j->u.arr = g_ptr_array_new_with_free_func((GDestroyNotify)gcd_json_free);
  return j;
}

GcdJson *gcd_json_str_new(const gchar *s)
{
  GcdJson *j = json_new(GCD_JSON_STR);
  j->u.str = g_strdup(s ? s : "");
  return j;
}

GcdJson *gcd_json_num_new(gdouble n)
{ GcdJson *j = json_new(GCD_JSON_NUM); j->u.num = n; return j; }

GcdJson *gcd_json_bool_new(gboolean b)
{ GcdJson *j = json_new(GCD_JSON_BOOL); j->u.boolean = b; return j; }

void gcd_json_obj_set(GcdJson *obj, const gchar *key, GcdJson *child)
{
  g_return_if_fail(obj && obj->type == GCD_JSON_OBJ && key);
  if (!child) child = json_new(GCD_JSON_NULL);
  if (g_hash_table_contains(obj->u.obj, key))
    g_hash_table_replace(obj->u.obj, g_strdup(key), child);
  else {
    g_hash_table_insert(obj->u.obj, g_strdup(key), child);
    g_ptr_array_add(obj->keys, g_strdup(key));
  }
}

void gcd_json_obj_set_str(GcdJson *o, const gchar *k, const gchar *s)
{ gcd_json_obj_set(o, k, gcd_json_str_new(s)); }

void gcd_json_obj_set_num(GcdJson *o, const gchar *k, gdouble n)
{ gcd_json_obj_set(o, k, gcd_json_num_new(n)); }

void gcd_json_obj_set_bool(GcdJson *o, const gchar *k, gboolean b)
{ gcd_json_obj_set(o, k, gcd_json_bool_new(b)); }

void gcd_json_arr_add(GcdJson *arr, GcdJson *child)
{
  g_return_if_fail(arr && arr->type == GCD_JSON_ARR);
  if (!child) child = json_new(GCD_JSON_NULL);
  g_ptr_array_add(arr->u.arr, child);
}

/* ======================================================== serializer */

static void emit_string(GString *out, const gchar *s)
{
  g_string_append_c(out, '"');
  for (const gchar *p = s; *p; p++) {
    guchar c = (guchar)*p;
    switch (c) {
      case '"':  g_string_append(out, "\\\""); break;
      case '\\': g_string_append(out, "\\\\"); break;
      case '\b': g_string_append(out, "\\b");  break;
      case '\f': g_string_append(out, "\\f");  break;
      case '\n': g_string_append(out, "\\n");  break;
      case '\r': g_string_append(out, "\\r");  break;
      case '\t': g_string_append(out, "\\t");  break;
      default:
        if (c < 0x20)
          g_string_append_printf(out, "\\u%04x", c);
        else
          g_string_append_c(out, (gchar)c);
    }
  }
  g_string_append_c(out, '"');
}

static void emit_value(GString *out, const GcdJson *j)
{
  if (!j) { g_string_append(out, "null"); return; }
  switch (j->type) {
    case GCD_JSON_NULL: g_string_append(out, "null"); break;
    case GCD_JSON_BOOL: g_string_append(out, j->u.boolean ? "true" : "false"); break;
    case GCD_JSON_NUM: {
      if (j->u.num == (gdouble)(gint64)j->u.num &&
          j->u.num < 9.0e15 && j->u.num > -9.0e15)
        g_string_append_printf(out, "%" G_GINT64_FORMAT, (gint64)j->u.num);
      else
        g_string_append_printf(out, "%.17g", j->u.num);
      break;
    }
    case GCD_JSON_STR: emit_string(out, j->u.str); break;
    case GCD_JSON_ARR:
      g_string_append_c(out, '[');
      for (guint i = 0; i < j->u.arr->len; i++) {
        if (i) g_string_append_c(out, ',');
        emit_value(out, g_ptr_array_index(j->u.arr, i));
      }
      g_string_append_c(out, ']');
      break;
    case GCD_JSON_OBJ: {
      g_string_append_c(out, '{');
      for (guint i = 0; i < j->keys->len; i++) {
        const gchar *k = g_ptr_array_index(j->keys, i);
        if (i) g_string_append_c(out, ',');
        emit_string(out, k);
        g_string_append_c(out, ':');
        emit_value(out, g_hash_table_lookup(j->u.obj, k));
      }
      g_string_append_c(out, '}');
      break;
    }
  }
}

gchar *gcd_json_to_string(const GcdJson *j)
{
  GString *out = g_string_new(NULL);
  emit_value(out, j);
  return g_string_free(out, FALSE);
}
