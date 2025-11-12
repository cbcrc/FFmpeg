/*
 * MXL JSON parser for Media eXchange Layer flows
 *
 * Copyright (c) 2025 Canadian Broadcasting Corporation / Radio-Canada
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

#ifndef AVFORMAT_MXL_JSON_H
#define AVFORMAT_MXL_JSON_H

// jsmn header file
// see: https://github.com/zserge/jsmn/blob/v1.1.0/README.md#usage
#define JSMN_HEADER
#include "jsmn.h"

#include <stdarg.h>

/**
 * Parsed JSON document wrapper.
 *
 * Produced by mxl_json_doc_build().
 * Released by mxl_json_doc_release() to free the token array.
 *
 * Fields:
 *   - json   : Pointer to the original NUL-terminated JSON buffer.
 *              Owned by the caller; must remain valid while the doc is used.
 *   - tokens : Heap-allocated array of jsmntok_t tokens.
 *   - ntok   : Number of tokens in tokens[] array.
 */
typedef struct {
    const char  *json;
    jsmntok_t   *tokens;
    unsigned int ntok;
} mxl_json_doc;

/**
 * mxl_json specific error codes
 *
 * These are distinct from system <errno.h> values and are returned in
 * the .err field of json_num/json_str results.
 */
enum MXL_JSON_ERROR {
    MXL_JSON_EBADTYPE = 2001, // wrong token type
    MXL_JSON_ENOTFOUND,       // lookup failed, key path not present
    MXL_JSON_NULL,            // null primitive found where value requested
    MXL_JSON_MALFORMED,       // malformed json structure
    MXL_JSON_UNSUPPORTED,     // unsupported json,
    MXL_JSON_EINTERNAL        // unspecified internal error
};

/**
 * Implementation limits
 */
enum MXL_JSON_LIMITS {
    // limit imposed on object depth and key path length
    MXL_JSON_MAX_DEPTH = 16,
    // limit imposed on the character count in a json number
    MXL_JSON_MAX_NUMBER_CHARS = 127,
    // the default token capacity used by mxl_json_doc_build
    MXL_JSON_INITIAL_TOKEN_CAPACITY_DEFAULT = 128,
    // default maximum token capacity
    MXL_JSON_TOKEN_CAPACITY_MAX_DEFAULT = 8192
};

/**
 * Result of parsing a JSON token as a double.
 *
 * @field value  Parsed numeric value.
 * @field err    Status code:
 *               0          - success
 *               EINVAL     - token not numeric or conversion failed
 *               ERANGE     - overflow or underflow
 *               MXL_JSON_* - an MXL_JSON_ERROR
 */
typedef struct {
    double value;
    int    err;
} mxl_json_num;

/**
 * Result of parsing a JSON token as a string. The value pointer is
 * heap allocated and owned by the caller.
 *
 * @field value  Parsed string value.
 * @field err    Status code:
 *               0          - success
 *               EINVAL     - token is not a string
 *               ENOMEM     - allocation failed
 *               MXL_JSON_* - an MXL_JSON_ERROR
 */
typedef struct {
    char *value;
    int   err;
} mxl_json_str;

/**
 * See @mxl_json_doc_build().
 *
 * @initial_token_capacity is the initial size of the json token array
 * allocated for @out. The capacity of the token array is doubled as
 * necessary until it is sufficiently large to parse @json. Value of
 * zero returns AVERROR(EINVAL).
 *
 * @max_token_capacity is a safety limit on the size of tokens
 * allocated to store the parsed @json. It must be >=
 * @initial_token_capacity.
 *
 */
int mxl_json_doc_build2(const char *json, mxl_json_doc *out,
                        unsigned int initial_token_capacity,
                        unsigned int max_token_capacity);

