// Daint: a simple MS-Paint-style raster paint program.
// Copyright (C) 2026  eagre.dev
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. This program is distributed WITHOUT ANY WARRANTY; see the GNU General
// Public License (LICENSE file) for details.

#pragma once

#include <QWidget>
#include <QImage>
#include <QColor>
#include <QPoint>
#include <QVector>
#include <QLinearGradient>

class QPainter;

// Canvas is the drawing surface and the document at once.
//
// The whole "document" is a single QImage (a flat RGBA bitmap) -- there are no
// layers, no objects, no scene graph. That destructiveness is the point: it is
// what keeps an MS-Paint-style editor simple. Tools just mutate the pixels.
//
// Undo/redo is implemented by snapshotting the QImage before each edit and
// pushing it onto a stack. For a small canvas this is wasteful in theory and
// completely fine in practice -- a 1080p RGBA image is ~8 MB, and we cap the
// history depth so memory stays bounded.
class Canvas : public QWidget
{
    Q_OBJECT

public:
    // The tools the user can select. "Pencil" and "Eraser" are freehand;
    // the shape tools rubber-band from press to release; Fill is a flood fill;
    // Picker samples a colour back into the active colour.
    enum class Tool {
        Pencil,
        Eraser,
        Spray,
        Line,
        Rectangle,
        Ellipse,
        Polygon,
        Fill,
        Picker,
        Text
    };

    explicit Canvas(QWidget *parent = nullptr);

    // Document lifecycle.
    void newImage(const QSize &size, const QColor &fill = Qt::white);
    bool openImage(const QString &path);
    bool saveImage(const QString &path);
    // Resize the CURRENT document, keeping the existing drawing at its pixel size
    // (anchored top-left): a larger canvas gains blank space, a smaller one crops.
    // Undoable. Background for new area = the secondary/background colour, so it
    // matches what the eraser and Clear use.
    void resizeCanvas(const QSize &size);

    // Tool / brush state, driven from the toolbar.
    void setTool(Tool tool);       // commits any open text before switching
    // Two colour slots, classic-Paint style: primary is painted with the left
    // mouse button, secondary with the right. (The eraser inverts this -- see
    // mousePressEvent -- because its "ink" is the background/secondary slot.)
    void setPrimaryColor(const QColor &c)   { m_primaryColor = c; }
    void setSecondaryColor(const QColor &c) { m_secondaryColor = c; }
    // "Rainbow" colour slots: when a slot is in rainbow mode, painting with it
    // cycles continuously through the full vivid spectrum (time-based) instead
    // of laying down a flat colour. Modelled as a property of the colour slot,
    // not a separate tool -- any tool that uses that slot paints rainbow.
    // (Provenance: Tux Paint's "Rainbow" tool, not MS Paint -- see DESIGN.md
    // §6b.) Setting a flat colour on a slot clears its rainbow flag.
    void setPrimaryRainbow(bool on)   { m_primaryRainbow = on; }
    void setSecondaryRainbow(bool on) { m_secondaryRainbow = on; }
    void setBrushSize(int px)               { m_brushSize = qMax(1, px); }
    void setFontPointSize(int pt);          // text tool font size
    // Shape fill: when on, the closed shape tools (rectangle, ellipse) fill their
    // interior as well as stroking the border. Classic Paint's "outline + fill"
    // style: the stroke button's colour is the border, the OTHER slot is the fill
    // (so left-drag = Color 1 border / Color 2 fill; right-drag swaps). Has no
    // effect on the line tool, which has no interior.
    void setShapeFill(bool on)              { m_shapeFill = on; update(); }
    bool shapeFill() const                  { return m_shapeFill; }

    Tool    tool()           const { return m_tool; }
    QColor  primaryColor()   const { return m_primaryColor; }
    QColor  secondaryColor() const { return m_secondaryColor; }
    int     brushSize()      const { return m_brushSize; }
    QSize   imageSize()      const { return m_image.size(); }

