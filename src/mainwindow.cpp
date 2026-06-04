// Daint: a simple MS-Paint-style raster paint program.
// Copyright (C) 2026  eagre.dev
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. This program is distributed WITHOUT ANY WARRANTY; see the GNU General
// Public License (LICENSE file) for details.

#include "mainwindow.h"
#include "canvas.h"
#include "coloreditor.h"
#include "ruler.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QMouseEvent>
#include <QSlider>
#include <QStatusBar>
#include <QToolButton>
#include <cmath>
#include <QInputDialog>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QSpinBox>
#include <QToolBar>
#include <QWidget>

namespace {
// The classic 16-colour-ish palette, leaning into the retro look.
const QList<QColor> kPalette = {
    QColor("#000000"), QColor("#7f7f7f"), QColor("#880015"), QColor("#ed1c24"),
    QColor("#ff7f27"), QColor("#fff200"), QColor("#22b14c"), QColor("#00a2e8"),
    QColor("#3f48cc"), QColor("#a349a4"), QColor("#ffffff"), QColor("#c3c3c3"),
    QColor("#b97a57"), QColor("#ffaec9"), QColor("#ffc90e"), QColor("#b5e61d"),
};
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    m_canvas = new Canvas(this);

    // The canvas sits in a scroll area so large images / small windows behave.
    m_scroll = new RulerScrollArea(this);
    m_scroll->setWidget(m_canvas);
    m_scroll->setAlignment(Qt::AlignCenter);
    m_scroll->setBackgroundRole(QPalette::Mid);
    setCentralWidget(m_scroll);

    createActions();
    createToolbar();
    createPalette();
    createStatusBar();
    setupRulers();

    connect(m_canvas, &Canvas::modifiedChanged, this, &MainWindow::updateTitle);
    connect(m_canvas, &Canvas::colorPicked, this, &MainWindow::onColorPicked);
    connect(m_canvas, &Canvas::secondaryColorPicked, this,
            &MainWindow::onSecondaryColorPicked);

    // Push the initial slot colours into the canvas and paint the swatches.
    setPrimaryColor(m_primaryColor);
    setSecondaryColor(m_secondaryColor);
    updateTitle();
    resize(1000, 720);

    // Centre the canvas once the window has laid out (scrollbar ranges aren't
    // valid until then). Deferred to the event loop so it runs after the first
    // show/layout pass; otherwise the overscroll padding leaves the canvas
    // scrolled to the top-left, partly off-screen.
    QTimer::singleShot(0, this, &MainWindow::centerView);
}

// ---------------------------------------------------------------------------
// Actions & menus
// ---------------------------------------------------------------------------