/**
 * Parse a JSON string into a json_doc.
 *
 * The caller retains ownership of @json, and it must remain
 * valid. The token array is owned by the mxl_json_doc instance.
 *
 * The maximum json document size is limited by
 * MXL_JSON_TOKEN_CAPACITY_MAX_DEFAULT. Use @mxl_json_doc_build2 to
 * increase the maximum token capacity.
 *
 * @param json  Raw JSON buffer (NUL-terminated).
 * @param out   Output json_doc (filled on success).
 * @return 0    on success, or error code
 *         AVERROR(EINVAL) if args are invalid,
 *         AVERROR(ENOMEM) on token capacity over limit, or allocation failure
 *         AVERROR(EIO)    on parse error (other than NOMEM).
 */
static inline int mxl_json_doc_build(const char *json, mxl_json_doc *out)
{
    return mxl_json_doc_build2(json, out,
                               MXL_JSON_INITIAL_TOKEN_CAPACITY_DEFAULT,
                               MXL_JSON_TOKEN_CAPACITY_MAX_DEFAULT);
}

/**
 * Release resource allocated by @mxl_json_doc_build and clear @doc.
 */
void mxl_json_doc_release(mxl_json_doc *doc);

/**
 * Look up a JSON key path and return its string value.
 *
 * Traverse the tokens in @doc along the specified sequence key
 * path. If the path resolves to a string, then a copy of its value is
 * returned in .value with .err == 0. The caller owns .value and is
 * responsible for freeing it.
 *
 * Errors:
 *   - MXL_JSON_ENOTFOUND   : key path not found or @doc is invalid
 *   - MXL_JSON_EBADTYPE    : token at path was not string
 *   - MXL_JSON_NULL        : token at path was json null, .value is null
 *   - MXL_JSON_UNSUPPORTED : token at path exceeds depth limit
 *   - ENOMEM               : allocation failed
 *
 * @param doc   JSON document (from json_doc_build()).
 * @param nkeys Number of keys in the path.
 * @param ap    Sequence of (const char*) keys to descend.
 * @return mxl_json_str with string in .value, and .err status.
 */
mxl_json_str mxl_json_doc_get_stringv(const mxl_json_doc *doc, unsigned int nkeys, va_list ap);
mxl_json_str mxl_json_doc_get_string(const mxl_json_doc *doc, unsigned int nkeys, ...);

/**
 * Look up a JSON key path and return its numeric value.
 *
 * Traverse the tokens in @doc along the specified sequence key
 * path. If the path resolves to a number, then its value is returned
 * in .value with .err == 0.
 *
 * Error:
 *   - MXL_JSON_ENOTFOUND   : key path not found or @doc invalid
 *   - MXL_JSON_EBADTYPE    : token at path was not numeric
 *   - MXL_JSON_NULL        : token at path was json null, .value is NaN
 *   - MXL_JSON_MALFORMED   : token at path was a malformed number
 *   - MXL_JSON_UNSUPPORTED : token path length exceeds depth limit
 *   - ERANGE               : overflow or underflow during conversion
 *
 * @param doc   Parsed JSON document (from mxl_json_doc_build()).
 * @param nkeys Number of keys in the path.
 * @param ap    Sequence of (const char*) keys to descend.
 * @return mxl_json_num with numeric value in .value, and .err status.
 */
mxl_json_num  mxl_json_doc_get_doublev(const mxl_json_doc *doc, unsigned int nkeys, va_list ap);
mxl_json_num  mxl_json_doc_get_double(const mxl_json_doc *doc, unsigned int nkeys, ...);

/* Fixed-arity convenience wrappers */
static inline mxl_json_num mxl_json_doc_get_double1(const mxl_json_doc *doc,
                                      const char *k1)
{ return mxl_json_doc_get_double(doc, 1, k1); }

static inline mxl_json_num mxl_json_doc_get_double2(const mxl_json_doc *doc,
                                      const char *k1, const char *k2)
{ return mxl_json_doc_get_double(doc, 2, k1, k2); }

static inline mxl_json_str mxl_json_doc_get_string1(const mxl_json_doc *doc,
                                                    const char *k1)
{ return mxl_json_doc_get_string(doc, 1, k1); }

static inline mxl_json_str mxl_json_doc_get_string2(const mxl_json_doc *doc,
                                                    const char *k1, const char *k2)
{ return mxl_json_doc_get_string(doc, 2, k1, k2); }

#endif /* AVFORMAT_MXL_JSON_H */
