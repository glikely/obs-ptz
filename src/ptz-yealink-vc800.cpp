#include "ptz-yealink-vc800.hpp"
#include "yealink-rsa.hpp"

#include <curl/curl.h>
#include <obs-module.h>
#include <util/platform.h>

#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

#include <string>

static size_t WriteCallback(
    void *contents,
    size_t size,
    size_t nmemb,
    void *userp)
{
    std::string *response =
        static_cast<std::string *>(userp);

    response->append(
        static_cast<char *>(contents),
        size * nmemb);

    return size * nmemb;
}

static QString getYealinkCookiePath()
{
    char *path = obs_module_config_path("yealink.cookies");

    if (!path)
        return QString();

    QString cookiePath =
        QString::fromUtf8(path);

    bfree(path);

    return cookiePath;
}

PTZYealinkVC800::PTZYealinkVC800(OBSData config)
    : PTZDevice(config)
{
    type = "yealink-vc800";

    commandTimer.setSingleShot(true);
    commandTimer.setInterval(100);

    connect(
        &commandTimer,
        &QTimer::timeout,
        this,
        [this]() {
            do_update();
        });

    curl_global_init(CURL_GLOBAL_DEFAULT);

    getDefaults(config);
    update(config);
}

QString PTZYealinkVC800::description()
{
    return QString("Yealink VC800");
}

void PTZYealinkVC800::getDefaults(OBSData config) const
{
    PTZDevice::getDefaults(config);

     obs_data_set_default_string(
        config,
        "name",
        "VC800");

    obs_data_set_default_string(
        config,
        "host",
        "");

    obs_data_set_default_int(
        config,
        "port",
        443);

    obs_data_set_default_string(
        config,
        "username",
        "");

    obs_data_set_default_string(
        config,
        "password",
        "");
}

void PTZYealinkVC800::update(OBSData config)
{
    PTZDevice::update(config);

    host = obs_data_get_string(
        config,
        "host");

    username = obs_data_get_string(
        config,
        "username");

    password = obs_data_get_string(
        config,
        "password");

    port = obs_data_get_int(
        config,
        "port");
}

void PTZYealinkVC800::save(OBSData config) const
{
    PTZDevice::save(config);

    obs_data_set_string(
        config,
        "host",
        host.toUtf8().constData());

    obs_data_set_int(
        config,
        "port",
        port);

    obs_data_set_string(
        config,
        "username",
        username.toUtf8().constData());

    obs_data_set_string(
        config,
        "password",
        password.toUtf8().constData());
}

obs_properties_t *PTZYealinkVC800::get_obs_properties()
{
    auto *props =
        PTZDevice::get_obs_properties();
    auto *connection =
        obs_property_group_content(obs_properties_get(props, "interface"));

    obs_properties_add_text(
        connection,
        "host",
        "IP Address",
        OBS_TEXT_DEFAULT);

    obs_properties_add_int(
        connection,
        "port",
        "Port",
        0,
        65535,
        1);

    obs_properties_add_text(
        connection,
        "username",
        "Username",
        OBS_TEXT_DEFAULT);

    obs_properties_add_text(
        connection,
        "password",
        "Password",
        OBS_TEXT_PASSWORD);

    return props;
}

