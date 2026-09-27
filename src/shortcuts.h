#pragma once

#include <QString>

// A system root can be supplied when checking captured hardware metadata in tests.
QString altKeyLabel(const QString &systemRoot = QString());
