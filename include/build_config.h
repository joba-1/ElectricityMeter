/* Defaults from platformio.ini build_flags - Generated on 2026-02-05 */

#ifndef ELECTRICITYMETER_BUILD_CONFIG_H
#define ELECTRICITYMETER_BUILD_CONFIG_H

#ifndef VERSION
#define VERSION "0.0"
#endif

#ifndef PROGNAME
#define PROGNAME "power"
#endif

#ifndef HOSTNAME
#define HOSTNAME "power3"
#endif

#ifndef INFLUX_DB
#define INFLUX_DB "power"
#endif

#ifndef INFLUX_SERVER
#define INFLUX_SERVER "job4"
#endif

#ifndef INFLUX_PORT
#define INFLUX_PORT 8086
#endif

#ifndef SYSLOG_SERVER
#define SYSLOG_SERVER "job4"
#endif

#ifndef SYSLOG_PORT
#define SYSLOG_PORT 514
#endif

#ifndef WLED_LEDS
#define WLED_LEDS 60
#endif

#ifndef WLED_BRIGHTNESS
#define WLED_BRIGHTNESS 100
#endif

#ifndef WLED_HOST
#define WLED_HOST "wled1"
#endif

#ifndef WLED_PORT
#define WLED_PORT 21324
#endif

#ifndef WLED_CONSUMPTION_HIGH
#define WLED_CONSUMPTION_HIGH 4000
#endif

#ifndef WLED_BACKFEED_TOO_HIGH
#define WLED_BACKFEED_TOO_HIGH 99999
#endif

#ifndef WLED_BACKFEED_VERY_HIGH
#define WLED_BACKFEED_VERY_HIGH 99999
#endif

#ifndef WLED_BACKFEED_GOOD
#define WLED_BACKFEED_GOOD 200
#endif

#ifndef MQTT_BROKER
#define MQTT_BROKER "job4"
#endif

#ifndef MQTT_PORT
#define MQTT_PORT 1883
#endif

#ifndef DTU_TOPIC
#define DTU_TOPIC "OpenDTU1"
#endif

#ifndef INVERTER_LIMIT
#define INVERTER_LIMIT 800
#endif

#ifndef BACKFEED_MIN
#define BACKFEED_MIN 5000
#endif

#ifndef BACKFEED_MAX
#define BACKFEED_MAX 5000
#endif

#ifndef LIMIT_CHECK_INTERVAL_S
#define LIMIT_CHECK_INTERVAL_S 10
#endif

#ifndef LIMIT_ROUND_GRANULARITY
#define LIMIT_ROUND_GRANULARITY 100
#endif

#ifndef PROD_KW_MAX
#define PROD_KW_MAX 15
#endif

#ifndef USAGE_KW_MAX
#define USAGE_KW_MAX 20
#endif

#ifndef NTP_SERVER
#define NTP_SERVER "fritz.box"
#endif

#ifndef SERIAL_SPEED
#define SERIAL_SPEED 9600
#endif

#endif // ELECTRICITYMETER_BUILD_CONFIG_H