    // Zoom: the document (m_image) is always stored at its true pixel size;
    // zoom only affects how it is displayed and how screen clicks map back to
    // image pixels. 1.0 == 100%.
    double  zoom() const { return m_zoom; }
    void    setZoom(double factor);
    // Overscroll padding (px) between the widget edge and the image, so callers
    // like the rulers can offset their mapping. See m_pad / canvasOrigin().
    int     scrollPadding() const { return m_pad; }
    // Image pixel (0,0) in this widget's local coords (i.e. the padding offset).
    // The rulers map this through to the viewport to get image-origin's true
    // on-screen position (covers scroll AND the scroll-area centring gap).
    QPoint  canvasOriginPoint() const { return QPoint(m_pad, m_pad); }
    // Recompute layout (overscroll padding depends on the viewport size). Call
    // when the viewport resizes without a zoom/document change.
    void    relayout() { applyZoom(); }
    void    zoomIn()  { setZoom(m_zoom * 1.25); }
    void    zoomOut() { setZoom(m_zoom / 1.25); }
    void    resetZoom() { setZoom(1.0); }

    // Smooth (anti-aliased) vs crisp edges. Off by default: crisp, MS-Paint
    // style, every pixel a solid colour. When on, shapes/strokes are smoothed
    // and the flood fill compensates for the resulting edge ramp.
    bool    antialiasing() const { return m_antialias; }
    void    setAntialiasing(bool on);

    // --- Indexed-palette (sprite) mode -------------------------------------
    // True when the document is a palette-indexed image (e.g. a pokeemerald
    // 4bpp sprite). Editing happens in an ARGB buffer (Qt's QPainter cannot
    // paint on Format_Indexed8 at all), but the colour choices are locked to the
    // file's palette and the original colour table (in slot order) is kept so
    // save can convert back to Format_Indexed8 with the index ORDER intact -- it
    // is the GBA palette-slot order and is semantically load-bearing. Entered
    // automatically when openImage loads an indexed PNG; a plain doodle
    // (newImage, opening a truecolour file) is never indexed. See DESIGN.md §14.
    bool    isIndexed() const { return m_indexed; }
    // The palette in slot order. Empty unless isIndexed(). The UI builds its
    // palette strip from this; the two colour slots reference entries by index.
    QVector<QRgb> palette() const { return m_palette; }
    // Set the active colour slot to palette entry `index`. Returns the resolved
    // QColor so the UI can mirror it in its indicator swatch. Out-of-range or
    // non-indexed -> no-op, returns an invalid QColor.
    QColor  setPrimaryPaletteIndex(int index);
    QColor  setSecondaryPaletteIndex(int index);

    // Pixel-art grid: a semi-transparent overlay drawn on top of the canvas with
    // lines every gridWidth x gridHeight IMAGE pixels (so it scales with zoom).
    // Off by default; purely visual (never baked into the image).
    bool    gridEnabled() const { return m_gridEnabled; }
    void    setGridEnabled(bool on);
    QSize   gridSize() const { return QSize(m_gridW, m_gridH); }
    void    setGridSize(int w, int h);
    // Pixel grid: an additional 1x1-pixel grid in a lighter shade, drawn beneath
    // the user's cell grid. Only sensible when zoomed in enough to see pixels, so
    // it auto-hides at low zoom. Independent toggle from the main grid.
    bool    pixelGridEnabled() const { return m_pixelGrid; }
    void    setPixelGridEnabled(bool on);

    bool isModified() const { return m_modified; }

