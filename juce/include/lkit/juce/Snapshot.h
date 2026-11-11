#pragma once
// Plugin window -> PNG without a host (face check against the Seam preview; from Pre 73 tools/snapshot, Vari-mu P1).
//
//   juce::ScopedJuceInitialiser_GUI gui;
//   std::unique_ptr<juce::AudioProcessor> p(createPluginFilter());
//   lkit::setPlain(*p, "high", 4.0f);                    // the preview's settings, in plain units
//   lkit::feedTone(*p, 48000.0, 512, 0.2f, 0.95f);        // a steady tone so the meters show a level
//   return lkit::writeEditorSnapshot(*p, "out.png", 2.0f) ? 0 : 1;
//
// templates/snapshot/ has the console app (CMake + main) to copy into <plugin>/tools/snapshot.
#include <juce_audio_processors/juce_audio_processors.h>

#if !JUCE_MODAL_LOOPS_PERMITTED
#error "lkit/juce/Snapshot.h needs JUCE_MODAL_LOOPS_PERMITTED=1 on the target (runDispatchLoopUntil)"
#endif

#include <cmath>
#include <cstdio>

namespace lkit {

// Set a parameter in plain units (notifies the host, as a user would).
inline bool setPlain(juce::AudioProcessor& p, const juce::String& id, float plain)
{
    for (auto* prm : p.getParameters())
        if (auto* r = dynamic_cast<juce::RangedAudioParameter*>(prm); r && r->getParameterID() == id) {
            r->setValueNotifyingHost(r->convertTo0to1(plain));
            return true;
        }
    return false;
}

// Prepare and run `blocks` blocks of a steady sine (peak `amp`, the second channel scaled by `ratioB`).
inline void feedTone(juce::AudioProcessor& p, double fs, int block, float amp, float ratioB = 1.0f, int blocks = 40)
{
    const int nch = juce::jmax(p.getTotalNumInputChannels(), p.getTotalNumOutputChannels());
    p.prepareToPlay(fs, block);
    juce::AudioBuffer<float> b(nch, block);
    juce::MidiBuffer midi;
    for (int k = 0; k < blocks; ++k) {
        for (int i = 0; i < block; ++i) {
            const float x = amp * std::sin(0.13f * static_cast<float>(k * block + i));
            for (int ch = 0; ch < nch; ++ch) b.setSample(ch, i, ch == 0 ? x : ratioB * x);
        }
        p.processBlock(b, midi);
    }
}

// Open the editor, let its timers run for `settleMs` (meters read their peaks), and write it to a PNG at `scale`.
// A relative path is taken from the current working directory.
inline bool writeEditorSnapshot(juce::AudioProcessor& p, const juce::String& path, float scale = 2.0f, int settleMs = 200)
{
    std::unique_ptr<juce::AudioProcessorEditor> ed(p.createEditorIfNeeded());
    if (ed == nullptr) return false;
    ed->setVisible(true);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(settleMs);
    auto img = ed->createComponentSnapshot(ed->getLocalBounds(), true, scale);
    juce::File out = juce::File::getCurrentWorkingDirectory().getChildFile(path);
    out.deleteFile();
    bool ok = false;
    {
        juce::FileOutputStream os(out);
        ok = os.openedOk() && juce::PNGImageFormat().writeImageToStream(img, os);
    }
    std::printf("wrote %s (%d x %d pt)\n", out.getFullPathName().toRawUTF8(), ed->getWidth(), ed->getHeight());
    p.editorBeingDeleted(ed.get());
    return ok;
}

} // namespace lkit
