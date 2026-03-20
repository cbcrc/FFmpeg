/*
 * MXL JSON parser for Media eXchange Layer flows
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

/*
 * Wrapper around jsmn (https://github.com/zserge/jsmn).
 * Depends on vendored jsmn v1.1.0 (see jsmn.h).
 */

#include "mxl_json.h"
#include "mxl_common.h"

#include "libavutil/error.h"
#include "libavutil/avassert.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <math.h>

// maximum json string length to log on parse error
enum { MAX_JSON_LOG = 256 };

// compare jsmn string token to value
static int is_token_name_equal(const char *js, const jsmntok_t *t, const char *s)
{
    av_assert1(js);
    av_assert1(s);

    if (!t || t->type != JSMN_STRING)
        return 0;

    // jsmn parser invariant debug assert and release safety check
    av_assert1(t->end >= t->start);
    if (t->end < t->start)
        return 0;

    size_t tok_len = (size_t)(t->end - t->start);

    return strlen(s) == tok_len &&
           strncmp(js + t->start, s, tok_len) == 0;
}

static unsigned int last_token_of_subtree(const jsmntok_t *tokens, unsigned int ntok,
                                          unsigned int node_idx, unsigned int depth);

// return the last jsmn token of an object
static unsigned int last_token_of_object(const jsmntok_t *tokens, unsigned int ntok,
                                         unsigned int node_idx, unsigned int depth)
{
    if (node_idx >= ntok || depth > MXL_JSON_MAX_DEPTH)
        return ntok;

    jsmntok_t t = tokens[node_idx];

    // type sanity
    av_assert0(t.type == JSMN_OBJECT);

    int num_children = t.size;
    for (int child = 0; child < num_children; child++) {
        // skip ahead to the key
        node_idx++;

        // node_idx sanity
        av_assert0(node_idx < ntok);

        // key type sanity
        av_assert0(tokens[node_idx].type == JSMN_STRING);

        // jump to end of value
        node_idx = last_token_of_subtree(tokens, ntok,
                                         node_idx + 1,
                                         depth+1);
        // depth guard
        if (node_idx >= ntok)
            return ntok;
    }

    return node_idx;
}

// return the last jsmn token of an array
static unsigned int last_token_of_array(const jsmntok_t *tokens, unsigned int ntok,
                                        unsigned int node_idx, unsigned int depth)
{
    if (node_idx >= ntok || depth > MXL_JSON_MAX_DEPTH)
        return ntok;

    jsmntok_t t = tokens[node_idx];

    // type sanity
    av_assert0(t.type == JSMN_ARRAY);

    int num_children = t.size;
    for (int child = 0; child < num_children; child++) {
        // jump to end of value
        node_idx = last_token_of_subtree(tokens, ntok,
                                         node_idx + 1,
                                         depth+1);
        // depth guard
        if (node_idx >= ntok)
            return ntok;
    }

    return node_idx;
}

// return the index of the last jsmn token of a json subtree
static unsigned int last_token_of_subtree(const jsmntok_t *tokens, unsigned int ntok,
                                          unsigned int node_idx, unsigned int depth)
{
    if (node_idx >= ntok || depth > MXL_JSON_MAX_DEPTH)
        return ntok;

    jsmntok_t t = tokens[node_idx];

    switch (t.type) {
    case JSMN_OBJECT:
        return last_token_of_object(tokens, ntok, node_idx, depth);
    case JSMN_ARRAY:
        return last_token_of_array(tokens, ntok, node_idx, depth);
    case JSMN_STRING:
    case JSMN_PRIMITIVE:
        return node_idx;
    default:
        av_assert0(!"unknown jsmn token type");
        return ntok;
    }
}

// jsmn token to string value with special case json null detection
static mxl_json_str jsmn_tok_to_string(const char *json, const jsmntok_t *t)
{
    if (!t) {
        return (mxl_json_str){ .value = NULL, .err = MXL_JSON_EBADTYPE };
    }

    // jsmn parser invariant debug check and release safety check
    av_assert1(t->end >= t->start);
    if (t->end < t->start)
        return (mxl_json_str){ .value = NULL, .err = MXL_JSON_EINTERNAL };

    if (t->type != JSMN_STRING)
        if (t->type == JSMN_PRIMITIVE &&
            t->end - t->start == 4 &&
            !strncmp(json + t->start, "null", 4))
            return (mxl_json_str){ .value = NULL, .err = MXL_JSON_NULL };
        else
            return (mxl_json_str){ .value = NULL, .err = MXL_JSON_EBADTYPE };

    size_t len = t->end - t->start;
    char *s = (char *)av_malloc(len + 1);
    if (!s)
        return (mxl_json_str){ .value = NULL, .err = ENOMEM };
    memcpy(s, json + t->start, len);
    s[len] = '\0';

    return (mxl_json_str){ .value = s, .err = 0 };
}

