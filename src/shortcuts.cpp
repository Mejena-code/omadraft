#include "shortcuts.h"
#include <QFile>

QString altKeyLabel(const QString &systemRoot) {
    for (const auto &path : {"/sys/firmware/devicetree/base/model", "/sys/class/dmi/id/product_name"}) {
        QFile file(systemRoot + QString::fromLatin1(path));
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QByteArray model = file.read(256).trimmed();
        if (model.startsWith("Apple MacBook") || model.startsWith("MacBook"))
            return QStringLiteral("⌥");
    }
    return QStringLiteral("Alt");
}
