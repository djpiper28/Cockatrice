#include "view_zone_widget.h"

#include "../../settings/cache_settings.h"
#include "../../client/ui/picture_loader.h"
#include "../../client/ui/pixel_map_generator.h"
#include "../../deck/custom_line_edit.h"
#include "../cards/card_item.h"
#include "../game_scene.h"
#include "../player/player.h"
#include "pb/command_shuffle.pb.h"
#include "view_zone.h"

#include <QCheckBox>
#include <QGraphicsLinearLayout>
#include <QGraphicsProxyWidget>
#include <QGraphicsSceneMouseEvent>
#include <QLabel>
#include <QPainter>
#include <QScrollBar>
#include <QStyleOption>
#include <QStyleOptionTitleBar>
#include <QDebug>
#include <QTextBrowser>
#include <QDir>

/**
 * @param _player the player the cards were revealed to.
 * @param _origZone the zone the cards were revealed from.
 * @param numberCards num of cards to reveal from the zone. Ex: scry the top 3 cards.
 * Pass in a negative number to reveal the entire zone.
 * -1 specifically will give the option to shuffle the zone upon closing the window.
 * @param _revealZone if false, the cards will be face down.
 * @param _writeableRevealZone whether the player can interact with the revealed cards.
 */
ZoneViewWidget::ZoneViewWidget(Player *_player,
                               CardZone *_origZone,
                               int numberCards,
                               bool _revealZone,
                               bool _writeableRevealZone,
                               const QList<const ServerInfo_Card *> &cardList)
    : QGraphicsWidget(0, Qt::Window), canBeShuffled(_origZone->getIsShufflable()), player(_player)
{
    setAcceptHoverEvents(true);
    setAttribute(Qt::WA_DeleteOnClose);
    setZValue(2000000006);
    setFlag(ItemIgnoresTransformations);

    QGraphicsLinearLayout *vbox = new QGraphicsLinearLayout(Qt::Vertical);

    // If the number is < 0, then it means that we can give the option to make the area sorted
    if (numberCards < 0) {
        // top row
        QGraphicsLinearLayout *hTopRow = new QGraphicsLinearLayout(Qt::Horizontal);

        // groupBy options
        QGraphicsProxyWidget *groupBySelectorProxy = new QGraphicsProxyWidget;
        groupBySelectorProxy->setWidget(&groupBySelector);
        groupBySelectorProxy->setZValue(2000000008);
        hTopRow->addItem(groupBySelectorProxy);

        // sortBy options
        QGraphicsProxyWidget *sortBySelectorProxy = new QGraphicsProxyWidget;
        sortBySelectorProxy->setWidget(&sortBySelector);
        sortBySelectorProxy->setZValue(2000000007);
        hTopRow->addItem(sortBySelectorProxy);

        vbox->addItem(hTopRow);

        // line
        QGraphicsProxyWidget *lineProxy = new QGraphicsProxyWidget;
        QFrame *line = new QFrame;
        line->setFrameShape(QFrame::HLine);
        line->setFrameShadow(QFrame::Sunken);
        lineProxy->setWidget(line);
        vbox->addItem(lineProxy);

        // bottom row
        QGraphicsLinearLayout *hBottomRow = new QGraphicsLinearLayout(Qt::Horizontal);

        // pile view options
        QGraphicsProxyWidget *pileViewProxy = new QGraphicsProxyWidget;
        pileViewProxy->setWidget(&pileViewCheckBox);
        hBottomRow->addItem(pileViewProxy);

        // shuffle options
        if (_origZone->getIsShufflable() && numberCards == -1) {
            shuffleCheckBox.setChecked(true);
            QGraphicsProxyWidget *shuffleProxy = new QGraphicsProxyWidget;
            shuffleProxy->setWidget(&shuffleCheckBox);
            hBottomRow->addItem(shuffleProxy);
        }

        vbox->addItem(hBottomRow);
    }

    // Search bar
    searchEdit = new SearchLineEdit;
    searchEdit->setObjectName("searchEdit");
    searchEdit->setPlaceholderText(tr("Search by card name (or search expressions)"));
    searchEdit->setClearButtonEnabled(true);
    searchEdit->addAction(loadColorAdjustedPixmap("theme:icons/search"), QLineEdit::LeadingPosition);
    auto help = searchEdit->addAction(QPixmap("theme:icons/info"), QLineEdit::TrailingPosition);

    searchEdit->installEventFilter(&searchKeySignals);

    connect(help, &QAction::triggered, this, &ZoneViewWidget::showSearchSyntaxHelp);
    connect(searchEdit, SIGNAL(textChanged(const QString &)), this, SLOT(updateSearch(const QString &)));

    QGraphicsProxyWidget *searchEditProxy= new QGraphicsProxyWidget;

    setFocusPolicy(Qt::ClickFocus);
    setFocusProxy(searchEditProxy);
    searchEditProxy->setWidget(searchEdit);
    vbox->addItem(searchEditProxy);

    extraHeight = vbox->sizeHint(Qt::PreferredSize).height();

    QGraphicsLinearLayout *zoneHBox = new QGraphicsLinearLayout(Qt::Horizontal);

    zoneContainer = new QGraphicsWidget(this);
    zoneContainer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    zoneContainer->setFlag(QGraphicsItem::ItemClipsChildrenToShape);
    zoneHBox->addItem(zoneContainer);

    scrollBar = new QScrollBar(Qt::Vertical);
    scrollBar->setMinimum(0);
    scrollBar->setSingleStep(20);
    scrollBar->setPageStep(200);
    connect(scrollBar, &QScrollBar::valueChanged, this, &ZoneViewWidget::handleScrollBarChange);
    scrollBarProxy = new ScrollableGraphicsProxyWidget;
    scrollBarProxy->setWidget(scrollBar);
    zoneHBox->addItem(scrollBarProxy);

    vbox->addItem(zoneHBox);

    zone = new ZoneViewZone(player, _origZone, numberCards, _revealZone, _writeableRevealZone, zoneContainer);
    connect(zone, SIGNAL(wheelEventReceived(QGraphicsSceneWheelEvent *)), scrollBarProxy,
            SLOT(recieveWheelEvent(QGraphicsSceneWheelEvent *)));
    retranslateUi();

    // only wire up sort options after creating ZoneViewZone, since it segfaults otherwise.
    if (numberCards < 0) {
        connect(&groupBySelector, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this,
                &ZoneViewWidget::processGroupBy);
        connect(&sortBySelector, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this,
                &ZoneViewWidget::processSortBy);
        connect(&pileViewCheckBox, &QCheckBox::QT_STATE_CHANGED, this, &ZoneViewWidget::processSetPileView);
        groupBySelector.setCurrentIndex(SettingsCache::instance().getZoneViewGroupByIndex());
        sortBySelector.setCurrentIndex(SettingsCache::instance().getZoneViewSortByIndex());
        pileViewCheckBox.setChecked(SettingsCache::instance().getZoneViewPileView());

        if (CardList::NoSort == static_cast<CardList::SortOption>(groupBySelector.currentData().toInt())) {
            pileViewCheckBox.setEnabled(false);
        }
    }

    setLayout(vbox);

    connect(zone, SIGNAL(optimumRectChanged()), this, SLOT(resizeToZoneContents()));
    connect(zone, SIGNAL(beingDeleted()), this, SLOT(zoneDeleted()));
    zone->initializeCards(cardList);

    // QLabel sizes aren't taken into account until the widget is rendered.
    // Force refresh after 1ms to fix glitchy rendering with long QLabels.
    auto *lastResizeBeforeVisibleTimer = new QTimer(this);
    connect(lastResizeBeforeVisibleTimer, &QTimer::timeout, this, [=, this] {
        resizeToZoneContents();
        disconnect(lastResizeBeforeVisibleTimer);
        lastResizeBeforeVisibleTimer->deleteLater();
    });
    lastResizeBeforeVisibleTimer->setSingleShot(true);
    lastResizeBeforeVisibleTimer->start(1);
}