void PTZYealinkVC800::login()
{
    setConnected(false);
    if (sessionCurl) {
        curl_easy_cleanup(sessionCurl);
        sessionCurl = nullptr;
    }

    CURL *curl = curl_easy_init();

    if (!curl) {
        blog(LOG_ERROR, "VC800: curl_easy_init() failed");
        return;
    }

    QString baseUrl =
        QString("https://%1:%2").arg(host).arg(port);

    QString cookieFile =
        getYealinkCookiePath();

    if (cookieFile.isEmpty()) {
        blog(
            LOG_ERROR,
            "VC800: cookie path is empty");

        curl_easy_cleanup(curl);
        return;
    }

    curl_easy_setopt(
        curl,
        CURLOPT_COOKIEFILE,
        cookieFile.toUtf8().constData());

    curl_easy_setopt(
        curl,
        CURLOPT_COOKIEJAR,
        cookieFile.toUtf8().constData());

    curl_easy_setopt(
        curl,
        CURLOPT_SSL_VERIFYPEER,
        0L);

    curl_easy_setopt(
        curl,
        CURLOPT_SSL_VERIFYHOST,
        0L);

    curl_easy_setopt(
        curl,
        CURLOPT_CONNECTTIMEOUT,
        3L);

    curl_easy_setopt(
        curl,
        CURLOPT_TIMEOUT,
        5L);

    curl_easy_setopt(
        curl,
        CURLOPT_FOLLOWLOCATION,
        0L);

    //
    // ---------------------------------------------------------
    // 1. GET /api/common/info
    // ---------------------------------------------------------
    //

    QString infoUrl =
        baseUrl +
        "/api/common/info?p=Login&token=&t=" +
        QString::number(QDateTime::currentMSecsSinceEpoch());

    QByteArray response;

    curl_easy_setopt(
        curl,
        CURLOPT_URL,
        infoUrl.toUtf8().constData());

    curl_easy_setopt(
        curl,
        CURLOPT_HTTPGET,
        1L);

    curl_easy_setopt(
        curl,
        CURLOPT_POST,
        0L);

    curl_easy_setopt(
        curl,
        CURLOPT_POSTFIELDS,
        nullptr);

    struct curl_slist *infoHeaders = nullptr;

    infoHeaders = curl_slist_append(
        infoHeaders,
        "Accept: application/json");

    curl_easy_setopt(
        curl,
        CURLOPT_HTTPHEADER,
        infoHeaders);

    curl_easy_setopt(
        curl,
        CURLOPT_WRITEFUNCTION,
        +[](char *ptr, size_t size, size_t nmemb,
            void *userdata) -> size_t {
            QByteArray *data =
                static_cast<QByteArray *>(userdata);

            data->append(
                ptr,
                size * nmemb);

            return size * nmemb;
        });

    curl_easy_setopt(
        curl,
        CURLOPT_WRITEDATA,
        &response);

    CURLcode res =
        curl_easy_perform(curl);

    curl_slist_free_all(infoHeaders);

    curl_easy_setopt(
        curl,
        CURLOPT_HTTPHEADER,
        nullptr);

    if (res != CURLE_OK) {
        blog(
            LOG_ERROR,
            "VC800: common/info failed: %s",
            curl_easy_strerror(res));

        curl_easy_cleanup(curl);
        return;
    }

    QJsonParseError jsonError;

    QJsonDocument doc =
        QJsonDocument::fromJson(
            response,
            &jsonError);

    if (doc.isNull() || !doc.isObject()) {
        blog(
            LOG_ERROR,
            "VC800: invalid common/info JSON: %s",
            jsonError.errorString().toUtf8().constData());

        curl_easy_cleanup(curl);
        return;
    }

    QJsonObject root =
        doc.object();

    QJsonObject data =
        root.value("data").toObject();

    rsaN =
        data.value("rsaN").toString();

    rsaE =
        data.value("rsaE").toString();

    if (rsaN.isEmpty() || rsaE.isEmpty()) {
        blog(
            LOG_ERROR,
            "VC800: RSA public key not found");

        curl_easy_cleanup(curl);
        return;
    }

    //
    // ---------------------------------------------------------
    // 2. RSA encrypt password
    // ---------------------------------------------------------
    //

    QString encryptedPassword =
        yealinkRsaEncrypt(
            password,
            rsaN,
            rsaE);

    if (encryptedPassword.isEmpty()) {
        blog(
            LOG_ERROR,
            "VC800: RSA encryption failed");

        curl_easy_cleanup(curl);
        return;
    }

    //
    // ---------------------------------------------------------
    // 3. POST /api/auth/login
    // ---------------------------------------------------------
    //

    QString loginUrl =
        baseUrl +
        "/api/auth/login?p=Login&token=&t=" +
        QString::number(
            QDateTime::currentMSecsSinceEpoch());

    QByteArray loginBody =
        "username=" +
        QUrl::toPercentEncoding(username) +
        "&pwd=" +
        QUrl::toPercentEncoding(encryptedPassword);

    response.clear();

    curl_easy_setopt(
        curl,
        CURLOPT_URL,
        loginUrl.toUtf8().constData());

    curl_easy_setopt(
        curl,
        CURLOPT_POST,
        1L);

    curl_easy_setopt(
        curl,
        CURLOPT_POSTFIELDS,
        loginBody.constData());

    struct curl_slist *headers = nullptr;

    headers = curl_slist_append(
        headers,
        "Content-Type: application/x-www-form-urlencoded");

    curl_easy_setopt(
        curl,
        CURLOPT_HTTPHEADER,
        headers);

    curl_easy_setopt(
        curl,
        CURLOPT_WRITEDATA,
        &response);

    res =
        curl_easy_perform(curl);

    if (res != CURLE_OK) {
        blog(
            LOG_ERROR,
            "VC800: auth/login failed: %s",
            curl_easy_strerror(res));

        curl_slist_free_all(headers);

        curl_easy_setopt(
            curl,
            CURLOPT_HTTPHEADER,
            nullptr);

        curl_easy_cleanup(curl);
        return;
    }

    curl_slist_free_all(headers);

    curl_easy_setopt(
        curl,
        CURLOPT_HTTPHEADER,
        nullptr);

    QJsonDocument loginDoc =
        QJsonDocument::fromJson(
            response,
            &jsonError);

    if (!loginDoc.isObject()) {
        blog(
            LOG_ERROR,
            "VC800: invalid auth/login response");

        curl_easy_cleanup(curl);
        return;
    }

    QJsonObject loginRoot =
        loginDoc.object();

    if (loginRoot.value("ret").toString() != "ok") {
        blog(
            LOG_ERROR,
            "VC800: authentication rejected");

        curl_easy_cleanup(curl);
        return;
    }

    //
    // ---------------------------------------------------------
    // 4. POST /api/common/info
    //
    //    This is where VC800 returns:
    //    wui.common.token
    // ---------------------------------------------------------
    //

    QString tokenInfoUrl =
        baseUrl +
        "/api/common/info?p=Login&token=&t=" +
        QString::number(
            QDateTime::currentMSecsSinceEpoch());

    QByteArray tokenBody =
        "{\"idlist\":[\"wui\"]}";

    response.clear();

    curl_easy_setopt(
        curl,
        CURLOPT_URL,
        tokenInfoUrl.toUtf8().constData());

    curl_easy_setopt(
        curl,
        CURLOPT_POST,
        1L);

    curl_easy_setopt(
        curl,
        CURLOPT_POSTFIELDS,
        tokenBody.constData());

    headers = nullptr;

    headers = curl_slist_append(
        headers,
        "Content-Type: application/json");

    curl_easy_setopt(
        curl,
        CURLOPT_HTTPHEADER,
        headers);

    curl_easy_setopt(
        curl,
        CURLOPT_WRITEDATA,
        &response);

    res =
        curl_easy_perform(curl);

    if (res != CURLE_OK) {
        blog(
            LOG_ERROR,
            "VC800: token info failed: %s",
            curl_easy_strerror(res));

        curl_slist_free_all(headers);

        curl_easy_setopt(
            curl,
            CURLOPT_HTTPHEADER,
            nullptr);

        curl_easy_cleanup(curl);
        return;
    }

    curl_slist_free_all(headers);

    curl_easy_setopt(
        curl,
        CURLOPT_HTTPHEADER,
        nullptr);

    QJsonDocument tokenDoc =
        QJsonDocument::fromJson(
            response,
            &jsonError);

    if (!tokenDoc.isObject()) {
        blog(
            LOG_ERROR,
            "VC800: invalid token info response");

        curl_easy_cleanup(curl);
        return;
    }

    QJsonObject tokenRoot =
        tokenDoc.object();

    QJsonObject tokenData =
        tokenRoot.value("data").toObject();

    token =
        tokenData.value("wui.common.token").toString();

    if (token.isEmpty()) {
        blog(
            LOG_ERROR,
            "VC800: token not found in common/info");

        curl_easy_cleanup(curl);
        return;
    }

    //
    // ---------------------------------------------------------
    // 5. Login complete
    // ---------------------------------------------------------
    //

    loggedIn = true;
    sessionCurl = curl;
    setConnected(true);
}

