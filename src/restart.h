#pragma once

#include <QString>
#include <QStringList>
class QLockFile;
class QLocalServer;

// Returns only on failure. Check lock.isLocked() before continuing to use notes.
void replaceProcess(const QString &executable, const QStringList &arguments,
                    QLockFile &lock, QLocalServer &server, QString &error);
