#!/bin/zsh
# Plugin-kit safety net: build (core + JUCE parts) and run all kit tests. Keep green.
set -euo pipefail
cd "${0:A:h}"
JUCE_DIR=${JUCE_DIR:-$HOME/Documents/ChatGPT/Plugins/.build-deps/JUCE-8.0.6}
[[ -x .venv/bin/python ]] || { /opt/homebrew/bin/python3.13 -m venv .venv && .venv/bin/pip install -q -r requirements.txt; }
cmake -S . -B build -DLKIT_BUILD_JUCE=ON -DJUCE_DIR="$JUCE_DIR" >/dev/null
cmake --build build -j 10 >/dev/null
ctest --test-dir build --output-on-failure
print "kit check: all green"
