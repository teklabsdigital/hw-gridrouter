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
    rv_destroy(s);
    if (rv_create(NULL) != RV_E_INVALID_ARGUMENT) ++failures;
    rv_destroy(NULL);

    printf("c_abi_smoke: %s\n", failures == 0 ? "ok" : "FAILED");
    return failures == 0 ? 0 : 1;
}
