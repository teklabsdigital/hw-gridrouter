// test_trace_api.cpp: the trace reader through the C interface, on files.

#include "check.hpp"
#include "synth.hpp"
#include "trace_util.hpp"

#include "rv.h"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct SessionGuard {
    rv_session *s = nullptr;
    SessionGuard() { rv_create(&s); }
    ~SessionGuard() { rv_destroy(s); }
};

} // namespace

TEST(api_new_status_values_are_stable) {
    CHECK_EQ(static_cast<int>(RV_E_IO), 4);
    CHECK_EQ(static_cast<int>(RV_E_FORMAT), 5);
    CHECK_EQ(static_cast<int>(RV_E_UNSUPPORTED), 6);
    CHECK_EQ(static_cast<int>(RV_CHUNK_END), 19);
    CHECK_EQ(static_cast<int>(RV_CHUNK_KIND_COUNT), 20);
    CHECK_STREQ(rv_kind_tag(RV_CHUNK_RUN), "RUN ");
    CHECK_STREQ(rv_kind_tag(RV_CHUNK_SRCH), "SRCH");
    CHECK_STREQ(rv_kind_tag(RV_CHUNK_END), "END ");
    CHECK_STREQ(rv_kind_tag(RV_CHUNK_UNKNOWN), "????");
    CHECK_STREQ(rv_kind_tag(12345), "????");
}

TEST(api_queries_without_a_trace) {
    SessionGuard g;
    rv_trace_info info;
    CHECK_EQ(rv_info(g.s, &info), RV_E_INVALID_ARGUMENT);
    CHECK(std::strlen(rv_last_error(g.s)) > 0);
    CHECK_EQ(rv_refresh(g.s, nullptr), RV_E_INVALID_ARGUMENT);
    CHECK_EQ(rv_close(g.s), RV_OK);
    CHECK_STREQ(rv_last_error(g.s), "");
    CHECK_EQ(rv_open(nullptr, "x"), RV_E_INVALID_ARGUMENT);
    CHECK_EQ(rv_open(g.s, nullptr), RV_E_INVALID_ARGUMENT);
    CHECK_EQ(rv_info(nullptr, &info), RV_E_INVALID_ARGUMENT);
}

TEST(api_open_errors) {
    rvtest::TempDir dir;
    CHECK(dir.ok());
    SessionGuard g;
    CHECK_EQ(rv_open(g.s, dir.path("does-not-exist.grtrace").c_str()), RV_E_IO);
    CHECK(std::strlen(rv_last_error(g.s)) > 0);
    CHECK_EQ(rv_open(g.s, dir.path("").c_str()), RV_E_IO); // a directory

    const std::string junk = dir.path("junk.grtrace");
    const std::vector<unsigned char> text = {'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd', '\n'};
    CHECK(synth::write_file(junk, text));
    CHECK_EQ(rv_open(g.s, junk.c_str()), RV_E_FORMAT);

    const std::string v2 = dir.path("v2.grtrace");
    CHECK(synth::write_file(v2, synth::Writer(1.0, 2, 1, 0).out));
    CHECK_EQ(rv_open(g.s, v2.c_str()), RV_E_UNSUPPORTED);
    CHECK(std::string(rv_last_error(g.s)).find("2.1") != std::string::npos);
}

