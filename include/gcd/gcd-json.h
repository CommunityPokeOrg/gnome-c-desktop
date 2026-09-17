/*
 * gcd-json.h — minimal JSON DOM: parser + builder.
 *
 * Self-contained (no json-glib dependency) so the sync codec works
 * everywhere the shell builds. Ownership is tree-shaped: children are
 * owned by their parent; freeing the root frees everything.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  GCD_JSON_NULL = 0,
  GCD_JSON_BOOL,
  GCD_JSON_NUM,
  GCD_JSON_STR,
  GCD_JSON_ARR,
  GCD_JSON_OBJ,
} GcdJsonType;

typedef struct GcdJson GcdJson;

/* ------------------------------------------------------------- parser */

/* Parse `len` bytes (NUL-terminated text is fine, pass -1). */
GcdJson *gcd_json_parse (const gchar *text, gssize len, GError **error);
void     gcd_json_free  (GcdJson     *json);

GcdJsonType gcd_json_type   (const GcdJson *json);
gboolean    gcd_json_bool   (const GcdJson *json);  /* FALSE unless BOOL */
gdouble     gcd_json_num    (const GcdJson *json);  /* 0 unless NUM */
const gchar *gcd_json_str   (const GcdJson *json);  /* NULL unless STR */

/* object accessors (NULL when not an object / key absent) */
const GcdJson *gcd_json_obj_get (const GcdJson *json, const gchar *key);
const gchar   *gcd_json_obj_str (const GcdJson *json, const gchar *key);
gdouble        gcd_json_obj_num (const GcdJson *json, const gchar *key,
                                 gdouble fallback);
gboolean       gcd_json_obj_bool(const GcdJson *json, const gchar *key,
                                 gboolean fallback);
/* Returns NULL-terminated array of member keys (g_free). */
gchar        **gcd_json_obj_keys(const GcdJson *json);

/* array accessors */
gsize         gcd_json_arr_len (const GcdJson *json);
const GcdJson *gcd_json_arr_get(const GcdJson *json, gsize index);

/* ------------------------------------------------------------- builder */

GcdJson *gcd_json_obj_new   (void);
GcdJson *gcd_json_arr_new   (void);
GcdJson *gcd_json_str_new   (const gchar *str);
GcdJson *gcd_json_num_new   (gdouble num);
GcdJson *gcd_json_bool_new  (gboolean value);

/* Take ownership of `child`. `obj_set_*` create the value inline. */
void gcd_json_obj_set     (GcdJson *obj, const gchar *key, GcdJson *child);
void gcd_json_obj_set_str (GcdJson *obj, const gchar *key, const gchar *str);
void gcd_json_obj_set_num (GcdJson *obj, const gchar *key, gdouble num);
void gcd_json_obj_set_bool(GcdJson *obj, const gchar *key, gboolean value);
void gcd_json_arr_add     (GcdJson *arr, GcdJson *child);

/* Compact serialization (no insignificant whitespace). */
gchar *gcd_json_to_string (const GcdJson *json);

G_END_DECLS
