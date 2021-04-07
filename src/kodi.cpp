/******************************************************************************
 *
 * Copyright (C) 2020 Michael Löcher <MichaelLoercher@web.de>
 *
 *
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

#include <QDataStream>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QProcess>
#include <QTcpSocket>
#include <QTextCodec>
#include <QUrlQuery>
#include <QXmlStreamReader>

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
            // TODO(milo) combine host & port configuration into kodiclient_url?
            QString host = map.value("kodiclient_url").toString();
            int     port = map.value("kodiclient_port").toInt();
            if (!host.isEmpty()) {
                m_kodiJSONRPCUrl.setScheme("http");
                m_kodiJSONRPCUrl.setHost(host);
                m_kodiJSONRPCUrl.setPort(port);
                m_kodiJSONRPCUrl.setPath("/jsonrpc");

                QString password = map.value("kodiclient_password").toString();
                if (!password.isEmpty()) {
                    m_kodiJSONRPCUrl.setUserName(map.value("kodiclient_user").toString());
                    m_kodiJSONRPCUrl.setPassword(password);
                }
            }
            host = map.value("kodiclient_url").toString();
            port = map.value("kodieventserver_port").toInt();
            if (!host.isEmpty()) {
                m_kodiEventServerUrl.setScheme("http");
                m_kodiEventServerUrl.setHost(host);
                m_kodiEventServerUrl.setPort(port);

                QString password = map.value("kodiclient_password").toString();
                if (!password.isEmpty()) {
                    m_kodiEventServerUrl.setUserName(map.value("kodiclient_user").toString());
                    m_kodiEventServerUrl.setPassword(password);
                }
            }
            for (auto& ob : map.value("epgchannels").toString().split(",")) {
                m_epgChannelList.append(ob.toInt());
            }
            // TODO(milo) combine host & port configuration into tvheadendclient_url?
            host = map.value("tvheadendclient_url").toString();
            port = map.value("tvheadendclient_port").toInt();
            if (!host.isEmpty()) {
                m_tvheadendJSONUrl.setScheme("http");
                m_tvheadendJSONUrl.setHost(host);
                m_tvheadendJSONUrl.setPort(port);

                QString password = map.value("tvheadendclient_password").toString();
                if (!password.isEmpty()) {
                    m_tvheadendJSONUrl.setUserName(map.value("tvheadendclient_user").toString());
                    m_tvheadendJSONUrl.setPassword(password);
                }
            }

            m_entityId = map.value("entity_id").toString();
            if (m_entityId.isEmpty()) {
                m_entityId = "media_player.kodi";
                qCWarning(m_logCategory) << "Property 'entity_id' not defined in integration. Using default:"
                                         << m_entityId;
            }
            // only one Kodi instance supported per integration definition!
            break;
        }
    }

    if (m_kodiJSONRPCUrl.isEmpty()) {
        qCCritical(m_logCategory) << "Error loading Kodi integration: kodiclient_url not configured!";
        return;
    }

    context_kodi = this;
    networkManagerTvHeadend = new QNetworkAccessManager(context_kodi);
    networkManagerKodi = new QNetworkAccessManager(context_kodi);
    manager = new QNetworkConfigurationManager(context_kodi);
    m_pollingTimer = new QTimer(context_kodi);
    m_pollingEPGLoadTimer = new QTimer(context_kodi);
    m_progressBarTimer = new QTimer(context_kodi);
    for (QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
        if (iface.type() == QNetworkInterface::Wifi) {
            qCDebug(m_logCategory) << iface.humanReadableName() << "(" << iface.name() << ")"
                                   << "is up:" << iface.flags().testFlag(QNetworkInterface::IsUp)
                                   << "is running:" << iface.flags().testFlag(QNetworkInterface::IsRunning);
            m_iface = iface;
        }
    }
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
                      << "UP"
                      << "DOWN"
                      << "OK"
                      << "LEFT"
                      << "RIGHT"
                      << "BACK"
                      << "MENU"
                      << "NEXT"
                      << "SEEK"
                      << "CHANNEL_UP"
                      << "CHANNEL_DOWN"
                      << "SHUFFLE"
                      << "SEARCH"
                      << "MEDIAPLAYEREPGVIEW"
                      << "MEDIAPLAYERREMOTE"
                      << "TVCHANNELLIST";
    addAvailableEntity(m_entityId, "media_player", integrationId(), friendlyName(), supportedFeatures);
}

void Kodi::connect() {
    qCDebug(m_logCategory) << manager->isOnline();
    m_firstrun = true;

    if (!m_iface.flags().testFlag(QNetworkInterface::IsUp) && !m_iface.flags().testFlag(QNetworkInterface::IsRunning)) {
        m_notifications->add(
            true, tr("Cannot connect to ").append(friendlyName()).append("."), tr("Reconnect"),
            [](QObject* param) {
                Integration* i = qobject_cast<Integration*>(param);
                i->connect();
            },
            context_kodi);

    } else {
        QObject::connect(context_kodi, &Kodi::requestReadyKodiConnectionCheck, context_kodi,
                         &Kodi::kodiconnectioncheck);
        QObject::connect(context_kodi, &Kodi::requestReadyTvheadendConnectionCheck, context_kodi,
                         &Kodi::Tvheadendconnectioncheck);

        setState(CONNECTING);

        qCDebug(m_logCategory) << "STARTING Kodi";
        if (!m_tvheadendJSONUrl.isEmpty()) {
            m_flagTVHeadendConfigured = true;

            tvheadendGetRequest("/api/serverinfo", {});
        } else {
            qCDebug(m_logCategory) << "TVHeadend not confgured";
            m_flagTVHeadendConfigured = false;
        }
        if (!m_kodiJSONRPCUrl.isEmpty()) {
            m_flagKodiConfigured = true;
            postRequest(
                "{ \"jsonrpc\": \"2.0\","
                " \"method\": \"JSONRPC.Ping\", \"params\": {  },"
                " \"id\":\"ConnectionCheck\"}");
        } else {
            if (_networktries == MAX_CONNECTIONTRY) {
                _networktries = 0;
                m_flagKodiConfigured = false;
                m_notifications->add(
                    true, tr("Cannot connect to ").append(friendlyName()).append("."), tr("Reconnect"),
                    [](QObject* param) {
                        Integration* i = qobject_cast<Integration*>(param);
                        i->connect();
                    },
                    context_kodi);
                disconnect();
                qCWarning(m_logCategory) << "Kodi not configured";
            } else {
                _networktries++;
                postRequest(
                    "{ \"jsonrpc\": \"2.0\","
                    " \"method\": \"JSONRPC.Ping\", \"params\": {  },"
                    " \"id\":\"ConnectionCheck\"}");
            }
        }
    }
}

void Kodi::clearMediaPlayerEntity() {
    EntityInterface* entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));
    if (!entity) {
        return;
    }
    entity->updateAttrByIndex(MediaPlayerDef::MEDIATYPE, "");
    // get the track title
    entity->updateAttrByIndex(MediaPlayerDef::MEDIATITLE, "");
    // get the artist
    entity->updateAttrByIndex(MediaPlayerDef::MEDIAARTIST, "");
    entity->updateAttrByIndex(MediaPlayerDef::MEDIAIMAGE, "");
    entity->updateAttrByIndex(MediaPlayerDef::STATE, MediaPlayerDef::States::IDLE);
}
void Kodi::clientDisconnected() {
    if (m_tcpSocketKodiEventServer->state() == QTcpSocket::ConnectedState) {
        m_flagKodiEventServerOnline = true;
    } else {
        m_flagKodiEventServerOnline = false;
        m_tcpSocketKodiEventServer->close();
        QObject::disconnect(m_tcpSocketKodiEventServer, &QTcpSocket::readyRead, context_kodi, &Kodi::readTcpData);
        QObject::disconnect(m_tcpSocketKodiEventServer, &QTcpSocket::disconnected, context_kodi,
                            &Kodi::clientDisconnected);
    }
}

void Kodi::readTcpData() {
    QString         reply = m_tcpSocketKodiEventServer->readAll();
    QJsonParseError parseerror;
    QJsonDocument   doc = QJsonDocument::fromJson(reply.toUtf8(), &parseerror);
    if (parseerror.error != QJsonParseError::NoError) {
        qCWarning(m_logCategory) << "JSON error : " << parseerror.errorString();
        return;
    }
    if (!doc.isEmpty()) {
        QVariantMap replyMap = doc.toVariant().toMap();
        if (replyMap.value("jsonrpc") == "2.0" && replyMap.contains("method")) {
            if (replyMap.value("method") == "System.OnQuit") {
                m_flagKodiOnline = false;
                m_flagTVHeadendOnline = false;
                disconnect();
            } else if (replyMap.value("method") == "Player.OnResume") {
                // m_flag = false;
                m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetActivePlayers;
                EntityInterface* entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));
                entity->updateAttrByIndex(MediaPlayerDef::STATE, MediaPlayerDef::States::PLAYING);
                getCurrentPlayer();
            } else if (replyMap.value("method") == "Player.OnPlay") {
            }
        }
    }
}

void Kodi::disconnect() {
    if (m_kodireply != nullptr) {
        if (!m_kodireply->isFinished()) {
            m_kodireply->abort();
            // m_kodireply->deleteLater();
            // m_kodireply = nullptr;
        }
    }

    if (m_tvreply != nullptr) {
        if (!m_tvreply->isFinished()) {
            m_tvreply->abort();
            // m_tvreply->deleteLater();
            // m_tvreply = nullptr;
        }
    }

    if (m_pollingTimer->isActive()) {
        m_pollingTimer->stop();
        QObject::disconnect(m_pollingTimer, &QTimer::timeout, context_kodi, &Kodi::onPollingTimerTimeout);
    }
    if (m_progressBarTimer->isActive()) {
        m_progressBarTimer->stop();
        QObject::disconnect(m_progressBarTimer, &QTimer::timeout, context_kodi, &Kodi::onProgressBarTimerTimeout);
    }
    if (m_pollingEPGLoadTimer->isActive()) {
        m_pollingEPGLoadTimer->stop();
        QObject::disconnect(m_pollingEPGLoadTimer, &QTimer::timeout, context_kodi, &Kodi::onPollingEPGLoadTimerTimeout);
    }

    if (m_flagKodiEventServerOnline) {
        m_tcpSocketKodiEventServer->close();
        QObject::disconnect(m_tcpSocketKodiEventServer, &QTcpSocket::readyRead, context_kodi, &Kodi::readTcpData);
        QObject::disconnect(m_tcpSocketKodiEventServer, &QTcpSocket::disconnected, context_kodi,
                            &Kodi::clientDisconnected);
    }

    QObject::disconnect(context_kodi, &Kodi::requestReadyKodiConnectionCheck, context_kodi, &Kodi::kodiconnectioncheck);
    QObject::disconnect(context_kodi, &Kodi::requestReadyTvheadendConnectionCheck, context_kodi,
                        &Kodi::Tvheadendconnectioncheck);
    QObject::disconnect(context_kodi, &Kodi::requestReadygetCurrentPlayer, context_kodi, &Kodi::updateCurrentPlayer);
    clearMediaPlayerEntity();
    // m_flagKodiOnline = false;
    /*m_notifications->add(
        true, tr("Cannot connect to ").append(friendlyName()).append("."), tr("Reconnect"),
        [](QObject* param) {
            Integration* i = qobject_cast<Integration*>(param);
            i->connect();
        },
        this);
    disconnect();
    qCWarning(m_logCategory) << "Kodi not reachable";*/
    m_flagKodiOnline = false;
    m_flagTVHeadendOnline = false;
    clearMediaPlayerEntity();
    setState(DISCONNECTED);
}

void Kodi::enterStandby() {
    /*if (!m_reply->isFinished()) {
        m_reply->abort();
    }

    if (!m_tvreply->isFinished()) {
        m_tvreply->abort();
    }

    if (m_pollingTimer->isActive()) {
        m_pollingTimer->stop();
    }
    if (m_progressBarTimer->isActive()) {
        m_progressBarTimer->stop();
    }
    if (m_pollingEPGLoadTimer->isActive()) {
        m_pollingEPGLoadTimer->stop();
    }

    if (m_flagKodiEventServerOnline) {
        m_tcpSocketKodiEventServer->close();
    }
    m_flagKodiOnline = false;
    m_flagTVHeadendOnline = false;*/
    disconnect();
}

