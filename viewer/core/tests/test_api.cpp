// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Trevor Attema, Teklabs Digital
//
// test_api.cpp: the session lifecycle through the C interface.

#include "check.hpp"

#include "rv.h"

TEST(api_version_matches_header) {
    CHECK_EQ(rv_api_version(), RV_API_VERSION);
}

TEST(status_values_are_stable) {
    // The numeric values are part of the ABI that every client relies on.
    CHECK_EQ(static_cast<int>(RV_OK), 0);
    CHECK_EQ(static_cast<int>(RV_E_INVALID_ARGUMENT), 1);
    CHECK_EQ(static_cast<int>(RV_E_NO_MEMORY), 2);
    CHECK_EQ(static_cast<int>(RV_E_INTERNAL), 3);
}

TEST(create_and_destroy) {
    rv_session *s = nullptr;
    CHECK_EQ(rv_create(&s), RV_OK);
    CHECK(s != nullptr);
    rv_destroy(s);
}

TEST(sessions_are_independent) {
    rv_session *a = nullptr;
    rv_session *b = nullptr;
    CHECK_EQ(rv_create(&a), RV_OK);
    CHECK_EQ(rv_create(&b), RV_OK);
    CHECK(a != nullptr);
    CHECK(b != nullptr);
    CHECK(a != b);
    rv_destroy(a);
    CHECK_STREQ(rv_last_error(b), "");
    rv_destroy(b);
}

TEST(last_error_on_fresh_session_is_empty) {
    rv_session *s = nullptr;
    CHECK_EQ(rv_create(&s), RV_OK);
    const char *msg = rv_last_error(s);
    CHECK(msg != nullptr);
    CHECK_STREQ(msg, "");
    rv_destroy(s);
}

TEST(create_with_null_out_is_rejected) {
    CHECK_EQ(rv_create(nullptr), RV_E_INVALID_ARGUMENT);
}

TEST(destroy_null_is_a_no_op) {
    rv_destroy(nullptr);
    CHECK(true);
}

TEST(last_error_of_null_session_is_a_message) {
    const char *msg = rv_last_error(nullptr);
    CHECK(msg != nullptr);
    CHECK(msg != nullptr && msg[0] != '\0');
}

TEST(many_sessions) {
    for (int i = 0; i < 1000; ++i) {
        rv_session *s = nullptr;
        if (rv_create(&s) != RV_OK || s == nullptr) {
            CHECK(false);
            return;
        }
        rv_destroy(s);
    }
    CHECK(true);
}
