// Daint: a simple MS-Paint-style raster paint program.
// Copyright (C) 2026  eagre.dev
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. This program is distributed WITHOUT ANY WARRANTY; see the GNU General
// Public License (LICENSE file) for details.

#include "canvas.h"

#include <QPainter>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QGuiApplication>
#include <QStack>
#include <QtMath>
#include <QScrollArea>
#include <QScrollBar>
#include <QLineEdit>
#include <QFont>
#include <QFontMetrics>
#include <QEvent>
#include <QTimer>
#include <QRandomGenerator>
#include <QPolygonF>
#include <QFile>
#include <cmath>
#include <cstring>

namespace {

// --- PNG chunk surgery, just enough to round-trip a tRNS chunk ---------------
// A PNG is an 8-byte signature followed by chunks laid out as
//   [4-byte big-endian length][4-byte type][length bytes of data][4-byte CRC].
// We never decode pixels here; we only locate/copy whole chunks by type. Qt
// writes a valid PNG but omits tRNS (per-index alpha), so for a faithful
// round-trip of an indexed sprite we lift the original file's tRNS chunk and
// splice it back into Qt's output, just before the first IDAT (the spec
// requires tRNS to appear after PLTE and before IDAT).

const char kPngSig[8] = { '\x89', 'P', 'N', 'G', '\r', '\n', '\x1a', '\n' };

bool looksLikePng(const QByteArray &b)
{
    return b.size() >= 8 && std::memcmp(b.constData(), kPngSig, 8) == 0;
}

// Find the byte offset of the chunk with `type` (e.g. "tRNS"), or -1. On success
// also reports the chunk's total length (12 + data length) via `outTotalLen`.
int findChunk(const QByteArray &png, const char *type, int *outTotalLen = nullptr)
{
    int pos = 8;                                   // skip signature
    while (pos + 8 <= png.size()) {
        const quint32 dataLen =
            (quint8(png[pos])     << 24) | (quint8(png[pos + 1]) << 16) |
            (quint8(png[pos + 2]) << 8)  |  quint8(png[pos + 3]);
        const int total = 12 + int(dataLen);       // len(4) + type(4) + data + crc(4)
        if (pos + total > png.size())
            break;                                 // truncated/corrupt; bail
        if (std::memcmp(png.constData() + pos + 4, type, 4) == 0) {
            if (outTotalLen) *outTotalLen = total;
            return pos;
        }
        pos += total;
    }
    return -1;
}

// Read the whole tRNS chunk (length+type+data+crc) from a PNG file, or an empty
// array if the file has none / isn't a readable PNG.
QByteArray readTrnsChunk(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QByteArray png = f.readAll();
    if (!looksLikePng(png))
        return {};
    int len = 0;
    const int at = findChunk(png, "tRNS", &len);
    if (at < 0)
        return {};
    return png.mid(at, len);
}

} // namespace

// Splice m_srcTrnsChunk into the PNG at `path` (which Qt just wrote), placing it
// immediately before the first IDAT. No-op if the file isn't a PNG, already has
// a tRNS, or has no IDAT (all of which mean "nothing safe to do").
void Canvas::reattachTrns(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return;
    QByteArray png = f.readAll();
    f.close();
    if (!looksLikePng(png) || findChunk(png, "tRNS") >= 0)
        return;                                    // not PNG, or already present
    const int idat = findChunk(png, "IDAT");
    if (idat < 0)
        return;

    png.insert(idat, m_srcTrnsChunk);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(png);
}

Canvas::Canvas(QWidget *parent)
    : QWidget(parent)
{
    // We paint the whole widget ourselves every frame, so tell Qt not to
    // bother clearing the background -- avoids flicker.
    setAttribute(Qt::WA_StaticContents);
    // Mouse tracking on so we receive move events even with no button held --
    // needed for the status-bar coordinate readout and the polygon rubber-band.
    setMouseTracking(true);
    // Accept keyboard focus so we receive Shift key events for shape-constrain.
    setFocusPolicy(Qt::StrongFocus);

    // Drives the rainbow colour cycle. It runs only while a rainbow stroke is
    // in progress; each tick nudges the hue and, for freehand strokes, lays down
    // a dab at the current point -- so holding the brush still keeps the colour
    // flowing, the way classic Paint's rainbow felt.
    m_rainbowTimer = new QTimer(this);
    m_rainbowTimer->setInterval(33);   // ~30 fps; full spectrum in a couple sec
    connect(m_rainbowTimer, &QTimer::timeout, this, &Canvas::onRainbowTick);

    // Spray bursts while a button is held. Faster than the eye so dwelling builds
    // density smoothly; each tick scatters a small burst at the live cursor.
    m_sprayTimer = new QTimer(this);
    m_sprayTimer->setInterval(40);     // ~25 bursts/sec
    connect(m_sprayTimer, &QTimer::timeout, this, &Canvas::onSprayTick);

    newImage(QSize(800, 600));
}

// ---------------------------------------------------------------------------
// Document lifecycle
// ---------------------------------------------------------------------------

void Canvas::newImage(const QSize &size, const QColor &fill)
{
    leaveIndexed();        // a fresh doodle is never palette-locked
    m_image = QImage(size, QImage::Format_ARGB32_Premultiplied);
    m_image.fill(fill);

    m_undoStack.clear();
    m_redoStack.clear();
    setModified(false);
    emit historyChanged();

    applyZoom();          // widget size = image size * zoom
}

void Canvas::resizeCanvas(const QSize &size)
{
    if (!size.isValid() || size.isEmpty() || size == m_image.size())
        return;

    // Commit any in-progress edits so we don't resize out from under them.
    if (m_textActive)
        commitText();
    if (m_polyActive)
        cancelPolygon();

    pushUndoSnapshot();   // undoable, like any other document mutation

    // New canvas filled with the background (secondary) colour, then the old
    // drawing painted back top-left. Larger -> blank margin on the right/bottom;
    // smaller -> the old image is clipped to the new bounds automatically.
    QImage resized(size, QImage::Format_ARGB32_Premultiplied);
    resized.fill(m_secondaryColor);
    {
        QPainter p(&resized);
        p.drawImage(0, 0, m_image);   // anchored top-left; clips if smaller
    }
    m_image = resized;

    setModified(true);
    applyZoom();          // widget tracks the new image size
    update();
}

bool Canvas::openImage(const QString &path)
{
    QImage loaded;
    if (!loaded.load(path))
        return false;

    // Indexed sprites (e.g. pokeemerald 4bpp PNGs) need their palette and its
    // slot order preserved or the file is corrupted on save. Qt loads a 4bpp
    // indexed PNG as Format_Indexed8 with the colour table intact; enterIndexed
    // captures that palette and switches the canvas into palette-locked mode
    // (editing happens in ARGB, see enterIndexed). A plain doodle never hits
    // this branch. (DESIGN.md §14.)
    if (loaded.format() == QImage::Format_Indexed8 && !loaded.colorTable().isEmpty()) {
        if (enterIndexed(loaded, path)) {
            m_undoStack.clear();
            m_redoStack.clear();
            setModified(false);
            emit historyChanged();
            applyZoom();
            return true;
        }
        // enterIndexed declined (shouldn't happen given the guard); fall through
        // to the normal truecolour path rather than failing the open.
    }

    leaveIndexed();
    m_image = loaded.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    m_undoStack.clear();
    m_redoStack.clear();
    setModified(false);
    emit historyChanged();

    applyZoom();
    return true;
}

bool Canvas::saveImage(const QString &path)
{
    if (m_indexed) {
        // Convert the ARGB editing buffer back to Format_Indexed8 using the
        // original palette in its original order, so every painted pixel resolves
        // to its true GBA palette slot and the colour table is byte-preserved.
        // Threshold (no dither) because the palette is locked: a painted pixel is
        // already an exact palette colour, and dithering would scatter wrong
        // indices. This converted image is what gbagfx re-packs to 4bpp.
        // (DESIGN.md §14, de-risk note 1.)
        QImage out = m_image.convertToFormat(QImage::Format_Indexed8, m_palette,
                                             Qt::ThresholdDither | Qt::ThresholdAlphaDither);
        if (!out.save(path))
            return false;
    } else if (!m_image.save(path)) {
        return false;
    }

    // Qt drops the PNG tRNS (per-index alpha) chunk on save. For the pokeemerald
    // pipeline that's harmless (transparency is the index-0 palette convention,
    // not the PNG tRNS), and gbagfx re-derives 4bpp packing from the palette, so
    // the indices round-trip byte-identically. But to keep a faithful PNG for
    // any other consumer we splice the original file's tRNS chunk back in.
    // (DESIGN.md §14, de-risk note 2.)
    if (m_indexed && !m_srcTrnsChunk.isEmpty() && path.endsWith(".png", Qt::CaseInsensitive))
        reattachTrns(path);

    setModified(false);
    return true;
}