void Kodi::leaveStandby() {
    connect();
    /*if (!m_tvheadendJSONUrl.isEmpty()) {
        m_flagTVHeadendConfigured = true;

        tvheadendGetRequest("/api/serverinfo", {}, "getTVHeadendConnectionCheck");
    } else {
        qCDebug(m_logCategory) << "TVHeadend not confgured";
        m_flagTVHeadendConfigured = false;
    }
    if (!m_kodiJSONRPCUrl.isEmpty()) {
        m_flagKodiConfigured = true;
        postRequest("ConnectionCheck",
                    "{ \"jsonrpc\": \"2.0\","
                    " \"method\": \"JSONRPC.Ping\", \"params\": {  },"
                    " \"id\":\"ConnectionCheck\"}");
    } else {
        m_flagKodiConfigured = false;
        m_notifications->add(
            true, tr("Cannot connect to ").append(friendlyName()).append("."), tr("Reconnect"),
            [](QObject* param) {
                Integration* i = qobject_cast<Integration*>(param);
                i->connect();
            },
            this);
        disconnect();
        qCWarning(m_logCategory) << "Kodi not configured";
    }*/
}

void Kodi::getTVEPGfromTVHeadend(int KodiChannelNumber) {
    QObject* context_getTVEPGfromTVHeadend = new QObject(context_kodi);
    QObject::connect(context_kodi, &Kodi::requestReadygetTVEPGfromTVHeadend, context_getTVEPGfromTVHeadend,
                     [=](const QJsonDocument& resultJSONDocument) {
                         // createa a map object
                         m_currentEPG.append(resultJSONDocument.object().value("entries").toVariant().toList());
                         context_getTVEPGfromTVHeadend->deleteLater();
                     });
    if (m_flagTVHeadendOnline && m_mapKodiChannelNumberToTVHeadendUUID.count() > 0) {
        tvheadendGetRequest(
            "/api/epg/events/grid",
            {{"limit", "1000"}, {"channel", m_mapKodiChannelNumberToTVHeadendUUID.value(KodiChannelNumber)}});
    }
}
void Kodi::getSingleTVChannelList(QString param) {
    QObject* context_getSingleTVChannelList = new QObject(context_kodi);

    QString channelnumber = "0";
    for (int i = 0; i < m_KodiTVChannelList.length(); i++) {
        if (m_KodiTVChannelList[i].toMap().value("channelid").toString() == param) {
            channelnumber = m_KodiTVChannelList[i].toMap().value("channelnumber").toString();
        }
    }
    if (channelnumber != "0" && m_flagTVHeadendOnline && m_currentEPG.count() > 0) {
        QObject::connect(
            context_kodi, &Kodi::requestReadygetSingleTVChannelList, context_getSingleTVChannelList,
            [=](const QJsonDocument& resultJSONDocument) {
                EntityInterface* entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));

                QMap<QString, QString> currenttvprogramm;
                for (int i = 0; i < m_currentEPG.length(); i++) {
                    if (m_currentEPG[i].toMap().value("channelNumber") == channelnumber) {
                        currenttvprogramm.insert(m_currentEPG[i].toMap().value("start").toString(),
                                                 m_currentEPG[i].toMap().value("title").toString());
                    }
                }
                if (currenttvprogramm.count() > 0) {
                    int currenttvchannelarrayid = 0;
                    for (int i = 0; i < m_KodiTVChannelList.length(); i++) {
                        if (m_KodiTVChannelList[i].toMap().value("channelid") == param) {
                            currenttvchannelarrayid = i;
                            break;
                        }
                    }
                    QString     id = m_KodiTVChannelList[currenttvchannelarrayid].toMap().value("channelid").toString();
                    QString     title = m_KodiTVChannelList[currenttvchannelarrayid].toMap().value("label").toString();
                    QString     subtitle = "";
                    QString     type = "tvchannellist";
                    QString     time = "";
                    QString     image = fixUrl(QString::fromStdString(QByteArray::fromPercentEncoding(
                                                                      m_KodiTVChannelList[currenttvchannelarrayid]
                                                                          .toMap()
                                                                          .value("thumbnail")
                                                                          .toString()
                                                                          .toUtf8())
                                                                      .toStdString())
                                               .mid(8));
                    QStringList commands = {"PLAY"};
                    /*BrowseTvChannelModel* tvchannel = nullptr;
                    if (entity) {
                        MediaPlayerInterface* me = static_cast<MediaPlayerInterface*>(entity->getSpecificInterface());
                        me->setBrowseModel(tvchannel);
                    }*/

                    BrowseTvChannelModel* tvchannel =
                        new BrowseTvChannelModel(id, time, title, subtitle, type, image, commands, nullptr);

                    for (const auto& key : currenttvprogramm.keys()) {
                        QDateTime timestamp;
                        timestamp.setTime_t(key.toUInt());

                        tvchannel->addtvchannelItem(
                            m_KodiTVChannelList[currenttvchannelarrayid].toMap().value("channelid").toString(),
                            timestamp.toString("hh:mm"), currenttvprogramm.value(key), "", "tvchannel", "", commands);
                    }

                    if (entity) {
                        MediaPlayerInterface* me = static_cast<MediaPlayerInterface*>(entity->getSpecificInterface());
                        me->setBrowseModel(tvchannel);
                    }

                    // context_getSingleTVChannelList->deleteLater();
                    // QObject::disconnect(this, &Kodi::requestReadygetSingleTVChannelList,
                    // context_getSingleTVChannelList, 0);
                } else {
                    // EntityInterface* entity =
                    // static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));
                    int currenttvchannelarrayid = 0;
                    for (int i = 0; i < m_KodiTVChannelList.length(); i++) {
                        if (m_KodiTVChannelList[i].toMap().value("channelid") == param) {
                            // currenttvchannel = m_KodiTVChannelList[i].toMap();
                            currenttvchannelarrayid = i;
                            break;
                        }
                    }
                    QString     id = m_KodiTVChannelList[currenttvchannelarrayid].toMap().value("channelid").toString();
                    QString     title = m_KodiTVChannelList[currenttvchannelarrayid].toMap().value("label").toString();
                    QString     subtitle = "";
                    QString     type = "tvchannellist";
                    QString     image = fixUrl(QString::fromStdString(QByteArray::fromPercentEncoding(
                                                                      m_KodiTVChannelList[currenttvchannelarrayid]
                                                                          .toMap()
                                                                          .value("thumbnail")
                                                                          .toString()
                                                                          .toUtf8())
                                                                      .toStdString())
                                               .mid(8));
                    QStringList commands = {"PLAY"};

                    BrowseTvChannelModel* tvchannel =
                        new BrowseTvChannelModel(id, "", title, subtitle, type, image, commands, nullptr);
                    tvchannel->addtvchannelItem(
                        m_KodiTVChannelList[currenttvchannelarrayid].toMap().value("channelid").toString(), " ",
                        "No programm available", "", "tvchannel", "", commands);
                    if (entity) {
                        MediaPlayerInterface* me = static_cast<MediaPlayerInterface*>(entity->getSpecificInterface());
                        me->setBrowseModel(tvchannel);
                    }
                }

                // }

                context_getSingleTVChannelList->deleteLater();
            });

        postRequest(
            "{ \"jsonrpc\": \"2.0\", \"method\": \"JSONRPC.Ping\","
            " \"params\": {  }, \"id\": \"getSingleTVChannelList\" }");
        /*else if (!m_flagTVHeadendOnline) {
           QObject::connect(
               this, &Kodi::requestReadygetSingleTVChannelList, context_getSingleTVChannelList,
               [=](const QJsonDocument& resultJSONDocument) {
                   if (resultJSONDocument.object().contains("result")) {
                       if (resultJSONDocument.object().value("result") == "pong") {
                           EntityInterface* entity =
                               static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));
                           int currenttvchannelarrayid = 0;
                           for (int i = 0; i < m_KodiTVChannelList.length(); i++) {
                               if (m_KodiTVChannelList[i].toMap().value("channelid") == param) {
                                   // currenttvchannel = m_KodiTVChannelList[i].toMap();
                                   currenttvchannelarrayid = i;
                                   break;
                               }
                           }
                           QString id =
       m_KodiTVChannelList[currenttvchannelarrayid].toMap().value("channelid").toString(); QString title =
       m_KodiTVChannelList[currenttvchannelarrayid].toMap().value("label").toString(); QString subtitle = ""; QString
       type = "tvchannellist"; QString image = fixUrl(QString::fromStdString(QByteArray::fromPercentEncoding(
                                                                             m_KodiTVChannelList[currenttvchannelarrayid]
                                                                                 .toMap()
                                                                                 .value("thumbnail")
                                                                                 .toString()
                                                                                 .toUtf8())
                                                                             .toStdString())
                                                      .mid(8));
                           QStringList commands = {};

                           BrowseTvChannelModel* tvchannel =
                               new BrowseTvChannelModel(id, "", title, subtitle, type, image, commands, nullptr);
                           tvchannel->addtvchannelItem(
                               m_KodiTVChannelList[currenttvchannelarrayid].toMap().value("channelid").toString(), "",
                               "No programm available", "", "tvchannel", "", commands);

                           if (entity) {
                               MediaPlayerInterface* me =
                                   static_cast<MediaPlayerInterface*>(entity->getSpecificInterface());
                               me->setBrowseModel(tvchannel);
                           }
                       }
                   }
                   //}
                   context_getSingleTVChannelList->deleteLater();
                   // QObject::disconnect(this, &Kodi::requestReadygetSingleTVChannelList,
                   // context_getSingleTVChannelList, 0);
               });
           qCDebug(m_logCategory) << "GET USERS PLAYLIST";
           QString jsonstring;

           postRequest(
               "{ \"jsonrpc\": \"2.0\", \"method\": \"JSONRPC.Ping\","
               " \"params\": {  }, \"id\": \"getSingleTVChannelList\" }");
       } */
    } else {
        QObject::connect(
            context_kodi, &Kodi::requestReadygetSingleTVChannelList, context_getSingleTVChannelList,
            [=](const QJsonDocument& resultJSONDocument) {
                if (resultJSONDocument.object().contains("result")) {
                    if (resultJSONDocument.object().value("result") == "pong") {
                        EntityInterface* entity =
                            static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));
                        int currenttvchannelarrayid = 0;
                        for (int i = 0; i < m_KodiTVChannelList.length(); i++) {
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
                        QString image = fixUrl(QString::fromStdString(QByteArray::fromPercentEncoding(
                                                                          m_KodiTVChannelList[currenttvchannelarrayid]
                                                                              .toMap()
                                                                              .value("thumbnail")
                                                                              .toString()
                                                                              .toUtf8())
                                                                          .toStdString())
                                                   .mid(8));
                        QStringList commands = {};

                        BrowseTvChannelModel* tvchannel =
                            new BrowseTvChannelModel(id, "", title, subtitle, type, image, commands, nullptr);
                        tvchannel->addtvchannelItem(
                            m_KodiTVChannelList[currenttvchannelarrayid].toMap().value("channelid").toString(), "",
                            "No programm available", "", "tvchannel", "", commands);
                        if (entity) {
                            MediaPlayerInterface* me =
                                static_cast<MediaPlayerInterface*>(entity->getSpecificInterface());
                            me->setBrowseModel(tvchannel);
                        }
                    }

                    // }
                }
                context_getSingleTVChannelList->deleteLater();
                // QObject::disconnect(this, &Kodi::requestReadygetSingleTVChannelList,
                // context_getSingleTVChannelList, 0);
            });
        qCDebug(m_logCategory) << "GET USERS PLAYLIST";
        QString jsonstring;

        postRequest(
            "{ \"jsonrpc\": \"2.0\", \"method\": \"JSONRPC.Ping\", \"params\": {  }, \"id\": "
            "\"getSingleTVChannelList\" }");
    }
}
void Kodi::getKodiChannelNumberToTVHeadendUUIDMapping() {
    QObject* context_getKodiChannelNumberToTVHeadendUUIDMapping = new QObject(context_kodi);
    QString  jsonstring;
    QObject::connect(
        context_kodi, &Kodi::requestReadygetKodiChannelNumberToTVHeadendUUIDMapping,
        context_getKodiChannelNumberToTVHeadendUUIDMapping, [=](const QJsonDocument& repliedJsonDocument) {
            if (m_KodiTVChannelList.length() > 0 && m_mapKodiChannelNumberToTVHeadendUUID.isEmpty() &&
                m_mapTVHeadendUUIDToKodiChannelNumber.isEmpty()) {
                if (!read(&m_mapKodiChannelNumberToTVHeadendUUID) || !read(&m_mapTVHeadendUUIDToKodiChannelNumber)) {
                    QMap<QString, QString> inv_map;
                    auto                   entries = repliedJsonDocument["entries"];
                    for (auto item : entries.toArray()) {
                        auto obj = item.toObject();
                        inv_map[obj["val"].toString()] = obj["key"].toString();
                    }

                    for (int j = 0; j < m_KodiTVChannelList.length(); j++) {
                        auto it = inv_map.find(m_KodiTVChannelList[j].toMap().values("label")[0].toString());
                        if (it != inv_map.end() &&
                            !m_mapKodiChannelNumberToTVHeadendUUID.contains(
                                m_KodiTVChannelList[j].toMap().value("channelnumber").toInt()) &&
                            !m_mapTVHeadendUUIDToKodiChannelNumber.contains(it.value())) {
                            m_mapKodiChannelNumberToTVHeadendUUID.insert(
                                m_KodiTVChannelList[j].toMap().value("channelnumber").toInt(), it.value());
                            m_mapTVHeadendUUIDToKodiChannelNumber.insert(
                                it.value(), m_KodiTVChannelList[j].toMap().value("channelnumber").toInt());
                        }
                    }
                    write(m_mapKodiChannelNumberToTVHeadendUUID);
                    write(m_mapTVHeadendUUIDToKodiChannelNumber);
                }
            }

            context_getKodiChannelNumberToTVHeadendUUIDMapping->deleteLater();
        });
    tvheadendGetRequest("/api/channel/list", {});
}

