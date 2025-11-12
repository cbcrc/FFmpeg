/*
 * MXL locator tests for Media eXchange Layer flows
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

#include "libavformat/mxl_loc.h"
#include "libavutil/error.h"
#include "libavutil/avassert.h"
#include "libavutil/log.h"

#include <stdio.h>
#include <string.h>
#include <locale.h>

typedef struct mxl_loc_const {
    const char *domain_path;
    const char * const *flow_ids;
    int nb_flow_ids;
} mxl_loc_const;

typedef struct test_case {
    const char *name;
    const char *locator;
    int expected_rc;
    mxl_loc_const expected;
} test_case;

static void dump_locator(const mxl_loc *loc)
{
    if (!loc) {
        printf("mxl_loc : (null)\n");
        return;
    }

    printf("mxl_loc {\n");
    printf("  domain_path: \"%s\"\n", loc->domain_path ? loc->domain_path : "(null)");
    printf("  nb_flow_ids: %d\n", loc->nb_flow_ids);
    printf("  flow_ids: [\n");
    for (int i = 0; i < loc->nb_flow_ids; i++)
        printf("             \"%s\"\n", loc->flow_ids[i] ? loc->flow_ids[i] : "(null)");
    printf("            ]\n");
    printf("}\n");
}

static void dump_expected_locator(const mxl_loc_const *expected) {
    mxl_loc loc = {
        .domain_path = (char *)expected->domain_path,
        .flow_ids = (char **)expected->flow_ids,
        .nb_flow_ids = expected->nb_flow_ids
    };

    dump_locator(&loc);
}

static void dump_test_case(const test_case *tc, int actual_rc, const mxl_loc *actual)
{
    printf("==== test case: %s\n", tc->name);

    printf("locator: \"%s\"\n", tc->locator ? tc->locator : "(null)");
    printf("expected return code: %d\n", tc->expected_rc);
    printf("actual return code: %d\n", actual_rc);

    printf("expected locator:\n");
    dump_expected_locator(&tc->expected);
    printf("actual locator:\n");
    dump_locator(actual);
}

static int compare_loc(const mxl_loc_const *expected, const mxl_loc *actual)
{
    av_assert0(expected && actual);

    if ((expected->domain_path == NULL) != (actual->domain_path == NULL)) {
        fprintf(stderr, "compare_loc: domain_path NULL mismatch\n");
        return -1;
    }

    if (expected->domain_path && actual->domain_path &&
        strcmp(expected->domain_path, actual->domain_path) != 0) {
        fprintf(stderr, "compare_loc: domain_path mismatch: expected '%s' actual '%s'\n",
                expected->domain_path, actual->domain_path);
        return -1;
    }

    if ((expected->flow_ids == NULL) != (actual->flow_ids == NULL)) {
        fprintf(stderr, "compare_loc: flow_ids NULL mismatch\n");
        return -1;
    }

    if (expected->nb_flow_ids != actual->nb_flow_ids) {
        fprintf(stderr, "compare_loc: nb_flow_ids mismatch: expected %d actual %d\n",
                expected->nb_flow_ids, actual->nb_flow_ids);
        return -1;
    }

    for (int i = 0; i < expected->nb_flow_ids; i++) {
        const char *e = expected->flow_ids[i];
        const char *a = actual->flow_ids[i];

        if (e == NULL || a == NULL) {
            fprintf(stderr, "compare_loc: flow_id[%d] NULL: expected %s actual %s\n",
                    i,
                    e ? e : "(null)",
                    a ? a : "(null)");
            return -1;
        }

        if (strcmp(e, a) != 0) {
            fprintf(stderr, "compare_loc: flow_id[%d] mismatch: expected '%s' actual '%s'\n",
                    i, e, a);
            return -1;
        }
    }

    return 0;
}

/* returns 0 if invariants are satisfied */
static int verify_invariants(int parse_rc, const mxl_loc* actual) {
    if (!actual)
        return -1;

    if (!parse_rc) {
        /* parse success invariants */
        if (actual->domain_path &&
            actual->nb_flow_ids >= 0 &&
            (actual->nb_flow_ids == 0 ? actual->flow_ids == NULL : actual->flow_ids != NULL)) {
            return 0;
        }
    }
    else {
        /* parse failure invariants */
        if (!actual->domain_path &&
            actual->nb_flow_ids == 0 && actual->flow_ids == NULL) {
            return 0;
        }
    }

    return -1;
}

