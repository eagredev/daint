// Daint: a simple MS-Paint-style raster paint program.
// Copyright (C) 2026  eagre.dev
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. This program is distributed WITHOUT ANY WARRANTY; see the GNU General
// Public License (LICENSE file) for details.

#include "ruler.h"

#include <QPainter>
#include <QPaintEvent>

Ruler::Ruler(Orientation orientation, QWidget *parent)
    : QWidget(parent), m_orientation(orientation)
{
    // The ruler is decorative chrome: it should never grab clicks meant for the
    // canvas, and it has a fixed thin band.
    setAttribute(Qt::WA_TransparentForMouseEvents);
    if (m_orientation == Orientation::Horizontal)
        setFixedHeight(kThickness);
    else
        setFixedWidth(kThickness);
}

QSize Ruler::sizeHint() const
{
    return m_orientation == Orientation::Horizontal ? QSize(0, kThickness)
                                                    : QSize(kThickness, 0);
}

void Ruler::setZoom(double zoom)
{
    if (qFuzzyCompare(m_zoom, zoom))
        return;
    m_zoom = zoom;
    update();
}

void Ruler::setScroll(int offsetPx)
{
    if (m_scroll == offsetPx)
        return;
    m_scroll = offsetPx;
    update();
}

void Ruler::setDocLength(int imagePixels)
{
    if (m_docLength == imagePixels)
        return;
    m_docLength = imagePixels;
    update();
}

void Ruler::setOrigin(int padPx)
{
    if (m_origin == padPx)
        return;
    m_origin = padPx;
    update();
}

void Ruler::setCursorImagePos(double imagePos, bool valid)
{
    if (qFuzzyCompare(m_cursorPos, imagePos) && m_cursorValid == valid)
        return;
    m_cursorPos = imagePos;
    m_cursorValid = valid;
    update();
}

// image px -> ruler widget px. The canvas widget is inset from its own origin by
// the overscroll padding (m_origin) and then scrolled by m_scroll, so a given
// image pixel sits at (imagePos * zoom) + origin - scroll along the axis.
double Ruler::imageToRuler(double imagePos) const
{
    return imagePos * m_zoom + m_origin - m_scroll;
}

// Choose a tick step (in image px) that keeps ~>=48 screen px between labels, so
// numbers never collide. Steps follow a 1/2/5 x 10^n progression (1,2,5,10,...).
int Ruler::chooseStep() const
{
    const double minLabelPx = 56.0;     // min on-screen gap between labelled ticks
    // Walk a clean 1-2-5 x 10^n progression and return the first step whose ticks
    // are far enough apart on screen. (The old version multiplied a running value
    // by 2/2.5 and rounded it, which drifted onto messy non-1-2-5 steps like 320
    // and could under-space, so labels collided at low zoom.)
    static const int mantissa[] = { 1, 2, 5 };
    int decade = 1;
    while (decade <= 1000000) {          // guard; far beyond any real canvas
        for (int m : mantissa) {
            const int step = m * decade;
            if (step * m_zoom >= minLabelPx)
                return step;
        }
        decade *= 10;
    }
    return decade;                       // unreachable in practice
}

void Ruler::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const bool horiz = (m_orientation == Orientation::Horizontal);

    // Band background + edge line, themed via the palette so it looks native.
    p.fillRect(rect(), palette().color(QPalette::Window));
    p.setPen(palette().color(QPalette::Mid));
    if (horiz)
        p.drawLine(0, height() - 1, width(), height() - 1);
    else
        p.drawLine(width() - 1, 0, width() - 1, height());

    if (m_docLength <= 0 || m_zoom <= 0.0)
        return;

    const int step = chooseStep();
    const QColor tickColor = palette().color(QPalette::Text);
    const QColor labelColor = tickColor;

    QFont f = font();
    f.setPointSizeF(qMax(6.0, f.pointSizeF() - 2.0));
    p.setFont(f);
    const QFontMetrics fm(f);

    // Draw a labelled major tick every `step` image px, plus a minor tick at the
    // half-step when there's room. We only iterate over ticks that fall within
    // the visible band (cheap; bounded by the widget's pixel length).
    const int bandLen = horiz ? width() : height();
    for (int i = 0; i * step <= m_docLength; ++i) {
        const int imgPos = i * step;
        const double rp = imageToRuler(imgPos);
        if (rp < -40 || rp > bandLen + 40)
            continue;   // off-screen tick; skip

        // Major tick mark.
        p.setPen(tickColor);
        if (horiz)
            p.drawLine(QPointF(rp, height() - 7), QPointF(rp, height() - 1));
        else
            p.drawLine(QPointF(width() - 7, rp), QPointF(width() - 1, rp));

        // Numeric label.
        p.setPen(labelColor);
        const QString text = QString::number(imgPos);
        if (horiz) {
            p.drawText(QPointF(rp + 3, fm.ascent() + 1), text);
        } else {
            // Vertical ruler: rotate the text 90deg so it reads bottom-to-top and
            // runs ALONG the ruler (the label sits just below its tick at `rp`).
            // We rotate about the tick position, then draw to the right of the
            // rotated origin so the glyphs sit inside the band, not clipped.
            p.save();
            p.translate(2, rp + 3);     // baseline near the inner edge of the band
            p.rotate(-90);
            // After -90 the text grows in -x of the rotated frame (i.e. upward on
            // screen); offset by the text width so it starts at the tick and runs
            // downward along the ruler instead of off the top.
            p.drawText(QPointF(-fm.horizontalAdvance(text), fm.ascent()), text);
            p.restore();
        }

        // Minor tick at the half-step (skip if it would crowd).
        if (step >= 2 && (step * m_zoom) >= 24.0) {
            const double mid = imageToRuler(imgPos + step / 2.0);
            if (mid >= 0 && mid <= bandLen) {
                p.setPen(tickColor);
                if (horiz)
                    p.drawLine(QPointF(mid, height() - 4), QPointF(mid, height() - 1));
                else
                    p.drawLine(QPointF(width() - 4, mid), QPointF(width() - 1, mid));
            }
        }
    }

    // Live cursor marker: a highlighted line at the pointer's position so you can
    // read off the scale at a glance (the MS Paint touch).
    if (m_cursorValid) {
        const double cp = imageToRuler(m_cursorPos);
        if (cp >= 0 && cp <= bandLen) {
            p.setPen(QPen(palette().color(QPalette::Highlight), 1));
            if (horiz)
                p.drawLine(QPointF(cp, 0), QPointF(cp, height() - 1));
            else
                p.drawLine(QPointF(0, cp), QPointF(width() - 1, cp));
        }
    }
}
