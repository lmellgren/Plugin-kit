// lkit_vst3render – headless host for our own VST3 (and AU) builds: renders the lkit_measure host job
// (JSON) through a plugin, so VST3 == AU can be checked with the same analysis (from Passive EQ).
// Usage: lkit_vst3render PLUGIN.vst3|PLUGIN.component JOB.json OUTDIR
// Job runs give "params" as {parameter name: normalised value 0..1}. Protocol as lkit_measure: settle
// 0.5 s, reset, pre-roll, then stimulus + tail. Writes NAME.f32 (interleaved) + NAME.json.
#include <juce_audio_processors/juce_audio_processors.h>

#include <cstdio>

int main(int argc, char** argv)
{
    if (argc < 4) {
        std::fprintf(stderr, "usage: lkit_vst3render PLUGIN JOB.json OUTDIR\n");
        return 2;
    }
    juce::ScopedJuceInitialiser_GUI gui;
    juce::AudioPluginFormatManager fm;
    fm.addDefaultFormats();
    juce::OwnedArray<juce::PluginDescription> types;
    for (auto* f : fm.getFormats()) f->findAllTypesForFile(types, argv[1]);
    if (types.isEmpty()) {
        std::fprintf(stderr, "error: no plugin in %s\n", argv[1]);
        return 3;
    }
    const auto job = juce::JSON::parse(juce::File(argv[2]));
    const double fs = job.getProperty("sample_rate", 48000.0);
    const int block = job.getProperty("block_size", 512);
    const double preroll = job.getProperty("preroll_s", 0.5), tail = job.getProperty("tail_s", 2.0);
    juce::MemoryBlock stimData;
    juce::File(job.getProperty("stimulus", "").toString()).loadFileAsData(stimData);
    const auto* stim = static_cast<const float*>(stimData.getData());
    const size_t stimN = stimData.getSize() / sizeof(float);
    const juce::File outDir(argv[3]);
    outDir.createDirectory();
    int failures = 0;
    if (auto* runs = job.getProperty("runs", {}).getArray())
        for (const auto& run : *runs) {
            juce::String err;
            auto inst = fm.createPluginInstance(*types[0], fs, block, err);
            if (!inst) {
                std::fprintf(stderr, "error: %s\n", err.toRawUTF8());
                return 3;
            }
            inst->setPlayConfigDetails(2, 2, fs, block);
            bool ok = true;
            if (auto* params = run.getProperty("params", {}).getDynamicObject())
                for (const auto& kv : params->getProperties()) {
                    bool found = false;
                    for (auto* prm : inst->getParameters())
                        if (prm->getName(64) == kv.name.toString()) {
                            prm->setValueNotifyingHost(static_cast<float>(static_cast<double>(kv.value)));
                            found = true;
                        }
                    if (!found) {
                        std::fprintf(stderr, "warning: parameter not found: %s\n", kv.name.toString().toRawUTF8());
                        ok = false;
                    }
                }
            inst->prepareToPlay(fs, block);
            const double gain = run.getProperty("gain", 1.0);
            juce::AudioBuffer<float> buf(2, block);
            juce::MidiBuffer midi;
            std::vector<float> out;
            auto render = [&](size_t n, bool signal, bool keep) {
                for (size_t pos = 0; pos < n; pos += static_cast<size_t>(block)) {
                    const int m = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), n - pos));
                    buf.setSize(2, m, false, false, true);
                    for (int i = 0; i < m; ++i) {
                        const size_t k = pos + static_cast<size_t>(i);
                        const float x = (signal && k < stimN) ? static_cast<float>(stim[k] * gain) : 0.0f;
                        buf.setSample(0, i, x);
                        buf.setSample(1, i, x);
                    }
                    inst->processBlock(buf, midi);
                    if (keep)
                        for (int i = 0; i < m; ++i) out.push_back(buf.getSample(0, i)), out.push_back(buf.getSample(1, i));
                }
            };
            render(static_cast<size_t>(0.5 * fs), false, false);
            inst->reset();
            render(static_cast<size_t>(preroll * fs), false, false);
            render(stimN + static_cast<size_t>(tail * fs + 0.5), true, true);
            const juce::String name = run.getProperty("name", "run").toString();
            outDir.getChildFile(name + ".f32").replaceWithData(out.data(), out.size() * sizeof(float));
            auto meta = std::make_unique<juce::DynamicObject>();
            auto plugin = std::make_unique<juce::DynamicObject>();
            plugin->setProperty("name", types[0]->name + " (" + types[0]->pluginFormatName + ")");
            plugin->setProperty("version", 0);
            plugin->setProperty("version_string", types[0]->version);
            plugin->setProperty("type", types[0]->pluginFormatName);
            plugin->setProperty("subtype", types[0]->pluginFormatName);
            plugin->setProperty("manufacturer", types[0]->manufacturerName);
            meta->setProperty("name", name);
            meta->setProperty("plugin", plugin.release());
            meta->setProperty("sample_rate", fs);
            meta->setProperty("block_size", block);
            meta->setProperty("channels", 2);
            meta->setProperty("frames", static_cast<int>(out.size() / 2));
            meta->setProperty("stimulus_frames", static_cast<int>(stimN));
            meta->setProperty("gain", gain);
            meta->setProperty("latency_reported_s", inst->getLatencySamples() / fs);
            meta->setProperty("tail_time_s", inst->getTailLengthSeconds());
            meta->setProperty("readback", new juce::DynamicObject());
            meta->setProperty("readback_after", new juce::DynamicObject());
            meta->setProperty("params_ok", ok);
            outDir.getChildFile(name + ".json").replaceWithText(juce::JSON::toString(juce::var(meta.release())));
            inst->releaseResources();
            std::fprintf(stderr, "%s %s\n", ok ? "ok  " : "FAIL", name.toRawUTF8());
            failures += ok ? 0 : 1;
        }
    return failures ? 4 : 0;
}
