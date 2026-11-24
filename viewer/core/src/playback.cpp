// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Trevor Attema, Teklabs Digital
//
// playback.cpp: stepping and the position strings (playback.hpp).

#include "playback.hpp"

#include "trace.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace rv {

const char *step_kind_name(std::uint32_t kind) noexcept {
    static const char *const names[kStepKindCount] = {"event", "search", "commit", "net", "rip-up", "failure", "phase"};
    return kind < kStepKindCount ? names[kind] : "";
}

bool step_matches(const Event &e, StepKind kind) {
    switch (kind) {
    case StepKind::Event: return true;
    case StepKind::Search: return e.kind == EvKind::Search;
    case StepKind::Commit: return e.kind == EvKind::Commit;
    case StepKind::Net: return e.kind == EvKind::RouteBegin;
    case StepKind::RipUp: return e.kind == EvKind::RipUp;
    case StepKind::Failure:
        return (e.kind == EvKind::RouteEnd || e.kind == EvKind::Drop) && (e.flags & evf::kFailed) != 0;
    case StepKind::Phase: return e.kind == EvKind::Phase;
    }
    return false;
}

std::uint32_t step_target(const Timeline &tl, double t, StepKind kind, int direction) {
    const std::uint32_t n = tl.size();
    if (n == 0) return kNone32;
    const std::uint32_t cur = tl.event_at(t);
    const std::vector<Event> &ev = tl.events();
    // Events without time are never shown as the current event: not targets.
    if (direction > 0) {
        for (std::uint32_t e = cur; e < n; ++e)
            if (ev[e].start > t && ev[e].duration > 0 && step_matches(ev[e], kind)) return e;
        return kNone32;
    }
    for (std::uint32_t e = cur + 1; e-- > 0;)
        if (ev[e].start < t && ev[e].duration > 0 && step_matches(ev[e], kind)) return e;
    return kNone32;
}

double event_fraction(const Timeline &tl, std::uint32_t e, double t) {
    if (e >= tl.size()) return 0;
    const Event &ev = tl.events()[e];
    if (!(ev.duration > 0)) return 1;
    return std::max(0.0, std::min(1.0, (t - ev.start) / ev.duration));
}

std::uint32_t revealed_count(std::uint32_t n, double fraction) {
    if (!(fraction > 0)) return 0; // also NaN
    if (!(fraction < kRevealEnd)) return n;
    const double v = std::floor(static_cast<double>(n) * fraction / kRevealEnd);
    return v >= n ? n : static_cast<std::uint32_t>(v);
}

std::uint32_t wavefront_source(const Trace &tr, const Timeline &tl, std::uint32_t e) {
    if (e >= tl.size()) return kNone32;
    const Event &ev = tl.events()[e];
    if (ev.kind != EvKind::Search || ev.record >= tr.searches().size()) return kNone32;
    const Search &s = tr.searches()[ev.record];
    if (s.trace_source == kNone32 || s.trace_source >= tr.searches().size()) return kNone32;
    return tr.searches()[s.trace_source].ntrace > 0 ? s.trace_source : kNone32;
}

std::uint32_t cells_revealed(const Trace &tr, const Timeline &tl, std::uint32_t e, double t) {
    const std::uint32_t src = wavefront_source(tr, tl, e);
    if (src == kNone32) return 0;
    return revealed_count(tr.searches()[src].ntrace, event_fraction(tl, e, t));
}

bool search_settled(const Timeline &tl, std::uint32_t e, double t) {
    if (e >= tl.size() || tl.events()[e].kind != EvKind::Search) return false;
    return !(event_fraction(tl, e, t) < kRevealEnd);
}