TEST(api_reads_the_sample) {
    rvtest::TempDir dir;
    CHECK(dir.ok());
    const synth::Sample s = synth::build_sample();
    const std::string path = dir.path("sample.grtrace");
    CHECK(synth::write_file(path, s.bytes));
    SessionGuard g;
    CHECK_EQ(rv_open(g.s, path.c_str()), RV_OK);
    CHECK_STREQ(rv_last_error(g.s), "");

    rv_trace_info info;
    CHECK_EQ(rv_info(g.s, &info), RV_OK);
    CHECK_EQ(info.header_complete, 1u);
    CHECK_EQ(info.version_major, 1u);
    CHECK_EQ(info.version_minor, 0u);
    CHECK_EQ(info.complete, 1u);
    CHECK_EQ(info.stopped, 0u);
    CHECK_EQ(info.failed, 0u);
    CHECK(info.open_time == s.open_time);
    CHECK_EQ(info.file_size, s.bytes.size());
    CHECK_EQ(info.indexed_end, s.bytes.size());
    CHECK_EQ(info.chunk_count, s.tags.size());
    CHECK_EQ(info.issue_count, 0u);
    CHECK_EQ(info.run_count, 2u);
    CHECK_EQ(info.net_count, 4u);
    CHECK_EQ(info.search_count, s.srchs.size());
    CHECK_EQ(info.route_count, 2u);
    CHECK_EQ(info.copper_count, 6u);
    CHECK_EQ(info.copper_removed, 1u);
    CHECK_EQ(info.failed_pads, 2u);
    CHECK_EQ(info.expanded_total, s.ends[0].expanded_total);
    CHECK_EQ(info.trace_entries, s.ends[0].trace_entries);
    CHECK(info.last_time == s.times.back());

    uint32_t n = 0;
    CHECK_EQ(rv_kind_count(g.s, RV_CHUNK_SRCH, &n), RV_OK);
    CHECK_EQ(n, s.srchs.size());
    CHECK_EQ(rv_kind_count(g.s, RV_CHUNK_UNKNOWN, &n), RV_OK);
    CHECK_EQ(n, 1u);
    CHECK_EQ(rv_kind_count(g.s, RV_CHUNK_KIND_COUNT, &n), RV_E_INVALID_ARGUMENT);

    for (uint32_t k = 0; k < info.chunk_count; ++k) {
        rv_chunk_info c;
        CHECK_EQ(rv_chunk(g.s, k, &c), RV_OK);
        CHECK_EQ(std::string(c.tag), s.tags[k]);
        CHECK_EQ(c.offset, s.chunk_offsets[k]);
        CHECK_EQ(c.corrupt, 0u);
        const char *text = nullptr;
        CHECK_EQ(rv_chunk_text(g.s, k, &text), RV_OK);
        CHECK(text != nullptr && text[0] != '\0');
    }
    rv_chunk_info c;
    CHECK_EQ(rv_chunk(g.s, info.chunk_count, &c), RV_E_INVALID_ARGUMENT);

    rv_run_info r;
    CHECK_EQ(rv_run(g.s, 0, &r), RV_OK);
    CHECK_EQ(r.valid, 1u);
    CHECK_EQ(r.nx, 6);
    CHECK_EQ(r.ny, 5);
    CHECK_EQ(r.nd, 8);
    CHECK(r.cell == 0.25);
    CHECK(r.origin_x == 100.5);
    CHECK_STREQ(r.layer_name[0], "F.Cu");
    CHECK_STREQ(r.layer_name[1], "B.Cu");
    CHECK_STREQ(r.producer, "synth test writer");
    CHECK_EQ(rv_run(g.s, 1, &r), RV_OK);
    CHECK_STREQ(r.layer_name[1], "In1.Cu");
    CHECK_EQ(rv_run(g.s, 2, &r), RV_E_INVALID_ARGUMENT);

    int32_t code = 0;
    const char *name = nullptr;
    CHECK_EQ(rv_net(g.s, 0, &code, &name), RV_OK);
    CHECK_EQ(code, 1);
    CHECK_STREQ(name, "GND");
    CHECK_EQ(rv_net(g.s, 2, &code, &name), RV_OK);
    CHECK_EQ(code, 3);
    CHECK_STREQ(name, "+3V3");

    rv_search_info si;
    CHECK_EQ(rv_search(g.s, 1, &si), RV_OK);
    CHECK_EQ(si.valid, 1u);
    CHECK_EQ(si.repeat_of, 0u);
    CHECK_EQ(si.wavefront_search, 0u);
    CHECK_EQ(si.wavefront_length, 6u);
    CHECK_EQ(si.route_id, 0u);
    CHECK_EQ(si.ctx, 1u);
    CHECK_EQ(si.session, 0u);
    CHECK_EQ(rv_search(g.s, 5, &si), RV_OK);
    CHECK_EQ(si.wavefront_search, RV_NONE);
    CHECK_EQ(si.expanded, 12);
    CHECK_EQ(rv_search(g.s, 6, &si), RV_OK);
    CHECK_EQ(si.session, 1u);

    const uint32_t *cells = nullptr;
    uint32_t count = 0;
    CHECK_EQ(rv_search_cells(g.s, 2, 2, &cells, &count), RV_OK);
    CHECK_EQ(count, s.srchs[2].path.size());
    CHECK(cells != nullptr && std::vector<uint32_t>(cells, cells + count) == s.srchs[2].path);
    CHECK_EQ(rv_search_cells(g.s, 2, 3, &cells, &count), RV_E_INVALID_ARGUMENT);

    const int32_t *costs = nullptr;
    CHECK_EQ(rv_wavefront(g.s, 1, &cells, &costs, &count), RV_OK);
    CHECK_EQ(count, 6u);
    CHECK(cells != nullptr && std::vector<uint32_t>(cells, cells + count) == s.srchs[0].trace_cell);
    CHECK(costs != nullptr && std::vector<int32_t>(costs, costs + count) == s.srchs[0].trace_d);
    CHECK_EQ(rv_wavefront(g.s, 2, &cells, &costs, &count), RV_OK);
    CHECK_EQ(count, 4u);
    CHECK(costs == nullptr); // search 2 recorded no costs
    CHECK_EQ(rv_wavefront(g.s, 5, &cells, &costs, &count), RV_OK);
    CHECK_EQ(count, 0u);
    CHECK(cells == nullptr);
    CHECK_EQ(rv_wavefront(g.s, 99, &cells, &costs, &count), RV_E_INVALID_ARGUMENT);

    uint32_t added = 99;
    CHECK_EQ(rv_refresh(g.s, &added), RV_OK);
    CHECK_EQ(added, 0u);
    CHECK_EQ(rv_close(g.s), RV_OK);
    CHECK_EQ(rv_info(g.s, &info), RV_E_INVALID_ARGUMENT);
}

