#pragma once
// Golden files (QA D4): a test renders fixed cases and compares them with committed reference
// renders. Files are small (float32, a few thousand samples per case), committed under tests/golden/.
//
//   lkit::golden::Store g("tests/golden");          // directory relative to the source tree
//   g.check("bypass_48k", samples, nch, n, 1e-6);    // max |diff| tolerance (absolute)
//
// With LKIT_UPDATE_GOLDEN=1 in the environment, check() rewrites the file instead of comparing
// (update only on purpose, and say why in the milestone report). A missing file is a failure.
// Format: "LKG1", int32 channels, int32 frames, float32 interleaved samples.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace lkit::golden {

inline bool updating()
{
    const char* e = std::getenv("LKIT_UPDATE_GOLDEN");
    return e != nullptr && std::strcmp(e, "1") == 0;
}

struct Result {
    bool ok = false;
    double maxDiff = 0.0;
    std::string message;
};

class Store {
public:
    explicit Store(std::string dir) : dir_(std::move(dir)) {}

    // interleaved: frames x channels
    Result check(const std::string& name, const float* interleaved, int channels, int frames, double tol) const
    {
        const std::string path = dir_ + "/" + name + ".lkg";
        Result r;
        if (updating()) {
            FILE* f = std::fopen(path.c_str(), "wb");
            if (!f) return {false, 0.0, "cannot write " + path};
            const std::int32_t hdr[2] = {channels, frames};
            std::fwrite("LKG1", 1, 4, f);
            std::fwrite(hdr, sizeof(hdr), 1, f);
            std::fwrite(interleaved, sizeof(float), static_cast<size_t>(channels) * static_cast<size_t>(frames), f);
            std::fclose(f);
            return {true, 0.0, "updated " + path};
        }
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return {false, 0.0, "missing golden file " + path + " (run with LKIT_UPDATE_GOLDEN=1 on purpose)"};
        char magic[4] = {};
        std::int32_t hdr[2] = {};
        const bool hdrOk = std::fread(magic, 1, 4, f) == 4 && std::memcmp(magic, "LKG1", 4) == 0 &&
                           std::fread(hdr, sizeof(hdr), 1, f) == 1;
        if (!hdrOk || hdr[0] != channels || hdr[1] != frames) {
            std::fclose(f);
            return {false, 0.0, name + ": shape differs from the golden file"};
        }
        std::vector<float> ref(static_cast<size_t>(channels) * static_cast<size_t>(frames));
        const size_t got = std::fread(ref.data(), sizeof(float), ref.size(), f);
        std::fclose(f);
        if (got != ref.size()) return {false, 0.0, name + ": truncated golden file"};
        for (size_t i = 0; i < ref.size(); ++i) {
            const double d = std::fabs(static_cast<double>(ref[i]) - interleaved[i]);
            r.maxDiff = std::isnan(d) ? INFINITY : std::max(r.maxDiff, d);
        }
        r.ok = r.maxDiff <= tol;
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s: max |diff| %.3g (tolerance %.3g)", name.c_str(), r.maxDiff, tol);
        r.message = buf;
        return r;
    }

private:
    std::string dir_;
};

} // namespace lkit::golden
