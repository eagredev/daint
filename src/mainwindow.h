// Daint: a simple MS-Paint-style raster paint program.
// Copyright (C) 2026  eagre.dev
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. This program is distributed WITHOUT ANY WARRANTY; see the GNU General
// Public License (LICENSE file) for details.

#pragma once

#include <QMainWindow>
#include <QColor>
#include <QSize>
#include <QVector>

class Canvas;
class Ruler;
class RulerScrollArea;
class QAction;
class QActionGroup;
class QLabel;
class QSlider;
class QSpinBox;
class QPushButton;

// MainWindow owns all the chrome: the tool toolbar, the colour palette, the
// menus, and the file open/save flow. The actual drawing lives in Canvas;
// this class is just wiring between the user's clicks and Canvas's API.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;   // prompt to save if dirty
    // Watches the scroll area's viewport to reposition the rulers on resize.
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void newFile();
    void openFile();
    bool saveFile();        // returns false if the user cancelled
    bool saveFileAs();
    void resizeCanvas();    // Image -> Resize... (resize the current canvas)
    void chooseColor();     // open the system colour dialog (edits primary)
    void swapColors();      // swap Color 1 <-> Color 2 (colour + rainbow flag)
    void updateTitle();
    void onColorPicked(const QColor &c);            // eyedropper -> primary
    void onSecondaryColorPicked(const QColor &c);   // eyedropper -> secondary
    // The canvas entered/left indexed (sprite) mode. Swaps the normal colour
    // strip for a locked palette strip built from `palette`, and toggles the
    // status-bar INDEXED badge. See DESIGN.md §14.
    void onIndexedModeChanged(bool indexed, const QVector<QRgb> &palette);