void PTZYealinkVC800::set(calldata_t *cd)
{
    PTZDevice::set(cd);
}

void PTZYealinkVC800::standbyToggle()
{
    // Standby/power control intentionally disabled for VC800.
}

void PTZYealinkVC800::getCameraLayout()
{
    if (!loggedIn) {
        blog(
            LOG_WARNING,
            "VC800: GET CAMERA LAYOUT aborted - not logged in");
        return;
    }

    if (token.isEmpty()) {
        blog(
            LOG_WARNING,
            "VC800: GET CAMERA LAYOUT aborted - token empty");
        return;
    }

    CURL *curl =
        sessionCurl;

    if (!curl) {
        blog(
            LOG_WARNING,
            "VC800: GET CAMERA LAYOUT session curl is null");
        return;
    }

    std::string response;

    QString url =
        QString(
            "https://%1:%2/api/camera/layout"
            "?p=Home&token=%3&t=%4")
            .arg(host)
            .arg(port)
            .arg(QUrl::toPercentEncoding(token))
            .arg(QDateTime::currentMSecsSinceEpoch());

    std::string urlStd =
        url.toStdString();

    QString cookiePath =
        getYealinkCookiePath();

    if (cookiePath.isEmpty()) {
        blog(
            LOG_WARNING,
            "VC800: GET CAMERA LAYOUT cookie path empty");

        return;
    }

    std::string cookiePathStd =
        cookiePath.toStdString();

    curl_easy_setopt(
        curl,
        CURLOPT_URL,
        urlStd.c_str());

    curl_easy_setopt(
        curl,
        CURLOPT_SSL_VERIFYPEER,
        0L);

    curl_easy_setopt(
        curl,
        CURLOPT_SSL_VERIFYHOST,
        0L);

    curl_easy_setopt(
        curl,
        CURLOPT_COOKIEFILE,
        cookiePathStd.c_str());

    curl_easy_setopt(
        curl,
        CURLOPT_HTTPGET,
        1L);

    curl_easy_setopt(
        curl,
        CURLOPT_WRITEFUNCTION,
        WriteCallback);

    curl_easy_setopt(
        curl,
        CURLOPT_WRITEDATA,
        &response);

    curl_easy_setopt(
        curl,
        CURLOPT_CONNECTTIMEOUT,
        3L);

    curl_easy_setopt(
        curl,
        CURLOPT_TIMEOUT,
        5L);

    CURLcode res =
        curl_easy_perform(curl);

    long httpCode = 0;

    curl_easy_getinfo(
        curl,
        CURLINFO_RESPONSE_CODE,
        &httpCode);

    if (res != CURLE_OK) {
        blog(
            LOG_WARNING,
            "VC800: camera/layout curl error: %s",
            curl_easy_strerror(res));

    } else if (httpCode != 200) {
        blog(
            LOG_WARNING,
            "VC800: camera/layout failed (HTTP %ld)",
            httpCode);
    }
}