ZoneViewWidget::~ZoneViewWidget()
{
    delete searchEdit;
}

void ZoneViewWidget::updateSearch(const QString &search)
{
    qDebug() << "[ViewZoneWidget] Filtering zone by" << search;
    // TODO: this should filter all cards...
}

void ZoneViewWidget::showSearchSyntaxHelp()
{

    QFile file("theme:help/search.md");

    if (!file.open(QFile::ReadOnly | QFile::Text)) {
        return;
    }

    QTextStream in(&file);
    QString text = in.readAll();
    file.close();

    // Poor Markdown Converter
    auto opts = QRegularExpression::MultilineOption;
    text = text.replace(QRegularExpression("^(###)(.*)", opts), "<h3>\\2</h3>")
               .replace(QRegularExpression("^(##)(.*)", opts), "<h2>\\2</h2>")
               .replace(QRegularExpression("^(#)(.*)", opts), "<h1>\\2</h1>")
               .replace(QRegularExpression("^------*", opts), "<hr />")
               .replace(QRegularExpression(R"(\[([^[]+)\]\(([^\)]+)\))", opts), R"(<a href='\2'>\1</a>)");

    auto browser = new QTextBrowser;
    browser->setParent(nullptr, Qt::Window | Qt::WindowTitleHint | Qt::WindowSystemMenuHint | Qt::WindowMinMaxButtonsHint |
                                 Qt::WindowCloseButtonHint | Qt::WindowFullscreenButtonHint);
    browser->setWindowTitle("Search Help");
    browser->setReadOnly(true);
    browser->setMinimumSize({500, 600});

    QString sheet = QString("a { text-decoration: underline; color: rgb(71,158,252) };");
    browser->document()->setDefaultStyleSheet(sheet);

    browser->setHtml(text);
    connect(browser, &QTextBrowser::anchorClicked, [this](const QUrl &link) { searchEdit->setText(link.fragment()); });
    browser->show();
}