void MainWindow::createActions()
{
    auto *fileMenu = menuBar()->addMenu(tr("&File"));

    auto *newAct = fileMenu->addAction(tr("&New"), this, &MainWindow::newFile);
    newAct->setShortcut(QKeySequence::New);

    auto *openAct = fileMenu->addAction(tr("&Open..."), this, &MainWindow::openFile);
    openAct->setShortcut(QKeySequence::Open);

    auto *saveAct = fileMenu->addAction(tr("&Save"), this, &MainWindow::saveFile);
    saveAct->setShortcut(QKeySequence::Save);

    auto *saveAsAct = fileMenu->addAction(tr("Save &As..."), this, &MainWindow::saveFileAs);
    saveAsAct->setShortcut(QKeySequence::SaveAs);

    fileMenu->addSeparator();
    auto *quitAct = fileMenu->addAction(tr("&Quit"), this, &QWidget::close);
    quitAct->setShortcut(QKeySequence::Quit);

    auto *editMenu = menuBar()->addMenu(tr("&Edit"));

    auto *undoAct = editMenu->addAction(tr("&Undo"), m_canvas, &Canvas::undo);
    undoAct->setShortcut(QKeySequence::Undo);
    auto *redoAct = editMenu->addAction(tr("&Redo"), m_canvas, &Canvas::redo);
    redoAct->setShortcut(QKeySequence::Redo);

    editMenu->addSeparator();
    editMenu->addAction(tr("&Clear Canvas"), m_canvas, &Canvas::clear);

    // Keep undo/redo enabled-state in sync with the canvas history.
    auto refresh = [this, undoAct, redoAct]() {
        undoAct->setEnabled(m_canvas->canUndo());
        redoAct->setEnabled(m_canvas->canRedo());
    };
    connect(m_canvas, &Canvas::historyChanged, this, refresh);
    refresh();

    // Image menu: operations on the canvas itself (its dimensions, for now).
    auto *imageMenu = menuBar()->addMenu(tr("&Image"));
    auto *resizeAct = imageMenu->addAction(tr("&Resize Canvas..."),
                                           this, &MainWindow::resizeCanvas);
    resizeAct->setShortcut(QKeySequence(tr("Ctrl+R")));

    auto *viewMenu = menuBar()->addMenu(tr("&View"));

    auto *zoomInAct = viewMenu->addAction(tr("Zoom &In"), m_canvas, &Canvas::zoomIn);
    zoomInAct->setShortcut(QKeySequence::ZoomIn);
    auto *zoomOutAct = viewMenu->addAction(tr("Zoom &Out"), m_canvas, &Canvas::zoomOut);
    zoomOutAct->setShortcut(QKeySequence::ZoomOut);
    auto *actualSizeAct = viewMenu->addAction(tr("&Actual Size"), m_canvas, &Canvas::resetZoom);
    actualSizeAct->setShortcut(QKeySequence(tr("Ctrl+0")));

    viewMenu->addSeparator();

    // The low-key home for the anti-aliasing toggle: a setting most people
    // never touch, so it lives here rather than crowding the tool toolbar.
    // Default unchecked == crisp, MS-Paint-style edges.
    auto *smoothAct = viewMenu->addAction(tr("&Smooth Edges"));
    smoothAct->setCheckable(true);
    smoothAct->setChecked(m_canvas->antialiasing());
    connect(smoothAct, &QAction::toggled, m_canvas, &Canvas::setAntialiasing);

    viewMenu->addSeparator();

    // Pixel-art aids, off by default so casual doodlers get a clean canvas.
    auto *gridAct = viewMenu->addAction(tr("Show &Grid"));
    gridAct->setCheckable(true);
    gridAct->setChecked(m_canvas->gridEnabled());
    connect(gridAct, &QAction::toggled, m_canvas, &Canvas::setGridEnabled);

    viewMenu->addAction(tr("Grid &Size..."), this, [this]() {
        // Purpose-built grid dialog: cell width x height + a 1x1 pixel-grid toggle.
        QDialog dlg(this);
        dlg.setWindowTitle(tr("Grid"));

        const QSize cur = m_canvas->gridSize();
        auto *wSpin = new QSpinBox(&dlg);
        wSpin->setRange(1, 10000); wSpin->setValue(cur.width());  wSpin->setSuffix(tr(" px"));
        auto *hSpin = new QSpinBox(&dlg);
        hSpin->setRange(1, 10000); hSpin->setValue(cur.height()); hSpin->setSuffix(tr(" px"));

        auto *pixelChk = new QCheckBox(tr("Show pixel grid (1×1)"), &dlg);
        pixelChk->setChecked(m_canvas->pixelGridEnabled());
        pixelChk->setToolTip(tr("Also draw a fine 1×1-pixel grid in a lighter "
                                "shade (visible when zoomed in)."));

        auto *form = new QFormLayout;
        form->addRow(tr("Cell width:"), wSpin);
        form->addRow(tr("Cell height:"), hSpin);

        auto *buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

        auto *lay = new QVBoxLayout(&dlg);
        lay->addLayout(form);
        lay->addWidget(pixelChk);
        lay->addWidget(buttons);

        if (dlg.exec() == QDialog::Accepted) {
            m_canvas->setGridSize(wSpin->value(), hSpin->value());
            m_canvas->setPixelGridEnabled(pixelChk->isChecked());
        }
    });

    auto *rulersAct = viewMenu->addAction(tr("Show &Rulers"));
    rulersAct->setCheckable(true);
    rulersAct->setChecked(m_rulersVisible);
    connect(rulersAct, &QAction::toggled, this, &MainWindow::setRulersVisible);
}

// ---------------------------------------------------------------------------
// Tool toolbar
// ---------------------------------------------------------------------------