private:
    void createActions();
    void createToolbar();
    void createPalette();
    void createStatusBar();
    void setupRulers();          // build + wire the top/left rulers
    void layoutRulers();         // position rulers in the viewport margins
    void centerView();           // scroll so the canvas is centred in the viewport
    void setRulersVisible(bool on);
    void refreshRulerMetrics();  // push current zoom + doc size to both rulers

    // Convert between the zoom factor (multiplicative, 0.1..32) and the slider's
    // linear integer position, using a logarithmic mapping so equal slider
    // travel means equal zoom ratio.
    int    zoomToSlider(double factor) const;
    double sliderToZoom(int value) const;

    // Modal width x height prompt, shared by New and Resize. `title` labels the
    // dialog; `initial` pre-fills the spinboxes. Returns an invalid QSize if the
    // user cancels.
    QSize  promptForSize(const QString &title, const QSize &initial);

    // The two colour slots. Primary = left button, secondary = right.
    // Selecting a flat colour clears that slot's rainbow flag; selecting the
    // rainbow swatch sets it. Either way the canvas and the indicator swatches
    // are kept in sync via refreshColorSwatches().
    void setPrimaryColor(const QColor &c);
    void setSecondaryColor(const QColor &c);
    void setPrimaryRainbow();                // left-click the rainbow swatch
    void setSecondaryRainbow();              // right-click the rainbow swatch
    void refreshColorSwatches();             // repaint the two overlapping swatches

    // CSS for a rainbow gradient fill, shared by the palette swatch and the
    // current-colour indicator when a slot is in rainbow mode.
    static QString rainbowStyleSheet();
    bool maybeSave();       // returns true if it's safe to discard

    // Repaint a slot button to show its stored colour. `row` picks which strip:
    // 0 = preset row, 1 = custom row.
    void paintSlot(int row, int index);
    // Build one row of clickable colour slots into `grid` at `gridRow`.
    // `colors` is the backing store (mutated by edits); `buttons` collects the
    // created QPushButtons for later repaint/double-click lookup.
    void buildColorRow(class QGridLayout *grid, int gridRow, int row,
                       QVector<QColor> &colors,
                       QVector<class QPushButton *> &buttons);

    Canvas *m_canvas = nullptr;
    RulerScrollArea *m_scroll = nullptr;
    Ruler  *m_hRuler = nullptr;     // top ruler (X axis)
    Ruler  *m_vRuler = nullptr;     // left ruler (Y axis)
    bool    m_rulersVisible = false;

    QActionGroup *m_toolGroup = nullptr;
    QLabel       *m_primarySwatch   = nullptr;  // front swatch (Color 1)
    QLabel       *m_secondarySwatch = nullptr;  // back swatch (Color 2)
    QWidget      *m_swatchBox        = nullptr;  // clickable container -> swapColors()
    QLabel       *m_zoomLabel   = nullptr;   // shows the current zoom %
    QLabel       *m_coordLabel  = nullptr;   // left-of-status cursor x,y readout
    QSlider      *m_zoomSlider  = nullptr;   // bottom-right zoom scrubber
    QSpinBox     *m_sizeSpin    = nullptr;   // brush-size spinner (defaults to 1
                                             // on entering indexed/sprite mode)
    QAction      *m_fontControlsAction = nullptr;  // toolbar slot for font size
                                                   // (shown only with Text tool)
    QAction      *m_fillControlsAction = nullptr;  // toolbar slot for shape-fill
                                                   // toggle (shown for rect/ellipse)
    bool          m_syncingZoom = false;     // guards the slider<->canvas loop

    QColor   m_primaryColor   = Qt::black;
    QColor   m_secondaryColor = Qt::white;
    QString  m_currentPath;                  // empty == untitled
    QSize    m_lastNewSize    = QSize(800, 600);  // default/last size for New

    // The colour box: two rows of slots. Click selects (left=Color 1,
    // right=Color 2). Editing both rows happens in the "Edit Colours..." dialog
    // (ColorEditor), which shows them as labelled "Main colors" / "Custom colors"
    // grids. The preset row starts from the built-in palette; the custom row
    // starts all-white.
    QVector<QPushButton *> m_presetSlots;
    QVector<QColor>        m_presetColors;
    QVector<QPushButton *> m_customSlots;
    QVector<QColor>        m_customColors;

    // The special rainbow swatch -- the last slot in the colour box. Clicking it
    // (left/right) puts the corresponding colour slot into rainbow mode, where
    // strokes cycle the spectrum instead of using a flat colour. Reviving MS
    // Paint's old rainbow brush, which lived in the palette rather than as a tool.
    QPushButton *m_rainbowSwatch = nullptr;
    bool         m_primaryRainbow   = false;
    bool         m_secondaryRainbow = false;

    // --- Indexed (sprite) mode UI ------------------------------------------
    // While indexed, the normal doodle colour controls (the two-row swatch grid,
    // the rainbow swatch, the Edit Colours button) are hidden and replaced by a
    // strip of the sprite's locked palette. We keep handles to the swappable
    // groups so we can show/hide them as a unit, and rebuild the palette strip
    // each time a new sprite opens. m_indexedBadge is the status-bar chip.
    void buildPaletteStrip(const QVector<QRgb> &palette);   // (re)populate strip
    class QToolBar *m_colourBar    = nullptr;   // the "Colours" toolbar
    QWidget        *m_colourGrid    = nullptr;   // normal preset/custom/rainbow grid
    QPushButton    *m_editColorsBtn = nullptr;   // "Edit Colours" button
    QWidget        *m_paletteStrip  = nullptr;   // locked-palette strip (indexed)
    QVector<QPushButton *> m_paletteSlots;       // its per-entry buttons
    QLabel         *m_indexedBadge  = nullptr;   // status-bar "INDEXED" chip
    // The QWidgetActions that wrap the toolbar widgets above. Toolbar widget
    // visibility is controlled through these actions, not the widgets directly.
    QAction        *m_colourGridAction   = nullptr;
    QAction        *m_colourSepAction    = nullptr;
    QAction        *m_editColorsAction   = nullptr;
    QAction        *m_paletteStripAction = nullptr;
};
