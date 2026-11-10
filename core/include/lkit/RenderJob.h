#pragma once
// Offline render driver for a plugin's DSP core (the counterpart of lkit_measure for our own code;
// generalised from Passive EQ tools/render). Reads the text job written by lkit.jobs (plugin
// "render:<exe>"), renders every run with the same protocol as lkit_measure (settle 0.5 s, reset,
// pre-roll, stimulus + tail) and writes NAME.f32 (interleaved float32) + NAME.json.
//
// A plugin's render tool is a few lines:
//   int main(int argc, char** argv) {
//       return lkit::render::main(argc, argv, "my_core", [](const lkit::render::Settings& s, double fs, int block) {
//           auto core = std::make_shared<MyCore>(); core->prepare(fs, block, 2); core->apply(s);
//           return lkit::render::Engine{[core](float* const* ch, int n) { core->process(ch, 2, n); },
//                                       [core] { core->reset(); }, core->latencySamples()};
//       });
//   }

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace lkit::render {

using Settings = std::map<std::string, std::string>;   // panel key -> value (as in the job)

inline double num(const Settings& s, const std::string& k, double dflt)
{
    auto it = s.find(k);
    return it == s.end() ? dflt : std::atof(it->second.c_str());
}
inline std::string str(const Settings& s, const std::string& k, const std::string& dflt)
{
    auto it = s.find(k);
    return it == s.end() ? dflt : it->second;
}

struct Engine {
    std::function<void(float* const* ch, int n)> process;   // stereo, in place
    std::function<void()> reset;
    int latency = 0;
};

using Factory = std::function<Engine(const Settings&, double fs, int block)>;

inline int main(int argc, char** argv, const char* name, const Factory& make)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s JOB.txt OUTDIR\n", argv[0]);
        return 2;
    }
    std::ifstream job(argv[1]);
    const std::string outDir = argv[2];
    double fs = 48000, preroll = 0.5, tail = 2.0;
    int block = 512;
    std::vector<float> stim;
    std::string line;
    while (std::getline(job, line)) {
        std::istringstream ss(line);
        std::string key;
        ss >> key;
        if (key == "sample_rate") ss >> fs;
        else if (key == "block_size") ss >> block;
        else if (key == "preroll_s") ss >> preroll;
        else if (key == "tail_s") ss >> tail;
        else if (key == "stimulus") {
            std::string path;
            ss >> path;
            std::ifstream f(path, std::ios::binary | std::ios::ate);
            const auto bytes = static_cast<size_t>(f.tellg());
            stim.resize(bytes / sizeof(float));
            f.seekg(0);
            f.read(reinterpret_cast<char*>(stim.data()), static_cast<std::streamsize>(bytes));
        } else if (key == "run") {
            std::string runName, kv;
            double gain = 1.0;
            ss >> runName >> gain;
            Settings s;
            while (ss >> kv) {
                const auto eq = kv.find('=');
                if (eq != std::string::npos) s[kv.substr(0, eq)] = kv.substr(eq + 1);
            }
            Engine e = make(s, fs, block);
            std::vector<float> l(static_cast<size_t>(block)), r(static_cast<size_t>(block)), out;
            float* ch[2] = {l.data(), r.data()};
            auto run = [&](size_t n, bool signal, bool keep) {
                for (size_t pos = 0; pos < n; pos += static_cast<size_t>(block)) {
                    const int m = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), n - pos));
                    for (int i = 0; i < m; ++i) {
                        const size_t k = pos + static_cast<size_t>(i);
                        const float x = (signal && k < stim.size()) ? static_cast<float>(stim[k] * gain) : 0.0f;
                        l[static_cast<size_t>(i)] = r[static_cast<size_t>(i)] = x;
                    }
                    e.process(ch, m);
                    if (keep)
                        for (int i = 0; i < m; ++i) out.push_back(l[static_cast<size_t>(i)]), out.push_back(r[static_cast<size_t>(i)]);
                }
            };
            run(static_cast<size_t>(0.5 * fs), false, false);   // settle
            if (e.reset) e.reset();
            run(static_cast<size_t>(preroll * fs), false, false);
            run(stim.size() + static_cast<size_t>(tail * fs + 0.5), true, true);
            std::ofstream o(outDir + "/" + runName + ".f32", std::ios::binary);
            o.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size() * sizeof(float)));
            std::ofstream mj(outDir + "/" + runName + ".json");
            mj << "{\"name\": \"" << runName << "\", \"plugin\": {\"name\": \"" << name
               << "\", \"version\": 0, \"version_string\": \"core\", \"type\": \"core\", \"subtype\": \"core\", "
                  "\"manufacturer\": \"Lmas\"}, \"sample_rate\": "
               << fs << ", \"block_size\": " << block << ", \"channels\": 2, \"frames\": " << out.size() / 2
               << ", \"stimulus_frames\": " << stim.size() << ", \"gain\": " << gain
               << ", \"latency_reported_s\": " << e.latency / fs
               << ", \"tail_time_s\": 0, \"readback\": {}, \"readback_after\": {}, \"params_ok\": true}\n";
            std::fprintf(stderr, "ok   %s\n", runName.c_str());
        }
    }
    return 0;
}

} // namespace lkit::render
