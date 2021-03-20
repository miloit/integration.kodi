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

#pragma once
#include <QAuthenticator>
#include <QByteArray>
#include <QDebug>
#include <QNetworkAccessManager>
#include <QNetworkConfigurationManager>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QProcess>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

#include "yio-interface/entities/mediaplayerinterface.h"
#include "yio-model/mediaplayer/epgmodel_mediaplayer.h"
#include "yio-model/mediaplayer/searchmodel_mediaplayer.h"
#include "yio-model/mediaplayer/tvchannelmodel_mediaplayer.h"
#include "yio-plugin/integration.h"
#include "yio-plugin/plugin.h"

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//// Kodi FACTORY
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

const bool USE_WORKER_THREAD = false;

class KodiPlugin : public Plugin {
    Q_OBJECT
    Q_INTERFACES(PluginInterface)
    Q_PLUGIN_METADATA(IID "YIO.PluginInterface" FILE "kodi.json")

 public:
    KodiPlugin();

    // Plugin interface
 protected:
    Integration* createIntegration(const QVariantMap& config, EntitiesInterface* entities,
                                   NotificationsInterface* notifications, YioAPIInterface* api,
                                   ConfigInterface* configObj) override;
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//// Kodi CLASS
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class Kodi : public Integration {
    Q_OBJECT

 public:
    explicit Kodi(const QVariantMap& config, EntitiesInterface* entities, NotificationsInterface* notifications,
                  YioAPIInterface* api, ConfigInterface* configObj, Plugin* plugin);

    void sendCommand(const QString& type, const QString& entitId, int command, const QVariant& param) override;
    enum KodiGetCurrentPlayerState { GetActivePlayers, GetItem, PrepareDownload, Stopped, GetProperties, NotActive };
    Q_ENUM(KodiGetCurrentPlayerState);

 public slots:
    void connect() override;
    void disconnect() override;
    void enterStandby() override;
    void leaveStandby() override;

    // void registerInterface(const QString &name, QObject *interface);
    // void call(const QString &interface, const QString &method,
    // const QVariantList &args, std::function<void(QVariant)> callback = NULL);

 signals:
    // void requestReady(const QVariantMap& obj, const QString& url);
    void requestReadygetKodiAvailableTVChannelList(const QJsonDocument& object);
    void requestReadyKodiConnectionCheck(const QJsonDocument& object);
    void requestReadyTvheadendConnectionCheck(const QJsonDocument& object);
    void requestReadygetKodiChannelNumberToTVHeadendUUIDMapping(const QJsonDocument& object);
    void requestReadygetTVEPGfromTVHeadend(const QJsonDocument& doc);
    void requestReadygetSingleTVChannelList(const QJsonDocument& doc);
    void requestReadygetCompleteTVChannelList(const QJsonDocument& doc);
    // void requestReadyt(const QVariantMap& obj, const QString& url);
    void requestReadygetCurrentPlayer(const QJsonDocument& doc);
    void requestReadyCommandPlay(const QJsonDocument& doc);
    void requestReadyCommandPause(const QJsonDocument& doc);
    void requestReadyCommandNext(const QJsonDocument& doc);
    void requestReadyCommandPrevious(const QJsonDocument& doc);
    void requestReadyCommandUp(const QJsonDocument& doc);
    void requestReadyCommandDown(const QJsonDocument& doc);
    void requestReadyCommandLeft(const QJsonDocument& doc);
    void requestReadyCommandRight(const QJsonDocument& doc);
    void requestReadyCommandOk(const QJsonDocument& doc);
    void requestReadyCommandBack(const QJsonDocument& doc);
    void requestReadyCommandChannelUp(const QJsonDocument& doc);
    void requestReadyCommandChannelDown(const QJsonDocument& doc);

    // void requestReadyoiu(const QVariantMap& obj, const QString& url);
    // void requestReadyParser(const QJsonDocument& doc, const QString& url);
    // v/oid requestReadyParserz(const QJsonDocument& doc, const QString& url);

    // void requestReadyQstring(const QString& qstring, const QString& url);
    // void closed();

 private slots:
    void onPollingTimerTimeout();
    void onPollingEPGLoadTimerTimeout();
    void onProgressBarTimerTimeout();
    // void processMessage(QString message);
    // void onPollingTimer();
    // void onNetWorkAccessible(QNetworkAccessManager::NetworkAccessibility accessibility);
    void readTcpData();
    void clientDisconnected();
    void checkTCPSocket();

 private:
    QString fixUrl(QString url);
    bool    read(QMap<int, QString>* map);
    bool    write(QMap<int, QString> map);
    bool    read(QMap<QString, int>* map);
    bool    write(QMap<QString, int> map);
    void    kodiconnectioncheck(const QJsonDocument& object);
    void    Tvheadendconnectioncheck(const QJsonDocument& object);

 private:
    bool    m_flagTVHeadendConfigured = false;
    bool    m_flagKodiConfigured = false;
    int     m_currentkodiplayerid = -1;
    QString m_currentKodiMediaType = "notset";
    QString m_currentkodiplayertype = "unknown";
    bool    m_startup = true;
    QString m_entityId;
    bool    m_flage = false;
    int     m_timer;

    QTimer* m_Timer;
    QTimer* m_pollingTimer;
    QTimer* m_pollingEPGLoadTimer;
    QTimer* m_progressBarTimer;
    int     m_progressBarPosition = 0;

    // Kodi auth stuff
    QMap<int, QString>            m_mapKodiChannelNumberToTVHeadendUUID;
    QMap<QString, int>            m_mapTVHeadendUUIDToKodiChannelNumber;
    QList<QVariant>               m_KodiTVChannelList;
    KodiGetCurrentPlayerState     m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetActivePlayers;
    KodiGetCurrentPlayerState     m_KodiNextPlayerState = KodiGetCurrentPlayerState::GetActivePlayers;
    QString                       m_KodiCurrentPlayerThumbnail = "";
    QString                       m_KodiCurrentPlayerTitle = "";
    int                           m_globalKodiRequestID = 12345;
    int                           m_tvProgrammExpireTimeInHours = 2;
    int                           m_EPGExpirationTimestamp = 0;
    QList<QVariant>               m_currentEPG;
    QProcess                      m_checkProcessKodiAvailability;
    QProcess                      m_checkProcessTVHeadendAvailability;
    bool                          m_flagKodiOnline = false;
    bool                          m_flagTVHeadendOnline = false;
    QUrl                          m_kodiJSONRPCUrl;
    QUrl                          m_kodiEventServerUrl;
    QUrl                          m_tvheadendJSONUrl;
    QTcpSocket*                   m_tcpSocketKodiEventServer;
    bool                          m_flagKodiEventServerOnline = false;
    int                           m_currentEPGchannelToLoad = 0;
    QNetworkConfigurationManager* manager = new QNetworkConfigurationManager(this);
    QList<int> m_epgChannelList = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23};
    QNetworkAccessManager* networkManagerTvHeadend = new QNetworkAccessManager(this);
    QNetworkAccessManager* networkManagerKodi = new QNetworkAccessManager(this);
    QNetworkReply*         m_reply;
    QNetworkReply*         m_tvreply;
    int                    _tries = 0;
    // Kodi API calls
    /*void search(QString query);
    void search(QString query, QString type);
    void search(QString query, QString type, QString limit, QString offset);
    void getAlbum(QString id);*/
    void getSingleTVChannelList(QString id);
    void getCompleteTVChannelList();
    // Kodi Connect API calls
    void getCurrentPlayer();
    void getKodiAvailableTVChannelList();
    void getKodiChannelNumberToTVHeadendUUIDMapping();
    // void updateEntity(const QString& entity_id, const QVariantMap& attr);
    void getTVEPGfromTVHeadend(int KodiChannelNumber);
    void getTVChannelLogos();
    void clearMediaPlayerEntity();
    void showepg();
    void showepg(int channel);
    // get and post requests
    void tvheadendGetRequest(const QString& path, const QList<QPair<QString, QString> >& queryItems);

    // void postRequest(const QString& params, const int& id);
    void postRequest(const QString& jsonstring);
    // void postRequestthumb(const QString& url, const QString& method, const QString& jsonstring);
};