void MainWindow::createToolbar()
{
    auto *bar = addToolBar(tr("Tools"));
    bar->setMovable(false);
    // Show icons only; the text label survives as the tooltip / accessible name.
    bar->setToolButtonStyle(Qt::ToolButtonIconOnly);

    m_toolGroup = new QActionGroup(this);
    m_toolGroup->setExclusive(true);

    // Helper to add a checkable tool button mapped to a Canvas::Tool.
    //
    // `iconName` is a freedesktop/KDE theme icon name. We pull it from the
    // user's icon theme via QIcon::fromTheme so the buttons look native and
    // follow the system (and light/dark) theme. If a theme happens not to
    // provide that name, the icon comes back null and we fall back to showing
    // the text label, so the button is never blank.
    auto addTool = [&](const QString &label, const QString &iconName,
                       Canvas::Tool tool, bool checked = false) {
        QAction *act = bar->addAction(label);
        act->setCheckable(true);
        act->setChecked(checked);
        act->setToolTip(label);
        const QIcon icon = QIcon::fromTheme(iconName);
        if (!icon.isNull())
            act->setIcon(icon);
        else
            act->setText(label);   // graceful fallback: keep the words
        m_toolGroup->addAction(act);
        connect(act, &QAction::triggered, this, [this, tool]() {
            m_canvas->setTool(tool);
            // Font controls are relevant only to the Text tool.
            if (m_fontControlsAction)
                m_fontControlsAction->setVisible(tool == Canvas::Tool::Text);
            // The fill toggle applies to the closed shape tools (rect/ellipse)
            // and the polygon.
            if (m_fillControlsAction)
                m_fillControlsAction->setVisible(tool == Canvas::Tool::Rectangle ||
                                                 tool == Canvas::Tool::Ellipse ||
                                                 tool == Canvas::Tool::Polygon);
        });
    };

    addTool(tr("Pencil"),  QStringLiteral("draw-freehand"),  Canvas::Tool::Pencil, true);
    addTool(tr("Eraser"),  QStringLiteral("draw-eraser"),    Canvas::Tool::Eraser);
    addTool(tr("Spray"),   QStringLiteral("tool-spray"),     Canvas::Tool::Spray);
    addTool(tr("Line"),    QStringLiteral("draw-line"),      Canvas::Tool::Line);
    addTool(tr("Rect"),    QStringLiteral("draw-rectangle"), Canvas::Tool::Rectangle);
    addTool(tr("Ellipse"), QStringLiteral("draw-ellipse"),   Canvas::Tool::Ellipse);
    addTool(tr("Polygon"), QStringLiteral("draw-polyline"),  Canvas::Tool::Polygon);
    addTool(tr("Fill"),    QStringLiteral("fill-color"),     Canvas::Tool::Fill);
    addTool(tr("Picker"),  QStringLiteral("color-picker"),   Canvas::Tool::Picker);
    addTool(tr("Text"),    QStringLiteral("insert-text"),    Canvas::Tool::Text);

    bar->addSeparator();

    // Brush size spinner.
    bar->addWidget(new QLabel(tr(" Size: ")));
    auto *sizeSpin = new QSpinBox(this);
    sizeSpin->setRange(1, 64);
    sizeSpin->setValue(m_canvas->brushSize());
    connect(sizeSpin, qOverload<int>(&QSpinBox::valueChanged),
            m_canvas, &Canvas::setBrushSize);
    bar->addWidget(sizeSpin);

    // Text font size, shown ONLY when the Text tool is active (like classic
    // Paint's Fonts toolbar that appeared in text mode). Label + spinbox live in
    // one container so a single QAction toggles both.
    auto *fontBox = new QWidget(this);
    auto *fontLay = new QHBoxLayout(fontBox);
    fontLay->setContentsMargins(6, 0, 0, 0);
    fontLay->setSpacing(4);
    fontLay->addWidget(new QLabel(tr("Font:")));
    auto *fontSpin = new QSpinBox(fontBox);
    fontSpin->setRange(4, 200);
    fontSpin->setValue(16);
    fontSpin->setSuffix(tr(" pt"));
    fontSpin->setToolTip(tr("Text font size"));
    connect(fontSpin, qOverload<int>(&QSpinBox::valueChanged),
            m_canvas, &Canvas::setFontPointSize);
    fontLay->addWidget(fontSpin);

    m_fontControlsAction = bar->addWidget(fontBox);
    m_fontControlsAction->setVisible(false);   // hidden until Text tool chosen

    // Shape-fill toggle, shown ONLY for the closed shape tools (rect/ellipse).
    // When checked, the shape's interior is filled: border = the slot of the
    // button you drag with, fill = the other slot (Color 1 border / Color 2 fill
    // on left-drag; swapped on right-drag). Lines have no interior, so it hides.
    auto *fillCheck = new QCheckBox(tr("Fill"), this);
    fillCheck->setToolTip(tr("Fill the shape's interior.\n"
                             "Border = drag button's colour, fill = the other."));
    connect(fillCheck, &QCheckBox::toggled, m_canvas, &Canvas::setShapeFill);
    m_fillControlsAction = bar->addWidget(fillCheck);
    m_fillControlsAction->setVisible(false);   // hidden until a shape tool chosen

    // Zoom controls live in the bottom-right status bar (classic Paint style),
    // built in createStatusBar() -- not here in the tool toolbar.
}

// ---------------------------------------------------------------------------
// Colour palette (a docked strip of swatches + current-colour indicator)
// ---------------------------------------------------------------------------

