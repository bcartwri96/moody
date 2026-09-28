#include <assert.h>
#include <stdint.h>
#include "../moody-rx/volume_control.h"

using moody_rx::TouchEdge;
using moody_rx::VolumeControl;

int main() {
  // A held finger is one tap; a new tap needs a release first.
  TouchEdge edge;
  assert(!edge.pressed(false));
  assert(edge.pressed(true));
  assert(!edge.pressed(true));
  assert(!edge.pressed(false));
  assert(edge.pressed(true));

  // Initial volume is clamped and snapped to the 10% step grid.
  assert(VolumeControl(70).volume() == 70);
  assert(VolumeControl(250).volume() == 100);
  assert(VolumeControl(64).volume() == 60);

  VolumeControl control(70);
  assert(!control.overlayVisible());

  // Right half steps up, left half steps down; the midpoint counts as right.
  control.onTap(400, 480, 1000);
  assert(control.volume() == 80 && control.overlayVisible());
  control.onTap(239, 480, 1100);
  assert(control.volume() == 70);
  control.onTap(240, 480, 1260);
  assert(control.volume() == 80);

  // Overlay stays for 1.5 s after the latest tap, then expires exactly once.
  assert(!control.overlayExpired(1260 + 1499));
  assert(control.overlayExpired(1260 + 1500));
  assert(!control.overlayVisible());
  assert(!control.overlayExpired(5000));

  // Taps at the limits still show the overlay but clamp.
  VolumeControl top(100);
  top.onTap(470, 480, 0);
  assert(top.volume() == 100 && top.overlayVisible());
  VolumeControl bottom(0);
  bottom.onTap(5, 480, 0);
  assert(bottom.volume() == 0 && bottom.overlayVisible());

  // Timing survives millis() wraparound.
  VolumeControl wrap(50);
  wrap.onTap(400, 480, 0xffffff00U);
  assert(!wrap.overlayExpired(0x00000100U));
  assert(wrap.overlayExpired(0xffffff00U + 1500U));

  // UI percent maps onto the codec's usable range: 0 mutes, 70 keeps the
  // previous fixed codec level, 100 stays well below the codec's +32 dB top.
  assert(moody_rx::codecVolumeFor(0) == 0);
  assert(moody_rx::codecVolumeFor(10) == 46);
  assert(moody_rx::codecVolumeFor(70) == 70);
  assert(moody_rx::codecVolumeFor(100) == 82);
  assert(moody_rx::codecVolumeFor(250) == 82);
}
