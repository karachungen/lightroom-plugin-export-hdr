#include "gui/hdr_rhi_viewport.h"

#import <AppKit/AppKit.h>

namespace uhdr_repack {

bool macos_display_supports_hdr() {
  NSScreen* screen = [NSScreen mainScreen];
  if (!screen) return false;
  if ([screen respondsToSelector:@selector(maximumPotentialExtendedDynamicRangeColorComponentValue)]) {
    return screen.maximumPotentialExtendedDynamicRangeColorComponentValue > 1.0;
  }
  return false;
}

}  // namespace uhdr_repack