// jsmn token to double value with null, infinity, and strtod errno detection
static mxl_json_num jsmn_tok_to_double(const char *json, const jsmntok_t *t)
{
    if (!t || t->type != JSMN_PRIMITIVE)
       return (mxl_json_num){.value = NAN, .err = MXL_JSON_EBADTYPE};

    // jsmn parser invariant debug check and release safety check
    av_assert1(t->end >= t->start);
    if (t->end < t->start)
       return (mxl_json_num){.value = NAN, .err = MXL_JSON_EINTERNAL};

    char buf[MXL_JSON_MAX_NUMBER_CHARS+1];
    size_t len = t->end - t->start;
    if (len >= sizeof(buf))
        return (mxl_json_num){ .value = NAN, .err = MXL_JSON_UNSUPPORTED};
    memcpy(buf, json + t->start, len);
    buf[len] = '\0';

    errno = 0;
    char *endptr = NULL;
    double v = strtod(buf, &endptr);

    // strtod() endptr no conversion case: detect attempts to convert
    // null, boolean, or invalid jsmn primitives.
    if (buf == endptr)
        if (len == 4 && !strncmp(buf, "null", 4))
            return (mxl_json_num){ .value = NAN, .err = MXL_JSON_NULL };
        else
            return (mxl_json_num){.value = NAN, .err = MXL_JSON_EBADTYPE};

    // strtod() incomplete conversion, general case, indicating a
    // malformed number
    if ((size_t)(endptr - buf) != len)
        return (mxl_json_num){.value = NAN, .err = MXL_JSON_MALFORMED};

    // strtod errno cases
    if (errno == ERANGE)
        return (mxl_json_num){ .value = v, .err = ERANGE };
    // safety catch-all, shouldn't happen with post C99 strtod()
    else if (errno)
        return (mxl_json_num){ .value = NAN, .err = errno };

    // nan/inf error flag
    if (isnan(v) || isinf(fabs(v)))
        return (mxl_json_num){.value = v, .err = MXL_JSON_EBADTYPE};

    return (mxl_json_num){ .value = v, .err = 0 };
}

// jsmn token and its index together
typedef struct token_idx_pair {
    const jsmntok_t *tok;
    const unsigned int node_idx;
} token_idx_pair;

// json path follower, recursively descends object tree
static token_idx_pair
find_path_rec(const char *json, const jsmntok_t *tokens, unsigned int ntok,
              unsigned int node_idx, const char **keys, unsigned int nkeys,
              unsigned int depth)
{
    // safety limits enforce
    if (depth > MXL_JSON_MAX_DEPTH || node_idx >= ntok)
        return (token_idx_pair){0,ntok};

    // key exhaustion means we've found the token
    if (nkeys == 0)
        return (token_idx_pair){&tokens[node_idx], node_idx};

    // cannot descend non object types
    if (tokens[node_idx].type != JSMN_OBJECT)
        return (token_idx_pair){0,ntok};

    // iterate over object's children to find the named child
    int num_children = tokens[node_idx].size;
    unsigned int child_node_idx = node_idx+1;
    for (int child = 0; child < num_children; child++) {

        // child_node_idx sanity
        av_assert0(child_node_idx < ntok);

        const jsmntok_t *child_tok = &tokens[child_node_idx];

        // keys[0] sanity
        av_assert0(keys[0]);

        // is this the named child we are looking for?
        if (is_token_name_equal(json, child_tok, keys[0])) {
            // descend into the child to find the next key
            return find_path_rec(json, tokens, ntok,
                                 child_node_idx+1,
                                 keys + 1, nkeys - 1, depth + 1);
        }

        // else jump past the child's contained tokens
        child_node_idx = last_token_of_subtree(tokens, ntok, child_node_idx+1,
                                               depth + 1) + 1;

        // did we step past end? (because depth limit exceeded)
        if (child_node_idx >= ntok)
            break;
    }

    // didn't find it
    return (token_idx_pair){0, ntok};
}

// varargs adapter, build a key array and call the recursive finder
static token_idx_pair find_path_va(const mxl_json_doc *doc, unsigned int nkeys,
                                   va_list ap)
{
    // doc and nkeys sanity
    av_assert0(doc && doc->tokens && 0 < nkeys && nkeys <= MXL_JSON_MAX_DEPTH);

    const char *keys[MXL_JSON_MAX_DEPTH];
    for (unsigned int i = 0; i < nkeys; i++)
        keys[i] = va_arg(ap, const char *);

    return find_path_rec(doc->json, doc->tokens, doc->ntok, 0, keys, nkeys, 0);
}

//
// public api
//