static int run_test_case(const test_case *tc)
{
    mxl_loc actual = {0};
    int test_case_rc = -1;

    int parse_rc = mxl_loc_parse(NULL, tc->locator, &actual);

    dump_test_case(tc, parse_rc, &actual);

    int invariants_rc = verify_invariants(parse_rc, &actual);
    if (invariants_rc != 0) {
        fprintf(stderr,
                "==== test '%s' FAIL: parsed locator invariants violated\n",
                tc->name);
        goto finally;
    }

    int compare_rc = compare_loc(&tc->expected, &actual);
    if (compare_rc != 0) {
        fprintf(stderr,
                "==== test '%s' FAIL: parsed locator mismatch\n",
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
        mxl_loc_free(&actual);

    return test_case_rc;
}

// expected mxl_loc value on error
static const mxl_loc_const expected_error_loc = {
    .domain_path = NULL,
    .flow_ids = NULL,
    .nb_flow_ids = 0,
};

static test_case abs_filesystem_path = {
    .name = "abs_filesystem_path",
    .locator = "/dev/shm/mxl/71f000dc-2578-4a06-b4ef-3e0e2b1e46e1.mxl-flow",
    .expected_rc = 0,
    .expected = {
        .domain_path = "/dev/shm/mxl",
        .flow_ids = (const char *[]) {
            "71f000dc-2578-4a06-b4ef-3e0e2b1e46e1"
        },
        .nb_flow_ids = 1
    }
};

static test_case abs_filesystem_path_root = {
    .name = "abs_filesystem_path_root",
    .locator = "/71f000dc-2578-4a06-b4ef-3e0e2b1e46e1.mxl-flow",
    .expected_rc = 0,
    .expected = {
        .domain_path = "/",
        .flow_ids = (const char *[]) {
            "71f000dc-2578-4a06-b4ef-3e0e2b1e46e1"
        },
        .nb_flow_ids = 1
    }
};

static test_case rel_filesystem_path = {
    .name = "rel_filesystem_path",
    .locator = "mxl/71f000dc-2578-4a06-b4ef-3e0e2b1e46e1.mxl-flow",
    .expected_rc = 0,
    .expected = {
        .domain_path = "mxl",
        .flow_ids = (const char *[]) {
            "71f000dc-2578-4a06-b4ef-3e0e2b1e46e1"
        },
        .nb_flow_ids = 1
    }
};

static test_case rel_filesystem_path_none = {
    .name = "rel_filesystem_path_none",
    .locator = "71f000dc-2578-4a06-b4ef-3e0e2b1e46e1.mxl-flow",
    .expected_rc = 0,
    .expected = {
        .domain_path = ".",
        .flow_ids = (const char *[]) {
            "71f000dc-2578-4a06-b4ef-3e0e2b1e46e1"
        },
        .nb_flow_ids = 1
    }
};

static test_case rel_filesystem_path_dot = {
    .name = "rel_filesystem_path_dot",
    .locator = "./71f000dc-2578-4a06-b4ef-3e0e2b1e46e1.mxl-flow",
    .expected_rc = 0,
    .expected = {
        .domain_path = ".",
        .flow_ids = (const char *[]) {
            "71f000dc-2578-4a06-b4ef-3e0e2b1e46e1"
        },
        .nb_flow_ids = 1
    }
};

static test_case rel_filesystem_path_dot_dot = {
    .name = "rel_filesystem_path_dot_dot",
    .locator = "../71f000dc-2578-4a06-b4ef-3e0e2b1e46e1.mxl-flow",
    .expected_rc = 0,
    .expected = {
        .domain_path = "..",
        .flow_ids = (const char *[]) {
            "71f000dc-2578-4a06-b4ef-3e0e2b1e46e1"
        },
        .nb_flow_ids = 1
    }
};

static test_case abs_filesystem_path_zero_flow = {
    .name = "abs_filesystem_path_zero_flow",
    .locator = "/tests/data/tmp/mxl",
    .expected_rc = 0,
    .expected = {
        .domain_path = "/tests/data/tmp/mxl",
        .flow_ids = NULL,
        .nb_flow_ids = 0
    }
};

static test_case abs_filesystem_no_flow = {
    .name = "abs_filesystem_no_flow",
    .locator = "/dev/shm/mxl",
    .expected_rc = 0,
    .expected = {
        .domain_path = "/dev/shm/mxl",
        .flow_ids = NULL,
        .nb_flow_ids = 0
    }
};

static test_case abs_filesystem_no_flow_root = {
    .name = "abs_filesystem_no_flow_root",
    .locator = "/",
    .expected_rc = 0,
    .expected = {
        .domain_path = "/",
        .flow_ids = NULL,
        .nb_flow_ids = 0
    }
};

static test_case rel_filesystem_no_flow = {
    .name = "rel_filesystem_no_flow",
    .locator = "rel/shm/mxl",
    .expected_rc = 0,
    .expected = {
        .domain_path = "rel/shm/mxl",
        .flow_ids = NULL,
        .nb_flow_ids = 0
    }
};

static test_case rel_filesystem_no_flow_pwd = {
    .name = "rel_filesystem_no_flow_pwd",
    .locator = ".",
    .expected_rc = 0,
    .expected = {
        .domain_path = ".",
        .flow_ids = NULL,
        .nb_flow_ids = 0
    }
};

static test_case rel_filesystem_path_trailing_slash = {
    .name = "rel_filesystem_path_trailing_slash",
    .locator = "mxl/",
    .expected_rc = 0,
    .expected = {
        .domain_path = "mxl/",
        .flow_ids = NULL,
        .nb_flow_ids = 0
    }
};

static test_case rel_filesystem_path_double_sep = {
    .name = "rel_filesystem_path_double_sep",
    .locator = "mxl//71f000dc-2578-4a06-b4ef-3e0e2b1e46e1.mxl-flow",
    .expected_rc = 0,
    .expected = {
        .domain_path = "mxl/",
        .flow_ids = (const char *[]) {
            "71f000dc-2578-4a06-b4ef-3e0e2b1e46e1"
        },
        .nb_flow_ids = 1
    }
};

static test_case abs_filesystem_path_wrong_ext = {
    .name = "abs_filesystem_path_wrong_ext",
    .locator = "/dev/shm/mxl/71f000dc-2578-4a06-b4ef-3e0e2b1e46e1.txt",
    .expected_rc = 0,
    .expected = {
        .domain_path = "/dev/shm/mxl/71f000dc-2578-4a06-b4ef-3e0e2b1e46e1.txt",
        .flow_ids = NULL,
        .nb_flow_ids = 0
    }
};

static test_case abs_filesystem_path_trailing_ext_junk = {
    .name = "abs_filesystem_path_trailing_ext_junk",
    .locator = "/dev/shm/mxl/71f000dc-2578-4a06-b4ef-3e0e2b1e46e1.mxl-flow.tmp",
    .expected_rc = 0,
    .expected = {
        .domain_path = "/dev/shm/mxl/71f000dc-2578-4a06-b4ef-3e0e2b1e46e1.mxl-flow.tmp",
        .flow_ids = NULL,
        .nb_flow_ids = 0
    }
};

static test_case abs_filesystem_path_no_ext = {
    .name = "abs_filesystem_path_no_ext",
    .locator = "/dev/shm/mxl/71f000dc-2578-4a06-b4ef-3e0e2b1e46e1",
    .expected_rc = 0,
    .expected = {
        .domain_path = "/dev/shm/mxl/71f000dc-2578-4a06-b4ef-3e0e2b1e46e1",
        .flow_ids = NULL,
        .nb_flow_ids = 0
    }
};

static test_case abs_filesystem_path_upcase_id = {
    .name = "abs_filesystem_path_upcase_id",
    .locator = "/dev/shm/mxl/71F000DC-2578-4A06-B4EF-3E0E2B1E46E1.mxl-flow",
    .expected_rc = 0,
    .expected = {
        .domain_path = "/dev/shm/mxl",
        .flow_ids = (const char *[]) {
            "71F000DC-2578-4A06-B4EF-3E0E2B1E46E1"
        },
        .nb_flow_ids = 1
    }
};

static test_case abs_filesystem_path_double_sep_a = {
    .name = "abs_filesystem_path_double_sep_a",
    .locator = "//dev/shm/mxl/71f000dc-2578-4a06-b4ef-3e0e2b1e46e1.mxl-flow",
    .expected_rc = 0,
    .expected = {
        .domain_path = "//dev/shm/mxl",
        .flow_ids = (const char *[]) {
            "71f000dc-2578-4a06-b4ef-3e0e2b1e46e1"
        },
        .nb_flow_ids = 1
    }
};

static test_case abs_filesystem_path_double_sep_b = {
    .name = "abs_filesystem_path_double_sep_b",
    .locator = "/dev/shm//mxl/71f000dc-2578-4a06-b4ef-3e0e2b1e46e1.mxl-flow",
    .expected_rc = 0,
    .expected = {
        .domain_path = "/dev/shm//mxl",
        .flow_ids = (const char *[]) {
            "71f000dc-2578-4a06-b4ef-3e0e2b1e46e1"
        },
        .nb_flow_ids = 1
    }
};

static test_case abs_filesystem_path_double_sep_c = {
    .name = "abs_filesystem_path_double_sep_c",
    .locator = "/dev/shm/mxl//71f000dc-2578-4a06-b4ef-3e0e2b1e46e1.mxl-flow",
    .expected_rc = 0,
    .expected = {
        .domain_path = "/dev/shm/mxl/",
        .flow_ids = (const char *[]) {
            "71f000dc-2578-4a06-b4ef-3e0e2b1e46e1"
        },
        .nb_flow_ids = 1
    }
};

static test_case abs_filesystem_directory_named_flow = {
    .name = "abs_filesystem_directory_named_flow",
    .locator = "/dev/shm/mxl/71f000dc-2578-4a06-b4ef-3e0e2b1e46e1.mxl-flow/",
    .expected_rc = 0,
    .expected = {
        .domain_path = "/dev/shm/mxl/71f000dc-2578-4a06-b4ef-3e0e2b1e46e1.mxl-flow/",
        .flow_ids = NULL,
        .nb_flow_ids = 0
    }
};

static test_case abs_filesystem_path_bad_id = {
    .name = "abs_filesystem_path_bad_id",
    .locator = "/dev/shm/mxl/bad_id.mxl-flow",
    .expected_rc = AVERROR(EINVAL),
    .expected = expected_error_loc,
};

static test_case abs_filesystem_path_missing_id = {
    .name = "abs_filesystem_path_missing_id",
    .locator = "/dev/shm/mxl/.mxl-flow",
    .expected_rc = AVERROR(EINVAL),
    .expected = expected_error_loc,
};

static test_case null_locator = {
    .name = "null_locator",
    .locator = NULL,
    .expected_rc = AVERROR(EINVAL),
    .expected = expected_error_loc,
};

static test_case empty_locator = {
    .name = "empty_locator",
    .locator = "",
    .expected_rc = AVERROR(ENOENT),
    .expected = expected_error_loc,
};

static test_case uri_zero_flow = {
    .name = "uri_zero_flow",
    .locator = "mxl:///dev/shm/mxl",
    .expected_rc = 0,
    .expected = {
        .domain_path = "/dev/shm/mxl",
        .flow_ids = NULL,
        .nb_flow_ids = 0
    }
};

static test_case uri_one_flow = {
    .name = "uri_one_flow",
    .locator = "mxl:///dev/shm/mxl?id=71f000dc-2578-4a06-b4ef-3e0e2b1e46e1",
    .expected_rc = 0,
    .expected = {
        .domain_path = "/dev/shm/mxl",
        .flow_ids = (const char *[]) {
            "71f000dc-2578-4a06-b4ef-3e0e2b1e46e1"
        },
        .nb_flow_ids = 1
    }
};

static test_case uri_two_flow = {
    .name = "uri_two_flow",
    .locator = "mxl:///dev/shm/mxl?"
               "id=71f000dc-2578-4a06-b4ef-3e0e2b1e46e1&"
               "id=ee43ad87-1ff5-43f5-a895-9cedbcc8884a",
    .expected_rc = 0,
    .expected = {
        .domain_path = "/dev/shm/mxl",
        .flow_ids = (const char *[]) {
            "71f000dc-2578-4a06-b4ef-3e0e2b1e46e1",
            "ee43ad87-1ff5-43f5-a895-9cedbcc8884a"
        },
        .nb_flow_ids = 2
    }
};

static const test_case *tests[] = {
    /* happy paths */
    &abs_filesystem_path,
    &abs_filesystem_path_root,
    &rel_filesystem_path,
    &rel_filesystem_path_none,
    &rel_filesystem_path_dot,
    &rel_filesystem_path_dot_dot,
    &abs_filesystem_no_flow,
    &abs_filesystem_no_flow_root,
    &rel_filesystem_no_flow,
    &rel_filesystem_no_flow_pwd,
    &rel_filesystem_path_trailing_slash,
    &rel_filesystem_path_double_sep,
    &abs_filesystem_path_zero_flow,
    &abs_filesystem_path_wrong_ext,
    &abs_filesystem_path_trailing_ext_junk,
    &abs_filesystem_path_no_ext,
    &abs_filesystem_path_upcase_id,
    &abs_filesystem_path_double_sep_a,
    &abs_filesystem_path_double_sep_b,
    &abs_filesystem_path_double_sep_c,
    &abs_filesystem_directory_named_flow,
    &uri_zero_flow,
    &uri_one_flow,
    &uri_two_flow,

    /* aberrant cases */
    &abs_filesystem_path_bad_id,
    &abs_filesystem_path_missing_id,
    &null_locator,
    &empty_locator,
};

int main(void) {
    printf("testing mxl_loc: mxl resource location parser\n\n");

    av_log_set_level(AV_LOG_QUIET);

    int rc = -1;

    // FFmpeg expects C locale, this URI parser depends on that.
    if (!setlocale(LC_ALL, "C"))
        return -1;

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        rc = run_test_case(tests[i]);
        if (rc)
            break;
    }

    if (rc)
        printf("\nFAIL\n");
    else
        printf("\nPASS\n");

    return rc;
}