void MainWindow::createPalette()
{
    auto *bar = addToolBar(tr("Colours"));
    bar->setMovable(false);
    addToolBarBreak();   // put the palette on its own row under the tools

    // Two overlapping swatches, classic-Paint style: the primary (Color 1) sits
    // in front and slightly up-left, the secondary (Color 2) peeks out behind.
    // They're children of a small fixed-size container, free-positioned. Clicking
    // anywhere on the container swaps the two colours (handled via an event filter
    // on m_swatchBox in eventFilter(), since QWidget has no clicked signal).
    m_swatchBox = new QWidget(this);
    m_swatchBox->setFixedSize(40, 32);
    m_swatchBox->setCursor(Qt::PointingHandCursor);
    m_swatchBox->setToolTip(tr("Click to swap Color 1 and Color 2"));
    m_swatchBox->installEventFilter(this);

    m_secondarySwatch = new QLabel(m_swatchBox);
    m_secondarySwatch->setGeometry(14, 10, 20, 20);
    m_secondarySwatch->setFrameShape(QFrame::Box);
    // The labels sit on top of the box, so let clicks fall through to the box.
    m_secondarySwatch->setAttribute(Qt::WA_TransparentForMouseEvents);

    m_primarySwatch = new QLabel(m_swatchBox);
    m_primarySwatch->setGeometry(2, 0, 20, 20);
    m_primarySwatch->setFrameShape(QFrame::Box);
    m_primarySwatch->setAttribute(Qt::WA_TransparentForMouseEvents);

    bar->addWidget(m_swatchBox);
    bar->addSeparator();

    // The colour box proper: two stacked rows in a tight grid, classic-Paint
    // style. Both rows are fully editable (double-click a slot). The top row
    // seeds from the built-in palette; the bottom row starts all-white and also
    // collects colours mixed via "Edit Colours...". Both rows share a column
    // count so they line up vertically.
    const int cols = kPalette.size();

    auto *grid = new QWidget(this);
    auto *g = new QGridLayout(grid);
    g->setContentsMargins(0, 0, 0, 0);
    g->setHorizontalSpacing(2);
    g->setVerticalSpacing(2);

    // Row 0: presets, seeded from kPalette. Row 1: custom, all white.
    m_presetColors = QVector<QColor>(kPalette.cbegin(), kPalette.cend());
    m_customColors = QVector<QColor>(cols, QColor(Qt::white));
    buildColorRow(g, /*gridRow*/ 0, /*row*/ 0, m_presetColors, m_presetSlots);
    buildColorRow(g, /*gridRow*/ 1, /*row*/ 1, m_customColors, m_customSlots);

    // The rainbow swatch: a special slot at the end of the colour box that
    // reviving MS Paint's old rainbow brush. It isn't an editable colour -- it's
    // a mode. Left/right-click sets Color 1 / Color 2 to "rainbow", and any tool
    // using that slot then cycles the spectrum. It spans both rows (a 2x20 tile)
    // so it reads as one distinct special swatch at the end of the strip.
    m_rainbowSwatch = new QPushButton(grid);
    m_rainbowSwatch->setFixedSize(20, 42);
    m_rainbowSwatch->setCursor(Qt::PointingHandCursor);
    m_rainbowSwatch->setToolTip(tr("Rainbow brush: cycles the spectrum as you "
                                   "draw.\nLeft: Color 1   Right: Color 2"));
    m_rainbowSwatch->setStyleSheet(rainbowStyleSheet());
    m_rainbowSwatch->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_rainbowSwatch, &QPushButton::clicked,
            this, &MainWindow::setPrimaryRainbow);
    connect(m_rainbowSwatch, &QPushButton::customContextMenuRequested,
            this, &MainWindow::setSecondaryRainbow);
    g->addWidget(m_rainbowSwatch, /*row*/ 0, /*col*/ cols, /*rowSpan*/ 2, 1);

    bar->addWidget(grid);

    bar->addSeparator();
    // Palette-glyph button (was a text "Edit Colours..."). `color-management` is
    // the Breeze actions icon that reads as a swatch palette and ships at 16/22/32
    // for light + dark; if the theme ever lacks it, fall back to the text label so
    // the button never disappears (a bare icon-less QPushButton would be invisible).
    auto *more = new QPushButton(this);
    const QIcon paletteIcon = QIcon::fromTheme(QStringLiteral("color-management"));
    if (paletteIcon.isNull())
        more->setText(tr("Edit Colours..."));
    else
        more->setIcon(paletteIcon);
    more->setToolTip(tr("Edit Colours: mix a custom colour (added to the custom row)"));
    connect(more, &QPushButton::clicked, this, &MainWindow::chooseColor);
    bar->addWidget(more);
}

// Build one row of colour slots. Click selects (left=Color 1, right=Color 2).
// Editing colours is no longer a per-swatch gesture here; it all happens in the
// "Edit Colours..." dialog, which shows both rows as labelled editable grids.
// The lambdas capture `row`/`i` so they always read the *current* stored colour.
void MainWindow::buildColorRow(QGridLayout *grid, int gridRow, int row,
                               QVector<QColor> &colors,
                               QVector<QPushButton *> &buttons)
{
    buttons.clear();
    for (int i = 0; i < colors.size(); ++i) {
        auto *btn = new QPushButton(grid->parentWidget());
        btn->setFixedSize(20, 20);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setToolTip(tr("Left: Color 1   Right: Color 2"));
        btn->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(btn, &QPushButton::clicked, this, [this, row, i]() {
            setPrimaryColor(row == 0 ? m_presetColors[i] : m_customColors[i]);
        });
        connect(btn, &QPushButton::customContextMenuRequested, this,
                [this, row, i]() {
                    setSecondaryColor(row == 0 ? m_presetColors[i]
                                               : m_customColors[i]);
                });
        buttons.append(btn);
        grid->addWidget(btn, gridRow, i);
        paintSlot(row, i);
    }
}