void PTZYealinkVC800::sendKey(int key)
{
    /*
     * O VC800 possui duas tabelas diferentes:
     *
     * keypad:
     *   POWER/I-O   = 28
     *   DPAD_LEFT   = 17
     *   DPAD_RIGHT  = 18
     *
     * câmera:
     *   LEFT  = 4
     *   RIGHT = 6
     *   UP    = 8
     *   DOWN  = 2
     *   IN    = 1
     *   OUT   = 0
     *
     * Para PTZ usamos /api/device/adjustcamera.
     * Para POWER/I-O usamos /api/device/keypad.
     */

    int action = -1;

    switch (key) {

    case 17:
        action = 4;
        break;

    case 18:
        action = 6;
        break;

    case 15:
        action = 8;
        break;

    case 16:
        action = 2;
        break;

    case 23:
        action = 1;
        break;

    case 24:
        action = 0;
        break;

    case 28:
        /*
         * POWER/I-O.
         *
         * Na interface web da VC800:
         *
         * POWER = 28
         *
         * O comando é enviado para:
         *
         * POST /api/device/keypad
         * key=28
         */
        break;

    default:
        blog(
            LOG_WARNING,
            "VC800: unsupported camera key=%d",
            key);

        return;
    }

    /*
     * Duas tentativas no máximo:
     *
     * tentativa 0 = sessão atual
     * tentativa 1 = após renovar a sessão
     */
    for (int attempt = 0; attempt < 2; ++attempt) {

        if (!loggedIn) {

            login();

            if (!loggedIn)
                return;
        }

        if (token.isEmpty()) {

            blog(
                LOG_WARNING,
                "VC800: camera command token is empty");

            loggedIn = false;
            return;
        }

        /*
         * Pegamos o sessionCurl DEPOIS do login.
         *
         * login() pode destruir o handle anterior e criar outro.
         */
        CURL *curl =
            sessionCurl;

        if (!curl) {

            blog(
                LOG_WARNING,
                "VC800: camera command session curl is null");

            loggedIn = false;
            return;
        }

        std::string response;

        QString url;
        QString body;

        if (key == 28) {

            /*
             * Comando POWER/I-O da VC800.
             *
             * A interface web envia:
             *
             * POST /api/device/keypad
             * key=28
             */
            url =
                QString(
                    "https://%1:%2/api/device/keypad")
                    .arg(host)
                    .arg(port);

            body =
                "key=28";

        } else {

            /*
             * Comandos direcionais e zoom.
             */
            url =
                QString(
                    "https://%1:%2/api/device/adjustcamera"
                    "?p=Home&token=%3&t=%4")
                    .arg(host)
                    .arg(port)
                    .arg(QUrl::toPercentEncoding(token))
                    .arg(QDateTime::currentMSecsSinceEpoch());

            body =
                QString(
                    "cameraid=0&action=%1")
                    .arg(action);
        }

        std::string urlStd =
            url.toStdString();

        std::string bodyStd =
            body.toStdString();

        curl_easy_setopt(
            curl,
            CURLOPT_URL,
            urlStd.c_str());

        curl_easy_setopt(
            curl,
            CURLOPT_SSL_VERIFYPEER,
            0L);

        curl_easy_setopt(
            curl,
            CURLOPT_SSL_VERIFYHOST,
            0L);

        QString cookiePath =
            getYealinkCookiePath();

        if (cookiePath.isEmpty()) {

            blog(
                LOG_WARNING,
                "VC800: camera command cookie path empty");

            loggedIn = false;
            return;
        }

        std::string cookiePathStd =
            cookiePath.toStdString();

        curl_easy_setopt(
            curl,
            CURLOPT_COOKIEFILE,
            cookiePathStd.c_str());

        QString referer =
            QString(
                "https://%1:%2/servlet?"
                "p=home&q=load")
                .arg(host)
                .arg(port);

        std::string refererStd =
            referer.toStdString();

        curl_easy_setopt(
            curl,
            CURLOPT_REFERER,
            refererStd.c_str());

        curl_easy_setopt(
            curl,
            CURLOPT_FOLLOWLOCATION,
            0L);

        /*
         * POWER/I-O e PTZ são enviados como POST.
         */
        curl_easy_setopt(
            curl,
            CURLOPT_HTTPGET,
            0L);

        curl_easy_setopt(
            curl,
            CURLOPT_POST,
            1L);

        curl_easy_setopt(
            curl,
            CURLOPT_POSTFIELDS,
            bodyStd.c_str());

        struct curl_slist *headers = nullptr;

        headers = curl_slist_append(
            headers,
            "Content-Type: application/x-www-form-urlencoded");

        curl_easy_setopt(
            curl,
            CURLOPT_HTTPHEADER,
            headers);

        curl_easy_setopt(
            curl,
            CURLOPT_WRITEFUNCTION,
            WriteCallback);

        curl_easy_setopt(
            curl,
            CURLOPT_WRITEDATA,
            &response);

        curl_easy_setopt(
            curl,
            CURLOPT_CONNECTTIMEOUT,
            3L);

        curl_easy_setopt(
            curl,
            CURLOPT_TIMEOUT,
            5L);

        CURLcode res =
            curl_easy_perform(curl);

        long httpCode = 0;

        curl_easy_getinfo(
            curl,
            CURLINFO_RESPONSE_CODE,
            &httpCode);

        /*
         * A VC800 pode responder HTTP 200 mesmo com a sessão
         * expirada. Nesse caso o corpo contém:
         *
         * {"ret":"ok","data":{"webStatus":"403"}}
         */
        bool sessionExpired =
            httpCode == 403 ||
            response.find("\"webStatus\":\"403\"") !=
                std::string::npos;

        curl_easy_setopt(
            curl,
            CURLOPT_HTTPHEADER,
            nullptr);

        curl_slist_free_all(headers);

        if (res != CURLE_OK) {

            blog(
                LOG_WARNING,
                "VC800: camera command failed: %s",
                curl_easy_strerror(res));

            return;
        }

        if (sessionExpired) {

            if (attempt == 0) {

                loggedIn = false;

                login();

                if (!loggedIn) {

                    blog(
                        LOG_WARNING,
                        "VC800: session renewal failed");

                    return;
                }

                continue;
            }

            blog(
                LOG_WARNING,
                "VC800: camera command failed after session renewal");

            return;
        }

        /*
         * Comando aceito pela câmera.
         */
        if (httpCode == 200 &&
            (
                response.find("\"ret\":\"ok\"") !=
                    std::string::npos ||
                response.find("ok") !=
                    std::string::npos ||
                response.find("complated") !=
                    std::string::npos
            )) {

            return;
        }

        blog(
            LOG_WARNING,
            "VC800: camera command failed (HTTP %ld)",
            httpCode);

        return;
    }
}

