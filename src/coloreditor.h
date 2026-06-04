// Daint: a simple MS-Paint-style raster paint program.
// Copyright (C) 2026  eagre.dev
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. This program is distributed WITHOUT ANY WARRANTY; see the GNU General
// Public License (LICENSE file) for details.

#pragma once

#include <QDialog>
#include <QWidget>
#include <QColor>
#include <QImage>
#include <QVariantMap>
#include <QVector>

class QPushButton;
class QSpinBox;
class QLineEdit;

// ---------------------------------------------------------------------------
// HueSatField: the 2D colour square. X axis = hue (0..359), Y axis = saturation
// (255 at top -> 0 at bottom), drawn at full value. Clicking/dragging picks a
// hue+saturation pair; the value (brightness) comes from a separate slider.
// Emits hueSatChanged(h, s) as the user drags.
// ---------------------------------------------------------------------------
class HueSatField : public QWidget
{
    Q_OBJECT
public:
    explicit HueSatField(QWidget *parent = nullptr);
    void setHueSat(int hue, int sat);     // move the marker without emitting
    QSize sizeHint() const override { return QSize(240, 200); }

signals:
    void hueSatChanged(int hue, int sat);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void resizeEvent(QResizeEvent *) override;

private:
    void pickAt(const QPoint &pos);
    void rebuildCache();                  // regenerate the gradient image

    int    m_hue = 0;
    int    m_sat = 0;
    QImage m_cache;                       // cached gradient at current size
};

// ---------------------------------------------------------------------------
// ValueBar: a vertical brightness slider for the currently-chosen hue/sat. Top
// = full value (255), bottom = 0. Emits valueChanged(v) as the user drags.
// ---------------------------------------------------------------------------
class ValueBar : public QWidget
{
    Q_OBJECT
public:
    explicit ValueBar(QWidget *parent = nullptr);
    void setHueSat(int hue, int sat);     // recolour the bar's gradient
    void setValue(int v);                 // move the marker without emitting
    QSize sizeHint() const override { return QSize(24, 200); }

signals:
    void valueChanged(int value);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;

private:
    void pickAt(const QPoint &pos);

    int m_hue = 0;
    int m_sat = 0;
    int m_value = 255;
};

// ---------------------------------------------------------------------------
// ColorEditor: Daint's "Edit Colours" dialog. A "Colors" grid (the canvas
// palette, editable in place) sits above a fully hand-built colour picker
// (hue/sat square + value bar + preview + RGB/HTML fields). Nothing from Qt's
// stock QColorDialog is embedded, so there's no private-layout dead space.
//
// Click a swatch to select it, then mix with the picker to recolour that swatch
// live. accept() returns the edited palette via colors().
// ---------------------------------------------------------------------------
class ColorEditor : public QDialog
{
    Q_OBJECT
public:
    ColorEditor(const QVector<QColor> &colors, int columns,
                const QColor &initial, QWidget *parent = nullptr);
    ~ColorEditor() override;

    QVector<QColor> colors()        const { return m_colors; }
    QColor          selectedColor() const;

protected:
    // Arrow keys move the selected palette slot (Left/Right within a row,
    // Up/Down between rows) no matter which control has focus, so you can sweep
    // slots without the mouse. We use an event filter rather than keyPressEvent
    // because focused children (the OK button, swatches) would otherwise consume
    // arrows for their own focus navigation before the dialog ever saw them.
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // Move the selected slot by a delta, clamped to range. Returns true if it
    // handled the key (so the filter can swallow it).
    bool moveSelection(int delta);
    QWidget *buildGrid(int columns);
    QWidget *buildPicker();
    void     paintSwatch(int index);
    void     selectSwatch(int index);       // highlight + load into picker

    // "Pick from screen" eyedropper: grab a colour from anywhere on screen via
    // the XDG desktop portal, then load it into the picker (and the selected
    // swatch) like any other edit. Async; the result arrives on the portal
    // Request's Response signal. Linux/Unix only; the portal is DBus-based, so
    // off those platforms the feature (and its button) are omitted entirely.
#ifdef DAINT_HAS_SCREEN_PICKER
private slots:
    void pickFromScreen();
    void onPortalColorResponse(uint response, const QVariantMap &results);
#endif

private:

    // Push m_current into every picker control + the selected swatch. `source`
    // is the control that originated the change, so we don't fight its own
    // editing (e.g. don't reset the spinbox the user is typing into).
    enum class Src { Field, Bar, Spin, Html, External };
    void     setCurrent(const QColor &c, Src source);

    QVector<QColor>        m_colors;
    QVector<QPushButton *> m_btns;
    int                    m_selIndex = -1;
    int                    m_columns  = 1;   // slots per palette row

    HueSatField *m_field   = nullptr;
    ValueBar    *m_bar     = nullptr;
    QWidget     *m_preview = nullptr;        // shows m_current
    QSpinBox    *m_red     = nullptr;
    QSpinBox    *m_green   = nullptr;
    QSpinBox    *m_blue    = nullptr;
    QLineEdit   *m_html    = nullptr;

    QColor m_current = Qt::black;
    bool   m_syncing = false;                // guards the control<->control echo
};
