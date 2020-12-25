/******************************************************************************
 *
 * Copyright (C) 2020 Michael Lörcher <MichaelLoercher@web.de>
 *
 * This file is part of the YIO-Remote software project.
 *
 * YIO-Remote software is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * YIO-Remote software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with YIO-Remote software. If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *****************************************************************************/

#include "kodi.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextCodec>
#include <QXmlStreamReader>
#include <QProcess>
#include <QDate>

KodiPlugin::KodiPlugin() : Plugin("yio.plugin.kodi", USE_WORKER_THREAD) {}

Integration* KodiPlugin::createIntegration(const QVariantMap& config, EntitiesInterface* entities,
                                           NotificationsInterface* notifications, YioAPIInterface* api,
                                           ConfigInterface* configObj) {
    qCInfo(m_logCategory) << "Creating Kodi integration plugin" << PLUGIN_VERSION;

    return new Kodi(config, entities, notifications, api, configObj, this);
}

Kodi::Kodi(const QVariantMap& config, EntitiesInterface* entities, NotificationsInterface* notifications,
           YioAPIInterface* api, ConfigInterface* configObj, Plugin* plugin)
    : Integration(config, entities, notifications, api, configObj, plugin) {
    for (QVariantMap::const_iterator iter = config.begin(); iter != config.end(); ++iter) {
        if (iter.key() == Integration::OBJ_DATA) {
            QVariantMap map = iter.value().toMap();
            if (map.value("kodiclient_url").toString().contains("http://")) {
                m_KodiClientUrl = map.value("kodiclient_url").toString();
            } else {
                m_KodiClientUrl = "http://" + map.value("kodiclient_url").toString();
            }
            m_KodiClientPort = map.value("kodiclient_port").toString();
            if (map.value("tvheadendclient_url").toString().contains("http://")) {
                m_TvheadendClientUrl = map.value("tvheadendclient_url").toString();
            } else {
                m_TvheadendClientUrl = "http://" + map.value("kodiclient_url").toString();
            }
            m_TvheadendClientPort = map.value("tvheadendclient_port").toString();
            m_TvheadendClientUser = map.value("tvheadendclient_user").toString();
            m_TvheadendClientPassword = map.value("tvheadendclient_password").toString();
            m_entityId = map.value("entity_id").toString();
        }
    }

    m_pollingTimer = new QTimer(this);
    m_pollingTimer->setInterval(4000);
    QObject::connect(m_pollingTimer, &QTimer::timeout, this, &Kodi::onPollingTimerTimeout);

    m_progressBarTimer = new QTimer(this);
    m_progressBarTimer->setInterval(1000);
    QObject::connect(m_progressBarTimer, &QTimer::timeout, this, &Kodi::onProgressBarTimerTimeout);

    // add available entity
    QStringList supportedFeatures;
    supportedFeatures << "SOURCE"
                      << "APP_NAME"
                      << "VOLUME"
                      << "VOLUME_UP"
                      << "VOLUME_DOWN"
                      << "VOLUME_SET"
                      << "MUTE"
                      << "MUTE_SET"
                      << "MEDIA_TYPE"
                      << "MEDIA_TITLE"
                      << "MEDIA_ARTIST"
                      << "MEDIA_ALBUM"
                      << "MEDIA_DURATION"
                      << "MEDIA_POSITION"
                      << "MEDIA_IMAGE"
                      << "PLAY"
                      << "PAUSE"
                      << "STOP"
                      << "PREVIOUS"
                      << "NEXT"
                      << "SEEK"
                      << "SHUFFLE"
                      << "SEARCH"
                      << "MEDIAPLAYERCOMMAND"
                      << "MEDIAPLAYERREMOTE"
                      << "TVCHANNELLIST";
    addAvailableEntity(m_entityId, "media_player", integrationId(), friendlyName(), supportedFeatures);
}

void Kodi::connect() {
    setState(CONNECTING);

    qCDebug(m_logCategory) << "STARTING Kodi";

    if (m_TvheadendClientUrl != "" && m_TvheadendClientPort != "") {
        if (QProcess::execute("curl", QStringList() << "-s" << m_TvheadendClientUrl+":"+m_TvheadendClientPort) == 0) {
            m_flagTVHeadendConfigured = true;
            getTVEPGfromTVHeadend();
        } else {
            qCDebug(m_logCategory) << "TVHeadend not confgured";
            m_flagTVHeadendConfigured = false;
        }
    } else {
        qCDebug(m_logCategory) << "TVHeadend not confgured";
        m_flagTVHeadendConfigured = false;
    }
    if (m_KodiClientUrl != "" && m_KodiClientPort != "") {
        if (QProcess::execute("curl", QStringList() << "-s" << m_KodiClientUrl+":"+m_KodiClientPort) == 0) {
            m_flagKodiConfigured = true;
            m_pollingTimer->start();
            if (m_KodiTVChannelList.count() == 0) {
                getKodiAvailableTVChannelList();
            }
        } else {
            qCDebug(m_logCategory) << "Kodi not confgured";
            m_flagKodiConfigured = false;
        }
    } else {
        qCDebug(m_logCategory) << "Kodi not confgured";
        m_flagKodiConfigured = false;
    }
    setState(CONNECTED);
}



void Kodi::disconnect() {
    setState(DISCONNECTED);
    m_pollingTimer->stop();
    m_progressBarTimer->stop();
}

void Kodi::enterStandby() {
    disconnect();
}

void Kodi::leaveStandby() {
    connect();
}


void Kodi::search(QString query) {
    search(query, "album,artist,playlist,track", "20", "0");
}

void Kodi::search(QString query, QString type) {
    search(query, type, "20", "0");
}

