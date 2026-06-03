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
#include <QScrollArea>

// QScrollArea::setViewportMargins is protected; this trivial subclass exposes it
// so the MainWindow can reserve space at the top/left for the rulers. (The
// viewport-margin gutter is the standard Qt pattern for rulers/line gutters.)
class RulerScrollArea : public QScrollArea
{
    Q_OBJECT
public:
    using QScrollArea::QScrollArea;
    void setRulerMargins(int top, int left) {
        setViewportMargins(left, top, 0, 0);
    }
};

// A thin MS-Paint-style ruler drawn along the top or left edge of the canvas
// viewport. It shows IMAGE-pixel coordinates with tick marks + numeric labels,
// scales with the canvas zoom, and tracks scrolling (so the numbers under the
// visible region are always correct). An optional cursor marker shows where the
// pointer currently is.
//
// The ruler is a standalone widget that the MainWindow places in the scroll
// area's viewport margins (the standard Qt pattern for gutters/rulers). It does
// no scrolling itself; the MainWindow feeds it the current scroll offset, zoom,
// and document length, and the ruler just paints.
class Ruler : public QWidget
{
    Q_OBJECT

public:
    enum class Orientation { Horizontal, Vertical };

    explicit Ruler(Orientation orientation, QWidget *parent = nullptr);

    // Thickness of the ruler band (height for horizontal, width for vertical).
    static constexpr int kThickness = 22;

    // Fed by the MainWindow whenever the view changes.
    void setZoom(double zoom);
    void setScroll(int offsetPx);        // scrollbar value along this axis
    void setDocLength(int imagePixels);  // image width (H) or height (V)
    void setOrigin(int padPx);           // overscroll padding before image px 0
    // Live cursor marker. Takes a precise (sub-pixel, un-clamped) image position
    // so the marker tracks the pointer smoothly even at low zoom, where many image
    // pixels map to one screen pixel and an integer position would visibly lag.
    void setCursorImagePos(double imagePos, bool valid);

protected:
    void paintEvent(QPaintEvent *event) override;
    QSize sizeHint() const override;

private:
    // Map an image-pixel coordinate to a position along this ruler (widget px).
    double imageToRuler(double imagePos) const;
    // Pick a "nice" tick step (in image px) so labels never overlap at this zoom.
    int    chooseStep() const;

    Orientation m_orientation;
    double m_zoom      = 1.0;
    int    m_scroll    = 0;     // px scrolled along this axis
    int    m_origin    = 0;     // overscroll padding before image pixel 0
    int    m_docLength = 0;     // image extent along this axis (px)
    double m_cursorPos = 0.0;   // precise image-pixel coord of the cursor (axis)
    bool   m_cursorValid = false;
};
