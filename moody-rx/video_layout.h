#pragma once

// A negative result intentionally places an oversized frame beyond the display
// edge, producing an even centre crop without changing the image's proportions.
constexpr int centeredCoordinate(int viewportSize, int contentSize) {
  return (viewportSize - contentSize) / 2;
}

// Map destination pixel centres back into a source image using CSS-style
// "cover" geometry: preserve aspect ratio, fill the destination, and crop the
// excess equally on the two opposite edges.
constexpr int coverCropSourceX(int destinationX,
                               int sourceWidth,
                               int sourceHeight,
                               int destinationWidth,
                               int destinationHeight) {
  const bool scaleFromWidth =
    destinationWidth * sourceHeight >= destinationHeight * sourceWidth;
  if (scaleFromWidth) {
    return ((2 * destinationX + 1) * sourceWidth) /
           (2 * destinationWidth);
  }

  return (sourceWidth * destinationHeight +
          (2 * destinationX + 1 - destinationWidth) * sourceHeight) /
         (2 * destinationHeight);
}

constexpr int coverCropSourceY(int destinationY,
                               int sourceWidth,
                               int sourceHeight,
                               int destinationWidth,
                               int destinationHeight) {
  const bool scaleFromWidth =
    destinationWidth * sourceHeight >= destinationHeight * sourceWidth;
  if (!scaleFromWidth) {
    return ((2 * destinationY + 1) * sourceHeight) /
           (2 * destinationHeight);
  }

  return (sourceHeight * destinationWidth +
          (2 * destinationY + 1 - destinationHeight) * sourceWidth) /
         (2 * destinationWidth);
}
