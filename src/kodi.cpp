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

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextCodec>
#include <QXmlStreamReader>
#include <QProcess>
#include <QDate>
#include <QTcpSocket>

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
    m_completeKodiJSONRPCUrl = m_KodiClientUrl +":" + m_KodiClientPort +"/jsonrpc";
    m_pollingTimer = new QTimer(this);
    // m_pollingTimer->setInterval(2000);
    // QObject::connect(m_pollingTimer, &QTimer::timeout, this, &Kodi::onPollingTimerTimeout);
    m_progressBarTimer = new QTimer(this);
    // m_progressBarTimer->setInterval(1000);
    // QObject::connect(m_progressBarTimer, &QTimer::timeout, this, &Kodi::onProgressBarTimerTimeout);

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
    QNetworkAccessManager* networkManagerTvHeadend = new QNetworkAccessManager(this);
    QNetworkRequest requestKodi;
    QNetworkRequest requestTVHeadend;
    QNetworkAccessManager* networkManagerKodi = new QNetworkAccessManager(this);

    /*QObject::connect(&m_checkProcessKodiAvailability,
                     static_cast<void(QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
                     [this](int exitCode, QProcess::ExitStatus exitStatus) {
        if (exitCode == 0 && exitStatus == QProcess::ExitStatus::NormalExit) {*/
    QObject::connect(networkManagerKodi, &QNetworkAccessManager::finished, this, [=](QNetworkReply* reply) {
        QString     answer = reply->readAll();
        if (!reply->error() && answer.contains("pong")) {
            m_flagKodiOnline = true;
            m_pollingTimer->setInterval(2000);
            QObject::connect(m_pollingTimer, &QTimer::timeout, this, &Kodi::onPollingTimerTimeout);

            m_progressBarTimer->setInterval(1000);
            QObject::connect(m_progressBarTimer, &QTimer::timeout, this, &Kodi::onProgressBarTimerTimeout);
            m_tcpSocketKodiEventServer = new QTcpSocket(this);

            m_tcpSocketKodiEventServer->connectToHost(m_KodiClientUrl, 9090);
            if (m_tcpSocketKodiEventServer->waitForConnected()) {
                QObject::connect(m_tcpSocketKodiEventServer, SIGNAL(readyRead()), SLOT(readTcpData()) );
                QObject::connect(m_tcpSocketKodiEventServer, SIGNAL(stateChanged()), SLOT(checkTCPSocket()) );
                m_flagKodiEventServerOnline = true;
            } else {
                m_flagKodiEventServerOnline = false;
            }
            m_pollingTimer->start();
            getKodiAvailableTVChannelList();
            setState(CONNECTED);
        } else {
            m_flagKodiOnline = false;
            m_notifications->add(
                        true, tr("Cannot connect to ").append(friendlyName()).append("."), tr("Reconnect"),
                        [](QObject* param) {
                Integration* i = qobject_cast<Integration*>(param);
                i->connect();
            },
            this);
            disconnect();
            qCDebug(m_logCategory) << "Kodi not reachable";
        }
        // QObject::disconnect(&m_checkProcessKodiAvailability,
        //                  static_cast<void(QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished), 0, 0);
        QObject::disconnect(networkManagerKodi, &QNetworkAccessManager::finished, this, 0);
    });

    /*QObject::connect(&m_checkProcessTVHeadendAvailability,
                     static_cast<void(QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
                     [this](int exitCode, QProcess::ExitStatus exitStatus) {
        if (exitCode == 0 && exitStatus == QProcess::ExitStatus::NormalExit) {*/
    QObject::connect(networkManagerTvHeadend, &QNetworkAccessManager::finished, this, [=](QNetworkReply* reply) {
        if (!reply->error()) {
            m_flagTVHeadendOnline = true;
            getTVEPGfromTVHeadend();
        } else {
            m_flagTVHeadendOnline = false;
            qCDebug(m_logCategory) << "TV Headend not reachable";
        }
        /*QObject::disconnect(&m_checkProcessTVHeadendAvailability,
                            static_cast<void(QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished), 0, 0);*/
        QObject::disconnect(networkManagerTvHeadend, &QNetworkAccessManager::finished, this, 0);
    });

    setState(CONNECTING);


    qCDebug(m_logCategory) << "STARTING Kodi";
    if (m_TvheadendClientUrl != "" && m_TvheadendClientPort != "") {
        m_flagTVHeadendConfigured = true;
        m_completeTVheadendJSONUrl = m_TvheadendClientUrl + ":" + m_TvheadendClientPort;
        // m_checkProcessTVHeadendAvailability.start("curl", QStringList() << "-s" << m_completeTVheadendJSONUrl);
        QString concatenated = m_TvheadendClientUser+":"+m_TvheadendClientPassword;
        QByteArray data = concatenated.toLocal8Bit().toBase64();
        QString headerData = "Basic " + data;
        requestTVHeadend.setRawHeader("Authorization", headerData.toLocal8Bit());
        // set the URL
        // url = "/v1/me/player"
        // params = "?q=stringquery&limit=20"
        requestTVHeadend.setUrl(QUrl(m_completeTVheadendJSONUrl));

        requestTVHeadend.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        networkManagerTvHeadend->get(requestTVHeadend);
    } else {
        qCDebug(m_logCategory) << "TVHeadend not confgured";
        m_flagTVHeadendConfigured = false;
    }
    if (m_KodiClientUrl != "" && m_KodiClientPort != "" && m_completeKodiJSONRPCUrl != "") {
        m_flagKodiConfigured = true;
        // m_checkProcessKodiAvailability.start("curl", QStringList() << "-s" << m_completeKodiJSONRPCUrl);
        /*m_KodiClientUrl +":" + m_KodiClientPort +"/jsonrpc", "getSingleTVChannelList",
                            "{ \"jsonrpc\": \"2.0\", \"method\": \"JSONRPC.Ping\","
                            " \"params\": {  }, \"id\": "+QString::number(m_globalKodiRequestID)+" }"*/
        QByteArray paramutf8 = QString("{ \"jsonrpc\": \"2.0\", \"method\": \"JSONRPC.Ping\","
                                       " \"params\": {  }, \"id\": "
                                       +QString::number(m_globalKodiRequestID)+" }").toUtf8();
        QUrl urlKodi = QUrl(m_completeKodiJSONRPCUrl);
        requestKodi.setUrl(urlKodi);
        requestKodi.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        requestKodi.setHeader(QNetworkRequest::ContentLengthHeader, QByteArray::number(paramutf8.size()));

        // send the get request
        networkManagerKodi->post(requestKodi, paramutf8);

    } else {
        qCDebug(m_logCategory) << "Kodi not confgured";
        m_flagKodiConfigured = false;
    }
}
void Kodi::clearMediaPlayerEntity() {
    EntityInterface* entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));
    entity->updateAttrByIndex(MediaPlayerDef::MEDIATYPE, "");
    // get the track title
    entity->updateAttrByIndex(MediaPlayerDef::MEDIATITLE, "");
    // get the artist
    entity->updateAttrByIndex(MediaPlayerDef::MEDIAARTIST, "");
    entity->updateAttrByIndex(MediaPlayerDef::MEDIAARTIST, "");
    entity->updateAttrByIndex(MediaPlayerDef::MEDIAIMAGE, "");
    entity->updateAttrByIndex(MediaPlayerDef::STATE, MediaPlayerDef::States::IDLE);
}
void Kodi::checkTCPSocket() {
    if (m_tcpSocketKodiEventServer->state() == QTcpSocket::ConnectedState) {
        m_flagKodiEventServerOnline = true;
    } else {
        m_flagKodiEventServerOnline = false;
    }
}
void Kodi::readTcpData() {
    QString reply = m_tcpSocketKodiEventServer->readAll();
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
            }
        }
    }
}

