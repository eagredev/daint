// Daint: a simple MS-Paint-style raster paint program.
// Copyright (C) 2026  eagre.dev
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. This program is distributed WITHOUT ANY WARRANTY; see the GNU General
// Public License (LICENSE file) for details.

#include "coloreditor.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDialogButtonBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusArgument>

// ===========================================================================
// HueSatField
// ===========================================================================

HueSatField::HueSatField(QWidget *parent) : QWidget(parent)
{
    setCursor(Qt::CrossCursor);
}

void HueSatField::setHueSat(int hue, int sat)
{
    m_hue = qBound(0, hue, 359);
    m_sat = qBound(0, sat, 255);
    update();
}

void HueSatField::resizeEvent(QResizeEvent *)
{
    rebuildCache();
}

// Cache the H/S gradient as an image so we don't recompute per-pixel every
// repaint (only on resize). X = hue across the width, Y = saturation top-down.
void HueSatField::rebuildCache()
{
    const int w = qMax(1, width());
    const int h = qMax(1, height());
    m_cache = QImage(w, h, QImage::Format_RGB32);
    for (int y = 0; y < h; ++y) {
        const int sat = 255 - (y * 255 / (h - 1 > 0 ? h - 1 : 1));
        QRgb *line = reinterpret_cast<QRgb *>(m_cache.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const int hue = x * 359 / (w - 1 > 0 ? w - 1 : 1);
            line[x] = QColor::fromHsv(hue, sat, 255).rgb();
        }
    }
}

void HueSatField::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    if (m_cache.size() != size())
        rebuildCache();
    p.drawImage(0, 0, m_cache);

    // Marker: a small ring at the current hue/sat position.
    const int x = m_hue * (width() - 1) / 359;
    const int y = (255 - m_sat) * (height() - 1) / 255;
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(Qt::black, 1.5));
    p.drawEllipse(QPointF(x, y), 5, 5);
    p.setPen(QPen(Qt::white, 1.5));
    p.drawEllipse(QPointF(x, y), 4, 4);
}

void HueSatField::pickAt(const QPoint &pos)
{
    const int x = qBound(0, pos.x(), width() - 1);
    const int y = qBound(0, pos.y(), height() - 1);
    m_hue = x * 359 / (width() - 1 > 0 ? width() - 1 : 1);
    m_sat = 255 - (y * 255 / (height() - 1 > 0 ? height() - 1 : 1));
    update();
    emit hueSatChanged(m_hue, m_sat);
}

void HueSatField::mousePressEvent(QMouseEvent *e) { pickAt(e->pos()); }
void HueSatField::mouseMoveEvent(QMouseEvent *e)
{
    if (e->buttons() & Qt::LeftButton)
        pickAt(e->pos());
}

// ===========================================================================
// ValueBar
// ===========================================================================

ValueBar::ValueBar(QWidget *parent) : QWidget(parent)
{
    setCursor(Qt::PointingHandCursor);
}

void ValueBar::setHueSat(int hue, int sat)
{
    m_hue = hue;
    m_sat = sat;
    update();
}

void ValueBar::setValue(int v)
{
    m_value = qBound(0, v, 255);
    update();
}

void ValueBar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    // Vertical gradient: full value at the top, black at the bottom, in the
    // current hue/sat.
    QLinearGradient grad(0, 0, 0, height());
    grad.setColorAt(0.0, QColor::fromHsv(m_hue, m_sat, 255));
    grad.setColorAt(1.0, QColor::fromHsv(m_hue, m_sat, 0));
    p.fillRect(rect(), grad);
    p.setPen(QPen(Qt::gray, 1));
    p.drawRect(rect().adjusted(0, 0, -1, -1));

    // Marker: a horizontal bar at the current value.
    const int y = (255 - m_value) * (height() - 1) / 255;
    p.setPen(QPen(Qt::black, 1));
    p.drawLine(0, y, width(), y);
    p.setPen(QPen(Qt::white, 1));
    p.drawLine(0, y - 1, width(), y - 1);
}

