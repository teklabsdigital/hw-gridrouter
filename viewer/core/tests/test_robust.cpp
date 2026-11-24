// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Trevor Attema, Teklabs Digital
//
// test_robust.cpp: truncation at every byte and corruption fuzzing.
//
// The reader must never crash, read out of bounds or trip ASan/UBSan on any
// bytes.  Every outcome must be either a clean error (FormatError or
// UnsupportedVersion at open, FormatError on an on-demand load) or a parse
// whose structural invariants hold.

#include "check.hpp"
#include "synth.hpp"
#include "trace_util.hpp"

#include "errors.hpp"
#include "trace.hpp"

#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

// splitmix64: a fixed, portable PRNG (no <random> distributions, whose
// output differs between standard libraries).
struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed) {}
    std::uint64_t next() {
        std::uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    std::size_t below(std::size_t n) { return n == 0 ? 0 : static_cast<std::size_t>(next() % n); }
};

struct Outcome {
    int parsed = 0;       // opened; invariants hold
    int rejected = 0;     // clean error at open
    int load_errors = 0;  // clean errors from on-demand loads
    int with_issues = 0;  // parsed, with at least one issue
    int bad = 0;          // anything else: a failure
};

// Open bytes, check invariants, load everything.  Never lets an exception
// out; anything unexpected counts as bad and is printed once.
void run_one(const std::vector<unsigned char> &bytes, Outcome &out, const std::string &what) {
    try {
        auto t = rvtest::open_bytes(bytes);
        const std::string why = rvtest::invariant_failure(*t);
        if (!why.empty()) {
            if (out.bad++ < 5) std::cerr << "    invariant failed (" << what << "): " << why << "\n";
            return;
        }
        out.load_errors += rvtest::exercise(*t);
        // refresh on unchanged bytes is a no-op
        if (t->refresh() != 0) {
            if (out.bad++ < 5) std::cerr << "    refresh added chunks (" << what << ")\n";
            return;
        }
        ++out.parsed;
        if (!t->issues().empty()) ++out.with_issues;
    } catch (const rv::FormatError &) {
        ++out.rejected;
    } catch (const rv::UnsupportedVersion &) {
        ++out.rejected;
    } catch (const std::exception &e) {
        if (out.bad++ < 5) std::cerr << "    unexpected exception (" << what << "): " << e.what() << "\n";
    }
}

void report(const char *name, const Outcome &o, int total) {
    std::cout << "    " << name << ": " << total << " inputs, " << o.parsed << " parsed (" << o.with_issues
              << " with issues), " << o.rejected << " rejected at open, " << o.load_errors
              << " on-demand load errors, " << o.bad << " bad\n";
}

void put_u32(std::vector<unsigned char> &b, std::size_t at, std::uint32_t v) {
    for (int k = 0; k < 4; ++k) b[at + static_cast<std::size_t>(k)] = static_cast<unsigned char>(v >> (8 * k));
}

} // namespace

TEST(truncation_at_every_byte_gives_a_prefix) {
    const synth::Sample s = synth::build_sample();
    auto full = rvtest::open_bytes(s.bytes);
    CHECK_EQ(full->issues().size(), 0u);
    std::vector<std::string> full_desc;
    for (std::size_t k = 0; k < full->chunks().size(); ++k)
        full_desc.push_back(full->describe(static_cast<std::uint32_t>(k)));

    int checked = 0;
    for (std::size_t n = 0; n <= s.bytes.size(); ++n) {
        std::vector<unsigned char> prefix(s.bytes.begin(), s.bytes.begin() + static_cast<std::ptrdiff_t>(n));
        std::unique_ptr<rv::Trace> t;
        try {
            t = rvtest::open_bytes(prefix);
        } catch (const std::exception &e) {
            std::cerr << "    prefix " << n << " failed to open: " << e.what() << "\n";
            CHECK(false);
            continue;
        }
        // the chunks that fit entirely
        std::size_t expect = 0;
        for (std::size_t k = 0; k < s.chunk_offsets.size(); ++k) {
            const std::size_t end = k + 1 < s.chunk_offsets.size() ? s.chunk_offsets[k + 1] : s.bytes.size();
            if (end <= n) expect = k + 1;
        }
        bool ok = t->chunks().size() == expect && t->issues().empty() &&
                  t->header_complete() == (n >= 24) && t->complete() == (n == s.bytes.size()) &&
                  rvtest::invariant_failure(*t).empty();
        for (std::size_t k = 0; ok && k < t->chunks().size(); ++k) {
            ok = t->chunks()[k].offset == full->chunks()[k].offset &&
                 t->describe(static_cast<std::uint32_t>(k)) == full_desc[k];
        }
        if (ok) ok = rvtest::exercise(*t) == 0;
        if (!ok) {
            std::cerr << "    prefix of " << n << " bytes: " << t->chunks().size() << " chunks (want " << expect
                      << "), " << t->issues().size() << " issues\n";
            for (const rv::Issue &is : t->issues()) std::cerr << "      " << is.message << "\n";
        }
        CHECK(ok);
        ++checked;
    }
    CHECK_EQ(static_cast<std::size_t>(checked), s.bytes.size() + 1);
    std::cout << "    truncation: " << checked << " prefixes of a " << s.bytes.size() << "-byte file\n";
}