void Kodi::disconnect() {
    m_pollingTimer->stop();
    m_progressBarTimer->stop();
    QObject::disconnect(m_pollingTimer, &QTimer::timeout, this, &Kodi::onPollingTimerTimeout);
    QObject::disconnect(m_progressBarTimer, &QTimer::timeout, this, &Kodi::onProgressBarTimerTimeout);
    QObject::disconnect(this, &Kodi::requestReadygetCurrentPlayer, 0, 0);

    if (m_flagKodiEventServerOnline) {
        m_tcpSocketKodiEventServer->close();

        QObject::disconnect(m_tcpSocketKodiEventServer, SIGNAL(readyRead()), 0, 0);
        QObject::disconnect(m_tcpSocketKodiEventServer, SIGNAL(stateChanged()), 0, 0);
    }

    QObject::disconnect(&m_checkProcessTVHeadendAvailability,
                        static_cast<void(QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished), 0, 0);
    QObject::disconnect(&m_checkProcessKodiAvailability,
                        static_cast<void(QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished), 0, 0);
    if (m_checkProcessKodiAvailability.Running) {
        m_checkProcessKodiAvailability.close();
    }
    if (m_checkProcessTVHeadendAvailability.Running) {
        m_checkProcessTVHeadendAvailability.close();
    }

    clearMediaPlayerEntity();
    setState(DISCONNECTED);
}

void Kodi::enterStandby() {
    disconnect();
}

void Kodi::leaveStandby() {
    connect();
}