void PTZYealinkVC800::sendPreset(
    int preset,
    const QString &action)
{
    for (int attempt = 0; attempt < 2; ++attempt) {

        if (!loggedIn) {
            login();

            if (!loggedIn)
                return;
        }

        if (token.isEmpty()) {
            blog(
                LOG_WARNING,
                "VC800: preset save token is empty");
            return;
        }

        CURL *curl =
            sessionCurl;

        if (!curl) {
            blog(
                LOG_WARNING,
                "VC800: preset save session curl is null");
            return;
        }

        std::string response;

        QString url =
            QString(
                "https://%1:%2/api/device/camerapreset"
                "?p=Home&token=%3&t=%4")
                .arg(host)
                .arg(port)
                .arg(QUrl::toPercentEncoding(token))
                .arg(QDateTime::currentMSecsSinceEpoch());

        QString body =
            QString(
                "cameraid=0&action=%1&prenumber=%2")
                .arg(action)
                .arg(preset);

        std::string urlStd =
            url.toStdString();

        std::string bodyStd =
            body.toStdString();

        curl_easy_setopt(
            curl,
            CURLOPT_URL,
            urlStd.c_str());

        curl_easy_setopt(
            curl,
            CURLOPT_SSL_VERIFYPEER,
            0L);

        curl_easy_setopt(
            curl,
            CURLOPT_SSL_VERIFYHOST,
            0L);

        curl_easy_setopt(
            curl,
            CURLOPT_POST,
            1L);

        curl_easy_setopt(
            curl,
            CURLOPT_POSTFIELDS,
            bodyStd.c_str());

        struct curl_slist *headers = nullptr;

        headers = curl_slist_append(
            headers,
            "Content-Type: application/x-www-form-urlencoded");

        curl_easy_setopt(
            curl,
            CURLOPT_HTTPHEADER,
            headers);

        curl_easy_setopt(
            curl,
            CURLOPT_WRITEFUNCTION,
            WriteCallback);

        curl_easy_setopt(
            curl,
            CURLOPT_WRITEDATA,
            &response);

        curl_easy_setopt(
            curl,
            CURLOPT_CONNECTTIMEOUT,
            3L);

        curl_easy_setopt(
            curl,
            CURLOPT_TIMEOUT,
            5L);

        CURLcode res =
            curl_easy_perform(curl);

        long httpCode = 0;

        curl_easy_getinfo(
            curl,
            CURLINFO_RESPONSE_CODE,
            &httpCode);

        bool sessionExpired =
            httpCode == 403 ||
            response.find("\"webStatus\":\"403\"") !=
                std::string::npos;

        curl_easy_setopt(
            curl,
            CURLOPT_HTTPHEADER,
            nullptr);

        curl_slist_free_all(headers);

        if (res != CURLE_OK) {

            blog(
                LOG_WARNING,
                "VC800: preset save curl error: %s",
                curl_easy_strerror(res));

            return;
        }

        if (sessionExpired) {

            if (attempt == 0) {
                loggedIn = false;
                login();

                if (!loggedIn) {

                    blog(
                        LOG_WARNING,
                        "VC800: session renewal failed");

                    return;
                }

                continue;
            }

            blog(
                LOG_WARNING,
                "VC800: preset %s failed after session renewal",
                action.toUtf8().constData());

            return;
        }

        if (httpCode == 200 &&
            response.find("\"ret\":\"ok\"") !=
                std::string::npos) {

            return;
        }

        blog(
            LOG_WARNING,
            "VC800: preset %s failed (HTTP %ld)",
            action.toUtf8().constData(),
            httpCode);

        return;
    }
}

