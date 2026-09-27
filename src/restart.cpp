#include "restart.h"

#include <QByteArray>
#include <QLockFile>
#include <QLocalServer>
#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <vector>

void replaceProcess(const QString &executable, const QStringList &arguments,
                    QLockFile &lock, QLocalServer &server, QString &error) {
    std::vector<QByteArray> encoded{executable.toLocal8Bit()};
    for (const QString &argument : arguments) encoded.push_back(argument.toLocal8Bit());
    std::vector<char *> command;
    for (auto &argument : encoded) command.push_back(argument.data());
    command.push_back(nullptr);
    const QString serverName = server.serverName();
    server.close();
    lock.unlock();
    ::execv(encoded[0].constData(), command.data());
    error = QString::fromLocal8Bit(std::strerror(errno));
    if (!lock.tryLock()) return;
    QLocalServer::removeServer(serverName);
    if (!server.listen(serverName)) error += "\n" + server.errorString();
}