    bool canUndo() const { return !m_undoStack.isEmpty(); }
    bool canRedo() const { return !m_redoStack.isEmpty(); }

public slots:
    void undo();
    void redo();
    void clear();          // fill the whole canvas with the background colour

signals:
    // Emitted whenever undo/redo availability or the modified flag may have
    // changed, so the MainWindow can refresh its menu/toolbar enabled-states.
    void historyChanged();
    void modifiedChanged(bool modified);
    // Emitted by the Picker tool so the UI can reflect the sampled colour.
    // colorPicked -> primary slot (left-click), secondaryColorPicked -> right.
    void colorPicked(const QColor &color);
    void secondaryColorPicked(const QColor &color);
    // Emitted when the zoom factor changes, so the UI can update its readout.
    void zoomChanged(double factor);
    // Emitted when the document's indexed-mode status changes (e.g. opening an
    // indexed sprite, or opening a plain image afterwards). The UI swaps its
    // colour strip for the palette strip (and the status-bar badge) on this.
    void indexedModeChanged(bool indexed, const QVector<QRgb> &palette);
    // Cursor position over the canvas, in IMAGE-pixel coordinates, for the status
    // bar readout. `cursorLeft` fires when the pointer leaves so the readout can
    // blank. (Position can be outside the image bounds while the pointer is over
    // the margin/zoomed widget; the readout shows it regardless.)
    void cursorPositionChanged(const QPoint &imagePos);
    // Precise (sub-pixel, un-clamped) cursor position for the rulers' markers, so
    // they track smoothly at low zoom where toImage()'s integer truncation lags.
    void cursorPositionChangedF(const QPointF &imagePos);
    void cursorLeft();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;  // closes a polygon
    void wheelEvent(QWheelEvent *event) override;   // Ctrl+wheel == zoom
    void leaveEvent(QEvent *event) override;         // blank the coord readout
    void keyPressEvent(QKeyEvent *event) override;   // Shift -> refresh preview
    void keyReleaseEvent(QKeyEvent *event) override;
    // Watches the text editor for Escape (cancel) -- the editor has focus while
    // typing, so the canvas's own key handler wouldn't see it.
    bool eventFilter(QObject *watched, QEvent *event) override;
    QSize sizeHint() const override { return m_image.size() * m_zoom; }

private:
    // Drawing helpers. All operate directly on m_image.
    void drawLineTo(const QPoint &endPoint);   // freehand pencil/eraser segment
    void commitShape(const QPoint &endPoint);  // finalise a rubber-banded shape
    void paintShape(QPainter &painter, const QPoint &start, const QPoint &end);

    // --- Free-form polygon tool --------------------------------------------
    // Click to drop vertices; a rubber-band runs to the cursor. Double-click or
    // Enter commits (always closed), Backspace removes the last vertex, Escape
    // cancels. Border = the slot of the button that started the path; with the
    // fill toggle on, the interior uses the other slot (like rect/ellipse).
    void paintPolygon(QPainter &painter, const QVector<QPoint> &pts,
                      bool closed) const;     // shared by preview + commit
    void commitPolygon();                     // bake the closed polygon
    void cancelPolygon();                     // discard the in-progress path
    bool polygonActive() const { return m_polyActive; }
    // Apply Shift-constrain to a shape's end point: lines snap to 45deg steps,
    // rectangles become squares, ellipses become circles. Returns end unchanged
    // when `constrain` is false. For lines, `orbit` selects the snap style:
    //   false (default, Shift)      -> project the cursor onto the nearest axis
    //                                  (endpoint tracks the hand)
    //   true  (Shift+Ctrl)          -> keep the dragged length, rotate to 45deg
    //                                  (endpoint orbits the start; radial mode)
    QPoint constrainedEnd(const QPoint &start, const QPoint &end,
                          bool constrain, bool orbit) const;

    // --- Text tool ---------------------------------------------------------
    // An overlaid QLineEdit lets the user type live; the text is baked into the
    // bitmap (commitText) only when they click away, switch tool, or press
    // Enter/Escape. Until then it's an editable overlay, not pixels yet.
    void beginText(const QPoint &imagePos);  // open the editor at an image pixel
    void commitText();                       // bake current editor text -> bitmap
    void cancelText();                       // discard the editor without baking
    void positionTextEditor();               // place/size the editor for zoom
    // Paint `text` at the text origin into `painter` (image coordinates), using
    // the current font/colour. Shared by the live preview and the bake so the
    // two are pixel-identical.
    void drawTextRun(QPainter &painter, const QString &text) const;
    bool textEditing() const { return m_textEdit != nullptr && m_textActive; }

    // The colour a given button paints with, for the current tool. Encapsulates
    // the primary=left / secondary=right rule and the eraser's inversion.
    QColor colorForButton(Qt::MouseButton button) const;
    // True when this tool+button combination is the "color eraser" (right-drag
    // eraser): replace only Color-1 pixels with Color 2 along the stroke.
    bool isColorEraser(Qt::MouseButton button) const;
    // Stamp a color-eraser dab from m_lastPoint to endPoint.
    void colorEraseLineTo(const QPoint &endPoint);
    void floodFill(const QPoint &startPoint, const QColor &newColor);

