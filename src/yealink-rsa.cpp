#include "yealink-rsa.hpp"

#include <QByteArray>
#include <QDebug>
#include <QRandomGenerator>
#include <cstring>

#include <mbedtls/bignum.h>
#include <mbedtls/rsa.h>

static int yealinkRng(
    void *p_rng,
    unsigned char *output,
    size_t output_len)
{
    Q_UNUSED(p_rng);

    QRandomGenerator *generator = QRandomGenerator::system();

    size_t offset = 0;

    while (offset < output_len) {
        quint32 value = generator->generate();

        const size_t remaining = output_len - offset;
        const size_t copySize =
            remaining < sizeof(value) ? remaining : sizeof(value);

        memcpy(
            output + offset,
            &value,
            copySize);

        offset += copySize;
    }

    return 0;
}

QString yealinkRsaEncrypt(
    const QString &plainText,
    const QString &rsaN,
    const QString &rsaE)
{
    if (plainText.isEmpty()) {
        qWarning()
            << "[obs-ptz] Yealink RSA: empty plaintext";
        return QString();
    }

    if (rsaN.isEmpty() || rsaE.isEmpty()) {
        qWarning()
            << "[obs-ptz] Yealink RSA: missing public key";
        return QString();
    }

    mbedtls_mpi n;
    mbedtls_mpi e;
    mbedtls_rsa_context rsa;

    mbedtls_mpi_init(&n);
    mbedtls_mpi_init(&e);
    mbedtls_rsa_init(&rsa);

    int ret = 0;

    ret = mbedtls_mpi_read_string(
        &n,
        16,
        rsaN.toLatin1().constData());

    if (ret != 0) {
        qWarning()
            << "[obs-ptz] Yealink RSA: invalid modulus:"
            << ret;

        mbedtls_rsa_free(&rsa);
        mbedtls_mpi_free(&n);
        mbedtls_mpi_free(&e);

        return QString();
    }

    ret = mbedtls_mpi_read_string(
        &e,
        16,
        rsaE.toLatin1().constData());

    if (ret != 0) {
        qWarning()
            << "[obs-ptz] Yealink RSA: invalid exponent:"
            << ret;

        mbedtls_rsa_free(&rsa);
        mbedtls_mpi_free(&n);
        mbedtls_mpi_free(&e);

        return QString();
    }

    ret = mbedtls_rsa_import(
        &rsa,
        &n,
        nullptr,
        nullptr,
        nullptr,
        &e);

    if (ret != 0) {
        qWarning()
            << "[obs-ptz] Yealink RSA: RSA import failed:"
            << ret;

        mbedtls_rsa_free(&rsa);
        mbedtls_mpi_free(&n);
        mbedtls_mpi_free(&e);

        return QString();
    }

    ret = mbedtls_rsa_complete(&rsa);

    if (ret != 0) {
        qWarning()
            << "[obs-ptz] Yealink RSA: RSA complete failed:"
            << ret;

        mbedtls_rsa_free(&rsa);
        mbedtls_mpi_free(&n);
        mbedtls_mpi_free(&e);

        return QString();
    }

    ret = mbedtls_rsa_set_padding(
        &rsa,
        MBEDTLS_RSA_PKCS_V15,
        MBEDTLS_MD_NONE);

    if (ret != 0) {
        qWarning()
            << "[obs-ptz] Yealink RSA: failed to configure padding:"
            << ret;

        mbedtls_rsa_free(&rsa);
        mbedtls_mpi_free(&n);
        mbedtls_mpi_free(&e);

        return QString();
    }

    QByteArray plaintext = plainText.toUtf8();

    const size_t rsaSize =
        mbedtls_rsa_get_len(&rsa);

    QByteArray encrypted(
        static_cast<int>(rsaSize),
        '\0');

    ret = mbedtls_rsa_pkcs1_encrypt(
        &rsa,
        yealinkRng,
        nullptr,
        static_cast<size_t>(plaintext.size()),
        reinterpret_cast<const unsigned char *>(
            plaintext.constData()),
        reinterpret_cast<unsigned char *>(
            encrypted.data()));

    if (ret != 0) {
        qWarning()
            << "[obs-ptz] Yealink RSA: encryption failed:"
            << ret;

        mbedtls_rsa_free(&rsa);
        mbedtls_mpi_free(&n);
        mbedtls_mpi_free(&e);

        return QString();
    }

    const QByteArray hex =
        encrypted.toHex();

    QString result =
        QStringLiteral("__WUI_ENC__:") +
        QString::fromLatin1(hex);

    qDebug()
        << "[obs-ptz] Yealink RSA: encryption successful";

    qDebug()
        << "[obs-ptz] Yealink RSA: ciphertext length:"
        << hex.size();

    mbedtls_rsa_free(&rsa);
    mbedtls_mpi_free(&n);
    mbedtls_mpi_free(&e);

    return result;
}
