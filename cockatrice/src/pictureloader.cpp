#include "pictureloader.h"
#include "carddatabase.h"
#include "main.h"
#include "settingscache.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPixmapCache>
#include <QSet>
#include <QSvgRenderer>
#include <QThread>
#include <QUrl>

class PictureToLoad::EnabledAndKeyCompareFunctor {
public:
    inline bool operator()(CardSet *a, CardSet *b) const
    {
        if(a->getEnabled())
        {
            if(b->getEnabled())
            {
                // both enabled: sort by key
                return a->getSortKey() < b->getSortKey();
            } else {
                // only a enabled
                return true;
            }
        } else {
            if(b->getEnabled())
            {
                // only b enabled
                return false;
            } else {
                // both disabled: sort by key
                return a->getSortKey() < b->getSortKey();
            }
        }
    }
};

PictureToLoad::PictureToLoad(CardInfo *_card)
    : card(_card), setIndex(0)
{
    if (card) {
        sortedSets = card->getSets();
        qSort(sortedSets.begin(), sortedSets.end(), EnabledAndKeyCompareFunctor());
    }
}

bool PictureToLoad::nextSet()
{
    if (setIndex == sortedSets.size() - 1)
        return false;
    ++setIndex;
    return true;
}

QString PictureToLoad::getSetName() const
{
    if (setIndex < sortedSets.size())
        return sortedSets[setIndex]->getCorrectedShortName();
    else
        return QString("");
}

CardSet *PictureToLoad::getCurrentSet() const
{
    if (setIndex < sortedSets.size())
        return sortedSets[setIndex];
    else
        return 0;
}

QStringList PictureLoader::md5Blacklist = QStringList()
    << "db0c48db407a907c16ade38de048a441"; // card back returned by gatherer when card is not found

PictureLoader::PictureLoader()
    : QObject(0),
      downloadRunning(false), loadQueueRunning(false)
{
    picsPath = settingsCache->getPicsPath();
    picDownload = settingsCache->getPicDownload();

    connect(this, SIGNAL(startLoadQueue()), this, SLOT(processLoadQueue()), Qt::QueuedConnection);
    connect(settingsCache, SIGNAL(picsPathChanged()), this, SLOT(picsPathChanged()));
    connect(settingsCache, SIGNAL(picDownloadChanged()), this, SLOT(picDownloadChanged()));

    networkManager = new QNetworkAccessManager(this);
    connect(networkManager, SIGNAL(finished(QNetworkReply *)), this, SLOT(picDownloadFinished(QNetworkReply *)));

    pictureLoaderThread = new QThread;
    pictureLoaderThread->start(QThread::LowPriority);
    moveToThread(pictureLoaderThread);
}

PictureLoader::~PictureLoader()
{
    pictureLoaderThread->deleteLater();
}

void PictureLoader::processLoadQueue()
{
    if (loadQueueRunning)
        return;

    loadQueueRunning = true;
    forever {
        mutex.lock();
        if (loadQueue.isEmpty()) {
            mutex.unlock();
            loadQueueRunning = false;
            return;
        }
        cardBeingLoaded = loadQueue.takeFirst();
        mutex.unlock();

        QString setName = cardBeingLoaded.getSetName();
        QString correctedCardname = cardBeingLoaded.getCard()->getCorrectedName();
        qDebug() << "Trying to load picture (set: " << setName << " card: " << correctedCardname << ")";

        //The list of paths to the folders in which to search for images
        QList<QString> picsPaths = QList<QString>() << picsPath + "/CUSTOM/" + correctedCardname;

        if(!setName.isEmpty())
        {
            picsPaths   << picsPath + "/" + setName + "/" + correctedCardname
                        << picsPath + "/downloadedPics/" + setName + "/" + correctedCardname;
        }

        QImage image;
        QImageReader imgReader;
        imgReader.setDecideFormatFromContent(true);
        bool found = false;

        //Iterates through the list of paths, searching for images with the desired name with any QImageReader-supported extension
        for (int i = 0; i < picsPaths.length() && !found; i ++) {
            imgReader.setFileName(picsPaths.at(i));
            if (imgReader.read(&image)) {
                qDebug() << "Picture found on disk (set: " << setName << " card: " << correctedCardname << ")";
                imageLoaded(cardBeingLoaded.getCard(), image);
                found = true;
                break;
            }
            imgReader.setFileName(picsPaths.at(i) + ".full");
            if (imgReader.read(&image)) {
                qDebug() << "Picture.full found on disk (set: " << setName << " card: " << correctedCardname << ")";
                imageLoaded(cardBeingLoaded.getCard(), image);
                found = true;
            }
        }

        if (!found) {
            if (picDownload) {
                qDebug() << "Picture NOT found, trying to download (set: " << setName << " card: " << correctedCardname << ")";
                cardsToDownload.append(cardBeingLoaded);
                cardBeingLoaded=0;
                if (!downloadRunning)
                    startNextPicDownload();
            } else {
                if (cardBeingLoaded.nextSet())
                {
                    qDebug() << "Picture NOT found and download disabled, moving to next set (newset: " << setName << " card: " << correctedCardname << ")";
                    mutex.lock();
                    loadQueue.prepend(cardBeingLoaded);
                    cardBeingLoaded=0;
                    mutex.unlock();
                } else {
                    qDebug() << "Picture NOT found, download disabled, no more sets to try: BAILING OUT (oldset: " << setName << " card: " << correctedCardname << ")";
                    imageLoaded(cardBeingLoaded.getCard(), QImage());
                }
            }
        }
    }
}

