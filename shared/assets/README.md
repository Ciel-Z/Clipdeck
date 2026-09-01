# Shared visual assets

This directory is the single source of truth for ClipDeck brand images used by
multiple platform clients and their installers.

- `clipdeck.svg` is the shared menu/tray and scalable application icon.
- `Assets.xcassets/AppIcon.appiconset` contains raster application icon sizes.
  Xcode consumes the asset catalog directly, while Linux packaging uses the
  256-pixel PNG from the same source.

Platform directories should reference these files instead of keeping copies.
