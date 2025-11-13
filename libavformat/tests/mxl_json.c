/*
 * MXL JSON parser tests for Media eXchange Layer flows
 *
 * Copyright (c) 2025 CBC/Radio-Canada
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, see <https://www.gnu.org/licenses/>.
 */

#include "libavformat/mxl_json.h"
#include "libavutil/avassert.h"
#include "libavutil/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <locale.h>

static const char* token_type_name(const jsmntok_t* tok) {

    switch(tok->type) {
    case JSMN_OBJECT:
        return "objc";
    case JSMN_ARRAY:
        return "arry";
    case JSMN_STRING:
        return "strn";
    case JSMN_PRIMITIVE:
        return "prim";
    default:
        return "unkn";
    }
}

static void mxl_json_dump_doc(mxl_json_doc *doc)
{
    if (!doc->json) {
        printf("no json present in doc");
        return;
    }

    // the original string
    printf("%s\n", doc->json);

    // the parsed tokens
    printf("%3s: %8s %3s %3s %3s\n", "idx", "type", "str", "end", "sz");
    for (int i = 0; i < doc->ntok; i++) {
        jsmntok_t *tok = &doc->tokens[i];

        const char* typename = token_type_name(tok);
        int size = tok->end - tok->start + 1;
        av_assert0(size > 0);
        char* value = malloc(size);
        av_assert0(value);
        memcpy(value, doc->json + tok->start, size-1);
        value[size-1] = 0;

        printf("%03d: %8s %03d %03d %03d \'%s\'\n",
               i, typename, tok->start, tok->end, tok->size, value);

        free(value);
    }
}

static const char* err_to_str(int err)
{
    switch(err) {
    case MXL_JSON_EBADTYPE:
        return "MXL_JSON_EBADTYPE";
    case MXL_JSON_ENOTFOUND:
        return "MXL_JSON_ENOTFOUND";
    case MXL_JSON_NULL:
        return "MXL_JSON_NULL";
    case MXL_JSON_MALFORMED:
        return "MXL_JSON_MALFORMED";
    case MXL_JSON_UNSUPPORTED:
        return "MXL_JSON_UNSUPPORTED";
    case MXL_JSON_EINTERNAL:
        return "MXL_JSON_EINTERNAL";
    case ERANGE:
        return "ERANGE";
    case AVERROR(ENOMEM):
        return "AVERROR(ENOMEM)";
    case AVERROR(EIO):
        return "AVERROR(EIO)";
    case AVERROR(EINVAL):
        return "AVERROR(EINVAL)";
    case AVERROR_INVALIDDATA:
        return "AVERROR_INVALIDDATA";
    case 0:
        return "SUCCESS";
    default:
        return "unknown";
    }
}

static char* args_to_str(int depth, va_list ap) {

    const char sep = '.';
    const char* null = "NULL";

    // start with len 1 for nul
    size_t total_len = 1;

    // copy va list and traverse to compute length
    va_list ap2;
    va_copy(ap2, ap);
    for (int i = 0; i < depth; i++) {
        const char *s = va_arg(ap2, const char *);
        total_len += (s ? strlen(s) : strlen(null));
        if (i < depth - 1)
            total_len += 1; // comma
    }
    va_end(ap2);

    char *result = malloc(total_len);
    if (!result)
        return NULL;

    // second pass: build the string
    char *p = result;
    for (int i = 0; i < depth; i++) {
        const char *s = va_arg(ap, const char *);
        const char *part = s ? s : null;
        size_t len = strlen(part);
        memcpy(p, part, len);
        p += len;
        if (i < depth - 1)
            *p++ = sep;
    }
    *p = '\0';

    return result;
}

static const char *null_str(const char* s) {
    return s == NULL ? "(null)" : s;
}

static int verify_json_string(const mxl_json_doc *doc,
                              const char *expected_value,
                              int expected_err,
                              int depth, ...)
{
    va_list ap;

    va_start(ap, depth);
    char* args_as_str = args_to_str(depth, ap);
    va_end(ap);

    va_start(ap, depth);
    mxl_json_str str = mxl_json_doc_get_stringv(doc, depth, ap);
    va_end(ap);

    int rc = -1;

    // first: trivial error equality
    if (expected_err != str.err) {
        printf("FAIL \"%s\" expected error %s (actual %s)\n",
               args_as_str, err_to_str(expected_err),  err_to_str(str.err));
        rc = 1;
        goto finally;
    }

    // second: error aware string value compare
    if (str.err != 0 && str.value != NULL) {
        printf("FAIL \"%s\" string non-null value on error\n",
                args_as_str);
            rc = 1;
            goto finally;
    }
    // else if no error then str.value must match expected
    else if (str.err == 0 && strcmp(str.value, expected_value) != 0) {
        printf("FAIL: \"%s\" str \"%s\" expected \"%s\"\n",
                args_as_str,
                str.value ? str.value : "(null)", expected_value);
        rc = 1;
        goto finally;
    }
    // else no failures, test passes
    else {
        rc = 0;
        printf("PASS \"%s\" = {\"%s\", %s}\n", args_as_str, null_str(str.value),
               err_to_str(str.err));
    }

finally:
    free(args_as_str);
    free(str.value);

    return rc;
}