// ---------------------------------------------------------------------------
// Indexed-palette (sprite) mode -- see DESIGN.md §14
// ---------------------------------------------------------------------------

bool Canvas::enterIndexed(const QImage &loaded, const QString &path)
{
    if (loaded.colorTable().isEmpty())
        return false;

    // Capture the palette in slot order BEFORE converting -- the order is the
    // GBA palette-slot mapping and must survive untouched to the save. We then
    // edit in ARGB, not Indexed8: Qt's QPainter cannot paint on a Format_Indexed8
    // image at all (verified -- "Cannot paint on an image with the
    // Format_Indexed8 format"), so every brush/shape tool would be dead. Instead
    // we constrain the *colours* to this locked palette while editing in ARGB,
    // then convert back to Indexed8 with this exact table at save time. That
    // round-trip is gbagfx-byte-identical (DESIGN.md §14, de-risk note).
    //
    // We force the editing palette OPAQUE (alpha stripped from every entry).
    // pokeemerald's transparent slot (conventionally index 0) carries alpha 0 in
    // the colour table; left as-is it would (a) make that slot's pixels invisible
    // and unpaintable in the ARGB buffer, and (b) be ambiguous to match back on
    // save. By editing with opaque colours, slot 0 is a normal visible, paintable
    // colour like any other; the real transparency is reconstructed from the
    // original tRNS chunk we re-attach on save, not from the buffer's alpha. (The
    // opaque round-trip is verified byte-identical through gbagfx.)
    const QVector<QRgb> raw = loaded.colorTable();   // slot order preserved
    m_palette.clear();
    m_palette.reserve(raw.size());
    for (QRgb c : raw)
        m_palette.push_back(qRgb(qRed(c), qGreen(c), qBlue(c)));   // force opaque

    // Convert to ARGB and repaint every pixel as its OPAQUE palette colour, so
    // formerly-transparent (slot-0) pixels become visible green rather than blank
    // canvas -- the user paints sprites against the real background colour, the
    // way the GBA shows it, and nothing is invisible.
    m_image = loaded.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < m_image.height(); ++y) {
        for (int x = 0; x < m_image.width(); ++x) {
            const int idx = loaded.pixelIndex(x, y);
            if (idx >= 0 && idx < m_palette.size())
                m_image.setPixel(x, y, m_palette.at(idx) | 0xff000000u);
        }
    }
    m_indexed = true;
    m_srcTrnsChunk = readTrnsChunk(path);

    // Anti-aliasing invents in-between colours that aren't palette entries, which
    // would write garbage indices. Lock it off while indexed (it's already the
    // crisp default, but a session may have turned it on).
    m_antialias = false;

    // Seed the two colour slots: Color 1 = first non-transparent entry (index 0
    // is the conventional transparent/background slot in GBA sprites), Color 2 =
    // index 0 itself, so right-click erases to "background" as Paint users expect.
    const int n = m_palette.size();
    m_secondaryIndex = 0;
    m_primaryIndex   = (n > 1) ? 1 : 0;
    m_primaryColor   = paletteColor(m_primaryIndex);
    m_secondaryColor = paletteColor(m_secondaryIndex);
    // Rainbow is meaningless against a locked palette; clear any latched flag.
    m_primaryRainbow = m_secondaryRainbow = false;

    emit indexedModeChanged(true, m_palette);
    return true;
}

void Canvas::leaveIndexed()
{
    if (!m_indexed) {
        m_srcTrnsChunk.clear();
        return;
    }
    m_indexed = false;
    m_palette.clear();
    m_primaryIndex = m_secondaryIndex = -1;
    m_srcTrnsChunk.clear();
    emit indexedModeChanged(false, {});
}

QColor Canvas::paletteColor(int index) const
{
    if (index < 0 || index >= m_palette.size())
        return QColor();
    return QColor::fromRgba(m_palette.at(index));
}

QColor Canvas::setPrimaryPaletteIndex(int index)
{
    if (!m_indexed || index < 0 || index >= m_palette.size())
        return QColor();
    m_primaryIndex = index;
    m_primaryColor = paletteColor(index);
    m_primaryRainbow = false;
    return m_primaryColor;
}

QColor Canvas::setSecondaryPaletteIndex(int index)
{
    if (!m_indexed || index < 0 || index >= m_palette.size())
        return QColor();
    m_secondaryIndex = index;
    m_secondaryColor = paletteColor(index);
    m_secondaryRainbow = false;
    return m_secondaryColor;
}

// ---------------------------------------------------------------------------
// Painting the widget
// ---------------------------------------------------------------------------

void Canvas::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);

    // Shift the whole drawing by the overscroll padding so the image sits inset
    // from the widget's (0,0). Done once here: every coordinate below is in
    // image*zoom space measured from the image's top-left, so the translate keeps
    // the image, previews, grid, etc. all consistent without per-call offsets.
    painter.translate(canvasOrigin());

    // Magnify the document by the zoom factor. Nearest-neighbour (smooth
    // transform OFF) gives sharp, square pixels when zoomed in -- the
    // MS-Paint / pixel-art look, and what makes per-pixel inspection possible.
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    const QRectF dest(0, 0, m_image.width() * m_zoom, m_image.height() * m_zoom);
    painter.drawImage(dest, m_image, QRectF(m_image.rect()));

    // While a shape tool is being dragged, draw a live preview ON TOP of the
    // committed image, without touching m_image itself. The shape is baked in
    // only on mouse-release (commitShape).
    //
    // The preview MUST be rendered through the same pipeline as the final
    // bitmap, or it lies at high zoom: drawing the shape directly onto the
    // (zoom-scaled) widget rasterises its edges against SCREEN pixels -- thin
    // and smooth -- whereas the committed shape is drawn into m_image at 1:1
    // and then magnified into chunky canvas-pixels. So we do the same here:
    // render the shape into a canvas-resolution overlay at 1:1, then blow that
    // overlay up with nearest-neighbour exactly like m_image above. Now the
    // preview is literally the pixels you'll get.
    if (m_drawing &&
        (m_tool == Tool::Line || m_tool == Tool::Rectangle || m_tool == Tool::Ellipse)) {
        QImage overlay(m_image.size(), QImage::Format_ARGB32_Premultiplied);
        overlay.fill(Qt::transparent);

        QPainter op(&overlay);
        op.setRenderHint(QPainter::Antialiasing, m_antialias);
        // For a rainbow shape the preview tracks the live hue, so the rubber-band
        // visibly cycles colour the way the committed shape will.
        const QColor previewColor = m_strokeRainbow ? currentRainbowColor()
                                                    : m_strokeColor;
        // MiterJoin to match commitShape -- sharp rectangle corners at any width.
        QPen pen(previewColor, m_brushSize, Qt::SolidLine, Qt::RoundCap, Qt::MiterJoin);
        op.setPen(pen);
        paintShape(op, m_shapeStart, m_shapeEnd);   // identical call to commitShape
        op.end();

        painter.drawImage(dest, overlay, QRectF(overlay.rect()));
    }

    // Live polygon preview: the placed vertices plus a rubber-band segment to the
    // cursor, rendered through the same overlay+pipeline as shapes so the stroke
    // matches the bake. We draw it OPEN (the chain so far + the live segment) and
    // add a thin dashed "will close here" hint from the cursor back to the start,
    // plus small handles on the placed vertices for clarity.
    if (m_tool == Tool::Polygon && m_polyActive && !m_polyPoints.isEmpty()) {
        QVector<QPoint> chain = m_polyPoints;
        chain.append(m_polyCursor);              // rubber-band to the cursor

        QImage overlay(m_image.size(), QImage::Format_ARGB32_Premultiplied);
        overlay.fill(Qt::transparent);
        {
            QPainter op(&overlay);
            op.setRenderHint(QPainter::Antialiasing, m_antialias);
            paintPolygon(op, chain, /*closed*/ false);
        }
        painter.drawImage(dest, overlay, QRectF(overlay.rect()));

        // Closing hint + vertex handles drawn directly on the widget (screen px),
        // so they stay 1px crisp regardless of zoom -- they're UI, not paint.
        if (m_polyPoints.size() >= 2) {
            QPen dash(QColor(0, 0, 0, 120), 1, Qt::DashLine);
            painter.setPen(dash);
            painter.drawLine(QPointF(m_polyCursor.x() * m_zoom, m_polyCursor.y() * m_zoom),
                             QPointF(m_polyPoints.first().x() * m_zoom,
                                     m_polyPoints.first().y() * m_zoom));
        }
        painter.setPen(QPen(QColor(0, 0, 0, 180), 1));
        painter.setBrush(QColor(255, 255, 255, 200));
        for (const QPoint &v : m_polyPoints) {
            const QPointF c(v.x() * m_zoom, v.y() * m_zoom);
            painter.drawRect(QRectF(c.x() - 2, c.y() - 2, 4, 4));
        }
        painter.setBrush(Qt::NoBrush);
    }

    // Live text preview: same overlay-through-the-pipeline trick. We paint the
    // editor's current text via drawTextRun -- the exact call commitText uses --
    // so the preview is pixel-identical to the bake at any zoom. We also paint
    // our OWN caret (the editor is parked off-screen), computed from the same
    // QFontMetrics + origin as the glyphs, so it can never drift from the text
    // however long it gets or however the zoom changes.
    if (m_textActive && m_textEdit) {
        QImage overlay(m_image.size(), QImage::Format_ARGB32_Premultiplied);
        overlay.fill(Qt::transparent);
        {
            QPainter op(&overlay);
            drawTextRun(op, m_textEdit->text());
        }
        painter.drawImage(dest, overlay, QRectF(overlay.rect()));

        if (m_caretOn) {
            QFont f = font();
            f.setPointSize(m_fontPointSize);
            const QFontMetrics fm(f);
            const QString upTo = m_textEdit->text().left(m_textEdit->cursorPosition());
            const int caretX = m_textOrigin.x() + fm.horizontalAdvance(upTo);
            const int top    = m_textOrigin.y();
            painter.setPen(m_primaryColor);
            painter.drawLine(QPointF(caretX * m_zoom, top * m_zoom),
                             QPointF(caretX * m_zoom, (top + fm.height()) * m_zoom));
        }
    }

    // Grid overlays, drawn on top of everything as a guide (never baked in). Two
    // independent layers: a fine 1x1 PIXEL grid (lighter, underneath) and the
    // user's CELL grid (subtle, on top). Each is skipped when its lines would be
    // too dense to read -- at low zoom a grid is just a smear that hurts.
    const double imgW = m_image.width()  * m_zoom;
    const double imgH = m_image.height() * m_zoom;

    // Fine 1x1 pixel grid first, so the bolder cell grid sits over it. Needs a
    // higher zoom to be useful (one image pixel must be several screen px).
    if (m_pixelGrid && m_zoom >= 6.0) {
        painter.setPen(QPen(QColor(0, 0, 0, 30), 1));   // lighter than cell grid
        for (double x = m_zoom; x < imgW; x += m_zoom)
            painter.drawLine(QPointF(x, 0), QPointF(x, imgH));
        for (double y = m_zoom; y < imgH; y += m_zoom)
            painter.drawLine(QPointF(0, y), QPointF(imgW, y));
    }

    if (m_gridEnabled) {
        const double cellW = m_gridW * m_zoom;
        const double cellH = m_gridH * m_zoom;
        if (cellW >= 4.0 && cellH >= 4.0) {
            painter.setPen(QPen(QColor(0, 0, 0, 70), 1));   // subtle, semi-transp
            for (double x = cellW; x < imgW; x += cellW)
                painter.drawLine(QPointF(x, 0), QPointF(x, imgH));
            for (double y = cellH; y < imgH; y += cellH)
                painter.drawLine(QPointF(0, y), QPointF(imgW, y));
        }
    }
}