void Kodi::search(QString query, QString type, QString limit, QString offset) {
    QString url = "/v1/search";

    query.replace(" ", "%20");

    QObject* context = new QObject(this);

    QObject::connect(this, &Kodi::requestReady, context, [=](const QVariantMap& map, const QString& rUrl) {
        if (rUrl == url) {
            // get the albums
            SearchModelList* albums = new SearchModelList();

            if (map.contains("albums")) {
                QVariantList map_albums = map.value("albums").toMap().value("items").toList();

                QStringList commands = {"PLAY", "ARTISTRADIO"};

                for (int i = 0; i < map_albums.length(); i++) {
                    QString id = map_albums[i].toMap().value("id").toString();
                    QString title = map_albums[i].toMap().value("name").toString();
                    QString subtitle =
                            map_albums[i].toMap().value("artists").toList()[0].toMap().value("name").toString();
                    QString image = "";
                    if (map_albums[i].toMap().contains("images") &&
                            map_albums[i].toMap().value("images").toList().length() > 0) {
                        QVariantList images = map_albums[i].toMap().value("images").toList();
                        for (int k = 0; k < images.length(); k++) {
                            if (images[k].toMap().value("width").toInt() == 300) {
                                image = images[k].toMap().value("url").toString();
                            }
                        }
                        if (image == "") {
                            image = map_albums[i].toMap().value("images").toList()[0].toMap().value("url").toString();
                        }
                    }

                    SearchModelListItem item = SearchModelListItem(id, "album", title, subtitle, image, QVariant());
                    albums->append(item);
                }
            }

            // get the tracks
            SearchModelList* tracks = new SearchModelList();

            if (map.contains("tracks")) {
                QVariantList map_tracks = map.value("tracks").toMap().value("items").toList();

                QStringList commands = {"PLAY", "SONGRADIO", "QUEUE"};

                for (int i = 0; i < map_tracks.length(); i++) {
                    QString id = map_tracks[i].toMap().value("id").toString();
                    QString title = map_tracks[i].toMap().value("name").toString();
                    QString subtitle = map_tracks[i].toMap().value("album").toMap().value("name").toString();
                    QString image = "";
                    if (map_tracks[i].toMap().value("album").toMap().contains("images") &&
                            map_tracks[i].toMap().value("album").toMap().value("images").toList().length() > 0) {
                        QVariantList images = map_tracks[i].toMap().value("album").toMap().value("images").toList();
                        for (int k = 0; k < images.length(); k++) {
                            if (images[k].toMap().value("width").toInt() == 64) {
                                image = images[k].toMap().value("url").toString();
                            }
                        }
                        if (image == "") {
                            image = map_tracks[i]
                                    .toMap()
                                    .value("album")
                                    .toMap()
                                    .value("images")
                                    .toList()[0]
                                    .toMap()
                                    .value("url")
                                    .toString();
                        }
                    }

                    SearchModelListItem item = SearchModelListItem(id, "track", title, subtitle, image, commands);
                    tracks->append(item);
                }
            }

            // get the artists
            SearchModelList* artists = new SearchModelList();

            if (map.contains("artists")) {
                QVariantList map_artists = map.value("artists").toMap().value("items").toList();

                QStringList commands = {"ARTISTRADIO"};

                for (int i = 0; i < map_artists.length(); i++) {
                    QString id = map_artists[i].toMap().value("id").toString();
                    QString title = map_artists[i].toMap().value("name").toString();
                    QString subtitle = "";
                    QString image = "";
                    if (map_artists[i].toMap().contains("images") &&
                            map_artists[i].toMap().value("images").toList().length() > 0) {
                        QVariantList images = map_artists[i].toMap().value("images").toList();
                        for (int k = 0; k < images.length(); k++) {
                            if (images[k].toMap().value("width").toInt() == 64) {
                                image = images[k].toMap().value("url").toString();
                            }
                        }
                        if (image == "") {
                            image = map_artists[i].toMap().value("images").toList()[0].toMap().value("url").toString();
                        }
                    }

                    SearchModelListItem item = SearchModelListItem(id, "artist", title, subtitle, image, commands);
                    artists->append(item);
                }
            }

            // get the playlists
            SearchModelList* playlists = new SearchModelList();

            if (map.contains("playlists")) {
                QVariantList map_playlists = map.value("playlists").toMap().value("items").toList();

                QStringList commands = {"PLAY", "PLAYLISTRADIO", "QUEUE"};

                for (int i = 0; i < map_playlists.length(); i++) {
                    QString id = map_playlists[i].toMap().value("id").toString();
                    QString title = map_playlists[i].toMap().value("name").toString();
                    QString subtitle = map_playlists[i].toMap().value("owner").toMap().value("display_name").toString();
                    QString image = "";
                    if (map_playlists[i].toMap().contains("images") &&
                            map_playlists[i].toMap().value("images").toList().length() > 0) {
                        QVariantList images = map_playlists[i].toMap().value("images").toList();
                        for (int k = 0; k < images.length(); k++) {
                            if (images[k].toMap().value("width").toInt() == 300) {
                                image = images[k].toMap().value("url").toString();
                            }
                        }
                        if (image == "") {
                            image =
                                    map_playlists[i].toMap().value("images").toList()[0].
                                    toMap().value("url").toString();
                        }
                    }
                    SearchModelListItem item = SearchModelListItem(id, "playlist", title, subtitle, image, commands);
                    playlists->append(item);
                }
            }

            SearchModelItem* ialbums = new SearchModelItem("albums", albums);
            SearchModelItem* itracks = new SearchModelItem("tracks", tracks);
            SearchModelItem* iartists = new SearchModelItem("artists", artists);
            SearchModelItem* iplaylists = new SearchModelItem("playlists", playlists);

            SearchModel* m_model = new SearchModel();

            m_model->append(ialbums);
            m_model->append(itracks);
            m_model->append(iartists);
            m_model->append(iplaylists);

            // update the entity
            EntityInterface* entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));
            if (entity) {
                MediaPlayerInterface* me = static_cast<MediaPlayerInterface*>(entity->getSpecificInterface());
                me->setSearchModel(m_model);
            }
        }
        context->deleteLater();
    });
    // getRequest(url, "?q=" + query + "&type=" + type + "&limit=" + limit + "&offset=" + offset);
}

void Kodi::getAlbum(QString id) {
    QString url = "/v1/albums/";

    QObject* context = new QObject(this);

    QObject::connect(this, &Kodi::requestReady, context, [=](const QVariantMap& map, const QString& rUrl) {
        if (rUrl == url) {
            qCDebug(m_logCategory) << "GET ALBUM";
            QString id = map.value("id").toString();
            QString title = map.value("name").toString();
            QString subtitle = map.value("artists").toList()[0].toMap().value("name").toString();
            QString type = "album";
            QString time = "";
            QString image = "";
            if (map.contains("images") && map.value("images").toList().length() > 0) {
                QVariantList images = map.value("images").toList();
                for (int k = 0; k < images.length(); k++) {
                    if (images[k].toMap().value("width").toInt() == 300) {
                        image = images[k].toMap().value("url").toString();
                    }
                }
                if (image == "") {
                    image = map.value("images").toList()[0].toMap().value("url").toString();
                }
            }

            QStringList commands = {"PLAY"};

            BrowsetvchannelModel* tvchannel = new BrowsetvchannelModel(nullptr, id,
                                                                       time, title, subtitle, type,
                                                                       image, commands);

            // add tracks to album
            QVariantList tracks = map.value("tracks").toMap().value("items").toList();
            for (int i = 0; i < tracks.length(); i++) {
                tvchannel->addtvchannelItem(tracks[i].toMap().value("id").toString(), "",
                                            tracks[i].toMap().value("name").toString(),
                                            tracks[i].toMap().value("artists").toList()[0].toMap().value("name").toString(),
                        "track",
                        "", commands);
            }

            // update the entity
            EntityInterface* entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));
            if (entity) {
                MediaPlayerInterface* me = static_cast<MediaPlayerInterface*>(entity->getSpecificInterface());
                me->setBrowseModel(tvchannel);
            }
        }
        context->deleteLater();
    });
    // getRequest(url, id);
}