TEST(api_failed_open_keeps_the_current_trace) {
    rvtest::TempDir dir;
    const synth::Sample s = synth::build_sample();
    const std::string path = dir.path("a.grtrace");
    CHECK(synth::write_file(path, s.bytes));
    SessionGuard g;
    CHECK_EQ(rv_open(g.s, path.c_str()), RV_OK);
    CHECK_EQ(rv_open(g.s, dir.path("missing").c_str()), RV_E_IO);
    rv_trace_info info;
    CHECK_EQ(rv_info(g.s, &info), RV_OK);
    CHECK_EQ(info.chunk_count, s.tags.size());
}

TEST(api_follows_a_growing_file) {
    rvtest::TempDir dir;
    const synth::Sample s = synth::build_sample();
    const std::string path = dir.path("live.grtrace");
    const std::size_t cut = s.chunk_offsets[5] + 7; // inside chunk 5
    CHECK(rvtest::append_file(path, s.bytes.data(), cut));
    SessionGuard g;
    CHECK_EQ(rv_open(g.s, path.c_str()), RV_OK);
    rv_trace_info info;
    CHECK_EQ(rv_info(g.s, &info), RV_OK);
    CHECK_EQ(info.chunk_count, 5u);
    CHECK_EQ(info.complete, 0u);
    CHECK_EQ(info.indexed_end, s.chunk_offsets[5]);
    CHECK_EQ(info.file_size, cut);
    CHECK(rvtest::append_file(path, s.bytes.data() + cut, s.bytes.size() - cut));
    uint32_t added = 0;
    CHECK_EQ(rv_refresh(g.s, &added), RV_OK);
    CHECK_EQ(added, s.tags.size() - 5);
    CHECK_EQ(rv_info(g.s, &info), RV_OK);
    CHECK_EQ(info.complete, 1u);
    CHECK_EQ(info.issue_count, 0u);

    // the writer starts over (file truncated and rewritten): failed state
    CHECK(synth::write_file(path, std::vector<unsigned char>(s.bytes.begin(), s.bytes.begin() + 100)));
    CHECK_EQ(rv_refresh(g.s, &added), RV_E_FORMAT);
    CHECK(std::strlen(rv_last_error(g.s)) > 0);
    CHECK_EQ(rv_info(g.s, &info), RV_OK);
    CHECK_EQ(info.failed, 1u);
    CHECK_EQ(info.chunk_count, s.tags.size()); // what was read stays readable
    CHECK_EQ(rv_refresh(g.s, &added), RV_E_FORMAT);
}

