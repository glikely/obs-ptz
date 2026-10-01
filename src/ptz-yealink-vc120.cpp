/* Yealink VC120 Driver for OBS-PTZ
 *
 * Copyright 2026
 */

#include "ptz-yealink-vc120.hpp"

#include <curl/curl.h>
#include <obs-module.h>
#include <util/platform.h>

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

PTZYealinkVC120::PTZYealinkVC120(OBSData config)
    : PTZDevice(config)
{
    type = "yealink-vc120";

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

QString PTZYealinkVC120::description()
{
    return QString("Yealink VC120");
}

void PTZYealinkVC120::getDefaults(OBSData config) const
{
    PTZDevice::getDefaults(config);

    obs_data_set_default_string(
        config,
        "name",
        "VC120");

    obs_data_set_default_string(
        config,
        "host",
        "");

    obs_data_set_default_int(
        config,
        "port",
        0);

    obs_data_set_default_string(
        config,
        "username",
        "");

    obs_data_set_default_string(
        config,
        "password",
        "");
}

void PTZYealinkVC120::update(OBSData config)
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

void PTZYealinkVC120::save(OBSData config) const
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

obs_properties_t *PTZYealinkVC120::get_obs_properties()
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

void PTZYealinkVC120::login()
{
    CURL *curl = curl_easy_init();

    if (!curl)
        return;

    std::string response;

    QString url =
        QString(
            "https://%1:%2/servlet?"
            "p=login&q=login")
            .arg(host)
            .arg(port);

    std::string urlStd =
        url.toStdString();

    std::string post =
        "username=" +
        username.toStdString() +
        "&pwd=" +
        password.toStdString();

    QString cookiePath =
        getYealinkCookiePath();

    if (cookiePath.isEmpty()) {
        blog(
            LOG_WARNING,
            "Yealink: unable to determine cookie path");

        curl_easy_cleanup(curl);
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
        CURLOPT_POST,
        1L);

    curl_easy_setopt(
        curl,
        CURLOPT_POSTFIELDS,
        post.c_str());

    curl_easy_setopt(
        curl,
        CURLOPT_SSL_VERIFYPEER,
        0L);

    curl_easy_setopt(
        curl,
        CURLOPT_SSL_VERIFYHOST,
        0L);

    /*
     * Ativa o mecanismo de cookies do libcurl.
     */
    curl_easy_setopt(
        curl,
        CURLOPT_COOKIEFILE,
        "");

    curl_easy_setopt(
        curl,
        CURLOPT_COOKIEJAR,
        cookiePathStd.c_str());

    curl_easy_setopt(
        curl,
        CURLOPT_COOKIESESSION,
        1L);

    curl_easy_setopt(
        curl,
        CURLOPT_WRITEFUNCTION,
        WriteCallback);

    curl_easy_setopt(
        curl,
        CURLOPT_WRITEDATA,
        &response);

    CURLcode res =
        curl_easy_perform(curl);

    long httpCode = 0;

    curl_easy_getinfo(
        curl,
        CURLINFO_RESPONSE_CODE,
        &httpCode);

    if (res == CURLE_OK &&
        httpCode == 200 &&
        response.find("ok") != std::string::npos)
    {
        loggedIn = true;
        setConnected(true);
    }
    else
    {
        loggedIn = false;
        setConnected(false);

        blog(
            LOG_WARNING,
            "Yealink login failed: HTTP %ld - %s",
            httpCode,
        curl_easy_strerror(res));
    }

    curl_easy_setopt(
        curl,
        CURLOPT_COOKIELIST,
        "FLUSH");

    curl_easy_cleanup(curl);
}

void PTZYealinkVC120::set(calldata_t *cd)
{
    bool power_on;

    if (calldata_get_bool(cd, "power_on", &power_on)) {
        Q_UNUSED(power_on);
        standbyToggle();
        return;
    }

    bool trigger;

    if (calldata_get_bool(cd, "focus_onetouch_trigger", &trigger) &&
        trigger) {
        standbyToggle();
        return;
    }

    PTZDevice::set(cd);
}

void PTZYealinkVC120::standbyToggle()
{
    sendKey(28);
}

void PTZYealinkVC120::sendKey(int key)
{
    if (!loggedIn) {
        login();

        if (!loggedIn)
            return;
    }

    CURL *curl = curl_easy_init();

    if (!curl)
        return;

    std::string response;

    QString url =
        QString(
            "https://%1:%2/servlet?"
            "p=home&q=sendmsg"
            "&msgtype=keypad&key=%3")
            .arg(host)
            .arg(port)
            .arg(key);

    std::string urlStd =
        url.toStdString();

    QString cookiePath =
        getYealinkCookiePath();

    if (cookiePath.isEmpty()) {
        curl_easy_cleanup(curl);
        return;
    }

    std::string cookiePathStd =
        cookiePath.toStdString();

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

    // Reutiliza a sessão criada pelo login().
    curl_easy_setopt(
        curl,
        CURLOPT_COOKIEFILE,
        cookiePathStd.c_str());

    curl_easy_setopt(
        curl,
        CURLOPT_FOLLOWLOCATION,
        0L);

    curl_easy_setopt(
        curl,
        CURLOPT_HTTPGET,
        1L);

    curl_easy_setopt(
        curl,
        CURLOPT_REFERER,
        refererStd.c_str());

    curl_easy_setopt(
        curl,
        CURLOPT_WRITEFUNCTION,
        WriteCallback);

    curl_easy_setopt(
        curl,
        CURLOPT_WRITEDATA,
        &response);

    CURLcode res =
        curl_easy_perform(curl);

    long httpCode = 0;

    curl_easy_getinfo(
        curl,
        CURLINFO_RESPONSE_CODE,
        &httpCode);

    /*
     * Se a sessão expirou, a VC120 responde HTTP 302.
     * Faz login novamente e repete o comando uma única vez.
     */
    if (res == CURLE_OK &&
        httpCode == 302)
    {

        loggedIn = false;

        curl_easy_cleanup(curl);

        login();

        if (loggedIn) {
            sendKey(key);
        }
        else {
            blog(
                LOG_WARNING,
                "Yealink re-login failed");
        }

        return;
    }

   if (res != CURLE_OK) {
    blog(
        LOG_WARNING,
        "Yealink curl error: %s",
        curl_easy_strerror(res));
    }
    else {
        if (httpCode == 200 &&
            response.find("ok") != std::string::npos)
        {
        }
        else {
            blog(
                LOG_WARNING,
                "Yealink command failed");
        }
    }

    curl_easy_cleanup(curl);
}

void PTZYealinkVC120::sendPreset(int preset)
{
    if (!loggedIn) {
        login();

        if (!loggedIn)
            return;
    }

    CURL *curl = curl_easy_init();

    if (!curl)
        return;

    std::string response;

    QString url =
        QString(
            "https://%1:%2/servlet?"
            "p=home&q=sendmsg"
            "&msgtype=presetting"
            "&prenumber=%3")
            .arg(host)
            .arg(port)
            .arg(preset);


    std::string urlStd =
        url.toStdString();

    QString cookiePath =
        getYealinkCookiePath();

    if (cookiePath.isEmpty()) {
        curl_easy_cleanup(curl);
        return;
    }

    std::string cookiePathStd =
        cookiePath.toStdString();

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
        CURLOPT_FOLLOWLOCATION,
        0L);

    curl_easy_setopt(
        curl,
        CURLOPT_HTTPGET,
        1L);

    curl_easy_setopt(
        curl,
        CURLOPT_REFERER,
        refererStd.c_str());

    curl_easy_setopt(
        curl,
        CURLOPT_WRITEFUNCTION,
        WriteCallback);

    curl_easy_setopt(
        curl,
        CURLOPT_WRITEDATA,
        &response);

    CURLcode res =
        curl_easy_perform(curl);

    long httpCode = 0;

    curl_easy_getinfo(
        curl,
        CURLINFO_RESPONSE_CODE,
        &httpCode);

    if (res == CURLE_OK &&
        httpCode == 302)
    {

        loggedIn = false;

        curl_easy_cleanup(curl);

        login();

        if (loggedIn) {
            sendPreset(preset);
        }

        return;
    }

    if (res != CURLE_OK) {
        blog(
            LOG_WARNING,
            "Yealink preset save curl error: %s",
            curl_easy_strerror(res));
    }
    else {
        if (httpCode == 200 &&
            response.find("complated") != std::string::npos)
        {
        }
        else {
            blog(
                LOG_WARNING,
                "Yealink preset save failed");
        }
    }

    curl_easy_cleanup(curl);
}

