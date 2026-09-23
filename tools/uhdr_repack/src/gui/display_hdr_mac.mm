#include "gui/hdr_rhi_viewport.h"

#import <AppKit/AppKit.h>

#include <QScreen>
#include <QWindow>

#include <algorithm>

namespace uhdr_repack {

namespace {

NSScreen* screen_named(const QString& name) {
  if (name.isEmpty()) return nil;
  for (NSScreen* screen in [NSScreen screens]) {
    if (QString::fromNSString(screen.localizedName) == name) return screen;
  }
  return nil;
}

NSScreen* screen_for_window(QWindow* window) {
  if (window && window->handle()) {
    NSView* view = reinterpret_cast<NSView*>(window->winId());
    if (view.window.screen) return view.window.screen;
  }
  if (window) {
    if (NSScreen* named = screen_named(window->screen() ? window->screen()->name() : QString())) {
      return named;
    }
  }
  return [NSScreen mainScreen];
}

float headroom_of(NSScreen* screen) {
  if (!screen) return 1.0f;
  CGFloat headroom = 1.0;
  if ([screen respondsToSelector:@selector(maximumExtendedDynamicRangeColorComponentValue)]) {
    headroom = std::max(headroom, screen.maximumExtendedDynamicRangeColorComponentValue);
  }
  if ([screen respondsToSelector:@selector(maximumPotentialExtendedDynamicRangeColorComponentValue)]) {
    headroom = std::max(headroom, screen.maximumPotentialExtendedDynamicRangeColorComponentValue);
  }
  return static_cast<float>(headroom);
}

}  // namespace

float macos_window_edr_headroom(QWindow* window) {
  return headroom_of(screen_for_window(window));
}

bool macos_display_supports_hdr(QWindow* window) {
  return macos_window_edr_headroom(window) > 1.0f;
}

}  // namespace uhdr_repack