void Kodi::getTVEPGfromTVHeadend() {
    QObject* context_getTVEPGfromTVHeadend = new QObject(this);
    QObject::connect(this, &Kodi::requestReadygetTVEPGfromTVHeadend, context_getTVEPGfromTVHeadend,
                     [=](const QString& answer, const QString& requestFunction) {
        if (requestFunction == "getTVEPGfromTVHeadend") {
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
        QObject::disconnect(this, &Kodi::requestReadygetTVEPGfromTVHeadend, context_getTVEPGfromTVHeadend, 0);
    });
    int temp_Timestamp = (m_EPGExpirationTimestamp - (QDateTime(QDate::currentDate()).toTime_t()));
    if (m_flagTVHeadendOnline && temp_Timestamp <= 0) {
        getRequestWithAuthentication(m_TvheadendClientUrl +":" + m_TvheadendClientPort +
                                     "/api/epg/events/grid?limit=2000", "getTVEPGfromTVHeadend",
                                     m_TvheadendClientUser, m_TvheadendClientPassword);
    }
}
void Kodi::getSingleTVChannelList(QString param) {
    QObject* context_getSingleTVChannelList = new QObject(this);

    QString channelnumber = "0";
    for (int i = 0; i < m_KodiTVChannelList.length(); i++) {
        if (m_KodiTVChannelList[i].toMap().value("channelid").toString() == param) {
            channelnumber = m_KodiTVChannelList[i].toMap().value("channelnumber").toString();
        }
    }
    if (channelnumber != "0" && m_flagTVHeadendOnline) {
        QObject::connect(this, &Kodi::requestReadygetSingleTVChannelList, context_getSingleTVChannelList,
                         [=](const QString& answer, const QString& requestFunction) {
            if (requestFunction == "getSingleTVChannelList") {
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
                QString type = "tvchannellist";
                QString time = "";
                QString image = QString::fromStdString(
                            QByteArray::fromPercentEncoding(
                                m_KodiTVChannelList[currenttvchannelarrayid].toMap().
                                value("thumbnail").toString().toUtf8()).toStdString()).mid(8);
                if (image.contains("127.0.0.1")) {
                    image = image.replace("127.0.0.1", m_TvheadendClientUrl.mid(7));
                }
                QStringList commands = {"PLAY"};

                BrowseTvChannelModel* tvchannel = new BrowseTvChannelModel(id, time, title, subtitle,
                                                                           type, image, commands, nullptr);

                for (auto key : currenttvprogramm.keys()) {
                    QDateTime timestamp;
                    timestamp.setTime_t(key.toUInt());

                    tvchannel->addtvchannelItem(m_KodiTVChannelList[currenttvchannelarrayid].toMap().
                                                value("channelid").toString(), timestamp.toString("hh:mm"),
                                                currenttvprogramm.value(key), "", "tvchannel", "", commands);
                }


                if (entity) {
                    MediaPlayerInterface* me = static_cast<MediaPlayerInterface*>(entity->getSpecificInterface());
                    me->setBrowseModel(tvchannel);
                }
            }
            context_getSingleTVChannelList->deleteLater();
            QObject::disconnect(this, &Kodi::requestReadygetSingleTVChannelList, context_getSingleTVChannelList, 0);
        });

        getRequestWithAuthentication(m_TvheadendClientUrl +":" + m_TvheadendClientPort +
                                     "/api/epg/events/grid?limit=1", "getSingleTVChannelList", m_TvheadendClientUser,
                                     m_TvheadendClientPassword);

    } else if (!m_flagTVHeadendOnline) {
        QObject::connect(this, &Kodi::requestReadygetSingleTVChannelList, context_getSingleTVChannelList,
                         [=](const QString& answer, const QString& rMethod) {
            if (rMethod == "getSingleTVChannelList") {
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
                    if ( map.contains("result") ) {
                        if (map.value("result") == "pong") {
                            EntityInterface* entity =
                                    static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));
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
                            QString type = "tvchannellist";
                            QString image = QString::fromStdString(
                                        QByteArray::fromPercentEncoding(
                                            m_KodiTVChannelList[currenttvchannelarrayid].toMap().
                                            value("thumbnail").toString().toUtf8()).toStdString()).mid(8);
                            if (image.contains("127.0.0.1")) {
                                image = image.replace("127.0.0.1", m_TvheadendClientUrl.mid(7));
                            }
                            QStringList commands = {};

                            BrowseTvChannelModel* tvchannel = new BrowseTvChannelModel(id, "",
                                                                                       title, subtitle, type, image,
                                                                                       commands, nullptr);
                            tvchannel->addtvchannelItem(m_KodiTVChannelList[currenttvchannelarrayid].toMap().
                                                        value("channelid").toString(), "", "No programm available",
                                                        "", "tvchannel", "", commands);

                            if (entity) {
                                MediaPlayerInterface* me =
                                        static_cast<MediaPlayerInterface*>(entity->getSpecificInterface());
                                me->setBrowseModel(tvchannel);
                            }
                        }
                    }
                }
            }
            context_getSingleTVChannelList->deleteLater();
            QObject::disconnect(this, &Kodi::requestReadygetSingleTVChannelList, context_getSingleTVChannelList, 0);
        });
        qCDebug(m_logCategory) << "GET USERS PLAYLIST";
        QString jsonstring;

        postRequest(m_KodiClientUrl +":" + m_KodiClientPort +"/jsonrpc", "getSingleTVChannelList",
                    "{ \"jsonrpc\": \"2.0\", \"method\": \"JSONRPC.Ping\","
                    " \"params\": {  }, \"id\": "+QString::number(m_globalKodiRequestID)+" }");
    } else {
        QObject::connect(this, &Kodi::requestReadygetSingleTVChannelList, context_getSingleTVChannelList,
                         [=](const QString& answer, const QString& rMethod) {
            if (rMethod == "getSingleTVChannelList") {
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
                    if (map.contains("result")) {
                        if (map.value("result") == "pong") {
                            EntityInterface* entity =
                                    static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));
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
                            QString type = "tvchannellist";
                            QString image = QString::fromStdString(
                                        QByteArray::fromPercentEncoding(
                                            m_KodiTVChannelList[currenttvchannelarrayid].toMap().
                                            value("thumbnail").toString().toUtf8()).toStdString()).mid(8);
                            if (image.contains("127.0.0.1")) {
                                image = image.replace("127.0.0.1", m_TvheadendClientUrl.mid(7));
                            }
                            QStringList commands = {};

                            BrowseTvChannelModel* tvchannel = new BrowseTvChannelModel(id, "", title,
                                                                                       subtitle, type,
                                                                                       image, commands, nullptr);
                            tvchannel->addtvchannelItem(m_KodiTVChannelList[currenttvchannelarrayid].toMap().
                                                        value("channelid").toString(), "", "No programm available",
                                                        "", "tvchannel", "", commands);
                            if (entity) {
                                MediaPlayerInterface* me = static_cast<MediaPlayerInterface*>
                                        (entity->getSpecificInterface());
                                me->setBrowseModel(tvchannel);
                            }
                        }
                    }
                }
            }
            context_getSingleTVChannelList->deleteLater();
            QObject::disconnect(this, &Kodi::requestReadygetSingleTVChannelList, context_getSingleTVChannelList, 0);
        });
        qCDebug(m_logCategory) << "GET USERS PLAYLIST";
        QString jsonstring;

        postRequest(m_KodiClientUrl +":" + m_KodiClientPort +
                    "/jsonrpc", "getSingleTVChannelList", "{ \"jsonrpc\": \"2.0\", "
                                                          "\"method\": \"JSONRPC.Ping\","
                                                          " \"params\": {  }, \"id\": "+
                    QString::number(m_globalKodiRequestID)+" }");
    }
}
void Kodi::getKodiChannelNumberToTVHeadendUUIDMapping() {
    QObject* context_getKodiChannelNumberToTVHeadendUUIDMapping = new QObject(this);
    QString jsonstring;
    QObject::connect(this, &Kodi::requestReadygetKodiChannelNumberToTVHeadendUUIDMapping,
                     context_getKodiChannelNumberToTVHeadendUUIDMapping,
                     [=](const QString& repliedString, const QString& requestFunction) {
        if (requestFunction == "getKodiChannelNumberToTVHeadendUUIDMapping") {
            QJsonParseError parseerror;
            QList<QVariant> mapOfEntries;
            QJsonDocument   doc = QJsonDocument::fromJson(repliedString.toUtf8(), &parseerror);
            if (parseerror.error != QJsonParseError::NoError) {
                qCWarning(m_logCategory) << "JSON error : " << parseerror.errorString();
                return;
            }
            mapOfEntries = doc.toVariant().toMap().value("entries").toList();
            for (int i = 0; i < mapOfEntries.length(); i++) {
                for (int j = 0; j < m_KodiTVChannelList.length(); j++) {
                    if (m_KodiTVChannelList[j].toMap().values().
                            indexOf(mapOfEntries[i].toMap().value("val").toString()) > 0)  {
                        m_mapKodiChannelNumberToTVHeadendUUID.insert(m_KodiTVChannelList[j].toMap()
                                                                     .value("channelnumber").toInt(),
                                                                     mapOfEntries[i].toMap().value("key")
                                                                     .toString());
                        break;
                    }
                }
            }
        }
        QObject::disconnect(this, &Kodi::requestReadygetKodiChannelNumberToTVHeadendUUIDMapping,
                            context_getKodiChannelNumberToTVHeadendUUIDMapping, 0);
        context_getKodiChannelNumberToTVHeadendUUIDMapping->deleteLater();
    });
    //
    getRequestWithAuthentication(m_TvheadendClientUrl +":" + m_TvheadendClientPort +
                                 "/api/channel/list", "getKodiChannelNumberToTVHeadendUUIDMapping",
                                 m_TvheadendClientUser, m_TvheadendClientPassword);
}

