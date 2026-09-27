#pragma once

#include "theme.h"
#include <QDialog>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QUrl>
#include <functional>
#include <memory>

class QLabel;
class QPushButton;
class QTemporaryDir;

struct UpdateRelease {
    QString version;
    QString architecture;
    QUrl url;
    QByteArray digest;
    qint64 size = 0;
};

class Updater : public QObject {
    Q_OBJECT
public:
    enum State { Idle, Checking, Available, Installing, Installed, Disabled };
    explicit Updater(QString executable, QObject *parent = nullptr, QNetworkAccessManager *transport = nullptr);
    ~Updater() override;
    void check();
    void install();
    State state() const { return m_state; }
    static QString repository();
    static QString architecture();
    static bool selectRelease(const QJsonObject &json, const QString &repo, const QString &arch,
                              UpdateRelease &release, QString &error);
    static bool validateBinary(const QByteArray &bytes, const UpdateRelease &release, QString &error);
    static bool replaceExecutable(const QString &path, const QByteArray &originalDigest,
                                  const QByteArray &bytes, QString &error);
    static QString installationProblem(const QString &path);
signals:
    void changed(Updater::State state, const QString &message);
private:
    void setState(State state, const QString &message);
    void fetchRelease();
    void fetch(const QUrl &url, qint64 maximum, std::function<void(QByteArray, QString)> done);
    void preflight(const QByteArray &bytes);
    QString m_executable;
    State m_state = Idle;
    UpdateRelease m_release;
    QNetworkAccessManager m_network;
    QNetworkAccessManager *m_transport;
    QPointer<QNetworkReply> m_reply;
    QByteArray m_originalDigest;
    std::unique_ptr<QTemporaryDir> m_stage;
};

class UpdateDialog : public QDialog {
    Q_OBJECT
public:
    UpdateDialog(const QString &executable, const Theme &theme, QWidget *parent = nullptr, QNetworkAccessManager *transport = nullptr);
    void setTheme(const Theme &theme);
signals:
    void restartRequested();
private:
    Updater m_updater;
    QLabel *m_status;
    QPushButton *m_action;
};