TEST(fuzz_every_u32_slot_with_extreme_values) {
    // Every 4-byte aligned slot (tags, lengths, counts, ids, offsets alike)
    // overwritten with values that break size arithmetic.
    const synth::Sample s = synth::build_sample();
    const std::uint32_t values[] = {0u, 1u, 3u, 7u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu,
                                    0xFFFFFFF8u, 0x40000000u, 0x10000u};
    Outcome o;
    int total = 0;
    for (std::size_t at = 0; at + 4 <= s.bytes.size(); at += 4) {
        for (std::uint32_t v : values) {
            std::vector<unsigned char> b = s.bytes;
            put_u32(b, at, v);
            run_one(b, o, "slot " + std::to_string(at) + " = " + std::to_string(v));
            ++total;
        }
    }
    report("u32 slots", o, total);
    CHECK_EQ(o.bad, 0);
    CHECK(o.parsed > 0);
}

TEST(fuzz_chunk_lengths) {
    // Every chunk's len field set to values around and far from the truth.
    const synth::Sample s = synth::build_sample();
    Outcome o;
    int total = 0;
    for (std::size_t k = 0; k < s.chunk_offsets.size(); ++k) {
        const std::size_t at = s.chunk_offsets[k] + 4;
        const std::uint32_t len = static_cast<std::uint32_t>(s.bytes[at]) | (static_cast<std::uint32_t>(s.bytes[at + 1]) << 8) |
                                  (static_cast<std::uint32_t>(s.bytes[at + 2]) << 16) |
                                  (static_cast<std::uint32_t>(s.bytes[at + 3]) << 24);
        for (std::int64_t d = -12; d <= 12; ++d) {
            const std::int64_t v = static_cast<std::int64_t>(len) + d;
            if (v < 0) continue;
            std::vector<unsigned char> b = s.bytes;
            put_u32(b, at, static_cast<std::uint32_t>(v));
            run_one(b, o, "chunk " + std::to_string(k) + " len " + std::to_string(v));
            ++total;
        }
    }
    report("chunk lengths", o, total);
    CHECK_EQ(o.bad, 0);
}

TEST(fuzz_random_mutations) {
    const synth::Sample s = synth::build_sample();
    const std::uint64_t seeds[] = {1, 0x5EED, 20261123};
    const int per_seed = 4000;
    Outcome o;
    int total = 0;
    for (std::uint64_t seed : seeds) {
        Rng rng(seed);
        for (int i = 0; i < per_seed; ++i) {
            std::vector<unsigned char> b = s.bytes;
            const int edits = 1 + static_cast<int>(rng.below(4));
            for (int e = 0; e < edits; ++e) {
                const std::size_t at = rng.below(b.size());
                switch (rng.below(5)) {
                case 0: // flip one bit
                    b[at] ^= static_cast<unsigned char>(1u << rng.below(8));
                    break;
                case 1: // random byte
                    b[at] = static_cast<unsigned char>(rng.next());
                    break;
                case 2: // random aligned u32
                    if (at / 4 * 4 + 4 <= b.size()) put_u32(b, at / 4 * 4, static_cast<std::uint32_t>(rng.next()));
                    break;
                case 3: // small count bump at an aligned slot
                    if (at / 4 * 4 + 4 <= b.size()) {
                        const std::size_t a = at / 4 * 4;
                        const std::uint32_t old = static_cast<std::uint32_t>(b[a]) | (static_cast<std::uint32_t>(b[a + 1]) << 8) |
                                                  (static_cast<std::uint32_t>(b[a + 2]) << 16) |
                                                  (static_cast<std::uint32_t>(b[a + 3]) << 24);
                        put_u32(b, a, old + static_cast<std::uint32_t>(rng.below(5)) - 2u);
                    }
                    break;
                default: // truncate
                    b.resize(at);
                    break;
                }
                if (b.empty()) break;
            }
            run_one(b, o, "seed " + std::to_string(seed) + " #" + std::to_string(i));
            ++total;
        }
    }
    report("random mutations", o, total);
    CHECK_EQ(o.bad, 0);
    CHECK(o.parsed > 0);
}

TEST(fuzz_tailing_corrupt_files) {
    // A corrupt file read piece by piece must end in the same state as read
    // in one go (or fail cleanly in both).
    const synth::Sample s = synth::build_sample();
    Rng rng(77);
    int compared = 0;
    for (int i = 0; i < 300; ++i) {
        std::vector<unsigned char> b = s.bytes;
        const std::size_t at = rng.below(b.size());
        b[at] = static_cast<unsigned char>(rng.next());
        std::string once;
        try {
            once = rvtest::fingerprint(*rvtest::open_bytes(b));
        } catch (const rv::FormatError &) {
            continue;
        } catch (const rv::UnsupportedVersion &) {
            continue;
        }
        auto data = std::make_shared<std::vector<unsigned char>>();
        std::unique_ptr<rv::Trace> t;
        bool failed = false;
        try {
            t = rvtest::open_shared(data);
            const std::size_t step = 1 + rng.below(97);
            for (std::size_t p = 0; p < b.size(); p += step) {
                const std::size_t n = std::min(step, b.size() - p);
                data->insert(data->end(), b.begin() + static_cast<std::ptrdiff_t>(p),
                             b.begin() + static_cast<std::ptrdiff_t>(p + n));
                t->refresh();
            }
        } catch (const rv::FormatError &) {
            failed = true;
        } catch (const rv::UnsupportedVersion &) {
            failed = true;
        }
        CHECK(!failed);
        if (!failed) {
            const bool same = rvtest::fingerprint(*t) == once;
            if (!same) std::cerr << "    tailing differs for mutation at " << at << "\n";
            CHECK(same);
            ++compared;
        }
    }
    std::cout << "    tailing corrupt files: " << compared << " compared\n";
    CHECK(compared > 100);
}
