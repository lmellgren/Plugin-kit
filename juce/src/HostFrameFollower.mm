#include "lkit/juce/HostFrameFollower.h"

#import <AppKit/AppKit.h>

namespace lkit {

HostFrameFollower::HostFrameFollower(juce::Component& editor, Callback onHostFrame)
    : juce::ComponentMovementWatcher(&editor), comp_(editor), cb_(std::move(onHostFrame))
{
    attach();
}

HostFrameFollower::~HostFrameFollower() { detach(); }

void HostFrameFollower::detach()
{
    if (token_ != nullptr) {
        [[NSNotificationCenter defaultCenter] removeObserver:(id) token_];
        CFRelease(token_);
        token_ = nullptr;
    }
}

void HostFrameFollower::attach()
{
    detach();
    auto* peer = comp_.getPeer();
    if (peer == nullptr) return;
    auto* own = static_cast<NSView*>(peer->getNativeHandle());
    NSView* parent = own != nil ? [own superview] : nil;
    if (parent == nil) return;
    [parent setPostsFrameChangedNotifications:YES];
    HostFrameFollower* self = this;   // the observer is removed in detach(), before this goes away
    id tok = [[NSNotificationCenter defaultCenter] addObserverForName:NSViewFrameDidChangeNotification
                                                               object:parent
                                                                queue:nil
                                                           usingBlock:^(NSNotification* note) {
                                                               const NSSize s = [(NSView*) note.object frame].size;
                                                               self->frameChanged(juce::roundToInt(s.width), juce::roundToInt(s.height));
                                                           }];
    token_ = const_cast<void*>(CFBridgingRetain(tok));
}

void HostFrameFollower::frameChanged(int width, int height)
{
    latest_ = {width, height};
    if (busy_ || !cb_) return;
    if (latest_ == comp_.getLocalBounds()) return;   // our own size coming back from the host
    juce::MessageManager::callAsync([this, alive = std::weak_ptr<int>(alive_)] {
        if (alive.expired() || token_ == nullptr) return;
        const auto s = latest_;   // the latest frame (several changes in a row collapse into one)
        if (s.isEmpty() || s == comp_.getLocalBounds()) return;
        const juce::ScopedValueSetter<bool> guard(busy_, true);
        cb_(s.getWidth(), s.getHeight());
    });
}

} // namespace lkit
