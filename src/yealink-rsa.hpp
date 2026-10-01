#pragma once

#include <QString>

QString yealinkRsaEncrypt(
    const QString &plainText,
    const QString &rsaN,
    const QString &rsaE);
