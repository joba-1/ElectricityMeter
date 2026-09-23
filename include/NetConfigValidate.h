/**
 * @file NetConfigValidate.h
 * @brief Pure validation helpers for runtime network configuration.
 *
 * Shared with Joba_Modbus (src/net/NetConfigValidate.h) - keep both in step.
 * Header-only, no Arduino dependency, so the native unit tests can cover
 * every path. Used by NetConfig before anything reaches flash: a bad value
 * stored in NVS would survive every reboot, so it is rejected up front.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace NetConfigValidate {

/// Longest host name accepted (RFC 1123 label rules, total length).
static constexpr size_t MAX_HOST_LEN = 63;
/// Longest InfluxDB base URL accepted ("http://host[:port][/path]").
static constexpr size_t MAX_URL_LEN = 127;

/// True for characters allowed in a host name: letters, digits, '-', '.'.
inline bool isHostChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '.';
}

/**
 * @brief Validate a host name of length @p len.
 *
 * Empty is valid and means "disabled". Otherwise: only host characters,
 * no leading/trailing '-' or '.', no empty label ("a..b"). IPv4 literals
 * pass (digits and dots) - allowed, though names are preferred.
 */
inline bool isValidHostN(const char* s, size_t len) {
    if (!s) return false;
    if (len == 0) return true;
    if (len > MAX_HOST_LEN) return false;
    if (s[0] == '-' || s[0] == '.' || s[len - 1] == '-' || s[len - 1] == '.')
        return false;
    for (size_t i = 0; i < len; ++i) {
        if (!isHostChar(s[i])) return false;
        if (s[i] == '.' && i + 1 < len && s[i + 1] == '.') return false;
    }
    return true;
}

/// Validate a NUL-terminated host name (see isValidHostN).
inline bool isValidHost(const char* s) {
    return s && isValidHostN(s, strnlen(s, MAX_HOST_LEN + 1));
}

/**
 * @brief Parse a decimal TCP/UDP port (1..65535).
 * @param s    NUL-terminated digits, no sign, no spaces.
 * @param out  Receives the port on success.
 * @return false on empty input, non-digits, 0 or > 65535.
 */
inline bool parsePort(const char* s, uint16_t& out) {
    if (!s || !*s) return false;
    uint32_t v = 0;
    for (const char* p = s; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10 + (uint32_t)(*p - '0');
        if (v > 65535) return false;
    }
    if (v == 0) return false;
    out = (uint16_t)v;
    return true;
}

/**
 * @brief Validate an InfluxDB base URL: "http://" host [":" port] ["/" path].
 *
 * Empty is valid (disabled). Only plain http - the firmware's raw writer
 * does not speak TLS. The path may contain URL-safe characters only, so a
 * stored value can never inject header or query syntax.
 */
inline bool isValidInfluxUrl(const char* s) {
    if (!s) return false;
    size_t len = strnlen(s, MAX_URL_LEN + 1);
    if (len == 0) return true;
    if (len > MAX_URL_LEN) return false;
    static const char scheme[] = "http://";
    if (strncmp(s, scheme, sizeof(scheme) - 1) != 0) return false;
    const char* host = s + sizeof(scheme) - 1;
    const char* end = host;
    while (*end && *end != ':' && *end != '/') ++end;
    if (end == host || !isValidHostN(host, (size_t)(end - host))) return false;
    const char* p = end;
    if (*p == ':') {
        char port[6] = {0};
        const char* q = ++p;
        while (*q && *q != '/') ++q;
        size_t n = (size_t)(q - p);
        if (n == 0 || n >= sizeof(port)) return false;
        memcpy(port, p, n);
        uint16_t dummy;
        if (!parsePort(port, dummy)) return false;
        p = q;
    }
    for (; *p; ++p) {
        char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '/' || c == '-' ||
                  c == '_' || c == '.' || c == '~';
        if (!ok) return false;
    }
    return true;
}

/// The runtime-configurable external resources. Fixed buffers, no heap.
struct Values {
    char     syslogHost[MAX_HOST_LEN + 1];
    uint16_t syslogPort;
    char     mqttHost[MAX_HOST_LEN + 1];
    uint16_t mqttPort;
    char     influxUrl[MAX_URL_LEN + 1];
    char     ntp1[MAX_HOST_LEN + 1];
    char     ntp2[MAX_HOST_LEN + 1];
};

/**
 * @brief Check a complete value set.
 * @param v        Values to check (strings must be NUL-terminated).
 * @param badField Receives the name of the first invalid field, if any.
 * @return true if every field is valid.
 */
inline bool validate(const Values& v, const char** badField) {
    const char* bad = nullptr;
    if      (!isValidHost(v.syslogHost))  bad = "syslogHost";
    else if (v.syslogPort == 0)           bad = "syslogPort";
    else if (!isValidHost(v.mqttHost))    bad = "mqttHost";
    else if (v.mqttPort == 0)             bad = "mqttPort";
    else if (!isValidInfluxUrl(v.influxUrl)) bad = "influxUrl";
    else if (!isValidHost(v.ntp1))        bad = "ntp1";
    else if (!isValidHost(v.ntp2))        bad = "ntp2";
    if (badField) *badField = bad;
    return bad == nullptr;
}

/**
 * @brief Copy @p src into a fixed field of size @p cap, NUL-terminated.
 * @return false (and leave @p dst untouched) if it does not fit.
 */
inline bool copyField(char* dst, size_t cap, const char* src) {
    if (!dst || !src || cap == 0) return false;
    size_t n = strnlen(src, cap);
    if (n >= cap) return false;
    memcpy(dst, src, n + 1);
    return true;
}

}  // namespace NetConfigValidate
