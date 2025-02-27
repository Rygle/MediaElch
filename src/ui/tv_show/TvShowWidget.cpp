#include "TvShowWidget.h"
#include "ui_TvShowWidget.h"

#include <QTimer>

#include "globals/Manager.h"
#include "globals/MessageIds.h"
#include "tv_shows/TvShow.h"
#include "tv_shows/TvShowEpisode.h"
#include "ui/notifications/NotificationBox.h"

TvShowWidget::TvShowWidget(QWidget* parent) : QWidget(parent), ui(new Ui::TvShowWidget)
{
    ui->setupUi(this);

    // clang-format off
    connect(ui->tvShowWidget, &TvShowWidgetTvShow::sigSetActionSearchEnabled, this, &TvShowWidget::sigSetActionSearchEnabled);
    connect(ui->tvShowWidget, &TvShowWidgetTvShow::sigSetActionSaveEnabled,   this, &TvShowWidget::sigSetActionSaveEnabled);
    connect(ui->tvShowWidget, &TvShowWidgetTvShow::sigDownloadsStarted,       this, &TvShowWidget::sigDownloadsStarted);
    connect(ui->tvShowWidget, &TvShowWidgetTvShow::sigDownloadsProgress,      this, &TvShowWidget::sigDownloadsProgress);
    connect(ui->tvShowWidget, &TvShowWidgetTvShow::sigDownloadsFinished,      this, &TvShowWidget::sigDownloadsFinished);

    connect(ui->episodeWidget, &TvShowWidgetEpisode::sigSetActionSaveEnabled,  this, &TvShowWidget::sigSetActionSaveEnabled);
    connect(ui->episodeWidget,&TvShowWidgetEpisode::sigSetActionSearchEnabled, this, &TvShowWidget::sigSetActionSearchEnabled);

    connect(ui->seasonWidget, &TvShowWidgetSeason::sigSetActionSaveEnabled,   this, &TvShowWidget::sigSetActionSaveEnabled);
    connect(ui->seasonWidget, &TvShowWidgetSeason::sigSetActionSearchEnabled, this, &TvShowWidget::sigSetActionSearchEnabled);
    // clang-format on
}

TvShowWidget::~TvShowWidget()
{
    delete ui;
}

void TvShowWidget::setBigWindow(bool bigWindow)
{
    ui->tvShowWidget->setBigWindow(bigWindow);
}

/**
 * \brief Clears the subwidgets
 */
void TvShowWidget::onClear()
{
    ui->episodeWidget->onClear();
    ui->tvShowWidget->onClear();
    ui->seasonWidget->onClear();
}

/**
 * \brief Shows the TV show widget and sets the show
 * \param show Current show object
 */
void TvShowWidget::onTvShowSelected(TvShow* show)
{
    qCDebug(generic) << "Entered, show=" << show->title();
    ui->stackedWidget->setCurrentIndex(0);
    ui->tvShowWidget->setTvShow(show);
}

void TvShowWidget::onSeasonSelected(TvShow* show, SeasonNumber season)
{
    qCDebug(generic) << "Entered, show=" << show->title() << "season=" << season.toString();
    ui->stackedWidget->setCurrentIndex(2);
    ui->seasonWidget->setSeason(show, season);
}

/**
 * \brief Shows the episode widget and set the episode
 * \param episode Current episode object
 */
void TvShowWidget::onEpisodeSelected(TvShowEpisode* episode)
{
    qCDebug(generic) << "Entered, episode=" << episode->title();
    ui->stackedWidget->setCurrentIndex(1);
    ui->episodeWidget->setEpisode(episode);
}

/**
 * \brief Sets the subwidgets enabled if there are no downloads
 */
void TvShowWidget::onTvShowSetEnabledTrue(TvShow* show, SeasonNumber season)
{
    if ((show != nullptr) && show->downloadsInProgress()) {
        qCDebug(generic) << "Downloads are in progress";
        return;
    }

    ui->episodeWidget->onSetEnabled(true);
    ui->tvShowWidget->onSetEnabled(true);
    ui->seasonWidget->onSetEnabled(true);
    emit sigSetActionSaveEnabled(true, MainWidgets::TvShows);
    emit sigSetActionSearchEnabled(season == SeasonNumber::NoSeason, MainWidgets::TvShows);
}

/**
 * \brief Sets the subwidgets enabled if there are no downloads
 */
void TvShowWidget::onEpisodeSetEnabledTrue(TvShowEpisode* episode)
{
    if ((episode != nullptr) && (episode->tvShow() != nullptr) && episode->tvShow()->downloadsInProgress()) {
        qCDebug(generic) << "Downloads are in progress";
        return;
    }

    ui->episodeWidget->onSetEnabled(true);
    ui->tvShowWidget->onSetEnabled(true);
    ui->seasonWidget->onSetEnabled(true);
    emit sigSetActionSaveEnabled(true, MainWidgets::TvShows);
    emit sigSetActionSearchEnabled(true, MainWidgets::TvShows);
}

/**
 * \brief Sets the subwidgets disabled
 */
void TvShowWidget::onSetDisabledTrue()
{
    ui->episodeWidget->onSetEnabled(false);
    ui->tvShowWidget->onSetEnabled(false);
    ui->seasonWidget->onSetEnabled(false);
    emit sigSetActionSaveEnabled(false, MainWidgets::TvShows);
    emit sigSetActionSearchEnabled(false, MainWidgets::TvShows);
}

/**
 * \brief Delegates the save event to the current subwidget
 */
