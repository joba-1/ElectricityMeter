/*
Runtime-configurable external hosts (syslog, InfluxDB, MQTT, NTP, WLED) for the ESP8266.

The build flags (platformio.ini) are only the defaults. POST /api/netconfig
stores a validated replacement in the EEPROM sector (magic, layout version
and CRC, so a blank or foreign sector is never taken for a setting), which
wins at the next boot. The EEPROM sector is separate from any filesystem,
so a filesystem upload keeps it. Same pattern as Joba_Modbus / IntertechnoGW / Radieschen. The boot
info lives in RTC memory, so the EEPROM sector is ours.
*/

#pragma once

#include "NetConfigValidate.h"

class NetConfig {
public:
    struct Values {
        char     syslogHost[NetConfigValidate::MAX_HOST_LEN + 1];
        uint16_t syslogPort;
        char     influxHost[NetConfigValidate::MAX_HOST_LEN + 1];
        uint16_t influxPort;
        char     mqttHost[NetConfigValidate::MAX_HOST_LEN + 1];
        uint16_t mqttPort;
        char     ntp[NetConfigValidate::MAX_HOST_LEN + 1];
        char     wledHost[NetConfigValidate::MAX_HOST_LEN + 1];
    };

    NetConfig(const char *syslogHost, uint16_t syslogPort,
              const char *influxHost, uint16_t influxPort,
              const char *mqttHost, uint16_t mqttPort, const char *ntp, const char *wledHost);

    // Load a stored set over the defaults; a missing/corrupt/invalid one is ignored.
    void load();
    // Validate and store; takes effect at the next boot.
    bool save(const Values &v, const char **badField);
    // Invalidate the stored set; the next boot uses the defaults.
    bool clear();

    static bool validate(const Values &v, const char **badField);

    const Values &active() const { return _active; }
    const Values &defaults() const { return _defaults; }
    bool fromFlash() const { return _fromFlash; }

private:
    Values _defaults;
    Values _active;
    bool _fromFlash = false;
};