// ---------------------------------------------------------------------------
// Mouse handling -- this is the dispatch point for every tool
// ---------------------------------------------------------------------------

void Canvas::mousePressEvent(QMouseEvent *event)
{
    // Middle button starts a pan, regardless of the active tool.
    if (event->button() == Qt::MiddleButton) {
        m_panning = true;
        m_panLastGlobal = event->globalPosition().toPoint();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }

    const Qt::MouseButton btn = event->button();
    const bool left  = (btn == Qt::LeftButton);
    const bool right = (btn == Qt::RightButton);
    if (!left && !right)
        return;

    m_heldButtons |= btn;
    const QPoint pos = toImage(event->position());

    switch (m_tool) {
    case Tool::Pencil:
    case Tool::Eraser:
        if (m_drawing) {
            // A stroke is already running and the OTHER button was just pressed:
            // switch the active button live, no new snapshot, no break in line.
            m_activeButton = btn;       // most-recent press wins
            m_strokeRainbow = rainbowForButton(btn);
            m_strokeColor  = colorForButton(btn);
        } else {
            pushUndoSnapshot();         // one snapshot at the start of the stroke
            m_drawing = true;
            m_activeButton = btn;
            m_strokeRainbow = rainbowForButton(btn);
            m_strokeColor  = colorForButton(btn);
            m_lastPoint = pos;
        }
        updateRainbowTimer();           // run the cycle iff this stroke is rainbow
        // Initial dab (so a click with no drag still marks). Routes to the
        // selective color-eraser when that's the active button+tool.
        if (isColorEraser(m_activeButton))
            colorEraseLineTo(pos);
        else
            drawLineTo(pos);
        break;

    case Tool::Spray:
        // Shares the freehand button model (snapshot once, handover while either
        // button is held), but a timer does the painting at the live cursor.
        if (m_drawing) {
            m_activeButton = btn;
            m_strokeRainbow = rainbowForButton(btn);
        } else {
            pushUndoSnapshot();         // one snapshot for the whole spray session
            m_drawing = true;
            m_activeButton = btn;
            m_strokeRainbow = rainbowForButton(btn);
        }
        m_sprayPos = pos;
        updateRainbowTimer();           // rainbow spray cycles hue concurrently
        if (!m_sprayTimer->isActive())
            m_sprayTimer->start();
        spray(pos);                     // immediate first burst on click
        break;

    case Tool::Line:
    case Tool::Rectangle:
    case Tool::Ellipse:
        // Shape tools latch their button for the whole rubber-band; ignore a
        // second button press while one is in progress.
        if (!m_drawing) {
            m_drawing = true;
            m_shapeButton = btn;
            m_strokeRainbow = rainbowForButton(btn);
            m_strokeColor = colorForButton(btn);
            m_shapeStart = pos;
            m_shapeEnd = pos;
            updateRainbowTimer();   // cycle the preview while rubber-banding
        }
        break;

    case Tool::Polygon:
        // Click-to-place vertices. The FIRST click starts the path and latches
        // the button (which decides the border/fill slots); later clicks append.
        // A second button press while a path is active is ignored (the path owns
        // the gesture). Double-click closes it (mouseDoubleClickEvent).
        if (!m_polyActive) {
            m_polyActive = true;
            m_polyButton = btn;
            m_polyPoints.clear();
            m_polyPoints.append(pos);
            m_polyCursor = pos;
            setFocus();               // so Enter/Esc/Backspace reach keyPressEvent
        } else if (btn == m_polyButton) {
            m_polyPoints.append(pos);
        }
        update();
        break;

    case Tool::Fill:
        pushUndoSnapshot();
        // A rainbow fill lays a gradient across the filled region instead of a
        // flat colour; floodFill reads m_strokeRainbow to decide. The flat hue we
        // pass is still used to drive flood matching/termination.
        m_strokeRainbow = rainbowForButton(btn);
        floodFill(pos, colorForButton(btn));
        m_strokeRainbow = false;   // fill is instantaneous; don't leave it latched
        break;

    case Tool::Picker: {
        // Left-click samples into the primary slot, right-click into secondary.
        if (m_image.rect().contains(pos)) {
            const QColor sampled = m_image.pixelColor(pos);
            if (left) emit colorPicked(sampled);
            else      emit secondaryColorPicked(sampled);
        }
        break;
    }

    case Tool::Text:
        // Left-click places a text box. If one is already open, clicking
        // elsewhere bakes it first (commit-on-click-away), then a new one opens.
        if (left) {
            if (m_textActive)
                commitText();
            beginText(pos);
        }
        break;
    }
}