// The math.h signbit macro is not working with ffmpeg compile flags,
// this is a compile flag independent alternative.
static inline int is_neg_zero(double x) {
    uint64_t u;
    memcpy(&u, &x, sizeof u);
    return u == 0x8000000000000000ULL; // IEEE-754 -0.0 value
}

// compare two doubles with adapted tolerance
static int dblcmp(double a, double b) {

    // must never attempt to compare NaN
    av_assert0(!isnan(a) && !isnan(b));

    // infinity compare
    if (isinf(a) || isinf(b)) {
        if (a == b)
            return 0;
        else
            return (a < b) ? -1 : 1;
    }

    // zero compare
    if (a == 0.0 && b == 0.0) {
        if (is_neg_zero(a) && !is_neg_zero(b))
            return -1;
        else if (!is_neg_zero(a) && is_neg_zero(b))
            return  1;
        else
            return 0;
    }

    // compute relative tolerance
    double diff = fabs(a - b);
    double tol  = DBL_EPSILON * fmax(fabs(a), fabs(b));

    // comparable if within tolerance
    if (diff <= tol)
        return 0;
    else
        return (a < b) ? -1 : 1;
}

static int verify_json_double(const mxl_json_doc *doc,
                              double expected_value,
                              int expected_err,
                              int depth, ...)
{
    va_list ap;

    va_start(ap, depth);
    char* args_as_str = args_to_str(depth, ap);
    va_end(ap);

    va_start(ap, depth);
    mxl_json_num dbl = mxl_json_doc_get_doublev(doc, depth, ap);
    va_end(ap);

    int rc = -1;

    // first: trivial error equality
    if (expected_err != dbl.err) {
        printf("FAIL \"%s\" expected error %s (actual %s)\n",
               args_as_str, err_to_str(expected_err),  err_to_str(dbl.err));
        rc = 1;
        goto finally;
    }

    // second: NaN aware value compare
    bool both_nan_or_both_compare = 0;
    if (isnan(expected_value) || isnan(dbl.value))
        both_nan_or_both_compare = isnan(expected_value) && isnan(dbl.value);
    else
        both_nan_or_both_compare = !dblcmp(expected_value, dbl.value);

    if (both_nan_or_both_compare) {
        printf("PASS \"%s\" = {%.17g, %s}\n",
               args_as_str, dbl.value, err_to_str(dbl.err));
        rc = 0;
    }
    else {
        printf("FAIL \"%s\" expected value %.17g (actual %.17g)\n",
               args_as_str, expected_value, dbl.value);
        rc = 1;
    }

finally:
    free(args_as_str);

    return rc;
}

//
// sanity test
//

static const char sanity_test_json[] =
    "{" \
    "\"array_a\": [0,1,2,3]," \
    "\"object_a\": {\"a\": 1, \"b\": [0,1,2,3], \"c\": {\"x\": \"y\"} }," \
    "\"grain_rate\": {\"numerator\": 25, \"denominator\": 1},"          \
    "\"format\": {\"name\": \"video\"}," \
    "\"media_type\": \"video/v210\"," \
    "\"frame_width\": 1920," \
    "\"array_b\": [{\"m\": n, \"o\": [4,5,6]}, \"p\", [\"q\",7,{\"r\":8}]]," \
    "\"object_b\": {\"s\": [9,\"t\",true], \"u\": {\"v\":{\"w\":10}}}," \
    "\"a_null\": null," \
    "\"a_true\": true," \
    "\"a_false\": false," \
    "\"array_c\": [0, \"one\", {\"three\": \"four\"}]," \
    "\"negnums\": {\"a\": -1.23, \"b\": -.123}," \
    "\"expnums\": {\"a\": 1.23e2, \"b\": 1.23e+2, \"c\": 1.23e-2}," \
    "\"object_c\": {\"last\": \"final\"}," \
    "\"ctrl_char\": {\"text\":\"line1\\nline2\"}"
    "}";