// Paint a slot button to reflect its stored colour.
void MainWindow::paintSlot(int row, int index)
{
    QVector<QPushButton *> &buttons = (row == 0) ? m_presetSlots : m_customSlots;
    QVector<QColor> &colors = (row == 0) ? m_presetColors : m_customColors;
    if (index < 0 || index >= buttons.size())
        return;
    buttons[index]->setStyleSheet(
        QString("background-color: %1; border: 1px solid #555;")
            .arg(colors.value(index).name()));
}

// ---------------------------------------------------------------------------
// Status bar with the classic-Paint zoom cluster, pinned bottom-right:
//   [-] ====O==== [+]   300%
// ---------------------------------------------------------------------------

// The slider is linear but zoom is multiplicative, so we map through log-space:
// equal slider travel == equal zoom *ratio*. The slider runs 0..1000 across the
// canvas's full 0.1x..32x range.
static constexpr double kZoomMin = 0.1;
static constexpr double kZoomMax = 32.0;
static constexpr int    kSliderMax = 1000;

int MainWindow::zoomToSlider(double factor) const
{
    const double t = (std::log(factor)      - std::log(kZoomMin))
                   / (std::log(kZoomMax)    - std::log(kZoomMin));
    return qBound(0, int(qRound(t * kSliderMax)), kSliderMax);
}

double MainWindow::sliderToZoom(int value) const
{
    const double t = double(value) / kSliderMax;
    return std::exp(std::log(kZoomMin) + t * (std::log(kZoomMax) - std::log(kZoomMin)));
}

void MainWindow::createStatusBar()
{
    auto *sb = statusBar();
    sb->setSizeGripEnabled(false);

    // Cursor coordinate readout, pinned to the left. Fixed width (sized to the
    // widest plausible "9999, 9999") so the readout doesn't jitter as you move.
    m_coordLabel = new QLabel(this);
    {
        const int w = m_coordLabel->fontMetrics().horizontalAdvance(
                          QStringLiteral("9999, 9999")) + 12;
        m_coordLabel->setMinimumWidth(w);
    }
    m_coordLabel->setToolTip(tr("Cursor position (image pixels)"));
    sb->addWidget(m_coordLabel);   // addWidget == left-aligned (non-permanent)

    connect(m_canvas, &Canvas::cursorPositionChanged, this, [this](const QPoint &p) {
        m_coordLabel->setText(QStringLiteral("%1, %2").arg(p.x()).arg(p.y()));
    });
    connect(m_canvas, &Canvas::cursorLeft, this, [this]() {
        m_coordLabel->clear();
    });

    // Build the cluster in a container so it sits as one unit on the right.
    auto *zoomWidget = new QWidget(this);
    auto *lay = new QHBoxLayout(zoomWidget);
    lay->setContentsMargins(0, 0, 6, 0);
    lay->setSpacing(4);

    auto iconButton = [&](const QString &iconName, const QString &fallback,
                          const QString &tip) {
        auto *b = new QToolButton(zoomWidget);
        const QIcon icon = QIcon::fromTheme(iconName);
        if (!icon.isNull())
            b->setIcon(icon);
        else
            b->setText(fallback);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        return b;
    };

    auto *minus = iconButton(QStringLiteral("zoom-out"), QStringLiteral("−"),
                             tr("Zoom out"));
    connect(minus, &QToolButton::clicked, m_canvas, &Canvas::zoomOut);

    m_zoomSlider = new QSlider(Qt::Horizontal, zoomWidget);
    m_zoomSlider->setRange(0, kSliderMax);
    m_zoomSlider->setValue(zoomToSlider(m_canvas->zoom()));
    m_zoomSlider->setFixedWidth(140);
    m_zoomSlider->setToolTip(tr("Zoom"));

    auto *plus = iconButton(QStringLiteral("zoom-in"), QStringLiteral("+"),
                            tr("Zoom in"));
    connect(plus, &QToolButton::clicked, m_canvas, &Canvas::zoomIn);

    m_zoomLabel = new QLabel(tr("100%"), zoomWidget);
    // Fixed width sized to the widest possible readout ("3200%"), so the slider
    // to its left never shuffles as the digit count changes. Measured from the
    // label's own font rather than hard-coding pixels, so it stays correct at
    // any font/DPI.
    {
        const QString widest = QStringLiteral("3200%");
        const int w = m_zoomLabel->fontMetrics().horizontalAdvance(widest) + 6;
        m_zoomLabel->setFixedWidth(w);
    }
    m_zoomLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    // Push the whole cluster flush to the right edge.
    lay->addStretch(1);
    lay->addWidget(minus);
    lay->addWidget(m_zoomSlider);
    lay->addWidget(plus);
    lay->addWidget(m_zoomLabel);

    sb->addPermanentWidget(zoomWidget);   // permanent == pinned to the right

    // Slider drag -> set canvas zoom. Guarded so the resulting zoomChanged
    // signal doesn't bounce back and fight the user's drag.
    connect(m_zoomSlider, &QSlider::valueChanged, this, [this](int v) {
        if (m_syncingZoom)
            return;
        m_syncingZoom = true;
        m_canvas->setZoom(sliderToZoom(v));
        m_syncingZoom = false;
    });

    // Canvas zoom changed (wheel, menu, buttons) -> move slider + update %.
    connect(m_canvas, &Canvas::zoomChanged, this, [this](double f) {
        m_zoomLabel->setText(QStringLiteral("%1%").arg(qRound(f * 100)));
        if (m_syncingZoom)
            return;
        m_syncingZoom = true;
        m_zoomSlider->setValue(zoomToSlider(f));
        m_syncingZoom = false;
    });
}

