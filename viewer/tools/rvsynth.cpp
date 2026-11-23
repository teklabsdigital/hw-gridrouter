// rvsynth: write the synthetic test trace (core/tests/synth.cpp) to a file.
// For testing rvdump and clients without the Python recorder.
//
//   rvsynth OUT                 the complete sample (every chunk kind, END)
//   rvsynth --no-end OUT        the sample without its END chunk
//   rvsynth --truncate N OUT    the first N bytes of the complete sample
//   rvsynth --stress OUT        the M2 stress board ("placed" and "final" BRD)
//   rvsynth --timeline OUT      the M3 routing run (synth_timeline.cpp)
//   rvsynth --drip SRC DST [--rate BYTES_PER_S] [--piece N] [--seed S]
//                               M6: copy the trace SRC to a new file DST
//                               while a viewer follows it: pieces of 1 to N
//                               bytes (pseudo-random, so they split chunk
//                               headers and payloads anywhere; default
//                               65536), each written and flushed, at RATE
//                               bytes per second (default 5000000)

#include "synth.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>

namespace {

int drip(int argc, char **argv) {
    if (argc < 4) return -1;
    const char *src = argv[2], *dst = argv[3];
    double rate = 5e6;
    unsigned long piece = 65536, seed = 1;
    for (int a = 4; a + 1 < argc; a += 2) {
        char *end = nullptr;
        if (std::strcmp(argv[a], "--rate") == 0) {
            rate = std::strtod(argv[a + 1], &end);
            if (end == argv[a + 1] || *end != '\0' || !(rate > 0)) return -1;
        } else if (std::strcmp(argv[a], "--piece") == 0) {
            piece = std::strtoul(argv[a + 1], &end, 10);
            if (end == argv[a + 1] || *end != '\0' || piece == 0) return -1;
        } else if (std::strcmp(argv[a], "--seed") == 0) {
            seed = std::strtoul(argv[a + 1], &end, 10);
            if (end == argv[a + 1] || *end != '\0') return -1;
        } else {
            return -1;
        }
    }
    if (argc % 2 != 0) return -1;
    std::FILE *in = std::fopen(src, "rb");
    if (in == nullptr) {
        std::fprintf(stderr, "rvsynth: cannot open %s\n", src);
        return 1;
    }
    std::FILE *out = std::fopen(dst, "wb");
    if (out == nullptr) {
        std::fclose(in);
        std::fprintf(stderr, "rvsynth: cannot create %s\n", dst);
        return 1;
    }
    std::mt19937 rng(static_cast<std::uint32_t>(seed));
    std::vector<unsigned char> buf(piece);
    const auto start = std::chrono::steady_clock::now();
    unsigned long long total = 0, pieces = 0;
    for (;;) {
        const std::size_t want = 1 + rng() % piece;
        const std::size_t got = std::fread(buf.data(), 1, want, in);
        if (got == 0) break;
        if (std::fwrite(buf.data(), 1, got, out) != got || std::fflush(out) != 0) {
            std::fprintf(stderr, "rvsynth: cannot write %s\n", dst);
            std::fclose(in);
            std::fclose(out);
            return 1;
        }
        total += got;
        ++pieces;
        // Hold the rate: the next piece is due when total / rate seconds passed.
        const auto due = start + std::chrono::duration<double>(static_cast<double>(total) / rate);
        std::this_thread::sleep_until(std::chrono::time_point_cast<std::chrono::steady_clock::duration>(due));
    }
    std::fclose(in);
    std::fclose(out);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("rvsynth: dripped %llu bytes in %llu pieces to %s in %.2f s (%.2f MB/s)\n", total, pieces, dst, secs,
                total / 1e6 / (secs > 0 ? secs : 1));
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--drip") == 0) {
        const int rc = drip(argc, argv);
        if (rc >= 0) return rc;
        std::fprintf(stderr, "usage: rvsynth --drip SRC DST [--rate BYTES_PER_S] [--piece N] [--seed S]\n");
        return 2;
    }
    bool with_end = true;
    long truncate = -1;
    const char *out = nullptr;
    if (argc == 3 && std::strcmp(argv[1], "--stress") == 0) {
        const std::vector<unsigned char> bytes = synth::build_stress_trace();
        if (!synth::write_file(argv[2], bytes)) {
            std::fprintf(stderr, "rvsynth: cannot write %s\n", argv[2]);
            return 1;
        }
        std::printf("rvsynth: wrote the stress board, %zu bytes, to %s\n", bytes.size(), argv[2]);
        return 0;
    }
    if (argc == 3 && std::strcmp(argv[1], "--timeline") == 0) {
        const std::vector<unsigned char> bytes = synth::build_timeline_sample();
        if (!synth::write_file(argv[2], bytes)) {
            std::fprintf(stderr, "rvsynth: cannot write %s\n", argv[2]);
            return 1;
        }
        std::printf("rvsynth: wrote the routing run, %zu bytes, to %s\n", bytes.size(), argv[2]);
        return 0;
    }
    if (argc == 2) {
        out = argv[1];
    } else if (argc == 3 && std::strcmp(argv[1], "--no-end") == 0) {
        with_end = false;
        out = argv[2];
    } else if (argc == 4 && std::strcmp(argv[1], "--truncate") == 0) {
        char *end = nullptr;
        truncate = std::strtol(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || truncate < 0) truncate = -2;
        out = argv[3];
    }
    if (out == nullptr || truncate == -2) {
        std::fprintf(stderr, "usage: rvsynth [--no-end | --truncate N | --stress | --timeline] OUT\n"
                             "       rvsynth --drip SRC DST [--rate BYTES_PER_S] [--piece N] [--seed S]\n");
        return 2;
    }
    synth::Sample s = synth::build_sample(with_end);
    if (truncate >= 0 && static_cast<unsigned long>(truncate) < s.bytes.size())
        s.bytes.resize(static_cast<std::size_t>(truncate));
    if (!synth::write_file(out, s.bytes)) {
        std::fprintf(stderr, "rvsynth: cannot write %s\n", out);
        return 1;
    }
    std::printf("rvsynth: wrote %zu bytes, %zu chunks in the full sample, to %s\n", s.bytes.size(),
                s.tags.size(), out);
    return 0;
}
