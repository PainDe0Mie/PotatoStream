#include "update_check.hpp"

#include <curl/curl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef VERSION_MAJOR
#define VERSION_MAJOR 0
#endif
#ifndef VERSION_MINOR
#define VERSION_MINOR 0
#endif
#ifndef VERSION_MICRO
#define VERSION_MICRO 0
#endif

#define UPDATE_RELEASE_API                                                     \
    "https://api.github.com/repos/PainDe0Mie/PotatoStream/releases/latest"
#define UPDATE_USER_AGENT "StreamPotato-3DS"
#define UPDATE_TIMEOUT_S 8L
#define UPDATE_MAX_BYTES (256 * 1024)

namespace {

size_t write_response(void *contents, size_t size, size_t nmemb, void *userp) {
    const size_t chunk = size * nmemb;
    std::string *body = static_cast<std::string *>(userp);
    if (body->size() + chunk > UPDATE_MAX_BYTES) {
        return 0;
    }
    body->append(static_cast<const char *>(contents), chunk);
    return chunk;
}

bool parse_version(const char *text, int out[3]) {
    if (text == nullptr) {
        return false;
    }
    while (*text == 'v' || *text == 'V' || *text == ' ') {
        text++;
    }
    if (*text < '0' || *text > '9') {
        return false;
    }

    out[0] = out[1] = out[2] = 0;
    for (int i = 0; i < 3; i++) {
        char *end = nullptr;
        const long value = strtol(text, &end, 10);
        if (end == text) {
            break;
        }
        out[i] = value < 0 ? 0 : (int)value;
        if (*end != '.') {
            break;
        }
        text = end + 1;
    }
    return true;
}

std::string extract_tag_name(const std::string &body) {
    const char *key = "\"tag_name\"";
    const size_t key_pos = body.find(key);
    if (key_pos == std::string::npos) {
        return std::string();
    }

    size_t pos = body.find(':', key_pos + strlen(key));
    if (pos == std::string::npos) {
        return std::string();
    }
    pos = body.find('"', pos);
    if (pos == std::string::npos) {
        return std::string();
    }
    const size_t end = body.find('"', pos + 1);
    if (end == std::string::npos) {
        return std::string();
    }
    return body.substr(pos + 1, end - pos - 1);
}

} // namespace

const char *update_check_current_version() {
    static char version[32];
    if (version[0] == '\0') {
        snprintf(version, sizeof(version), "%d.%d.%d", VERSION_MAJOR,
                 VERSION_MINOR, VERSION_MICRO);
    }
    return version;
}

UpdateCheckResult update_check_run() {
    UpdateCheckResult result;

    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        result.failed = true;
        result.error = "Cannot create the HTTP client";
        return result;
    }

    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, UPDATE_RELEASE_API);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, UPDATE_USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 4L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_response);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, UPDATE_TIMEOUT_S);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, UPDATE_TIMEOUT_S);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    const CURLcode status = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (status != CURLE_OK) {
        result.failed = true;
        result.error = curl_easy_strerror(status);
        return result;
    }

    result.latest_tag = extract_tag_name(body);
    if (result.latest_tag.empty()) {
        result.failed = true;
        result.error = "No release tag in the GitHub answer";
        return result;
    }

    int latest[3];
    if (!parse_version(result.latest_tag.c_str(), latest)) {
        result.failed = true;
        result.error = "Unreadable release tag";
        return result;
    }

    const int current[3] = {VERSION_MAJOR, VERSION_MINOR, VERSION_MICRO};
    for (int i = 0; i < 3; i++) {
        if (latest[i] != current[i]) {
            result.update_available = latest[i] > current[i];
            break;
        }
    }
    return result;
}