// ---------------------------------------------------------------------------
// Rulers: two thin widgets in the scroll area's viewport margins (the standard
// Qt "gutter" pattern). They don't scroll themselves; we feed them the scrollbar
// offset, the zoom, the document size, and the cursor position, and they paint.
// ---------------------------------------------------------------------------

void MainWindow::setupRulers()
{
    m_hRuler = new Ruler(Ruler::Orientation::Horizontal, m_scroll);
    m_vRuler = new Ruler(Ruler::Orientation::Vertical, m_scroll);

    // Reposition the rulers whenever the viewport resizes.
    m_scroll->viewport()->installEventFilter(this);

    // Scroll -> recompute the rulers' origin from the canvas's true viewport
    // position (refreshRulerMetrics maps it through; the scrollbar value alone
    // would miss the centring gap at low zoom).
    connect(m_scroll->horizontalScrollBar(), &QScrollBar::valueChanged,
            this, [this](int) { refreshRulerMetrics(); });
    connect(m_scroll->verticalScrollBar(), &QScrollBar::valueChanged,
            this, [this](int) { refreshRulerMetrics(); });

    // Zoom or document-size changes -> refresh both rulers' scale/length.
    connect(m_canvas, &Canvas::zoomChanged, this,
            [this](double) { refreshRulerMetrics(); });

    // Live cursor marker on both rulers (and clear it when the pointer leaves).
    // Uses the PRECISE float position so the marker tracks smoothly at low zoom.
    connect(m_canvas, &Canvas::cursorPositionChangedF, this, [this](const QPointF &p) {
        m_hRuler->setCursorImagePos(p.x(), true);
        m_vRuler->setCursorImagePos(p.y(), true);
    });
    connect(m_canvas, &Canvas::cursorLeft, this, [this]() {
        m_hRuler->setCursorImagePos(0.0, false);
        m_vRuler->setCursorImagePos(0.0, false);
    });

    refreshRulerMetrics();
    setRulersVisible(m_rulersVisible);   // hidden initially
}

void MainWindow::layoutRulers()
{
    if (!m_hRuler || !m_vRuler)
        return;
    const int t = Ruler::kThickness;
    const QRect vp = m_scroll->viewport()->geometry();   // inside the margins

    // The corner square (t x t) sits top-left; the horizontal ruler spans the
    // width to its right, the vertical ruler spans the height below it.
    if (m_rulersVisible) {
        m_hRuler->setGeometry(vp.left(), vp.top() - t, vp.width(), t);
        m_vRuler->setGeometry(vp.left() - t, vp.top(), t, vp.height());
    }
}

void MainWindow::setRulersVisible(bool on)
{
    m_rulersVisible = on;
    const int t = on ? Ruler::kThickness : 0;
    // Reserve space at the top/left of the viewport for the rulers.
    m_scroll->setRulerMargins(t, t);
    m_hRuler->setVisible(on);
    m_vRuler->setVisible(on);
    if (on) {
        refreshRulerMetrics();
        layoutRulers();
    }
}

void MainWindow::refreshRulerMetrics()
{
    if (!m_hRuler || !m_vRuler)
        return;
    const double z = m_canvas->zoom();
    const QSize sz = m_canvas->imageSize();
    m_hRuler->setZoom(z);
    m_vRuler->setZoom(z);
    m_hRuler->setDocLength(sz.width());
    m_vRuler->setDocLength(sz.height());

    // Feed the rulers the TRUE on-screen position of image pixel (0,0), measured
    // from the canvas widget's actual placement in the viewport. This captures
    // BOTH the scrollbar offset AND the scroll area's centring gap (which appears
    // when the zoomed widget is smaller than the viewport, the scrollbar value
    // stays 0 then, so reading the scrollbar alone undercounts the offset and the
    // ruler drifts below ~50% zoom). Mapping the widget's image-origin point to
    // the viewport is correct in every case.
    const QPoint imgOriginInViewport =
        m_canvas->mapTo(m_scroll->viewport(), m_canvas->canvasOriginPoint());
    m_hRuler->setOrigin(imgOriginInViewport.x());
    m_vRuler->setOrigin(imgOriginInViewport.y());
    m_hRuler->setScroll(0);   // offset is fully captured by origin now
    m_vRuler->setScroll(0);
}