void Kodi::getKodiChannelNumberToRadioHeadendUUIDMapping() {
    QObject* context_getKodiChannelNumberToRadioHeadendUUIDMapping = new QObject(context_kodi);
    QString  jsonstring;
    QObject::connect(
        context_kodi, &Kodi::requestReadygetKodiChannelNumberToRadioHeadendUUIDMapping,
        context_getKodiChannelNumberToRadioHeadendUUIDMapping, [=](const QJsonDocument& repliedJsonDocument) {
            if (m_KodiRadioChannelList.length() > 0 && m_mapKodiChannelNumberToRadioHeadendUUID.isEmpty() &&
                m_mapRadioHeadendUUIDToKodiChannelNumber.isEmpty()) {
                if (!read(&m_mapKodiChannelNumberToRadioHeadendUUID) ||
                    !read(&m_mapRadioHeadendUUIDToKodiChannelNumber)) {
                    QMap<QString, QString> inv_map;
                    auto                   entries = repliedJsonDocument["entries"];
                    for (auto item : entries.toArray()) {
                        auto obj = item.toObject();
                        inv_map[obj["val"].toString()] = obj["key"].toString();
                    }

                    for (int j = 0; j < m_KodiRadioChannelList.length(); j++) {
                        auto it = inv_map.find(m_KodiRadioChannelList[j].toMap().values("label")[0].toString());
                        if (it != inv_map.end() &&
                            !m_mapKodiChannelNumberToRadioHeadendUUID.contains(
                                m_KodiRadioChannelList[j].toMap().value("channelnumber").toInt()) &&
                            !m_mapRadioHeadendUUIDToKodiChannelNumber.contains(it.value())) {
                            m_mapKodiChannelNumberToRadioHeadendUUID.insert(
                                m_KodiRadioChannelList[j].toMap().value("channelnumber").toInt(), it.value());
                            m_mapRadioHeadendUUIDToKodiChannelNumber.insert(
                                it.value(), m_KodiRadioChannelList[j].toMap().value("channelnumber").toInt());
                        }
                    }
                    write(m_mapKodiChannelNumberToRadioHeadendUUID);
                    write(m_mapRadioHeadendUUIDToKodiChannelNumber);
                }
            }
            context_getKodiChannelNumberToRadioHeadendUUIDMapping->deleteLater();
        });
    tvheadendGetRequest("/api/channel/list", {});
}

void Kodi::getKodiAvailableRadioChannelList() {
    QObject* context_getgetKodiAvailableRadioChannelList = new QObject(context_kodi);

    QObject::connect(context_kodi, &Kodi::requestReadygetKodiAvailableRadioChannelList,
                     context_getgetKodiAvailableRadioChannelList, [=](const QJsonDocument& resultJSONDocument) {
                         if (resultJSONDocument.object().contains("result")) {
                             QString strJson(resultJSONDocument.toJson(QJsonDocument::Compact));
                             // qCDebug(m_logCategory) << strJson;
                             m_KodiRadioChannelList =
                                 resultJSONDocument.object().value("result")["channels"].toVariant().toList();
                             if (m_flagTVHeadendOnline) {
                                 if (m_mapKodiChannelNumberToRadioHeadendUUID.isEmpty()) {
                                     getKodiChannelNumberToRadioHeadendUUIDMapping();
                                 } else {
                                     qCDebug(m_logCategory) << "m_mapKodiChannelNumberToTVHeadendUUID already loaded";
                                 }
                             } else {
                                 qCDebug(m_logCategory) << "TV Headend not configured";
                             }
                         }
                         context_getgetKodiAvailableRadioChannelList->deleteLater();
                     });
    if (m_flagKodiOnline) {
        QString jsonstring =
            "{\"jsonrpc\":\"2.0\",\"id\": \"getKodiAvailableRadioChannelList\",\"method\":\"PVR.GetChannels\","
            " \"params\": {\"channelgroupid\": \"allradio\", \"properties\":"
            "[\"thumbnail\",\"uniqueid\",\"channelnumber\"]}}";
        postRequest(jsonstring);
    }
}

void Kodi::getKodiAvailableTVChannelList() {
    QObject* context_getgetKodiAvailableTVChannelList = new QObject(context_kodi);

    QObject::connect(context_kodi, &Kodi::requestReadygetKodiAvailableTVChannelList,
                     context_getgetKodiAvailableTVChannelList, [=](const QJsonDocument& resultJSONDocument) {
                         if (resultJSONDocument.object().contains("result")) {
                             QString strJson(resultJSONDocument.toJson(QJsonDocument::Compact));
                             // qCDebug(m_logCategory) << strJson;
                             m_KodiTVChannelList =
                                 resultJSONDocument.object().value("result")["channels"].toVariant().toList();
                             if (m_flagTVHeadendOnline) {
                                 if (m_mapKodiChannelNumberToTVHeadendUUID.isEmpty()) {
                                     getKodiChannelNumberToTVHeadendUUIDMapping();
                                 } else {
                                     qCDebug(m_logCategory) << "m_mapKodiChannelNumberToTVHeadendUUID already loaded";
                                 }
                             } else {
                                 qCDebug(m_logCategory) << "TV Headend not configured";
                             }
                         }
                         context_getgetKodiAvailableTVChannelList->deleteLater();
                     });
    if (m_flagKodiOnline) {
        QString jsonstring =
            "{\"jsonrpc\":\"2.0\",\"id\": \"getKodiAvailableTVChannelList\",\"method\":\"PVR.GetChannels\","
            " \"params\": {\"channelgroupid\": \"alltv\", \"properties\":"
            "[\"thumbnail\",\"uniqueid\",\"channelnumber\"]}}";
        postRequest(jsonstring);
    }
}

void Kodi::getCompleteTVChannelList(QString param) {
    QObject* context_getCompleteTVChannelList = new QObject(context_kodi);

    QObject::connect(
        context_kodi, &Kodi::requestReadygetCompleteTVChannelList, context_getCompleteTVChannelList,
        [=](const QJsonDocument& resultJSONDocument) {


            if (resultJSONDocument.object().contains("result")) {
                if (resultJSONDocument.object().value("result").toString() == "pong") {
                    EntityInterface* entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));

                    if (param == "Radio") {
                        QString               channelId = "";
                        QString               label = "";
                        QString               thumbnail = "";
                        QString               unqueId = "";
                        QString               type = "tvchannellist";
                        QStringList           commands = {};

                        /*BrowseTvChannelModel* tvchannel =
                            new BrowseTvChannelModel(channelId, "", label, unqueId, type, thumbnail, commands, nullptr);*/
                        tvchannel->reset();
                        for (int i = 0; i < m_KodiRadioChannelList.length(); i++) {
                            QString thumbnail =
                                fixUrl(QString::fromStdString(
                                           QByteArray::fromPercentEncoding(
                                               m_KodiRadioChannelList[i].toMap().value("thumbnail").toString().toUtf8())
                                               .toStdString())
                                           .mid(8));
                            QStringList commands = {"PLAY"};
                            tvchannel->addtvchannelItem(m_KodiRadioChannelList[i].toMap().value("channelid").toString(),
                                                        "", m_KodiRadioChannelList[i].toMap().value("label").toString(),
                                                        "", type, thumbnail, commands);
                        }

                        if (entity) {
                            MediaPlayerInterface* me =
                                static_cast<MediaPlayerInterface*>(entity->getSpecificInterface());
                            me->setBrowseModel(tvchannel);
                        }
                    } else {
                        QString               channelId = "";
                        QString               label = "";
                        QString               thumbnail = "";
                        QString               unqueId = "";
                        QString               type = "tvchannellist";
                        QStringList           commands = {};
                        /*BrowseTvChannelModel* tvchannel =
                            new BrowseTvChannelModel(channelId, "", label, unqueId, type, thumbnail, commands, nullptr);*/
tvchannel->reset();
                        for (int i = 0; i < m_KodiTVChannelList.length(); i++) {
                            QString thumbnail =
                                fixUrl(QString::fromStdString(
                                           QByteArray::fromPercentEncoding(
                                               m_KodiTVChannelList[i].toMap().value("thumbnail").toString().toUtf8())
                                               .toStdString())
                                           .mid(8));
                            QStringList commands = {"PLAY"};
                            tvchannel->addtvchannelItem(m_KodiTVChannelList[i].toMap().value("channelid").toString(),
                                                        "", m_KodiTVChannelList[i].toMap().value("label").toString(),
                                                        "", type, thumbnail, commands);
                        }

                        if (entity) {
                            MediaPlayerInterface* me =
                                static_cast<MediaPlayerInterface*>(entity->getSpecificInterface());
                            me->setBrowseModel(tvchannel);
                        }
                    }
                }
            }
            // QObject::disconnect(this, &Kodi::requestReadygetCompleteTVChannelList, context_getCompleteTVChannelList,
            // 0);
            context_getCompleteTVChannelList->deleteLater();
        });
    qCDebug(m_logCategory) << "GET COMPLETE TV CHANNEL LIST";

    if (m_flagKodiOnline) {
        QString jsonstring;
        postRequest(
            "{ \"jsonrpc\": \"2.0\","
            " \"method\": \"JSONRPC.Ping\", \"params\": {  },"
            " \"id\":\"getCompleteTVChannelList\"}");
    }
}

