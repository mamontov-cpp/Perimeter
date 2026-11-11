// ============================================================================
// MapEditor UI — Krita-style advanced color picker + the panel around it.
//
// GradientStrip — one horizontal gradient for a single HSV channel:
//   * Hue:        H 0..359 at the current S and V,
//   * Saturation: S 0..255 at the current H and V,
//   * Value:      V 0..255 at the current H and S.
// A marker shows the current channel value; press/drag sets the channel
// from the cursor position. Because every gradient is drawn through the
// CURRENT color, clicking one strip re-renders the other two (Krita's
// advanced color selector behaviour).
//
// KritaColorPicker — the three strips + a swatch + a hex readout.
// PalettePanel — the dock panel (picker + hint), keeping the outer API the
// MainWindow already talks to (setSession/setColor/colorChanged).
// ============================================================================

#include "PalettePanel.h"

#include "EditorSession.h"

#include <QColor>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace MapEditor {

namespace {

unsigned int rgbOf(const QColor& c)
{
    return (static_cast<unsigned int>(c.red()) << 16) |
           (static_cast<unsigned int>(c.green()) << 8) |
           static_cast<unsigned int>(c.blue());
}

} // namespace

// ---------------------------------------------------------------------------

class GradientStrip : public QWidget
{
    Q_OBJECT

public:
    enum class Channel
    {
        Hue,
        Saturation,
        Value,
    };

    explicit GradientStrip(Channel channel, QWidget* parent = nullptr)
        : QWidget(parent)
        , channel_(channel)
    {
        setMinimumHeight(26);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setCursor(Qt::PointingHandCursor);
        setToolTip(channel == Channel::Hue
                       ? tr("Hue — click or drag to set the hue")
                       : channel == Channel::Saturation
                             ? tr("Saturation — click or drag to set it")
                             : tr("Value (lightness) — click or drag to set it"));
    }

    /// Reflect the new current color: the strip's gradient is rebuilt
    /// through it and the marker moves.
    void setColor(const QColor& c)
    {
        color_ = c.toHsv();
        rebuildGradient();
        update();
    }

    QSize sizeHint() const override { return QSize(180, 26); }

signals:
    /// The user picked a channel value (hue 0..359, others 0..255).
    void channelChanged(int value);

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.drawPixmap(0, 0, gradient_);
        // Marker at the current value: a black-white-black vertical line so
        // it reads on any gradient.
        const int x = markerX();
        p.fillRect(x - 1, 0, 3, height(), QColor(0, 0, 0, 160));
        p.fillRect(x, 0, 1, height(), QColor(255, 255, 255, 220));
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton) {
            pick(event->position().x());
        }
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (event->buttons() & Qt::LeftButton) {
            pick(event->position().x());
        }
    }

private:
    int channelMax() const
    {
        return channel_ == Channel::Hue ? 359 : 255;
    }

    int currentValue() const
    {
        switch (channel_) {
        case Channel::Hue:
            // Achromatic colors report hue -1; pin the marker at 0 there.
            return color_.hue() < 0 ? 0 : color_.hue();
        case Channel::Saturation:
            return color_.saturation();
        case Channel::Value:
            return color_.value();
        }
        return 0;
    }

    int markerX() const
    {
        const int w = std::max(1, width() - 1);
        return std::clamp(currentValue() * w / channelMax(), 0, w);
    }

    void pick(double x)
    {
        const int w = std::max(1, width() - 1);
        const int value =
            std::clamp(static_cast<int>(std::lround(x * channelMax() / w)),
                       0, channelMax());
        emit channelChanged(value);
    }

    QColor colorFor(int channelValue) const
    {
        const int h = color_.hue() < 0 ? 0 : color_.hue();
        const int s = color_.saturation();
        const int v = color_.value();
        switch (channel_) {
        case Channel::Hue:
            return QColor::fromHsv(channelValue, s, v);
        case Channel::Saturation:
            return QColor::fromHsv(h, channelValue, v);
        case Channel::Value:
            return QColor::fromHsv(h, s, channelValue);
        }
        return color_;
    }

    /// Per-pixel gradient: every column shows this strip's channel at that
    /// position with the OTHER two channels taken from the current color.
    void rebuildGradient()
    {
        const int w = std::max(1, width());
        const int h = std::max(1, height());
        gradient_ = QPixmap(w, h);
        QPainter p(&gradient_);
        for (int x = 0; x < w; ++x) {
            const int channelValue = x * channelMax() / (w - 1 > 0 ? w - 1 : 1);
            p.fillRect(x, 0, 1, h, colorFor(channelValue));
        }
        // Thin frame like Krita's strips.
        p.setPen(QColor(60, 60, 60));
        p.drawRect(0, 0, w - 1, h - 1);
    }

    void resizeEvent(QResizeEvent*) override
    {
        rebuildGradient();
    }

    Channel channel_;
    QColor color_ = QColor(255, 255, 255).toHsv();
    QPixmap gradient_;
};