QString PictureLoader::getPicUrl()
{
    if (!picDownload) return QString("");

    CardInfo *card = cardBeingDownloaded.getCard();
    CardSet *set=cardBeingDownloaded.getCurrentSet();
    QString picUrl = QString("");

    // if sets have been defined for the card, they can contain custom picUrls
    if(set)
    {
        picUrl = card->getCustomPicURL(set->getShortName());
        if (!picUrl.isEmpty())
            return picUrl;
    }

    // if a card has a muid, use the default url; if not, use the fallback
    int muid = set ? card->getMuId(set->getShortName()) : 0;
    picUrl = muid ? settingsCache->getPicUrl() : settingsCache->getPicUrlFallback();

    picUrl.replace("!name!", QUrl::toPercentEncoding(card->getCorrectedName()));
    picUrl.replace("!name_lower!", QUrl::toPercentEncoding(card->getCorrectedName().toLower()));
    picUrl.replace("!cardid!", QUrl::toPercentEncoding(QString::number(muid)));
    if (set)
    {
        picUrl.replace("!setcode!", QUrl::toPercentEncoding(set->getShortName()));
        picUrl.replace("!setcode_lower!", QUrl::toPercentEncoding(set->getShortName().toLower()));
        picUrl.replace("!setname!", QUrl::toPercentEncoding(set->getLongName()));
        picUrl.replace("!setname_lower!", QUrl::toPercentEncoding(set->getLongName().toLower()));
    }

    if (
        picUrl.contains("!name!") ||
        picUrl.contains("!name_lower!") ||
        picUrl.contains("!setcode!") ||
        picUrl.contains("!setcode_lower!") ||
        picUrl.contains("!setname!") ||
        picUrl.contains("!setname_lower!") ||
        picUrl.contains("!cardid!")
        )
    {
        qDebug() << "Insufficient card data to download" << card->getName() << "Url:" << picUrl;
        return QString("");
    }

    return picUrl;
}

void PictureLoader::startNextPicDownload()
{
    if (cardsToDownload.isEmpty()) {
        cardBeingDownloaded = 0;
        downloadRunning = false;
        return;
    }

    downloadRunning = true;

    cardBeingDownloaded = cardsToDownload.takeFirst();

    QString picUrl = getPicUrl();
    if (picUrl.isEmpty()) {
        downloadRunning = false;
        picDownloadFailed();
    } else {
        QUrl url(picUrl);

        QNetworkRequest req(url);
        qDebug() << "starting picture download:" << cardBeingDownloaded.getCard()->getName() << "Url:" << req.url();
        networkManager->get(req);
    }
}

void PictureLoader::picDownloadFailed()
{
    if (cardBeingDownloaded.nextSet())
    {
        qDebug() << "Picture NOT found, download failed, moving to next set (newset: " << cardBeingDownloaded.getSetName() << " card: " << cardBeingDownloaded.getCard()->getCorrectedName() << ")";
        mutex.lock();
        loadQueue.prepend(cardBeingDownloaded);
        mutex.unlock();
        emit startLoadQueue();
    } else {
        qDebug() << "Picture NOT found, download failed, no more sets to try: BAILING OUT (oldset: " << cardBeingDownloaded.getSetName() << " card: " << cardBeingDownloaded.getCard()->getCorrectedName() << ")";
        cardBeingDownloaded = 0;
        imageLoaded(cardBeingDownloaded.getCard(), QImage());
    }
}