void Kodi::updateCurrentPlayer(const QJsonDocument& resultJSONDocument) {
    // qCDebug(m_logCategory) << "test" << resultJSONDocument.object().value("result")["protocol"].toString();
    EntityInterface*      entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));
    MediaPlayerInterface* me = static_cast<MediaPlayerInterface*>(entity->getSpecificInterface());
    if (resultJSONDocument.object().value("id") == "Player.GetActivePlayers") {
        if (entity) {
            if (resultJSONDocument.object().contains("result")) {
                if (resultJSONDocument.object()["result"].toArray().count() != 0) {
                    m_currentkodiplayerid =
                        resultJSONDocument.object()["result"].toArray()[0].toObject()["playerid"].toInt();
                    if (m_currentkodiplayerid > 0) {
                        m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetItem;
                    }
                    m_currentkodiplayertype =
                        resultJSONDocument.object()["result"].toArray()[0].toObject()["type"].toString();
                    if (m_currentkodiplayertype == "video") {
                        QString jsonstring =
                            "{\"jsonrpc\": \"2.0\", \"method\": \"Player.GetItem\",\"params\":"
                            "{ \"properties\": [\"title\", \"album\", \"artist\", \"season\", \"episode\","
                            " \"duration\", \"showtitle\", \"tvshowid\", \"thumbnail\", \"file\", \"fanart\","
                            " \"streamdetails\"], \"playerid\": "
                            "" +
                            QString::number(m_currentkodiplayerid) + " }, \"id\": \"Player.GetItem\"}";
                        postRequest(jsonstring);
                        // m_flag = true;
                    } else if (m_currentkodiplayertype == "audio") {
                        QString jsonstring =
                            "{\"jsonrpc\": \"2.0\", \"method\": \"Player.GetItem\",\"params\":"
                            "{ \"properties\": [\"title\", \"album\", \"artist\", \"season\", "
                            "\"episode\", \"duration\", \"showtitle\", \"tvshowid\", \"thumbnail\", "
                            "\"file\", \"fanart\", \"streamdetails\"], \"playerid\": "
                            "" +
                            QString::number(m_currentkodiplayerid) + " }, \"id\": \"Player.GetItem\"}";
                        postRequest(jsonstring);
                        // m_flag = true;
                    }
                } else {
                    // m_flag = false;
                }
            } else {
                // m_flag = false;
            }
        }
    } else if (resultJSONDocument.object().value("id") == "Player.GetItem") {
        if (resultJSONDocument.object().contains("result")) {
            if (resultJSONDocument.object().value("result")["item"].toObject().contains("type")) {
                if (me->mediaTitle() == resultJSONDocument.object().value("result")["item"]["title"].toString() &&
                    !m_firstrun) {
                    m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetActivePlayers;
                    // m_flag = false;
                } else {
                    if ((resultJSONDocument.object().value("result")["item"]["type"].toString() == "channel" &&
                         !(entity->state() == MediaPlayerDef::States::IDLE)) ||
                        (resultJSONDocument.object().value("result")["item"]["type"].toString() == "channel" &&
                         m_firstrun)) {
                        QString fanart = resultJSONDocument.object().value("result")["item"]["type"].toString();
                        QString id = resultJSONDocument.object().value("result")["item"]["id"].toString();
                        QString label = resultJSONDocument.object().value("result")["item"]["label"].toString();
                        QString title = resultJSONDocument.object().value("result")["item"]["title"].toString();

                        QString type = resultJSONDocument.object().value("result")["item"]["type"].toString();
                        // m_currentKodiMediaType = type;
                        entity->updateAttrByIndex(MediaPlayerDef::MEDIATYPE, type);
                        // get the track title
                        entity->updateAttrByIndex(MediaPlayerDef::MEDIATITLE, title);
                        // get the artist
                        entity->updateAttrByIndex(MediaPlayerDef::MEDIAARTIST, label);
                        // entity->updateAttrByIndex(MediaPlayerDef::STATE, MediaPlayerDef::PLAYING);
                        if (!resultJSONDocument.object().value("result")["item"]["thumbnail"].toString().isEmpty()) {
                            m_KodiCurrentPlayerThumbnail =
                                resultJSONDocument.object().value("result")["item"]["thumbnail"].toString();
                            m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::PrepareDownload;
                            QString jsonstring =
                                "{\"jsonrpc\": \"2.0\", \"method\": \"Files.PrepareDownload\", "
                                "\"params\": { \"path\": \"" +
                                m_KodiCurrentPlayerThumbnail + "\" }, \"id\": \"Files.PrepareDownload\"}";
                            postRequest(jsonstring);
                            // m_flag = false;
                        } else {
                            m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetProperties;
                            // m_flag = false;
                        }

                        // // m_flag = false;
                    } else {
                        // m_flag = false;
                        m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetActivePlayers;
                    }
                }
            } else {
                // m_flag = false;
                m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetActivePlayers;
            }
        } else {
            // m_flag = false;
            m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetActivePlayers;
        }
    } else if (resultJSONDocument.object().value("id") == "Files.PrepareDownload") {
        m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetProperties;
        if (resultJSONDocument.object().contains("result")) {
            if (resultJSONDocument.object().value("result")["protocol"].toString() == "http" &&
                resultJSONDocument.object().value("result")["mode"].toString() == "redirect") {
                entity->updateAttrByIndex(
                    MediaPlayerDef::MEDIAIMAGE,
                    QString("%1://%2:%3/%4")
                        .arg(m_kodiJSONRPCUrl.scheme(), m_kodiJSONRPCUrl.host())
                        .arg(m_kodiJSONRPCUrl.port())
                        .arg(resultJSONDocument.object().value("result")["details"]["path"].toString()));
                m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetProperties;
                QString jsonstring =
                    "{\"jsonrpc\": \"2.0\", \"method\": \"Player.GetProperties\", "
                    "\"params\": { \"playerid\":" +
                    QString::number(m_currentkodiplayerid) +
                    ", \"properties\": "
                    "[\"totaltime\", \"time\", \"speed\"] }, "
                    "\"id\": \"Player.GetProperties\"}";
                postRequest(jsonstring);
            } else {
                // m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetProperties;
                // // m_flag = false;
            }
        } else {
            // m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetProperties;
            // // m_flag = false;
        }
    } else if (resultJSONDocument.object().value("id") == "Player.GetProperties") {
        if (resultJSONDocument.object().contains("result")) {
            m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetActivePlayers;

            if (resultJSONDocument.object().value("result").toObject().contains("totaltime")) {
                int hours = resultJSONDocument.object().value("result")["totaltime"]["hours"].toInt();
                int milliseconds = resultJSONDocument.object().value("result")["totaltime"]["milliseconds"].toInt();
                int minutes = resultJSONDocument.object().value("result")["totaltime"]["minutes"].toInt();
                int seconds = resultJSONDocument.object().value("result")["totaltime"]["seconds"].toInt();
                int totalmilliseconds = (hours * 3600000) + (minutes * 60000) + (seconds * 1000) + milliseconds;
                entity->updateAttrByIndex(MediaPlayerDef::MEDIADURATION, totalmilliseconds / 1000);
            }
            if (resultJSONDocument.object().value("result").toObject().contains("time")) {
                int hours = resultJSONDocument.object().value("result")["time"]["hours"].toInt();
                int milliseconds = resultJSONDocument.object().value("result")["time"]["milliseconds"].toInt();
                int minutes = resultJSONDocument.object().value("result")["time"]["minutes"].toInt();
                int seconds = resultJSONDocument.object().value("result")["time"]["seconds"].toInt();
                int totalmilliseconds = (hours * 3600000) + (minutes * 60000) + (seconds * 1000) + milliseconds;
                entity->updateAttrByIndex(MediaPlayerDef::MEDIAPROGRESS, totalmilliseconds / 1000);
                m_progressBarPosition = totalmilliseconds / 1000;
            }
            if (resultJSONDocument.object().value("result").toObject().contains("speed")) {
                if (resultJSONDocument.object().value("result")["speed"].toInt() > 0) {
                    m_progressBarTimer->stop();
                    m_progressBarTimer->start();
                    entity->updateAttrByIndex(MediaPlayerDef::STATE, MediaPlayerDef::PLAYING);
                } else {
                    clearMediaPlayerEntity();
                }
            }
            /*QString jsonstring =
                "{\"jsonrpc\": \"2.0\", \"method\": \"Player.GetActivePlayers\", \"id\":\"Player.GetActivePlayers\"}";
            postRequest(jsonstring);*/
            m_firstrun = false;
        } else {
            m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetActivePlayers;
            // m_flag = false;
        }
    }
}
void Kodi::getCurrentPlayer() {
    QString jsonstring =
        "{\"jsonrpc\": \"2.0\", \"method\": \"Player.GetActivePlayers\", \"id\":\"Player.GetActivePlayers\"}";
    postRequest(jsonstring);
    /*QObject* contextgetCurrentPlayer = new QObject(context_kodi);
    QString  method = "Player.GetActivePlayers";
    QString  thumbnail = "";

    QObject::connect(
        context_kodi, &Kodi::requestReadygetCurrentPlayer, contextgetCurrentPlayer,
        [=](const QJsonDocument& resultJSONDocument) {
            EntityInterface* entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));
            if (resultJSONDocument.object().value("id") == "Player.GetActivePlayers") {
                if (entity) {
                    QString strJson(resultJSONDocument.toJson(QJsonDocument::Compact));
                    // qCDebug(m_logCategory) << strJson;

                    if (resultJSONDocument.object().contains("result")) {
                        if (resultJSONDocument.object()["result"].toArray().count() != 0) {
                            m_currentkodiplayerid =
                                resultJSONDocument.object()["result"].toArray()[0].toObject()["playerid"].toInt();
                            if (m_currentkodiplayerid > 0) {
                                m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetItem;
                            }
                            m_currentkodiplayertype =
                                resultJSONDocument.object()["result"].toArray()[0].toObject()["type"].toString();
                            // m_flag = false;
                        } else {
                            // m_flag = false;
                        }
                    } else {
                        // m_flag = false;
                    }
                }
            } else if (resultJSONDocument.object().value("id") == "Player.GetItem") {
                if (resultJSONDocument.object().contains("result")) {
                    if (resultJSONDocument.object().value("result")["item"].toObject().contains("type")) {
                        /*if (m_KodiCurrentPlayerTitle ==
                            resultJSONDocument.object().value("result")["item"]["title"].toString()) {
                            m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetActivePlayers;
                            // m_flag = false;
                        } else*/
    /*                  if (resultJSONDocument.object().value("result")["item"]["type"].toString() == "channel") {
                          QString fanart = resultJSONDocument.object().value("result")["item"]["type"].toString();
                          QString id = resultJSONDocument.object().value("result")["item"]["id"].toString();
                          QString label = resultJSONDocument.object().value("result")["item"]["label"].toString();
                          QString title = resultJSONDocument.object().value("result")["item"]["title"].toString();

                          QString type = resultJSONDocument.object().value("result")["item"]["type"].toString();
                          // m_currentKodiMediaType = type;
                          entity->updateAttrByIndex(MediaPlayerDef::MEDIATYPE, type);
                          // get the track title
                          entity->updateAttrByIndex(MediaPlayerDef::MEDIATITLE, title);
                          // get the artist
                          entity->updateAttrByIndex(MediaPlayerDef::MEDIAARTIST, label);
                          // entity->updateAttrByIndex(MediaPlayerDef::STATE, MediaPlayerDef::PLAYING);
                          if (!resultJSONDocument.object()
                                   .value("result")["item"]["thumbnail"]
                                   .toString()
                                   .isEmpty()) {
                              m_KodiCurrentPlayerThumbnail =
                                  resultJSONDocument.object().value("result")["item"]["thumbnail"].toString();
                              m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::PrepareDownload;
                              // m_flag = false;
                          } else {
                              m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetProperties;
                              // m_flag = false;
                          }
                          // // m_flag = false;
                      } else {
                          // m_flag = false;
                          m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetActivePlayers;
                      }
                  } else {
                      // m_flag = false;
                      m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetActivePlayers;
                  }
              } else {
                  // m_flag = false;
                  m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetActivePlayers;
              }
          } else if (resultJSONDocument.object().value("id") == "Files.PrepareDownload") {
              m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetProperties;
              if (resultJSONDocument.object().contains("result")) {
                  if (resultJSONDocument.object().value("result")["protocol"].toString() == "http" &&
                      resultJSONDocument.object().value("result")["mode"].toString() == "redirect") {
                      entity->updateAttrByIndex(
                          MediaPlayerDef::MEDIAIMAGE,
                          QString("%1://%2:%3/%4")
                              .arg(m_kodiJSONRPCUrl.scheme(), m_kodiJSONRPCUrl.host())
                              .arg(m_kodiJSONRPCUrl.port())
                              .arg(resultJSONDocument.object().value("result")["details"]["path"].toString()));
                      m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetProperties;
                      // m_flag = false;
                  } else {
                      // m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetProperties;
                      // // m_flag = false;
                  }
              } else {
                  // m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetProperties;
                  // // m_flag = false;
              }
          } else if (resultJSONDocument.object().value("id") == "Player.GetProperties") {
              if (resultJSONDocument.object().contains("result")) {
                  m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetActivePlayers;

                  if (resultJSONDocument.object().value("result").toObject().contains("totaltime")) {
                      int hours = resultJSONDocument.object().value("result")["totaltime"]["hours"].toInt();
                      int milliseconds =
                          resultJSONDocument.object().value("result")["totaltime"]["milliseconds"].toInt();
                      int minutes = resultJSONDocument.object().value("result")["totaltime"]["minutes"].toInt();
                      int seconds = resultJSONDocument.object().value("result")["totaltime"]["seconds"].toInt();
                      int totalmilliseconds = (hours * 3600000) + (minutes * 60000) + (seconds * 1000) + milliseconds;
                      entity->updateAttrByIndex(MediaPlayerDef::MEDIADURATION, totalmilliseconds / 1000);
                  }
                  if (resultJSONDocument.object().value("result").toObject().contains("time")) {
                      int hours = resultJSONDocument.object().value("result")["time"]["hours"].toInt();
                      int milliseconds = resultJSONDocument.object().value("result")["time"]["milliseconds"].toInt();
                      int minutes = resultJSONDocument.object().value("result")["time"]["minutes"].toInt();
                      int seconds = resultJSONDocument.object().value("result")["time"]["seconds"].toInt();
                      int totalmilliseconds = (hours * 3600000) + (minutes * 60000) + (seconds * 1000) + milliseconds;
                      entity->updateAttrByIndex(MediaPlayerDef::MEDIAPROGRESS, totalmilliseconds / 1000);
                      m_progressBarPosition = totalmilliseconds / 1000;
                  }
                  if (resultJSONDocument.object().value("result").toObject().contains("speed")) {
                      if (resultJSONDocument.object().value("result")["speed"].toInt() > 0) {
                          m_progressBarTimer->stop();
                          m_progressBarTimer->start();
                          entity->updateAttrByIndex(MediaPlayerDef::STATE, MediaPlayerDef::PLAYING);
                      } else {
                          clearMediaPlayerEntity();
                      }
                  }
                  // m_flag = false;
              } else {
                  m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetActivePlayers;
                  // m_flag = false;
              }
          } else {
          }
          contextgetCurrentPlayer->deleteLater();
      });
  QString jsonstring;
  if (// m_flagKodiOnline && !m_flag) {
      qCDebug(m_logCategory) << "kodi nr leer" << m_flag;
      if ((m_KodiGetCurrentPlayerState == KodiGetCurrentPlayerState::GetActivePlayers ||
           m_KodiGetCurrentPlayerState == KodiGetCurrentPlayerState::Stopped)) {  // && !// m_flag) {
          // postRequest(method, m_globalKodiRequestID);
          jsonstring =
              "{\"jsonrpc\": \"2.0\", \"method\": \"Player.GetActivePlayers\", \"id\":\"Player.GetActivePlayers\"}";
          postRequest(jsonstring);
          // m_flag = true;
          m_flag = true;
      } else if (m_KodiGetCurrentPlayerState == KodiGetCurrentPlayerState::GetItem) {  // && !// m_flag) {
          if (m_currentkodiplayertype == "video") {
              jsonstring =
                  "{\"jsonrpc\": \"2.0\", \"method\": \"Player.GetItem\",\"params\":"
                  "{ \"properties\": [\"title\", \"album\", \"artist\", \"season\", \"episode\","
                  " \"duration\", \"showtitle\", \"tvshowid\", \"thumbnail\", \"file\", \"fanart\","
                  " \"streamdetails\"], \"playerid\": "
                  "" +
                  QString::number(m_currentkodiplayerid) + " }, \"id\": \"Player.GetItem\"}";
              postRequest(jsonstring);
              m_flag = true;
          } else if (m_currentkodiplayertype == "audio") {
              jsonstring =
                  "{\"jsonrpc\": \"2.0\", \"method\": \"Player.GetItem\",\"params\":"
                  "{ \"properties\": [\"title\", \"album\", \"artist\", \"season\", "
                  "\"episode\", \"duration\", \"showtitle\", \"tvshowid\", \"thumbnail\", "
                  "\"file\", \"fanart\", \"streamdetails\"], \"playerid\": "
                  "" +
                  QString::number(m_currentkodiplayerid) + " }, \"id\": \"Player.GetItem\"}";
              postRequest(jsonstring);
              m_flag = true;
          }
      } else if (m_KodiGetCurrentPlayerState == KodiGetCurrentPlayerState::GetProperties) {  // && !// m_flag) {
          jsonstring =
              "{\"jsonrpc\": \"2.0\", \"method\": \"Player.GetProperties\", "
              "\"params\": { \"playerid\":" +
              QString::number(m_currentkodiplayerid) +
              ", \"properties\": "
              "[\"totaltime\", \"time\", \"speed\"] }, "
              "\"id\": \"Player.GetProperties\"}";
          postRequest(jsonstring);
          qCDebug(m_logCategory) << "kodi nr 2";
          m_flag = true;
      } else if (m_KodiGetCurrentPlayerState == KodiGetCurrentPlayerState::PrepareDownload) {  //&& !// m_flag) {
          jsonstring =
              "{\"jsonrpc\": \"2.0\", \"method\": \"Files.PrepareDownload\", "
              "\"params\": { \"path\": \"" +
              m_KodiCurrentPlayerThumbnail + "\" }, \"id\": \"Files.PrepareDownload\"}";
          postRequest(jsonstring);
          qCDebug(m_logCategory) << "kodi nr 1";
          m_flag = true;
      } else {
          // m_flag = false;
      }
  } else {
      qCDebug(m_logCategory) << "kodi nr leer";
  }*/
}
void Kodi::tvheadendGetRequest(const QString& path, const QList<QPair<QString, QString> >& queryItems) {
    // create new networkacces manager and request

    QNetworkRequest request;

    // connect to finish signal
    // QObject::connect(manager, &QNetworkAccessManager::finished, context_getRequestwitchAuthentication,
    //               [=](QNetworkReply* reply) {
    // set the URL
    QUrl url(m_tvheadendJSONUrl);
    url.setPath(path);

    if (!queryItems.isEmpty()) {
        QUrlQuery urlQuery;
        urlQuery.setQueryItems(queryItems);
        url.setQuery(urlQuery);
    }

    request.setUrl(url);
    QString u = request.url().toString();
    // send the get request
    m_tvreply = networkManagerTvHeadend->get(request);
    QObject::connect(m_tvreply, &QNetworkReply::finished, context_kodi, [=]() {
        QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
        // QObject::connect(manager, &QNetworkAccessManager::finished, contextpostt, [=](QNetworkReply* reply) {
        QJsonDocument doc;
        if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 0) {
            if (reply->error()) {
                QString errorString = reply->errorString();
                qCWarning(m_logCategory) << errorString;
            }

            QString       answer = reply->readAll();
            QJsonDocument doc;
            if (answer != "") {
                // convert to json
                QJsonParseError parseerror;
                doc = QJsonDocument::fromJson(answer.toUtf8(), &parseerror);

                if (parseerror.error != QJsonParseError::NoError) {
                    qCWarning(m_logCategory) << "JSON error : " << parseerror.errorString();
                    return;
                }
                QVariant z = doc.object().value("entries").toArray();
                if (doc.object().value("name") == "Tvheadend") {
                    emit requestReadyTvheadendConnectionCheck(doc);
                } else if (doc.object().contains("entries") && !doc.object().contains("totalCount")) {
                    emit requestReadygetKodiChannelNumberToTVHeadendUUIDMapping(doc);
                    emit requestReadygetKodiChannelNumberToRadioHeadendUUIDMapping(doc);
                } else if (doc.object().contains("entries") && doc.object().contains("totalCount")) {
                    emit requestReadygetTVEPGfromTVHeadend(doc);
                }
            } else {
                if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 0) {
                    emit requestReadyTvheadendConnectionCheck(doc);
                }
            }
        } else {
            if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 0) {
                emit requestReadyTvheadendConnectionCheck(doc);
            }
        }
        /*reply->deleteLater();
        context_getRequestwitchAuthentication->deleteLater();*/
        // QObject::disconnect(manager, &QNetworkAccessManager::finished,
        // context_getRequestwitchAuthentication, 0);
        // manager->deleteLater();
    });
}

