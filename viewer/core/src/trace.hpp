// trace.hpp: the .grtrace reader (docs/TRACE_FORMAT.md, version 1.x).
//
// Opening scans the chunk headers once and builds the chunk index.  Each
// chunk is parsed as it is indexed:
//   - eagerly, into the typed tables below: every chunk kind, including the
//     SRCH fixed header with its source, target and path cells, the GRDS
//     header, and the BRD tables (the whole BRD is validated at that point);
//   - on demand, through small caches: SRCH wavefront arrays (resolving
//     repeat_of), GRDS grid arrays, BRD geometry.
// The reader never loads the whole file.
//
// refresh() indexes chunks appended since the last scan, so a viewer can
// follow a file that is still being written.  A file that shrank or whose
// header changed puts the trace in a failed state.
//
// A corrupt chunk (payload not fitting its layout) becomes an error issue
// and is skipped; it never stops the reader, except a chunk header with a
// non-printable tag, where the spec says reading stops (3.3).
#ifndef RV_TRACE_HPP
#define RV_TRACE_HPP

#include "lru.hpp"
#include "records.hpp"
#include "source.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace rv {

class Trace {
public:
    // Reads what the source holds now.  Throws FormatError (not a trace
    // file), UnsupportedVersion (major version other than 1) or IoError.
    // Fewer than 24 bytes that agree with the start of a header are not an
    // error: the header is "not written yet" (10 of the spec).
    explicit Trace(std::unique_ptr<Source> source);
    static std::unique_ptr<Trace> open_file(const std::string &utf8_path);

    Trace(const Trace &) = delete;
    Trace &operator=(const Trace &) = delete;

    // Index chunks appended since the last scan.  Returns how many were
    // added.  Throws FormatError if the file shrank or was rewritten (and
    // keeps failing afterwards; the tables stay as they were), IoError if it
    // cannot be read.
    std::uint32_t refresh();

    // --- file-level facts -------------------------------------------------
    bool header_complete() const { return header_done_; }
    std::uint16_t major() const { return major_; }
    std::uint16_t minor() const { return minor_; }
    std::uint32_t header_flags() const { return header_flags_; }
    double open_time() const { return open_time_; }
    bool complete() const { return complete_; }  // END indexed
    bool stopped() const { return stopped_; }    // corrupt chunk header; reading stopped
    bool failed() const { return !failure_.empty(); }
    const std::string &failure() const { return failure_; }
    std::uint64_t file_size() const { return size_; }
    std::uint64_t indexed_end() const { return scan_off_; } // first byte not yet indexed

    // --- the index (event sequence) and problems ----------------------------
    const std::vector<Chunk> &chunks() const { return chunks_; }
    const std::vector<Issue> &issues() const { return issues_; }
    std::uint32_t error_count() const { return errors_; }
    std::uint32_t warning_count() const { return warnings_; }
    // Chunks of each kind, corrupt ones included.
    std::uint32_t kind_count(Kind kind) const { return kind_counts_[static_cast<int>(kind)]; }

    // --- typed tables -------------------------------------------------------
    // Chunk::record indexes the table of its kind.  runs(), searches() and
    // routes() hold an entry for every chunk of their kind (valid = false
    // for a corrupt one), so their index is the session index, search_id
    // and route_id.  copper() is indexed by copper_id.  The other tables
    // hold only chunks that parsed; a corrupt chunk has record = kNone32.
    const std::vector<Run> &runs() const { return runs_; }
    const std::vector<NetsChunk> &nets_chunks() const { return nets_; }
    const std::vector<Pad> &pads() const { return pads_; }
    const std::vector<Rect> &rects() const { return rects_; }
    const std::vector<Disc> &discs() const { return discs_; }
    const std::vector<Via> &vias() const { return vias_; }
    const std::vector<Snap> &snaps() const { return snaps_; }
    const std::vector<GridsInfo> &grids_infos() const { return grids_; }
    const std::vector<Route> &routes() const { return routes_; }
    const std::vector<RouteEnd> &route_ends() const { return route_ends_; }
    const std::vector<Search> &searches() const { return searches_; }
    const std::vector<Commit> &commits() const { return commits_; }
    const std::vector<Rebuild> &rebuilds() const { return rebuilds_; }
    const std::vector<Rip> &rips() const { return rips_; }
    const std::vector<Drop> &drops() const { return drops_; }
    const std::vector<Mark> &marks() const { return marks_; }
    const std::vector<Routes> &routes_outputs() const { return routes_out_; }
    const std::vector<BoardInfo> &boards() const { return boards_; }
    const std::vector<End> &ends() const { return ends_; }