void ZoneViewWidget::processGroupBy(int index)
{
    auto option = static_cast<CardList::SortOption>(groupBySelector.itemData(index).toInt());
    SettingsCache::instance().setZoneViewGroupByIndex(index);
    zone->setGroupBy(option);

    // disable pile view checkbox if we're not grouping by anything
    pileViewCheckBox.setEnabled(option != CardList::NoSort);

    // reset sortBy if it has the same value as groupBy
    if (option != CardList::NoSort &&
    option == static_cast<CardList::SortOption>(sortBySelector.currentData().toInt())) {
        sortBySelector.setCurrentIndex(1); // set to SortByName
    }
}

void ZoneViewWidget::processSortBy(int index)
{
    auto option = static_cast<CardList::SortOption>(sortBySelector.itemData(index).toInt());

    // set to SortByName instead if it has the same value as groupBy
    if (option != CardList::NoSort &&
    option == static_cast<CardList::SortOption>(groupBySelector.currentData().toInt())) {
        sortBySelector.setCurrentIndex(1); // set to SortByName
        return;
    }

    SettingsCache::instance().setZoneViewSortByIndex(index);
    zone->setSortBy(option);
}

void ZoneViewWidget::processSetPileView(QT_STATE_CHANGED_T value)
{
    SettingsCache::instance().setZoneViewPileView(value);
    zone->setPileView(value);
}

void ZoneViewWidget::retranslateUi()
{
    setWindowTitle(zone->getTranslatedName(false, CaseNominative));

    {   // We can't change the strings after they're put into the QComboBox, so this is our workaround
        int oldIndex = groupBySelector.currentIndex();
        groupBySelector.clear();
        groupBySelector.addItem(tr("Ungrouped"), CardList::NoSort);
        groupBySelector.addItem(tr("Group by Type"), CardList::SortByMainType);
        groupBySelector.addItem(tr("Group by Mana Value"), CardList::SortByManaValue);
        groupBySelector.addItem(tr("Group by Color"), CardList::SortByColorGrouping);
        groupBySelector.setCurrentIndex(oldIndex);
    }

    {
        int oldIndex = sortBySelector.currentIndex();
        sortBySelector.clear();
        sortBySelector.addItem(tr("Unsorted"), CardList::NoSort);
        sortBySelector.addItem(tr("Sort by Name"), CardList::SortByName);
        sortBySelector.addItem(tr("Sort by Type"), CardList::SortByType);
        sortBySelector.addItem(tr("Sort by Mana Cost"), CardList::SortByManaCost);
        sortBySelector.addItem(tr("Sort by Colors"), CardList::SortByColors);
        sortBySelector.addItem(tr("Sort by P/T"), CardList::SortByPt);
        sortBySelector.addItem(tr("Sort by Set"), CardList::SortBySet);
        sortBySelector.setCurrentIndex(oldIndex);
    }

    shuffleCheckBox.setText(tr("shuffle when closing"));
    pileViewCheckBox.setText(tr("pile view"));
}