void Kodi::sendCommand(const QString& type, const QString& entityId, int command, const QVariant& param) {
    if (!(type == "media_player" && entityId == m_entityId)) {
        return;
    }

    qCDebug(m_logCategory) << "Keypressed" << command;
    // qCDebug(m_logCategory) << "Key next" << entity->getCommandIndex()
    QObject* contextsendCommand = new QObject(context_kodi);
    //
    if (command == MediaPlayerDef::C_PLAY) {
    } else if (command == MediaPlayerDef::C_PLAY_ITEM) {
        if (param.toMap().value("type") == "tvchannellist" || param.toMap().value("type") == "tvchannel") {
            QObject::connect(context_kodi, &Kodi::requestReadyCommandPlay, contextsendCommand,
                             [=](const QJsonDocument& resultJSONDocument) {
                                 if (resultJSONDocument.object().contains("result")) {
                                     if (resultJSONDocument.object().value("result") == "OK") {
                                         EntityInterface* entity =
                                             static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));
                                         entity->updateAttrByIndex(MediaPlayerDef::STATE,
                                                                   MediaPlayerDef::States::PLAYING);
                                         getCurrentPlayer();
                                     }
                                 }
                                 contextsendCommand->deleteLater();
                                 // QObject::disconnect(this, &Kodi::requestReadyCommandPlay,
                                 // contextsendCommand, 0);
                             });
            QString jsonstring =
                "{\"jsonrpc\": \"2.0\", \"method\": \"Player.Open\",\"params\":"
                " {\"item\":{\"channelid\": " +
                param.toMap().value("id").toString() + "}}, \"id\": \"sendCommandPlay\"}";
            // qCDebug(m_logCategory).noquote() << jsonstring;
            postRequest(jsonstring);
        }
    } else if (command == MediaPlayerDef::C_UP) {
        QObject::connect(context_kodi, &Kodi::requestReadyCommandUp, contextsendCommand,
                         [=](const QJsonDocument& resultJSONDocument) {
                             if (resultJSONDocument.object().contains("result")) {
                                 if (resultJSONDocument.object().value("result") == "OK") {
                                 } else {
                                 }
                             }
                             contextsendCommand->deleteLater();
                         });
        QString jsonstring =
            "{\"jsonrpc\": \"2.0\", \"method\":"
            " \"Input.Up\",\"params\": "
            "{ }, \"id\":\"sendCommandUp\"}";
        postRequest(jsonstring);
        // qCDebug(m_logCategory).noquote() << jsonstring;
        // postRequest("sendCommandPlay", jsonstring);
    } else if (command == MediaPlayerDef::C_MUTE) {
        QObject::connect(context_kodi, &Kodi::requestReadyCommandMute, contextsendCommand,
                         [=](const QJsonDocument& resultJSONDocument) {
                             if (resultJSONDocument.object().contains("result")) {
                                 if (resultJSONDocument.object().value("result") == "OK") {
                                 } else {
                                 }
                             }
                             contextsendCommand->deleteLater();
                         });
        QString jsonstring =
            "{\"jsonrpc\": \"2.0\", \"method\":"
            " \"Application.SetMute\",\"params\": "
            "{ \"mute\": \"toggle\"}, \"id\":\"sendCommandMute\"}";
        postRequest(jsonstring);
    } else if (command == MediaPlayerDef::C_OK) {
        QObject::connect(context_kodi, &Kodi::requestReadyCommandOk, contextsendCommand,
                         [=](const QJsonDocument& resultJSONDocument) {
                             if (resultJSONDocument.object().contains("result")) {
                                 if (resultJSONDocument.object().value("result") == "OK") {
                                 } else {
                                 }
                             }
                             contextsendCommand->deleteLater();
                         });
        QString jsonstring =
            "{\"jsonrpc\": \"2.0\", \"method\":"
            " \"Input.Select\",\"params\": "
            "{ }, \"id\":\"sendCommandOk\"}";
        postRequest(jsonstring);
    } else if (command == MediaPlayerDef::C_DOWN) {
        QObject::connect(context_kodi, &Kodi::requestReadyCommandDown, contextsendCommand,
                         [=](const QJsonDocument& resultJSONDocument) {
                             if (resultJSONDocument.object().contains("result")) {
                                 if (resultJSONDocument.object().value("result") == "OK") {
                                 } else {
                                 }
                             }
                             contextsendCommand->deleteLater();
                         });
        QString jsonstring =
            "{\"jsonrpc\": \"2.0\", \"method\":"
            " \"Input.Down\",\"params\": "
            "{ }, \"id\":\"sendCommandDown\"}";
        postRequest(jsonstring);
        // qCDebug(m_logCategory).noquote() << jsonstring;
        // postRequest("sendCommandPlay", jsonstring);
    } else if (command == MediaPlayerDef::C_RIGHT) {
        QObject::connect(context_kodi, &Kodi::requestReadyCommandRight, contextsendCommand,
                         [=](const QJsonDocument& resultJSONDocument) {
                             if (resultJSONDocument.object().contains("result")) {
                                 if (resultJSONDocument.object().value("result") == "OK") {
                                 } else {
                                 }
                             }
                             contextsendCommand->deleteLater();
                         });
        QString jsonstring =
            "{\"jsonrpc\": \"2.0\", \"method\":"
            " \"Input.Right\",\"params\": "
            "{ }, \"id\":\"sendCommandRight\"}";
        postRequest(jsonstring);
        // qCDebug(m_logCategory).noquote() << jsonstring;
        // postRequest("sendCommandPlay", jsonstring);
    } else if (command == MediaPlayerDef::C_LEFT) {
        QObject::connect(context_kodi, &Kodi::requestReadyCommandLeft, contextsendCommand,
                         [=](const QJsonDocument& resultJSONDocument) {
                             if (resultJSONDocument.object().contains("result")) {
                                 if (resultJSONDocument.object().value("result") == "OK") {
                                 } else {
                                 }
                             }
                             contextsendCommand->deleteLater();
                         });
        QString jsonstring =
            "{\"jsonrpc\": \"2.0\", \"method\":"
            " \"Input.Left\",\"params\": "
            "{ }, \"id\":\"sendCommandLeft\"}";
        postRequest(jsonstring);
    } else if (command == 35) {
        QObject::connect(context_kodi, &Kodi::requestReadyCommandBack, contextsendCommand,
                         [=](const QJsonDocument& resultJSONDocument) {
                             if (resultJSONDocument.object().contains("result")) {
                                 if (resultJSONDocument.object().value("result") == "OK") {
                                 } else {
                                 }
                             }
                             contextsendCommand->deleteLater();
                         });
        QString jsonstring =
            "{\"jsonrpc\": \"2.0\", \"method\":"
            " \"Input.Back\",\"params\": "
            "{ }, \"id\":\"sendCommandBack\"}";
        postRequest(jsonstring);
    } else if (command == MediaPlayerDef::C_MENU) {
        QObject::connect(context_kodi, &Kodi::requestReadyCommandMenu, contextsendCommand,
                         [=](const QJsonDocument& resultJSONDocument) {
                             if (resultJSONDocument.object().contains("result")) {
                                 if (resultJSONDocument.object().value("result") == "OK") {
                                 } else {
                                 }
                             }
                             contextsendCommand->deleteLater();
                         });
        QString jsonstring =
            "{\"jsonrpc\": \"2.0\", \"method\":"
            " \"Input.ContextMenu\",\"params\": "
            "{ }, \"id\":\"sendCommandMenu\"}";
        postRequest(jsonstring);
    } else if (command == MediaPlayerDef::C_CHANNEL_UP) {
        EntityInterface* entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));

        MediaPlayerInterface* me = static_cast<MediaPlayerInterface*>(entity->getSpecificInterface());

        if (me->mediaType() == "channel") {
            QObject::connect(context_kodi, &Kodi::requestReadyCommandChannelUp, contextsendCommand,
                             [=](const QJsonDocument& resultJSONDocument) {
                                 if (resultJSONDocument.object().contains("result")) {
                                     if (resultJSONDocument.object().value("result") == "OK") {
                                         m_progressBarTimer->stop();
                                         getCurrentPlayer();
                                     }
                                 }
                                 contextsendCommand->deleteLater();
                                 // QObject::disconnect(this, &Kodi::requestReadyCommandNext,
                                 // contextsendCommand, 0);
                             });
            QString jsonstring =
                "{\"jsonrpc\": \"2.0\", \"method\":"
                " \"Input.ExecuteAction\",\"params\": "
                "{ \"action\": \"channelup\" }, \"id\":\"sendCommandChannelUp\"}";
            postRequest(jsonstring);
        }
    } else if (command == MediaPlayerDef::C_CHANNEL_DOWN) {
        EntityInterface* entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));

        MediaPlayerInterface* me = static_cast<MediaPlayerInterface*>(entity->getSpecificInterface());

        if (me->mediaType() == "channel") {
            QObject::connect(context_kodi, &Kodi::requestReadyCommandChannelDown, contextsendCommand,
                             [=](const QJsonDocument& resultJSONDocument) {
                                 if (resultJSONDocument.object().contains("result")) {
                                     if (resultJSONDocument.object().value("result") == "OK") {
                                         m_progressBarTimer->stop();
                                         getCurrentPlayer();
                                     }
                                 }
                                 contextsendCommand->deleteLater();
                                 // QObject::disconnect(this,
                                 // &Kodi::requestReadyCommandPrevious, contextsendCommand,
                                 // 0);
                             });
            QString jsonstring =
                "{\"jsonrpc\": \"2.0\","
                " \"method\": \"Input.ExecuteAction\","
                "\"params\": { \"action\": \"channeldown\" }, "
                "\"id\": \"sendCommandChannelDown\"}";
            postRequest(jsonstring);
        }
    } else if (command == MediaPlayerDef::C_QUEUE) {
    } else if (command == MediaPlayerDef::C_STOP) {
        QObject::connect(context_kodi, &Kodi::requestReadyCommandStop, contextsendCommand,
                         [=](const QJsonDocument& resultJSONDocument) {
                             if (resultJSONDocument.object().contains("result")) {
                                 if (resultJSONDocument.object().value("result") == "OK") {
                                     m_progressBarTimer->stop();
                                     m_currentkodiplayertype = "unknown";
                                     m_currentkodiplayerid = -1;
                                     m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::Stopped;
                                     // m_flag = false;
                                     clearMediaPlayerEntity();
                                 }
                             }
                             contextsendCommand->deleteLater();
                             // QObject::disconnect(this, &Kodi::requestReadyCommandPause, contextsendCommand, 0);
                         });
        QString jsonstring =
            "{\"jsonrpc\": \"2.0\", \"method\": \"Player.Stop\","
            " \"params\": { \"playerid\": " +
            QString::number(m_currentkodiplayerid) + " },\"id\": \"sendCommandStop\"}";
        postRequest(jsonstring);
    } else if (command == MediaPlayerDef::C_PAUSE) {
        QObject::connect(context_kodi, &Kodi::requestReadyCommandPause, contextsendCommand,
                         [=](const QJsonDocument& resultJSONDocument) {
                             if (resultJSONDocument.object().contains("result")) {
                                 if (resultJSONDocument.object().value("result") == "OK") {
                                     qCDebug(m_logCategory) << "Pause";
                                 }
                             }
                             contextsendCommand->deleteLater();
                             // QObject::disconnect(this, &Kodi::requestReadyCommandPause, contextsendCommand, 0);
                         });
        QString jsonstring =
            "{\"jsonrpc\": \"2.0\", \"method\": \"Player.PlayPause\","
            " \"params\": { \"playerid\": " +
            QString::number(m_currentkodiplayerid) + " },\"id\": \"sendCommandPause\"}";
        postRequest(jsonstring);
    } else if (command == MediaPlayerDef::C_NEXT) {
        EntityInterface* entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));

        MediaPlayerInterface* me = static_cast<MediaPlayerInterface*>(entity->getSpecificInterface());

        if (me->mediaType() == "channel") {
            QObject::connect(context_kodi, &Kodi::requestReadyCommandNext, contextsendCommand,
                             [=](const QJsonDocument& resultJSONDocument) {
                                 if (resultJSONDocument.object().contains("result")) {
                                     if (resultJSONDocument.object().value("result") == "OK") {
                                         m_progressBarTimer->stop();
                                         getCurrentPlayer();
                                     }
                                 }
                                 contextsendCommand->deleteLater();
                                 // QObject::disconnect(this, &Kodi::requestReadyCommandNext,
                                 // contextsendCommand, 0);
                             });
            QString jsonstring =
                "{\"jsonrpc\": \"2.0\", \"method\":"
                " \"Input.ExecuteAction\",\"params\": "
                "{ \"action\": \"channelup\" }, \"id\":\"sendCommandNext\"}";
            postRequest(jsonstring);
        }
    } else if (command == MediaPlayerDef::C_PREVIOUS) {
        EntityInterface* entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));

        MediaPlayerInterface* me = static_cast<MediaPlayerInterface*>(entity->getSpecificInterface());

        if (me->mediaType() == "channel") {
            QObject::connect(context_kodi, &Kodi::requestReadyCommandPrevious, contextsendCommand,
                             [=](const QJsonDocument& resultJSONDocument) {
                                 if (resultJSONDocument.object().contains("result")) {
                                     if (resultJSONDocument.object().contains("result")) {
                                         m_progressBarTimer->stop();
                                         getCurrentPlayer();
                                     }
                                     KodiApplicationProperties();
                                 }
                                 contextsendCommand->deleteLater();
                                 // QObject::disconnect(this,
                                 // &Kodi::requestReadyCommandPrevious, contextsendCommand,
                                 // 0);
                             });
            QString jsonstring =
                "{\"jsonrpc\": \"2.0\","
                " \"method\": \"Input.ExecuteAction\","
                "\"params\": { \"action\": \"channeldown\" }, "
                "\"id\": \"sendCommandPrevious\"}";
            postRequest(jsonstring);
        }
    } else if (command == MediaPlayerDef::C_VOLUME_SET) {
        /*qCDebug(m_logCategory)
            << "Volume"
            << param.toString();  // putRequest("/v1/me/player/volume" {{ "volume_percent", param.toString() }} "");*/
        QObject::connect(context_kodi, &Kodi::requestReadyCommandVolume, contextsendCommand,
                         [=](const QJsonDocument& resultJSONDocument) {
                             if (resultJSONDocument.object().contains("result")) {
                                 EntityInterface* entity =
                                     static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));
                                 entity->updateAttrByIndex(MediaPlayerDef::VOLUME,
                                                           resultJSONDocument.object().value("result").toInt());
                             }
                             contextsendCommand->deleteLater();
                         });
        QString jsonstring =
            "{\"jsonrpc\": \"2.0\", \"method\":"
            " \"Application.SetVolume\",\"params\": "
            "{\"volume\": " +
            param.toString() + " }, \"id\":\"sendCommandVolume\"}";
        postRequest(jsonstring);
        // {"jsonrpc":"2.0","method":"Application.SetVolume","id":1,"params":{"volume":64}}
    } else if (command == MediaPlayerDef::C_SEARCH) {
        // search(param.toString());
    } else if (command == MediaPlayerDef::C_GETMEDIAPLAYEREPGVIEW) {
        if (param == "all") {
            //  getCompleteTVChannelList();
            showepg();
        } else {
            showepg(param.toInt());
        }
    } else if (command == MediaPlayerDef::C_GETALBUM) {
        // getAlbum(param.toString());
    } else if (command == MediaPlayerDef::C_GETTVCHANNELLIST) {
        qCDebug(m_logCategory) << "debug:" << param;
        if (param == "All" || param == "Radio" || param == "TV") {
            getCompleteTVChannelList(param.toString());
        } else {
            getCompleteTVChannelList(param.toString());
        }
    }
    /*else if (command == MediaPlayerDef::C_GETPLAYLIST) {
        if (param == "user") {
            // getCompleteTVChannelList();
        } else {
            // getSingleTVChannelList(param.toString());
        }
    }*/
}