static int sanity_test(void) {

#define S1(VAL, ERR, P1) \
    rc = verify_json_string(&doc, VAL, ERR, 1, P1); \
    if (rc) goto finally;
#define S2(VAL, ERR, P1, P2) \
    rc = verify_json_string(&doc, VAL, ERR, 2, P1, P2); \
    if (rc) goto finally;

#define D1(VAL, ERR, P1) \
    rc = verify_json_double(&doc, VAL, ERR, 1, P1); \
    if (rc) goto finally;
#define D2(VAL, ERR, P1, P2) \
    rc = verify_json_double(&doc, VAL, ERR, 2, P1, P2); \
    if (rc) goto finally;
#define D4(VAL, ERR, P1, P2, P3, P4) \
    rc = verify_json_double(&doc, VAL, ERR, 4, P1, P2, P3, P4); \
    if (rc) goto finally;

    printf("\nsanity test:\n");

    mxl_json_doc doc;
    int rc = -1;

    if (mxl_json_doc_build(sanity_test_json, &doc) < 0) {
        printf("json_doc_build failed\n");
        rc = 1;
        goto finally;
    }

    mxl_json_dump_doc(&doc);

    // happy paths
    S1("video/v210", 0, "media_type");
    S2("video", 0, "format", "name");
    S2("final", 0, "object_c", "last");
    D1(1920, 0, "frame_width");
    D2(25, 0, "grain_rate", "numerator");
    D2(1, 0,  "grain_rate", "denominator");
    D2(1, 0, "object_a", "a");
    D4(10, 0, "object_b", "u", "v", "w");
    D2(-1.23, 0, "negnums", "a");
    D2(-0.123, 0, "negnums", "b");
    D2(1.23e2, 0, "expnums", "a");
    D2(1.23e+2, 0, "expnums", "b");
    D2(1.23e-2, 0, "expnums", "c");
    S2("line1\\nline2", 0, "ctrl_char", "text");

    // impossible null primitive conversion
    D1(NAN, MXL_JSON_NULL, "a_null");
    S1(NULL, MXL_JSON_NULL, "a_null");

    // error: impossible null primitive conversion
    D1(NAN, MXL_JSON_EBADTYPE, "a_true");
    D1(NAN, MXL_JSON_EBADTYPE, "a_false");
    S1(NULL, MXL_JSON_EBADTYPE, "a_false");
    S1(NULL, MXL_JSON_EBADTYPE, "a_true");

    // error: bad type access to supported type
    D2(NAN, MXL_JSON_EBADTYPE, "format", "name");
    S2(NULL, MXL_JSON_EBADTYPE, "grain_rate", "numerator");

    // error: bad type access to unsupported array type
    D1(NAN, MXL_JSON_EBADTYPE, "array_a");
    S1(NULL, MXL_JSON_EBADTYPE, "array_b");

    // error: path not found
    D1(NAN, MXL_JSON_ENOTFOUND, "does_not_exist");
    D2(NAN, MXL_JSON_ENOTFOUND, "does", "not_exist");
    S1(NULL, MXL_JSON_ENOTFOUND, "blah");
    S2(NULL, MXL_JSON_ENOTFOUND, "blah", "yadda");

finally:
    mxl_json_doc_release(&doc);

    if (doc.json || doc.tokens || doc.ntok) {
        printf("FAIL mxl_json_doc_release did not clear structure values\n");
        rc = 1;
    }

    return rc;

#undef D1
#undef D2
#undef D4
#undef S1
#undef S2
}

//
// abberant input tests
//

static const char invalid_empty[] = "";
static const char valid_empty_object[] = "{}";
static const char valid_empty_array[] = "[]";
static const char valid_empty_string[] = "\"\"";
static const char valid_single_number[] = "123.346";
static const char valid_single_neg_number[] = "-123.346";
static const char valid_single_exp_number[] = "1.23346e2";
static const char valid_single_true[] = "true";
static const char valid_single_false[] = "false";
static const char valid_single_null[] = "null";
static const char misspelled_single_null[] = "nul";
static const char misspelled_contained_null[] = "{\"misspelled_null\": nul}";
static const char capital_contained_null[] = "{\"capital_null\": NULL}";
static const char invalid_leading_plus[] = "{\"a\": +123.346}";
static const char invalid_exp_number[] = "{\"a\": 1.23346e}";
static const char invalid_number_sep[] = "{\"a\": 1,23346}";
static const char not_json[] = "this is totally wrong";
static const char missing_key_quotes[] = "{foo: \"bar\"}";
static const char single_quotes[] = "{'foo': 'bar'}";
static const char trailing_comma_object[] = "{ \"foo\": \"bar\", }";
static const char trailing_comma_array[] = "[ \"a\", \"b\", ]";
static const char missing_both_braces[] = "\"foo\": \"bar\"";
static const char missing_start_brace[] = "\"foo\": \"bar\"}";
static const char missing_end_brace[] = "{\"foo\": \"bar\"";
static const char dangling_key[] = "{\"foo\":}";
static const char abnormal_termination[] = "{\"foo\": \"bar\"} what's this?";
static const char unescaped_ctl_char[] = "{ \"text\": \"line1\nline2\" }";
static const char unquoted_literal[] = "{\"what\": yes }";
static const char nan_value[] = "{\"value\": NaN }";
static const char inf_values[] = "{\"a\":Infinity,\"b\":-Infinity," \
    "\"c\":Inf, \"d\":-Inf}";
