// ============================================================================
// BattleMapPanel implementation (T8.2/T8.3).
// ============================================================================

#include "BattleMapPanel.h"

#include "EditorSession.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace MapEditor {

namespace {

QString shortType(const QString &identifier)
{
    // REAL_PLAYER_TYPE_PLAYER -> PLAYER.
    const int sep = identifier.lastIndexOf('_');
    return sep >= 0 ? identifier.mid(sep + 1) : identifier;
}

/// Player colors approximating the in-game player color bars (the exact
/// palette lives in the shell UI; any distinct set works for the overlay).
const QColor kPlayerColors[8] = {
    QColor(255, 64, 64),   QColor(64, 96, 255),  QColor(255, 200, 0),
    QColor(80, 200, 80),   QColor(220, 100, 255), QColor(0, 220, 220),
    QColor(255, 128, 0),   QColor(180, 180, 180),
};

} // namespace

BattleMapPanel::BattleMapPanel(SpgFile &battleMap, EditorSession &session,
                               QWidget *parent)
    : QWidget(parent), battleMap_(battleMap), session_(session)
{
    auto *layout = new QVBoxLayout(this);

    header_ = new QLabel(tr("No battle map loaded."), this);
    header_->setWordWrap(true);
    layout->addWidget(header_);

    list_ = new QListWidget(this);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(list_);

    auto *buttons = new QHBoxLayout;
    auto *gotoButton = new QPushButton(tr("Go To"), this);
    auto *moveButton = new QPushButton(tr("Move to View Centre"), this);
    auto *editButton = new QPushButton(tr("Edit Properties..."), this);
    buttons->addWidget(gotoButton);
    buttons->addWidget(moveButton);
    buttons->addWidget(editButton);
    layout->addLayout(buttons);

    auto *newRow = new QHBoxLayout;
    newTemplateCombo_ = new QComboBox(this);
    newTemplateCombo_->setToolTip(tr("Template .spg the new battle map is built from"));
    newPlayerCountSpin_ = new QSpinBox(this);
    newPlayerCountSpin_->setRange(1, 8);
    newPlayerCountSpin_->setValue(2);
    auto *newButton = new QPushButton(tr("New Battle Map..."), this);
    newRow->addWidget(new QLabel(tr("Template:"), this));
    newRow->addWidget(newTemplateCombo_, 1);
    newRow->addWidget(new QLabel(tr("Players:"), this));
    newRow->addWidget(newPlayerCountSpin_);
    newRow->addWidget(newButton);
    layout->addLayout(newRow);

    connect(gotoButton, &QPushButton::clicked, this, &BattleMapPanel::onGoTo);
    connect(moveButton, &QPushButton::clicked, this,
            &BattleMapPanel::onMoveToViewCenter);
    connect(editButton, &QPushButton::clicked, this,
            &BattleMapPanel::onEditProperties);
    connect(list_, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *) { onGoTo(); });
    connect(newButton, &QPushButton::clicked, this, [this] {
        if (newTemplateCombo_->currentText().isEmpty()) {
            QMessageBox::information(
                this, tr("New Battle Map"),
                tr("No template battle maps found in RESOURCE/Battle."));
            return;
        }
        emit newBattleMapRequested(QString(), newTemplateCombo_->currentText(),
                                   newPlayerCountSpin_->value());
    });
}

void BattleMapPanel::setTemplates(const QStringList &templateNames)
{
    newTemplateCombo_->clear();
    newTemplateCombo_->addItems(templateNames);
}

QString BattleMapPanel::battleMapTitle() const
{
    return header_->text();
}

void BattleMapPanel::refresh()
{
    if (!battleMap_.loaded()) {
        header_->setText(tr("No battle map loaded."));
        list_->clear();
        return;
    }
    header_->setText(tr("World: %1   |   zeroLayerHeight: %2   |   players: %3")
                         .arg(QString::fromStdString(battleMap_.worldName()))
                         .arg(battleMap_.zeroLayerHeight())
                         .arg(battleMap_.playerCount()));

    list_->clear();
    for (int i = 0; i < battleMap_.playerCount(); ++i) {
        const SpgFile::PlayerData data = battleMap_.playerData(i);
        double x = 0;
        double y = 0;
        battleMap_.playerAnchor(i, x, y);
        auto *item = new QListWidgetItem(
            QStringLiteral("%1: %2 [%3]  @ %4, %5")
                .arg(i)
                .arg(QString::fromStdString(data.name),
                     shortType(QString::fromStdString(data.realPlayerType)))
                .arg(qRound(x))
                .arg(qRound(y)),
            list_);
        item->setData(Qt::UserRole, i);
        item->setForeground(kPlayerColors[i % 8]);
    }
}