void Kodi::getTVEPGfromTVHeadend() {
    QObject* context_getTVEPGfromTVHeadend = new QObject(this);
    QObject::connect(this, &Kodi::requestReadyQstring, context_getTVEPGfromTVHeadend,
                     [=](const QString& answer, const QString& rUrl) {
        if (rUrl == "getTVEPGfromTVHeadend") {
            QJsonParseError parseerror;
            QJsonDocument   doc = QJsonDocument::fromJson(answer.toUtf8(), &parseerror);
            if (parseerror.error != QJsonParseError::NoError) {
                qCWarning(m_logCategory) << "JSON error : " << parseerror.errorString();
                return;
            }

            // createa a map object
            m_currentEPG = doc.toVariant().toMap().value("entries").toList();
            m_EPGExpirationTimestamp = QDateTime(QDate::currentDate()).toTime_t() +
                    (m_tvProgrammExpireTimeInHours * 3600);
        }
        context_getTVEPGfromTVHeadend->deleteLater();
    });
    int temp_Timestamp = (m_EPGExpirationTimestamp - (QDateTime(QDate::currentDate()).toTime_t()));
    if (m_flagTVHeadendConfigured && temp_Timestamp < 0) {
        getRequestWithAuthentication(m_TvheadendClientUrl +":" + m_TvheadendClientPort +
                                     "/api/epg/events/grid?limit=2000", "getTVEPGfromTVHeadend",
                                     m_TvheadendClientUser, m_TvheadendClientPassword);
    }
}
void Kodi::getSingleTVChannelList(QString param) {
    QObject* context_getTVChannelList = new QObject(this);

    QString channelnumber = "0";
    for (int i = 0; i < m_KodiTVChannelList.length(); i++) {
        if (m_KodiTVChannelList[i].toMap().value("channelid").toString() == param) {
            channelnumber = m_KodiTVChannelList[i].toMap().value("channelnumber").toString();
        }
    }
    if (channelnumber != "0" && m_xml && m_flagTVHeadendConfigured) {
        QObject::connect(this, &Kodi::requestReadyQstring, context_getTVChannelList,
                         [=](const QString& answer, const QString& rUrl) {
            QXmlStreamReader reader(answer);
            if (rUrl == "tvprogrammparser") {
                EntityInterface* entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(
                                                                            m_entityId));
                if (!answer.contains("<?xml")) {
                    int currenttvchannelarrayid = 0;
                    for (int i=0; i < m_KodiTVChannelList.length(); i++) {
                        if (m_KodiTVChannelList[i].toMap().value("channelid") == param) {
                            // currenttvchannel = m_KodiTVChannelList[i].toMap();
                            currenttvchannelarrayid = i;
                            break;
                        }
                    }
                    QString id = m_KodiTVChannelList[currenttvchannelarrayid].toMap().
                            value("channelid").toString();
                    QString title = m_KodiTVChannelList[currenttvchannelarrayid].toMap().
                            value("label").toString();
                    QString subtitle = "";
                    QString type = "playlist";
                    QString image = QString::fromStdString(QByteArray::fromPercentEncoding(
                                                               m_KodiTVChannelList[currenttvchannelarrayid].
                                                               toMap().value("thumbnail").toString().toUtf8()).
                                                           toStdString()).mid(8);
                    if (image.contains("127.0.0.1")) {
                        image = image.replace("127.0.0.1", m_KodiClientUrl.mid(7));
                    }
                    QStringList commands = {};

                    BrowsetvchannelModel* tvchannel = new BrowsetvchannelModel(nullptr, id, "",
                                                                               title, subtitle, type, image,
                                                                               commands);
                    tvchannel->addtvchannelItem(m_KodiTVChannelList[currenttvchannelarrayid].
                                                toMap().value("channelid").toString(),
                                                "",
                                                "No programm available",
                                                "",
                                                "track", "", commands);

                    if (entity) {
                        MediaPlayerInterface* me = static_cast<MediaPlayerInterface*>(
                                    entity->getSpecificInterface());
                        me->setBrowseModel(tvchannel);
                    }
                } else if (!reader.hasError()) {
                    QMap<QString, QString> currenttvprogramm;

                    QString starttime = "";
                    QString title = "";
                    bool nextday = false;
                    while (reader.readNext()) {
                        QString z = reader.name().toString();
                        if (reader.name().toString() =="programme") {
                            QList<QXmlStreamAttribute> list = reader.attributes().toList();
                            for (const auto& element : list) {
                                if (element.name().toString() == "channel" &&
                                        element.value().toString() == m_mapKodiChannelNumberToTVHeadendUUID.
                                        value(channelnumber.toInt())) {
                                    starttime = reader.attributes().toList()[0].value().toString();

                                    if (starttime.mid(6, 2).toInt() > QDate::currentDate().toString("dd").toInt()) {
                                        nextday = true;
                                    }

                                    while (reader.readNext()) {
                                        if (reader.name().toString() == "title") {
                                            title = reader.readElementText();
                                            currenttvprogramm.insert(starttime, title);
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                        if (nextday) {
                            break;
                        }
                    }
                    int currenttvchannelarrayid = 0;
                    for (int i=0; i < m_KodiTVChannelList.length(); i++) {
                        if (m_KodiTVChannelList[i].toMap().value("channelid") == param) {
                            currenttvchannelarrayid = i;
                            break;
                        }
                    }
                    QString id = m_KodiTVChannelList[currenttvchannelarrayid].toMap().value("channelid").toString();
                    title = m_KodiTVChannelList[currenttvchannelarrayid].toMap().value("label").toString();
                    QString subtitle = "";
                    QString type = "playlist";
                    QString time = "";
                    QString image = QString::fromStdString(QByteArray::fromPercentEncoding(m_KodiTVChannelList[currenttvchannelarrayid].toMap().value("thumbnail").toString().toUtf8()).toStdString()).mid(8);
                    if (image.contains("127.0.0.1")) {
                        image = image.replace("127.0.0.1", m_KodiClientUrl.mid(7));
                    }
                    QStringList commands = {"PLAY"};

                    BrowsetvchannelModel* album = new BrowsetvchannelModel(nullptr, id, time, title, subtitle,
                                                                           type, image, commands);

                    for (auto key : currenttvprogramm.keys()) {
                        album->addtvchannelItem(m_KodiTVChannelList[currenttvchannelarrayid].toMap().
                                                value("channelid").toString(), key.mid(8, 4),
                                                currenttvprogramm.value(key), "", "tvchannel", "", commands);
                    }
                    if (entity) {
                        MediaPlayerInterface* me = static_cast<MediaPlayerInterface*>(entity->getSpecificInterface());
                        me->setBrowseModel(album);
                    }
                }
            }
            context_getTVChannelList->deleteLater();
        });

        if (channelnumber != "0") {
            getRequestWithAuthentication(m_TvheadendClientUrl +":" + m_TvheadendClientPort +
                                         "/xmltv/channelnumber/"+channelnumber, "tvprogrammparser",
                                         m_TvheadendClientUser, m_TvheadendClientPassword);
        } else {
            getRequestWithAuthentication(m_TvheadendClientUrl +":" + m_TvheadendClientPort +
                                         "/xmltv/channels/", "tvprogrammparser", m_TvheadendClientUser,
                                         m_TvheadendClientPassword);
        }
    } else if (channelnumber != "0" && !m_xml && m_flagTVHeadendConfigured) {
        QObject::connect(this, &Kodi::requestReadyQstring, context_getTVChannelList,
                         [=](const QString& answer, const QString& rUrl) {
            if (rUrl == "tvprogrammparser") {
                EntityInterface* entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));
                QJsonParseError parseerror;
                QJsonDocument   doc = QJsonDocument::fromJson(answer.toUtf8(), &parseerror);
                if (parseerror.error != QJsonParseError::NoError) {
                    qCWarning(m_logCategory) << "JSON error : " << parseerror.errorString();
                    return;
                }
                QMap<QString, QString> currenttvprogramm;
                for (int i = 0; i < m_currentEPG.length(); i++) {
                    if (m_currentEPG[i].toMap().value("channelNumber") == channelnumber) {
                        currenttvprogramm.insert(m_currentEPG[i].toMap().value("start").toString(),
                                                 m_currentEPG[i].toMap().value("title").toString());
                    }
                }
                int currenttvchannelarrayid = 0;
                for (int i=0; i < m_KodiTVChannelList.length(); i++) {
                    if (m_KodiTVChannelList[i].toMap().value("channelid") == param) {
                        currenttvchannelarrayid = i;
                        break;
                    }
                }
                QString id = m_KodiTVChannelList[currenttvchannelarrayid].toMap().value("channelid").toString();
                QString title = m_KodiTVChannelList[currenttvchannelarrayid].toMap().value("label").toString();
                QString subtitle = "";
                QString type = "playlist";
                QString time = "";
                QString image = QString::fromStdString(QByteArray::fromPercentEncoding(m_KodiTVChannelList[currenttvchannelarrayid].toMap().value("thumbnail").toString().toUtf8()).toStdString()).mid(8);
                if (image.contains("127.0.0.1")) {
                    image = image.replace("127.0.0.1", m_KodiClientUrl.mid(7));
                }
                QStringList commands = {"PLAY"};

                BrowsetvchannelModel* album = new BrowsetvchannelModel(nullptr, id, time, title, subtitle,
                                                                       type, image, commands);

                for (auto key : currenttvprogramm.keys()) {
                    QDateTime timestamp;
                    timestamp.setTime_t(key.toUInt());

                    album->addtvchannelItem(m_KodiTVChannelList[currenttvchannelarrayid].toMap().
                                            value("channelid").toString(), timestamp.toString("hh:mm"),
                                            currenttvprogramm.value(key), "", "tvchannel", "", commands);
                }


                if (entity) {
                    MediaPlayerInterface* me = static_cast<MediaPlayerInterface*>(entity->getSpecificInterface());
                    me->setBrowseModel(album);
                }
            }
            context_getTVChannelList->deleteLater();
        });

        getRequestWithAuthentication(m_TvheadendClientUrl +":" + m_TvheadendClientPort +
                                     "/api/epg/events/grid?limit=1", "tvprogrammparser", m_TvheadendClientUser,
                                     m_TvheadendClientPassword);

    } else if (!m_flagTVHeadendConfigured) {
        QObject::connect(this, &Kodi::requestReady, context_getTVChannelList,
                         [=](const QVariantMap& map, const QString& rMethod) {
            if (rMethod == "getTVChannelList" && map.contains("result")) {
                if (map.value("result") == "pong") {
                    EntityInterface* entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));
                    int currenttvchannelarrayid = 0;
                    for (int i=0; i < m_KodiTVChannelList.length(); i++) {
                        if (m_KodiTVChannelList[i].toMap().value("channelid") == param) {
                            // currenttvchannel = m_KodiTVChannelList[i].toMap();
                            currenttvchannelarrayid = i;
                            break;
                        }
                    }
                    QString id = m_KodiTVChannelList[currenttvchannelarrayid].toMap().value("channelid").toString();
                    QString title = m_KodiTVChannelList[currenttvchannelarrayid].toMap().value("label").toString();
                    QString subtitle = "";
                    QString type = "tvchannellist";
                    QString image = QString::fromStdString(QByteArray::fromPercentEncoding(m_KodiTVChannelList[currenttvchannelarrayid].toMap().value("thumbnail").toString().toUtf8()).toStdString()).mid(8);
                    if (image.contains("127.0.0.1")) {
                        image = image.replace("127.0.0.1", m_KodiClientUrl.mid(7));
                    }
                    QStringList commands = {};

                    BrowsetvchannelModel* tvchannel = new BrowsetvchannelModel(nullptr, id, "",
                                                                               title, subtitle, type, image,
                                                                               commands);
                    tvchannel->addtvchannelItem(m_KodiTVChannelList[currenttvchannelarrayid].toMap().
                                                value("channelid").toString(), "", "No programm available",
                                                "", "tvchannel", "", commands);

                    if (entity) {
                        MediaPlayerInterface* me = static_cast<MediaPlayerInterface*>(entity->getSpecificInterface());
                        me->setBrowseModel(tvchannel);
                    }
                }
            }
            context_getTVChannelList->deleteLater();
        });
        qCDebug(m_logCategory) << "GET USERS PLAYLIST";
        QString jsonstring;

        postRequest(m_KodiClientUrl +":" + m_KodiClientPort +"/jsonrpc", "getTVChannelList",
                    "{ \"jsonrpc\": \"2.0\", \"method\": \"JSONRPC.Ping\","
                    " \"params\": {  }, \"id\": "+QString::number(m_globalKodiRequestID)+" }");
    } else {
        QObject::connect(this, &Kodi::requestReady, context_getTVChannelList,
                         [=](const QVariantMap& map, const QString& rMethod) {
            if (rMethod == "getTVChannelList" && map.contains("result")) {
                if (map.value("result") == "pong") {
                    EntityInterface* entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));
                    int currenttvchannelarrayid = 0;
                    for (int i=0; i < m_KodiTVChannelList.length(); i++) {
                        if (m_KodiTVChannelList[i].toMap().value("channelid") == param) {
                            // currenttvchannel = m_KodiTVChannelList[i].toMap();
                            currenttvchannelarrayid = i;
                            break;
                        }
                    }
                    QString id = m_KodiTVChannelList[currenttvchannelarrayid].toMap().value("channelid").toString();
                    QString title = m_KodiTVChannelList[currenttvchannelarrayid].toMap().value("label").toString();
                    QString subtitle = "";
                    QString type = "tvchannellist";
                    QString image = QString::fromStdString(QByteArray::fromPercentEncoding(m_KodiTVChannelList[currenttvchannelarrayid].toMap().value("thumbnail").toString().toUtf8()).toStdString()).mid(8);
                    if (image.contains("127.0.0.1")) {
                        image = image.replace("127.0.0.1", m_KodiClientUrl.mid(7));
                    }
                    QStringList commands = {};

                    BrowsetvchannelModel* tvchannel = new BrowsetvchannelModel(nullptr, id, "", title,
                                                                               subtitle, type, image, commands);
                    tvchannel->addtvchannelItem(m_KodiTVChannelList[currenttvchannelarrayid].toMap().
                                                value("channelid").toString(), "", "No programm available",
                                                "", "tvchannel", "", commands);
                    if (entity) {
                        MediaPlayerInterface* me = static_cast<MediaPlayerInterface*>(entity->getSpecificInterface());
                        me->setBrowseModel(tvchannel);
                    }
                }
            }
            context_getTVChannelList->deleteLater();
        });
        qCDebug(m_logCategory) << "GET USERS PLAYLIST";
        QString jsonstring;

        postRequest(m_KodiClientUrl +":" + m_KodiClientPort +
                    "/jsonrpc", "getTVChannelList", "{ \"jsonrpc\": \"2.0\", \"method\": \"JSONRPC.Ping\","
                                                    " \"params\": {  }, \"id\": "+QString::number(m_globalKodiRequestID)+" }");
    }
}
void Kodi::getKodiChannelNumberToTVHeadendUUIDMapping() {
    QObject* context_getKodiChannelNumberToTVHeadendUUIDMapping = new QObject(this);
    QString jsonstring;
    QObject::connect(this, &Kodi::requestReadyQstring, context_getKodiChannelNumberToTVHeadendUUIDMapping,
                     [=](const QString& answer, const QString& rUrl) {
        if (rUrl == "parser") {
            if (m_xml) {
                QXmlStreamReader reader(answer);
                if (!reader.hasError()) {
                    QMap<QString, QString> currenttvprogramm;
                    QString starttime = "";
                    QString title = "";
                    QString uuid = "";
                    QString channelnumber = "";
                    while (reader.readNext()) {
                        if (reader.name().toString() =="programme") {
                            break;
                        }
                        if (reader.name().toString() =="channel") {
                            QString Z = reader.name().toString();
                            if (reader.attributes().count() > 0) {
                                QList<QXmlStreamAttribute> list = reader.attributes().toList();
                                uuid = list[0].value().toString();
                                while (reader.readNext()) {
                                    if (reader.name().toString() == "icon") {
                                        break;
                                    }
                                    if (reader.name().toString() == "display-name") {
                                        QString tempReaderName = reader.readElementText();
                                        while (reader.readNext()) {
                                            if (reader.name().toString() == "display-name") {
                                                channelnumber = reader.readElementText();
                                                m_mapKodiChannelNumberToTVHeadendUUID.
                                                        insert(channelnumber.toInt(), uuid);
                                                break;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            } else if (!m_xml) {
                QJsonParseError parseerror;
                QList<QVariant> mapofentries;
                QJsonDocument   doc = QJsonDocument::fromJson(answer.toUtf8(), &parseerror);
                if (parseerror.error != QJsonParseError::NoError) {
                    qCWarning(m_logCategory) << "JSON error : " << parseerror.errorString();
                    return;
                }
                mapofentries = doc.toVariant().toMap().value("entries").toList();
                for (int i = 0; i < mapofentries.length(); i++) {
                    for (int j = 0; j < m_KodiTVChannelList.length(); j++) {
                        if (m_KodiTVChannelList[j].toMap().values().indexOf(mapofentries[i].toMap().value("val").toString()) > 0)  {
                            m_mapKodiChannelNumberToTVHeadendUUID.insert(m_KodiTVChannelList[j].toMap()
                                                                         .value("channelnumber").toInt(),
                                                                         mapofentries[i].toMap().value("key")
                                                                         .toString());
                            break;
                        }
                    }
                }
            }
        }
        context_getKodiChannelNumberToTVHeadendUUIDMapping->deleteLater();
    });
    if (m_xml) {
        getRequestWithAuthentication(m_TvheadendClientUrl +":" + m_TvheadendClientPort +
                                     "/xmltv/channels/", "parser", m_TvheadendClientUser,
                                     m_TvheadendClientPassword);
    } else if (!m_xml) {
        getRequestWithAuthentication(m_TvheadendClientUrl +":" + m_TvheadendClientPort +
                                     "/api/channel/list", "parser", m_TvheadendClientUser,
                                     m_TvheadendClientPassword);
    }
}

void Kodi::getKodiAvailableTVChannelList() {
    QObject* context_gettvchannellist = new QObject(this);
    QString jsonstring;

    QObject::connect(this, &Kodi::requestReady, context_gettvchannellist,
                     [=](const QVariantMap& map, const QString& rUrl) {
        if (rUrl == "PVR.GetChannels") {
            if (map.contains("result")) {
                if (map.value("result").toMap().count() != 0) {
                    m_KodiTVChannelList = map.value("result").toMap().value("channels").toList();
                    if (m_flagTVHeadendConfigured) {
                        if (m_mapKodiChannelNumberToTVHeadendUUID.isEmpty())  {
                            getKodiChannelNumberToTVHeadendUUIDMapping();
                        } else {
                            qCDebug(m_logCategory) << "Host not reachable";
                        }
                    } else {
                        qCDebug(m_logCategory) << "Tv Headend not configured";
                    }
                }
            }
        }
        context_gettvchannellist->deleteLater();
    });
    if (m_flagKodiConfigured) {
        jsonstring = "{\"jsonrpc\":\"2.0\",\"id\": "+
                QString::number(m_globalKodiRequestID)+",\"method\":\"PVR.GetChannels\","
                                                       " \"params\": {\"channelgroupid\": \"alltv\", \"properties\":"
                                                       "[\"thumbnail\",\"uniqueid\",\"channelnumber\"]}}";
        postRequest(m_KodiClientUrl +":" + m_KodiClientPort +"/jsonrpc", "PVR.GetChannels", jsonstring);
    }
}


void Kodi::getCompleteTVChannelList() {
    QObject* context_getUserPlaylists = new QObject(this);
    QObject::connect(this, &Kodi::requestReady, context_getUserPlaylists,
                     [=](const QVariantMap& map, const QString& rMethod) {
        if (rMethod == "getUserPlaylists" && map.contains("result")) {
            if (map.value("result") == "pong") {
                EntityInterface* entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));
                QString     channelId = "";
                QString     label = "";
                QString     thumbnail = "";
                QString     unqueId = "";
                QString     type = "tvchannellist";
                QStringList commands = {};
                BrowsetvchannelModel* tvchannel = new BrowsetvchannelModel(nullptr, channelId, "",
                                                                           label, unqueId, type, thumbnail,
                                                                           commands);


                for (int i = 0; i < m_KodiTVChannelList.length(); i++) {
                    QString thumbnail = QString::fromStdString(QByteArray::fromPercentEncoding(
                                                                   m_KodiTVChannelList[i].toMap().value("thumbnail").toString().toUtf8())
                                                               .toStdString()).mid(8);
                    if (thumbnail.contains("127.0.0.1")) {
                        thumbnail = thumbnail.replace("127.0.0.1", m_KodiClientUrl.mid(7));
                    }
                    QStringList commands = {"PLAY"};
                    tvchannel->addtvchannelItem(m_KodiTVChannelList[i].toMap().value("channelid").toString(), "",
                                                m_KodiTVChannelList[i].toMap().value("label").toString(), "",
                                                type, thumbnail, commands);
                }
                if (entity) {
                    MediaPlayerInterface* me = static_cast<MediaPlayerInterface*>(entity->getSpecificInterface());
                    me->setBrowseModel(tvchannel);
                }
            }
        }
        context_getUserPlaylists->deleteLater();
    });
    qCDebug(m_logCategory) << "GET USERS PLAYLIST";

    if (m_flagKodiConfigured) {
        QString jsonstring;
        postRequest(m_KodiClientUrl +":" + m_KodiClientPort +
                    "/jsonrpc", "getUserPlaylists", "{ \"jsonrpc\": \"2.0\","
                                                    " \"method\": \"JSONRPC.Ping\", \"params\": {  },"
                                                    " \"id\":" +QString::number(m_globalKodiRequestID)+ "}");
    }
}

void Kodi::getCurrentPlayer() {
    QObject* context = new QObject(this);
    QString method = "Player.GetActivePlayers";
    QString thumbnail = "";

    QObject::connect(this, &Kodi::requestReady, context, [=](const QVariantMap& map, const QString& rMethod) {
        EntityInterface* entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));
        if (rMethod == "Player.GetActivePlayers") {
            if (entity) {
                if (map.contains("result")) {
                    if (map.value("result").toList().count() != 0) {
                        QString jsonstring;
                        m_currentkodiplayerid = map.value("result").toList().value(0).
                                toMap().value("playerid").toInt();
                        if (m_currentkodiplayerid > 0) {
                            m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetItem;
                        }
                        m_currentkodiplayertype = map.value("result").toList().value(0).
                                toMap().value("type").toString();
                        if (m_currentkodiplayertype == "video") {
                            jsonstring = "{\"jsonrpc\": \"2.0\", \"method\": \"Player.GetItem\","
                                         "\"params\":{ \"properties\": [\"title\", \"album\", \"artist\","
                                         " \"season\", \"episode\", \"duration\", \"showtitle\", \"tvshowid\","
                                         " \"thumbnail\", \"file\", \"fanart\", \"streamdetails\"],"
                                         " \"playerid\": " + QString::number(m_currentkodiplayerid) + " },"
                                                                                                      " \"id\": "+QString::number(m_globalKodiRequestID)+"}";
                        } else if (m_currentkodiplayertype == "audio") {
                            jsonstring = "{\"jsonrpc\": \"2.0\", \"method\": \"Player.GetItem\","
                                         "\"params\":{ \"properties\": [\"title\", \"album\", \"artist\", "
                                         "\"season\", \"episode\", \"duration\", \"showtitle\", \"tvshowid\", "
                                         "\"thumbnail\", \"file\", \"fanart\", \"streamdetails\"], "
                                         "\"playerid\": " + QString::number(m_currentkodiplayerid) + " }, ""\"id\": "
                                                                                                     ""+QString::number(m_globalKodiRequestID)+"}";
                        }
                        // postRequest(m_KodiClientUrl +":" + m_KodiClientPort +"/jsonrpc", "Player.GetItem", jsonstring);
                    }
                }
            }
        } else if (rMethod == "Player.GetItem") {
            if (map.contains("result")) {
                if (map.value("result").toMap().value("item").toMap().contains("type")) {
                    if (map.value("result").toMap().value("item").toMap().value("type") == "channel") {
                        QString jsonstring;
                        QVariant fanart = map.value("result").toMap().value("item").toMap().value("fanart");
                        QVariant id = map.value("result").toMap().value("item").toMap().value("id");
                        QVariant label = map.value("result").toMap().value("item").toMap().value("label");
                        QVariant title = map.value("result").toMap().value("item").toMap().value("title");
                        QVariant type = map.value("result").toMap().value("item").toMap().value("type");
                        m_currentKodiMediaType = type.toString();
                        entity->updateAttrByIndex(MediaPlayerDef::MEDIATYPE, type);
                        // get the track title
                        entity->updateAttrByIndex(MediaPlayerDef::MEDIATITLE, title.toString());
                        // get the artist
                        entity->updateAttrByIndex(MediaPlayerDef::MEDIAARTIST, label.toString());
                        entity->updateAttrByIndex(MediaPlayerDef::STATE, MediaPlayerDef::PLAYING);
                        if (!map.value("result").toMap().value("item").toMap().value("thumbnail").
                                toString().isEmpty()) {
                            m_KodiGetCurrentPlayerThumbnail = map.value("result").toMap().
                                    value("item").toMap().value("thumbnail").toString();
                            m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::PrepareDownload;
                        } else {
                            m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetProperties;
                        }
                    } else if (map.value("result").toMap().value("item").toMap().value("type") == "unknown") {
                        QString jsonstring;
                        QVariant fanart = "";
                        QVariant id = map.value("result").toMap().value("item").toMap().value("tvshowid");
                        QVariant label = map.value("result").toMap().value("item").toMap().value("label");
                        QVariant title = map.value("result").toMap().value("item").toMap().value("title");
                        QVariant type = map.value("result").toMap().value("item").toMap().value("type");
                        m_currentKodiMediaType = type.toString();
                        entity->updateAttrByIndex(MediaPlayerDef::MEDIATYPE, type);
                        // get the track title
                        entity->updateAttrByIndex(MediaPlayerDef::MEDIATITLE, title.toString());
                        // get the artist
                        entity->updateAttrByIndex(MediaPlayerDef::MEDIAARTIST, label.toString());
                        // entity->updateAttrByIndex(MediaPlayerDef::STATE, MediaPlayerDef::PLAYING);
                        if (!map.value("result").toMap().value("item").toMap().value("thumbnail").
                                toString().isEmpty()) {
                            m_KodiGetCurrentPlayerThumbnail = map.value("result").toMap().value("item").
                                    toMap().value("thumbnail").toString();
                            m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::PrepareDownload;
                        } else {
                            m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetProperties;
                        }
                    }
                }
            }
        } else if (rMethod == "Files.PrepareDownload") {
            m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetProperties;
            if (map.contains("result")) {
                if (map.value("result").toMap().value("protocol") ==
                        "http" && map.value("result").toMap().value("mode") == "redirect") {
                    entity->updateAttrByIndex(MediaPlayerDef::MEDIAIMAGE,
                                              m_KodiClientUrl+":"+m_KodiClientPort+
                                              "/"+map.value("result").toMap().value("details").toMap().
                                              value("path").toString());
                    // m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetProperties;
                } else {
                    // m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetProperties;
                }
            } else {
                // m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetProperties;
            }
        } else if (rMethod == "Player.GetProperties") {
            if (map.contains("result")) {
                m_KodiGetCurrentPlayerState = m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetItem;
                if (map.value("result").toMap().contains("totaltime")) {
                    int hours = map.value("result").toMap().value("totaltime").toMap().value("hours").toInt();
                    int milliseconds = map.value("result").toMap().value("totaltime").toMap().
                            value("milliseconds").toInt();
                    int minutes = map.value("result").toMap().value("totaltime").toMap().value("minutes").toInt();
                    int seconds = map.value("result").toMap().value("totaltime").toMap().value("seconds").toInt();
                    int totalmilliseconds = (hours*3600000) + (minutes*60000) + (seconds*1000) + milliseconds;
                    entity->updateAttrByIndex(MediaPlayerDef::MEDIADURATION, totalmilliseconds/1000);
                    // entity->updateAttrByIndex(MediaPlayerDef::MEDIAPROGRESS, totalmilliseconds/1000);
                }
                if (map.value("result").toMap().contains("time")) {
                    int hours = map.value("result").toMap().value("time").toMap().value("hours").toInt();
                    int milliseconds = map.value("result").toMap().value("time").toMap().value("milliseconds").toInt();
                    int minutes = map.value("result").toMap().value("time").toMap().value("minutes").toInt();
                    int seconds = map.value("result").toMap().value("time").toMap().value("seconds").toInt();
                    int totalmilliseconds = (hours*3600000) + (minutes*60000) + (seconds*1000) + milliseconds;
                    entity->updateAttrByIndex(MediaPlayerDef::MEDIAPROGRESS, totalmilliseconds/1000);
                    m_progressBarPosition = totalmilliseconds/1000;
                }
                if (map.value("result").toMap().contains("speed")) {
                    if (map.value("result").toMap().value("speed").toInt() > 0)  {
                        m_progressBarTimer->stop();
                        m_progressBarTimer->start();
                        entity->updateAttrByIndex(MediaPlayerDef::STATE, MediaPlayerDef::PLAYING);
                    }
                }
            } else {
                m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetItem;
            }
        }
        context->deleteLater();
    });

    if (m_flagKodiConfigured) {
        if (m_KodiGetCurrentPlayerState == KodiGetCurrentPlayerState::GetActivePlayers || m_KodiGetCurrentPlayerState == KodiGetCurrentPlayerState::Stopped) {
            postRequest(m_KodiClientUrl +":" + m_KodiClientPort +"/jsonrpc", method, m_globalKodiRequestID);
        } else if (m_KodiGetCurrentPlayerState == KodiGetCurrentPlayerState::GetItem) {
            QString jsonstring;
            if (m_currentkodiplayertype == "video") {
                jsonstring = "{\"jsonrpc\": \"2.0\", \"method\": \"Player.GetItem\",\"params\":"
                             "{ \"properties\": [\"title\", \"album\", \"artist\", \"season\", \"episode\","
                             " \"duration\", \"showtitle\", \"tvshowid\", \"thumbnail\", \"file\", \"fanart\","
                             " \"streamdetails\"], \"playerid\": "
                             "" + QString::number(m_currentkodiplayerid) + " }, "
                                                                           "\"id\": "+QString::number(m_globalKodiRequestID)+"}";
            } else if (m_currentkodiplayertype == "audio") {
                jsonstring = "{\"jsonrpc\": \"2.0\", \"method\": \"Player.GetItem\",\"params\":"
                             "{ \"properties\": [\"title\", \"album\", \"artist\", \"season\", "
                             "\"episode\", \"duration\", \"showtitle\", \"tvshowid\", \"thumbnail\", "
                             "\"file\", \"fanart\", \"streamdetails\"], \"playerid\": "
                             "" + QString::number(m_currentkodiplayerid) + " }, \"id\": "
                                                                           ""+QString::number(m_globalKodiRequestID)+"}";
            }
            postRequest(m_KodiClientUrl +":" + m_KodiClientPort +"/jsonrpc", "Player.GetItem", jsonstring);
        } else if (m_KodiGetCurrentPlayerState == KodiGetCurrentPlayerState::GetProperties) {
            QString jsonstring = "{\"jsonrpc\": \"2.0\", \"method\": \"Player.GetProperties\", "
                                 "\"params\": { \"playerid\":" +QString::number(m_currentkodiplayerid) +", "
                                                                                                        "\"properties\": [\"totaltime\", \"time\", \"speed\"] }, "
                                                                                                        "\"id\": "+QString::number(m_globalKodiRequestID)+"}";
            postRequest(m_KodiClientUrl +":" + m_KodiClientPort +"/jsonrpc", "Player.GetProperties", jsonstring);
        } else if (m_KodiGetCurrentPlayerState == KodiGetCurrentPlayerState::PrepareDownload) {
            QString jsonstring = "{\"jsonrpc\": \"2.0\", \"method\": \"Files.PrepareDownload\", "
                                 "\"params\": { \"path\": \"" +m_KodiGetCurrentPlayerThumbnail+"\" }, "
                                                                                               "\"id\": "+QString::number(m_globalKodiRequestID)+"}";
            postRequest(m_KodiClientUrl +":" + m_KodiClientPort +"/jsonrpc", "Files.PrepareDownload", jsonstring);
        }
    }
}
void Kodi::getRequestWithAuthentication(const QString& url, const QString& method, const QString& user, const QString& password) {
    // create new networkacces manager and request
    QNetworkAccessManager* manager = new QNetworkAccessManager(this);
    QNetworkRequest        request;
    QObject* context_getRequestwitchAuthentication = new QObject(this);
    // connect to finish signal
    QObject::connect(manager, &QNetworkAccessManager::finished, context_getRequestwitchAuthentication,
                     [=](QNetworkReply* reply) {
        if (reply->error()) {
            QString errorString = reply->errorString();
            qCWarning(m_logCategory) << errorString;
        }

        QString     answer = reply->readAll();
        if (answer != "") {
            emit requestReadyQstring(answer, method);
        }
        reply->deleteLater();
        context_getRequestwitchAuthentication->deleteLater();
        manager->deleteLater();
    });

    QObject::connect(
                manager, &QNetworkAccessManager::networkAccessibleChanged, context_getRequestwitchAuthentication,
                [=](QNetworkAccessManager::NetworkAccessibility accessibility) {
        qCDebug(m_logCategory) << accessibility; });

    // set headers
    QString concatenated = user+":"+password;
    QByteArray data = concatenated.toLocal8Bit().toBase64();
    QString headerData = "Basic " + data;
    request.setRawHeader("Authorization", headerData.toLocal8Bit());
    // set the URL
    // url = "/v1/me/player"
    // params = "?q=stringquery&limit=20"
    request.setUrl(QUrl(url));
    // send the get request
    manager->get(request);
}

void Kodi::sendCommand(const QString& type, const QString& entityId, int command, const QVariant& param) {
    if (!(type == "media_player" && entityId == m_entityId)) {
        return;
    }
    QObject* context = new QObject(this);
    EntityInterface* entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));
    if (command == MediaPlayerDef::C_PLAY) {
    } else if (command == MediaPlayerDef::C_PLAY_ITEM) {
        if (param.toMap().value("type") == "playlist" || param.toMap().value("type") == "track") {
            QObject::connect(this, &Kodi::requestReady, context, [=](const QVariantMap& map, const QString& rUrl) {
                if (rUrl == "Player.Open") {
                    if (map.contains("result")) {
                        if (map.value("result") == "OK") {
                            getCurrentPlayer();
                        }
                    }
                }});
            QString jsonstring = "{\"jsonrpc\": \"2.0\", \"method\": \"Player.Open\",\"params\":"
                                 " {\"item\":{\"channelid\": "+param.toMap().value("id").toString()+"}},"
                                                                                                    " \"id\": "+QString::number(m_globalKodiRequestID)+"}";
            qCDebug(m_logCategory) << jsonstring;
            postRequest(m_KodiClientUrl +":" + m_KodiClientPort +"/jsonrpc", "Player.Open", jsonstring);
        } /*else {
                            if (param.toMap().contains("type")) {
                                if (param.toMap().value("type").toString() == "track") {
                                    QString  url = "/v1/tracks/";
                                    QObject* context = new QObject(this);
                                    QObject::connect(this, &Kodi::requestReady, context,
                                                     [=](const QVariantMap& map, const QString& rUrl) {
                                        if (rUrl == url) {
                                            qCDebug(m_logCategory) << "PLAY MEDIA" << map.value("uri").toString();
                                            QVariantMap rMap;
                                            QStringList rList;
                                            rList.append(map.value("uri").toString());
                                            rMap.insert("uris", rList);
                                            QJsonDocument doc = QJsonDocument::fromVariant(rMap);
                                            QString       message = doc.toJson(QJsonDocument::JsonFormat::Compact);
                                            qCDebug(m_logCategory) << message;
                                            putRequest("/v1/me/player/play", message);
                                        }
                                        context->deleteLater();
                                    });
                                    getRequest(url, param.toMap().value("id").toString());
                                } else if (param.toMap().value("type").toString() == "album") {
                                    QString  url = "/v1/albums/";
                                    QObject* context = new QObject(this);
                                    QObject::connect(this, &Kodi::requestReady, context,
                                                     [=](const QVariantMap& map, const QString& rUrl) {
                                        if (rUrl == url) {
                                            QString url = "/v1/me/player/play";
                                            qCDebug(m_logCategory) << "PLAY MEDIA" << map.value("uri").toString();
                                            QVariantMap rMap;
                                            rMap.insert("context_uri", map.value("uri").toString());
                                            QJsonDocument doc = QJsonDocument::fromVariant(rMap);
                                            QString       message = doc.toJson(QJsonDocument::JsonFormat::Compact);
                                            qCDebug(m_logCategory) << message;
                                            putRequest("/v1/me/player/play", message);
                                        }
                                        context->deleteLater();
                                    });
                                    getRequest(url, param.toMap().value("id").toString());
                                } else if (param.toMap().value("type").toString() == "artist") {
                                    QString  url = "/v1/artists/";
                                    QObject* context = new QObject(this);
                                    QObject::connect(this, &Kodi::requestReady, context,
                                                     [=](const QVariantMap& map, const QString& rUrl) {
                                        if (rUrl == url) {
                                            QString url = "/v1/me/player/play";
                                            qCDebug(m_logCategory) << "PLAY MEDIA" << map.value("uri").toString();
                                            QVariantMap rMap;
                                            rMap.insert("context_uri", map.value("uri").toString());
                                            QJsonDocument doc = QJsonDocument::fromVariant(rMap);
                                            QString       message = doc.toJson(QJsonDocument::JsonFormat::Compact);
                                            qCDebug(m_logCategory) << message;
                                            putRequest("/v1/me/player/play", message);
                                        }
                                        context->deleteLater();
                                    });
                                    getRequest(url, param.toMap().value("id").toString());
                                } else if (param.toMap().value("type").toString() == "playlist") {
                                    QString  url = "/v1/playlists/";
                                    QObject* context = new QObject(this);
                                    QObject::connect(this, &Kodi::requestReady, context,
                                                     [=](const QVariantMap& map, const QString& rUrl) {
                                        if (rUrl == url) {
                                            QString url = "/v1/me/player/play";
                                            qCDebug(m_logCategory) << "PLAY MEDIA" << map.value("uri").toString();
                                            QVariantMap rMap;
                                            rMap.insert("context_uri", map.value("uri").toString());
                                            QJsonDocument doc = QJsonDocument::fromVariant(rMap);
                                            QString       message = doc.toJson(QJsonDocument::JsonFormat::Compact);
                                            qCDebug(m_logCategory) << message;
                                            putRequest("/v1/me/player/play", message);
                                        }
                                        context->deleteLater();
                                    });
                                    getRequest(url, param.toMap().value("id").toString());
                                }
                            }
                        }*/
    } else if (command == MediaPlayerDef::C_QUEUE) {
        /*if (param.toMap().contains("type")) {
                            if (param.toMap().value("type").toString() == "track") {
                                QString  url = "/v1/tracks/";
                                QObject* context = new QObject(this);
                                QObject::connect(this, &Kodi::requestReady, context,
                                                 [=](const QVariantMap& map, const QString& rUrl) {
                                    if (rUrl == url) {
                                        qCDebug(m_logCategory) << "QUEUE MEDIA" << map.value("uri").toString();
                                        QString message = "?uri=" + map.value("uri").toString();
                                        //postRequest("/v1/me/player/queue", message);
                                    }
                                    context->deleteLater();
                                });
                                getRequest(url, param.toMap().value("id").toString());
                            }
                        }*/
    } else if (command == MediaPlayerDef::C_PAUSE) {
        QObject::connect(this, &Kodi::requestReady, context, [=](const QVariantMap& map, const QString& rUrl) {
            if (rUrl == "Player.Stop") {
                if (map.contains("result")) {
                    if (map.value("result") == "OK") {
                        m_progressBarTimer->stop();
                        m_currentkodiplayertype ="unknown";
                        m_currentkodiplayerid = -1;
                        m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::Stopped;
                        entity->updateAttrByIndex(MediaPlayerDef::STATE, MediaPlayerDef::IDLE);
                        entity->updateAttrByIndex(MediaPlayerDef::MEDIAIMAGE, "");
                        // get the track title
                        entity->updateAttrByIndex(MediaPlayerDef::MEDIATITLE, "");
                        // get the artist
                        entity->updateAttrByIndex(MediaPlayerDef::MEDIAARTIST, "");
                        getCurrentPlayer();
                    }
                }
            }});
        QString jsonstring = "{\"jsonrpc\": \"2.0\", \"method\": \"Player.Stop\","
                             " \"params\": { \"playerid\": " + QString::number(m_currentkodiplayerid) + " },"
                                                                                                        " \"id\": "+QString::number(m_globalKodiRequestID)+"}";
        postRequest(m_KodiClientUrl +":" + m_KodiClientPort +"/jsonrpc", "Player.Stop", jsonstring);
    } else if (command == MediaPlayerDef::C_NEXT) {
        if (m_currentKodiMediaType == "channel") {
            QObject::connect(this, &Kodi::requestReady, context, [=](const QVariantMap& map, const QString& rUrl) {
                if (rUrl == "Input.ExecuteAction") {
                    if (map.contains("result")) {
                        if (map.value("result") == "OK") {
                            m_progressBarTimer->stop();
                            getCurrentPlayer();
                        }
                    }
                }});
            QString jsonstring = "{\"jsonrpc\": \"2.0\", \"method\":"
                                 " \"Input.ExecuteAction\",\"params\": "
                                 "{ \"action\": \"channelup\" }, \"id\":"
                                 " "+QString::number(m_globalKodiRequestID)+"}";
            postRequest(m_KodiClientUrl +":" + m_KodiClientPort +"/jsonrpc", "Input.ExecuteAction", jsonstring);
        }
    } else if (command == MediaPlayerDef::C_PREVIOUS) {
        if (m_currentKodiMediaType == "channel") {
            QObject::connect(this, &Kodi::requestReady, context, [=](const QVariantMap& map, const QString& rUrl) {
                if (rUrl == "Input.ExecuteAction") {
                    if (map.contains("result")) {
                        if (map.value("result") == "OK") {
                            m_progressBarTimer->stop();
                            getCurrentPlayer();
                        }
                    }
                }});
            QString jsonstring = "{\"jsonrpc\": \"2.0\","
                                 " \"method\": \"Input.ExecuteAction\","
                                 "\"params\": { \"action\": \"channeldown\" }, "
                                 "\"id\": "+QString::number(m_globalKodiRequestID)+"}";
            postRequest(m_KodiClientUrl +":" + m_KodiClientPort +"/jsonrpc", "Input.ExecuteAction", jsonstring);
        }
    } else if (command == MediaPlayerDef::C_VOLUME_SET) {
        // putRequest("/v1/me/player/volume?volume_percent=" + param.toString(), "");
    } else if (command == MediaPlayerDef::C_SEARCH) {
        // search(param.toString());
    } else if (command == MediaPlayerDef::C_GETALBUM) {
        // getAlbum(param.toString());
    } else if (command == MediaPlayerDef::C_GETTVCHANNELLIST) {
        if (param == "all") {
            getCompleteTVChannelList();
        } else {
            getSingleTVChannelList(param.toString());
        }
    } else if (command == MediaPlayerDef::C_GETPLAYLIST) {
        if (param == "user") {
            getCompleteTVChannelList();
        } else {
            getSingleTVChannelList(param.toString());
        }
    }
}

void Kodi::updateEntity(const QString& entity_id, const QVariantMap& attr) {
    EntityInterface* entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(entity_id));
    if (entity) {
        // update the media player
        entity->updateAttrByIndex(MediaPlayerDef::Attributes::STATE, attr.value("state").toInt());
        entity->updateAttrByIndex(MediaPlayerDef::Attributes::SOURCE, attr.value("device").toString());
        entity->updateAttrByIndex(MediaPlayerDef::Attributes::VOLUME, attr.value("volume").toInt());
        entity->updateAttrByIndex(MediaPlayerDef::Attributes::MEDIATITLE, attr.value("title").toString());
        entity->updateAttrByIndex(MediaPlayerDef::Attributes::MEDIAARTIST, attr.value("artist").toString());
        entity->updateAttrByIndex(MediaPlayerDef::Attributes::MEDIAIMAGE, attr.value("image").toString());
    }
}

void Kodi::postRequest(const QString& url, const QString& method, const int& requestid) {
    // create new networkacces manager and request
    QNetworkAccessManager* manager = new QNetworkAccessManager(this);
    QUrl serviceUrl = QUrl(url);
    QNetworkRequest request(serviceUrl);
    QObject* context = new QObject(this);
    // connect to finish signal
    QObject::connect(manager, &QNetworkAccessManager::finished, context, [=](QNetworkReply* reply) {
        if (reply->error()) {
            QString errorString = reply->errorString();
            qCWarning(m_logCategory) << errorString;
        }
        QString     answer = reply->readAll();
        QVariantMap map;
        if (answer != "") {
            // convert to json
            QJsonParseError parseerror;
            QJsonDocument   doc = QJsonDocument::fromJson(answer.toUtf8(), &parseerror);
            if (parseerror.error != QJsonParseError::NoError) {
                qCWarning(m_logCategory) << "JSON error : " << parseerror.errorString();
                return;
            }
            // createa a map object
            map = doc.toVariant().toMap();
            emit requestReady(map, method);
        }
        reply->deleteLater();
        context->deleteLater();
        manager->deleteLater();
    });

    QObject::connect(
                manager, &QNetworkAccessManager::networkAccessibleChanged, context,
                [=](QNetworkAccessManager::NetworkAccessibility accessibility) {
        qCDebug(m_logCategory) << accessibility; });

    QJsonObject json;
    json.insert("jsonrpc", "2.0");
    json.insert("method", method);
    json.insert("id", requestid);
    QJsonDocument jsonDoc(json);
    QByteArray jsonData = jsonDoc.toJson();
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setHeader(QNetworkRequest::ContentLengthHeader, QByteArray::number(jsonData.size()));

    // send the get request
    manager->post(request, jsonData);
}


void Kodi::postRequest(const QString& url, const QString& method, const QString& param) {
    // create new networkacces manager and request
    QNetworkAccessManager* manager = new QNetworkAccessManager(this);
    QUrl serviceUrl = QUrl(url);
    QNetworkRequest request(serviceUrl);
    QObject* context = new QObject(this);
    // connect to finish signal
    QObject::connect(manager, &QNetworkAccessManager::finished, context, [=](QNetworkReply* reply) {
        if (reply->error()) {
            QString errorString = reply->errorString();
            qCWarning(m_logCategory) << errorString;
        }
        QString     answer = reply->readAll();
        QVariantMap map;
        if (answer != "") {
            // convert to json
            QJsonParseError parseerror;
            QJsonDocument   doc = QJsonDocument::fromJson(answer.toUtf8(), &parseerror);
            if (parseerror.error != QJsonParseError::NoError) {
                qCWarning(m_logCategory) << "JSON error : " << parseerror.errorString();
                return;
            }
            // createa a map object
            map = doc.toVariant().toMap();
            if (method == "tvprogramm") {
                emit requestReadyParser(doc, method);
            } else {
                emit requestReady(map, method);
            }
        }
        reply->deleteLater();
        context->deleteLater();
        manager->deleteLater();
    });

    QObject::connect(
                manager, &QNetworkAccessManager::networkAccessibleChanged, context,
                [=](QNetworkAccessManager::NetworkAccessibility accessibility) {
        qCDebug(m_logCategory) << accessibility; });

    // set headers
    QByteArray paramutf8 = param.toUtf8();

    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setHeader(QNetworkRequest::ContentLengthHeader, QByteArray::number(paramutf8.size()));

    // send the get request
    manager->post(request, paramutf8);
}



void Kodi::onPollingTimerTimeout() {
    getCurrentPlayer();
    getTVEPGfromTVHeadend();
}

void Kodi::onProgressBarTimerTimeout() {
    EntityInterface* entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));
    if (entity) {
        m_progressBarPosition++;
        entity->updateAttrByIndex(MediaPlayerDef::MEDIAPROGRESS, m_progressBarPosition);
    }
}