void Kodi::getKodiAvailableTVChannelList() {
    QObject* context_getgetKodiAvailableTVChannelList = new QObject(this);
    QString jsonstring;

    QObject::connect(this, &Kodi::requestReadygetKodiAvailableTVChannelList, context_getgetKodiAvailableTVChannelList,
                     [=](const QVariantMap& map, const QString& requestFunction) {
        if (requestFunction == "getKodiAvailableTVChannelList") {
            if (map.contains("result")) {
                if (map.value("result").toMap().count() != 0) {
                    m_KodiTVChannelList = map.value("result").toMap().value("channels").toList();
                    if (m_flagTVHeadendOnline) {
                        if (m_mapKodiChannelNumberToTVHeadendUUID.isEmpty())  {
                            getKodiChannelNumberToTVHeadendUUIDMapping();
                        } else {
                            qCDebug(m_logCategory) << "m_mapKodiChannelNumberToTVHeadendUUID already loaded";
                        }
                    } else {
                        qCDebug(m_logCategory) << "TV Headend not configured";
                    }
                }
            }
        }
        QObject::disconnect(this, &Kodi::requestReadygetKodiAvailableTVChannelList,
                            context_getgetKodiAvailableTVChannelList, 0);
        context_getgetKodiAvailableTVChannelList->deleteLater();
    });
    if (m_flagKodiOnline) {
        QString jsonstring = "{\"jsonrpc\":\"2.0\",\"id\": "+
                QString::number(m_globalKodiRequestID)+",\"method\":\"PVR.GetChannels\","
                                                       " \"params\": {\"channelgroupid\": \"alltv\", \"properties\":"
                                                       "[\"thumbnail\",\"uniqueid\",\"channelnumber\"]}}";
        postRequest(m_KodiClientUrl +":" + m_KodiClientPort +"/jsonrpc", "getKodiAvailableTVChannelList", jsonstring);
    }
}