void PTZYealinkVC120::recallPreset(int preset)
{
    if (!loggedIn) {
        login();

        if (!loggedIn)
            return;
    }

    CURL *curl = curl_easy_init();

    if (!curl)
        return;

    std::string response;

    QString url =
        QString(
            "https://%1:%2/servlet?"
            "p=home&q=sendmsg"
            "&msgtype=preset"
            "&callid=0"
            "&prenumber=%3")
            .arg(host)
            .arg(port)
            .arg(preset);

    std::string urlStd =
        url.toStdString();

    QString cookiePath =
        getYealinkCookiePath();

    if (cookiePath.isEmpty()) {
        curl_easy_cleanup(curl);
        return;
    }

    std::string cookiePathStd =
        cookiePath.toStdString();

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
        CURLOPT_FOLLOWLOCATION,
        0L);

    curl_easy_setopt(
        curl,
        CURLOPT_HTTPGET,
        1L);

    curl_easy_setopt(
        curl,
        CURLOPT_REFERER,
        refererStd.c_str());

    curl_easy_setopt(
        curl,
        CURLOPT_WRITEFUNCTION,
        WriteCallback);

    curl_easy_setopt(
        curl,
        CURLOPT_WRITEDATA,
        &response);

    CURLcode res =
        curl_easy_perform(curl);

    long httpCode = 0;

    curl_easy_getinfo(
        curl,
        CURLINFO_RESPONSE_CODE,
        &httpCode);

    if (res == CURLE_OK &&
        httpCode == 302)
    {
        loggedIn = false;

        curl_easy_cleanup(curl);

        login();

        if (loggedIn)
            recallPreset(preset);

        return;
    }

    if (res != CURLE_OK) {
        blog(
            LOG_WARNING,
            "Yealink preset recall curl error: %s",
            curl_easy_strerror(res));
    }
    else {
        if (httpCode == 200 &&
            response.find("complated") != std::string::npos)
        {
        }
        else {
            blog(
                LOG_WARNING,
                "Yealink preset recall failed");
        }
    }

    curl_easy_cleanup(curl);
}