    // --- derived tables -----------------------------------------------------
    const std::vector<Copper> &copper() const { return copper_; }
    // Router net number to name, the latest NETS entry for each code.
    const std::map<std::int32_t, std::string> &net_names() const { return net_names_; }
    // Router net number to the chunks about that net, in file order: PAD,
    // RECT (op 0), DISC, VIA, RNB, RNE, SRCH, CMIT, RIP, DROP, and RBLD under
    // the net of its route_net call.  BLOCK is not a net.
    const std::map<std::int32_t, std::vector<std::uint32_t>> &net_chunks() const { return net_chunks_; }

    // Totals over the indexed chunks.
    std::int64_t expanded_total() const { return expanded_total_; } // SRCH expanded >= 0
    std::int64_t trace_entries() const { return trace_entries_; }   // SRCH ntrace
    std::uint32_t failed_pads() const { return failed_pads_; }      // RNE nfailed
    std::uint32_t drops_failed() const { return drops_failed_; }    // DROP ok = 0
    std::uint32_t drops_raised() const { return drops_raised_; }    // DROP ok = 2
    std::uint32_t copper_removed() const { return copper_removed_; }

    // --- on demand ----------------------------------------------------------
    // The wavefront of a search (repeat_of resolved), or nullptr when it has
    // none (tracing off, unknown enc, invalid repeat).  Throws
    // std::invalid_argument for a bad id, FormatError if the arrays hold a
    // cell outside the grid or cannot be read.
    std::shared_ptr<const Wavefront> wavefront(std::uint32_t search_id);
    std::shared_ptr<const Grids> grids(std::uint32_t grds_index);
    std::shared_ptr<const BoardGeometry> board_geometry(std::uint32_t brd_index);

    // Wavefront cache: at most `entries` wavefronts and `bytes` bytes (the
    // most recent one is always kept).  Defaults: 16 and 64 MiB.
    void set_wavefront_cache(std::size_t entries, std::size_t bytes) { wave_cache_.set_limits(entries, bytes); }
    const CacheStats &wavefront_cache_stats() const { return wave_cache_.stats(); }
    template <class F>
    void wavefront_cache_keys(F f) const { wave_cache_.for_each_key(f); }

    // One line describing a chunk's own content (not what later chunks do
    // to it).  Throws std::invalid_argument for a bad index.
    std::string describe(std::uint32_t chunk) const;

private:
    // Reading
    void scan();
    void read_header(std::uint64_t size);
    void check_unchanged();
    void process(Chunk c);
    void read_exact(std::uint64_t offset, void *buf, std::size_t n);
    const unsigned char *window(std::uint64_t offset, std::size_t n);
    const std::vector<unsigned char> &payload(const Chunk &c, std::uint64_t from, std::uint64_t n);
    void issue(Issue::Severity sev, std::uint32_t chunk, std::uint64_t offset, const std::string &msg);
    void fail_state(const std::string &message);

