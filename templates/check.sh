#!/bin/zsh
# Safety net (house standard): run before every commit. Copy to <plugin>/tools/check.sh and set the
# names below. Green = build (core + plugin AU/VST3), unit tests incl. bypass bit-exact, state round-trip
# and golden files (ctest), kit tests, auval, pluginval (strictness from PLUGINVAL_STRICTNESS, default 8).
set -euo pipefail
ROOT=${0:A:h:h}
cd "$ROOT"
JUCE_DIR=${JUCE_DIR:-$HOME/Documents/ChatGPT/Plugins/.build-deps/JUCE-8.0.6}
BUILD=${BUILD:-build}
PLUGIN_TARGET=${PLUGIN_TARGET:?set PLUGIN_TARGET (the juce_add_plugin target)}
PRODUCT=${PRODUCT:?set PRODUCT (bundle name, e.g. "LOMAS Pre 73")}
AU_CODES=${AU_CODES:?set AU_CODES (type subtype manufacturer, e.g. "aufx Lp73 Lmas")}
STRICT=${PLUGINVAL_STRICTNESS:-8}
PLUGINVAL=${PLUGINVAL:-/Applications/pluginval.app/Contents/MacOS/pluginval}

step() { print -P "%F{cyan}== $1%f" }
step "configure + build"
cmake -S . -B "$BUILD" -DJUCE_DIR="$JUCE_DIR" >/dev/null
cmake --build "$BUILD" -j 10 >/dev/null
step "ctest"
ctest --test-dir "$BUILD" --output-on-failure
ART="$BUILD/plugin/${PLUGIN_TARGET}_artefacts/Release"
step "install AU to ~/Library/Audio/Plug-Ins/Components (for auval)"
rm -rf "$HOME/Library/Audio/Plug-Ins/Components/$PRODUCT.component"
cp -R "$ART/AU/$PRODUCT.component" "$HOME/Library/Audio/Plug-Ins/Components/"
killall -9 AudioComponentRegistrar 2>/dev/null || true
step "auval"
auval -v ${=AU_CODES} | tail -3 | grep -q "AU VALIDATION SUCCEEDED" || { auval -v ${=AU_CODES} | tail -40; exit 1; }
print "auval: AU VALIDATION SUCCEEDED"
step "pluginval (strictness $STRICT): VST3 and AU"
for f in "$ART/VST3/$PRODUCT.vst3" "$HOME/Library/Audio/Plug-Ins/Components/$PRODUCT.component"; do
  "$PLUGINVAL" --strictness-level "$STRICT" --validate-in-process --timeout-ms 600000 --skip-gui-tests --validate "$f" \
    > "$BUILD/pluginval-${f:t:r}-${f:e}.log" 2>&1 || { tail -40 "$BUILD/pluginval-${f:t:r}-${f:e}.log"; exit 1; }
  print "pluginval ${f:e}: SUCCESS"
done
print -P "%F{green}check.sh: all green%f"