void Canvas::mouseMoveEvent(QMouseEvent *event)
{
    // Panning: translate mouse movement (in screen pixels) into scrollbar
    // movement. Moving the mouse right scrolls content left so the canvas
    // appears to follow the cursor -- the standard grab-and-drag feel.
    if (m_panning) {
        if (auto *area = enclosingScrollArea()) {
            const QPoint now = event->globalPosition().toPoint();
            const QPoint delta = now - m_panLastGlobal;
            m_panLastGlobal = now;
            area->horizontalScrollBar()->setValue(
                area->horizontalScrollBar()->value() - delta.x());
            area->verticalScrollBar()->setValue(
                area->verticalScrollBar()->value() - delta.y());
        }
        event->accept();
        return;
    }

    // Status-bar readout: report the cursor in image-pixel coords on every move
    // (hover or drag). Emitted before the tool-specific handling below, which can
    // early-return, so the readout always updates. The integer form drives the
    // status bar; the precise float form drives the ruler markers (smooth at low
    // zoom, where the integer would visibly lag the pointer).
    emit cursorPositionChanged(toImage(event->position()));
    emit cursorPositionChangedF(toImageF(event->position()));

    // Polygon rubber-band: the path is placed by clicks, not a held drag, so we
    // track the cursor here (mouse-tracking is on while a path is active) to draw
    // the live segment from the last vertex to the cursor.
    if (m_tool == Tool::Polygon && m_polyActive) {
        m_polyCursor = toImage(event->position());
        update();
        return;
    }

    if (!m_drawing)
        return;

    const QPoint pos = toImage(event->position());

    if (m_tool == Tool::Pencil || m_tool == Tool::Eraser) {
        if (isColorEraser(m_activeButton))
            colorEraseLineTo(pos);   // right-drag eraser: selective replace
        else
            drawLineTo(pos);
    } else if (m_tool == Tool::Spray) {
        m_sprayPos = pos;            // the timer sprays here; dragging moves it
    } else if (m_tool == Tool::Line || m_tool == Tool::Rectangle || m_tool == Tool::Ellipse) {
        m_shapeEnd = pos;
        update();   // repaint to show the rubber-band preview
    }
}

void Canvas::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::MiddleButton && m_panning) {
        m_panning = false;
        unsetCursor();
        event->accept();
        return;
    }

    const Qt::MouseButton btn = event->button();
    if ((btn != Qt::LeftButton && btn != Qt::RightButton) || !m_drawing) {
        m_heldButtons &= ~btn;
        return;
    }

    m_heldButtons &= ~btn;
    const QPoint pos = toImage(event->position());

    // Freehand: a stroke lives while EITHER drawing button is held. Releasing
    // one button while the other is still down doesn't end the stroke -- it
    // hands the active colour over to the remaining button, seamlessly. Spray
    // rides the same model (its timer keeps bursting until fully released).
    if (m_tool == Tool::Pencil || m_tool == Tool::Eraser || m_tool == Tool::Spray) {
        const Qt::MouseButtons stillHeld =
            m_heldButtons & (Qt::LeftButton | Qt::RightButton);
        if (stillHeld != Qt::NoButton) {
            // Pick the remaining held button as the new active one.
            m_activeButton = (stillHeld & Qt::LeftButton) ? Qt::LeftButton
                                                          : Qt::RightButton;
            m_strokeRainbow = rainbowForButton(m_activeButton);
            m_strokeColor = colorForButton(m_activeButton);
            updateRainbowTimer();   // handover may flip rainbow on/off
            return;   // stroke continues; do NOT clear m_drawing
        }
        // No drawing button left -> end the stroke.
        m_drawing = false;
        m_activeButton = Qt::NoButton;
        if (m_sprayTimer->isActive())
            m_sprayTimer->stop();   // stop spraying
        updateRainbowTimer();       // stops the cycle (no stroke live now)
        update();
        return;
    }

    // Shape tools: only the latched button ends the rubber-band.
    if (btn != m_shapeButton)
        return;

    if (m_tool == Tool::Line || m_tool == Tool::Rectangle || m_tool == Tool::Ellipse) {
        pushUndoSnapshot();      // snapshot before baking the shape in
        commitShape(pos);
    }

    m_drawing = false;
    m_shapeButton = Qt::NoButton;
    updateRainbowTimer();        // stops the cycle (rubber-band finished)
    update();
}