// Scroll so the image sits centred in the viewport. Needed because the overscroll
// padding makes the widget larger than the image, so the default top-left scroll
// position would leave the canvas down-and-right, partly off-screen.
void MainWindow::centerView()
{
    const double z   = m_canvas->zoom();
    const int    pad = m_canvas->scrollPadding();
    const QSize  img = m_canvas->imageSize();
    const QSize  vp  = m_scroll->viewport()->size();

    // Image centre in widget coords, minus half the viewport, clamped by the bars.
    const int cx = int(pad + img.width()  * z / 2.0 - vp.width()  / 2.0);
    const int cy = int(pad + img.height() * z / 2.0 - vp.height() / 2.0);
    m_scroll->horizontalScrollBar()->setValue(cx);
    m_scroll->verticalScrollBar()->setValue(cy);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_scroll->viewport() && event->type() == QEvent::Resize) {
        // Overscroll padding scales with the viewport, so recompute the canvas
        // layout, then reposition the rulers and refresh their metrics.
        m_canvas->relayout();
        layoutRulers();
        refreshRulerMetrics();
    } else if (watched == m_swatchBox
               && event->type() == QEvent::MouseButtonRelease) {
        // Click the current-colour indicator to swap Color 1 and Color 2.
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton) {
            swapColors();
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::setPrimaryColor(const QColor &c)
{
    m_primaryColor = c;
    m_primaryRainbow = false;           // picking a flat colour leaves rainbow
    m_canvas->setPrimaryColor(c);
    m_canvas->setPrimaryRainbow(false);
    refreshColorSwatches();
}

void MainWindow::setSecondaryColor(const QColor &c)
{
    m_secondaryColor = c;
    m_secondaryRainbow = false;
    m_canvas->setSecondaryColor(c);
    m_canvas->setSecondaryRainbow(false);
    refreshColorSwatches();
}

void MainWindow::setPrimaryRainbow()
{
    m_primaryRainbow = true;
    m_canvas->setPrimaryRainbow(true);
    refreshColorSwatches();
}

void MainWindow::setSecondaryRainbow()
{
    m_secondaryRainbow = true;
    m_canvas->setSecondaryRainbow(true);
    refreshColorSwatches();
}

// A left-to-right spectrum, used for the palette swatch and for the indicator
// swatches when a slot is in rainbow mode.
QString MainWindow::rainbowStyleSheet()
{
    return QStringLiteral(
        "border: 1px solid #000; background: qlineargradient("
        "x1:0, y1:0, x2:1, y2:0,"
        " stop:0 #ff0000, stop:0.17 #ffff00, stop:0.33 #00ff00,"
        " stop:0.5 #00ffff, stop:0.67 #0000ff, stop:0.83 #ff00ff,"
        " stop:1 #ff0000);");
}

void MainWindow::refreshColorSwatches()
{
    if (m_primarySwatch)
        m_primarySwatch->setStyleSheet(
            m_primaryRainbow ? rainbowStyleSheet()
                             : QString("background-color: %1; border: 1px solid #000;")
                                   .arg(m_primaryColor.name()));
    if (m_secondarySwatch)
        m_secondarySwatch->setStyleSheet(
            m_secondaryRainbow ? rainbowStyleSheet()
                               : QString("background-color: %1; border: 1px solid #000;")
                                     .arg(m_secondaryColor.name()));
}

void MainWindow::chooseColor()
{
    // The canvas colour box is two visual rows; the editor treats them as one
    // flat list laid out at the same column count, so its grid mirrors the
    // canvas. Combine, edit, then split the result back into the two rows.
    const int cols = m_presetColors.size();   // colours per canvas row
    QVector<QColor> combined = m_presetColors;
    combined += m_customColors;

    ColorEditor dlg(combined, cols, m_primaryColor, this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    const QVector<QColor> edited = dlg.colors();
    for (int i = 0; i < m_presetColors.size() && i < edited.size(); ++i)
        m_presetColors[i] = edited[i];
    for (int i = 0; i < m_customColors.size() && (cols + i) < edited.size(); ++i)
        m_customColors[i] = edited[cols + i];

    for (int i = 0; i < m_presetSlots.size(); ++i)
        paintSlot(/*row*/ 0, i);
    for (int i = 0; i < m_customSlots.size(); ++i)
        paintSlot(/*row*/ 1, i);

    const QColor c = dlg.selectedColor();
    if (c.isValid())
        setPrimaryColor(c);
}

// Swap Color 1 and Color 2 in place, carrying the rainbow flag with each slot.
// Triggered by clicking the overlapping current-colour indicator.
void MainWindow::swapColors()
{
    std::swap(m_primaryColor, m_secondaryColor);
    std::swap(m_primaryRainbow, m_secondaryRainbow);

    // Push both slots back to the canvas. setPrimary/SecondaryRainbow and
    // setPrimary/SecondaryColor each also refresh the swatches, but call them
    // explicitly so the canvas's rainbow mode tracks the flags exactly.
    if (m_primaryRainbow) setPrimaryRainbow();   else setPrimaryColor(m_primaryColor);
    if (m_secondaryRainbow) setSecondaryRainbow(); else setSecondaryColor(m_secondaryColor);
}

void MainWindow::onColorPicked(const QColor &c)
{
    if (c.isValid())
        setPrimaryColor(c);
}

void MainWindow::onSecondaryColorPicked(const QColor &c)
{
    if (c.isValid())
        setSecondaryColor(c);
}

// ---------------------------------------------------------------------------
// File operations
// ---------------------------------------------------------------------------

// Modal width x height prompt shared by New and Resize. Two spinboxes in a small
// dialog (clearer than chaining two QInputDialogs). Returns invalid on cancel.
QSize MainWindow::promptForSize(const QString &title, const QSize &initial)
{
    QDialog dlg(this);
    dlg.setWindowTitle(title);

    auto *wSpin = new QSpinBox(&dlg);
    wSpin->setRange(1, 10000);
    wSpin->setValue(initial.width());
    wSpin->setSuffix(tr(" px"));
    auto *hSpin = new QSpinBox(&dlg);
    hSpin->setRange(1, 10000);
    hSpin->setValue(initial.height());
    hSpin->setSuffix(tr(" px"));

    auto *form = new QFormLayout;
    form->addRow(tr("Width:"), wSpin);
    form->addRow(tr("Height:"), hSpin);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    auto *lay = new QVBoxLayout(&dlg);
    lay->addLayout(form);
    lay->addWidget(buttons);

    wSpin->setFocus();
    wSpin->selectAll();

    if (dlg.exec() != QDialog::Accepted)
        return QSize();   // invalid -> cancelled
    return QSize(wSpin->value(), hSpin->value());
}

void MainWindow::newFile()
{
    if (!maybeSave())
        return;
    const QSize size = promptForSize(tr("New Image"), m_lastNewSize);
    if (!size.isValid())
        return;                 // cancelled -> keep the current canvas
    m_lastNewSize = size;       // remember for next time
    m_canvas->newImage(size);
    m_currentPath.clear();
    updateTitle();
    refreshRulerMetrics();
    centerView();
}

void MainWindow::resizeCanvas()
{
    const QSize size = promptForSize(tr("Resize Canvas"), m_canvas->imageSize());
    if (!size.isValid())
        return;
    m_canvas->resizeCanvas(size);
    updateTitle();
    refreshRulerMetrics();
}

void MainWindow::openFile()
{
    if (!maybeSave())
        return;
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open Image"), QString(),
        tr("Images (*.png *.jpg *.jpeg *.bmp)"));
    if (path.isEmpty())
        return;
    if (!m_canvas->openImage(path)) {
        QMessageBox::warning(this, tr("Open failed"),
                             tr("Could not open %1").arg(path));
        return;
    }
    m_currentPath = path;
    updateTitle();
    refreshRulerMetrics();
    centerView();
}

bool MainWindow::saveFile()
{
    if (m_currentPath.isEmpty())
        return saveFileAs();
    if (!m_canvas->saveImage(m_currentPath)) {
        QMessageBox::warning(this, tr("Save failed"),
                             tr("Could not save %1").arg(m_currentPath));
        return false;
    }
    updateTitle();
    return true;
}

bool MainWindow::saveFileAs()
{
    QString path = QFileDialog::getSaveFileName(
        this, tr("Save Image"), QString(), tr("PNG image (*.png)"));
    if (path.isEmpty())
        return false;
    if (!path.endsWith(".png", Qt::CaseInsensitive))
        path += ".png";
    if (!m_canvas->saveImage(path)) {
        QMessageBox::warning(this, tr("Save failed"),
                             tr("Could not save %1").arg(path));
        return false;
    }
    m_currentPath = path;
    updateTitle();
    return true;
}

// ---------------------------------------------------------------------------
// Dirty-state handling
// ---------------------------------------------------------------------------

bool MainWindow::maybeSave()
{
    if (!m_canvas->isModified())
        return true;
    const auto ret = QMessageBox::warning(
        this, QApplication::applicationName(),
        tr("The image has unsaved changes.\nDo you want to save them?"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    switch (ret) {
    case QMessageBox::Save:    return saveFile();
    case QMessageBox::Discard: return true;
    default:                   return false;   // Cancel
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (maybeSave())
        event->accept();
    else
        event->ignore();
}

void MainWindow::updateTitle()
{
    const QString name = m_currentPath.isEmpty()
                             ? tr("Untitled")
                             : QFileInfo(m_currentPath).fileName();
    const QString star = m_canvas->isModified() ? "*" : "";
    setWindowTitle(QString("%1%2 - %3")
                       .arg(name, star, QApplication::applicationName()));
}
