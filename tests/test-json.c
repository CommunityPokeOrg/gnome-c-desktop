/* test-json.c — JSON parser/serializer round-trips. MIT. */
#include "gcd/gcd-json.h"

static void test_scalar(void)
{
  GcdJson *j = gcd_json_parse("42", -1, NULL);
  g_assert_nonnull(j);
  g_assert_cmpint(gcd_json_type(j), ==, GCD_JSON_NUM);
  g_assert_cmpfloat(gcd_json_num(j), ==, 42.0);
  gcd_json_free(j);

  j = gcd_json_parse("\"hi\\nthere\"", -1, NULL);
  g_assert_cmpstr(gcd_json_str(j), ==, "hi\nthere");
  gcd_json_free(j);

  j = gcd_json_parse("true", -1, NULL);
  g_assert_true(gcd_json_bool(j));
  gcd_json_free(j);

  j = gcd_json_parse("[1,2,3]", -1, NULL);
  g_assert_cmpint(gcd_json_arr_len(j), ==, 3);
  g_assert_cmpfloat(gcd_json_num(gcd_json_arr_get(j, 2)), ==, 3.0);
  gcd_json_free(j);
}

static void test_object(void)
{
  GcdJson *j = gcd_json_parse(
      "{\"a\":1,\"b\":\"x\",\"c\":{\"d\":[true,null]}}", -1, NULL);
  g_assert_nonnull(j);
  g_assert_cmpfloat(gcd_json_obj_num(j, "a", -1), ==, 1.0);
  g_assert_cmpstr(gcd_json_obj_str(j, "b"), ==, "x");
  g_assert_cmpfloat(gcd_json_obj_num(j, "missing", -7), ==, -7.0);
  gcd_json_free(j);
}

static void test_roundtrip(void)
{
  GcdJson *o = gcd_json_obj_new();
  gcd_json_obj_set_str(o, "name", "w1");
  gcd_json_obj_set_num(o, "n", 12);
  gcd_json_obj_set_bool(o, "ok", TRUE);
  GcdJson *a = gcd_json_arr_new();
  gcd_json_arr_add(a, gcd_json_str_new("x"));
  gcd_json_arr_add(a, gcd_json_num_new(2.5));
  gcd_json_obj_set(o, "arr", a);
  gchar *s = gcd_json_to_string(o);
  gcd_json_free(o);

  GcdJson *r = gcd_json_parse(s, -1, NULL);
  g_assert_nonnull(r);
  g_assert_cmpstr(gcd_json_obj_str(r, "name"), ==, "w1");
  g_assert_cmpfloat(gcd_json_obj_num(r, "n", 0), ==, 12.0);
  g_assert_true(gcd_json_obj_bool(r, "ok", FALSE));
  g_assert_cmpint(gcd_json_arr_len(gcd_json_obj_get(r, "arr")), ==, 2);
  gcd_json_free(r);
  g_free(s);
}

static void test_bad_input(void)
{
  GError *e = NULL;
  GcdJson *j = gcd_json_parse("{bad", -1, &e);
  g_assert_null(j);
  g_assert_nonnull(e);
  g_clear_error(&e);
  g_assert_null(gcd_json_parse("{\"a\":1} trailing", -1, &e));
  g_clear_error(&e);
  /* unicode escape */
  j = gcd_json_parse("\"\\u0041\\u00e9\"", -1, NULL);
  g_assert_cmpstr(gcd_json_str(j), ==, "A\xc3\xa9");
  gcd_json_free(j);
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/json/scalar", test_scalar);
  g_test_add_func("/json/object", test_object);
  g_test_add_func("/json/roundtrip", test_roundtrip);
  g_test_add_func("/json/bad-input", test_bad_input);
  return g_test_run();
}