    // Spray-paint: scatter random dots in a disc around `center`. Density per
    // burst scales with the radius (brush size) so a big spray isn't sparse.
    // Driven by m_sprayTimer while a button is held, so dwelling builds up paint.
    void   spray(const QPoint &center);
    void   onSprayTick();

    // Rainbow helpers. tick advances the hue and (if a rainbow freehand stroke
    // is live) stamps a dab at the current point so a still brush keeps flowing.
    void   onRainbowTick();
    void   updateRainbowTimer();          // start/stop the cycle for this stroke
    QColor currentRainbowColor() const;   // full-vivid hue at m_rainbowHue
    // A diagonal (top-left -> bottom-right) full-spectrum rainbow gradient spanning
    // `box`, used to paint rainbow shapes and fills as a gradient rather than a
    // flat hue. The spectrum is rotated by the live hue so a rubber-banded shape
    // still cycles colour while you drag.
    QLinearGradient rainbowGradient(const QRectF &box) const;

    // Undo plumbing.
    void pushUndoSnapshot();   // call BEFORE mutating m_image
    void setModified(bool m);

    // Map a position in widget (screen) coordinates to image-pixel coordinates,
    // accounting for the current zoom. Single source of truth for every tool.
    QPoint toImage(const QPointF &widgetPos) const;
    // Precise widget->image mapping (sub-pixel, NOT clamped to bounds). Used for
    // the ruler markers; tools/readout use the integer, clamped toImage().
    QPointF toImageF(const QPointF &widgetPos) const;
    void   applyZoom();        // resize the widget to image*zoom and repaint

    // Overscroll padding: the widget is made larger than image*zoom by this many
    // pixels on every side, and the image is drawn offset by it, so the canvas
    // edges/corners can be scrolled away from the viewport border (e.g. out from
    // under the toolbar) and worked on comfortably. Computed from the viewport
    // size in applyZoom(). canvasOrigin() is the image's top-left in widget px.
    int     m_pad = 0;
    QPoint  canvasOrigin() const { return QPoint(m_pad, m_pad); }

    // The QScrollArea that contains this canvas (canvas -> viewport ->
    // scrollarea), or nullptr if unparented. Used by pan and zoom-anchoring.
    class QScrollArea *enclosingScrollArea() const;

    QImage  m_image;           // the document
    Tool    m_tool       = Tool::Pencil;
    QColor  m_primaryColor   = Qt::black;
    QColor  m_secondaryColor = Qt::white;
    QColor  m_strokeColor    = Qt::black;  // colour chosen at press for this stroke
    int     m_brushSize  = 3;

    // Rainbow colour slots (see setPrimaryRainbow). A slot in rainbow mode makes
    // its strokes cycle the hue over time rather than paint a flat colour.
    bool    m_primaryRainbow   = false;
    bool    m_secondaryRainbow = false;
    // True when the stroke currently in progress is being painted in rainbow.
    // Latched at press time from the active button's slot flag.
    bool    m_strokeRainbow = false;
    // The live cycling hue (0..359), advanced by m_rainbowTimer while a rainbow
    // stroke is active so a held-still brush keeps flowing colour (Paint feel).
    int     m_rainbowHue = 0;
    class QTimer *m_rainbowTimer = nullptr;   // drives time-based hue cycling
    // Whether the current button+tool combination paints in rainbow.
    bool rainbowForButton(Qt::MouseButton button) const;

    // Spray tool: a timer stamps a burst at the live cursor while a button is
    // held, so dwelling deposits more paint (classic airbrush feel).
    class QTimer *m_sprayTimer = nullptr;
    QPoint  m_sprayPos;                        // current cursor (image px)

    // Shape fill (see setShapeFill). When on, rect/ellipse (and the closed
    // polygon) fill their interior.
    bool    m_shapeFill = false;

