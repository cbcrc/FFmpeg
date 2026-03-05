/*
 * MXL URI parser tests for Media eXchange Layer flows
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

#include "libavformat/mxl_uri.h"
#include "libavutil/avassert.h"
#include "libavutil/error.h"
#include "libavutil/log.h"

#include <locale.h>
#include <stdio.h>
#include <string.h>

typedef struct test_case {
    const char *name;
    const char *uri;
    int expected_rc;
    mxl_uri expected;
} test_case;

static int compare_uri(const mxl_uri *expected, const mxl_uri *actual)
{
    if (!expected || !actual) {
        fprintf(stderr, "compare_uri: expected or actual is NULL\n");
        return -1;
    }

    if (expected->port != actual->port) {
        fprintf(stderr, "compare_uri: port mismatch: expected %d actual %d\n",
                expected->port, actual->port);
        return -1;
    }

    if ((expected->host == NULL) != (actual->host == NULL)) {
        fprintf(stderr, "compare_uri: host NULL mismatch: expected %s actual %s\n",
                expected->host ? expected->host : "(null)",
                actual->host ? actual->host : "(null)");
        return -1;
    }

    if (expected->host && strcmp(expected->host, actual->host) != 0) {
        fprintf(stderr, "compare_uri: host mismatch: expected '%s' actual '%s'\n",
                expected->host, actual->host);
        return -1;
    }

    if ((expected->domain == NULL) != (actual->domain == NULL)) {
        fprintf(stderr, "compare_uri: domain NULL mismatch: expected %s actual %s\n",
                expected->domain ? expected->domain : "(null)",
                actual->domain ? actual->domain : "(null)");
        return -1;
    }

    if (expected->domain && strcmp(expected->domain, actual->domain) != 0) {
        fprintf(stderr, "compare_uri: domain mismatch: expected '%s' actual '%s'\n",
                expected->domain, actual->domain);
        return -1;
    }

    if ((expected->flow_ids == NULL) != (actual->flow_ids == NULL)) {
        fprintf(stderr, "compare_uri: flow_ids pointer mismatch\n");
        return -1;
    }

    if (expected->nb_flow_ids != actual->nb_flow_ids) {
        fprintf(stderr, "compare_uri: flow_id count mismatch: expected %d actual %d\n",
                expected->nb_flow_ids, actual->nb_flow_ids);
        return -1;
    }

    for (int i = 0; i < expected->nb_flow_ids; i++) {
        const char *e = expected->flow_ids[i];
        const char *a = actual->flow_ids[i];

        if (e == NULL || a == NULL) {
            fprintf(stderr, "compare_uri: flow_id[%d] NULL: expected %s actual %s\n",
                    i,
                    e ? e : "(null)",
                    a ? a : "(null)");
            return -1;
        }

        if (strcmp(e, a) != 0) {
            fprintf(stderr, "compare_uri: flow_id[%d] mismatch: expected '%s' actual '%s'\n",
                    i, e, a);
            return -1;
        }
    }

    return 0;
}

static void dump_uri(const mxl_uri *u)
{
    if (!u) {
        printf("mxl_uri: (null)\n");
        return;
    }

    printf("mxl_uri {\n");

    printf("  host: \"%s\"\n", u->host ? u->host : "(null)");
    printf("  port: %d\n", u->port);
    printf("  domain: \"%s\"\n", u->domain ? u->domain : "(null)");

    printf("  flow_ids (%d):\n", u->nb_flow_ids);
    for (int i = 0; i < u->nb_flow_ids; i++) {
        printf("    [%d] \"%s\"\n", i,
               (u->flow_ids && u->flow_ids[i]) ? u->flow_ids[i] : "(null)");
    }

    printf("}\n");
}

static void dump_test_case(const test_case *tc, int actual_parse_rc, const mxl_uri *actual)
{
    printf("==== test case: %s\n", tc->name);

    printf("URI: \"%s\"\n", tc->uri);

    printf("expected return code: %d\n", tc->expected_rc);
    printf("actual return code: %d\n", actual_parse_rc);

    printf("expected URI:\n");
    dump_uri(&tc->expected);
    printf("actual URI:\n");
    dump_uri(actual);
}

static int run_test_case(const test_case *tc)
{
    mxl_uri actual = {0};
    int parse_rc = -1;
    int compare_rc = -1;
    int test_case_rc = -1;

    parse_rc = mxl_parse_uri(NULL, tc->uri, &actual);

    dump_test_case(tc, parse_rc, &actual);

    compare_rc = compare_uri(&tc->expected, &actual);
    if (compare_rc != 0) {
        fprintf(stderr,
                "==== test '%s' FAIL: parsed URI mismatch\n",
                tc->name);
        goto finally;
    }

    if (parse_rc != tc->expected_rc) {
        fprintf(stderr,
                "==== test '%s' FAIL: return code mismatch "
                "(expected %d got %d)\n",
                tc->name, tc->expected_rc, parse_rc);
        goto finally;
    }

    test_case_rc = 0;
    printf("==== test case: %s PASS\n", tc->name);

finally:
    if (!parse_rc)
        mxl_uri_free(&actual);

    return test_case_rc;
}

// expected mxl_uri value on error
static const mxl_uri expected_error_uri = {
    .host = NULL,
    .port = -1,
    .domain = NULL,
    .flow_ids = NULL,
    .nb_flow_ids = 0,
};

static test_case zero_id = {
    .name = "zero_id",

    .uri = "mxl://host1.local/domain/path",

    .expected_rc = 0,

    .expected = {
        .host   = "host1.local",
        .port   = -1,
        .domain = "/domain/path",
        .flow_ids = NULL,
        .nb_flow_ids = 0,
    },
};

static test_case one_id = {
    .name = "one_id",

    .uri =
        "mxl://host1.local/domain/path?"
        "id=3645f1e2-90fc-49cb-af9c-236c2ac124cd",

    .expected_rc = 0,

    .expected = {
        .host   = "host1.local",
        .port   = -1,
        .domain = "/domain/path",

        .flow_ids = (const char *[]) {
            "3645f1e2-90fc-49cb-af9c-236c2ac124cd"
        },

        .nb_flow_ids = 1,
    },
};

static test_case two_id = {
    .name = "two_id",

    .uri =
        "mxl://host1.local/domain/path?"
        "id=3645f1e2-90fc-49cb-af9c-236c2ac124cd&"
        "id=876fc244-9c99-48a8-a461-927e43a05bdc",

    .expected_rc = 0,

    .expected = {
        .host   = "host1.local",
        .port   = -1,
        .domain = "/domain/path",

        .flow_ids = (const char *[]) {
            "3645f1e2-90fc-49cb-af9c-236c2ac124cd",
            "876fc244-9c99-48a8-a461-927e43a05bdc",
        },

        .nb_flow_ids = 2,
    },
};

static test_case three_id = {
    .name = "three_id",

    .uri =
        "mxl://host1.local/domain/path?"
        "id=3645f1e2-90fc-49cb-af9c-236c2ac124cd&"
        "id=876fc244-9c99-48a8-a461-927e43a05bdc&"
        "id=2b528ffe-c94c-454f-984a-35c98c717006",

    .expected_rc = 0,

    .expected = {
        .host   = "host1.local",
        .port   = -1,
        .domain = "/domain/path",

        .flow_ids = (const char *[]) {
            "3645f1e2-90fc-49cb-af9c-236c2ac124cd",
            "876fc244-9c99-48a8-a461-927e43a05bdc",
            "2b528ffe-c94c-454f-984a-35c98c717006"
        },

        .nb_flow_ids = 3,
    },
};

static test_case two_id_upcase = {
    .name = "two_id_upcase",

    .uri =
        "MXL://HOST1.LOCAL/DOMAIN/PATH?"
        "id=3645F1E2-90FC-49CB-AF9C-236C2AC124CD&"
        "id=876FC244-9C99-48A8-A461-927E43A05BDC",

    .expected_rc = 0,

    .expected = {
        .host   = "HOST1.LOCAL",
        .port   = -1,
        .domain = "/DOMAIN/PATH",

        .flow_ids = (const char *[]) {
            "3645F1E2-90FC-49CB-AF9C-236C2AC124CD",
            "876FC244-9C99-48A8-A461-927E43A05BDC",
        },

        .nb_flow_ids = 2,
    },
};

static test_case two_id_upcase_key = {
    .name = "two_id_upcase_key",

    .uri =
        "mxl://host1.local/domain/path?"
        "ID=3645f1e2-90fc-49cb-af9c-236c2ac124cd&"
        "id=876fc244-9c99-48a8-a461-927e43a05bdc",

    .expected_rc = 0,

    .expected = {
        .host   = "host1.local",
        .port   = -1,
        .domain = "/domain/path",

        .flow_ids = (const char *[]) {
            "876fc244-9c99-48a8-a461-927e43a05bdc",
        },

        .nb_flow_ids = 1,
    },
};

static test_case unknown_query_param = {
    .name = "unknown_query_param",

    .uri =
        "mxl://host1.local/domain/path?"
        "id=3645f1e2-90fc-49cb-af9c-236c2ac124cd&"
        "unknown=unknown&"
        "id=876fc244-9c99-48a8-a461-927e43a05bdc",

    .expected_rc = 0,

    .expected = {
        .host   = "host1.local",
        .port   = -1,
        .domain = "/domain/path",

        .flow_ids = (const char *[]) {
            "3645f1e2-90fc-49cb-af9c-236c2ac124cd",
            "876fc244-9c99-48a8-a461-927e43a05bdc",
        },

        .nb_flow_ids = 2,
    },
};

static test_case unknown_flag_query_param = {
    .name = "unknown_flag_query_param",

    .uri =
        "mxl://host1.local/domain/path?"
        "id=3645f1e2-90fc-49cb-af9c-236c2ac124cd&"
        "empty&"
        "id=876fc244-9c99-48a8-a461-927e43a05bdc",

    .expected_rc = 0,

    .expected = {
        .host   = "host1.local",
        .port   = -1,
        .domain = "/domain/path",

        .flow_ids = (const char *[]) {
            "3645f1e2-90fc-49cb-af9c-236c2ac124cd",
            "876fc244-9c99-48a8-a461-927e43a05bdc",
        },

        .nb_flow_ids = 2,
    },
};

static test_case empty_query = {
    .name = "empty_query",

    .uri = "mxl://host1.local/domain/path?",

    .expected_rc = 0,

    .expected = {
        .host = "host1.local",
        .port = -1,
        .domain = "/domain/path",
        .flow_ids = NULL,
        .nb_flow_ids = 0,
    },
};

static test_case ipv4 = {
    .name = "ipv4",

    .uri =
        "mxl://10.1.2.3:5000/domain/path?"
        "id=8c8e8a37-4a5b-4e0a-9f0b-2e3e71e3f5e1&"
        "id=5d9d1a64-7a77-4b54-8e32-6e6e8b0b9a44",

    .expected_rc = 0,

    .expected = {
        .host = "10.1.2.3",
        .port = 5000,
        .domain = "/domain/path",

        .flow_ids = (const char *[]) {
            "8c8e8a37-4a5b-4e0a-9f0b-2e3e71e3f5e1",
            "5d9d1a64-7a77-4b54-8e32-6e6e8b0b9a44",
        },

        .nb_flow_ids = 2,
    },
};

static test_case zero_port = {
    .name = "zero_port",

    .uri =
        "mxl://10.1.2.3:0/domain/path?"
        "id=8c8e8a37-4a5b-4e0a-9f0b-2e3e71e3f5e1&"
        "id=5d9d1a64-7a77-4b54-8e32-6e6e8b0b9a44",

    .expected_rc = AVERROR(EINVAL),

    .expected = expected_error_uri
};

static test_case ipv6 = {
    .name = "ipv6",

    .uri =
        "mxl://[2001:db8::2]/domain/path?"
        "id=1e9c4c74-6c44-4f7d-b8a3-3e4f9a4e8a92&"
        "id=3fbcf2a1-2e8a-4a4d-9c8f-0d7e4a6b7c51",

    .expected_rc = 0,

    .expected = {
        .host = "2001:db8::2",
        .port = -1,
        .domain = "/domain/path",

        .flow_ids = (const char *[]) {
            "1e9c4c74-6c44-4f7d-b8a3-3e4f9a4e8a92",
            "3fbcf2a1-2e8a-4a4d-9c8f-0d7e4a6b7c51",
        },

        .nb_flow_ids = 2,
    },
};

static test_case ipv6_with_port = {
    .name = "ipv6_with_port",

    .uri =
        "mxl://[2001:db8::2]:5000/domain/path?"
        "id=1e9c4c74-6c44-4f7d-b8a3-3e4f9a4e8a92&"
        "id=3fbcf2a1-2e8a-4a4d-9c8f-0d7e4a6b7c51",

    .expected_rc = 0,

    .expected = {
        .host = "2001:db8::2",
        .port = 5000,
        .domain = "/domain/path",

        .flow_ids = (const char *[]) {
            "1e9c4c74-6c44-4f7d-b8a3-3e4f9a4e8a92",
            "3fbcf2a1-2e8a-4a4d-9c8f-0d7e4a6b7c51",
        },

        .nb_flow_ids = 2,
    },
};

static test_case malformed_ipv6_bracket = {
    .name = "malformed_ipv6_bracket",

    .uri =
        "mxl://[2001:db8::2/domain/path?"
        "id=3645f1e2-90fc-49cb-af9c-236c2ac124cd",

    .expected_rc = AVERROR(EINVAL),

    .expected = expected_error_uri
};

static test_case bad_scheme = {
    .name = "bad_scheme",

    .uri =
        "ftp://host1.local/domain/path?"
        "id=3645f1e2-90fc-49cb-af9c-236c2ac124cd&"
        "id=876fc244-9c99-48a8-a461-927e43a05bdc",

    .expected_rc = AVERROR(EINVAL),

    .expected = expected_error_uri
};

static test_case no_scheme = {
    .name = "no_scheme",

    .uri =
        "/host1.local/domain/path?"
        "id=3645f1e2-90fc-49cb-af9c-236c2ac124cd&"
        "id=876fc244-9c99-48a8-a461-927e43a05bdc",

    .expected_rc = AVERROR(EINVAL),

    .expected = expected_error_uri
};

static test_case null_uri = {
    .name = "null_uri",

    .uri = NULL,

    .expected_rc = AVERROR(EINVAL),

    .expected = expected_error_uri
};

static test_case empty_uri = {
    .name = "empty_uri",

    .uri = "",

    .expected_rc = AVERROR(EINVAL),

    .expected = expected_error_uri
};

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(
    MXL_SCHEME_MAX == 4,
    "Update equal_max_scheme and greater_max_scheme tests if MXL_SCHEME_MAX changes");
#endif
// scheme too long (length == MXL_SCHEME_MAX)
static test_case equal_max_scheme = {
    .name = "equal_max_scheme",

    .uri =
        "aaaa:/host1.local/domain/path?"
        "id=3645f1e2-90fc-49cb-af9c-236c2ac124cd&"
        "id=876fc244-9c99-48a8-a461-927e43a05bdc",

    .expected_rc = AVERROR(ENAMETOOLONG),

    .expected = expected_error_uri
};

// scheme too long (length > MXL_SCHEME_MAX)
static test_case greater_max_scheme = {
    .name = "greater_max_scheme",

    .uri =
        "aaaaa:/host1.local/domain/path?"
        "id=3645f1e2-90fc-49cb-af9c-236c2ac124cd&"
        "id=876fc244-9c99-48a8-a461-927e43a05bdc",

    .expected_rc = AVERROR(ENAMETOOLONG),

    .expected = expected_error_uri
};

static test_case empty_middle_id_query_param = {
    .name = "empty_middle_id_query_param",

    .uri =
        "mxl://host1.local/domain/path?"
        "id=3645f1e2-90fc-49cb-af9c-236c2ac124cd&"
        "id=&"
        "id=876fc244-9c99-48a8-a461-927e43a05bdc",

    .expected_rc = AVERROR(EINVAL),

    .expected = expected_error_uri
};

static test_case empty_end_id_query_param = {
    .name = "empty_end_id_query_param",

    .uri =
        "mxl://host1.local/domain/path?"
        "id=3645f1e2-90fc-49cb-af9c-236c2ac124cd&"
        "id=876fc244-9c99-48a8-a461-927e43a05bdc&"
        "id=",

    .expected_rc = AVERROR(EINVAL),

    .expected = expected_error_uri
};

static test_case bad_separator_query_param = {
    .name = "bad_separator_query_param",

    .uri =
        "mxl://host1.local/domain/path?"
        "id="
        "id=3645f1e2-90fc-49cb-af9c-236c2ac124cd&"
        "id=876fc244-9c99-48a8-a461-927e43a05bdc",

    .expected_rc = AVERROR(EINVAL),

    .expected = expected_error_uri
};

static test_case bad_uuid_query_param = {
    .name = "bad_uuid_query_param",

    .uri =
        "mxl://host1.local/domain/path?"
        "id=X645f1e2-90fc-49cb-af9c-236c2ac124cd&"
        "id=876fc244-9c99-48a8-a461-927e43a05bdc",

    .expected_rc = AVERROR(EINVAL),

    .expected = expected_error_uri
};

// special case -1 port value, caller must interpret as "not set"
static test_case valid_negative_one_port = {
    .name = "valid_negative_one_port",

    .uri =
        "mxl://10.1.2.3:-1/domain/path?"
        "id=8c8e8a37-4a5b-4e0a-9f0b-2e3e71e3f5e1&"
        "id=5d9d1a64-7a77-4b54-8e32-6e6e8b0b9a44",

    .expected_rc = 0,

    .expected = {
        .host = "10.1.2.3",
        .port = -1,
        .domain = "/domain/path",

        .flow_ids = (const char *[]) {
            "8c8e8a37-4a5b-4e0a-9f0b-2e3e71e3f5e1",
            "5d9d1a64-7a77-4b54-8e32-6e6e8b0b9a44",
        },

        .nb_flow_ids = 2,
    },
};

static test_case invalid_negative_port = {
    .name = "invalid_negative_port",

    .uri =
        "mxl://host1.local:-2/domain/path?"
        "id=645f1e2-90fc-49cb-af9c-236c2ac124cd&"
        "id=876fc244-9c99-48a8-a461-927e43a05bdc",

    .expected_rc = AVERROR(EINVAL),

    .expected = expected_error_uri
};

static test_case invalid_empty_port = {
    .name = "invalid_empty_port",

    .uri =
        "mxl://10.1.2.3:/domain/path?"
        "id=8c8e8a37-4a5b-4e0a-9f0b-2e3e71e3f5e1&"
        "id=5d9d1a64-7a77-4b54-8e32-6e6e8b0b9a44",

    .expected_rc = AVERROR(EINVAL),

    .expected = expected_error_uri
};

static test_case invalid_non_integer_port = {
    .name = "invalid_non_integer_port",

    .uri =
        "mxl://10.1.2.3:abc/domain/path?"
        "id=8c8e8a37-4a5b-4e0a-9f0b-2e3e71e3f5e1&"
        "id=5d9d1a64-7a77-4b54-8e32-6e6e8b0b9a44",

    .expected_rc = AVERROR(EINVAL),

    .expected = expected_error_uri
};

static test_case valid_at_range_limit_port = {
    .name = "valid_at_range_limit_port",

    .uri =
        "mxl://host1.local:65535/domain/path?"
        "id=3645f1e2-90fc-49cb-af9c-236c2ac124cd&"
        "id=876fc244-9c99-48a8-a461-927e43a05bdc",

    .expected_rc = 0,

    .expected = {
        .host   = "host1.local",
        .port   = 65535,
        .domain = "/domain/path",

        .flow_ids = (const char *[]) {
            "3645f1e2-90fc-49cb-af9c-236c2ac124cd",
            "876fc244-9c99-48a8-a461-927e43a05bdc",
        },

        .nb_flow_ids = 2,
    },
};

static test_case invalid_out_of_range_port = {
    .name = "invalid_out_of_range_port",

    .uri =
        "mxl://10.1.2.3:65536/domain/path?"
        "id=8c8e8a37-4a5b-4e0a-9f0b-2e3e71e3f5e1&"
        "id=5d9d1a64-7a77-4b54-8e32-6e6e8b0b9a44",

    .expected_rc = AVERROR(EINVAL),

    .expected = expected_error_uri
};

static test_case only_unknown_query_param = {
    .name = "only_unknown_query_param",

    .uri =
        "mxl://host1.local/domain/path?foo=bar&baz=qux",

    .expected_rc = 0,

    .expected = {
        .host   = "host1.local",
        .port   = -1,
        .domain = "/domain/path",
        .flow_ids = NULL,
        .nb_flow_ids = 0,
    },
};

static test_case leading_amp_query_param = {
    .name = "leading_amp_query_param",

    .uri =
        "mxl://host1.local/domain/path?"
        "&id=3645f1e2-90fc-49cb-af9c-236c2ac124cd",

    .expected_rc = 0,

    .expected = {
        .host   = "host1.local",
        .port   = -1,
        .domain = "/domain/path",

        .flow_ids = (const char *[]) {
            "3645f1e2-90fc-49cb-af9c-236c2ac124cd",
        },

        .nb_flow_ids = 1,
    },
};

static test_case double_separator_query_param = {
    .name = "double_separator_query_param",

    .uri =
        "mxl://host1.local/domain/path?"
        "id=3645f1e2-90fc-49cb-af9c-236c2ac124cd&&"
        "id=876fc244-9c99-48a8-a461-927e43a05bdc",

    .expected_rc = 0,

    .expected = {
        .host   = "host1.local",
        .port   = -1,
        .domain = "/domain/path",

        .flow_ids = (const char *[]) {
            "3645f1e2-90fc-49cb-af9c-236c2ac124cd",
            "876fc244-9c99-48a8-a461-927e43a05bdc",
        },

        .nb_flow_ids = 2,
    },
};

static test_case invalid_no_domain_path = {
    .name = "invalid_no_domain_path",

    .uri =
        "mxl://host1.local?"
        "id=3645f1e2-90fc-49cb-af9c-236c2ac124cd",

    .expected_rc = AVERROR(EINVAL),

    .expected = expected_error_uri
};

static test_case fragment_after_query = {
    .name = "fragment_after_query",

    .uri =
        "mxl://host1.local/domain/path?"
        "id=3645f1e2-90fc-49cb-af9c-236c2ac124cd#fragment",

    .expected_rc = AVERROR(EINVAL),

    .expected = expected_error_uri
};

static test_case percent_encoded_in_uuid_query_value = {
    .name = "percent_encoded_in_uuid_query_value",

    .uri =
        "mxl://host1.local/domain/path?"
        "id=3645f1e2-90fc-49cb-af9c-236c2ac124cd%26extra",

    .expected_rc = AVERROR(EINVAL),

    .expected = expected_error_uri
};

static test_case percent_encoded_in_other_query_value = {
    .name = "percent_encoded_in_other_query_value",

    .uri =
        "mxl://host1.local/domain/path?"
        "other=%26extra&"
        "id=3645f1e2-90fc-49cb-af9c-236c2ac124cd&"
        "id=876fc244-9c99-48a8-a461-927e43a05bdc",

    .expected_rc = 0,

    .expected = {
        .host   = "host1.local",
        .port   = -1,
        .domain = "/domain/path",

        .flow_ids = (const char *[]) {
            "3645f1e2-90fc-49cb-af9c-236c2ac124cd",
            "876fc244-9c99-48a8-a461-927e43a05bdc",
        },

        .nb_flow_ids = 2,
    },
};

static test_case percent_encoded_key = {
    .name = "percent_encoded_key",

    .uri =
    "mxl://host1.local/domain/path?"
    "%69d=3645f1e2-90fc-49cb-af9c-236c2ac124cd",

    .expected_rc = 0,

    .expected = {
        .host   = "host1.local",
        .port   = -1,
        .domain = "/domain/path",
        .flow_ids = NULL,
        .nb_flow_ids = 0,
    },
};

static test_case trailing_amp_query_param = {
    .name = "trailing_amp_query_param",

    .uri =
        "mxl://host1.local/domain/path?"
        "id=3645f1e2-90fc-49cb-af9c-236c2ac124cd&",

    .expected_rc = 0,

    .expected = {
        .host   = "host1.local",
        .port   = -1,
        .domain = "/domain/path",

        .flow_ids = (const char *[]){
            "3645f1e2-90fc-49cb-af9c-236c2ac124cd",
        },

        .nb_flow_ids = 1,
    },
};

static test_case userinfo_in_authority = {
    .name = "userinfo_in_authority",

    .uri =
        "mxl://user@host1.local/domain/path?"
        "id=3645f1e2-90fc-49cb-af9c-236c2ac124cd",

    .expected_rc = AVERROR(EINVAL),

    .expected = expected_error_uri
};

static test_case userpass_in_authority = {
    .name = "userpass_in_authority",

    .uri =
        "mxl://user:pass@host1.local/domain/path?"
        "id=3645f1e2-90fc-49cb-af9c-236c2ac124cd",

    .expected_rc = AVERROR(EINVAL),

    .expected = expected_error_uri
};

static test_case multiple_at_userinfo = {
    .name = "multiple_at_userinfo",

    .uri =
        "mxl://a@b@host1.local/domain/path?"
        "id=3645f1e2-90fc-49cb-af9c-236c2ac124cd",

    .expected_rc = AVERROR(EINVAL),

    .expected = expected_error_uri
};

static test_case local_domain_triple_slash = {
    .name = "local_domain_triple_slash",

    .uri =
        "mxl:///domain/path?"
        "id=3645f1e2-90fc-49cb-af9c-236c2ac124cd",

    .expected_rc = 0,

    .expected = {
        .host   = "",
        .port   = -1,
        .domain = "/domain/path",

        .flow_ids = (const char *[]) {
            "3645f1e2-90fc-49cb-af9c-236c2ac124cd",
        },

        .nb_flow_ids = 1,
    },
};

static test_case local_domain_looks_like_port_but_is_not = {
    .name = "local_domain_looks_like_port_but_is_not",

    .uri =
        "mxl:///domain/path:5000?"
        "id=3645f1e2-90fc-49cb-af9c-236c2ac124cd",

    .expected_rc = 0,

    .expected = {
        .host   = "",
        .port   = -1,
        .domain = "/domain/path:5000",

        .flow_ids = (const char *[]) {
            "3645f1e2-90fc-49cb-af9c-236c2ac124cd",
        },

        .nb_flow_ids = 1,
    },
};

static test_case local_domain_single_slash = {
    .name = "local_domain_single_slash",

    .uri =
        "mxl:/domain/path?"
        "id=3645f1e2-90fc-49cb-af9c-236c2ac124cd",

    .expected_rc = AVERROR(EINVAL),

    .expected = expected_error_uri
};

static test_case userinfo_with_empty_host = {
    .name = "userinfo_with_empty_host",

    .uri =
        "mxl://user@/domain/path?"
        "id=3645f1e2-90fc-49cb-af9c-236c2ac124cd",

    .expected_rc = AVERROR(EINVAL),

    .expected = expected_error_uri
};

static test_case empty_host_with_port = {
    .name = "empty_host_with_port",

    .uri =
    "mxl://:5000/domain/path?"
    "id=3645f1e2-90fc-49cb-af9c-236c2ac124cd",

    .expected_rc = 0,

    .expected = {
        .host   = "",
        .port   = 5000,
        .domain = "/domain/path",

        .flow_ids = (const char *[]) {
            "3645f1e2-90fc-49cb-af9c-236c2ac124cd",
        },

        .nb_flow_ids = 1,
    },
};

static test_case query_without_path = {
    .name = "query_without_path",

    .uri =
        "mxl://?"
        "id=3645f1e2-90fc-49cb-af9c-236c2ac124cd",

    .expected_rc = AVERROR(EINVAL),

    .expected = expected_error_uri
};

static test_case double_leading_slash_path = {
    .name = "double_leading_slash_path",

    .uri =
        "mxl:////domain/path?"
        "id=3645f1e2-90fc-49cb-af9c-236c2ac124cd",

    .expected_rc = 0,

    .expected = {
        .host   = "",
        .port   = -1,
        .domain = "//domain/path",

        .flow_ids = (const char *[]) {
            "3645f1e2-90fc-49cb-af9c-236c2ac124cd",
        },

        .nb_flow_ids = 1,
    },
};

static test_case empty_ipv6_host = {
    .name = "empty_ipv6_host",

    .uri =
        "mxl://[]/domain/path?"
        "id=3645f1e2-90fc-49cb-af9c-236c2ac124cd",

    .expected_rc = AVERROR(EINVAL),

    .expected = expected_error_uri
};

static test_case empty_ipv6_host_with_port = {
    .name = "empty_ipv6_host_with_port",

    .uri =
        "mxl://[]:5000/domain/path?"
        "id=3645f1e2-90fc-49cb-af9c-236c2ac124cd",

    .expected_rc = AVERROR(EINVAL),

    .expected = expected_error_uri
};

static test_case empty_domain_path = {
    .name = "empty_domain_path",

    .uri =
        "mxl://host1.local/?"
        "id=3645f1e2-90fc-49cb-af9c-236c2ac124cd",

    .expected_rc = AVERROR(EINVAL),

    .expected = expected_error_uri
};

static test_case empty_domain_path_no_query = {
    .name = "empty_domain_path_no_query",

    .uri = "mxl://host1.local",

    .expected_rc = AVERROR(EINVAL),

    .expected = expected_error_uri
};

static test_case empty_domain_path_no_query_trailing_slash = {
    .name = "empty_domain_path_no_query_trailing_slash",

    .uri = "mxl://host1.local/",

    .expected_rc = AVERROR(EINVAL),

    .expected = expected_error_uri
};

static const test_case *tests[] = {
    // happy path cases
    &zero_id,
    &one_id,
    &two_id,
    &three_id,
    &two_id_upcase,
    &two_id_upcase_key,
    &unknown_query_param,
    &unknown_flag_query_param,
    &ipv4,
    &ipv6,
    &ipv6_with_port,
    &valid_negative_one_port,
    &only_unknown_query_param,
    &local_domain_triple_slash,
    &local_domain_single_slash,

    // boundary cases
    &zero_port,
    &fragment_after_query,
    &valid_at_range_limit_port,
    &invalid_out_of_range_port,
    &empty_query,
    &leading_amp_query_param,
    &double_separator_query_param,
    &percent_encoded_in_other_query_value,
    &percent_encoded_key,
    &trailing_amp_query_param,
    &local_domain_looks_like_port_but_is_not,
    &empty_host_with_port,
    &userinfo_with_empty_host,
    &double_leading_slash_path,
    &query_without_path,
    &empty_ipv6_host,
    &empty_ipv6_host_with_port,
    &empty_domain_path,
    &empty_domain_path_no_query,
    &empty_domain_path_no_query_trailing_slash,

    // aberrant cases
    &bad_scheme,
    &no_scheme,
    &null_uri,
    &empty_uri,
    &equal_max_scheme,
    &greater_max_scheme,
    &empty_middle_id_query_param,
    &empty_end_id_query_param,
    &bad_separator_query_param,
    &bad_uuid_query_param,
    &invalid_negative_port,
    &invalid_empty_port,
    &invalid_non_integer_port,
    &invalid_no_domain_path,
    &malformed_ipv6_bracket,
    &percent_encoded_in_uuid_query_value,
    &userinfo_in_authority,
    &userpass_in_authority,
    &multiple_at_userinfo
};

static int run_uri_limit_test(const char *name,
                              size_t uri_len,
                              int expected_rc)
{
    static char uri_buf[MXL_URI_MAX + 2];

    const char *prefix =
        "mxl://host1.local/domain/path?"
        "id=3645f1e2-90fc-49cb-af9c-236c2ac124cd&"
        "id=876fc244-9c99-48a8-a461-927e43a05bdc&"
        "junk=";

    size_t p = strlen(prefix);

    av_assert0(uri_len >= p);
    av_assert0(uri_len < sizeof(uri_buf));

    memcpy(uri_buf, prefix, p);

    for (size_t i = p; i < uri_len; i++)
        uri_buf[i] = 'a';

    uri_buf[uri_len] = '\0';

    test_case tc = {
        .name = name,
        .uri = uri_buf,
        .expected_rc = expected_rc,
    };

    if (expected_rc == 0) {
        tc.expected.host = "host1.local";
        tc.expected.port = -1;
        tc.expected.domain = "/domain/path";

        static const char *ids[] = {
            "3645f1e2-90fc-49cb-af9c-236c2ac124cd",
            "876fc244-9c99-48a8-a461-927e43a05bdc"
        };

        tc.expected.flow_ids = ids;
        tc.expected.nb_flow_ids = 2;
    } else {
        tc.expected = expected_error_uri;
    }

    return run_test_case(&tc);
}

static int test_mxl_uri_free(void) {

    // sanity check freeing a null
    mxl_uri_free(NULL);

    // Parse a known good uri that sets all fields, validate, then free.
    mxl_uri parsed = {0};
    int rc = mxl_parse_uri(NULL, ipv4.uri, &parsed);
    if (rc)
        return rc;
    rc = compare_uri(&ipv4.expected, &parsed);
    if (rc)
        return rc;

    // every field should be set
    if (parsed.host == NULL || parsed.port == 0 || parsed.port == 1 ||
        parsed.domain == NULL ||
        parsed.flow_ids == NULL || parsed.nb_flow_ids == 0) {
        fprintf(stderr, "==== test_mxl_uri_free FAIL: parsed uri not set\n");
        return -1;
    }

    mxl_uri_free(&parsed);

    // every field should be clear
    if (parsed.host != NULL || parsed.port != 0 || parsed.domain != NULL ||
        parsed.flow_ids != NULL || parsed.nb_flow_ids != 0) {
        fprintf(stderr, "==== test_mxl_uri_free FAIL: freed mxl_uri not cleared\n");
        return -1;
    }

    return 0;
}


int main(void) {

    printf("testing mxl_uri: mxl URI parser\n");

    // av_log_set_level(AV_LOG_QUIET);
    av_log_set_level(AV_LOG_ERROR);

    int rc = -1;

    // FFmpeg expects C locale, this URI parser depends on that.
    if (!setlocale(LC_ALL, "C"))
        return -1;

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        rc = run_test_case(tests[i]);
        if (rc)
            goto finally;
    }

    // URI max length limit tests use generated URI, hence are separate.
    rc = run_uri_limit_test("equal_max_len_url", MXL_URI_MAX, 0);
    if (rc)
        goto finally;
    rc = run_uri_limit_test("greater_max_len_url", MXL_URI_MAX + 1, AVERROR(ENAMETOOLONG));
    if (rc)
        goto finally;

    // test freeing a mxl_uri parse result
    rc = test_mxl_uri_free();

finally:

    if (rc)
        printf("\nFAIL\n");
    else
        printf("\nPASS\n");

    return rc;
}