int mxl_json_doc_build2(void* logctx,
                        const char *json, mxl_json_doc *out,
                        const unsigned int initial_token_capacity,
                        const unsigned int max_token_capacity)
{
    if (!json || !out)
        return AVERROR(EINVAL);

    if (0 == initial_token_capacity || initial_token_capacity > max_token_capacity)
        return AVERROR(EINVAL);

    size_t json_len = strlen(json);
    jsmn_parser p;
    jsmntok_t *tokens = NULL;
    unsigned int token_capacity = initial_token_capacity;
    int rc;

    jsmn_init(&p);

    for (;;) {

        if (token_capacity > max_token_capacity) {
            loge(logctx, "JSON token cap (%u) exceeded\n",
                    max_token_capacity);
            av_free(tokens);
            return AVERROR(ENOMEM);
        }

        size_t token_buf_size = (size_t)token_capacity * sizeof(*tokens);

        // size_t overflow guard
        if (token_buf_size/token_capacity != sizeof(*tokens)) {
            av_free(tokens);
            return AVERROR(ENOMEM);
        }

        jsmntok_t *new_tokens = (jsmntok_t *)av_realloc(tokens, token_buf_size);
        if (!new_tokens) {
            av_free(tokens);
            return AVERROR(ENOMEM);
        }
        tokens = new_tokens;

        rc = jsmn_parse(&p, json, json_len, tokens, token_capacity);

        if (JSMN_ERROR_NOMEM == rc) {
            // paranoid double; force cap check if near limit
            if (token_capacity > max_token_capacity / 2u) {
                // will fail cap check on next loop
                token_capacity = max_token_capacity + 1u;
            } else {
                token_capacity *= 2u;
            }
            continue;
        }

        break;
    }

    if (rc < 0) {
        loge(logctx, "JSON failed to parse: >>%.*s%s<<\n",
             MAX_JSON_LOG, json, json_len > MAX_JSON_LOG ? "..." : "");

        av_free(tokens);
        return AVERROR_INVALIDDATA;
    }

    out->json = json;
    out->tokens  = tokens;
    out->ntok = (unsigned int)rc;

    return 0;
}

void mxl_json_doc_release(mxl_json_doc *doc)
{
    if (!doc)
        return;

    av_free(doc->tokens);
    *doc = (mxl_json_doc){0};
}

mxl_json_str mxl_json_doc_get_stringv(const mxl_json_doc *doc, unsigned int nkeys,
                                      va_list ap)
{
    if (!doc || !doc->tokens)
        return (mxl_json_str){ .value = NULL, .err = MXL_JSON_ENOTFOUND };

    if (nkeys > MXL_JSON_MAX_DEPTH)
        return (mxl_json_str){ .value = NULL, .err = MXL_JSON_UNSUPPORTED };

    token_idx_pair t = find_path_va(doc, nkeys, ap);
    if (!t.tok)
        return (mxl_json_str){ .value = NULL, .err = MXL_JSON_ENOTFOUND };

    return jsmn_tok_to_string(doc->json, t.tok);
}

mxl_json_str mxl_json_doc_get_string(const mxl_json_doc *doc, unsigned int nkeys,
                                     ...)
{
    va_list ap;
    va_start(ap, nkeys);
    mxl_json_str str = mxl_json_doc_get_stringv(doc, nkeys, ap);
    va_end(ap);

    return str;
}

mxl_json_num mxl_json_doc_get_doublev(const mxl_json_doc *doc, unsigned int nkeys,
                                      va_list ap)
{
    if (!doc || !doc->tokens)
        return (mxl_json_num){ .value = NAN, .err = MXL_JSON_ENOTFOUND };

    if (nkeys > MXL_JSON_MAX_DEPTH)
        return (mxl_json_num){ .value = NAN, .err = MXL_JSON_UNSUPPORTED };

    token_idx_pair t = find_path_va(doc, nkeys, ap);
    if (!t.tok)
        return (mxl_json_num){ .value = NAN, .err = MXL_JSON_ENOTFOUND };

    // Token look ahead to validate json formation, only one primitive
    // should follow an object key.
    if (t.tok->type == JSMN_PRIMITIVE &&
        t.node_idx+1 < doc->ntok &&
        doc->tokens[t.node_idx+1].type == JSMN_PRIMITIVE) {
        return (mxl_json_num){ .value = NAN, .err = MXL_JSON_MALFORMED};
    }

    return jsmn_tok_to_double(doc->json, t.tok);
}

mxl_json_num mxl_json_doc_get_double(const mxl_json_doc *doc, unsigned int nkeys,
                                     ...)
{
    va_list ap;
    va_start(ap, nkeys);
    mxl_json_num num = mxl_json_doc_get_doublev(doc, nkeys, ap);
    va_end(ap);

    return num;
}