void ValueBar::pickAt(const QPoint &pos)
{
    const int y = qBound(0, pos.y(), height() - 1);
    m_value = 255 - (y * 255 / (height() - 1 > 0 ? height() - 1 : 1));
    update();
    emit valueChanged(m_value);
}

void ValueBar::mousePressEvent(QMouseEvent *e) { pickAt(e->pos()); }
void ValueBar::mouseMoveEvent(QMouseEvent *e)
{
    if (e->buttons() & Qt::LeftButton)
        pickAt(e->pos());
}

// ===========================================================================
// ColorEditor
// ===========================================================================

namespace {
constexpr int kSwatch = 22;       // palette swatch button edge, px
}

ColorEditor::ColorEditor(const QVector<QColor> &colors, int columns,
                         const QColor &initial, QWidget *parent)
    : QDialog(parent), m_colors(colors), m_columns(qMax(1, columns))
{
    setWindowTitle(tr("Edit Colours"));
    setFocusPolicy(Qt::StrongFocus);   // so the dialog itself can hold key focus

    auto *outer = new QVBoxLayout(this);
    outer->addWidget(new QLabel(tr("Colors"), this));
    outer->addWidget(buildGrid(columns));

    auto *line = new QFrame(this);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    outer->addWidget(line);

    outer->addWidget(buildPicker());

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                         QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // The "Pick from screen" eyedropper sits on the bottom row, but hard-left
    // (under the picker) rather than clustered against OK/Cancel. A QDialogButtonBox
    // ActionRole would pack it tight beside them, so build the row by hand: button,
    // stretch, then the OK/Cancel box pushed to the right.
    auto *pick = new QPushButton(tr("Pick from screen"), this);
    pick->setIcon(QIcon::fromTheme(QStringLiteral("color-picker")));
    pick->setToolTip(tr("Eyedrop a colour from anywhere on screen"));
    pick->setFocusPolicy(Qt::ClickFocus);
    connect(pick, &QPushButton::clicked, this, &ColorEditor::pickFromScreen);

    auto *bottomRow = new QHBoxLayout;
    bottomRow->setContentsMargins(0, 0, 0, 0);
    bottomRow->addWidget(pick);
    bottomRow->addStretch(1);
    bottomRow->addWidget(buttons);
    outer->addLayout(bottomRow);
    // The OK/Cancel buttons take click focus only, so they don't grab keyboard
    // focus on open (which would steal the arrow keys for button navigation).
    for (QAbstractButton *b : buttons->buttons())
        b->setFocusPolicy(Qt::ClickFocus);

    // Catch arrow keys before any focused child consumes them. Filtering at the
    // application level is the only reliable way to intercept keys regardless of
    // which widget (square, swatch, button) currently holds focus; eventFilter
    // ignores everything that isn't a key for one of our own widgets.
    qApp->installEventFilter(this);

    // Select the swatch matching `initial`, if any; else the first.
    int idx = -1;
    for (int i = 0; i < m_colors.size(); ++i)
        if (m_colors[i] == initial) { idx = i; break; }
    selectSwatch(idx >= 0 ? idx : 0);

    // Give the dialog itself keyboard focus on open so the very first arrow key
    // navigates slots rather than the button box.
    setFocus();
}

ColorEditor::~ColorEditor()
{
    if (qApp)
        qApp->removeEventFilter(this);
}