TEST(api_reports_issues) {
    rvtest::TempDir dir;
    synth::Writer w;
    synth::WSrch bad;
    bad.src = {0};
    w.chunk("SRCH", synth::encode(bad)); // before any RUN: corrupt
    const std::string path = dir.path("issues.grtrace");
    CHECK(synth::write_file(path, w.out));
    SessionGuard g;
    CHECK_EQ(rv_open(g.s, path.c_str()), RV_OK);
    rv_trace_info info;
    CHECK_EQ(rv_info(g.s, &info), RV_OK);
    CHECK_EQ(info.error_count, 1u);
    rv_issue_info is;
    CHECK_EQ(rv_issue(g.s, 0, &is), RV_OK);
    CHECK_EQ(is.severity, RV_ISSUE_ERROR);
    CHECK_EQ(is.chunk, 0u);
    CHECK(is.message != nullptr && std::strstr(is.message, "RUN") != nullptr);
    CHECK_EQ(rv_issue(g.s, 1, &is), RV_E_INVALID_ARGUMENT);
    rv_chunk_info c;
    CHECK_EQ(rv_chunk(g.s, 0, &c), RV_OK);
    CHECK_EQ(c.corrupt, 1u);
    rv_search_info si;
    CHECK_EQ(rv_search(g.s, 0, &si), RV_OK);
    CHECK_EQ(si.valid, 0u);
}

// The recorder's real output, written by the Python side from the same
// spec.  The only test allowed to skip: the file is produced separately.
TEST(real_sample_from_the_recorder) {
    std::string here = __FILE__;
    const std::size_t slash = here.find_last_of("/\\");
    here = slash == std::string::npos ? std::string(".") : here.substr(0, slash);
    const std::string path = here + "/data/small.grtrace";
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        std::cout << "    skipped: sample not present (" << path << ")\n";
        CHECK(true);
        return;
    }
    std::fclose(f);
    SessionGuard g;
    const rv_status st = rv_open(g.s, path.c_str());
    if (st != RV_OK) std::cerr << "    open failed: " << rv_last_error(g.s) << "\n";
    CHECK_EQ(st, RV_OK);
    if (st != RV_OK) return;
    rv_trace_info info;
    CHECK_EQ(rv_info(g.s, &info), RV_OK);
    CHECK_EQ(info.header_complete, 1u);
    CHECK_EQ(info.version_major, 1u);
    for (uint32_t k = 0; k < info.issue_count; ++k) {
        rv_issue_info is;
        if (rv_issue(g.s, k, &is) == RV_OK)
            std::cerr << "    sample " << (is.severity == RV_ISSUE_ERROR ? "error" : "warning") << " (chunk "
                      << is.chunk << "): " << is.message << "\n";
    }
    CHECK_EQ(info.error_count, 0u);
    CHECK_EQ(info.warning_count, 0u);
    CHECK_EQ(info.stopped, 0u);
    // every wavefront loads
    int loaded = 0;
    for (uint32_t k = 0; k < info.search_count; ++k) {
        const uint32_t *cells = nullptr;
        const int32_t *costs = nullptr;
        uint32_t count = 0;
        const rv_status ws = rv_wavefront(g.s, k, &cells, &costs, &count);
        if (ws != RV_OK) std::cerr << "    search " << k << ": " << rv_last_error(g.s) << "\n";
        CHECK_EQ(ws, RV_OK);
        if (count > 0) ++loaded;
    }
    std::cout << "    sample: " << info.chunk_count << " chunks, " << info.search_count << " searches ("
              << loaded << " with a wavefront), " << info.copper_count << " copper, "
              << (info.complete ? "complete" : "incomplete") << "\n";
    // and through the C++ reader: invariants, grids and board geometry load
    auto t = rv::Trace::open_file(path);
    CHECK_EQ(rvtest::invariant_failure(*t), std::string());
    CHECK_EQ(rvtest::exercise(*t), 0);
}