// ---------------------------------------------------------------------------

class KritaColorPicker : public QWidget
{
    Q_OBJECT

public:
    explicit KritaColorPicker(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        QVBoxLayout* layout = new QVBoxLayout(this);
        layout->setContentsMargins(2, 2, 2, 2);
        layout->setSpacing(4);

        // Swatch + hex readout of the current color.
        QHBoxLayout* top = new QHBoxLayout;
        top->setSpacing(6);
        swatch_ = new QLabel(this);
        swatch_->setFixedSize(48, 24);
        swatch_->setFrameShape(QFrame::StyledPanel);
        top->addWidget(swatch_);
        readout_ = new QLabel(this);
        top->addWidget(readout_, 1);
        layout->addLayout(top);

        hue_ = new GradientStrip(GradientStrip::Channel::Hue, this);
        saturation_ = new GradientStrip(GradientStrip::Channel::Saturation, this);
        value_ = new GradientStrip(GradientStrip::Channel::Value, this);
        layout->addWidget(hue_);
        layout->addWidget(saturation_);
        layout->addWidget(value_);
        layout->addStretch(1);

        connect(hue_, &GradientStrip::channelChanged, this,
                [this](int h) {
                    const QColor c = color_.toHsv();
                    apply(QColor::fromHsv(h, c.saturation(), c.value()));
                });
        connect(saturation_, &GradientStrip::channelChanged, this,
                [this](int s) {
                    const QColor c = color_.toHsv();
                    apply(QColor::fromHsv(c.hue() < 0 ? 0 : c.hue(), s, c.value()));
                });
        connect(value_, &GradientStrip::channelChanged, this,
                [this](int v) {
                    const QColor c = color_.toHsv();
                    apply(QColor::fromHsv(c.hue() < 0 ? 0 : c.hue(), c.saturation(), v));
                });

        apply(QColor(255, 255, 255));
    }

    QColor color() const { return color_; }

public slots:
    void setColor(const QColor& c)
    {
        apply(c);
    }

signals:
    void colorChanged(const QColor& color);

private:
    void apply(const QColor& c)
    {
        color_ = c;
        const QColor hsv = c.toHsv();
        hue_->setColor(hsv);
        saturation_->setColor(hsv);
        value_->setColor(hsv);
        swatch_->setStyleSheet(
            QStringLiteral("background-color: %1;").arg(c.name()));
        readout_->setText(QLocale().toUpper(
            QStringLiteral("#%1  H:%2 S:%3 V:%4")
                .arg(rgbOf(c), 6, 16, QLatin1Char('0'))
                .arg(hsv.hue() < 0 ? 0 : hsv.hue())
                .arg(hsv.saturation())
                .arg(hsv.value())));
        emit colorChanged(color_);
    }

    QLabel* swatch_ = nullptr;
    QLabel* readout_ = nullptr;
    GradientStrip* hue_ = nullptr;
    GradientStrip* saturation_ = nullptr;
    GradientStrip* value_ = nullptr;
    QColor color_ = QColor(255, 255, 255);
};

// ---------------------------------------------------------------------------

PalettePanel::PalettePanel(QWidget* parent)
    : QWidget(parent)
{
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    hint_ = new QLabel(tr("No world loaded."), this);
    hint_->setAlignment(Qt::AlignCenter);

    picker_ = new KritaColorPicker(this);
    connect(picker_, &KritaColorPicker::colorChanged, this,
            [this](const QColor& c) {
                currentRgb_ = rgbOf(c) & 0x00FFFFFFu;
                emit colorChanged(currentRgb_);
            });

    layout->addWidget(hint_);
    layout->addWidget(picker_, 1);
}

void PalettePanel::setSession(EditorSession* session)
{
    session_ = session;
    const bool has = session_ && session_->isWorldLoaded();
    picker_->setEnabled(has);
    hint_->setVisible(!has);
}

void PalettePanel::setColor(unsigned int rgb)
{
    currentRgb_ = rgb & 0x00FFFFFFu;
    const QSignalBlocker blocker(picker_);
    picker_->setColor(QColor(static_cast<int>((currentRgb_ >> 16) & 0xFF),
                             static_cast<int>((currentRgb_ >> 8) & 0xFF),
                             static_cast<int>(currentRgb_ & 0xFF)));
}

} // namespace MapEditor

#include "PalettePanel.moc"
