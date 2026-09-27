#include "../moody-rx/video_layout.h"

#include <cassert>

int main() {
  // Matching frames fill the screen from the top-left corner.
  static_assert(centeredCoordinate(480, 480) == 0);
  static_assert(centeredCoordinate(320, 320) == 0);

  // Larger frames remain centred, cropping evenly instead of stretching.
  static_assert(centeredCoordinate(480, 640) == -80);
  static_assert(centeredCoordinate(320, 480) == -80);
  // An odd difference puts the spare cropped pixel on the far edge.
  static_assert(centeredCoordinate(480, 481) == 0);

  // Smaller frames remain centred when a fallback profile is received.
  static_assert(centeredCoordinate(480, 320) == 80);
  static_assert(centeredCoordinate(320, 240) == 40);

  // A 176x144 frame is narrower than the 480x320 display. Cover scaling
  // therefore preserves the full width and crops equal amounts vertically.
  static_assert(coverCropSourceX(0, 176, 144, 480, 320) == 0);
  static_assert(coverCropSourceX(479, 176, 144, 480, 320) == 175);
  static_assert(coverCropSourceY(0, 176, 144, 480, 320) == 13);
  static_assert(coverCropSourceY(319, 176, 144, 480, 320) == 130);

  // Exact-aspect inputs map one-to-one, while wide inputs crop horizontally.
  static_assert(coverCropSourceX(479, 480, 320, 480, 320) == 479);
  static_assert(coverCropSourceY(319, 480, 320, 480, 320) == 319);
  static_assert(coverCropSourceX(0, 640, 320, 480, 320) == 80);
  static_assert(coverCropSourceX(479, 640, 320, 480, 320) == 559);
  static_assert(coverCropSourceY(0, 640, 320, 480, 320) == 0);
  static_assert(coverCropSourceY(319, 640, 320, 480, 320) == 319);

  int previousX = -1;
  for (int x = 0; x < 480; ++x) {
    const int sourceX = coverCropSourceX(x, 176, 144, 480, 320);
    assert(sourceX >= previousX && sourceX >= 0 && sourceX < 176);
    previousX = sourceX;
  }

  int previousY = -1;
  for (int y = 0; y < 320; ++y) {
    const int sourceY = coverCropSourceY(y, 176, 144, 480, 320);
    assert(sourceY >= previousY && sourceY >= 0 && sourceY < 144);
    previousY = sourceY;
  }
}