static const char valid_overflow_pos_number[] = "{\"a\": 1e400}";
static const char valid_overflow_neg_number[] = "{\"a\": -1e400}";
static const char valid_underflow_pos_number[] = "{\"a\": 1e-400}";
static const char valid_underflow_neg_number[] = "{\"a\": -1e-400}";

static int test_input_build(const char* json, mxl_json_doc* doc)
{
    int rc = mxl_json_doc_build(json, doc);
    if (rc < 0)
        printf("json_doc_build failed\n");
    return rc;
}

static int abberant_input_tests(void) {

#define TS(VARIANT) \
    { \
        name = #VARIANT; \
        printf("\nSTART abberant test: %s\n", name);  \
        rc = test_input_build(VARIANT, &doc); \
        if (rc) goto finally; \
        mxl_json_dump_doc(&doc); \
    }

#define S1(VAL, ERR, P1) \
    rc = verify_json_string(&doc, VAL, ERR, 1, P1); \
    if (rc) goto finally;

#define D1(VAL, ERR, P1) \
    rc = verify_json_double(&doc, VAL, ERR, 1, P1); \
    if (rc) goto finally;

#define TE(VARIANT) \
    av_assert0(strcmp(name, #VARIANT) == 0); \
    av_assert0(!rc); \
    printf("PASS abberant: %s\n", name); \
    mxl_json_doc_release(&doc);

#define BUILD_AND_ACCESS_TEST(VARIANT) \
    TS(VARIANT); \
    S1(NULL, MXL_JSON_ENOTFOUND, "absent"); \
    D1(NAN, MXL_JSON_ENOTFOUND, "absent");  \
    TE(VARIANT);

#define BUILD_EXPECT_ERROR_TEST(VARIANT, EXPECTED_ERR) \
    { \
        name = #VARIANT; \
        printf("\nSTART abberant test: %s\n", name); \
        rc = test_input_build(VARIANT, &doc); \
        if (rc != EXPECTED_ERR) { \
            printf("FAIL expected error %s (actual %s)\n", \
                   err_to_str(EXPECTED_ERR), err_to_str(rc)); \
            if (!rc) rc = 1; \
            goto finally; \
        } \
        printf("PASS expected build error: %s (%s)\n", err_to_str(rc), err_to_str(rc)); \
        printf("PASS abberant: %s\n", name); \
        rc = 0; \
    }

    printf("\nabberant input tests:\n");

    int rc = -1;
    mxl_json_doc doc = {};
    const char* name;

    // verify that the json parses and can be traversed
    BUILD_AND_ACCESS_TEST(invalid_empty);
    BUILD_AND_ACCESS_TEST(valid_empty_object);
    BUILD_AND_ACCESS_TEST(valid_empty_array);
    BUILD_AND_ACCESS_TEST(valid_empty_string);
    BUILD_AND_ACCESS_TEST(valid_single_number);
    BUILD_AND_ACCESS_TEST(valid_single_neg_number);
    BUILD_AND_ACCESS_TEST(valid_single_exp_number);
    BUILD_AND_ACCESS_TEST(valid_single_true);
    BUILD_AND_ACCESS_TEST(valid_single_false);
    BUILD_AND_ACCESS_TEST(valid_single_null);
    BUILD_AND_ACCESS_TEST(misspelled_single_null);
    BUILD_AND_ACCESS_TEST(invalid_leading_plus);
    BUILD_AND_ACCESS_TEST(invalid_exp_number);
    BUILD_AND_ACCESS_TEST(invalid_number_sep);
    BUILD_AND_ACCESS_TEST(not_json);
    BUILD_AND_ACCESS_TEST(missing_key_quotes);
    BUILD_AND_ACCESS_TEST(single_quotes);
    BUILD_AND_ACCESS_TEST(trailing_comma_object);
    BUILD_AND_ACCESS_TEST(trailing_comma_array);
    BUILD_AND_ACCESS_TEST(dangling_key);
    BUILD_AND_ACCESS_TEST(abnormal_termination);
    BUILD_AND_ACCESS_TEST(unescaped_ctl_char);
    BUILD_AND_ACCESS_TEST(missing_both_braces);

    // verify that the json parse fails
    BUILD_EXPECT_ERROR_TEST(missing_start_brace, AVERROR_INVALIDDATA);
    BUILD_EXPECT_ERROR_TEST(missing_end_brace, AVERROR_INVALIDDATA);


    // verfiy failure accessing incorrect fields

    TS(misspelled_contained_null);
    S1(NULL, MXL_JSON_ENOTFOUND, "absent");
    D1(NAN, MXL_JSON_ENOTFOUND, "absent");
    S1(NULL, MXL_JSON_EBADTYPE, "misspelled_null");
    D1(NAN, MXL_JSON_EBADTYPE, "misspelled_null");
    TE(misspelled_contained_null);

    TS(capital_contained_null);
    S1(NULL, MXL_JSON_ENOTFOUND, "absent");
    D1(NAN, MXL_JSON_ENOTFOUND, "absent");
    S1(NULL, MXL_JSON_EBADTYPE, "capital_null");
    D1(NAN, MXL_JSON_EBADTYPE, "capital_null");
    TE(capital_contained_null);

    TS(unquoted_literal);
    S1(NULL, MXL_JSON_ENOTFOUND, "absent");
    D1(NAN, MXL_JSON_ENOTFOUND, "absent");
    S1(NULL, MXL_JSON_EBADTYPE, "what");
    D1(NAN, MXL_JSON_EBADTYPE, "what");
    TE(unquoted_literal);

    TS(nan_value);
    S1(NULL, MXL_JSON_ENOTFOUND, "absent");
    D1(NAN, MXL_JSON_ENOTFOUND, "absent");
    S1(NULL, MXL_JSON_EBADTYPE, "value");
    D1(NAN, MXL_JSON_EBADTYPE, "value");
    TE(nan_value);

    TS(inf_values);
    D1(INFINITY, MXL_JSON_EBADTYPE, "a");
    D1(-INFINITY, MXL_JSON_EBADTYPE, "b");
    D1(INFINITY, MXL_JSON_EBADTYPE, "c");
    D1(-INFINITY, MXL_JSON_EBADTYPE, "d");
    TE(inf_values);

    TS(valid_overflow_pos_number);
    D1(HUGE_VAL, ERANGE, "a");
    TE(valid_overflow_pos_number);

    TS(valid_overflow_neg_number);
    D1(-HUGE_VAL, ERANGE, "a");
    TE(valid_overflow_neg_number);

    TS(valid_underflow_pos_number);
    D1(0.0, ERANGE, "a");
    TE(valid_underflow_pos_number);

    TS(valid_underflow_neg_number);
    D1(-0.0, ERANGE, "a");
    TE(valid_underflow_neg_number);

finally:

    return rc;

#undef BUILD_AND_ACCESS_TEST
#undef BUILD_EXPECT_ERROR_TEST
#undef TS
#undef TE
#undef S1
#undef D1
}


//
// non "C" locale tests: errant non "C" locale numbers in json string
//

enum expect_what {
    MALFORMED,
    NOPARSE
};

typedef struct bad_number_case {
    const char *num;
    const char *name;
    enum expect_what expect;
} bad_number_case;

static const bad_number_case bad_number_cases[] = {
    { "3,14",        "comma_dec_a", MALFORMED},
    { "0,001",       "comma_dec_b", MALFORMED },

    { "1.234,56",    "dot_thou_a", MALFORMED  },
    { "12.345,67",   "dot_thou_b", MALFORMED  },

    { "1,234.56",    "comma_thou_a", MALFORMED},
    { "12,345.67",   "comma_thou_b", MALFORMED},

    { "1 234,56",    "space_thou_a", MALFORMED},
    { "1 234.56",    "space_thou_b", MALFORMED},

    { "1" "\xC2\xA0" "234,56", "nbsp_thou_a", NOPARSE },
    { "1" "\xC2\xA0" "234.56", "nbsp_thou_b", NOPARSE },

    { "1" "\xE2\x80\x89" "234,56", "thin_thou_a", NOPARSE },
    { "1" "\xE2\x80\x89" "234.56", "thin_thou_b", NOPARSE },

    { "1'234,56",    "swiss_thou_a", MALFORMED },
    { "1'234.56",    "swiss_thou_b", MALFORMED },

    { "\xD9\xA3" "\xDB\x8B" "\xD9\xA1" "\xD9\xA4", "arabic_314", NOPARSE },
    { "\xD9\xA1" "\xD9\xAC" "\xD9\xA2" "\xD9\xA3" "\xD9\xA4" "\xDB\x8B"
      "\xD9\xA5" "\xD9\xA6", "arabic_1234", NOPARSE },
};

static int test_bad_number_case(const bad_number_case* cse) {
    printf("test %s\n", cse->name);

    mxl_json_doc doc = {0};
    char json[MXL_JSON_MAX_NUMBER_CHARS+1] = {0};

    int rc = snprintf(json, sizeof(json), "{ \"%s\": %s }",
                      cse->name, cse->num);
    if (rc < 0 || (size_t)rc >= sizeof(json))
        goto finally;

    rc = mxl_json_doc_build(json, &doc);

    if (!rc)
        mxl_json_dump_doc(&doc);

    if (NOPARSE == cse->expect)
        if (rc == 0) {
            printf("FAIL expected NOPARSE but parse succeeded\n");
            rc = 1;
            goto finally;
        }
        else {
            printf("PASS expected NOPARSE and parse did not succeed\n");
            rc = 0;
            goto finally;
        }

    if (MALFORMED == cse->expect) {
        av_assert0(!rc);
        mxl_json_num num = mxl_json_doc_get_double1(&doc, cse->name);

        if (num.err != MXL_JSON_MALFORMED) {
            printf("FAIL expected MXL_JSON_MALFORMED error (actual %s)\n",
                   err_to_str(num.err));
            rc = 1;
            goto finally;
        }
        else {
            printf("PASS %s identified as malformed\n", cse->name);
            rc = 0;
            goto finally;
        }
    }

finally:
    if (!rc)
        mxl_json_doc_release(&doc);

    return rc;
}

static int non_c_locale_tests(void)
{
    printf("\nnon C locale number tests:\n");

    int rc = -1;

    for (int i = 0; i < sizeof(bad_number_cases)/sizeof(bad_number_case); i++) {
        rc = test_bad_number_case(&bad_number_cases[i]);
        if (rc) goto finally;
    }

finally:
    return rc;
}


//
// real world MXL json string test
//

const char *real_world_mxl_json_str =
"{\n"
"  \"description\": \"MXL Test File\",\n"
"  \"id\": \"5fbec3b1-1b0f-417d-9059-8b94a47197ef\",\n"
"  \"tags\": {},\n"
"  \"format\": \"urn:x-nmos:format:video\",\n"
"  \"label\": \"MXL Test File\",\n"
"  \"parents\": [],\n"
"  \"media_type\": \"video/v210\",\n"
"  \"grain_rate\": {\n"
"    \"numerator\": 50,\n"
"    \"denominator\": 1\n"
"  },\n"
"  \"frame_width\": 1920,\n"
"  \"frame_height\": 1080,\n"
"  \"colorspace\": \"BT709\",\n"
"  \"components\": [\n"
"    {\n"
"      \"name\": \"Y\",\n"
"      \"width\": 1920,\n"
"      \"height\": 1080,\n"
"      \"bit_depth\": 10\n"
"    },\n"
"    {\n"
"      \"name\": \"Cb\",\n"
"      \"width\": 960,\n"
"      \"height\": 1080,\n"
"      \"bit_depth\": 10\n"
"    },\n"
"    {\n"
"      \"name\": \"Cr\",\n"
"      \"width\": 960,\n"
"      \"height\": 1080,\n"
"      \"bit_depth\": 10\n"
"    }\n"
"  ]\n"
"}\n";

static int check_real_world_values(const mxl_json_doc *doc)
{
#define S1(VAL, ERR, P1)                            \
    rc = verify_json_string(doc, VAL, ERR, 1, P1); \
    if (rc) goto finally;
#define S2(VAL, ERR, P1, P2) \
    rc = verify_json_string(doc, VAL, ERR, 2, P1, P2); \
    if (rc) goto finally;
#define D1(VAL, ERR, P1) \
    rc = verify_json_double(doc, VAL, ERR, 1, P1); \
    if (rc) goto finally;
#define D2(VAL, ERR, P1, P2) \
    rc = verify_json_double(doc, VAL, ERR, 2, P1, P2); \
    if (rc) goto finally;

    int rc = -1;

    S1("5fbec3b1-1b0f-417d-9059-8b94a47197ef", 0, "id");
    S1( "MXL Test File", 0, "label");
    S1("video/v210", 0, "media_type");
    S1("urn:x-nmos:format:video", 0, "format");
    S1("BT709", 0, "colorspace");
    D2(50, 0, "grain_rate", "numerator");
    D2(1, 0, "grain_rate", "denominator");
    D1(1920, 0, "frame_width");
    D1(1080, 0, "frame_height");

finally:
    return rc;
#undef S1
#undef S2
#undef D1
#undef D2
}


static int real_world_mxl_json_test(void) {

    printf("\nreal world MXL test:\n");

    mxl_json_doc doc;
    int rc = -1;

    if (mxl_json_doc_build(real_world_mxl_json_str, &doc) < 0) {
        printf("json_doc_build failed\n");
        rc = 1;
        goto finally;
    }

    mxl_json_dump_doc(&doc);

    rc = check_real_world_values(&doc);

finally:
    mxl_json_doc_release(&doc);

    return rc;
}

//
// token capacity tests
//

static int token_capacity_test(const unsigned int initial_token_capacity,
                               const unsigned int max_token_capacity,
                               const int expected_error,
                               const char *name)
{
    mxl_json_doc doc;
    int exit_status = -1;

    printf("parse with token capacity initial=%u max=%u\n", initial_token_capacity, max_token_capacity);

    int rc = mxl_json_doc_build2(real_world_mxl_json_str, &doc,
                             initial_token_capacity, max_token_capacity);
    if (expected_error != rc) {
        printf("FAIL json_doc_build status \"%s\"\n", name);
        exit_status = 1;
        goto finally;
    }

    // expected error and got error?
    if (expected_error != 0 && rc != 0) {
        printf("PASS expected build error %s (%d)\n", err_to_str(expected_error), expected_error);
        exit_status = 0;
        goto finally;
    }

    mxl_json_dump_doc(&doc);

    rc = check_real_world_values(&doc);
    if (rc) {
        printf("FAIL token capacity \"%s\"\n", name);
        exit_status = 1;
        goto finally ;
    }

    exit_status = 0;

finally:
    mxl_json_doc_release(&doc);

    if (0 == exit_status)
        printf("PASS token capacity \"%s\"\n", name);

    return exit_status;
}

static int token_capacity_tests(void)
{
    printf("\ntoken_capacity test:\n");

    int rc = -1;
    const unsigned int max_cap_def = MXL_JSON_TOKEN_CAPACITY_MAX_DEFAULT;

    // initial token capcity tests
    const unsigned int low_non_zero = 2;
    if (low_non_zero > MXL_JSON_INITIAL_TOKEN_CAPACITY_DEFAULT) {
        printf("FAIL MXL_JSON_INITIAL_TOKEN_CAPACITY_DEFAULT low_non_zero test mismatch\n");
        rc = 1;
        goto finally;
    }

    rc = token_capacity_test(low_non_zero, max_cap_def, 0, "low non zero initial capacity");
    if (rc) goto finally;

    rc = token_capacity_test(1, 2, AVERROR(ENOMEM), "exceed max capacity");
    if (rc) goto finally;

    rc = token_capacity_test(2, 2, AVERROR(ENOMEM), "initial equal to max");
    if (rc) goto finally;

    rc = token_capacity_test(0, max_cap_def,  AVERROR(EINVAL), "zero initial capacity");
    if (rc) goto finally;

    rc = token_capacity_test(2, 1, AVERROR(EINVAL), "initial greater than max");
    if (rc) goto finally;

        rc = token_capacity_test(1, 0, AVERROR(EINVAL), "zero max capacity");
    if (rc) goto finally;


finally:
    return rc;
}

//
// limit tests: validate implementation limits
//

static const char number_over_conversion_limit[] = "{\"too_long\":"
   //0       1         2         3         4          5
   //12345678901234567890123456789012345678901234567890
    "11111111111111111111111111111111111111111111111111"
    "11111111111111111111111111111111111111111111111111"
    "1111111111111111111111111111}";

static const char number_under_conversion_limit[] = "{\"not_too_long\":"
   //0        1         2         3         4         5
   //12345678901234567890123456789012345678901234567890
    "11111111111111111111111111111111111111111111111111"
    "11111111111111111111111111111111111111111111111111"
    "111111111111111111111111111}";

static const char object_depth_over_limit[] =
    "{\"accessible_a\": 1,"
     "\"too_deep\":" "{\"a\":{\"a\":{\"a\":{\"a\":"
                     "{\"a\":{\"a\":{\"a\":{\"a\":"
                     "{\"a\":{\"a\":{\"a\":{\"a\":"
                     "{\"a\":{\"a\":{\"a\":{\"a\":2"
                     "}}}}}}}}}}}}}}}},"
    "\"inaccessible_b\": 3}";

static const char object_depth_under_limit[] =
    "{\"accessible_a\": 1,"
     "\"not_too_deep\":" "{\"a\":{\"a\":{\"a\":{\"a\":"
                         "{\"a\":{\"a\":{\"a\":{\"a\":"
                         "{\"a\":{\"a\":{\"a\":{\"a\":"
                         "{\"a\":{\"a\":{\"a\":2"
                         "}}}}}}}}}}}}}}},"
     "\"accessible_b\": 3}";

static int limit_tests(void)
{
    printf("\nlimit tests:\n");

    const char* name;
    mxl_json_doc doc;
    int rc = -1;

#define TS(VARIANT) \
    { \
        name = #VARIANT; \
        printf("\nSTART limit test: %s\n", name);  \
        rc = test_input_build(VARIANT, &doc); \
        if (rc) goto finally; \
        mxl_json_dump_doc(&doc); \
    }

#define D1(VAL, ERR, P1) \
    rc = verify_json_double(&doc, VAL, ERR, 1, P1); \
    if (rc) goto finally;

#define TE(VARIANT) \
    av_assert0(strcmp(name, #VARIANT) == 0); \
    av_assert0(!rc); \
    printf("PASS limit: %s\n", name); \
    mxl_json_doc_release(&doc);

    // over-limit numbes tests are hardcode for 127 max characters
    if (127 != MXL_JSON_MAX_NUMBER_CHARS) {
        printf("FAIL MXL_JSON_MAX_NUMBER_CHARS test mismatch\n");
        rc = 1;
        goto finally;
    }
    else {
        printf("PASS MXL_JSON_MAX_NUMBER_CHARS test match\n");
    }

    TS(number_over_conversion_limit);
    D1(NAN, MXL_JSON_UNSUPPORTED, "too_long");
    TE(number_over_conversion_limit);

    TS(number_under_conversion_limit);
    D1(1.1111111111111111e126, 0, "not_too_long");
    TE(number_under_conversion_limit);

    // over-limit depth tests are hard coded for a max depth of 16.
    if (16 != MXL_JSON_MAX_DEPTH) {
        printf("FAIL MXL_JSON_MAX_DEPTH test mismatch\n");
        rc = 1;
        goto finally;
    }
    else {
        printf("PASS MXL_JSON_MAX_DEPTH test match\n");
    }

    TS(object_depth_over_limit);
    // works because the over-depth object does not have to be traversed
    D1(1, 0, "accessible_a");
    // fails at the the key length limit
    rc = verify_json_double(&doc, NAN, MXL_JSON_UNSUPPORTED, 17,
                            "too_deep",
                            "a","a","a","a","a","a","a","a",
                            "a","a","a","a","a","a","a","a");
    if (rc) goto finally;
    // fails to traverse the over-depth object
    D1(NAN, MXL_JSON_ENOTFOUND, "inaccessible_b");
    TE(object_depth_over_limit);


    TS(object_depth_under_limit);
    D1(1, 0, "accessible_a");
    rc = verify_json_double(&doc, 2, 0, 16,
                            "not_too_deep",
                            "a","a","a","a","a","a","a","a",
                            "a","a","a","a","a","a","a");
    if (rc) goto finally;
    // fails at the key length limit
    rc = verify_json_double(&doc, NAN, MXL_JSON_UNSUPPORTED, 17,
                            "not_too_deep",
                            "a","a","a","a","a","a","a","a",
                            "a","a","a","a","a","a","a","a");
    if (rc) goto finally;
    D1(3, 0, "accessible_b");
    TE(object_depth_under_limit);

finally:

    return rc;

#undef TS
#undef TE
#undef D1
}

int main(void) {

    printf("testing mxl_json: jsmn based MXL resource definition parser\n");

    int rc = -1;

    // FFmpeg expects C locale, this JSON parser depends on that.
    if (!setlocale(LC_ALL, "C"))
        goto finally;

    rc = sanity_test();
    if (rc) goto finally;

    rc = abberant_input_tests();
    if (rc) goto finally;

    rc = non_c_locale_tests();
    if (rc) goto finally;

    rc = real_world_mxl_json_test();
    if (rc) goto finally;

    rc = token_capacity_tests();
    if (rc) goto finally;

    rc = limit_tests();
    if (rc) goto finally;

finally:

    if (rc)
        printf("\nFAIL\n");
    else
        printf("\nPASS\n");

    return rc;
}