void Kodi::getCompleteTVChannelList() {
    QObject* context_getCompleteTVChannelList = new QObject(this);
    QObject::connect(this, &Kodi::requestReadygetCompleteTVChannelList, context_getCompleteTVChannelList,
                     [=](const QVariantMap& map, const QString& rMethod) {
        if (rMethod == "getCompleteTVChannelList" && map.contains("result")) {
            if (map.value("result") == "pong") {
                EntityInterface* entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));
                QString     channelId = "";
                QString     label = "";
                QString     thumbnail = "";
                QString     unqueId = "";
                QString     type = "tvchannellist";
                QStringList commands = {};
                BrowseTvChannelModel* tvchannel = new BrowseTvChannelModel(channelId, "",
                                                                           label, unqueId, type, thumbnail,
                                                                           commands, nullptr);

                for (int i = 0; i < m_KodiTVChannelList.length(); i++) {
                    QString thumbnail = QString::fromStdString(
                                QByteArray::fromPercentEncoding(m_KodiTVChannelList[i].toMap().
                                                                value("thumbnail").toString().toUtf8())
                                .toStdString()).mid(8);
                    if (thumbnail.contains("127.0.0.1")) {
                        thumbnail = thumbnail.replace("127.0.0.1", m_TvheadendClientUrl.mid(7));
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
        // QObject::disconnect(this, &Kodi::requestReadygetCompleteTVChannelList, context_getCompleteTVChannelList, 0);
        context_getCompleteTVChannelList->deleteLater();
    });
    qCDebug(m_logCategory) << "GET USERS PLAYLIST";

    if (m_flagKodiOnline) {
        // m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::NotActive;
        QString jsonstring;
        postRequest(m_KodiClientUrl +":" + m_KodiClientPort +
                    "/jsonrpc", "getCompleteTVChannelList", "{ \"jsonrpc\": \"2.0\","
                                                            " \"method\": \"JSONRPC.Ping\", \"params\": {  },"
                                                            " \"id\":" +QString::number(m_globalKodiRequestID)+ "}");
    }
}

void Kodi::getCurrentPlayer() {
    QObject* contextgetCurrentPlayer = new QObject(this);
    QString method = "Player.GetActivePlayers";
    QString thumbnail = "";

    QObject::connect(this, &Kodi::requestReadygetCurrentPlayer, contextgetCurrentPlayer,
                     [=](const QVariantMap& map, const QString& rMethod) {
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
                        // m_flag = false;
                    } else {
                        // m_flag = false;
                    }
                } else {
                    // m_flag = false;
                }
            }
        } else if (rMethod == "Player.GetItem") {
            if (map.contains("result")) {
                if (map.value("result").toMap().value("item").toMap().contains("type")) {
                    if (map.value("result").toMap().value("item").toMap().value("type") == "channel") {
                        if (map.value("result").toMap().value("item").toMap().
                                value("title").toString() == m_KodiCurrentPlayerTitle) {
                            // m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetActivePlayers;
                            // m_flag = false;
                        } else {
                            QVariant fanart = map.value("result").toMap().value("item").toMap().value("fanart");
                            QVariant id = map.value("result").toMap().value("item").toMap().value("id");
                            QVariant label = map.value("result").toMap().value("item").toMap().value("label");
                            QVariant title = map.value("result").toMap().value("item").toMap().value("title");
                            m_KodiCurrentPlayerTitle = title.toString();
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
                                m_KodiCurrentPlayerThumbnail = map.value("result").toMap().
                                        value("item").toMap().value("thumbnail").toString();
                                m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::PrepareDownload;
                            } else {
                                m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetProperties;
                            }
                            // m_flag = false;
                        }
                    } else if (map.value("result").toMap().value("item").toMap().value("type") == "unknown") {
                        if (map.value("result").toMap().value("item").toMap().
                                value("title").toString() == m_KodiCurrentPlayerTitle) {
                            m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetActivePlayers;
                            // m_flag = false;
                        } else {
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
                                m_KodiCurrentPlayerThumbnail = map.value("result").toMap().value("item").
                                        toMap().value("thumbnail").toString();
                                m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::PrepareDownload;
                            } else {
                                m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetProperties;
                            }
                            // m_flag = false;
                        }
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
                    // m_flag = false;
                } else {
                    // m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetProperties;
                    // m_flag = false;
                }
            } else {
                // m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetProperties;
                // m_flag = false;
            }
        } else if (rMethod == "Player.GetProperties") {
            if (map.contains("result")) {
                m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetActivePlayers;



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
                // m_flag = false;
            } else {
                m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetActivePlayers;
                // m_flag = false;
            }
        }
        contextgetCurrentPlayer->deleteLater();
    });

    if (m_flagKodiOnline) {
        if ((m_KodiGetCurrentPlayerState == KodiGetCurrentPlayerState::GetActivePlayers ||
             m_KodiGetCurrentPlayerState == KodiGetCurrentPlayerState::Stopped)) {  // && !// m_flag) {
            postRequest(m_KodiClientUrl +":" + m_KodiClientPort +"/jsonrpc", method, m_globalKodiRequestID);
            // m_flag = true;
        } else if (m_KodiGetCurrentPlayerState == KodiGetCurrentPlayerState::GetItem) {     // && !// m_flag) {
            QString jsonstring;
            if (m_currentkodiplayertype == "video") {
                jsonstring = "{\"jsonrpc\": \"2.0\", \"method\": \"Player.GetItem\",\"params\":"
                             "{ \"properties\": [\"title\", \"album\", \"artist\", \"season\", \"episode\","
                             " \"duration\", \"showtitle\", \"tvshowid\", \"thumbnail\", \"file\", \"fanart\","
                             " \"streamdetails\"], \"playerid\": "
                             "" + QString::number(m_currentkodiplayerid) +
                        " }, \"id\": "+QString::number(m_globalKodiRequestID)+"}";
                postRequest(m_KodiClientUrl +":" + m_KodiClientPort +"/jsonrpc", "Player.GetItem", jsonstring);
                // m_flag = true;
            } else if (m_currentkodiplayertype == "audio") {
                jsonstring = "{\"jsonrpc\": \"2.0\", \"method\": \"Player.GetItem\",\"params\":"
                             "{ \"properties\": [\"title\", \"album\", \"artist\", \"season\", "
                             "\"episode\", \"duration\", \"showtitle\", \"tvshowid\", \"thumbnail\", "
                             "\"file\", \"fanart\", \"streamdetails\"], \"playerid\": "
                             "" + QString::number(m_currentkodiplayerid) +
                        " }, \"id\": "+QString::number(m_globalKodiRequestID)+"}";
                postRequest(m_KodiClientUrl +":" + m_KodiClientPort +"/jsonrpc", "Player.GetItem", jsonstring);
                // m_flag = true;
            }
        } else if (m_KodiGetCurrentPlayerState == KodiGetCurrentPlayerState::GetProperties) {   // && !// m_flag) {
            QString jsonstring = "{\"jsonrpc\": \"2.0\", \"method\": \"Player.GetProperties\", "
                                 "\"params\": { \"playerid\":" +
                    QString::number(m_currentkodiplayerid) +", \"properties\": "
                                                            "[\"totaltime\", \"time\", \"speed\"] }, "
                                                            "\"id\": "+QString::number(m_globalKodiRequestID)+"}";
            postRequest(m_KodiClientUrl +":" + m_KodiClientPort +"/jsonrpc", "Player.GetProperties", jsonstring);
            // m_flag = true;
        } else if (m_KodiGetCurrentPlayerState == KodiGetCurrentPlayerState::PrepareDownload) {   //&& !// m_flag) {
            QString jsonstring = "{\"jsonrpc\": \"2.0\", \"method\": \"Files.PrepareDownload\", "
                                 "\"params\": { \"path\": \"" +
                    m_KodiCurrentPlayerThumbnail+"\" }, \"id\": "+QString::number(m_globalKodiRequestID)+"}";
            postRequest(m_KodiClientUrl +":" + m_KodiClientPort +"/jsonrpc", "Files.PrepareDownload", jsonstring);
            // m_flag = true;
        } else {
            // m_flag = false;
        }
    }
}
void Kodi::getRequestWithAuthentication(const QString& url, const QString& callFunction,
                                        const QString& user, const QString& password) {
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
            if (callFunction == "getKodiChannelNumberToTVHeadendUUIDMapping") {
                emit requestReadygetKodiChannelNumberToTVHeadendUUIDMapping(answer, callFunction);
            } else if (callFunction == "getTVEPGfromTVHeadend") {
                emit requestReadygetTVEPGfromTVHeadend(answer, callFunction);
            } else if (callFunction == "getSingleTVChannelList") {
                emit requestReadygetSingleTVChannelList(answer, callFunction);
            }
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
    QObject* contextsendCommand = new QObject(this);
    EntityInterface* entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));
    if (command == MediaPlayerDef::C_PLAY) {
    } else if (command == MediaPlayerDef::C_PLAY_ITEM) {
        if (param.toMap().value("type") == "tvchannellist" || param.toMap().value("type") == "track") {
            QObject::connect(this, &Kodi::requestReadyCommandPlay, contextsendCommand,
                             [=](const QVariantMap& map, const QString& rUrl) {
                if (rUrl == "sendCommand") {
                    if (map.contains("result")) {
                        if (map.value("result") == "OK") {
                            getCurrentPlayer();
                        }
                    }
                    contextsendCommand->deleteLater();
                    QObject::disconnect(this, &Kodi::requestReadyCommandPlay, contextsendCommand, 0);
                }});
            QString jsonstring = "{\"jsonrpc\": \"2.0\", \"method\": \"Player.Open\",\"params\":"
                                 " {\"item\":{\"channelid\": "+
                    param.toMap().value("id").toString()+"}}, \"id\": "
                                                         ""+QString::number(m_globalKodiRequestID)+"}";
            qCDebug(m_logCategory) << jsonstring;
            postRequest(m_KodiClientUrl +":" + m_KodiClientPort +"/jsonrpc", "sendCommandPlay", jsonstring);
        }
    } else if (command == MediaPlayerDef::C_QUEUE) {
    } else if (command == MediaPlayerDef::C_PAUSE) {
        QObject::connect(this, &Kodi::requestReadyCommandPause, contextsendCommand,
                         [=](const QVariantMap& map, const QString& rUrl) {
            if (rUrl == "Player.Stop") {
                if (map.contains("result")) {
                    if (map.value("result") == "OK") {
                        m_progressBarTimer->stop();
                        m_currentkodiplayertype ="unknown";
                        m_currentkodiplayerid = -1;
                        m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::Stopped;
                        clearMediaPlayerEntity();
                        getCurrentPlayer();
                    }
                }
                contextsendCommand->deleteLater();
                QObject::disconnect(this, &Kodi::requestReadyCommandPause, contextsendCommand, 0);
            }});
        QString jsonstring = "{\"jsonrpc\": \"2.0\", \"method\": \"Player.Stop\","
                             " \"params\": { \"playerid\": " +
                QString::number(m_currentkodiplayerid) + " },\"id\": "
                +QString::number(m_globalKodiRequestID)+"}";
        postRequest(m_KodiClientUrl +":" + m_KodiClientPort +"/jsonrpc", "sendCommandPause", jsonstring);
    } else if (command == MediaPlayerDef::C_NEXT) {
        if (m_currentKodiMediaType == "channel") {
            QObject::connect(this, &Kodi::requestReadyCommandNext, contextsendCommand,
                             [=](const QVariantMap& map, const QString& rUrl) {
                if (rUrl == "Input.ExecuteAction") {
                    if (map.contains("result")) {
                        if (map.value("result") == "OK") {
                            m_progressBarTimer->stop();
                            getCurrentPlayer();
                        }
                    }
                    contextsendCommand->deleteLater();
                    QObject::disconnect(this, &Kodi::requestReadyCommandNext, contextsendCommand, 0);
                }});
            QString jsonstring = "{\"jsonrpc\": \"2.0\", \"method\":"
                                 " \"Input.ExecuteAction\",\"params\": "
                                 "{ \"action\": \"channelup\" }, \"id\":"
                                 " "+QString::number(m_globalKodiRequestID)+"}";
            postRequest(m_KodiClientUrl +":" + m_KodiClientPort +"/jsonrpc", "sendCommandNext", jsonstring);
        }
    } else if (command == MediaPlayerDef::C_PREVIOUS) {
        if (m_currentKodiMediaType == "channel") {
            QObject::connect(this, &Kodi::requestReadyCommandPrevious, contextsendCommand,
                             [=](const QVariantMap& map, const QString& rUrl) {
                if (rUrl == "Input.ExecuteAction") {
                    if (map.contains("result")) {
                        if (map.value("result") == "OK") {
                            m_progressBarTimer->stop();
                            getCurrentPlayer();
                        }
                    }
                    contextsendCommand->deleteLater();
                    QObject::disconnect(this, &Kodi::requestReadyCommandPrevious, contextsendCommand, 0);
                }});
            QString jsonstring = "{\"jsonrpc\": \"2.0\","
                                 " \"method\": \"Input.ExecuteAction\","
                                 "\"params\": { \"action\": \"channeldown\" }, "
                                 "\"id\": "+QString::number(m_globalKodiRequestID)+"}";
            postRequest(m_KodiClientUrl +":" + m_KodiClientPort +"/jsonrpc", "sendCommandPrevious", jsonstring);
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
    }
    /*else if (command == MediaPlayerDef::C_GETPLAYLIST) {
        if (param == "user") {
            // getCompleteTVChannelList();
        } else {
            // getSingleTVChannelList(param.toString());
        }
    }*/
}