void Kodi::postRequest(const QString& param) {
    // create new networkacces manager and request
    // QNetworkAccessManager* manager = new QNetworkAccessManager(this);
    QNetworkRequest request(m_kodiJSONRPCUrl);
    // QObject*               contextpostt = new QObject(this);
    // const QString          u = callfunction;

    // set headers
    QByteArray paramutf8 = param.toUtf8();

    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    // send the post request
    // qCDebug(m_logCategory) << "POST:" << request.url() << paramutf8;

    m_kodireply = networkManagerKodi->post(request, paramutf8);
    // qCDebug(m_logCategory) << param;
    // connect to finish signal
    QObject::connect(m_kodireply,
                     static_cast<void (QNetworkReply::*)(QNetworkReply::NetworkError)>(&QNetworkReply::error),
                     context_kodi, [=]() {
                         qCDebug(m_logCategory) << "1";
                         QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
                         // qCDebug(m_logCategory) << reply->error();
                     });
    QObject::connect(m_kodireply, &QNetworkReply::finished, context_kodi, [=]() {
        QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
        // QObject::connect(manager, &QNetworkAccessManager::finished, contextpostt, [=](QNetworkReply* reply) {
        QJsonDocument doc;
        // qCDebug(m_logCategory) << reply->error();
        if (reply->error() == QNetworkReply::OperationCanceledError) {
            qCDebug(m_logCategory) << "2";
        }
        if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200) {
            if (reply->error()) {
                QString errorString = reply->errorString();
                qCWarning(m_logCategory) << errorString;
            }
            QString answer = reply->readAll();
            // qCDebug(m_logCategory).noquote() << "RECEIVED:" << answer;
            // QVariantMap map;

            if (answer != "") {
                // convert to json
                QJsonParseError parseerror;
                doc = QJsonDocument::fromJson(answer.toUtf8(), &parseerror);
                QString strJson(doc.toJson(QJsonDocument::Compact));
                // qCDebug(m_logCategory) << strJson;
                if (parseerror.error != QJsonParseError::NoError) {
                    qCWarning(m_logCategory) << "JSON error : " << parseerror.errorString();
                    return;
                }
                if (doc.object().value("id") == 2) {
                    QString h = "gg";
                }
                if (doc.object().value("id").toString() == "getKodiAvailableTVChannelList") {
                    emit requestReadygetKodiAvailableTVChannelList(doc);
                } else if (doc.object().value("id").toString() == "getKodiAvailableRadioChannelList") {
                    emit requestReadygetKodiAvailableRadioChannelList(doc);
                } else if (doc.object().value("id").toString() == "getSingleTVChannelList") {
                    emit requestReadygetSingleTVChannelList(doc);
                } else if (doc.object().value("id").toString() == "getCompleteTVChannelList") {
                    emit requestReadygetCompleteTVChannelList(doc);
                } else if (doc.object().value("id").toString() == "getCompleteRadioChannelList") {
                    emit requestReadygetCompleteRadioChannelList(doc);
                } else if (doc.object().value("id").toString() == "epg") {
                    emit requestReadygetEPG(doc);
                } else if (doc.object().value("id").toString() == "sendCommandPlay") {
                    emit requestReadyCommandPlay(doc);
                } else if (doc.object().value("id").toString() == "sendCommandPause") {
                    emit requestReadyCommandPause(doc);
                } else if (doc.object().value("id").toString() == "sendCommandStop") {
                    emit requestReadyCommandStop(doc);
                } else if (doc.object().value("id").toString() == "sendCommandNext") {
                    emit requestReadyCommandNext(doc);
                } else if (doc.object().value("id").toString() == "sendCommandPrevious") {
                    emit requestReadyCommandPrevious(doc);
                } else if (doc.object().value("id").toString() == "sendCommandUp") {
                    emit requestReadyCommandUp(doc);
                } else if (doc.object().value("id").toString() == "sendCommandDown") {
                    emit requestReadyCommandDown(doc);
                } else if (doc.object().value("id").toString() == "sendCommandLeft") {
                    emit requestReadyCommandLeft(doc);
                } else if (doc.object().value("id").toString() == "sendCommandRight") {
                    emit requestReadyCommandRight(doc);
                } else if (doc.object().value("id").toString() == "sendCommandOk") {
                    emit requestReadyCommandOk(doc);
                } else if (doc.object().value("id").toString() == "sendCommandMenu") {
                    emit requestReadyCommandMenu(doc);
                } else if (doc.object().value("id").toString() == "sendCommandBack") {
                    emit requestReadyCommandBack(doc);
                } else if (doc.object().value("id").toString() == "sendCommandChannelUp") {
                    emit requestReadyCommandChannelUp(doc);
                } else if (doc.object().value("id").toString() == "sendCommandChannelDown") {
                    emit requestReadyCommandChannelDown(doc);
                } else if (doc.object().value("id").toString() == "sendCommandMute") {
                    emit requestReadyCommandMute(doc);
                } else if (doc.object().value("id").toString() == "sendCommandVolume") {
                    emit requestReadyCommandVolume(doc);
                } else if (doc.object().value("id").toString() == "ConnectionCheck") {
                    emit requestReadyKodiConnectionCheck(doc);
                } else if (doc.object().value("id").toString() == "Application.GetProperties") {
                    emit requestReadyKodiApplicationProperties(doc);

                } else if (doc.object().value("id").toString() == "Player.GetActivePlayers" ||
                           doc.object().value("id").toString() == "Player.GetItem" ||
                           doc.object().value("id").toString() == "Player.GetProperties" ||
                           doc.object().value("id").toString() == "Files.PrepareDownload") {
                    emit requestReadygetCurrentPlayer(doc);
                } else {
                    qCWarning(m_logCategory) << "no callback function defined ";
                }
            } else {
                if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 0) {
                    emit requestReadyKodiConnectionCheck(doc);
                }
            }
        } else {
            if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 0) {
                emit requestReadyKodiConnectionCheck(doc);
            }
        }
        // reply->deleteLater();
        // contextpostt->deleteLater();
        // manager->deleteLater();
    });
}

