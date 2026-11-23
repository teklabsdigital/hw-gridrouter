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

    /* M2, the board and the camera, from C with no trace open. */
    {
        rv_display_list list;
        rv_layer_info layer;
        rv_board_info board;
        rv_view view;
        double bx = 1.0, by = 1.0;
        uint32_t visible = 0;
        if (rv_board_count(s, &count) != RV_OK || count != 0) ++failures;
        if (rv_board(s, 0, &board) != RV_E_INVALID_ARGUMENT) ++failures;
        if (rv_board_select(s, RV_NONE) != RV_OK) ++failures;
        if (rv_board_selected(s, &count) != RV_OK || count != RV_NONE) ++failures;
        if (rv_layer_count(s, &count) != RV_OK || count != 0) ++failures;
        if (rv_layer(s, 0, &layer) != RV_E_INVALID_ARGUMENT) ++failures;
        if (rv_layer_set_visible(s, "F.Cu", 1) != RV_OK) ++failures;
        if (rv_class_count(s, &count) != RV_OK || count != 12) ++failures;
        if (rv_class(s, RV_OBJ_VIAS, &text, &visible) != RV_OK || strcmp(text, "Vias") != 0) ++failures;
        if (rv_class_set_visible(s, RV_OBJ_TEXT, 0) != RV_OK) ++failures;
        if (rv_get_display_list(s, &list) != RV_OK || list.batch_count != 0) ++failures;
        if (list.background != 0x001023FFu) ++failures;
        if (rv_order_key(s, RV_NONE, RV_SUB_PAD_HOLE, &count) != RV_OK) ++failures;
        if (rv_view_set_size(s, 640.0, 480.0, 2.0) != RV_OK) ++failures;
        if (rv_view_fit(s) != RV_OK || rv_view_zoom(s, 2.0, 0.0, 0.0) != RV_OK) ++failures;
        if (rv_view_pan(s, 5.0, 5.0) != RV_OK || rv_view_get(s, &view) != RV_OK) ++failures;
        if (view.scale != 20.0 || view.backing != 2.0) ++failures;
        if (rv_view_to_board(s, 5.0, 5.0, &bx, &by) != RV_OK) ++failures;
        if (rv_board_to_view(s, bx, by, &bx, &by) != RV_OK || bx != 5.0 || by != 5.0) ++failures;
        if (rv_view_status(s, 0.0, 0.0, &text) != RV_OK || text == NULL) ++failures;
        if (rv_view_zoom(s, 0.0, 0.0, 0.0) != RV_E_INVALID_ARGUMENT) ++failures;
    }

    /* M3, the timeline and playback, from C with no trace open. */
    {
        rv_timeline tl;
        rv_position pos;
        rv_phase_info phase;
        rv_display_list list;
        const rv_slot *slots = NULL;
        uint32_t changed = 9;
        if (rv_get_timeline(s, &tl) != RV_OK || tl.event_count != 0 || tl.events != NULL) ++failures;
        if (rv_phase(s, 0, &phase) != RV_E_INVALID_ARGUMENT) ++failures;
        if (rv_position_get(s, &pos) != RV_OK || pos.event != RV_NONE || pos.playing != 0) ++failures;
        if (rv_play(s, 1, &changed) != RV_OK || changed != 0) ++failures;
        if (rv_advance(s, 0.5, &changed) != RV_OK || changed != 0) ++failures;
        if (rv_advance(s, -1.0, &changed) != RV_E_INVALID_ARGUMENT) ++failures;
        if (rv_set_speed(s, 3.0) != RV_OK || rv_speed_step(s, 1) != RV_OK) ++failures;
        if (rv_set_direction(s, -1) != RV_OK || rv_set_direction(s, 0) != RV_E_INVALID_ARGUMENT) ++failures;
        if (rv_seek_fraction(s, 0.5, &changed) != RV_OK) ++failures;
        if (rv_seek_event(s, 0, &changed) != RV_E_INVALID_ARGUMENT) ++failures;
        if (rv_step(s, RV_STEP_COMMIT, 1, &changed) != RV_OK || changed != 0) ++failures;
        if (rv_jump(s, 1, NULL) != RV_OK) ++failures;
        if (rv_text(s, RV_TEXT_SPEED, &text) != RV_OK || strcmp(text, "8\xc3\x97") != 0) ++failures;
        if (rv_text(s, RV_TEXT_CAPTION, &text) != RV_OK || text == NULL) ++failures;
        if (rv_get_overlay(s, &list, &slots, &count) != RV_OK || list.batch_count != 0 || count != 0) ++failures;
        if (strcmp(rv_event_kind_name(RV_EV_RIPUP), "rip-up") != 0) ++failures;
        if (strcmp(rv_step_kind_name(RV_STEP_KIND_COUNT), "") != 0) ++failures;
        if (rv_order_key(s, RV_OVERLAY_LAYER, 0x10, &count) != RV_OK || count != 1000u * 256u + 0x10u) ++failures;
    }

    /* M4, the wavefront images and pacing, from C with no trace open. */
    {
        rv_wavefront_image img;
        rv_pacing pacing;
        uint32_t changed = 9;
        memset(&img, 0, sizeof img);
        if (rv_wavefront_image_count(s, &count) != RV_OK || count != 0) ++failures;
        if (rv_get_wavefront_image(s, 0, &img) != RV_OK || img.pixels != NULL || img.width != 0) ++failures;
        if (rv_get_wavefront_image(s, 2, &img) != RV_E_INVALID_ARGUMENT) ++failures;
        if (rv_get_pacing(s, &pacing) != RV_OK || pacing.mode != RV_PACING_FIT) ++failures;
        if (pacing.fit_seconds != 180.0 || pacing.slowdown != 10.0) ++failures;
        pacing.mode = RV_PACING_REAL_TIME;
        pacing.slowdown = 2.0;
        if (rv_set_pacing(s, &pacing, &changed) != RV_OK) ++failures;
        if (rv_get_pacing(s, &pacing) != RV_OK || pacing.mode != RV_PACING_REAL_TIME) ++failures;
        pacing.slowdown = 0.0;
        if (rv_set_pacing(s, &pacing, &changed) != RV_E_INVALID_ARGUMENT) ++failures;
        pacing.slowdown = 2.0;
        pacing.mode = RV_PACING_MODE_COUNT;
        if (rv_set_pacing(s, &pacing, NULL) != RV_E_INVALID_ARGUMENT) ++failures;
        if (rv_text(s, RV_TEXT_PACING, &text) != RV_OK || strncmp(text, "real time", 9) != 0) ++failures;
        if (strcmp(rv_pacing_mode_name(RV_PACING_NETS_ONLY), "nets only") != 0) ++failures;
        if (strcmp(rv_pacing_mode_name(RV_PACING_MODE_COUNT), "") != 0) ++failures;
        if (RV_CHANGED_WAVEFRONT != 8u || RV_SLOT_WAVEFRONT != 3u || RV_OBJ_WAVEFRONT != 11) ++failures;
    }

    /* M5, inspection, from C with no trace open. */
    {
        rv_nets_info nets;
        rv_net_row row;
        rv_selection sel;
        rv_hit hit;
        rv_history_entry entry;
        const uint32_t *events = NULL;
        uint32_t changed = 9;
        if (rv_nets_get(s, &nets) != RV_OK || nets.row_count != 0 || nets.view_count != 0) ++failures;
        if (nets.selected_position != RV_NONE || nets.sort_column != RV_NETCOL_NAME) ++failures;
        if (rv_nets_set_view(s, RV_NETCOL_SEARCHES, 1, "sig") != RV_OK) ++failures;
        if (rv_nets_set_view(s, RV_NETCOL_COUNT, 0, NULL) != RV_E_INVALID_ARGUMENT) ++failures;
        if (rv_nets_row(s, 0, &row) != RV_E_INVALID_ARGUMENT) ++failures;
        if (rv_nets_cell(s, 0, RV_NETCOL_NAME, &text) != RV_E_INVALID_ARGUMENT) ++failures;
        if (strcmp(rv_net_column_title(RV_NETCOL_FAILED_PADS), "Failed pads") != 0) ++failures;
        if (rv_net_column_align(RV_NETCOL_NAME) != 0 || rv_net_column_align(RV_NETCOL_VIAS) != 1) ++failures;
        if (strcmp(rv_net_state_name(RV_NET_RIPPED), "ripped") != 0) ++failures;
        if (strcmp(rv_net_state_name(RV_NET_STATE_COUNT), "") != 0) ++failures;
        if (rv_select_name(s, "/SIG", &changed) != RV_OK || (changed & RV_CHANGED_SELECTION) == 0) ++failures;
        if (rv_selection_get(s, &sel) != RV_OK || sel.active != 1 || sel.has_net != 0) ++failures;
        if (strcmp(sel.name, "/SIG") != 0 || sel.board_nets != 0) ++failures;
        if (rv_select_net(s, 3, &changed) != RV_OK || rv_selection_get(s, &sel) != RV_OK) ++failures;
        if (sel.has_net != 1 || sel.net != 3) ++failures;
        if (rv_select_clear(s, &changed) != RV_OK || rv_selection_get(s, &sel) != RV_OK || sel.active != 0) ++failures;
        if (rv_hit_test(s, 10.0, 10.0, &count) != RV_OK || count != 0) ++failures;
        if (rv_hit_get(s, 0, &hit) != RV_E_INVALID_ARGUMENT) ++failures;
        if (rv_hover_status(s, 10.0, 10.0, &text) != RV_OK || text == NULL) ++failures;
        if (rv_click(s, 10.0, 10.0, NULL) != RV_OK) ++failures;
        if (rv_net_history(s, 1, &count) != RV_OK || count != 0) ++failures;
        if (rv_history_get(s, 0, &entry) != RV_E_INVALID_ARGUMENT) ++failures;
        if (rv_net_events(s, 1, &events, &count) != RV_OK || count != 0) ++failures;
        if (RV_SLOT_DIM != 4u || RV_SLOT_HIGHLIGHT != 5u || RV_CHANGED_TIMELINE != 32u) ++failures;
        if (RV_PICK_POINTS != 3.0 || RV_HIT_WAVEFRONT != 7u || RV_HIST_RIP_OTHER != 10u) ++failures;
    }

    /* M6, following a growing file, from C with no trace open. */
    {
        rv_live live;
        uint32_t changed = 7;
        if (rv_open_ex(s, "/nonexistent/dir/missing.grtrace", RV_OPEN_LIVE) != RV_E_IO) ++failures;
        if (rv_open_ex(s, "/nonexistent/dir/missing.grtrace", 8u) != RV_E_INVALID_ARGUMENT) ++failures;
        if (rv_reload(s) != RV_E_INVALID_ARGUMENT) ++failures;
        if (rv_poll(s, 1.0, &count, &changed) != RV_E_INVALID_ARGUMENT || count != 0 || changed != 0) ++failures;
        if (rv_live_get(s, &live) != RV_OK || live.state != RV_LIVE_NONE || live.following != 0) ++failures;
        if (rv_live_text(s, &text) != RV_OK || text == NULL || text[0] != '\0') ++failures;
        if (rv_set_following(s, 1, &changed) != RV_OK || changed != 0) ++failures;
        if (rv_live_get(NULL, &live) != RV_E_INVALID_ARGUMENT) ++failures;
        if (RV_E_REPLACED != 7 || RV_CHANGED_LIVE != 64u || RV_LIVE_FAILED != 7) ++failures;
        if (RV_FOLLOW_LAG != 2.0 || RV_FOLLOW_CATCH_UP != 0.5 || RV_FOLLOW_SETTLED != 0.9) ++failures;
        if (RV_LIVE_STALL_SECONDS != 5.0 || RV_OPEN_WAIT != 1u || RV_OPEN_LIVE != 2u) ++failures;
        if (RV_LIVE_FIT_WEIGHT != 30000.0) ++failures;
    }

    if (strcmp(rv_kind_tag(RV_CHUNK_BRD), "BRD ") != 0) ++failures;
    if (strcmp(rv_kind_tag(RV_CHUNK_KIND_COUNT), "????") != 0) ++failures;
    if (RV_NONE != 0xFFFFFFFFu || (RV_SRCH_HAS_TRACE >> 9) != 1u) ++failures;

    rv_destroy(s);
    if (rv_create(NULL) != RV_E_INVALID_ARGUMENT) ++failures;
    rv_destroy(NULL);

    printf("c_abi_smoke: %s\n", failures == 0 ? "ok" : "FAILED");
    return failures == 0 ? 0 : 1;
}
