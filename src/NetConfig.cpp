#include "NetConfig.h"

#include <Arduino.h>
#include <EEPROM.h>
#include <Syslog.h>   // LOG_* priorities

extern Syslog syslog;   // main.cpp

namespace {

const uint32_t MAGIC = 0x4E434650;   // "NCFP" (power3 layout)
const uint8_t LAYOUT_VERSION = 1;    // bump when Values changes

struct Stored {
    uint32_t magic;
    uint8_t version;
    NetConfig::Values values;
    uint32_t crc;
};

uint32_t crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    while (len--) {
        crc ^= *data++;
        for (int i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xEDB88320 & (0 - (crc & 1)));
    }
    return ~crc;
}

uint32_t crcOf(const Stored &s) {
    return crc32(reinterpret_cast<const uint8_t *>(&s), offsetof(Stored, crc));
}

// Make sure every string field is terminated (the sector may hold anything).
void terminate(NetConfig::Values &v) {
    v.syslogHost[sizeof(v.syslogHost) - 1] = '\0';
    v.influxHost[sizeof(v.influxHost) - 1] = '\0';
    v.mqttHost[sizeof(v.mqttHost) - 1] = '\0';
    v.ntp[sizeof(v.ntp) - 1] = '\0';
    v.wledHost[sizeof(v.wledHost) - 1] = '\0';
}

}  // namespace

NetConfig::NetConfig(const char *syslogHost, uint16_t syslogPort,
                     const char *influxHost, uint16_t influxPort,
                     const char *mqttHost, uint16_t mqttPort, const char *ntp, const char *wledHost) {
    using NetConfigValidate::copyField;
    memset(&_defaults, 0, sizeof(_defaults));
    copyField(_defaults.syslogHost, sizeof(_defaults.syslogHost), syslogHost ? syslogHost : "");
    _defaults.syslogPort = syslogPort;
    copyField(_defaults.influxHost, sizeof(_defaults.influxHost), influxHost ? influxHost : "");
    _defaults.influxPort = influxPort;
    copyField(_defaults.mqttHost, sizeof(_defaults.mqttHost), mqttHost ? mqttHost : "");
    _defaults.mqttPort = mqttPort;
    copyField(_defaults.ntp, sizeof(_defaults.ntp), ntp ? ntp : "");
    copyField(_defaults.wledHost, sizeof(_defaults.wledHost), wledHost ? wledHost : "");
    _active = _defaults;
}

bool NetConfig::validate(const Values &v, const char **badField) {
    using namespace NetConfigValidate;
    const char *bad = nullptr;
    if (!isValidHost(v.syslogHost))      bad = "syslogHost";
    else if (v.syslogPort == 0)          bad = "syslogPort";
    else if (!isValidHost(v.influxHost)) bad = "influxHost";
    else if (v.influxPort == 0)          bad = "influxPort";
    else if (!isValidHost(v.mqttHost))   bad = "mqttHost";
    else if (v.mqttPort == 0)            bad = "mqttPort";
    else if (!isValidHost(v.ntp))        bad = "ntp";
    else if (!isValidHost(v.wledHost))   bad = "wledHost";
    if (badField) *badField = bad;
    return bad == nullptr;
}

void NetConfig::load() {
    Stored s;
    EEPROM.begin(sizeof(Stored));
    EEPROM.get(0, s);
    EEPROM.end();
    if (s.magic != MAGIC || s.version != LAYOUT_VERSION || s.crc != crcOf(s)) {
        return;   // blank, foreign or corrupt: defaults
    }
    terminate(s.values);
    const char *bad = nullptr;
    if (!validate(s.values, &bad)) {
        syslog.logf(LOG_WARNING, "Stored netconfig value '%s' invalid - using defaults", bad);
        return;
    }
    _active = s.values;
    _fromFlash = true;
}

bool NetConfig::save(const Values &v, const char **badField) {
    if (!validate(v, badField)) {
        return false;
    }
    Stored s;
    memset(&s, 0, sizeof(s));
    s.magic = MAGIC;
    s.version = LAYOUT_VERSION;
    s.values = v;
    s.crc = crcOf(s);
    EEPROM.begin(sizeof(Stored));
    EEPROM.put(0, s);
    bool ok = EEPROM.commit();
    EEPROM.end();
    if (!ok && badField) *badField = "eeprom";
    syslog.logf(LOG_NOTICE, "Netconfig saved: syslog=%s:%u influx=%s:%u mqtt=%s:%u ntp=%s wled=%s (%s)",
                v.syslogHost, v.syslogPort, v.influxHost, v.influxPort, v.mqttHost, v.mqttPort,
                v.ntp, v.wledHost, ok ? "ok" : "FAILED");
    return ok;
}

bool NetConfig::clear() {
    EEPROM.begin(sizeof(Stored));
    uint32_t zero = 0;
    EEPROM.put(0, zero);   // breaks the magic; the rest is ignored
    bool ok = EEPROM.commit();
    EEPROM.end();
    syslog.log(LOG_NOTICE, "Netconfig cleared, defaults at next boot");
    return ok;
}
