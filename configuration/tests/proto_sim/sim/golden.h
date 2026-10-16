// Golden-frame support for the S0 "pin" stage of the MAC separation refactor
// (docs/mac-separation-layering-plan.md).
//
// A golden file holds the byte-exact wire output of one scenario, captured on
// the code as it stood before the refactor. Stages S2-S7 must reproduce it
// byte for byte; only S8 (the explicit wire break) may re-record.
//
// Record (deliberately, and only after reviewing why the bytes changed):
//     PROTO_SIM_GOLDEN_RECORD=1 ctest -R Golden
// Files live in tests/proto_sim/golden/<name>.golden. Line endings are ignored
// on compare, so a CRLF checkout does not break a match.
//
// DETERMINISM: every input to the bytes must be pinned by the scenario: the
// libc rand() seed (esp_random() in the shim is rand()), the esp_timer clock,
// the SimClock, the wall clock and the time zone. A golden scenario is judged
// one process per test (ctest's gtest_discover_tests); the file-static key maps
// in lora_client.cpp are per process.
#pragma once

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef PROTO_SIM_GOLDEN_DIR
#error "PROTO_SIM_GOLDEN_DIR must be defined by the build"
#endif

namespace golden {

inline std::string hex(const uint8_t *p, size_t n) {
    static const char *d = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s.push_back(d[p[i] >> 4]);
        s.push_back(d[p[i] & 0xF]);
    }
    return s;
}
inline std::string hex(const std::vector<uint8_t> &v) { return hex(v.data(), v.size()); }

inline std::string stripCr(const std::string &s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s)
        if (c != '\r') o.push_back(c);
    return o;
}

inline std::vector<std::string> lines(const std::string &s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string l;
    while (std::getline(in, l)) out.push_back(l);
    return out;
}

inline std::string path(const std::string &name) {
    return std::string(PROTO_SIM_GOLDEN_DIR) + "/" + name + ".golden";
}

inline bool recording() {
    const char *r = std::getenv("PROTO_SIM_GOLDEN_RECORD");
    return r != nullptr && r[0] == '1';
}

// Compare `actual` with the stored golden `name`, or (re)write it when
// PROTO_SIM_GOLDEN_RECORD=1. An empty capture is always a failure: a scenario
// that produced no frames pins nothing.
inline ::testing::AssertionResult matches(const std::string &name, const std::string &actual) {
    if (actual.empty())
        return ::testing::AssertionFailure() << "golden '" << name << "': empty capture";
    if (recording()) {
        std::ofstream o(path(name), std::ios::binary);
        o << actual;
        if (!o) return ::testing::AssertionFailure() << "could not write " << path(name);
        return ::testing::AssertionSuccess();
    }
    std::ifstream i(path(name), std::ios::binary);
    if (!i)
        return ::testing::AssertionFailure()
               << "golden file missing: " << path(name)
               << " (record with PROTO_SIM_GOLDEN_RECORD=1)";
    std::stringstream ss;
    ss << i.rdbuf();
    const std::string want = stripCr(ss.str());
    const std::string got  = stripCr(actual);
    if (want == got) return ::testing::AssertionSuccess();
    const auto w = lines(want), g = lines(got);
    size_t k = 0;
    while (k < w.size() && k < g.size() && w[k] == g[k]) ++k;
    return ::testing::AssertionFailure()
           << "golden '" << name << "' differs at line " << (k + 1) << " (golden has "
           << w.size() << " lines, capture " << g.size() << ")\n  golden : "
           << (k < w.size() ? w[k] : "<end>") << "\n  capture: "
           << (k < g.size() ? g[k] : "<end>");
}

}  // namespace golden