void ZoneViewWidget::moveEvent(QGraphicsSceneMoveEvent * /* event */)
{
    if (!scene())
        return;

    int titleBarHeight = 24;

    QPointF scenePos = pos();

    if (scenePos.x() < 0) {
        scenePos.setX(0);
    } else {
        qreal maxw = scene()->sceneRect().width() - 100;
        if (scenePos.x() > maxw)
            scenePos.setX(maxw);
    }

    if (scenePos.y() < titleBarHeight) {
        scenePos.setY(titleBarHeight);
    } else {
        qreal maxh = scene()->sceneRect().height() - titleBarHeight;
        if (scenePos.y() > maxh)
            scenePos.setY(maxh);
    }

    if (scenePos != pos())
        setPos(scenePos);
}

void ZoneViewWidget::resizeEvent(QGraphicsSceneResizeEvent *event)
{
    // We need to manually resize the scroll bar whenever the window gets resized
    resizeScrollbar(event->newSize().height() - extraHeight - 10);
}

void ZoneViewWidget::resizeScrollbar(const qreal newZoneHeight)
{
    qreal totalZoneHeight = zone->getOptimumRect().height();
    qreal newMax = qMax(totalZoneHeight - newZoneHeight, 0.0);
    scrollBar->setMaximum(newMax);
}

/**
 * Calculates the max initial height from the settings.
 * The max initial height setting is given as number of rows, so we need to map it to a height.
 **/
static qreal calcMaxInitialHeight()
{
    const qreal cardsHeight = (SettingsCache::instance().getCardViewInitialRowsMax() + 1) * (CARD_HEIGHT / 3);
    return cardsHeight + 5; // +5 padding to make the cutoff look nicer
}

/**
 * @brief Handles edge cases in determining the next default zone height. We want the height to snap when the number of
 * rows changes, but not if the player has already expanded the window.
 */
static qreal determineNewZoneHeight(qreal oldZoneHeight)
{
    // don't snap if window is taller than max initial height
    if (oldZoneHeight > calcMaxInitialHeight()) {
        return oldZoneHeight;
    }

    return calcMaxInitialHeight();
}

void ZoneViewWidget::resizeToZoneContents()
{
    QRectF zoneRect = zone->getOptimumRect();
    qreal totalZoneHeight = zoneRect.height();

    qreal width = qMax(QGraphicsWidget::layout()->effectiveSizeHint(Qt::MinimumSize, QSizeF()).width(),
                       zoneRect.width() + scrollBar->width() + 10);

    QSizeF maxSize(width, zoneRect.height() + extraHeight + 10);

    qreal currentZoneHeight = rect().height() - extraHeight - 10;
    qreal newZoneHeight = determineNewZoneHeight(currentZoneHeight);

    QSizeF initialSize(width, newZoneHeight + extraHeight + 10);

    setMaximumSize(maxSize);
    resize(initialSize);
    resizeScrollbar(newZoneHeight);

    zone->setGeometry(QRectF(0, -scrollBar->value(), zoneContainer->size().width(), totalZoneHeight));

    if (layout())
        layout()->invalidate();
}

void ZoneViewWidget::handleScrollBarChange(int value)
{
    zone->setY(-value);
}

void ZoneViewWidget::closeEvent(QCloseEvent *event)
{
    disconnect(zone, SIGNAL(beingDeleted()), this, 0);
    if (shuffleCheckBox.isChecked())
        player->sendGameCommand(Command_Shuffle());
    emit closePressed(this);
    deleteLater();
    event->accept();
}

void ZoneViewWidget::zoneDeleted()
{
    emit closePressed(this);
    deleteLater();
}

void ZoneViewWidget::initStyleOption(QStyleOption *option) const
{
    QStyleOptionTitleBar *titleBar = qstyleoption_cast<QStyleOptionTitleBar *>(option);
    if (titleBar)
        titleBar->icon = QPixmap("theme:cockatrice");
}
