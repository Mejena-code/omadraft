#pragma once

#include <QByteArray>
#include <QString>
#include <QMap>
#include <QVector>

struct Note {
    QString id;
    QString text;
    int cursor = 0;
    int anchor = 0;
    int scroll = 0;
    static Note blank();
};

struct Session {
    QVector<Note> notes;
    int active = 0;
    QByteArray geometry;
};

class SessionStore {
public:
    explicit SessionStore(QString directory);
    bool load(Session &session, QString &error, QString &notice);
    bool save(Session &session, QString &error);
    bool synchronize(Session &session, QString &error);
    QString draftsDirectory() const { return m_directory + "/drafts"; }
    QString directory() const { return m_directory; }
    static QByteArray encode(const Session &session);
    static bool decode(const QByteArray &bytes, Session &session, QString &error);

private:
    bool initializeDrafts(Session &session, QString &error);
    bool saveSnapshot(const Session &session, QString &error);
    QMap<QString, QString> m_baseline;
    bool writeAtomic(const QString &path, const QByteArray &bytes, QString &error);
    QString m_directory;
    QByteArray m_lastGood;
    bool m_canSave = false;
    bool m_recovered = false;
};
