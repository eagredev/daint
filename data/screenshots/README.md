# Screenshots for the Flathub / AppStream listing

These are the store screenshots referenced by
`data/io.github.eagredev.Daint.metainfo.xml`. Drop the captured PNGs here with
the exact filenames below, then the metainfo URLs resolve once the repo is
pushed and tagged.

## What to capture (3 shots, matching the metainfo captions)

1. **`01-canvas.png`** - "The Daint canvas with the tool palette"
   A clean, inviting default view: the toolbar, the two-colour palette, and a
   simple doodle on the canvas. This is the `type="default"` shot (the one shown
   first in the store), so make it the most representative.

2. **`02-rainbow.png`** - "Drawing with the rainbow brush"
   The rainbow swatch selected and a visible rainbow stroke (and/or a rainbow
   gradient shape/fill). This is the feature that sells Daint, so make it pop.

3. **`03-pixelart.png`** - "Pixel-art work with the grid and rulers"
   Zoomed in with the grid + rulers turned on, a small piece of pixel art in
   progress. Shows the editor-grade side.

## Specs (Flathub requirements)

- **Format:** PNG.
- **Width:** at least 1000px wide; aim for roughly 1200-1600px. The window on a
  1080p display at a sensible size is fine. Avoid full-4K (huge files).
- **Aspect:** landscape, the natural window shape. Don't pad to a fixed ratio.
- **Content:** the app window only (no desktop wallpaper / other windows / mouse
  cursor clutter). A tight window-only grab is cleanest.
- **No personal info** anywhere in frame (file paths, other apps).

## How to capture on KDE (Spectacle)

- Spectacle -> "Active Window" (or rectangular region around just the window).
- Save as PNG into this folder with the filename above.

## After capturing

Re-run the validator to confirm the listing is green:

```sh
flatpak run --filesystem="$PWD" --command=appstreamcli org.kde.Sdk//6.10 \
  validate "$PWD/data/io.github.eagredev.Daint.metainfo.xml"
```

(URL-reachability warnings are expected until the repo is pushed + the `v0.1.0`
tag exists; everything else should pass.)