    // Free-form polygon tool state. m_polyActive is true between the first click
    // and commit/cancel; m_polyPoints are the placed vertices; m_polyCursor is
    // the live cursor for the rubber-band segment; m_polyButton is the button
    // that started the path (decides border slot, and fill slot via fillButtonFor).
    bool    m_polyActive = false;
    QVector<QPoint> m_polyPoints;
    QPoint  m_polyCursor;
    Qt::MouseButton m_polyButton = Qt::NoButton;
    // The button opposite the one that started the current shape -- its colour
    // slot supplies the fill (border = stroke button, fill = the other slot).
    Qt::MouseButton fillButtonFor(Qt::MouseButton borderButton) const;

    bool    m_drawing    = false;
    QPoint  m_lastPoint;       // last mouse position during a freehand stroke

    // Live button tracking for freehand strokes (pencil/eraser). A stroke
    // persists while EITHER drawing button is held, and its colour follows the
    // most-recently-pressed button that is still down -- so you can switch
    // primary<->secondary mid-stroke by pressing/releasing the other button.
    // m_activeButton is that current button; m_heldButtons is the set down now.
    Qt::MouseButton  m_activeButton = Qt::NoButton;
    Qt::MouseButtons m_heldButtons  = Qt::NoButton;
    // The single-press shape tools still latch their button at press time:
    Qt::MouseButton  m_shapeButton  = Qt::NoButton;

    // Middle-button panning: drag the canvas to scroll the view. Tracked in
    // GLOBAL screen coordinates because we adjust the enclosing scroll area's
    // scrollbars, which is independent of zoom and of the widget's own origin.
    bool    m_panning    = false;
    QPoint  m_panLastGlobal;
    QPoint  m_shapeStart;      // press point for a rubber-banded shape
    QPoint  m_shapeEnd;        // current mouse point during a rubber-banded shape

    bool    m_modified   = false;
    double  m_zoom       = 1.0;
    bool    m_antialias  = false;   // crisp edges by default (MS-Paint style)

    // --- Indexed-palette (sprite) mode state -------------------------------
    // m_indexed: the document is palette-locked. m_palette: the colour table in
    // slot order (a copy of m_image.colorTable(), kept so the UI doesn't have to
    // reach into the image). m_primaryIndex/m_secondaryIndex: which palette slot
    // each colour button currently paints with (so we can keep the painted
    // colour an EXACT palette entry, and so the picker can report the index).
    // The path the file was opened from + its original PNG bytes are kept so we
    // can splice the tRNS chunk back onto Qt's save output (Qt drops it).
    bool          m_indexed = false;
    QVector<QRgb> m_palette;
    int           m_primaryIndex   = -1;
    int           m_secondaryIndex = -1;
    QByteArray    m_srcTrnsChunk;   // original PNG's tRNS chunk bytes, or empty
    // Load an indexed image without converting it, capture its palette, and set
    // up the two colour slots. Returns false if `loaded` isn't actually indexed.
    bool enterIndexed(const QImage &loaded, const QString &path);
    void leaveIndexed();            // drop indexed state (opening a plain image)
    // Resolve the colour for a palette slot, clamped/guarded. Used by the
    // setter slots and to keep m_primaryColor/m_secondaryColor in sync.
    QColor paletteColor(int index) const;
    // Splice m_srcTrnsChunk back into the PNG just written at `path` (Qt drops
    // tRNS on save). No-op unless the file is a PNG missing a tRNS.
    void reattachTrns(const QString &path);

    // Pixel-art grid overlay (off by default). Cell size in image pixels.
    bool    m_gridEnabled = false;
    int     m_gridW = 16;
    int     m_gridH = 16;
    bool    m_pixelGrid = false;   // additional 1x1 grid (lighter), off by default

    // Text tool state.
    class QLineEdit *m_textEdit = nullptr;   // invisible input model (lazy)
    class QTimer    *m_caretBlink = nullptr; // drives our own caret blink
    bool    m_textActive  = false;           // editor currently open?
    bool    m_caretOn     = true;            // caret blink phase
    QPoint  m_textOrigin;                    // top-left of the text, in image px
    int     m_fontPointSize = 16;

    static constexpr int kMaxHistory = 32;
    QVector<QImage> m_undoStack;
    QVector<QImage> m_redoStack;
};
