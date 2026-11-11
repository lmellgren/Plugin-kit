// my_snapshot: render the plugin window to a PNG without a host (design review). Usage: my_snapshot out.png [scale]
// Set the parameters to the Seam preview's values (seamgen <plugin>_body) so the face can be compared.
#include "lkit/juce/Snapshot.h"

#include <cstdlib>

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    std::unique_ptr<juce::AudioProcessor> p(createPluginFilter());
    // lkit::setPlain(*p, "someParam", 4.0f);
    lkit::feedTone(*p, 48000.0, 512, 0.2f, 0.95f);   // about -14 dBFS, so the meters show a level
    const float scale = argc > 2 ? static_cast<float>(std::atof(argv[2])) : 2.0f;
    return lkit::writeEditorSnapshot(*p, argc > 1 ? argv[1] : "snapshot.png", scale) ? 0 : 1;
}