void Kodi::onPollingEPGLoadTimerTimeout() {
    //
    if (m_mapKodiChannelNumberToTVHeadendUUID.count() > 0) {
        int temp_Timestamp = (m_EPGExpirationTimestamp - (QDateTime(QDate::currentDate()).toTime_t()));
        if (temp_Timestamp <= 0) {
            if (m_currentEPGchannelToLoad == m_epgChannelList.count()) {
                m_EPGExpirationTimestamp =
                    QDateTime(QDate::currentDate()).toTime_t() + (m_tvProgrammExpireTimeInHours * 3600);
                m_currentEPGchannelToLoad = 0;
            } else {
                getTVEPGfromTVHeadend(m_epgChannelList[m_currentEPGchannelToLoad]);
                m_currentEPGchannelToLoad++;
            }
        } else {
            // tvheadendGetRequest("/api/serverinfo", {});
        }
        // qCDebug(m_logCategory) << "urls" << m_tvheadendJSONUrl;
    }
}
void Kodi::onPollingTimerTimeout() {
    if (m_flagKodiOnline) {
        // qCDebug(m_logCategory) << "polling";
        getCurrentPlayer();
        if (m_timer == 10) {
            KodiApplicationProperties();
            postRequest(
                "{ \"jsonrpc\": \"2.0\","
                " \"method\": \"JSONRPC.Ping\", \"params\": {  },"
                " \"id\":\"ConnectionCheck\"}");
            m_timer = 0;
        } else {
            m_timer++;
        }
    } else {
    }
}

void Kodi::onProgressBarTimerTimeout() {
    EntityInterface* entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));
    if (entity) {
        m_progressBarPosition++;
        entity->updateAttrByIndex(MediaPlayerDef::MEDIAPROGRESS, m_progressBarPosition);
    }
}

void Kodi::showepg() {
    QObject* contextshowepg = new QObject(context_kodi);
    QObject::connect(
        context_kodi, &Kodi::requestReadygetEPG, contextshowepg, [=](const QJsonDocument& resultJSONDocument) {
            qCDebug(m_logCategory) << "finished request showepg()";
            EntityInterface* entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));
            QDateTime        timestamp;

            QString     channelId = "2";
            QString     label = "";
            QString     thumbnail = "";
            QString     unqueId = "";
            QString     type = "epg";
            QStringList commands = {};
            // 60minuten = 360px; 1min = 6px
            epgitem->reset();
            //epgitem->~BrowseEPGModel();
            //epgitem = new BrowseEPGModel("channelId", 20, 1, 400, 40, "epglist", "#FF0000", "#FFFFFF",
              //                                           "Test", "", "", "", "", "", commands, nullptr);
            QDateTime       current = QDateTime::currentDateTime();
            int             hnull = current.time().hour() - 1;
            int             dnull = current.date().day();
            int             mnull = current.date().month();
            int             ynull = current.date().year();
            qCDebug(m_logCategory) << "1 for start";
            for (int i = hnull; i < (hnull + 80); i++) {
                if (i < 24) {
                    epgitem->addEPGItem(QString::number(i), ((i - hnull) * 360) + 170, 0, 360, 40, "epg", "#FF0000",
                                        "#FFFFFF",
                                        QString::number(i) + " Uhr  " + QString::number(dnull) + "." +
                                            QString::number(mnull) + "." + QString::number(ynull),
                                        "", "", "", "", "", commands);
                } else if (i < 48) {
                    epgitem->addEPGItem(QString::number(i), ((i - hnull) * 360) + 170, 0, 360, 40, "epg", "#FF0000",
                                        "#FFFFFF",
                                        QString::number((i - 24)) + " Uhr  " + QString::number(dnull + 1) + "." +
                                            QString::number(mnull) + "." + QString::number(ynull),
                                        "", "", "", "", "", commands);
                } else if (i < 72) {
                    epgitem->addEPGItem(QString::number(i), ((i - hnull) * 360) + 170, 0, 360, 40, "epg", "#FF0000",
                                        "#FFFFFF",
                                        QString::number((i - 48)) + " Uhr  " + QString::number(dnull + 2) + "." +
                                            QString::number(mnull) + "." + QString::number(ynull),
                                        "", "", "", "", "", commands);
                } else {
                    epgitem->addEPGItem(QString::number(i), ((i - hnull) * 360) + 170, 0, 360, 40, "epg", "#FF0000",
                                        "#FFFFFF",
                                        QString::number((i - 72)) + " Uhr  " + QString::number(dnull + 3) + "." +
                                            QString::number(mnull) + "." + QString::number(ynull),
                                        "", "", "", "", "", commands);
                }
            }
            int     i = 1;
            QString channelname = "a";
            qCDebug(m_logCategory) << "1 for end";
            qCDebug(m_logCategory) << "2 for start";
            for (int const& channel : m_epgChannelList) {
                /*for (int j = 0; m_KodiTVChannelList.count() > j; j++) {
                    if (m_KodiTVChannelList[j].toMap().value("channelnumber").toInt() == channel) {
                        channelname = m_KodiTVChannelList[j].toMap().value("label").toString();
                        break;
                    }
                }*/
                epgitem->addEPGItem(QString::number(i), 0, i, 170, 40, "epg", "#0000FF", "#FFFFFF",
                                    m_KodiTVChannelList[channel - 1].toMap().value("label").toString(), "", "", "", "",
                                    "", commands);
                i++;
            }
            qCDebug(m_logCategory) << "2 for end";
            i = 0;
            qCDebug(m_logCategory) << "3 for start";
            for (auto const& ob : m_currentEPG) {
                QString channelUuid = ob.toMap().value("channelUuid").toString();
                // QString channelUuid = m_currentEPG.value(i).toMap().value("channelUuid").toString();
                if (m_epgChannelList.contains(m_mapTVHeadendUUIDToKodiChannelNumber.value(channelUuid))) {
                    timestamp.setTime_t(m_currentEPG.value(i).toMap().value("start").toInt());
                    int column = m_mapTVHeadendUUIDToKodiChannelNumber.value(channelUuid);
                    if (column != 0) {
                        int h = (timestamp.date().day() - dnull) * 1440 + (timestamp.time().hour() - hnull) * 60 +
                                timestamp.time().minute();
                        if (h > 15000) {
                            //
                        } else {
                            int width =
                                ((ob.toMap().value("stop").toInt() - ob.toMap().value("start").toInt()) / 60) * 6;
                            epgitem->addEPGItem(QString::number(i), (h * 6) + 170, column, width, 40, "epg", "#FFFF00",
                                                "#FFFFFF", ob.toMap().value("title").toString(), "", "", "", "", "",
                                                commands);
                        }
                    }
                }
                i++;
            }
            qCDebug(m_logCategory) << "3 for end";
            MediaPlayerInterface* me = static_cast<MediaPlayerInterface*>(entity->getSpecificInterface());
            me->setBrowseModel(epgitem);
            contextshowepg->deleteLater();
        });
    qCDebug(m_logCategory) << "GET USERS PLAYLIST";

    // m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::NotActive;
    QString jsonstring;
    postRequest(
        "{ \"jsonrpc\": \"2.0\","
        " \"method\": \"JSONRPC.Ping\", \"params\": {  },"
        " \"id\":\"epg\"}");
}

