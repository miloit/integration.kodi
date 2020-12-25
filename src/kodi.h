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
#include <QNetworkAccessManager>
#include <QUrl>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QVariantMap>
#include <QNetworkCookieJar>
#include <QByteArray>
#include <QDebug>
#include <QAuthenticator>
#include <QTimer>
#include <QObject>


#include "yio-interface/entities/mediaplayerinterface.h"
#include "yio-model/mediaplayer/tvchannelmodel_mediaplayer.h"
#include "yio-model/mediaplayer/searchmodel_mediaplayer.h"
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

public slots:  // NOLINT open issue: https://github.com/cpplint/cpplint/pull/99
    void connect() override;
    void disconnect() override;
    void enterStandby() override;
    void leaveStandby() override;


    void registerInterface(const QString &name, QObject *interface);
    void call(const QString &interface, const QString &method,
              const QVariantList &args, std::function<void(QVariant)> callback = NULL);


signals:
    void requestReady(const QVariantMap& obj, const QString& url);
    void requestReadyt(const QVariantMap& obj, const QString& url);
    void requestReadyz(const QVariantMap& obj, const QString& url);
    void requestReadyParser(const QJsonDocument& doc, const QString& url);
    void requestReadyParserz(const QJsonDocument& doc, const QString& url);
    void requestReadyQstring(const QString& qstring, const QString& url);
    void closed();

private slots:  // NOLINT open issue: https://github.com/cpplint/cpplint/pull/99
    void onPollingTimerTimeout();
    void onProgressBarTimerTimeout();
    void processMessage(QString message);
    void onPollingTimer();
    void onNetWorkAccessible(QNetworkAccessManager::NetworkAccessibility accessibility);

private:
    bool m_flagTVHeadendConfigured = false;
    bool m_flagKodiConfigured = false;
    int m_currentkodiplayerid = -1;
    QString m_currentKodiMediaType = "notset";
    QString m_currentkodiplayertype = "unknown";
    bool    m_startup = true;
    QString m_entityId;


    // polling tiQQueue m_sendQueue;mer
    QTimer* m_pollingTimer;
    QTimer* m_progressBarTimer;
    int m_progressBarPosition = 0;

    // Kodi auth stuff
    QMap <int, QString> m_mapKodiChannelNumberToTVHeadendUUID;
    QString m_KodiClientPassword;
    QString m_KodiClientUser;
    QString m_KodiClientUrl;
    QString m_KodiClientPort;
    QString m_TvheadendClientPassword;
    QString m_TvheadendClientUser;
    QString m_TvheadendClientUrl;
    QString m_TvheadendClientPort;
    QList<QVariant> m_KodiTVChannelList;
    KodiGetCurrentPlayerState m_KodiGetCurrentPlayerState = KodiGetCurrentPlayerState::GetActivePlayers;
    KodiGetCurrentPlayerState m_KodiNextPlayerState = KodiGetCurrentPlayerState::GetActivePlayers;
    QString m_KodiCurrentPlayerThumbnail = "";
    QString m_KodiCurrentPlayerTitle = "";
    int m_globalKodiRequestID = 12345;
    int m_tvProgrammExpireTimeInHours = 2;
    int m_EPGExpirationTimestamp = 0;
    QList<QVariant> m_currentEPG;


    // Kodi API calls
    void search(QString query);
    void search(QString query, QString type);
    void search(QString query, QString type, QString limit, QString offset);
    void getAlbum(QString id);
    void getSingleTVChannelList(QString id);
    void getCompleteTVChannelList();
    // Kodi Connect API calls
    void getCurrentPlayer();
    void getKodiAvailableTVChannelList();
    void getKodiChannelNumberToTVHeadendUUIDMapping();
    void updateEntity(const QString& entity_id, const QVariantMap& attr);
    void getTVEPGfromTVHeadend();
    void getTVChannelLogos();
    // get and post requests
    void getRequestWithAuthentication(const QString& url, const QString& method,
                                      const QString& user, const QString& password);
    void postRequest(const QString& url, const QString& params, const int& id);
    void postRequest(const QString& url, const QString& method, const QString& jsonstring);
};