void BattleMapPanel::onGoTo()
{
    QListWidgetItem *item = list_->currentItem();
    if (!item) {
        return;
    }
    const int playerIndex = item->data(Qt::UserRole).toInt();
    double x = 0;
    double y = 0;
    if (battleMap_.playerAnchor(playerIndex, x, y)) {
        emit goToCell(qRound(x), qRound(y));
    }
}

void BattleMapPanel::onMoveToViewCenter()
{
    QListWidgetItem *item = list_->currentItem();
    if (!item) {
        return;
    }
    const int playerIndex = item->data(Qt::UserRole).toInt();
    if (!session_.isWorldLoaded()) {
        QMessageBox::information(this, tr("Move Spawn"),
                                 tr("Load the battle map's world first."));
        return;
    }
    double ax = 0;
    double ay = 0;
    if (!battleMap_.playerAnchor(playerIndex, ax, ay)) {
        return;
    }

    // The new anchor is the current view centre (provided by the MainWindow
    // through the panel's property — reused from the marker flow).
    const QRect view = viewCenterProvider_ ? viewCenterProvider_() : QRect();
    if (view.isEmpty()) {
        return;
    }
    const int nx = view.center().x();
    const int ny = view.center().y();

    // z_new = z_old + (terrain_new - terrain_old): keeps the model's own
    // vertical offset over the ground (the offset is not derivable from the
    // terrain — COMPASS frame z=328 with zeroLayerHeight=177).
    const int oldTerrain = session_.terrainHeightAt(qRound(ax), qRound(ay));
    const int newTerrain = session_.terrainHeightAt(nx, ny);
    if (oldTerrain < 0 || newTerrain < 0) {
        QMessageBox::warning(this, tr("Move Spawn"),
                             tr("Cannot read the terrain height under the "
                                "spawn."));
        return;
    }
    const double dz = static_cast<double>(newTerrain - oldTerrain);
    if (!battleMap_.setPlayerAnchor(playerIndex, nx, ny, dz)) {
        QMessageBox::warning(this, tr("Move Spawn"),
                             tr("Failed to move the spawn group."));
        return;
    }
    refresh();
    emit battleMapChanged();
}

void BattleMapPanel::onEditProperties()
{
    QListWidgetItem *item = list_->currentItem();
    if (!item) {
        return;
    }
    const int playerIndex = item->data(Qt::UserRole).toInt();
    SpgFile::PlayerData data = battleMap_.playerData(playerIndex);

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Player %1 Properties").arg(playerIndex));
    auto *form = new QFormLayout(&dialog);

    auto *nameEdit = new QLineEdit(QString::fromStdString(data.name), &dialog);
    form->addRow(tr("Name:"), nameEdit);

    auto *typeEdit = new QLineEdit(
        QString::fromStdString(data.realPlayerType), &dialog);
    form->addRow(tr("Player type:"), typeEdit);

    auto *belligerentEdit = new QLineEdit(
        QString::fromStdString(data.belligerent), &dialog);
    form->addRow(tr("Belligerent:"), belligerentEdit);

    auto *difficultyEdit = new QLineEdit(
        QString::fromStdString(data.difficulty), &dialog);
    form->addRow(tr("Difficulty:"), difficultyEdit);

    auto *colorSpin = new QSpinBox(&dialog);
    colorSpin->setRange(0, 15);
    colorSpin->setValue(data.colorIndex);
    form->addRow(tr("Color index:"), colorSpin);

    auto *clanSpin = new QSpinBox(&dialog);
    clanSpin->setRange(0, 255);
    clanSpin->setValue(data.clan);
    form->addRow(tr("Clan:"), clanSpin);

    auto *handicapSpin = new QSpinBox(&dialog);
    handicapSpin->setRange(10, 400);
    handicapSpin->setValue(data.handicap);
    form->addRow(tr("Handicap (%):"), handicapSpin);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                         QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    data.name = nameEdit->text().toStdString();
    data.realPlayerType = typeEdit->text().toStdString();
    data.belligerent = belligerentEdit->text().toStdString();
    data.difficulty = difficultyEdit->text().toStdString();
    data.colorIndex = colorSpin->value();
    data.clan = clanSpin->value();
    data.handicap = handicapSpin->value();
    battleMap_.setPlayerData(playerIndex, data);
    refresh();
    emit battleMapChanged();
}

} // namespace MapEditor