    // Per kind: parse the chunk at index idx and update the derived tables.
    // Throw CorruptChunk for a payload that does not fit its layout.
    void on_run(Chunk &c, std::uint32_t idx);
    void on_nets(Chunk &c, std::uint32_t idx);
    void on_pad(Chunk &c, std::uint32_t idx);
    void on_rect(Chunk &c, std::uint32_t idx);
    void on_disc(Chunk &c, std::uint32_t idx);
    void on_via(Chunk &c, std::uint32_t idx);
    void on_snap(Chunk &c, std::uint32_t idx);
    void on_grds(Chunk &c, std::uint32_t idx);
    void on_rnb(Chunk &c, std::uint32_t idx);
    void on_rne(Chunk &c, std::uint32_t idx);
    void on_srch(Chunk &c, std::uint32_t idx);
    void on_cmit(Chunk &c, std::uint32_t idx);
    void on_rbld(Chunk &c, std::uint32_t idx);
    void on_rip(Chunk &c, std::uint32_t idx);
    void on_drop(Chunk &c, std::uint32_t idx);
    void on_mark(Chunk &c, std::uint32_t idx);
    void on_rout(Chunk &c, std::uint32_t idx);
    void on_brd(Chunk &c, std::uint32_t idx);
    void on_end(Chunk &c, std::uint32_t idx);

    const Run &current_run(const char *what) const; // throws CorruptChunk without a valid RUN
    void check_route_ref(std::uint32_t route_id, std::uint32_t idx, const Chunk &c, const char *what);
    void add_net_chunk(std::int32_t net, std::uint32_t idx);

    std::unique_ptr<Source> src_;

    // header and scan state
    bool header_done_ = false;
    unsigned char header_[24] = {};
    std::uint16_t major_ = 0, minor_ = 0;
    std::uint32_t header_flags_ = 0;
    double open_time_ = 0;
    std::uint64_t size_ = 0;
    std::uint64_t scan_off_ = 0;
    bool complete_ = false;
    bool stopped_ = false;
    bool after_end_warned_ = false;
    std::string failure_;

    // read-ahead window, valid only within one scan
    std::vector<unsigned char> win_;
    std::uint64_t win_base_ = 0;
    std::size_t win_have_ = 0;
    std::vector<unsigned char> buf_;

    // index, problems, counts
    std::vector<Chunk> chunks_;
    std::vector<Issue> issues_;
    std::uint32_t errors_ = 0, warnings_ = 0;
    std::array<std::uint32_t, kKindCount> kind_counts_{};

    // tables
    std::vector<Run> runs_;
    std::vector<NetsChunk> nets_;
    std::vector<Pad> pads_;
    std::vector<Rect> rects_;
    std::vector<Disc> discs_;
    std::vector<Via> vias_;
    std::vector<Snap> snaps_;
    std::vector<GridsInfo> grids_;
    std::vector<Route> routes_;
    std::vector<RouteEnd> route_ends_;
    std::vector<Search> searches_;
    std::vector<Commit> commits_;
    std::vector<Rebuild> rebuilds_;
    std::vector<Rip> rips_;
    std::vector<Drop> drops_;
    std::vector<Mark> marks_;
    std::vector<Routes> routes_out_;
    std::vector<BoardInfo> boards_;
    std::vector<End> ends_;

    // derived
    std::vector<Copper> copper_;
    std::map<std::int32_t, std::string> net_names_;
    std::map<std::int32_t, std::vector<std::uint32_t>> net_chunks_;
    std::int64_t expanded_total_ = 0, trace_entries_ = 0;
    std::uint32_t failed_pads_ = 0, drops_failed_ = 0, drops_raised_ = 0, copper_removed_ = 0;

    // state of the latest session while scanning
    std::uint32_t session_ = kNone32;
    bool snapped_ = false;
    std::map<std::int32_t, std::vector<std::uint32_t>> since_snap_; // net -> copper ids
    std::uint32_t open_route_ = kNone32;
    bool expect_snapshot_grids_ = false;

    // on-demand caches
    Lru<Wavefront> wave_cache_{16, 64u << 20};
    Lru<Grids> grids_cache_{2, 256u << 20};
    Lru<BoardGeometry> board_cache_{2, 512u << 20};
};

} // namespace rv

#endif // RV_TRACE_HPP