void Canvas::mouseDoubleClickEvent(QMouseEvent *event)
{
    // Double-click closes and commits an in-progress polygon. (Qt has already
    // delivered the first click's press, which placed a vertex at this spot, so
    // the path is complete; any coincident final point is harmless when closed.)
    if (m_tool == Tool::Polygon && m_polyActive &&
        (event->button() == m_polyButton)) {
        commitPolygon();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

// ---------------------------------------------------------------------------
// Drawing primitives
// ---------------------------------------------------------------------------

// Left button paints the primary slot, right paints the secondary -- except the
// eraser, whose "ink" is the background (secondary) slot, so it inverts: left
// erases to secondary, and right is the selective color-eraser (handled
// separately) rather than a plain stroke.
QColor Canvas::colorForButton(Qt::MouseButton button) const
{
    // A rainbow slot resolves to the live cycling hue rather than a flat colour.
    // (The eraser ignores rainbow -- its "ink" is the background; a rainbow
    // eraser would be meaningless.)
    if (m_tool != Tool::Eraser && rainbowForButton(button))
        return currentRainbowColor();

    const bool left = (button == Qt::LeftButton);
    if (m_tool == Tool::Eraser)
        return left ? m_secondaryColor : m_primaryColor;
    return left ? m_primaryColor : m_secondaryColor;
}

// Whether the slot this button paints with is in rainbow mode. Mirrors the
// left=primary / right=secondary mapping of colorForButton (no eraser
// inversion -- rainbow doesn't apply to the eraser).
bool Canvas::rainbowForButton(Qt::MouseButton button) const
{
    return (button == Qt::LeftButton) ? m_primaryRainbow : m_secondaryRainbow;
}

// For shape fill, the interior uses the slot OPPOSITE the border (stroke) button:
// left-drag -> Color 1 border, Color 2 fill; right-drag swaps. So the fill colour
// is just colorForButton() of the other button.
Qt::MouseButton Canvas::fillButtonFor(Qt::MouseButton borderButton) const
{
    return (borderButton == Qt::LeftButton) ? Qt::RightButton : Qt::LeftButton;
}

// Scatter one burst of single-pixel dots in a disc around `center`. We sample
// points uniformly over the disc (sqrt on the radius avoids clustering at the
// centre), so the spray reads as an even circular cloud. Dot count scales with
// the radius's area so a wide spray isn't sparse; a tight one isn't a blob.
// Colour comes from colorForButton(), so the two-colour system and rainbow
// (the hue is advanced by the rainbow timer) work here for free.
void Canvas::spray(const QPoint &center)
{
    const double radius = m_brushSize;   // brush size doubles as spray radius
    // ~12% of the disc's pixels per burst -- dense enough to feel like paint,
    // sparse enough that dwelling visibly builds up. Clamp so tiny/huge radii
    // still behave.
    const int dots = qBound(3, int(M_PI * radius * radius * 0.12), 400);

    const QColor color = colorForButton(m_activeButton);
    QPainter painter(&m_image);
    painter.setRenderHint(QPainter::Antialiasing, false);   // crisp speckle
    painter.setPen(color);

    auto *rng = QRandomGenerator::global();
    for (int i = 0; i < dots; ++i) {
        const double ang = rng->generateDouble() * 2.0 * M_PI;
        const double r   = std::sqrt(rng->generateDouble()) * radius;
        const int x = center.x() + int(std::round(std::cos(ang) * r));
        const int y = center.y() + int(std::round(std::sin(ang) * r));
        if (m_image.rect().contains(x, y))
            painter.drawPoint(x, y);
    }
    painter.end();

    setModified(true);
    update();
}

// One spray burst at the live cursor; the timer fires this while a button is
// held, so dwelling deposits more paint.
void Canvas::onSprayTick()
{
    if (m_drawing && m_tool == Tool::Spray)
        spray(m_sprayPos);
}

// The current point on the rainbow: full saturation and value for the classic
// vivid crayon spectrum.
QColor Canvas::currentRainbowColor() const
{
    return QColor::fromHsv(m_rainbowHue % 360, 255, 255);
}

QLinearGradient Canvas::rainbowGradient(const QRectF &box) const
{
    // Diagonal: top-left -> bottom-right of the shape/region's bounding box.
    QLinearGradient g(box.topLeft(), box.bottomRight());

    // One full spectrum across the box, rotated by the live hue so a shape being
    // rubber-banded keeps cycling. We lay down enough stops for a smooth sweep.
    constexpr int kStops = 24;
    for (int i = 0; i <= kStops; ++i) {
        const double t = double(i) / kStops;
        const int hue = (m_rainbowHue + int(t * 360.0)) % 360;
        g.setColorAt(t, QColor::fromHsv(hue, 255, 255));
    }
    return g;
}

// Run the rainbow cycle only while a rainbow stroke is actually in progress;
// otherwise it's idle (no wasted repaints, and a non-rainbow stroke is unaffected).
void Canvas::updateRainbowTimer()
{
    const bool shouldRun = m_drawing && m_strokeRainbow;
    if (shouldRun && !m_rainbowTimer->isActive())
        m_rainbowTimer->start();
    else if (!shouldRun && m_rainbowTimer->isActive())
        m_rainbowTimer->stop();
}

// One step of the colour cycle. Advance the hue, then keep colour flowing:
//  - freehand: stamp a dab at the current point, so a held-still brush still
//    paints a moving rainbow (the classic Paint feel);
//  - shapes: just repaint, so the rubber-band preview re-tints to the new hue.
void Canvas::onRainbowTick()
{
    // ~3 deg/tick at 30fps -> a full 360deg sweep in roughly 4 seconds.
    m_rainbowHue = (m_rainbowHue + 3) % 360;

    if (!m_drawing)
        return;

    if (m_tool == Tool::Pencil) {
        // Re-stamp at the last point; drawLineTo refreshes m_strokeColor from the
        // live hue. A zero-length segment still lays down the round brush dab.
        drawLineTo(m_lastPoint);
    } else {
        // Spray paints via its own timer (each burst reads the freshly-advanced
        // hue), and shapes have nothing to bake yet -- so here we just repaint to
        // re-tint a live shape preview.
        update();
    }
}

bool Canvas::isColorEraser(Qt::MouseButton button) const
{
    return m_tool == Tool::Eraser && button == Qt::RightButton;
}

// Selective color-eraser (classic Paint right-drag eraser): along the brush
// stroke, replace only pixels matching Color 1 with Color 2, leaving every
// other colour untouched. We rasterise the brush dab into a mask the same way
// drawLineTo would, then for each covered pixel swap it only if it matches.
void Canvas::colorEraseLineTo(const QPoint &endPoint)
{
    // Render the brush stroke shape into a 1-bit-ish mask so we know exactly
    // which pixels the brush covers (respecting brush size and round caps).
    QImage mask(m_image.size(), QImage::Format_ARGB32_Premultiplied);
    mask.fill(Qt::transparent);
    {
        QPainter mp(&mask);
        mp.setRenderHint(QPainter::Antialiasing, false);  // crisp coverage test
        QPen pen(Qt::black, m_brushSize, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        mp.setPen(pen);
        mp.drawLine(m_lastPoint, endPoint);
    }

    const QRgb from = m_primaryColor.rgb() | 0xff000000;   // Color 1 (opaque)
    const QRgb to   = m_secondaryColor.rgba();
    const int w = m_image.width(), h = m_image.height();

    // Only scan the bounding box of the dab, not the whole canvas.
    QRect bb = QRect(m_lastPoint, endPoint).normalized()
                   .adjusted(-m_brushSize, -m_brushSize, m_brushSize, m_brushSize)
                   .intersected(m_image.rect());

    for (int y = bb.top(); y <= bb.bottom(); ++y) {
        for (int x = bb.left(); x <= bb.right(); ++x) {
            if (x < 0 || y < 0 || x >= w || y >= h)
                continue;
            if (qAlpha(mask.pixel(x, y)) == 0)
                continue;                       // brush didn't cover this pixel
            if ((m_image.pixel(x, y) | 0xff000000u) == from)
                m_image.setPixel(x, y, to);     // matches Color 1 -> swap
        }
    }

    m_lastPoint = endPoint;
    setModified(true);
    update();
}

void Canvas::drawLineTo(const QPoint &endPoint)
{
    QPainter painter(&m_image);
    painter.setRenderHint(QPainter::Antialiasing, m_antialias);

    // m_strokeColor was chosen at mouse-press based on the button and tool
    // (the eraser already resolves to the background/secondary slot there), so
    // the pencil and eraser share this one path. A rainbow stroke refreshes the
    // colour from the live hue on every segment, so motion cycles too (the timer
    // handles a held-still brush).
    if (m_strokeRainbow)
        m_strokeColor = currentRainbowColor();
    QPen pen(m_strokeColor, m_brushSize, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    painter.drawLine(m_lastPoint, endPoint);

    m_lastPoint = endPoint;
    setModified(true);
    update();
}

void Canvas::commitShape(const QPoint &endPoint)
{
    QPainter painter(&m_image);
    painter.setRenderHint(QPainter::Antialiasing, m_antialias);

    // Bake a rainbow shape with the hue it ended on, matching the live preview.
    if (m_strokeRainbow)
        m_strokeColor = currentRainbowColor();
    // MiterJoin keeps rectangle corners sharp at any pen width (RoundJoin rounds
    // them off as the pen widens). It's a no-op for ellipses and single-segment
    // lines, so all three shape tools share it safely.
    QPen pen(m_strokeColor, m_brushSize, Qt::SolidLine, Qt::RoundCap, Qt::MiterJoin);
    painter.setPen(pen);

    paintShape(painter, m_shapeStart, endPoint);

    setModified(true);
    update();
}

// ---------------------------------------------------------------------------
// Free-form polygon tool
// ---------------------------------------------------------------------------

// Stroke (and, with the fill toggle, fill) a polygon through the shared pipeline
// so the live preview and the committed bake are pixel-identical. `closed`
// connects the last vertex back to the first; the preview passes false (it draws
// the open chain plus a rubber-band segment itself). Border = the path's button
// slot; fill = the other slot (matching rect/ellipse via fillButtonFor).
void Canvas::paintPolygon(QPainter &painter, const QVector<QPoint> &pts,
                          bool closed) const
{
    if (pts.isEmpty())
        return;

    const double off = m_antialias ? 0.0 : 0.5;
    QVector<QPointF> p;
    p.reserve(pts.size());
    for (const QPoint &q : pts)
        p.append(QPointF(q.x() + off, q.y() + off));

    // Border colour: the path's button slot, with rainbow resolving to a diagonal
    // gradient over the polygon's bounding box (consistent with §6b shapes).
    const bool borderRainbow = rainbowForButton(m_polyButton);
    QRectF box = QRectF(QPolygonF(p).boundingRect());
    if (box.width()  < 1.0) box.setWidth(1.0);
    if (box.height() < 1.0) box.setHeight(1.0);

    QPen pen(colorForButton(m_polyButton), m_brushSize,
             Qt::SolidLine, Qt::RoundCap, Qt::MiterJoin);
    if (borderRainbow)
        pen.setBrush(rainbowGradient(box));
    painter.setPen(pen);

    // Fill the interior only on a closed polygon with the toggle on.
    if (closed && m_shapeFill) {
        const Qt::MouseButton fillBtn = fillButtonFor(m_polyButton);
        if (rainbowForButton(fillBtn))
            painter.setBrush(rainbowGradient(box));
        else
            painter.setBrush(colorForButton(fillBtn));
    } else {
        painter.setBrush(Qt::NoBrush);
    }

    if (closed && p.size() >= 3)
        painter.drawPolygon(QPolygonF(p));
    else
        painter.drawPolyline(QPolygonF(p));   // <3 pts or open: just the chain
}

void Canvas::commitPolygon()
{
    if (!m_polyActive)
        return;

    // A degenerate path (a single point / no area) isn't worth baking; just
    // cancel so we don't push a no-op onto the undo stack.
    if (m_polyPoints.size() < 2) {
        cancelPolygon();
        return;
    }

    pushUndoSnapshot();
    {
        QPainter painter(&m_image);
        painter.setRenderHint(QPainter::Antialiasing, m_antialias);
        paintPolygon(painter, m_polyPoints, /*closed*/ true);
    }
    setModified(true);

    m_polyActive = false;
    m_polyPoints.clear();
    m_polyButton = Qt::NoButton;
    update();
}

void Canvas::cancelPolygon()
{
    m_polyActive = false;
    m_polyPoints.clear();
    m_polyButton = Qt::NoButton;
    update();
}

// Draw the current shape tool from `start` to `end` using the given painter.
// Shared by the committed draw (onto m_image) and the live preview (onto the
// widget) so the two are guaranteed pixel-identical.
//
// The half-pixel offset is the fix for the asymmetric-corner bug: with
// anti-aliasing off, a pen stroked along integer coordinates straddles the
// pixel boundary, and Qt's rounding lands cleanly on one corner but a half
// pixel off on the others -- so three corners stair-step and one looks sharp.
// Shifting by (0.5, 0.5) puts integer coordinates on pixel centres, so all
// four edges rasterise identically. With AA on we skip the offset (smoothing
// already handles sub-pixel edges and the offset would just blur them).
QPoint Canvas::constrainedEnd(const QPoint &start, const QPoint &end,
                              bool constrain, bool orbit) const
{
    if (!constrain)
        return end;

    const int dx = end.x() - start.x();
    const int dy = end.y() - start.y();

    if (m_tool == Tool::Line) {
        const double len = std::hypot(double(dx), double(dy));
        if (len < 0.5)
            return end;

        const double step = M_PI / 4.0;                           // 45 degrees
        const double snapped = std::round(std::atan2(double(dy), double(dx)) / step)
                               * step;

        if (orbit) {
            // Radial mode (Shift+Ctrl): keep the dragged length, just rotate to
            // the nearest 45deg. Endpoint orbits the start point.
            return QPoint(start.x() + int(std::round(std::cos(snapped) * len)),
                          start.y() + int(std::round(std::sin(snapped) * len)));
        }

        // Default (Shift): project the cursor onto the nearest 45deg guide line,
        // so the endpoint tracks your hand rather than orbiting. The projected
        // distance along the axis is the dot product of the drag vector with the
        // axis's unit vector.
        const double ux = std::cos(snapped);
        const double uy = std::sin(snapped);
        const double proj = dx * ux + dy * uy;        // signed distance on axis
        return QPoint(start.x() + int(std::round(ux * proj)),
                      start.y() + int(std::round(uy * proj)));
    }

    // Rectangle -> square, ellipse -> circle: equal extent on both axes, sized
    // to the larger drag, growing in the drag's direction (anchored at start).
    const int side = qMax(qAbs(dx), qAbs(dy));
    const int sx = (dx < 0) ? -1 : 1;
    const int sy = (dy < 0) ? -1 : 1;
    return QPoint(start.x() + sx * side, start.y() + sy * side);
}

void Canvas::paintShape(QPainter &painter, const QPoint &start, const QPoint &rawEnd)
{
    const double off = m_antialias ? 0.0 : 0.5;

    // Shift constrains the shape live (queried at paint time so pressing or
    // releasing Shift mid-drag updates the preview immediately). For lines,
    // adding Ctrl switches to the radial (orbit) snap; plain Shift projects.
    const Qt::KeyboardModifiers mods = QGuiApplication::keyboardModifiers();
    const bool shift = mods & Qt::ShiftModifier;
    const bool orbit = mods & Qt::ControlModifier;
    const QPoint end = constrainedEnd(start, rawEnd, shift, orbit);

    // Rainbow shapes paint as a diagonal full-spectrum gradient instead of a flat
    // hue. We rewrite the pen's brush to a gradient spanning the shape's bounding
    // box (for a line, its start->end extent), keeping the caller's width/caps.
    // Both the preview and the commit go through here, so they stay identical.
    if (m_strokeRainbow) {
        const QRect ext = QRect(start, end).normalized();
        QRectF box(ext);
        // A perfectly horizontal/vertical line has a zero-area box; give the
        // gradient a little depth so it still sweeps the spectrum along the line.
        if (box.width() < 1.0)  box.setWidth(1.0);
        if (box.height() < 1.0) box.setHeight(1.0);
        QPen pen = painter.pen();
        pen.setBrush(rainbowGradient(box));
        painter.setPen(pen);
    }

    if (m_tool == Tool::Line) {
        painter.drawLine(QPointF(start.x() + off, start.y() + off),
                         QPointF(end.x()   + off, end.y()   + off));
        return;
    }

    const QRect ri = QRect(start, end).normalized();
    const QRectF r(ri.x() + off, ri.y() + off, ri.width(), ri.height());

    // Fill toggle: paint the interior with the slot opposite the border button.
    // The border keeps the pen set by the caller (and the rainbow override above);
    // here we set the brush. A rainbow fill slot becomes a gradient over the
    // shape's box, matching how a rainbow border/fill looks elsewhere (§6b).
    if (m_shapeFill && (m_tool == Tool::Rectangle || m_tool == Tool::Ellipse)) {
        const Qt::MouseButton fillBtn = fillButtonFor(m_shapeButton);
        if (rainbowForButton(fillBtn)) {
            QRectF box(ri);
            if (box.width()  < 1.0) box.setWidth(1.0);
            if (box.height() < 1.0) box.setHeight(1.0);
            painter.setBrush(rainbowGradient(box));
        } else {
            painter.setBrush(colorForButton(fillBtn));
        }
    } else {
        painter.setBrush(Qt::NoBrush);
    }

    if (m_tool == Tool::Rectangle)
        painter.drawRect(r);
    else if (m_tool == Tool::Ellipse)
        painter.drawEllipse(r);
}

// 4-connected flood fill with colour tolerance.
//
// Tolerance matters because our shapes are anti-aliased: the boundary between
// (say) a black outline and the white interior is not a clean edge but a ramp
// of in-between greys. An *exact* colour match would stop the moment it met
// the first near-white edge pixel, leaving a white halo just inside the
// outline -- and would also skip stray near-white pixels in the interior.
// Matching anything "close enough" to the clicked colour closes both gaps.
//
// We compare squared Euclidean distance in RGB against a fixed threshold, and
// keep a `visited` bitmap so we never test a pixel twice. The visited guard is
// essential here: with tolerance, a just-filled pixel can itself be within
// tolerance of the target, so without it the fill could revisit forever.
void Canvas::floodFill(const QPoint &startPoint, const QColor &newColor)
{
    if (!m_image.rect().contains(startPoint))
        return;

    const int w = m_image.width();
    const int h = m_image.height();

    const QRgb target = m_image.pixel(startPoint);
    const QRgb replacement = newColor.rgba();

    // How far a pixel's colour may stray from the clicked colour and still be
    // filled, as a squared RGB distance. The tolerance only exists to swallow
    // anti-aliasing ramps (the grey gradient between an outline and its interior),
    // so it applies ONLY in smooth mode. In crisp mode -- the default, and the
    // only mode in indexed/sprite work -- every region is a flat colour, so the
    // fill must be an EXACT match: otherwise it bleeds across adjacent shades
    // (e.g. the two near-whites of a sprite's hair merge into one). Indexed mode
    // is crisp by construction, so distinct palette slots are never merged.
    const int kTolerance   = m_antialias ? 32 : 0;
    const int kThresholdSq = 3 * kTolerance * kTolerance;

    const int tr = qRed(target), tg = qGreen(target), tb = qBlue(target);

    // Does pixel colour `c` count as part of the region we're filling?
    auto matches = [&](QRgb c) -> bool {
        const int dr = qRed(c)   - tr;
        const int dg = qGreen(c) - tg;
        const int db = qBlue(c)  - tb;
        return (dr * dr + dg * dg + db * db) <= kThresholdSq;
    };

    if (matches(replacement) && matches(target)) {
        // Filling a region with a colour that is itself within tolerance of
        // the region would never terminate cleanly and produces no visible
        // change anyway -- bail out.
        if (target == replacement)
            return;
    }

    // `filled[idx]` == this pixel was reached by the flood and recoloured.
    // `visited[idx]` == this pixel has been enqueued/tested already.
    QVector<bool> filled(w * h, false);
    QVector<bool> visited(w * h, false);

    QStack<QPoint> stack;
    const int startIdx = startPoint.y() * w + startPoint.x();
    stack.push(startPoint);
    visited[startIdx] = true;

    auto consider = [&](int x, int y) {
        if (x < 0 || x >= w || y < 0 || y >= h)
            return;
        const int idx = y * w + x;
        if (visited[idx])
            return;
        visited[idx] = true;                 // mark on enqueue, not on pop
        if (matches(m_image.pixel(x, y)))
            stack.push(QPoint(x, y));
    };

    while (!stack.isEmpty()) {
        const QPoint p = stack.pop();
        m_image.setPixel(p, replacement);
        filled[p.y() * w + p.x()] = true;
        consider(p.x() + 1, p.y());
        consider(p.x() - 1, p.y());
        consider(p.x(), p.y() + 1);
        consider(p.x(), p.y() - 1);
    }

    // --- Edge dilation: grow the fill one pixel UNDER the anti-aliased edge ---
    //
    // Only needed when anti-aliasing is on. With crisp (aliased) edges -- the
    // default -- shapes are solid-colour right up to the boundary, so the
    // exact-ish flood already meets the outline with no gap, and running the
    // dilation would wrongly bleed into neighbouring solid regions. So we skip
    // it entirely unless smoothing produced an edge ramp to compensate for.
    //
    // When AA *is* on, the flood stops where the outline's ramp gets too far
    // from the clicked colour, leaving a thin un-recoloured ring (the ramp's
    // mid-tones) between fill and outline. We repaint each *unfilled* pixel
    // bordering the fill UNLESS it is strongly part of the outline (clearly
    // dark), tucking the fill under the light part of the ramp without bleeding
    // across the line core. 2 one-pixel passes cover a normal stroke's ramp.
    if (m_antialias) {
        const int outlineLuma = 64;  // pixels darker than this are treated as line
        auto luma = [](QRgb c) {
            return (qRed(c) * 30 + qGreen(c) * 59 + qBlue(c) * 11) / 100;
        };

        for (int pass = 0; pass < 2; ++pass) {
            QVector<QPoint> toPaint;
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const int idx = y * w + x;
                    if (filled[idx])
                        continue;
                    // Is any 4-neighbour already filled?
                    const bool borders =
                        (x > 0     && filled[idx - 1]) ||
                        (x < w - 1 && filled[idx + 1]) ||
                        (y > 0     && filled[idx - w]) ||
                        (y < h - 1 && filled[idx + w]);
                    if (!borders)
                        continue;
                    // Don't paint over the actual outline core.
                    if (luma(m_image.pixel(x, y)) < outlineLuma)
                        continue;
                    toPaint.push_back(QPoint(x, y));
                }
            }
            if (toPaint.isEmpty())
                break;
            for (const QPoint &p : toPaint) {
                m_image.setPixel(p, replacement);
                filled[p.y() * w + p.x()] = true;
            }
        }
    }

    // --- Rainbow fill: re-tint the filled region with a diagonal spectrum ------
    //
    // The flood above already determined the region (the `filled` set), using the
    // flat hue for matching. Now overpaint only those pixels with the rainbow
    // gradient spanning the region's bounding box. We render the gradient into a
    // buffer once and copy it through the `filled` mask, so the gradient is keyed
    // to the region's geometry (not the whole canvas) and respects holes/edges.
    if (m_strokeRainbow) {
        QRect bbox;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                if (filled[y * w + x]) {
                    if (bbox.isNull()) bbox = QRect(x, y, 1, 1);
                    else               bbox |= QRect(x, y, 1, 1);
                }

        if (!bbox.isNull()) {
            QImage grad(bbox.size(), QImage::Format_ARGB32_Premultiplied);
            grad.fill(Qt::transparent);
            {
                QPainter gp(&grad);
                // Gradient in buffer-local coords (origin at the bbox corner).
                gp.fillRect(grad.rect(),
                            rainbowGradient(QRectF(QPointF(0, 0), QSizeF(bbox.size()))));
            }
            for (int y = bbox.top(); y <= bbox.bottom(); ++y)
                for (int x = bbox.left(); x <= bbox.right(); ++x)
                    if (filled[y * w + x])
                        m_image.setPixel(x, y,
                            grad.pixel(x - bbox.left(), y - bbox.top()));
        }
    }

    setModified(true);
    update();
}

// ---------------------------------------------------------------------------
// Undo / redo
// ---------------------------------------------------------------------------

void Canvas::pushUndoSnapshot()
{
    m_undoStack.push_back(m_image);
    if (m_undoStack.size() > kMaxHistory)
        m_undoStack.removeFirst();
    m_redoStack.clear();   // a fresh edit invalidates the redo branch
    emit historyChanged();
}

void Canvas::undo()
{
    if (m_undoStack.isEmpty())
        return;
    m_redoStack.push_back(m_image);
    m_image = m_undoStack.takeLast();
    applyZoom();
    setModified(true);
    emit historyChanged();
}

void Canvas::redo()
{
    if (m_redoStack.isEmpty())
        return;
    m_undoStack.push_back(m_image);
    m_image = m_redoStack.takeLast();
    applyZoom();
    setModified(true);
    emit historyChanged();
}

void Canvas::clear()
{
    pushUndoSnapshot();
    m_image.fill(Qt::white);
    setModified(true);
    update();
}

void Canvas::setModified(bool m)
{
    if (m_modified == m)
        return;
    m_modified = m;
    emit modifiedChanged(m_modified);
}

// ---------------------------------------------------------------------------
// Text tool
// ---------------------------------------------------------------------------

void Canvas::setTool(Tool tool)
{
    // Switching away from the text tool while a box is open bakes it first.
    if (m_textActive && tool != Tool::Text)
        commitText();
    // Switching away from the polygon tool discards an unfinished path (it was
    // never committed, so we don't silently bake it -- mirrors hitting Escape).
    if (m_polyActive && tool != Tool::Polygon)
        cancelPolygon();
    // Defensive: never leave the spray/rainbow timers running across a tool swap.
    if (tool != Tool::Spray && m_sprayTimer && m_sprayTimer->isActive())
        m_sprayTimer->stop();
    m_tool = tool;
}

void Canvas::setFontPointSize(int pt)
{
    m_fontPointSize = qBound(4, pt, 400);
    // If the editor is open, re-style it live so size changes are visible.
    if (m_textActive)
        positionTextEditor();
}

void Canvas::beginText(const QPoint &imagePos)
{
    if (!m_textEdit) {
        m_textEdit = new QLineEdit(this);
        m_textEdit->setFrame(false);
        // Transparent background so the canvas shows through and it reads as
        // text sitting on the image rather than a widget. The zero margins and
        // zero text margin are important: they make the editor's text start at
        // its exact top-left corner, so the baked text (drawn at the same
        // origin) lines up pixel-for-pixel with what was typed. Without this,
        // the widget's built-in padding shifts the bake up-and-left.
        // color: transparent hides the editor's own glyphs reliably (palette
        // alone proved flaky). We render the visible glyphs ourselves in
        // paintEvent. The native blinking caret still shows (it ignores this
        // color), which is exactly what we want -- one caret, and it tracks
        // typing for free.
        // The widget is parked off-screen (see positionTextEditor) and is never
        // seen; it's just the text/cursor model + keyboard target. We render all
        // visible glyphs and the caret ourselves in paintEvent.
        m_textEdit->setAttribute(Qt::WA_MacShowFocusRect, false);
        // editingFinished fires on Enter AND on focus loss (e.g. clicking a
        // toolbar button), so it covers commit-on-click-away in one signal.
        connect(m_textEdit, &QLineEdit::editingFinished, this, [this]() {
            if (m_textActive)
                commitText();
        });
        // Repaint as the user types / moves the caret; reset the blink to ON so
        // the caret is solid right after a keystroke (feels responsive).
        auto resetCaret = [this]() { m_caretOn = true; update(); };
        connect(m_textEdit, &QLineEdit::textChanged, this, resetCaret);
        connect(m_textEdit, &QLineEdit::cursorPositionChanged, this, resetCaret);
        connect(m_textEdit, &QLineEdit::selectionChanged, this, resetCaret);
        m_textEdit->installEventFilter(this);   // for Escape == cancel
    }

    if (!m_caretBlink) {
        m_caretBlink = new QTimer(this);
        m_caretBlink->setInterval(530);   // standard caret blink period
        connect(m_caretBlink, &QTimer::timeout, this, [this]() {
            m_caretOn = !m_caretOn;
            update();
        });
    }

    m_textOrigin = imagePos;
    m_textActive = true;
    m_caretOn = true;
    m_textEdit->clear();
    positionTextEditor();
    m_textEdit->show();
    m_textEdit->setFocus();
    m_caretBlink->start();
}

void Canvas::positionTextEditor()
{
    if (!m_textEdit)
        return;

    // The QLineEdit is purely an invisible INPUT MODEL now: it holds the text,
    // the cursor index, and the selection, and it receives all keystrokes while
    // focused -- but we render every visible thing ourselves (glyphs AND caret)
    // in paintEvent, from the same QFontMetrics/origin, so nothing can drift as
    // text grows or zoom changes. To guarantee its own caret and internal
    // horizontal scrolling never appear, we park the widget well off-screen and
    // give it generous width so it never scrolls its content.
    m_textEdit->setGeometry(-10000, -10000, 100000, 40);
}

// Paint text at the document's true resolution. Used by BOTH the live preview
// and the final bake, so what you see while typing is exactly what lands. The
// painter is assumed to be in image coordinates.
void Canvas::drawTextRun(QPainter &painter, const QString &text) const
{
    if (text.isEmpty())
        return;
    painter.setRenderHint(QPainter::Antialiasing, m_antialias);
    painter.setRenderHint(QPainter::TextAntialiasing, m_antialias);

    QFont f = font();
    f.setPointSize(m_fontPointSize);
    painter.setFont(f);
    painter.setPen(m_primaryColor);

    // drawText's point is the text BASELINE; our origin is the top-left, so add
    // the font ascent to drop down to the baseline.
    const QFontMetrics fm(f);
    painter.drawText(m_textOrigin.x(), m_textOrigin.y() + fm.ascent(), text);
}

void Canvas::commitText()
{
    if (!m_textActive || !m_textEdit)
        return;

    const QString text = m_textEdit->text();
    m_textActive = false;
    m_textEdit->hide();
    if (m_caretBlink)
        m_caretBlink->stop();

    if (text.isEmpty())
        return;   // nothing typed -> nothing to bake, no undo entry

    pushUndoSnapshot();
    QPainter painter(&m_image);
    drawTextRun(painter, text);   // identical call to the live preview
    setModified(true);
    update();
}

void Canvas::cancelText()
{
    if (!m_textEdit)
        return;
    m_textActive = false;
    m_textEdit->hide();
    if (m_caretBlink)
        m_caretBlink->stop();
    update();
}

// ---------------------------------------------------------------------------
// Zoom
// ---------------------------------------------------------------------------

// Map a widget-space position to an image pixel. Dividing by the zoom factor
// is the inverse of the scale we apply in paintEvent. clamp keeps tool clicks
// from running off the edge of the document by a sub-pixel at high zoom.
QPoint Canvas::toImage(const QPointF &widgetPos) const
{
    // Subtract the overscroll padding: the image's top-left is at (m_pad, m_pad)
    // in widget space, not (0, 0).
    const int x = qBound(0, int((widgetPos.x() - m_pad) / m_zoom), m_image.width()  - 1);
    const int y = qBound(0, int((widgetPos.y() - m_pad) / m_zoom), m_image.height() - 1);
    return QPoint(x, y);
}

QPointF Canvas::toImageF(const QPointF &widgetPos) const
{
    // Same mapping as toImage but precise and un-clamped -- for the ruler markers,
    // which need sub-pixel accuracy at low zoom (where one screen px spans many
    // image px) and should track the pointer even out over the overscroll margin.
    return QPointF((widgetPos.x() - m_pad) / m_zoom,
                   (widgetPos.y() - m_pad) / m_zoom);
}

QScrollArea *Canvas::enclosingScrollArea() const
{
    // setWidget() reparents us onto the scroll area's viewport, so the chain is
    // canvas -> viewport -> QScrollArea.
    QWidget *vp = parentWidget();
    return qobject_cast<QScrollArea *>(vp ? vp->parentWidget() : nullptr);
}

void Canvas::applyZoom()
{
    // The widget's own size carries the magnification: at 4x a 800x600 image
    // occupies 3200x2400, and the surrounding QScrollArea scrolls over it.
    //
    // We also pad the widget around the image so the canvas can be *overscrolled*
    // -- dragged so an edge/corner clears the viewport border (and the toolbar)
    // and can be worked on comfortably. A modest fraction of the viewport is
    // plenty; more than that just drowns the canvas in grey. Floored for when the
    // viewport isn't known yet.
    int pad = 80;
    if (QScrollArea *area = enclosingScrollArea()) {
        const QSize vp = area->viewport()->size();
        pad = qMax(pad, qMax(vp.width(), vp.height()) / 3);
    }
    m_pad = pad;

    setFixedSize(m_image.size() * m_zoom + QSize(2 * m_pad, 2 * m_pad));
    updateGeometry();
    if (m_textActive)
        positionTextEditor();   // keep the open text box aligned under zoom
    update();
}

void Canvas::setZoom(double factor)
{
    // Clamp to a sane range. 0.1x .. 32x covers "see the whole thing" through
    // "edit one pixel at a time" without letting the widget get absurd.
    const double clamped = qBound(0.1, factor, 32.0);
    if (qFuzzyCompare(clamped, m_zoom))
        return;
    m_zoom = clamped;
    applyZoom();
    emit zoomChanged(m_zoom);
}

void Canvas::setAntialiasing(bool on)
{
    // Indexed mode locks crisp edges: anti-aliasing invents in-between colours
    // that aren't palette entries, which would convert to wrong indices on save.
    if (m_indexed)
        on = false;
    // Affects only future drawing -- existing pixels are not re-rendered.
    m_antialias = on;
}

void Canvas::setGridEnabled(bool on)
{
    if (m_gridEnabled == on)
        return;
    m_gridEnabled = on;
    update();        // overlay only -- no document change
}

void Canvas::setGridSize(int w, int h)
{
    m_gridW = qMax(1, w);
    m_gridH = qMax(1, h);
    if (m_gridEnabled)
        update();
}

void Canvas::setPixelGridEnabled(bool on)
{
    if (m_pixelGrid == on)
        return;
    m_pixelGrid = on;
    update();        // overlay only -- no document change
}

void Canvas::leaveEvent(QEvent *event)
{
    emit cursorLeft();          // blank the status-bar coordinate readout
    QWidget::leaveEvent(event);
}

void Canvas::wheelEvent(QWheelEvent *event)
{
    // Plain wheel = let the scroll area scroll (default behaviour).
    // Ctrl+wheel = zoom, anchored on the pixel under the cursor.
    if (!(event->modifiers() & Qt::ControlModifier)) {
        QWidget::wheelEvent(event);
        return;
    }

    // The image pixel currently under the cursor -- the anchor we keep fixed.
    // Subtract the overscroll padding: the image starts at (m_pad, m_pad).
    const QPointF cursorInWidget = event->position();
    const double imgX = (cursorInWidget.x() - m_pad) / m_zoom;
    const double imgY = (cursorInWidget.y() - m_pad) / m_zoom;

    const double step = (event->angleDelta().y() > 0) ? 1.25 : 1.0 / 1.25;
    const double target = qBound(0.1, m_zoom * step, 32.0);
    if (qFuzzyCompare(target, m_zoom)) {
        event->accept();
        return;
    }

    // Find the enclosing scroll area (if any) so we can re-anchor afterwards.
    // The cursor's position within the VIEWPORT must stay constant; only the
    // scroll offset changes. viewportPos = widgetPos - scrollOffset.
    auto *area = enclosingScrollArea();

    QPoint viewportCursor;
    if (area)
        viewportCursor = mapTo(area->viewport(), cursorInWidget.toPoint());

    m_zoom = target;
    applyZoom();
    emit zoomChanged(m_zoom);

    if (area) {
        // Where the anchor pixel sits in the (newly sized) widget, and the
        // scroll offset that puts it back under the same viewport position.
        // (+ m_pad because the image is inset by the overscroll padding.)
        const QPointF anchorInWidget(imgX * m_zoom + m_pad, imgY * m_zoom + m_pad);
        area->horizontalScrollBar()->setValue(
            int(anchorInWidget.x() - viewportCursor.x()));
        area->verticalScrollBar()->setValue(
            int(anchorInWidget.y() - viewportCursor.y()));
    }

    event->accept();
}

// While a shape is being dragged, pressing/releasing Shift must refresh the
// preview even if the mouse hasn't moved, so the snap appears/disappears live.
// (paintShape reads the live modifier state; we just need to trigger a repaint.)
// Shift and (for lines) Ctrl change the constrain result, so refresh the
// preview when either is pressed/released mid-drag, even without mouse motion.
static bool isConstrainKey(int key)
{
    return key == Qt::Key_Shift || key == Qt::Key_Control;
}

void Canvas::keyPressEvent(QKeyEvent *event)
{
    if (isConstrainKey(event->key()) && m_drawing &&
        (m_tool == Tool::Line || m_tool == Tool::Rectangle || m_tool == Tool::Ellipse)) {
        update();
    }

    // Polygon keyboard control: Enter commits (closed), Escape cancels, Backspace
    // removes the last placed vertex (cancelling the whole path if none remain).
    if (m_tool == Tool::Polygon && m_polyActive) {
        switch (event->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
            commitPolygon();
            event->accept();
            return;
        case Qt::Key_Escape:
            cancelPolygon();
            event->accept();
            return;
        case Qt::Key_Backspace:
            m_polyPoints.removeLast();
            if (m_polyPoints.isEmpty())
                cancelPolygon();
            else
                update();
            event->accept();
            return;
        default:
            break;
        }
    }

    QWidget::keyPressEvent(event);
}

void Canvas::keyReleaseEvent(QKeyEvent *event)
{
    if (isConstrainKey(event->key()) && m_drawing &&
        (m_tool == Tool::Line || m_tool == Tool::Rectangle || m_tool == Tool::Ellipse)) {
        update();
    }
    QWidget::keyReleaseEvent(event);
}

bool Canvas::eventFilter(QObject *watched, QEvent *event)
{
    // Escape while typing discards the text box without baking it.
    if (watched == m_textEdit && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        if (ke->key() == Qt::Key_Escape) {
            cancelText();
            return true;   // swallow it
        }
    }
    return QWidget::eventFilter(watched, event);
}