void Kodi::showepg(int channel) {
    QObject* contextshowepg = new QObject(context_kodi);
    QObject::connect(
        context_kodi, &Kodi::requestReadygetEPG, contextshowepg, [=](const QJsonDocument& resultJSONDocument) {
            EntityInterface* entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));
            QDateTime        timestamp;

            QString     channelId = "2";
            QVariantMap channelEpg = m_currentEPG.value(channel).toMap();
            QUrl        imageUrl(m_tvheadendJSONUrl);
            if (!imageUrl.isEmpty()) {
                imageUrl.setPath("/" + channelEpg.value("channelIcon").toString());
            }

            QString     thumbnail = "";
            QString     unqueId = "";
            QString     type = "epg";
            QStringList commands = {};
            // 60minuten = 360px; 1min = 6px

            epgitem->reset();
            epgitem->~BrowseEPGModel();
            epgitem = new BrowseEPGModel(
                QString::number(
                    m_mapTVHeadendUUIDToKodiChannelNumber.value(channelEpg.value("channelUUID").toString())),
                0, 0, 0, 0, "epg", "#FFFF00", "#FFFFFF", channelEpg.value("title").toString(),
                channelEpg.value("subtitle").toString(), channelEpg.value("description").toString(), "starttime",
                "endtime", imageUrl.url(), commands);
            // epgitem->addEPGItem(QString::number(i), (h*6), column, width, 40, "epg", "#FFFF00",
            //     m_currentEPG.value(i).toMap().value("title").toString(), "", "", "", "", "", commands);
            /*QDateTime current = QDateTime::currentDateTime();
            int hnull = current.time().hour();
            for (int i = hnull; i < (hnull+5); i++) {
                if (i < 24){
                    epgitem->addEPGItem(QString::number(i), (i-hnull)*360, 0, 360, 40, "epg", "#FF0000",
            QString::number(i) + " Uhr", "", "", "", "", "", commands);
                }
                else
                {
                    epgitem->addEPGItem(QString::number(i), (i-hnull)*360, 0, 360, 40, "epg", "#FF0000",
            QString::number((i-24)) + " Uhr", "", "", "", "", "", commands);
                }
            }

            for (int i=0; i < m_currentEPG.length(); i++) {
                timestamp.setTime_t(m_currentEPG.value(i).toMap().value("start").toInt());
                int column =
            m_mapTVHeadendUUIDToKodiChannelNumber.value(m_currentEPG.value(i).toMap().value("channelUuid").toString());
                if (column != 0) {
                    int h = (timestamp.time().hour()-hnull)*60 + timestamp.time().minute();
                    int width =
            ((m_currentEPG.value(i).toMap().value("stop").toInt()-m_currentEPG.value(i).toMap().value("start").toInt())/60)*6;
                    epgitem->addEPGItem(QString::number(i), (h*6), column, width, 40, "epg", "#FFFF00",
            m_currentEPG.value(i).toMap().value("title").toString(), "", "", "", "", "", commands);
                }
            }*/
            MediaPlayerInterface* me = static_cast<MediaPlayerInterface*>(entity->getSpecificInterface());
            me->setBrowseModel(epgitem);
            contextshowepg->deleteLater();
        });
    qCDebug(m_logCategory) << "GET USERS PLAYLIST";

    // m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::NotActive;
    QString jsonstring;
    postRequest(
        "{ \"jsonrpc\": \"2.0\","
        " \"method\": \"JSONRPC.Ping\", \"params\": {  },"
        " \"id\":\"epg\"}");
}

QString Kodi::fixUrl(QString url) {
    if (url.contains("127.0.0.1")) {
        url = url.replace("127.0.0.1", m_tvheadendJSONUrl.host());
    }
    if (url.endsWith('/')) {
        url = url.chopped(1);
    }
    return url;
}

bool Kodi::read(QMap<QString, int>* map) {
    QString path = "/opt/yio/userdata/kodi/";
    QString filename = "data1.dat";
    QFile   myFile(path + filename);
    // QMap<int, QString> map;
    QDataStream in(&myFile);
    in.setVersion(QDataStream::Qt_5_8);

    if (!myFile.open(QIODevice::ReadOnly)) {
        qCDebug(m_logCategory) << "Could not read the file:" << filename << "Error string:" << myFile.errorString();
        qCDebug(m_logCategory) << "Read status" << in.status();
        return false;
    }

    in >> *map;
    qCDebug(m_logCategory) << "Read status" << in.status();
    return true;
}

bool Kodi::write(QMap<QString, int> map) {
    QString path = "/opt/yio/userdata/kodi/";
    QString filename = "data1.dat";
    QFile   myFile(filename);
    QDir    dir(path);

    if (!dir.exists()) {
        qCDebug(m_logCategory) << "Creating " << path << "directory";

        if (dir.mkpath(path)) {
            qCDebug(m_logCategory) << path << "successfully created";
        } else {
            qCDebug(m_logCategory) << "error during creation of " << path;
            return false;
        }
    } else {
        qCDebug(m_logCategory) << path << " already exists";
    }

    if (!myFile.open(QIODevice::WriteOnly)) {
        qCDebug(m_logCategory) << "Could not write to file:" << filename << "Error string:" << myFile.errorString();
        return false;
    }
    QDataStream out(&myFile);
    out.setVersion(QDataStream::Qt_5_8);
    out << map;
    qCDebug(m_logCategory) << "Write status" << out.status();
    return true;
}

bool Kodi::read(QMap<int, QString>* map) {
    QString path = "/opt/yio/userdata/kodi/";
    QString filename = "data.dat";
    QFile   myFile(path + filename);

    QDataStream in(&myFile);
    in.setVersion(QDataStream::Qt_5_8);

    if (!myFile.open(QIODevice::ReadOnly)) {
        qCDebug(m_logCategory) << "Could not read the file:" << filename << "Error string:" << myFile.errorString();
        qCDebug(m_logCategory) << "Read status" << in.status();
        return false;
    }

    in >> *map;
    qCDebug(m_logCategory) << "Read status" << in.status();
    return true;
}

bool Kodi::write(QMap<int, QString> map) {
    QString path = "/opt/yio/userdata/kodi/";
    QString filename = "data.dat";
    QFile   myFile(filename);
    QDir    dir(path);

    if (!dir.exists()) {
        qCDebug(m_logCategory) << "Creating " << path << "directory";

        if (dir.mkpath(path)) {
            qCDebug(m_logCategory) << path << "successfully created";
        } else {
            qCDebug(m_logCategory) << "error during creation of " << path;
            return false;
        }
    } else {
        qCDebug(m_logCategory) << path << " already exists";
    }

    if (!myFile.open(QIODevice::WriteOnly)) {
        qCDebug(m_logCategory) << "Could not write to file:" << filename << "Error string:" << myFile.errorString();
        return false;
    }

    QDataStream out(&myFile);
    out.setVersion(QDataStream::Qt_5_8);
    out << map;
    qCDebug(m_logCategory) << "Write status" << out.status();
    return true;
}

void Kodi::kodiconnectioncheck(const QJsonDocument& resultJSONDocument) {
    if (resultJSONDocument.object().contains("result")) {
        if (resultJSONDocument.object().value("result") == "pong") {
            if (!m_flagKodiOnline) {
                m_flagKodiOnline = true;
                m_pollingTimer->setInterval(5000);
                QObject::connect(m_pollingTimer, &QTimer::timeout, context_kodi, &Kodi::onPollingTimerTimeout);

                m_progressBarTimer->setInterval(1000);
                QObject::connect(m_progressBarTimer, &QTimer::timeout, context_kodi, &Kodi::onProgressBarTimerTimeout);
                m_tcpSocketKodiEventServer = new QTcpSocket(context_kodi);
                m_tcpSocketKodiEventServer->connectToHost(m_kodiEventServerUrl.host(), m_kodiEventServerUrl.port());
                if (m_tcpSocketKodiEventServer->waitForConnected()) {
                    QObject::connect(m_tcpSocketKodiEventServer, &QTcpSocket::readyRead, context_kodi,
                                     &Kodi::readTcpData);
                    QObject::connect(m_tcpSocketKodiEventServer, &QTcpSocket::disconnected, context_kodi,
                                     &Kodi::clientDisconnected);
                    m_flagKodiEventServerOnline = true;
                    QObject::connect(context_kodi, &Kodi::requestReadygetCurrentPlayer, context_kodi,
                                     &Kodi::updateCurrentPlayer);
                } else {
                    m_flagKodiEventServerOnline = false;
                }
                m_pollingTimer->start();
                getKodiAvailableTVChannelList();
                getKodiAvailableRadioChannelList();
                getCurrentPlayer();
                setState(CONNECTED);
            } else {
                m_flagKodiOnline = true;
                getCurrentPlayer();
            }

        } else {
            if (_networktries == MAX_CONNECTIONTRY) {
                _networktries = 0;
                m_flagKodiOnline = false;
                m_notifications->add(
                    true, tr("Cannot connect to ").append(friendlyName()).append("."), tr("Reconnect"),
                    [](QObject* param) {
                        Integration* i = qobject_cast<Integration*>(param);
                        i->connect();
                    },
                    context_kodi);
                disconnect();
                qCWarning(m_logCategory) << "Kodi not reachable";
            } else {
                _networktries++;
                postRequest(
                    "{ \"jsonrpc\": \"2.0\","
                    " \"method\": \"JSONRPC.Ping\", \"params\": {  },"
                    " \"id\":\"ConnectionCheck\"}");
            }
        }
    } else if (m_kodireply->error() == QNetworkReply::NetworkError::OperationCanceledError) {
    } else {
        if (_networktries == MAX_CONNECTIONTRY) {
            _networktries = 0;
            m_flagKodiOnline = false;
            m_notifications->add(
                true, tr("Cannot connect to ").append(friendlyName()).append("."), tr("Reconnect"),
                [](QObject* param) {
                    Integration* i = qobject_cast<Integration*>(param);
                    i->connect();
                },
                context_kodi);
            qCDebug(m_logCategory) << m_kodireply->error() << m_kodireply->errorString();
            disconnect();
            qCWarning(m_logCategory) << "Kodi not reachable";
        } else {
            _networktries++;
            postRequest(
                "{ \"jsonrpc\": \"2.0\","
                " \"method\": \"JSONRPC.Ping\", \"params\": {  },"
                " \"id\":\"ConnectionCheck\"}");
        }
    }
}

void Kodi::Tvheadendconnectioncheck(const QJsonDocument& resultJSONDocument) {
    if (resultJSONDocument.object().contains("name")) {
        // qCDebug(m_logCategory) << "tvheadend configured";
        m_flagTVHeadendOnline = true;
        // getTVEPGfromTVHeadend();
        if (!m_pollingEPGLoadTimer->isActive()) {
            m_pollingEPGLoadTimer->setInterval(10000);
            QObject::connect(m_pollingEPGLoadTimer, &QTimer::timeout, context_kodi,
                             &Kodi::onPollingEPGLoadTimerTimeout);
            m_pollingEPGLoadTimer->start();
        }
    } else {
        m_flagTVHeadendOnline = false;
        tvheadendGetRequest("/api/serverinfo", {});
        qCWarning(m_logCategory) << "TV Headend not reachable";
    }
}

void Kodi::KodiApplicationProperties() {
    QObject* contextKodiApplicationProperties = new QObject(context_kodi);

    QObject::connect(context_kodi, &Kodi::requestReadyKodiApplicationProperties, contextKodiApplicationProperties,
                     [=](const QJsonDocument& resultJSONDocument) {
                         EntityInterface* entity =
                             static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));
                         if (resultJSONDocument.object().value("id") == "Application.GetProperties") {
                             // QString strJson(resultJSONDocument.toJson(QJsonDocument::Compact));
                             // qCDebug(m_logCategory) << strJson;
                             entity->updateAttrByIndex(MediaPlayerDef::VOLUME,
                                                       resultJSONDocument.object().value("result")["volume"].toInt());
                         }
                         contextKodiApplicationProperties->deleteLater();
                     });
    postRequest(
        "{ \"jsonrpc\": \"2.0\","
        " \"method\": \"Application.GetProperties\", \"params\" : { \"properties\" : [ \"volume\", \"muted\" ] },"
        " \"id\":\"Application.GetProperties\"}");
}
