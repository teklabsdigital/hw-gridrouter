/*
 * c_abi_smoke.c: proves rv.h is plain C99 and that the C++ core links and
 * runs when called from C.  Built with cc -std=c99 -Wall -Wextra -Wpedantic
 * -Werror and linked against the core objects (see the Makefile).
 */
#include "rv.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    rv_session *s = NULL;
    rv_status st;
    int failures = 0;
    rv_trace_info info;
    rv_chunk_info chunk;
    rv_issue_info issue;
    rv_run_info run;
    rv_search_info search;
    const uint32_t *cells = NULL;
    const int32_t *costs = NULL;
    const char *text = NULL;
    uint32_t count = 0;
    int32_t code = 0;

    if (rv_api_version() != RV_API_VERSION) {
        fprintf(stderr, "c_abi_smoke: api version %u, header %u\n",
                (unsigned)rv_api_version(), (unsigned)RV_API_VERSION);
        ++failures;
    }
    st = rv_create(&s);
    if (st != RV_OK || s == NULL) {
        fprintf(stderr, "c_abi_smoke: rv_create returned %d\n", (int)st);
        ++failures;
    } else if (strcmp(rv_last_error(s), "") != 0) {
        fprintf(stderr, "c_abi_smoke: fresh session has an error: %s\n",
                rv_last_error(s));
        ++failures;
    }

    /* The trace functions, called from C.  No trace is open: every query is
     * rejected with a message, and opening a missing file is an I/O error. */
    if (rv_open(s, "/nonexistent/dir/missing.grtrace") != RV_E_IO) ++failures;
    if (rv_last_error(s)[0] == '\0') ++failures;
    if (rv_info(s, &info) != RV_E_INVALID_ARGUMENT) ++failures;
    if (rv_refresh(s, &count) != RV_E_INVALID_ARGUMENT || count != 0) ++failures;
    if (rv_kind_count(s, RV_CHUNK_SRCH, &count) != RV_E_INVALID_ARGUMENT) ++failures;
    if (rv_chunk(s, 0, &chunk) != RV_E_INVALID_ARGUMENT) ++failures;
    if (rv_chunk_text(s, 0, &text) != RV_E_INVALID_ARGUMENT || text != NULL) ++failures;
    if (rv_issue(s, 0, &issue) != RV_E_INVALID_ARGUMENT) ++failures;
    if (rv_run(s, 0, &run) != RV_E_INVALID_ARGUMENT) ++failures;
    if (rv_net(s, 0, &code, &text) != RV_E_INVALID_ARGUMENT) ++failures;
    if (rv_search(s, 0, &search) != RV_E_INVALID_ARGUMENT) ++failures;
    if (rv_search_cells(s, 0, 0, &cells, &count) != RV_E_INVALID_ARGUMENT) ++failures;
    if (rv_wavefront(s, 0, &cells, &costs, &count) != RV_E_INVALID_ARGUMENT) ++failures;
    if (cells != NULL || costs != NULL || count != 0) ++failures;
    if (rv_close(s) != RV_OK) ++failures;
    if (strcmp(rv_kind_tag(RV_CHUNK_BRD), "BRD ") != 0) ++failures;
    if (strcmp(rv_kind_tag(RV_CHUNK_KIND_COUNT), "????") != 0) ++failures;
    if (RV_NONE != 0xFFFFFFFFu || (RV_SRCH_HAS_TRACE >> 9) != 1u) ++failures;

    rv_destroy(s);
    if (rv_create(NULL) != RV_E_INVALID_ARGUMENT) ++failures;
    rv_destroy(NULL);

    printf("c_abi_smoke: %s\n", failures == 0 ? "ok" : "FAILED");
    return failures == 0 ? 0 : 1;
}
