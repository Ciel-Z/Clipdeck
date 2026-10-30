#pragma once

#include <QImage>

// The Deck material. DWM acrylic cannot be clipped to the Deck's 24 pt corners
// on a DirectComposition window, so (like the Linux client) the screen behind
// the Deck is captured when it opens and turned into a frosted backdrop here;
// QML scales it back up and adds the macOS tint layers on top.
namespace DeckBackdrop {

// Downscales by `downscale`, box-blurs `passes` times with `radius`, and
// raises saturation (vibrancy). The result is small; bilinear upscaling in
// the scene graph completes the blur.
QImage frost(const QImage &screen, int downscale = 8, int radius = 3, int passes = 3, double saturation = 1.18);

} // namespace DeckBackdrop
