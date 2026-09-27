#pragma once

#include "theme.h"
#include <QDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QUrl>
#include <functional>

class QLabel;
class QComboBox;
class QLineEdit;
class QPushButton;

class SyncthingClient : public QObject {
    Q_OBJECT
public:
    using Callback = std::function<void(const QJsonDocument &, const QString &)>;
    explicit SyncthingClient(QObject *parent = nullptr);
    void discover(std::function<void(const QString &)> done);
    void request(const QByteArray &method, const QString &path, const QJsonObject &body, Callback done);
    static QString normalizeDeviceId(QString id);
    static bool prepareFolder(const QJsonArray &folders, const QJsonObject &defaults,
                              const QString &directory, const QString &local, const QString &peer,
                              QJsonObject &folder, QString &error);
    static constexpr const char *folderId = "omadraft-drafts-v1";
private:
    QNetworkAccessManager m_network;
    QUrl m_url;
    QByteArray m_key;
};

class SyncDialog : public QDialog {
    Q_OBJECT
public:
    SyncDialog(QString directory, const Theme &theme, QWidget *parent = nullptr);
    void setTheme(const Theme &theme);
private:
    void load();
    void share();
    void busy(bool value);
    void fail(const QString &message);
    SyncthingClient m_client;
    QString m_directory;
    QString m_localId;
    QJsonArray m_devices;
    QLabel *m_status;
    QLineEdit *m_ownId;
    QComboBox *m_peers;
    QLineEdit *m_deviceId;
    QPushButton *m_enable;
    QPushButton *m_refresh;
    QPushButton *m_start;
};