void Kodi::postRequest(const QString& url, const QString& callFunction, const int& requestid) {
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
            if (callFunction == "Player.GetActivePlayers" ||
                    callFunction == "Player.GetItem" ||
                    callFunction == "Player.GetProperties" ||
                    callFunction == "Files.PrepareDownload") {
                emit requestReadygetCurrentPlayer(map, callFunction);
            } else {
                qCDebug(m_logCategory) << "no callback function defined for " << callFunction;
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

    QJsonObject json;
    json.insert("jsonrpc", "2.0");
    json.insert("method", callFunction);
    json.insert("id", requestid);
    QJsonDocument jsonDoc(json);
    QByteArray jsonData = jsonDoc.toJson();
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setHeader(QNetworkRequest::ContentLengthHeader, QByteArray::number(jsonData.size()));

    // send the get request
    manager->post(request, jsonData);
}

void Kodi::postRequest(const QString& url, const QString& callfunction, const QString& param) {
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
            if (callfunction == "getKodiAvailableTVChannelList") {
                emit requestReadygetKodiAvailableTVChannelList(map, callfunction);
            } else if (callfunction == "getSingleTVChannelList") {
                emit requestReadygetSingleTVChannelList(answer, callfunction);
            } else if (callfunction == "getCompleteTVChannelList") {
                emit requestReadygetCompleteTVChannelList(map, callfunction);
            } else if (callfunction == "sendCommandPlay") {
                emit requestReadyCommandPlay(map, callfunction);
            } else if (callfunction == "sendCommandPause") {
                emit requestReadyCommandPause(map, callfunction);
            } else if (callfunction == "sendCommandNext") {
                emit requestReadyCommandNext(map, callfunction);
            } else if (callfunction == "sendCommandPrevious") {
                emit requestReadyCommandPrevious(map, callfunction);
            } else if (callfunction == "Player.GetActivePlayers" ||
                       callfunction == "Player.GetItem" ||
                       callfunction == "Player.GetProperties" ||
                       callfunction == "Files.PrepareDownload") {
                emit requestReadygetCurrentPlayer(map, callfunction);
            } else {
                qCDebug(m_logCategory) << "no callback function defined for " << callfunction;
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
    QNetworkAccessManager* networkManagerTvHeadend = new QNetworkAccessManager(this);
    QNetworkRequest requestKodi;
    QNetworkRequest requestTVHeadend;
    QNetworkAccessManager* networkManagerKodi = new QNetworkAccessManager(this);

    /*QObject::connect(&m_checkProcessKodiAvailability,
                     static_cast<void(QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
                     [this](int exitCode, QProcess::ExitStatus exitStatus) {
        if (exitCode == 0 && exitStatus == QProcess::ExitStatus::NormalExit) {*/
    QObject::connect(networkManagerKodi, &QNetworkAccessManager::finished, this, [=](QNetworkReply* reply) {
        QString     answer = reply->readAll();
        if (!reply->error() && answer.contains("pong")) {
            m_flagKodiOnline = true;
        } else {
            m_flagKodiOnline = false;
            m_notifications->add(
                        true, tr("Cannot connect to ").append(friendlyName()).append("."), tr("Reconnect"),
                        [](QObject* param) {
                Integration* i = qobject_cast<Integration*>(param);
                i->connect();
            },
            this);
            disconnect();
            qCDebug(m_logCategory) << "Kodi not reachable";
        }
        QObject::disconnect(networkManagerKodi, &QNetworkAccessManager::finished, this, 0);
    });

    /*QObject::connect(&m_checkProcessTVHeadendAvailability,
                     static_cast<void(QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
                     [this](int exitCode, QProcess::ExitStatus exitStatus) {
        if (exitCode == 0 && exitStatus == QProcess::ExitStatus::NormalExit) {*/
    QObject::connect(networkManagerTvHeadend, &QNetworkAccessManager::finished, this, [=](QNetworkReply* reply) {
        if (!reply->error()) {
            m_flagTVHeadendOnline = true;
        } else {
            m_flagTVHeadendOnline = false;
            qCDebug(m_logCategory) << "TV Headend not reachable";
        }
        /*QObject::disconnect(&m_checkProcessTVHeadendAvailability,
                            static_cast<void(QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished), 0, 0);*/
        QObject::disconnect(networkManagerTvHeadend, &QNetworkAccessManager::finished, this, 0);
    });




    if (m_flagKodiOnline) {
        /*if (!m_checkProcessKodiAvailability.Running)  {
            m_checkProcessKodiAvailability.start("curl", QStringList() << "-s" << m_completeKodiJSONRPCUrl);
        }*/
        QByteArray paramutf8 = QString("{ \"jsonrpc\": \"2.0\", \"method\": \"JSONRPC.Ping\","
                                       " \"params\": {  }, \"id\": "
                                       +QString::number(m_globalKodiRequestID)+" }").toUtf8();
        QUrl urlKodi = QUrl(m_completeKodiJSONRPCUrl);
        requestKodi.setUrl(urlKodi);
        requestKodi.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        requestKodi.setHeader(QNetworkRequest::ContentLengthHeader, QByteArray::number(paramutf8.size()));

        // send the get request
        networkManagerKodi->post(requestKodi, paramutf8);
        getCurrentPlayer();
    }
    int temp_Timestamp = (m_EPGExpirationTimestamp - (QDateTime(QDate::currentDate()).toTime_t()));
    if (m_flagTVHeadendOnline && temp_Timestamp <= 0) {
        getTVEPGfromTVHeadend();
        /*if (m_checkProcessTVHeadendAvailability.Running) {
            m_checkProcessTVHeadendAvailability.start("curl", QStringList() << "-s" << m_completeTVheadendJSONUrl);
        }*/
        QString concatenated = m_TvheadendClientUser+":"+m_TvheadendClientPassword;
        QByteArray data = concatenated.toLocal8Bit().toBase64();
        QString headerData = "Basic " + data;
        requestTVHeadend.setRawHeader("Authorization", headerData.toLocal8Bit());
        // set the URL
        // url = "/v1/me/player"
        // params = "?q=stringquery&limit=20"
        requestTVHeadend.setUrl(QUrl(m_completeTVheadendJSONUrl));

        requestTVHeadend.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        networkManagerTvHeadend->get(requestTVHeadend);
    }
}

void Kodi::onProgressBarTimerTimeout() {
    EntityInterface* entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));
    if (entity) {
        m_progressBarPosition++;
        entity->updateAttrByIndex(MediaPlayerDef::MEDIAPROGRESS, m_progressBarPosition);
    }
}