void PictureLoader::picDownloadFinished(QNetworkReply *reply)
{
    if (reply->error()) {
        qDebug() << "Download failed:" << reply->errorString();
    }

    int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (statusCode == 301 || statusCode == 302) {
        QUrl redirectUrl = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
        QNetworkRequest req(redirectUrl);
        qDebug() << "following redirect:" << cardBeingDownloaded.getCard()->getName() << "Url:" << req.url();
        networkManager->get(req);
        return;
    }

    const QByteArray &picData = reply->peek(reply->size()); //peek is used to keep the data in the buffer for use by QImageReader

    // check if the image is blacklisted
    QString md5sum = QCryptographicHash::hash(picData, QCryptographicHash::Md5).toHex();
    if(md5Blacklist.contains(md5sum))
    {
        qDebug() << "Picture downloaded, but blacklisted (" << md5sum << "), will consider it as not found";
        picDownloadFailed();
        reply->deleteLater();
        startNextPicDownload();
        return;
    }

    QImage testImage;
    
    QImageReader imgReader;
    imgReader.setDecideFormatFromContent(true);
    imgReader.setDevice(reply);
    QString extension = "." + imgReader.format(); //the format is determined prior to reading the QImageReader data into a QImage object, as that wipes the QImageReader buffer
    if (extension == ".jpeg")
        extension = ".jpg";
    
    if (imgReader.read(&testImage)) {
        QString setName = cardBeingDownloaded.getSetName();
        if(!setName.isEmpty())
        {
            if (!QDir().mkpath(picsPath + "/downloadedPics/" + setName)) {
                qDebug() << picsPath + "/downloadedPics/" + setName + " could not be created.";
                return;
            }

            QFile newPic(picsPath + "/downloadedPics/" + setName + "/" + cardBeingDownloaded.getCard()->getCorrectedName() + extension);
            if (!newPic.open(QIODevice::WriteOnly))
                return;
            newPic.write(picData);
            newPic.close();
        }

        imageLoaded(cardBeingDownloaded.getCard(), testImage);
    } else {
        picDownloadFailed();
    } 

    reply->deleteLater();
    startNextPicDownload();
}

void PictureLoader::enqueueImageLoad(CardInfo *card)
{
    QMutexLocker locker(&mutex);

    // avoid queueing the same card more than once
    if(card == 0 || card == cardBeingLoaded.getCard() || card == cardBeingDownloaded.getCard())
        return;

    foreach(PictureToLoad pic, loadQueue)
    {
        if(pic.getCard() == card)
            return;
    }

    loadQueue.append(PictureToLoad(card));
    emit startLoadQueue();
}

void PictureLoader::picDownloadChanged()
{
    QMutexLocker locker(&mutex);
    picDownload = settingsCache->getPicDownload();

    QPixmapCache::clear();
}

void PictureLoader::picsPathChanged()
{
    QMutexLocker locker(&mutex);
    picsPath = settingsCache->getPicsPath();

    QPixmapCache::clear();
}

void PictureLoader::getPixmap(QPixmap &pixmap, CardInfo *card, QSize size)
{
    QPixmap bigPixmap;
    if(card)
    {    
        // search for an exact size copy of the picure in cache
        QString key = card->getPixmapCacheKey();
        QString sizekey = key + QLatin1Char('_') + QString::number(size.width()) + QString::number(size.height());
        if(QPixmapCache::find(sizekey, &pixmap))
            return;

        // load the image and create a copy of the correct size
        if(QPixmapCache::find(key, &bigPixmap))
        {
            pixmap = bigPixmap.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            QPixmapCache::insert(sizekey, pixmap);
            return;
        }
    }

    // load a temporary back picture
    QString backCacheKey = "_trice_card_back_" + QString::number(size.width()) + QString::number(size.height());
    if(!QPixmapCache::find(backCacheKey, &pixmap))
    {
        pixmap = QPixmap("theme:cardback").scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QPixmapCache::insert(backCacheKey, pixmap);
    }

    if(card)
    {
        // add the card to the load queue
        getInstance().enqueueImageLoad(card);
    }
}


void PictureLoader::imageLoaded(CardInfo *card, const QImage &image)
{
    if(image.isNull())
        return;

    if(card->getUpsideDownArt())
    {
        QImage mirrorImage = image.mirrored(true, true);
        QPixmapCache::insert(card->getPixmapCacheKey(), QPixmap::fromImage(mirrorImage));
    } else {
        QPixmapCache::insert(card->getPixmapCacheKey(), QPixmap::fromImage(image));
    }

    card->emitPixmapUpdated();
}

void PictureLoader::clearPixmapCache(CardInfo *card)
{
    //qDebug() << "Deleting pixmap for" << name;
    if(card)
        QPixmapCache::remove(card->getPixmapCacheKey());
}

void PictureLoader::clearPixmapCache()
{
    QPixmapCache::clear();
}

void PictureLoader::cacheCardPixmaps(QList<CardInfo *> cards)
{
    QPixmap tmp;
    // never cache more than 300 cards at once for a single deck
    int max = qMin(cards.size(), 300);
    for (int i = 0; i < max; ++i)
    {
        CardInfo * card = cards.at(i);
        if(!card)
            continue;

        QString key = card->getPixmapCacheKey();
        if(QPixmapCache::find(key, &tmp))
            continue;

        getInstance().enqueueImageLoad(card);
    }
}