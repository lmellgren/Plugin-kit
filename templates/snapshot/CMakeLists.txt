# <plugin>_snapshot: the plugin window rendered to a PNG without a host (face check against the Seam preview).
# Copy to <plugin>/tools/snapshot/, replace MyPlugin / my_ names, add_subdirectory(tools/snapshot) after the plugin.
juce_add_console_app(MyPluginSnapshot PRODUCT_NAME "my_snapshot")
target_sources(MyPluginSnapshot PRIVATE snapshot.cpp)
target_include_directories(MyPluginSnapshot PRIVATE "${CMAKE_SOURCE_DIR}/plugin")
target_compile_definitions(MyPluginSnapshot PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(MyPluginSnapshot PRIVATE MyPlugin lkit::juce lkit::seam juce::juce_audio_utils
  juce::juce_recommended_config_flags)
lkit_seam_tokens(MyPluginSnapshot)
