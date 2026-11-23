// rvsynth: write the synthetic test trace (core/tests/synth.cpp) to a file.
// For testing rvdump and clients without the Python recorder.
//
//   rvsynth OUT                 the complete sample (every chunk kind, END)
//   rvsynth --no-end OUT        the sample without its END chunk
//   rvsynth --truncate N OUT    the first N bytes of the complete sample

#include "synth.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char **argv) {
    bool with_end = true;
    long truncate = -1;
    const char *out = nullptr;
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
        std::fprintf(stderr, "usage: rvsynth [--no-end | --truncate N] OUT\n");
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