void PTZYealinkVC800::recallPreset(int preset)
{
    for (int attempt = 0; attempt < 2; ++attempt) {

        if (!loggedIn) {
            login();

            if (!loggedIn)
                return;
        }

        if (token.isEmpty()) {
            blog(
                LOG_WARNING,
                "VC800: preset recall token is empty");
            return;
        }

        CURL *curl =
            sessionCurl;

        if (!curl) {
            blog(
                LOG_WARNING,
                "VC800: preset recall session curl is null");
            return;
        }

        std::string response;

        QString url =
            QString(
                "https://%1:%2/api/device/camerapreset"
                "?p=Home&token=%3&t=%4")
                .arg(host)
                .arg(port)
                .arg(QUrl::toPercentEncoding(token))
                .arg(QDateTime::currentMSecsSinceEpoch());

        QString body =
            QString(
                "cameraid=0&action=apply&prenumber=%1")
                .arg(preset);

        std::string urlStd =
            url.toStdString();

        std::string bodyStd =
            body.toStdString();

        curl_easy_setopt(
            curl,
            CURLOPT_URL,
            urlStd.c_str());

        curl_easy_setopt(
            curl,
            CURLOPT_SSL_VERIFYPEER,
            0L);

        curl_easy_setopt(
            curl,
            CURLOPT_SSL_VERIFYHOST,
            0L);

        curl_easy_setopt(
            curl,
            CURLOPT_POST,
            1L);

        curl_easy_setopt(
            curl,
            CURLOPT_POSTFIELDS,
            bodyStd.c_str());

        struct curl_slist *headers = nullptr;

        headers = curl_slist_append(
            headers,
            "Content-Type: application/x-www-form-urlencoded");

        headers = curl_slist_append(
            headers,
            "Accept: application/json, text/plain, */*");

        headers = curl_slist_append(
            headers,
            QString(
                "Origin: https://%1:%2")
                .arg(host)
                .arg(port)
                .toUtf8()
                .constData());

        headers = curl_slist_append(
            headers,
            QString(
                "Referer: https://%1:%2/api")
                .arg(host)
                .arg(port)
                .toUtf8()
                .constData());

        curl_easy_setopt(
            curl,
            CURLOPT_HTTPHEADER,
            headers);

        curl_easy_setopt(
            curl,
            CURLOPT_WRITEFUNCTION,
            WriteCallback);

        curl_easy_setopt(
            curl,
            CURLOPT_WRITEDATA,
            &response);

        curl_easy_setopt(
            curl,
            CURLOPT_CONNECTTIMEOUT,
            3L);

        curl_easy_setopt(
            curl,
            CURLOPT_TIMEOUT,
            5L);

        CURLcode res =
            curl_easy_perform(curl);

        long httpCode = 0;

        curl_easy_getinfo(
            curl,
            CURLINFO_RESPONSE_CODE,
            &httpCode);

        bool sessionExpired =
            httpCode == 403 ||
            response.find("\"webStatus\":\"403\"") !=
                std::string::npos;

        curl_easy_setopt(
            curl,
            CURLOPT_HTTPHEADER,
            nullptr);

        curl_slist_free_all(headers);

        if (res != CURLE_OK) {

            blog(
                LOG_WARNING,
                "VC800: preset recall curl error: %s",
                curl_easy_strerror(res));

            return;
        }

        if (sessionExpired) {

            if (attempt == 0) {

                loggedIn = false;

                login();

                if (!loggedIn) {

                    blog(
                        LOG_WARNING,
                        "VC800: session renewal failed");

                    return;
                }

                continue;
            }

            blog(
                LOG_WARNING,
                "VC800: preset recall failed after session renewal");

            return;
        }

        if (httpCode == 200 &&
            response.find("\"ret\":\"ok\"") !=
                std::string::npos) {

            return;
        }

        blog(
            LOG_WARNING,
            "VC800: preset recall failed (HTTP %ld)",
            httpCode);

        return;
    }
}