QWidget *ColorEditor::buildGrid(int columns)
{
    auto *host = new QWidget(this);
    auto *g = new QGridLayout(host);
    g->setContentsMargins(0, 0, 0, 0);
    g->setSpacing(3);
    m_btns.clear();
    for (int i = 0; i < m_colors.size(); ++i) {
        auto *b = new QPushButton(host);
        b->setFixedSize(kSwatch, kSwatch);
        b->setCursor(Qt::PointingHandCursor);
        // Click focus only: our own highlight border shows the selection, so we
        // don't want Qt's focus ring hopping between swatches on arrow keys (it
        // would compete with (and visually contradict) m_selIndex).
        b->setFocusPolicy(Qt::ClickFocus);
        connect(b, &QPushButton::clicked, this, [this, i]() { selectSwatch(i); });
        m_btns.append(b);
        g->addWidget(b, i / columns, i % columns);
    }
    for (int i = 0; i < m_colors.size(); ++i)
        paintSwatch(i);
    return host;
}

QWidget *ColorEditor::buildPicker()
{
    auto *host = new QWidget(this);
    auto *row = new QHBoxLayout(host);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(10);

    // Left: the hue/sat square + value bar, side by side. (The "Pick from
    // screen" eyedropper lives on the dialog's button row, see the constructor.)
    m_field = new HueSatField(host);
    m_bar   = new ValueBar(host);
    m_bar->setFixedWidth(24);
    auto *fieldRow = new QHBoxLayout;
    fieldRow->setSpacing(8);
    fieldRow->addWidget(m_field, /*stretch*/ 1);
    fieldRow->addWidget(m_bar);

    row->addLayout(fieldRow, /*stretch*/ 1);

    // Right: preview swatch above the numeric fields.
    auto *rightCol = new QVBoxLayout;
    rightCol->setSpacing(8);

    m_preview = new QWidget(host);
    m_preview->setMinimumSize(72, 48);
    m_preview->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    rightCol->addWidget(m_preview);

    auto *form = new QGridLayout;
    form->setHorizontalSpacing(6);
    form->setVerticalSpacing(6);
    auto addSpin = [&](const QString &label, int r, int c) {
        auto *l = new QLabel(label, host);
        auto *s = new QSpinBox(host);
        s->setRange(0, 255);
        form->addWidget(l, r, c, Qt::AlignRight);
        form->addWidget(s, r, c + 1);
        return s;
    };
    m_red   = addSpin(tr("R:"), 0, 0);
    m_green = addSpin(tr("G:"), 1, 0);
    m_blue  = addSpin(tr("B:"), 2, 0);

    auto *htmlLabel = new QLabel(tr("HTML:"), host);
    m_html = new QLineEdit(host);
    m_html->setMaxLength(7);
    // #rrggbb only.
    auto *rx = new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("#?[0-9A-Fa-f]{0,6}")), m_html);
    m_html->setValidator(rx);
    form->addWidget(htmlLabel, 3, 0, Qt::AlignRight);
    form->addWidget(m_html, 3, 1);

    rightCol->addLayout(form);
    rightCol->addStretch(1);
    row->addLayout(rightCol);

    // --- wiring ---------------------------------------------------------
    connect(m_field, &HueSatField::hueSatChanged, this, [this](int h, int s) {
        if (m_syncing) return;
        int v = m_current.value();
        // If the current colour is black, picking a hue/sat would still come out
        // black; lift value to full so the pick is immediately visible (and move
        // the value bar with it).
        if (v == 0) {
            v = 255;
            m_bar->setValue(255);
        }
        setCurrent(QColor::fromHsv(h, s, v), Src::Field);
    });
    connect(m_bar, &ValueBar::valueChanged, this, [this](int v) {
        if (m_syncing) return;
        int h, s, x, a;
        m_current.getHsv(&h, &s, &x, &a);
        setCurrent(QColor::fromHsv(qMax(0, h), s, v, a), Src::Bar);
    });
    auto spinChanged = [this]() {
        if (m_syncing) return;
        setCurrent(QColor(m_red->value(), m_green->value(), m_blue->value()),
                   Src::Spin);
    };
    connect(m_red,   qOverload<int>(&QSpinBox::valueChanged), this, spinChanged);
    connect(m_green, qOverload<int>(&QSpinBox::valueChanged), this, spinChanged);
    connect(m_blue,  qOverload<int>(&QSpinBox::valueChanged), this, spinChanged);
    connect(m_html, &QLineEdit::textEdited, this, [this](const QString &t) {
        if (m_syncing) return;
        QString s = t;
        if (!s.startsWith('#')) s.prepend('#');
        if (s.length() == 7) {                  // only act on a complete colour
            QColor c(s);
            if (c.isValid())
                setCurrent(c, Src::Html);
        }
    });

    return host;
}