void PTZYealinkVC120::do_update()
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

void PTZYealinkVC120::pantilt_home()
{
    sendKey(25);
}
void PTZYealinkVC120::memory_set(int preset)
{
    if (preset < 0 || preset > 15)
        return;

    sendPreset(preset);

}
void PTZYealinkVC120::memory_recall(int preset)
{
    if (preset < 0 || preset > 15)
        return;

    recallPreset(preset);
}

void PTZYealinkVC120::memory_reset(int preset)
{
    if (preset < 0 || preset > 15)
        return;

    if (!loggedIn)
    {
        login();

        if (!loggedIn)
            return;
    }

    CURL *curl = curl_easy_init();

    if (!curl)
        return;

    std::string response;

    QString url =
        QString(
            "https://%1:%2/servlet?"
            "p=home&q=sendmsg"
            "&msgtype=deletepreset"
            "&callid=0"
            "&prenumber=%3")
            .arg(host)
            .arg(port)
            .arg(preset);

    QString cookiePath =
        getYealinkCookiePath();

    if (cookiePath.isEmpty())
    {
        curl_easy_cleanup(curl);
        return;
    }

    std::string urlStd =
        url.toStdString();

    std::string cookiePathStd =
        cookiePath.toStdString();

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
        CURLOPT_FOLLOWLOCATION,
        0L);

    curl_easy_setopt(
        curl,
        CURLOPT_HTTPGET,
        1L);

    curl_easy_setopt(
        curl,
        CURLOPT_REFERER,
        refererStd.c_str());

    curl_easy_setopt(
        curl,
        CURLOPT_WRITEFUNCTION,
        WriteCallback);

    curl_easy_setopt(
        curl,
        CURLOPT_WRITEDATA,
        &response);

    CURLcode res =
        curl_easy_perform(curl);

    long httpCode = 0;

    curl_easy_getinfo(
        curl,
        CURLINFO_RESPONSE_CODE,
        &httpCode);

    if (res == CURLE_OK &&
        httpCode == 302)
    {
        loggedIn = false;

        curl_easy_cleanup(curl);

        login();

        if (loggedIn)
        {

            memory_reset(preset);
        }
        return;
    }

    if (res != CURLE_OK)
    {
        blog(
            LOG_WARNING,
            "Yealink preset delete curl error: %s",
            curl_easy_strerror(res));
    }
    else
    {
        if (httpCode == 200 &&
            response.find("complated") != std::string::npos)
        {
        }
    }
    curl_easy_cleanup(curl);
}