void TvShowWidget::onSaveInformation()
{
    QVector<TvShow*> shows = TvShowFilesWidget::instance().selectedShows();
    QVector<TvShowEpisode*> episodes = TvShowFilesWidget::instance().selectedEpisodes(false);
    QVector<TvShow*> seasons = TvShowFilesWidget::instance().selectedSeasons();

    if (shows.count() == 1 && episodes.count() == 0 && seasons.count() == 0 && ui->stackedWidget->currentIndex() == 0) {
        ui->tvShowWidget->onSaveInformation();
        return;
    }
    if (shows.count() == 0 && episodes.count() == 1 && seasons.count() == 0 && ui->stackedWidget->currentIndex() == 1) {
        ui->episodeWidget->onSaveInformation();
        return;
    }
    if (shows.count() == 0 && episodes.count() == 0 && seasons.count() == 1 && ui->stackedWidget->currentIndex() == 2) {
        ui->seasonWidget->onSaveInformation();
        return;
    }

    for (TvShow* show : seasons) {
        if (!shows.contains(show)) {
            shows.append(show);
        }
    }

    const int itemsToSave = qsizetype_to_int(shows.count() + episodes.count());
    int itemsSaved = 0;
    NotificationBox::instance()->showProgressBar(
        tr("Saving changed TV Shows and Episodes"), Constants::TvShowWidgetSaveProgressMessageId);
    QApplication::processEvents();

    for (elch_size_t i = 0, n = shows.count(); i < n; ++i) {
        itemsSaved++;
        if (shows.at(i)->hasChanged()) {
            shows.at(i)->saveData(Manager::instance()->mediaCenterInterfaceTvShow());
            NotificationBox::instance()->progressBarProgress(
                itemsSaved, itemsToSave, Constants::TvShowWidgetSaveProgressMessageId);
            QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        }
    }

    for (elch_size_t i = 0, n = episodes.count(); i < n; ++i) {
        itemsSaved++;
        if (episodes.at(i)->hasChanged()) {
            episodes.at(i)->saveData(Manager::instance()->mediaCenterInterfaceTvShow());
            NotificationBox::instance()->progressBarProgress(
                itemsSaved, itemsToSave, Constants::TvShowWidgetSaveProgressMessageId);
            QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        }
    }

    NotificationBox::instance()->hideProgressBar(Constants::TvShowWidgetSaveProgressMessageId);
    NotificationBox::instance()->showSuccess(tr("TV Shows and Episodes Saved"));
}

/**
 * \brief Saves all changed TV shows and episodes
 */
void TvShowWidget::onSaveAll()
{
    qCDebug(generic) << "[TvShowWidget] Save all episodes";
    QVector<TvShow*> shows = Manager::instance()->tvShowModel()->tvShows();
    int episodesToSave = 0;
    int episodesSaved = 0;
    for (elch_size_t i = 0, n = shows.count(); i < n; ++i) {
        if (shows[i]->hasChanged()) {
            episodesToSave++;
        }
        for (int x = 0, y = qsizetype_to_int(shows[i]->episodes().count()); x < y; ++x) {
            if (shows[i]->episodes().at(x)->hasChanged()) {
                episodesToSave++;
            }
        }
    }
    qCDebug(generic) << "episodesToSave=" << episodesToSave;

    NotificationBox::instance()->showProgressBar(
        tr("Saving changed TV Shows and Episodes"), Constants::TvShowWidgetSaveProgressMessageId);
    QApplication::processEvents();

    for (elch_size_t i = 0, n = shows.count(); i < n; ++i) {
        if (shows[i]->hasChanged()) {
            qCDebug(generic) << "SAVING TV SHOW" << shows[i]->title();
            shows[i]->saveData(Manager::instance()->mediaCenterInterfaceTvShow());
            NotificationBox::instance()->progressBarProgress(
                ++episodesSaved, episodesToSave, Constants::TvShowWidgetSaveProgressMessageId);
            QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        }
        for (int x = 0, y = qsizetype_to_int(shows[i]->episodes().count()); x < y; ++x) {
            if (shows[i]->episodes().at(x)->hasChanged()) {
                shows[i]->episodes().at(x)->saveData(Manager::instance()->mediaCenterInterfaceTvShow());
                NotificationBox::instance()->progressBarProgress(
                    ++episodesSaved, episodesToSave, Constants::TvShowWidgetSaveProgressMessageId);
                QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
            }
        }
    }
    NotificationBox::instance()->hideProgressBar(Constants::TvShowWidgetSaveProgressMessageId);
    NotificationBox::instance()->showSuccess(tr("All TV Shows and Episodes Saved"));
}

/**
 * \brief Delegates the search to the current subwidget
 */
void TvShowWidget::onStartScraperSearch()
{
    qCDebug(generic) << "Entered, currentIndex=" << ui->stackedWidget->currentIndex();
    if (ui->stackedWidget->currentIndex() == 0) {
        QTimer::singleShot(0, ui->tvShowWidget, &TvShowWidgetTvShow::onStartScraperSearch);
    } else if (ui->stackedWidget->currentIndex() == 1) {
        QTimer::singleShot(0, ui->episodeWidget, &TvShowWidgetEpisode::onStartScraperSearch);
    }
}

void TvShowWidget::updateInfo()
{
    if (ui->stackedWidget->currentIndex() == 0) {
        ui->tvShowWidget->updateTvShowInfo();
    } else if (ui->stackedWidget->currentIndex() == 1) {
        ui->episodeWidget->updateEpisodeInfo();
    } else if (ui->stackedWidget->currentIndex() == 2) {
        ui->seasonWidget->updateSeasonInfo();
    }
}