// Eyedrop a colour from anywhere on screen via the XDG desktop portal's
// org.freedesktop.portal.Screenshot.PickColor. This is what the system colour
// pickers use; it needs no extra permissions and works in the Flatpak sandbox
// on both X11 and Wayland (a self-rolled screen grab would be X11-only). The
// call is async: PickColor returns a request object path, and the chosen colour
// arrives later on that request's Response signal. The portal draws its own
// picker UI (so there's no in-app hover preview), and the result is loaded into
// the picker like any manual edit.
void ColorEditor::pickFromScreen()
{
    const QString service   = QStringLiteral("org.freedesktop.portal.Desktop");
    const QString path      = QStringLiteral("/org/freedesktop/portal/desktop");
    const QString interface = QStringLiteral("org.freedesktop.portal.Screenshot");

    QDBusInterface portal(service, path, interface, QDBusConnection::sessionBus());
    if (!portal.isValid()) {
        QMessageBox::information(this, tr("Pick from screen"),
            tr("The screen colour picker needs the desktop portal, which "
               "isn't available here."));
        return;
    }

    const QString parentWindow;   // empty handle is accepted by every backend
    const QVariantMap options;

    QDBusPendingCall call =
        portal.asyncCall(QStringLiteral("PickColor"), parentWindow, options);
    auto *watcher = new QDBusPendingCallWatcher(call, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this](QDBusPendingCallWatcher *w) {
        QDBusPendingReply<QDBusObjectPath> reply = *w;
        w->deleteLater();
        if (reply.isError()) {
            QMessageBox::information(this, tr("Pick from screen"),
                tr("Couldn't start the screen picker: %1")
                    .arg(reply.error().message()));
            return;
        }
        const QString requestPath = reply.value().path();
        QDBusConnection::sessionBus().connect(
            QString(), requestPath,
            QStringLiteral("org.freedesktop.portal.Request"),
            QStringLiteral("Response"),
            this, SLOT(onPortalColorResponse(uint, QVariantMap)));
    });
}

void ColorEditor::onPortalColorResponse(uint response, const QVariantMap &results)
{
    // response: 0 = success, 1 = user cancelled, 2 = ended some other way.
    if (response != 0)
        return;

    const QVariant v = results.value(QStringLiteral("color"));
    if (!v.canConvert<QDBusArgument>())
        return;

    // "color" is a (ddd) struct of red/green/blue in 0..1, delivered as a
    // QDBusArgument we have to stream out by hand.
    double r = 0, g = 0, b = 0;
    const QDBusArgument arg = v.value<QDBusArgument>();
    arg.beginStructure();
    arg >> r >> g >> b;
    arg.endStructure();

    const QColor c = QColor::fromRgbF(qBound(0.0, r, 1.0),
                                      qBound(0.0, g, 1.0),
                                      qBound(0.0, b, 1.0));
    if (c.isValid())
        setCurrent(c, Src::External);   // updates picker + recolours selected swatch
}

void ColorEditor::paintSwatch(int index)
{
    if (index < 0 || index >= m_btns.size())
        return;
    const bool selected = (index == m_selIndex);
    const QString border = selected ? "border: 3px solid palette(highlight);"
                                     : "border: 1px solid #555;";
    m_btns[index]->setStyleSheet(
        QString("background-color: %1; %2").arg(m_colors.value(index).name(),
                                                border));
}

bool ColorEditor::moveSelection(int delta)
{
    if (m_selIndex < 0 || m_colors.isEmpty())
        return false;
    const int next = m_selIndex + delta;
    if (next < 0 || next >= m_colors.size())
        return true;     // at an edge: swallow the key, but don't move (no wrap)
    selectSwatch(next);
    return true;
}

