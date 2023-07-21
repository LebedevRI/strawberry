/*
 * Strawberry Music Player
 * Copyright 2018-2021, Jonas Kvinge <jonas@jkvinge.net>
 *
 * Strawberry is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Strawberry is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Strawberry.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "config.h"

#include <QObject>
#include <QByteArray>
#include <QVariant>
#include <QString>
#include <QUrl>
#include <QUrlQuery>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QXmlStreamReader>

#include "core/logging.h"
#include "core/shared_ptr.h"
#include "core/networkaccessmanager.h"
#include "utilities/strutils.h"
#include "lyricssearchrequest.h"
#include "lyricssearchresult.h"
#include "chartlyricsprovider.h"

const char *ChartLyricsProvider::kUrlSearch = "http://api.chartlyrics.com/apiv1.asmx/SearchLyricDirect";

ChartLyricsProvider::ChartLyricsProvider(SharedPtr<NetworkAccessManager> network, QObject *parent) : LyricsProvider("ChartLyrics", false, false, network, parent) {}

ChartLyricsProvider::~ChartLyricsProvider() {

  while (!replies_.isEmpty()) {
    QNetworkReply *reply = replies_.takeFirst();
    QObject::disconnect(reply, nullptr, this, nullptr);
    reply->abort();
    reply->deleteLater();
  }

}

bool ChartLyricsProvider::StartSearch(const int id, const LyricsSearchRequest &request) {

  QUrlQuery url_query;
  url_query.addQueryItem("artist", QUrl::toPercentEncoding(request.artist));
  url_query.addQueryItem("song", QUrl::toPercentEncoding(request.title));

  QUrl url(kUrlSearch);
  url.setQuery(url_query);
  QNetworkRequest req(url);
  req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
  QNetworkReply *reply = network_->get(req);
  replies_ << reply;
  QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, id, request]() { HandleSearchReply(reply, id, request); });

  return true;

}

void ChartLyricsProvider::CancelSearch(const int) {}

void ChartLyricsProvider::HandleSearchReply(QNetworkReply *reply, const int id, const LyricsSearchRequest &request) {

  if (!replies_.contains(reply)) return;
  replies_.removeAll(reply);
  QObject::disconnect(reply, nullptr, this, nullptr);
  reply->deleteLater();

  if (reply->error() != QNetworkReply::NoError) {
    Error(QString("%1 (%2)").arg(reply->errorString()).arg(reply->error()));
    emit SearchFinished(id);
    return;
  }

  if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
    Error(QString("Received HTTP code %1").arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()));
    emit SearchFinished(id);
    return;
  }

  QXmlStreamReader reader(reply);
  LyricsSearchResults results;
  LyricsSearchResult result;

  while (!reader.atEnd()) {
    QXmlStreamReader::TokenType type = reader.readNext();
    QString name = reader.name().toString();
    if (type == QXmlStreamReader::StartElement) {
      if (name == "GetLyricResult") {
        result = LyricsSearchResult();
      }
      if (name == "LyricArtist") {
        result.artist = reader.readElementText();
      }
      else if (name == "LyricSong") {
        result.title = reader.readElementText();
      }
      else if (name == "Lyric") {
        result.lyrics = reader.readElementText();
      }
    }
    else if (type == QXmlStreamReader::EndElement) {
      if (name == "GetLyricResult") {
        if (!result.artist.isEmpty() && !result.title.isEmpty() && !result.lyrics.isEmpty() &&
            (result.artist.compare(request.albumartist, Qt::CaseInsensitive) == 0 ||
             result.artist.compare(request.artist, Qt::CaseInsensitive) == 0 ||
             result.title.compare(request.title, Qt::CaseInsensitive) == 0)) {
          result.lyrics = Utilities::DecodeHtmlEntities(result.lyrics);
          results << result;
        }
        result = LyricsSearchResult();
      }
    }
  }

  if (results.isEmpty()) {
    qLog(Debug) << "ChartLyrics: No lyrics for" << request.artist << request.title;
  }
  else {
    qLog(Debug) << "ChartLyrics: Got lyrics for" << request.artist << request.title;
  }

  emit SearchFinished(id, results);

}

void ChartLyricsProvider::Error(const QString &error, const QVariant &debug) {

  qLog(Error) << "ChartLyrics:" << error;
  if (debug.isValid()) qLog(Debug) << debug;

}