void PTZYealinkVC800::do_update()
{
    bool moving = false;

    if (pan_speed < 0)
    {
        sendKey(17);
        moving = true;
    }
    else if (pan_speed > 0)
    {
        sendKey(18);
        moving = true;
    }

    if (tilt_speed > 0)
    {
        sendKey(15);
        moving = true;
    }
    else if (tilt_speed < 0)
    {
        sendKey(16);
        moving = true;
    }

    if (zoom_speed > 0)
    {
        sendKey(23);
        moving = true;
    }
    else if (zoom_speed < 0)
    {
        sendKey(24);
        moving = true;
    }

    if (moving)
        commandTimer.start();
    else
        commandTimer.stop();
}

void PTZYealinkVC800::pantilt_home()
{
    sendKey(25);
}

void PTZYealinkVC800::memory_set(int preset)
{
    if (preset < 0 || preset > 15)
        return;

    QString name =
        presetName(preset);

    QString action;

    if (name.isEmpty())
        action = "add";
    else
        action = "replace";

    sendPreset(
        preset,
        action);
}

void PTZYealinkVC800::memory_recall(int preset)
{
    if (preset < 0 || preset > 15)
        return;

    recallPreset(preset);
}

void PTZYealinkVC800::memory_reset(int preset)
{
    if (preset < 0 || preset > 15)
        return;

    for (int attempt = 0; attempt < 2; ++attempt) {

        if (!loggedIn) {
            login();

            if (!loggedIn)
                return;
        }

        if (token.isEmpty()) {
            blog(
                LOG_WARNING,
                "VC800: preset delete token is empty");
            return;
        }

        CURL *curl =
            sessionCurl;

        if (!curl) {
            blog(
                LOG_WARNING,
                "VC800: preset delete session curl is null");
            return;
        }

        std::string response;

        QString url =
            QString(
                "https://%1:%2/api/device/camerapreset"
                "?p=Home&token=%3&t=%4")
                .arg(host)
                .arg(port)
                .arg(QUrl::toPercentEncoding(token))
                .arg(QDateTime::currentMSecsSinceEpoch());

        QString body =
            QString(
                "cameraid=0&action=delete&prenumber=%1")
                .arg(preset);

        std::string urlStd =
            url.toStdString();

        std::string bodyStd =
            body.toStdString();

        curl_easy_setopt(
            curl,
            CURLOPT_URL,
            urlStd.c_str());

        curl_easy_setopt(
            curl,
            CURLOPT_SSL_VERIFYPEER,
            0L);

        curl_easy_setopt(
            curl,
            CURLOPT_SSL_VERIFYHOST,
            0L);

        curl_easy_setopt(
            curl,
            CURLOPT_POST,
            1L);

        curl_easy_setopt(
            curl,
            CURLOPT_POSTFIELDS,
            bodyStd.c_str());

        struct curl_slist *headers = nullptr;

        headers = curl_slist_append(
            headers,
            "Content-Type: application/x-www-form-urlencoded");

        curl_easy_setopt(
            curl,
            CURLOPT_HTTPHEADER,
            headers);

        curl_easy_setopt(
            curl,
            CURLOPT_WRITEFUNCTION,
            WriteCallback);

        curl_easy_setopt(
            curl,
            CURLOPT_WRITEDATA,
            &response);

        curl_easy_setopt(
            curl,
            CURLOPT_CONNECTTIMEOUT,
            3L);

        curl_easy_setopt(
            curl,
            CURLOPT_TIMEOUT,
            5L);

        CURLcode res =
            curl_easy_perform(curl);

        long httpCode = 0;

        curl_easy_getinfo(
            curl,
            CURLINFO_RESPONSE_CODE,
            &httpCode);

        bool sessionExpired =
            httpCode == 403 ||
            response.find("\"webStatus\":\"403\"") !=
                std::string::npos;

        curl_easy_setopt(
            curl,
            CURLOPT_HTTPHEADER,
            nullptr);

        curl_slist_free_all(headers);

        if (res != CURLE_OK) {

            blog(
                LOG_WARNING,
                "VC800: preset delete curl error: %s",
                curl_easy_strerror(res));

            return;
        }

        if (sessionExpired) {

            if (attempt == 0) {

                loggedIn = false;

                login();

                if (!loggedIn) {

                    blog(
                        LOG_WARNING,
                        "VC800: session renewal failed");

                    return;
                }

                continue;
            }

            blog(
                LOG_WARNING,
                "VC800: preset delete failed after session renewal");

            return;
        }

        if (httpCode == 200 &&
            response.find("\"ret\":\"ok\"") !=
                std::string::npos) {

            return;
        }

        blog(
            LOG_WARNING,
            "VC800: preset delete failed (HTTP %ld)",
            httpCode);

        return;
    }
}