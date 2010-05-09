/* This file is part of Clementine.

   Clementine is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Clementine is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Clementine.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "magnatuneservice.h"
#include "song.h"
#include "radiomodel.h"
#include "mergedproxymodel.h"
#include "librarymodel.h"
#include "librarybackend.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QXmlStreamReader>
#include <QtIOCompressor>
#include <QSortFilterProxyModel>

#include <QtDebug>

const char* MagnatuneService::kServiceName = "Magnatune";
const char* MagnatuneService::kDatabaseUrl =
    "http://magnatune.com/info/song_info2_xml.gz";
const char* MagnatuneService::kSongsTable = "magnatune_songs";

MagnatuneService::MagnatuneService(RadioModel* parent)
  : RadioService(kServiceName, parent),
    root_(NULL),
    library_backend_(new LibraryBackend(parent->db(), kSongsTable,
                                        QString::null, QString::null, this)),
    library_model_(new LibraryModel(library_backend_, this)),
    library_sort_model_(new QSortFilterProxyModel(this)),
    total_song_count_(0),
    network_(new QNetworkAccessManager(this))
{
  connect(library_backend_, SIGNAL(TotalSongCountUpdated(int)),
          SLOT(UpdateTotalSongCount(int)));

  library_sort_model_->setSourceModel(library_model_);
  library_sort_model_->setSortRole(LibraryModel::Role_SortText);
  library_sort_model_->setDynamicSortFilter(true);
  library_sort_model_->sort(0);

  library_model_->Init();
}

RadioItem* MagnatuneService::CreateRootItem(RadioItem *parent) {
  root_ = new RadioItem(this, RadioItem::Type_Service, kServiceName, parent);
  root_->icon = QIcon(":magnatune.png");

  model()->merged_model()->AddSubModel(
      model()->index(root_->row, 0, model()->ItemToIndex(parent)),
      library_sort_model_);

  return root_;
}

void MagnatuneService::LazyPopulate(RadioItem *item) {
  switch (item->type) {
    case RadioItem::Type_Service:
      if (total_song_count_ == 0)
        ReloadDatabase();
      break;

    default:
      break;
  }

  item->lazy_loaded = true;
}

void MagnatuneService::StartLoading(const QUrl& url) {
  emit StreamReady(url, url);
}

void MagnatuneService::ReloadDatabase() {
  QNetworkRequest request = QNetworkRequest(QUrl(kDatabaseUrl));
  request.setRawHeader("User-Agent", QString("%1 %2").arg(
      QCoreApplication::applicationName(), QCoreApplication::applicationVersion()).toUtf8());

  QNetworkReply* reply = network_->get(request);
  connect(reply, SIGNAL(finished()), SLOT(ReloadDatabaseFinished()));
  
  emit TaskStarted(MultiLoadingIndicator::LoadingMagnatune);
}

void MagnatuneService::ReloadDatabaseFinished() {
  QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());

  emit TaskFinished(MultiLoadingIndicator::LoadingMagnatune);
  root_->lazy_loaded = true;

  if (reply->error() != QNetworkReply::NoError) {
    // TODO: Error handling
    qDebug() << reply->errorString();
    return;
  }

  root_->ClearNotify();

  QtIOCompressor gzip(reply);
  gzip.setStreamFormat(QtIOCompressor::GzipFormat);
  if (!gzip.open(QIODevice::ReadOnly)) {
    qWarning() << "Error opening gzip stream";
    return;
  }

  SongList songs;

  QXmlStreamReader reader(&gzip);
  while (!reader.atEnd()) {
    reader.readNext();

    if (reader.tokenType() == QXmlStreamReader::StartElement &&
        reader.name() == "Track") {
      songs << ReadTrack(reader);
    }
  }

  library_backend_->AddOrUpdateSongs(songs);
}

Song MagnatuneService::ReadTrack(QXmlStreamReader& reader) {
  QXmlStreamAttributes attributes = reader.attributes();

  Song song;
  song.Init(attributes.value("title").toString(),
            attributes.value("artist").toString(),
            attributes.value("album").toString(),
            attributes.value("seconds").toString().toInt());
  song.set_track(attributes.value("tracknum").toString().toInt());
  song.set_year(attributes.value("year").toString().toInt());
  song.set_filename(attributes.value("url").toString());
  song.set_filetype(Song::Type_Stream);

  // We need to set these to satisfy the database constraints
  song.set_directory_id(0);
  song.set_mtime(0);
  song.set_ctime(0);
  song.set_filesize(0);

  return song;
}