bool ColorEditor::eventFilter(QObject *watched, QEvent *event)
{
    // Only act on key presses while this dialog is the active window; the
    // filter is installed app-wide, so we must not hijack keys meant for other
    // windows.
    if (event->type() == QEvent::KeyPress && isActiveWindow()) {
        auto *ke = static_cast<QKeyEvent *>(event);
        // While the user is editing the RGB spinboxes or HTML field, let those
        // widgets keep the arrow keys (value-stepping / cursor movement). A
        // spinbox delivers key events to a child line edit, so check ancestry,
        // not just identity. Everywhere else, arrows drive slot navigation.
        auto *w = qobject_cast<QWidget *>(watched);
        const bool inTextEntry =
            w && (w == m_red    || m_red->isAncestorOf(w)   ||
                  w == m_green  || m_green->isAncestorOf(w) ||
                  w == m_blue   || m_blue->isAncestorOf(w)  ||
                  w == m_html   || m_html->isAncestorOf(w));
        if (!inTextEntry) {
            switch (ke->key()) {
            case Qt::Key_Left:  return moveSelection(-1);
            case Qt::Key_Right: return moveSelection(+1);
            case Qt::Key_Up:    return moveSelection(-m_columns);
            case Qt::Key_Down:  return moveSelection(+m_columns);
            default: break;
            }
        }
    }
    return QDialog::eventFilter(watched, event);
}

void ColorEditor::selectSwatch(int index)
{
    const int prev = m_selIndex;
    m_selIndex = index;
    paintSwatch(prev);
    paintSwatch(index);
    setCurrent(m_colors.value(index), Src::External);
}

// Drive every control + the selected swatch from `c`. We skip writing back to
// whichever control originated the edit so we don't clobber what the user is
// actively dragging/typing. A pure-black colour carries no hue/sat, so we keep
// the field/bar's existing hue rather than snapping them to 0; the value bar
// can sit at the bottom while the field stays where the user left it.
void ColorEditor::setCurrent(const QColor &c, Src source)
{
    if (!c.isValid())
        return;
    m_current = c;
    m_syncing = true;

    int h, s, v, a;
    c.getHsv(&h, &s, &v, &a);

    // Field + bar reflect HSV. For an achromatic colour (s==0 or v==0) hue is
    // undefined (-1); keep whatever the field currently shows so the marker
    // doesn't jump to red.
    if (source != Src::Field) {
        if (h < 0) {
            // leave the field's hue as-is; only update saturation row position
            m_field->setHueSat(m_current.hslHue() >= 0 ? m_current.hslHue() : 0,
                               s);
        } else {
            m_field->setHueSat(h, s);
        }
    }
    if (source != Src::Bar) {
        m_bar->setHueSat(h < 0 ? 0 : h, s);
        m_bar->setValue(v);
    } else {
        // user is dragging the bar: keep its gradient in sync with hue/sat.
        m_bar->setHueSat(h < 0 ? 0 : h, s);
    }

    if (source != Src::Spin) {
        m_red->setValue(c.red());
        m_green->setValue(c.green());
        m_blue->setValue(c.blue());
    }
    if (source != Src::Html)
        m_html->setText(c.name());

    if (m_preview)
        m_preview->setStyleSheet(
            QString("background-color: %1; border: 1px solid #555;")
                .arg(c.name()));

    // Recolour the active swatch live.
    if (m_selIndex >= 0 && m_selIndex < m_colors.size()) {
        m_colors[m_selIndex] = c;
        // repaint without disturbing selection border
        paintSwatch(m_selIndex);
    }

    m_syncing = false;
}

QColor ColorEditor::selectedColor() const
{
    if (m_selIndex < 0 || m_selIndex >= m_colors.size())
        return m_current;
    return m_colors.value(m_selIndex);
}
