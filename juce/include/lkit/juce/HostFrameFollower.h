#pragma once
// HostFrameFollower (from LOMAS Pre 73 P7e, in the kit since Vari-mu P7; link lkit::hostframe): lets an editor follow
// the size a host gives the native view the editor's window sits in.
//
// Why: JUCE 8.0.6's AUv2 wrapper passes the editor's own setSize to the host (the AU view and its superview get the
// new frame), but not the other way: when the host sets the AU view's frame, JUCE's own view inside keeps its size
// (autoresizing mask 0) and the editor never sees it. LOMAS chain mode (Seam 1C) needs that direction: the host sets
// the height, the editor picks its layout. No JUCE fork, no hidden parameter.
//
// How: once the editor has a peer, observe NSViewFrameDidChangeNotification on the peer view's superview (AUv2: the
// JUCEAUView_ the host holds) and call back, asynchronously on the message thread, with that view's size in points.
// The parent view is never retained: in AUv2 its dealloc is what deletes the editor (a retain would keep the editor
// alive after the processor; found by pluginval). The size is read from the notification itself. Echoes are ignored: notifications while the callback runs (the editor resizing itself), and frames equal to the
// editor's own size. Guarded by tools/auview (check.sh): if a JUCE update changes the view nesting, the check fails.
// Use it only where the native parent is the host's plugin view (AUv2); a standalone window has no such parent.
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace lkit {

class HostFrameFollower final : private juce::ComponentMovementWatcher {
public:
    using Callback = std::function<void(int width, int height)>;
    HostFrameFollower(juce::Component& editor, Callback onHostFrame);
    ~HostFrameFollower() override;
    bool isAttached() const { return token_ != nullptr; }

private:
    void componentPeerChanged() override { attach(); }
    void componentMovedOrResized(bool, bool) override {}
    void componentVisibilityChanged() override {}
    void attach();
    void detach();
    void frameChanged(int width, int height);

    juce::Component& comp_;
    Callback cb_;
    void* token_ = nullptr;   // NSNotificationCenter observer (retained; the observed view is not)
    juce::Rectangle<int> latest_;
    bool busy_ = false;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);

    JUCE_DECLARE_NON_COPYABLE(HostFrameFollower)
};

} // namespace lkit
