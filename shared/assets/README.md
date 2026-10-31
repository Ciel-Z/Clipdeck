# Shared visual assets

This directory is the single source of truth for ClipDeck brand images used by
multiple platform clients and their installers. Every icon comes from one
design — stacked cards with a `</>` mark on a transparent background — rendered
by `generate_icons.py`. Regenerate them all with
`python -I generate_icons.py` (needs Pillow).

- `clipdeck.svg` is the menu bar (macOS) and tray (Linux) icon. It is only shown
  at 18–24 px, so it uses the simplified two-card stack.
- `Assets.xcassets/AppIcon.appiconset` contains the raster application icon
  sizes. Xcode consumes the asset catalog directly, while Linux packaging
  installs the 32–256 px PNGs as the launcher icon.
- `windows/` holds the Windows icon set (`icon-*.png`, `clipdeck.ico`) for the
  executable, taskbar and tray.

Platform directories should reference these files instead of keeping copies.
