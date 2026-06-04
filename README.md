# Daint

[![CI](https://github.com/eagredev/daint/actions/workflows/ci.yml/badge.svg)](https://github.com/eagredev/daint/actions/workflows/ci.yml)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)

A deliberately simple, MS-Paint-style raster paint program for Linux, for when
you just want to pull up a canvas and doodle, without the weight of GIMP or the
fiddliness of a full image editor.

Native KDE app (C++ / Qt6), packaged as a Flatpak.

## Install

Download `daint-0.1.0.flatpak` from the
[latest release](https://github.com/eagredev/daint/releases/latest), then:

```sh
# One-time, only if you don't already have Flathub set up. This provides the
# shared KDE runtime Daint needs; Daint itself is not on Flathub.
flatpak remote-add --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo

flatpak install --user daint-0.1.0.flatpak
flatpak run io.github.eagredev.Daint
```

To build it yourself instead, see [Building & running](#building--running) below.

## What it does

- **Tools:** pencil, eraser, spray/airbrush, line, rectangle, ellipse, free-form
  polygon, flood fill, colour picker, and text.
- **Rainbow brush:** a special swatch in the palette that cycles the spectrum as
  you draw (a freehand cycle, or a diagonal gradient on shapes and fills). The
  fondly-remembered Tux-Paint-era rainbow, brought into a clean modern editor.
- **Two-colour painting** (classic Paint): left button paints Color 1, right
  button paints Color 2. The eraser "erases to" Color 2, and right-dragging it is
  a selective *color-eraser* (replaces only Color-1 pixels). You can even switch
  colour mid-stroke by pressing the other button, and **click the colour
  indicator to swap** the two.
- **Editable palette** with a hand-built *Edit Colours* dialog (hue/sat square +
  value bar + RGB/hex), plus a **Pick from screen** eyedropper that grabs a colour
  from anywhere on screen. **Shape fill toggle** for rectangle/ellipse/polygon
  (outline in one colour, fill in the other).
- **Crisp by default:** sharp, MS-Paint-style pixels, with an optional
  *Smooth Edges* mode (View menu) for anti-aliased strokes.
- **Pixel-art aids:** an optional **grid** (configurable cell size, plus a fine
  1×1 pixel grid) and **rulers** along the top and left edges that track zoom,
  scrolling, and the cursor. A live **x, y coordinate readout** in the status bar.
- **Zoom** with Ctrl+mouse-wheel (anchored on the cursor) or the bottom-right
  slider; **middle-click drag to pan**, with room to scroll past the edges so you
  can work on corners comfortably. Zoomed-in pixels stay crisp and square.
- **Shift to constrain:** lines snap to 45°, rectangles to squares, ellipses to
  circles. (Shift+Ctrl on a line gives a fixed-length radial snap.)
- **Resizable canvas** (new-image size prompt and *Image > Resize Canvas*),
  **undo/redo**, and **image open/save** (opens PNG/JPEG/BMP, saves PNG).

## Building & running

Requires Flatpak and the KDE runtime/SDK. On an immutable distro (e.g. SteamOS)
this is the *only* setup you need. The KDE SDK supplies the compiler and Qt
inside the build sandbox; nothing is installed onto the base system.

One-time toolchain (user-level, no root):

```sh
flatpak install --user flathub org.kde.Sdk//6.10 org.kde.Platform//6.10 org.flatpak.Builder
```

Build, install, and run from the project root:

```sh
flatpak run org.flatpak.Builder --repo=repo --force-clean build-dir io.github.eagredev.Daint.yml
flatpak install --user -y --reinstall daint-local io.github.eagredev.Daint   # first time: adds the local remote
flatpak run io.github.eagredev.Daint
```

> **Note:** always use `--reinstall` when iterating. The local repo commit often
> doesn't change between builds, so a plain install reports "already installed"
> and you'd run stale code. If an install errors, check it's still present with
> `flatpak list --user --app | grep -i daint`.

First-time only, add the local remote (the install command above will need it):

```sh
flatpak remote-add --user --no-gpg-verify --if-not-exists daint-local repo
```

## Project layout

| Path | Purpose |
|------|---------|
| `src/canvas.{h,cpp}` | The canvas: bitmap document, all tools, zoom, pan, text, grid |
| `src/mainwindow.{h,cpp}` | Toolbar, palette, menus, status bar, rulers, file I/O |
| `src/coloreditor.{h,cpp}` | The hand-built *Edit Colours* dialog |
| `src/ruler.{h,cpp}` | Top/left rulers (and a small QScrollArea subclass) |
| `src/main.cpp` | Entry point |
| `io.github.eagredev.Daint.yml` | Flatpak manifest |
| `data/` | `.desktop` launcher, AppStream metainfo, app icon, screenshots |

## Design notes

The document is a single bitmap, with no layers and no objects. That
destructiveness is the point; it's what keeps the app small and immediate. Edits
are destructive but undoable (a snapshot-based undo/redo stack), and shapes are
pixel-snapped so the on-canvas preview matches the committed result exactly.

## Status

Released as v0.1.0, see the [latest release](https://github.com/eagredev/daint/releases/latest)
for the installable Flatpak bundle. The tool set is complete (pencil, eraser,
spray, line, rectangle, ellipse, free-form polygon, fill, picker, text, plus the
rainbow brush), with an editable palette, a resizable canvas, and pixel-art aids
(grid + rulers).

Known gap: Daint works in RGB, so it does not yet round-trip indexed-palette PNGs
(the format used by some sprite pipelines). Indexed-palette support is the planned
next feature.

## License

GPLv3 (GNU General Public License, version 3 or later); see [LICENSE](LICENSE).

A copyleft license is the deliberate choice for Daint: it's a Qt/KDE desktop app
(the ecosystem norm, since KolourPaint and Krita are GPL too), it keeps any forks and
derivatives open, and it's honest about the fact that the Qt the binary links
against is itself LGPL/GPL. (This differs on purpose from the MIT-licensed inkmd,
which is a library/CLI meant to be embedded anywhere.)