namespace {

std::string fmt(const char *f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

std::string net_name(const Trace &tr, std::int32_t net) {
    auto it = tr.net_names().find(net);
    if (it != tr.net_names().end() && !it->second.empty()) return it->second;
    return fmt("net %d", net);
}

std::string join(const std::vector<std::string> &v, std::size_t max_items) {
    std::string s;
    for (std::size_t k = 0; k < v.size() && k < max_items; ++k) {
        if (k > 0) s += ", ";
        s += v[k];
    }
    if (v.size() > max_items) s += fmt(" and %zu more", v.size() - max_items);
    return s;
}

const char *ctx_name(std::uint32_t ctx) {
    return ctx == 1 ? "route_net" : ctx == 2 ? "drop_to_plane" : "direct";
}

std::string what(const Trace &tr, const Timeline &tl, std::uint32_t e) {
    const Event &ev = tl.events()[e];
    switch (ev.kind) {
    case EvKind::Session: {
        const Run &r = tr.runs()[ev.record];
        if (!r.valid) return fmt("router session %u (corrupt RUN: no grid, no copper)", ev.record + 1);
        return fmt("router session %u: %d × %d cells of %g mm, %s / %s", ev.record + 1, r.nx, r.ny, r.cell,
                   r.layer_name[0].c_str(), r.layer_name[1].c_str());
    }
    case EvKind::Snapshot: return "snapshot: the copper so far becomes permanent";
    case EvKind::Search: {
        const Search &s = tr.searches()[ev.record];
        std::string x = fmt("search %u (%s) for %s: ", s.id_field, ctx_name(s.ctx), net_name(tr, s.net).c_str());
        if (s.has(srch_flags::kError)) x += "error";
        else if (s.has(srch_flags::kFound)) x += fmt("found, cost %lld, %u path cells", static_cast<long long>(s.cost), s.npath);
        else x += "not found";
        if (s.expanded >= 0) x += fmt(", %lld states expanded", static_cast<long long>(s.expanded));
        return x;
    }
    case EvKind::Commit: {
        const Commit &c = tr.commits()[ev.record];
        const std::uint32_t id = c.copper < tr.copper().size() ? tr.copper()[c.copper].id : kNone32;
        std::string x = fmt("commit of %s: copper %u, %zu path cells", net_name(tr, c.net).c_str(), id, c.path.size());
        if (!c.vias.empty()) x += fmt(", %zu new vias", c.vias.size());
        if (!c.stubs.empty()) x += fmt(", %zu stubs", c.stubs.size());
        return x;
    }
    case EvKind::Via: {
        const Via &v = tr.vias()[ev.record];
        return fmt("via of %s at cell (%d, %d)%s", net_name(tr, v.net).c_str(), v.i, v.j,
                   v.off_grid ? ", outside the grid" : "");
    }
    case EvKind::RipUp: {
        const std::uint32_t removed = tl.counts(e).removed - (e > 0 ? tl.counts(e - 1).removed : 0);
        const std::uint32_t rips = tl.counts(e).rips - (e > 0 ? tl.counts(e - 1).rips : 0);
        if (ev.record != kNone32) {
            const Rebuild &rb = tr.rebuilds()[ev.record];
            return fmt("rebuild: %u rip-ups (%s), %u copper items removed", rips, join(rb.names, 4).c_str(), removed);
        }
        return fmt("rip-up of %s: %u copper items removed", net_name(tr, ev.net).c_str(), removed);
    }
    case EvKind::RouteBegin: {
        const Route &r = tr.routes()[ev.record];
        return fmt("route_net begins: %s, %zu pads%s", r.name.c_str(), r.pads.size(), r.wide ? ", wide" : "");
    }
    case EvKind::RouteEnd: {
        const RouteEnd &re = tr.route_ends()[ev.record];
        const std::string name = ev.route != kNone32 ? tr.routes()[ev.route].name : net_name(tr, re.net);
        std::string x = "route_net ends: " + name + ", ";
        if (re.status == 1) x += "raised";
        else if (re.failed.empty()) x += "ok";
        if (!re.failed.empty())
            x += fmt("%s%zu pads failed (%s)", re.status == 1 ? ", " : "", re.failed.size(), join(re.failed, 4).c_str());
        if (!re.ripped.empty()) x += fmt(", ripped %s", join(re.ripped, 4).c_str());
        return x;
    }
    case EvKind::Drop: {
        const Drop &d = tr.drops()[ev.record];
        return fmt("drop_to_plane %s pad %s: %s", d.name.c_str(), d.pad_label.c_str(),
                   d.ok == 1 ? "ok" : d.ok == 0 ? "failed" : "raised");
    }
    case EvKind::Phase: return "phase: " + tr.marks()[ev.record].text;
    case EvKind::Board: return "board \"" + tr.boards()[ev.record].label + "\"";
    case EvKind::End: {
        const End &x = tr.ends()[ev.record];
        return fmt("end of trace: %u routes, %u failed pads, router wall time %.2f s", x.routes, x.nfailed, x.wall_s);
    }
    }
    return "";
}

} // namespace

std::string caption(const Trace &tr, const Timeline &tl, std::uint32_t e, std::uint32_t failures_shown) {
    if (e >= tl.size()) return tr.chunks().empty() ? "no events" : "no events (nothing to play)";
    const Event &ev = tl.events()[e];
    const char *sep = " · ";
    std::string s;
    if (ev.phase != kNone32) s += tl.phases()[ev.phase].label + sep;
    if (ev.route != kNone32 && ev.kind != EvKind::RouteBegin && ev.kind != EvKind::RouteEnd)
        s += fmt("route_net %u/%zu %s", ev.route + 1, tr.routes().size(), tr.routes()[ev.route].name.c_str()) + sep;
    s += what(tr, tl, e);
    const Counts &c = tl.counts(e);
    s += sep;
    s += fmt("%u commits, %u vias, %u rip-ups, %u failed pads", c.commits, c.vias, c.rips, failures_shown);
    const double rt = tr.chunks()[ev.last].t;
    s += sep;
    s += fmt("router %.2f s", rt);
    return s;
}

namespace {

// Seconds for a clock: negative and NaN as 0, at most kClockMax, so the
// minutes always fit a long long.  The timeline keeps its times far below
// (kMaxEventSeconds); this guards the conversion whatever it is given.
double clock_seconds(double s) {
    constexpr double kClockMax = 1e15;
    if (!(s > 0)) return 0.0;
    return s < kClockMax ? s : kClockMax;
}

} // namespace

std::string position_text(const Timeline &tl, std::uint32_t e, double t) {
    auto clock = [](double s) {
        const double v = clock_seconds(s);
        const long long m = static_cast<long long>(v / 60);
        return fmt("%lld:%04.1f", m, v - 60.0 * static_cast<double>(m));
    };
    if (e >= tl.size()) return "no events";
    return fmt("event %u of %u · %s / %s", e + 1, tl.size(), clock(t).c_str(), clock(tl.total()).c_str());
}

std::string speed_text(double speed) {
    return fmt("%g×", speed);
}

std::string pacing_text(const PacingSettings &p, const Trace *tr, double total) {
    auto clock = [](double s) {
        const double v = clock_seconds(s);
        const long long m = static_cast<long long>(v / 60);
        return fmt("%lld:%02.0f", m, std::floor(v - 60.0 * static_cast<double>(m)));
    };
    switch (p.mode) {
    case PacingMode::Fit: return "fit length " + clock(total);
    case PacingMode::NetsOnly: return "nets only, " + clock(total);
    case PacingMode::OneNet: {
        std::string name = tr != nullptr ? net_name(*tr, p.net) : fmt("net %d", p.net);
        return "one net: " + name + ", " + clock(total);
    }
    case PacingMode::SkipFailed: return "skip failed searches, " + clock(total);
    case PacingMode::RealTime: return fmt("real time ×%g, ", p.slowdown) + clock(total);
    }
    return "";
}

} // namespace rv
