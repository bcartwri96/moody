#include "../moody-tx/video_profile.h"

int main() {
  static_assert(kSoftwareJpegWidth == 176);
  static_assert(kSoftwareJpegHeight == 144);
  static_assert(kSoftwareJpegQuality == 100);
  static_assert(kSoftwareJpegOutputBytes >=
                static_cast<size_t>(kSoftwareJpegWidth) *
                kSoftwareJpegHeight * 2);
}
