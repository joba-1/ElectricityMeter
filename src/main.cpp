#include <Arduino.h>

// defaults, can be overridden by platformio.ini build_flags
#include "build_config.h"

// Web Updater
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiClient.h>

// Post to InfluxDB
#include <ESP8266HTTPClient.h>

// Infrastructure
#include <NTPClient.h>
#include <Syslog.h>
#include <WiFiManager.h>
#include <WiFiUdp.h>
#include <SoftwareSerial.h>

#ifndef PWMRANGE
#define PWMRANGE 1023
#endif

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(*(a)))

#define DB_LED_PIN D4
#define IR_LED_PIN D1

#define DB_LED_ON LOW
#define DB_LED_OFF HIGH

#define WEBSERVER_PORT 80

SoftwareSerial mirror(NOT_A_PIN, IR_LED_PIN, true);  // TX only

ESP8266WebServer web_server(WEBSERVER_PORT);

ESP8266HTTPUpdateServer esp_updater;

// Post to InfluxDB
WiFiClient client;
HTTPClient http;
int influx_status = 0;
time_t post_time = 0;

const uint32_t ok_interval = 5000;
const uint32_t err_interval = 1000;

uint32_t breathe_interval = ok_interval; // ms for one led breathe cycle

WiFiUDP ntpUDP;
NTPClient ntp(ntpUDP, NTP_SERVER);
static char start_time[30] = "";

WiFiUDP logUDP;
Syslog syslog(logUDP, SYSLOG_PROTO_IETF);

uint32_t last_counter_reset = 0;      // millis() of last counter reset
volatile uint32_t counter_events = 0; // events of current interval so far

typedef struct itron_3hz {
  uint8_t valid;  // valid if 63 (one bit for each field)
  char id[3];
  char serial[10];
  uint64_t file;
  uint32_t uptime;
  uint64_t aPlus;  // now 1/10 Wh
  uint64_t aMinus; // now 1/10 Wh
  bool detailed;
} itron_3hz_t;

itron_3hz_t itron = {0};
time_t recv_time = 0;
bool recv_detailed = true;

uint8_t sml_raw[2560];  // last sml record, enough for 2s at 9600 baud
size_t sml_len = 0;  // length of last sml record

char *to_hex( char *buf, size_t len, char sep ) {
  static char hex[1024*3+1];
  char *out = hex;
  while( len-- ) {
    snprintf(out, 4, "%02x%c", *(buf++), sep);
    out += 3;
  }
  *(--out) = '\0';
  return hex;
}

// Post data to InfluxDB
void post_data() {
  static const char uri[] = "/write?db=" INFLUX_DB "&precision=s";

  const char fmt[] = "energy,meter=%s watt=%llu,watt_out=%llu\n";
  char msg[sizeof(fmt) + 30 + 2 * 10];
  char *serial = to_hex(itron.serial, sizeof(itron.serial), '-');
  snprintf(msg, sizeof(msg), fmt, serial, (itron.aPlus+5)/10, (itron.aMinus+5)/10);

  http.begin(client, INFLUX_SERVER, INFLUX_PORT, uri);
  http.setUserAgent(PROGNAME);
  influx_status = http.POST(msg);
  String payload = http.getString();
  http.end();
  
  if (influx_status < 200 || influx_status > 299) {
    breathe_interval = err_interval;
    syslog.logf(LOG_ERR, "Post %s:%d%s status=%d msg='%s' response='%s'", INFLUX_SERVER,
                INFLUX_PORT, uri, influx_status, msg, payload.c_str());
  } else {
    breathe_interval = ok_interval;
    post_time = time(NULL);
  };
}

#ifdef WLED_LEDS
WiFiUDP wledUDP;
const uint8_t wled_secs = 5;
// Consider enabling wled setting "Force max brightness" to be independent from wled master brightness
const uint8_t wled_brightness = WLED_BRIGHTNESS;  // 0..255
// only for display on web page
uint8_t wled_r = 0;
uint8_t wled_g = 0;
uint8_t wled_b = 0;
uint32_t wled_update = 0;  // ms of last udp packet
uint32_t wled_change = 0;  // ms of last color change

void send_wled() {
  static uint32_t uptime = 0;
  static uint64_t aPlus = 0;
  static uint64_t aMinus = 0;
  static bool isOn = false;  // for on/off hysteresis
  
  uint64_t aPlusW = 0;
  uint64_t aMinusW = 0;

  if( itron.valid == 0x3f ) {
    if( uptime != itron.uptime && (itron.aPlus != aPlus || itron.aMinus != aMinus) ) {
      if( uptime ) {
        aPlusW = (itron.aPlus - aPlus) * 360 / (itron.uptime - uptime);
        aMinusW = (itron.aMinus - aMinus) * 360 / (itron.uptime - uptime);
      }
      
      uptime = itron.uptime;
      aPlus = itron.aPlus;
      aMinus = itron.aMinus;

      uint8_t r = 0, g = 0, b = 0;
      if( aPlusW > WLED_CONSUMPTION_HIGH ) {
        r = 0xff, b = 0x22;  // red warning on high load
        isOn = false;
      }
      else if( aMinusW > WLED_BACKFEED_TOO_HIGH ) {
        b = 0xff;  // too high back feed: blue
        isOn = true;
      }
      else if( aMinusW > WLED_BACKFEED_VERY_HIGH ) {
        g = 0xff; b = 0xff;  // very high back feed: cyan
        isOn = true;
      }
      else if( (isOn and (aPlusW == 0 || aMinusW > 0)) || aMinusW > WLED_BACKFEED_GOOD ) {
        g = 0xff; // good back feed: green
        isOn = true;
      }
      else {
        isOn = false;
      }
      
      if( isOn ) {
        r = (uint16_t)wled_brightness * r / 255;
        g = (uint16_t)wled_brightness * g / 255;
        b = (uint16_t)wled_brightness * b / 255;
      }

      // syslog.logf(LOG_NOTICE, "wled: A+ %llu W, A- %llu W -> rgb %u,%u,%u", aPlusW, aMinusW, r, g, b);

      if( (r || g || b) && wledUDP.beginPacket(WLED_HOST, WLED_PORT) ) {
        wledUDP.write(2);  // WLED proto DRGB
        wledUDP.write(wled_secs);  // hold color for some seconds
        int led = WLED_LEDS;
        while( led-- ) {
          wledUDP.write(r);
          wledUDP.write(g);
          wledUDP.write(b);
        }
        wledUDP.endPacket();
        wled_update = millis();
        if( wled_r != r || wled_g != g || wled_b != b ) {
          wled_change = wled_update;
          wled_r = r;
          wled_g = g;
          wled_b = b;
        }
      }
    }
  }
}
#endif

#ifdef DTU_TOPIC
#include <PubSubClient.h>

WiFiClient wifiMqtt;
PubSubClient mqtt(wifiMqtt);
const char topic_name[] =      DTU_TOPIC "/" INVERTER_SERIAL "/name";
const char topic_limit[] =     DTU_TOPIC "/" INVERTER_SERIAL "/status/limit_absolute";
const char topic_reachable[] = DTU_TOPIC "/" INVERTER_SERIAL "/status/reachable";
const char topic_dynamic[] =   DTU_TOPIC "/" INVERTER_SERIAL "/status/limit_dynamic";
char inverter[80] = "?";  
uint16_t curr_limit = UINT16_MAX;
bool reachable = false;
bool dynamic = false;

/*
Send MQTT request to change power limit if it changed
Limit is rounded to full 100W  
*/
void publish_limit( uint64_t prod, uint16_t limit ) {
  /// syslog.logf(LOG_INFO, "publish_limit for prod %llu: %u W", prod, limit);

  limit = ((limit + LIMIT_ROUND_GRANULARITY/2) / LIMIT_ROUND_GRANULARITY) * LIMIT_ROUND_GRANULARITY;  // round limit

  if( limit != curr_limit ) {
    char payload[10];
    snprintf(payload, sizeof(payload), "%u", limit);

    if( !mqtt.connected() || (dynamic && !mqtt.publish(DTU_TOPIC "/" INVERTER_SERIAL "/cmd/limit_nonpersistent_absolute", payload))) {
      syslog.logf(LOG_ERR, "Mqtt publish limit %s for inverter '%s' failed", payload, inverter);
    }
    else if (dynamic) {
      syslog.logf(LOG_NOTICE, "Producing %llu W -> change nonpersistent limit of inverter '%s' from %u to %s W", prod, inverter, curr_limit, payload);
    }
    else {
      syslog.logf(LOG_NOTICE, "Producing %llu W -> no change of limit for inverter '%s' from %u to %s W due to topic '%s' is not 1", prod, inverter, curr_limit, payload, topic_dynamic);
    }
  }
  else {
      syslog.logf(LOG_NOTICE, "Producing %llu W -> rounded limit for inverter '%s' of %u W still the same", prod, inverter, curr_limit);
  }
}

/*
If feed to the grid is outside of a given range, adjust inverter limit to be as close as possible in the center of that range
*/
void check_limit() {
  const uint16_t max_limit = INVERTER_LIMIT;  // unthrottled WR while backfeed is small enough
  const uint16_t min_aMinus = BACKFEED_MIN;   // if actual backfeed is lower, inverter gets less limited 
  const uint16_t max_aMinus = BACKFEED_MAX;   // if actual backfeed is higher, inverter gets more limited
  const uint16_t min_check_delay_s = LIMIT_CHECK_INTERVAL_S;  // high enough to make power calc from counter reliable
                                                              // low enough to limit time with too high backfeed
  static uint32_t uptime = 0;
  static uint64_t aMinus = 0;
  
  uint64_t aMinusW = 0;

  if( itron.valid == 0x3f ) {
    // we have valid backfeed data
    uint32_t delta_t = itron.uptime - uptime;
    if( uptime && delta_t > min_check_delay_s ) {
      // the last check is more than min_check_delay_s ago
      // calculate average backfeed in W from the ever increasing backfeed counter
      // and the elapsed time since last check
      aMinusW = (itron.aMinus - aMinus) * 360 / delta_t;
      /// syslog.logf(LOG_INFO, "Check: Curr A-: %llu W, dt = %u s", aMinusW, delta_t);
    
      if( curr_limit != UINT16_MAX && reachable ) {
        // only try to change the limit if the current limit is known 
        // and the inverter is reachable
        if( aMinusW > max_aMinus && curr_limit > 0 ) {
          // current backfeed is too high and inverter is not yet fully limited
          // change of inverter limit to backfeed right in the middle of the desired range
          uint16_t delta = aMinusW - (min_aMinus + max_aMinus)/2;
          publish_limit(aMinusW, (curr_limit > delta) ? curr_limit - delta : 0);
        }
        else if( aMinusW < min_aMinus && curr_limit < max_limit ) {
          // current backfeed is too low and inverter is not yet fully opened
          // change of inverter limit to backfeed right in the middle of the desired range
          uint16_t delta = (min_aMinus + max_aMinus)/2 - aMinusW;
          publish_limit(aMinusW, (curr_limit + delta < max_limit) ? curr_limit + delta : max_limit);
        }
        else {
          /// syslog.logf(LOG_INFO, "Check: current limit %u W not changed for prod %llu W", curr_limit, aMinusW);
        }
      }
      else {
        /// syslog.logf(LOG_INFO, "Check: current limit %u W not changed, elapsed since update: %u s", curr_limit, (now - update_ms)/1000);
      }
      uptime = itron.uptime;
      aMinus = itron.aMinus;
    }
  }
}

// send current power consumption or production to mqtt
void publish_data() {
  static uint64_t lastAPlusW = 0;
  static uint64_t lastAMinusW = 0;

  uint64_t aPlusW = (itron.aPlus + 5) / 10;
  uint64_t aMinusW = (itron.aMinus + 5) / 10;

  if( aPlusW != lastAPlusW ) {
    char wh[20];
    snprintf(wh, sizeof(wh), "%llu", aPlusW);
    mqtt.publish(HOSTNAME "/Wh_In", wh);
    lastAPlusW = aPlusW;
  }

  if( aMinusW != lastAMinusW ) {
    char wh[20];
    snprintf(wh, sizeof(wh), "%llu", aMinusW);
    mqtt.publish(HOSTNAME "/Wh_Out", wh);
    lastAMinusW = aMinusW;
  }
}

// Called on incoming mqtt limit_absolute messages
void mqtt_callback(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic_reachable, topic) == 0) {
    if( length > 0 ) {
      bool flag = payload[0] != '0';
      if( flag != reachable ) {
          syslog.logf(LOG_NOTICE, "Inverter '%s' is %s", inverter, flag ? "reachable" : "unreachable");
          reachable = flag;
      }
    }
  }
  else if (strcmp(topic_limit, topic) == 0) {
    char *endp;
    char *str = (char *)payload;
    if( length > 0 ) {
      unsigned long limit = strtoul(str, &endp, 10);
      if( endp != str && limit < UINT16_MAX ) {
        limit = ((limit + LIMIT_ROUND_GRANULARITY/2) / LIMIT_ROUND_GRANULARITY) * LIMIT_ROUND_GRANULARITY;
        if( limit != curr_limit ) {
          syslog.logf(LOG_NOTICE, "Inverter '%s' limit is %lu W", inverter, limit);
          curr_limit = limit;
        }
      }
    }
  }
  else if (strcmp(topic_name, topic) == 0) {
    if( length > 0 ) {
      size_t len = min((size_t)sizeof(inverter)-1, length);
      if( len != strlen(inverter) || strncmp(inverter, (char *)payload, len)) {
        strncpy(inverter, (char *)payload, len);
        inverter[len] = '\0';
        syslog.logf(LOG_NOTICE, "Inverter name is '%s'", inverter);
      }
    }
  }
  else if (strcmp(topic_dynamic, topic) == 0) {
    bool flag = false;
    if( length > 0 ) {
      flag = payload[0] == '1';
    }
    if( flag != dynamic ) {
        syslog.logf(LOG_NOTICE, "Inverter '%s' limit is %s", inverter, flag ? "dynamic" : "static");
        dynamic = flag;
    }
  }
  else {
    syslog.logf(LOG_ERR, "Unknown topic '%s'", topic);
  }
}

void handle_mqtt() {
  static const int32_t interval = 5000;  // if disconnected try reconnect this often in ms
  static uint32_t prev = -interval;      // first connect attempt without delay
  static char msg[128];

  if (mqtt.connected()) {
    mqtt.loop();
  }
  else {
    uint32_t now = millis();
    if (now - prev > interval) {
      prev = now;

      if (mqtt.connect(HOSTNAME, HOSTNAME "/LWT", 0, true, "Offline")
      && mqtt.publish(HOSTNAME "/LWT", "Online", true)
      && mqtt.publish(HOSTNAME "/Version", VERSION, true)
      && mqtt.subscribe(topic_reachable)
      && mqtt.subscribe(topic_limit)
      && mqtt.subscribe(topic_dynamic)
      && mqtt.subscribe(topic_name)) {
        snprintf(msg, sizeof(msg), "Connected to MQTT broker %s:%d using topic %s", MQTT_BROKER, MQTT_PORT, HOSTNAME);
        syslog.log(LOG_NOTICE, msg);
      }
      else {
        int error = mqtt.state();
        mqtt.disconnect();
        snprintf(msg, sizeof(msg), "Connect to MQTT broker %s:%d failed with code %d", MQTT_BROKER, MQTT_PORT, error);
        syslog.log(LOG_ERR, msg);
      }
    }
  }
}
#endif

// Period consumption tracking. Baselines hold the meter reading at the start
// of each calendar period; rotating them on rollover lets us derive
// current-period and previous-period consumption from delta math.
typedef struct {
  uint64_t aPlus;   // 1/10 Wh
  uint64_t aMinus;  // 1/10 Wh
  bool valid;
} period_t;

static period_t today_start      = {0, 0, false};
static period_t yesterday_start  = {0, 0, false};
static period_t thisweek_start   = {0, 0, false};
static period_t lastweek_start   = {0, 0, false};
static period_t thismonth_start  = {0, 0, false};
static period_t lastmonth_start  = {0, 0, false};
static period_t thisyear_start   = {0, 0, false};
static period_t lastyear_start   = {0, 0, false};

static int last_day_idx   = -1;
static int last_week_idx  = -1;
static int last_month_idx = -1;
static int last_year_idx  = -1;

// UTC epoch for the start of the local "today minus N days"
static time_t local_day_start( time_t now, int days_back ) {
  struct tm t;
  localtime_r(&now, &t);
  t.tm_sec = 0;
  t.tm_min = 0;
  t.tm_hour = 0;
  t.tm_mday -= days_back;
  t.tm_isdst = -1;
  return mktime(&t);
}

// UTC epoch for the start of the local Monday of "this week minus N weeks"
static time_t local_week_start( time_t now, int weeks_back ) {
  struct tm t;
  localtime_r(&now, &t);
  t.tm_sec = 0;
  t.tm_min = 0;
  t.tm_hour = 0;
  int wday_mon = (t.tm_wday + 6) % 7;  // Mon=0..Sun=6
  t.tm_mday -= wday_mon + weeks_back * 7;
  t.tm_isdst = -1;
  return mktime(&t);
}

static time_t local_month_start( time_t now, int months_back ) {
  struct tm t;
  localtime_r(&now, &t);
  t.tm_sec = 0;
  t.tm_min = 0;
  t.tm_hour = 0;
  t.tm_mday = 1;
  t.tm_mon -= months_back;
  t.tm_isdst = -1;
  return mktime(&t);
}

static time_t local_year_start( time_t now, int years_back ) {
  struct tm t;
  localtime_r(&now, &t);
  t.tm_sec = 0;
  t.tm_min = 0;
  t.tm_hour = 0;
  t.tm_mday = 1;
  t.tm_mon = 0;
  t.tm_year -= years_back;
  t.tm_isdst = -1;
  return mktime(&t);
}

// Query InfluxDB for the first 'watt' / 'watt_out' values at-or-after epoch.
// On success, fills out and returns true. The stored values are integer Wh
// (from post_data) so we scale back to 1/10 Wh to match itron.aPlus units.
static bool query_baseline( time_t epoch, period_t *out ) {
  static char url[256];
  // Manual URL encoding: ',' -> %2C, '>=' -> %3E%3D, spaces -> '+'
  snprintf(url, sizeof(url),
    "/query?db=" INFLUX_DB "&epoch=s"
    "&q=SELECT+first(watt)%%2Cfirst(watt_out)+FROM+energy+WHERE+time+%%3E%%3D+%lus",
    (unsigned long)epoch);

  http.begin(client, INFLUX_SERVER, INFLUX_PORT, url);
  http.setUserAgent(PROGNAME);
  http.addHeader("Accept", "application/csv");
  int code = http.GET();

  bool ok = false;
  if( code == 200 ) {
    String body = http.getString();
    int nl = body.indexOf('\n');
    if( nl > 0 && (size_t)(nl + 1) < body.length() ) {
      // CSV columns: name,tags,time,first,first_1 — skip 3 commas to reach 'first'
      const char *line = body.c_str() + nl + 1;
      int comma = 0;
      while( *line && comma < 3 ) {
        if( *line == ',' ) comma++;
        line++;
      }
      if( comma == 3 ) {
        char *end;
        unsigned long long ap = strtoull(line, &end, 10);
        if( end > line && *end == ',' ) {
          unsigned long long am = strtoull(end + 1, &end, 10);
          out->aPlus  = (uint64_t)ap * 10;
          out->aMinus = (uint64_t)am * 10;
          out->valid  = true;
          ok = true;
        }
      }
    }
  }
  http.end();
  if( !ok ) {
    syslog.logf(LOG_NOTICE, "Influx baseline query for epoch %lu http=%d (no data?)",
                (unsigned long)epoch, code);
  }
  return ok;
}

static void load_baselines_from_influx( time_t now ) {
  syslog.log(LOG_NOTICE, "Loading period baselines from InfluxDB");
  query_baseline(local_day_start(now, 0),    &today_start);
  query_baseline(local_day_start(now, 1),    &yesterday_start);
  query_baseline(local_week_start(now, 0),   &thisweek_start);
  query_baseline(local_week_start(now, 1),   &lastweek_start);
  query_baseline(local_month_start(now, 0),  &thismonth_start);
  query_baseline(local_month_start(now, 1),  &lastmonth_start);
  query_baseline(local_year_start(now, 0),   &thisyear_start);
  query_baseline(local_year_start(now, 1),   &lastyear_start);
  syslog.logf(LOG_NOTICE,
    "Baselines loaded: today=%d yest=%d thisweek=%d lastweek=%d "
    "thismonth=%d lastmonth=%d thisyear=%d lastyear=%d",
    today_start.valid, yesterday_start.valid,
    thisweek_start.valid, lastweek_start.valid,
    thismonth_start.valid, lastmonth_start.valid,
    thisyear_start.valid, lastyear_start.valid);
}

void update_period_baselines() {
  if( itron.valid != 0x3f ) return;
  time_t now = time(NULL);
  if( now < 1000000000 ) return;  // wait for NTP sync (seconds since epoch ~2001)

  struct tm tm_now;
  localtime_r(&now, &tm_now);

  int day_idx   = tm_now.tm_year * 400 + tm_now.tm_yday;  // unique per local day
  int wday_mon  = (tm_now.tm_wday + 6) % 7;               // Mon=0..Sun=6
  int week_idx  = day_idx - wday_mon;                     // unique per ISO-style week
  int month_idx = tm_now.tm_year * 12 + tm_now.tm_mon;
  int year_idx  = tm_now.tm_year;

  period_t curr = { itron.aPlus, itron.aMinus, true };

  if( last_day_idx == -1 ) {
    // First valid reading after boot+NTP: try to recover historical baselines
    // from InfluxDB so reboots don't reset our period counters.
    load_baselines_from_influx(now);
    // Fall back to current reading for any period without InfluxDB data.
    if( !today_start.valid     ) today_start     = curr;
    if( !thisweek_start.valid  ) thisweek_start  = curr;
    if( !thismonth_start.valid ) thismonth_start = curr;
    if( !thisyear_start.valid  ) thisyear_start  = curr;
  }
  else {
    if( day_idx   != last_day_idx   ) { yesterday_start = today_start;     today_start     = curr; }
    if( week_idx  != last_week_idx  ) { lastweek_start  = thisweek_start;  thisweek_start  = curr; }
    if( month_idx != last_month_idx ) { lastmonth_start = thismonth_start; thismonth_start = curr; }
    if( year_idx  != last_year_idx  ) { lastyear_start  = thisyear_start;  thisyear_start  = curr; }
  }

  last_day_idx   = day_idx;
  last_week_idx  = week_idx;
  last_month_idx = month_idx;
  last_year_idx  = year_idx;
}

void format_kwh( char *buf, size_t bufsize, uint64_t one_tenth_wh ) {
  snprintf(buf, bufsize, "%.4f", one_tenth_wh / 10000.0);
}

// Shared chunk of buffer for HTML composition (one HTTP request at a time)
static char web_buf[1536];

static const char css_block[] PROGMEM =
  "<style>"
  "*{box-sizing:border-box}"
  "body{margin:0;font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;"
       "background:#0d1117;color:#c9d1d9;line-height:1.5}"
  "header{background:#161b22;padding:1rem;border-bottom:1px solid #30363d}"
  "h1{margin:0;font-size:1.25rem;color:#58a6ff;font-weight:600}"
  "h1 .muted{font-size:.85rem;font-weight:400;margin-left:.5rem}"
  "nav{background:#161b22;padding:.5rem 1rem;border-bottom:1px solid #30363d;"
      "display:flex;gap:.5rem;flex-wrap:wrap;align-items:center}"
  "nav a,nav button{background:#21262d;color:#c9d1d9;border:1px solid #30363d;"
                   "padding:.5rem .9rem;border-radius:6px;text-decoration:none;"
                   "font-size:.9rem;cursor:pointer;font-family:inherit}"
  "nav a:hover,nav button:hover{background:#30363d;border-color:#58a6ff}"
  "nav a.active{background:#1f6feb;border-color:#1f6feb;color:#fff}"
  "nav .right{margin-left:auto}"
  "main{max-width:920px;margin:0 auto;padding:1rem}"
  ".card{background:#161b22;border:1px solid #30363d;border-radius:8px;"
        "padding:1rem;margin-bottom:1rem}"
  ".card h2{margin:0 0 .75rem;font-size:.8rem;color:#8b949e;font-weight:600}"
  ".cap{text-transform:uppercase;letter-spacing:.05em}"
  "table{width:100%;border-collapse:collapse}"
  "th,td{padding:.55rem .5rem;text-align:right;border-bottom:1px solid #21262d;"
        "font-variant-numeric:tabular-nums}"
  "th:first-child,td:first-child{text-align:left}"
  "th{color:#8b949e;font-weight:600;font-size:.78rem}"
  "thead th{text-transform:uppercase;letter-spacing:.04em}"
  "tr:last-child td{border-bottom:none}"
  ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:.75rem}"
  ".stat{background:#0d1117;border:1px solid #30363d;border-radius:6px;padding:.75rem}"
  ".stat .label{font-size:.72rem;color:#8b949e;text-transform:uppercase;letter-spacing:.05em}"
  ".stat .value{font-size:1.5rem;font-weight:600;margin-top:.2rem;color:#58a6ff;"
               "font-variant-numeric:tabular-nums}"
  ".stat .unit{font-size:.85rem;color:#8b949e;font-weight:400;margin-left:.25rem}"
  ".stat .sub{font-size:.78rem;color:#8b949e;margin-top:.2rem}"
  ".muted{color:#8b949e}"
  ".pos{color:#3fb950}"
  ".neg{color:#f85149}"
  ".swatch{display:inline-block;width:1em;height:1em;border-radius:3px;"
          "border:1px solid #30363d;vertical-align:middle;margin-right:.4em}"
  "code{background:#0d1117;padding:.1rem .35rem;border-radius:3px;font-size:.85em;"
       "border:1px solid #30363d}"
  "footer{text-align:center;padding:1rem;color:#8b949e;font-size:.8rem}"
  "footer a{color:#8b949e}"
  "p{margin:.5rem 0}"
  "</style>";

void send_html_head( int status, const char *meta_refresh ) {
  web_server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  web_server.send(status, "text/html", "");
  web_server.sendContent_P(PSTR(
    "<!doctype html><html lang=\"en\"><head>"
    "<title>" PROGNAME " " HOSTNAME " v" VERSION "</title>"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<meta charset=\"utf-8\">"));
  if( meta_refresh && *meta_refresh ) {
    web_server.sendContent(meta_refresh);
  }
  web_server.sendContent_P(css_block);
  web_server.sendContent_P(PSTR(
    "</head><body>"
    "<header><h1>" PROGNAME
    " <span class=\"muted\">" HOSTNAME " &middot; v" VERSION "</span></h1></header>"));
}

void send_nav( const char *active ) {
  snprintf_P(web_buf, sizeof(web_buf), PSTR(
    "<nav>"
    "<a href=\"/\"%s>Home</a>"
    "<a href=\"/monitor\"%s>Monitor</a>"
    "<a href=\"/json\" target=\"_blank\">JSON</a>"
    "<a href=\"/sml\">SML</a>"
    "<form method=\"post\" action=\"/reset\" class=\"right\" "
    "onsubmit=\"return confirm('Reset device?');\">"
    "<button type=\"submit\">Reset</button>"
    "</form>"
    "</nav><main>"),
    strcmp(active, "home")    == 0 ? " class=\"active\"" : "",
    strcmp(active, "monitor") == 0 ? " class=\"active\"" : "");
  web_server.sendContent(web_buf);
}

void send_html_foot() {
  web_server.sendContent_P(PSTR(
    "</main><footer>" PROGNAME " v" VERSION " &middot; "
    "<a href=\"/update\">OTA update</a>"
    "</footer></body></html>"));
  web_server.sendContent("");
}

void emit_period_row( const char *label, const char *id, const period_t *cur, const period_t *base ) {
  char in_buf[24]  = "&mdash;";
  char out_buf[24] = "&mdash;";
  if( cur->valid && base->valid && cur->aPlus >= base->aPlus ) {
    format_kwh(in_buf, sizeof(in_buf), cur->aPlus - base->aPlus);
  }
  if( cur->valid && base->valid && cur->aMinus >= base->aMinus ) {
    format_kwh(out_buf, sizeof(out_buf), cur->aMinus - base->aMinus);
  }
  snprintf(web_buf, sizeof(web_buf),
    "<tr><th>%s</th><td id=\"%s_in\">%s</td><td id=\"%s_out\">%s</td></tr>",
    label, id, in_buf, id, out_buf);
  web_server.sendContent(web_buf);
}

// Smoothed live power tracker. Updated only on validated SML readings, and
// only when the integration window is at least POWER_WINDOW_S seconds long
// so quantization noise from 1/10 Wh deltas doesn't make the displayed W jump.
struct power_state_t {
  uint32_t uptime;
  uint64_t aPlus;
  uint64_t aMinus;
  uint64_t aPlusW;   // 1/10 W
  uint64_t aMinusW;  // 1/10 W
};

static power_state_t live_power = {0, 0, 0, 0, 0};
static const uint32_t POWER_WINDOW_S = 30;

// Last meter readings from a validated SML message. Used for display so a
// partial or rejected message that lands in the global itron doesn't make
// the live values flicker to 0.
static uint64_t latest_aPlus  = 0;
static uint64_t latest_aMinus = 0;
static bool     meter_seen    = false;

void update_power() {
  if( itron.valid != 0x3f ) return;  // skip rejected/incomplete readings
  if( live_power.uptime == 0 ) {
    live_power.uptime = itron.uptime;
    live_power.aPlus  = itron.aPlus;
    live_power.aMinus = itron.aMinus;
    return;
  }
  if( itron.aPlus < live_power.aPlus || itron.aMinus < live_power.aMinus ) {
    return;  // safety net: sml_data should have caught this; skip to avoid underflow
  }
  uint32_t dt = itron.uptime - live_power.uptime;
  if( dt >= POWER_WINDOW_S ) {
    live_power.aPlusW  = (itron.aPlus  - live_power.aPlus)  * 3600 / dt;
    live_power.aMinusW = (itron.aMinus - live_power.aMinus) * 3600 / dt;
    // Only reset anchor when window exceeds 2x minimum — this keeps the window
    // growing (and updating the display on every reading) rather than resetting
    // to zero on each chunk boundary, which causes 0/72W toggling at low power.
    if( dt >= 2 * POWER_WINDOW_S ) {
      live_power.uptime = itron.uptime;
      live_power.aPlus  = itron.aPlus;
      live_power.aMinus = itron.aMinus;
    }
  }
}

void send_power_card() {
  snprintf(web_buf, sizeof(web_buf),
    "<div class=\"card\"><h2 class=\"cap\">Live Power</h2><div class=\"grid\">"
    "<div class=\"stat\"><div class=\"label\">Consumption (A+)</div>"
    "<div class=\"value\"><span id=\"ap_w\">%.1f</span><span class=\"unit\">W</span></div>"
    "<div class=\"sub\"><span id=\"ap_kwh\">%.4f</span> kWh</div></div>"
    "<div class=\"stat\"><div class=\"label\">Backfeed (A-)</div>"
    "<div class=\"value\"><span id=\"am_w\">%.1f</span><span class=\"unit\">W</span></div>"
    "<div class=\"sub\"><span id=\"am_kwh\">%.4f</span> kWh</div></div>"
    "</div></div>",
    live_power.aPlusW  / 10.0, latest_aPlus  / 10000.0,
    live_power.aMinusW / 10.0, latest_aMinus / 10000.0);
  web_server.sendContent(web_buf);
}

// JS poll: refresh live power, meter totals and (where present) period cells
// without reloading the page. Lives in PROGMEM, ~600 B.
static const char poll_script[] PROGMEM =
  "<script>"
  "async function r(){try{"
  "const ac=new AbortController();"
  "const t=setTimeout(()=>ac.abort(),5000);"
  "const x=await fetch('/api/stats',{cache:'no-store',signal:ac.signal});"
  "clearTimeout(t);"
  "if(!x.ok)return;"
  "const d=await x.json();"
  "const s=(id,v,n)=>{const e=document.getElementById(id);"
  "if(e)e.textContent=(v==null)?'\\u2014':v.toFixed(n);};"
  "s('ap_w',d.ap_w,1);s('am_w',d.am_w,1);"
  "s('ap_kwh',d.ap_kwh,4);s('am_kwh',d.am_kwh,4);"
  "['today','yesterday','thisweek','lastweek','thismonth','lastmonth','thisyear','lastyear']"
  ".forEach(p=>{s(p+'_in',d[p+'_in'],4);s(p+'_out',d[p+'_out'],4);});"
  "}catch(e){}finally{setTimeout(r,2000);}}"
  "r();"
  "</script>";

void send_main_page() {
  send_html_head(200, NULL);
  send_nav("home");
  send_power_card();

  // Status card
  char curr_time[40];
  time_t now;
  time(&now);
  strftime(curr_time, sizeof(curr_time), "%FT%T%Z", localtime(&now));

  char serial_str[40];
  char *serial = to_hex(itron.serial, sizeof(itron.serial), '-');
  strncpy(serial_str, serial, sizeof(serial_str) - 1);
  serial_str[sizeof(serial_str) - 1] = '\0';

  snprintf(web_buf, sizeof(web_buf),
    "<div class=\"card\"><h2>Status</h2><table>"
    "<tr><th>Meter</th><td>%3.3s &middot; <code>%s</code></td></tr>"
    "<tr><th>Detailed readings</th><td>%s</td></tr>"
    "<tr><th>InfluxDB</th><td><code>" INFLUX_SERVER "</code> status %d</td></tr>"
    "<tr><th>Last update</th><td>%s</td></tr>"
    "</table></div>",
    itron.id, serial_str,
    recv_detailed ? "<span class=\"pos\">yes (1/10 Wh)</span>"
                  : "<span class=\"neg\">no (kWh only)</span>",
    influx_status, curr_time);
  web_server.sendContent(web_buf);

#ifdef DTU_TOPIC
  if( curr_limit != UINT16_MAX ) {
    snprintf(web_buf, sizeof(web_buf),
      "<div class=\"card\"><h2>Inverter</h2><table>"
      "<tr><th>Name</th><td>%s</td></tr>"
      "<tr><th>Reachable</th><td>%s</td></tr>"
      "<tr><th>Limit mode</th><td>%s</td></tr>"
      "<tr><th>Current limit</th><td>%u W</td></tr>"
      "</table></div>",
      inverter,
      reachable ? "<span class=\"pos\">yes</span>" : "<span class=\"neg\">no</span>",
      dynamic   ? "dynamic" : "static",
      curr_limit);
  }
  else {
    snprintf(web_buf, sizeof(web_buf),
      "<div class=\"card\"><h2>Inverter</h2>"
      "<div class=\"muted\">Waiting for MQTT data on "
      "<code>" DTU_TOPIC "/" INVERTER_SERIAL "</code></div></div>");
  }
  web_server.sendContent(web_buf);
#endif

#ifdef WLED_LEDS
  uint32_t now_ms = millis();
  if( (wled_r || wled_g || wled_b) && (now_ms - wled_update) >= (wled_secs * 1000) ) {
    wled_change += wled_secs * 1000;
    wled_r = 0;
    wled_g = 0;
    wled_b = 0;
  }
  uint32_t color_int = ((uint32_t)wled_r << 16) | ((uint32_t)wled_g << 8) | wled_b;
  snprintf(web_buf, sizeof(web_buf),
    "<div class=\"card\"><h2>WLED</h2><table>"
    "<tr><th>Host</th><td><code>" WLED_HOST ":%d</code> &middot; %d LEDs</td></tr>"
    "<tr><th>Color</th><td><span class=\"swatch\" style=\"background:#%06x\"></span>"
    "<code>#%06x</code></td></tr>"
    "<tr><th>Stable for</th><td>%u s</td></tr>"
    "</table></div>",
    WLED_PORT, WLED_LEDS,
    color_int, color_int,
    (now_ms - wled_change) / 1000);
  web_server.sendContent(web_buf);
#endif

  web_server.sendContent_P(poll_script);
  send_html_foot();
}

// Helper: emit one period's "_in/_out" key-value pair as kWh number or null
static size_t emit_period_json( char *buf, size_t bufsize, const char *key,
                                const period_t *cur, const period_t *base ) {
  size_t pos = 0;
  if( cur->valid && base->valid && cur->aPlus >= base->aPlus ) {
    pos += snprintf(buf + pos, bufsize - pos, ",\"%s_in\":%.4f",
                    key, (cur->aPlus - base->aPlus) / 10000.0);
  } else {
    pos += snprintf(buf + pos, bufsize - pos, ",\"%s_in\":null", key);
  }
  if( cur->valid && base->valid && cur->aMinus >= base->aMinus ) {
    pos += snprintf(buf + pos, bufsize - pos, ",\"%s_out\":%.4f",
                    key, (cur->aMinus - base->aMinus) / 10000.0);
  } else {
    pos += snprintf(buf + pos, bufsize - pos, ",\"%s_out\":null", key);
  }
  return pos;
}

// Define web pages for update, reset or for event infos
void setup_webserver() {
  // AJAX-friendly stats endpoint (separate from the stable /json API).
  // Returns instant power, meter totals (kWh) and per-period consumption (kWh, or null).
  web_server.on("/api/stats", []() {
    static char json[1024];
    size_t pos = 0;
    pos += snprintf(json + pos, sizeof(json) - pos,
      "{\"ap_w\":%.1f,\"am_w\":%.1f,\"ap_kwh\":%.4f,\"am_kwh\":%.4f",
      live_power.aPlusW  / 10.0, live_power.aMinusW / 10.0,
      latest_aPlus / 10000.0, latest_aMinus / 10000.0);

    period_t now_period = { latest_aPlus, latest_aMinus, meter_seen };
    pos += emit_period_json(json + pos, sizeof(json) - pos, "today",     &now_period,      &today_start);
    pos += emit_period_json(json + pos, sizeof(json) - pos, "yesterday", &today_start,     &yesterday_start);
    pos += emit_period_json(json + pos, sizeof(json) - pos, "thisweek",  &now_period,      &thisweek_start);
    pos += emit_period_json(json + pos, sizeof(json) - pos, "lastweek",  &thisweek_start,  &lastweek_start);
    pos += emit_period_json(json + pos, sizeof(json) - pos, "thismonth", &now_period,      &thismonth_start);
    pos += emit_period_json(json + pos, sizeof(json) - pos, "lastmonth", &thismonth_start, &lastmonth_start);
    pos += emit_period_json(json + pos, sizeof(json) - pos, "thisyear",  &now_period,      &thisyear_start);
    pos += emit_period_json(json + pos, sizeof(json) - pos, "lastyear",  &thisyear_start,  &lastyear_start);
    snprintf(json + pos, sizeof(json) - pos, "}");
    web_server.send(200, "application/json", json);
  });

  // Stable JSON API: keep field names and ordering (consumed by external scripts)
  web_server.on("/json", []() {
    static const char fmt[] = "{\n"
                              " \"meta\": {\n"
                              "  \"device\": \"" HOSTNAME "\",\n"
                              "  \"program\": \"" PROGNAME "\",\n"
                              "  \"version\": \"" VERSION "\",\n"
                              "  \"started\": \"%s\",\n"
                              "  \"posted\": \"%s\",\n"
                              "  \"received\": \"%s\"\n"
                              " },\n"
                              " \"energy\": {\n"
                              "  \"id\": \"%3.3s\",\n"
                              "  \"serial\": \"%s\",\n"
                              "  \"detailed\": \"%s\",\n"
                              "  \"uptime\": %u,\n"
                              "  \"aplus\": %.1f,\n"
                              "  \"aminus\": %.1f\n"
                              " }\n"
                              "}\n";
    static char msg[sizeof(fmt) + 3 * 22 + 30 + 4 * 10];
    static char inf_time[30];
    static char rec_time[30];
    strftime(inf_time, sizeof(inf_time), "%FT%T%Z", localtime(&post_time));
    strftime(rec_time, sizeof(rec_time), "%FT%T%Z", localtime(&recv_time));
    char *serial = to_hex(itron.serial, sizeof(itron.serial), '-');
    snprintf(msg, sizeof(msg), fmt, start_time, inf_time, rec_time,
             itron.id, serial, recv_detailed ? "yes" : "no", itron.uptime, itron.aPlus/10.0, itron.aMinus/10.0);
    web_server.send(200, "application/json", msg);
  });

  // Download last raw SML record (binary)
  web_server.on("/sml", []() {
    web_server.send(200, "application/octet-stream", sml_raw, sml_len);
  });

  // Reset the ESP (POST only)
  web_server.on("/reset", HTTP_POST, []() {
    syslog.log(LOG_NOTICE, "RESET");
    send_html_head(200, "<meta http-equiv=\"refresh\" content=\"7; url=/\">");
    send_nav("");
    web_server.sendContent_P(PSTR(
      "<div class=\"card\"><h2>Reset</h2>"
      "<p>Resetting device&hellip; you will be redirected back to the home page in a few seconds.</p>"
      "</div>"));
    send_html_foot();
    delay(200);
    ESP.restart();
  });

  // Live monitor with current power and per-period consumption (kWh)
  web_server.on("/monitor", []() {
    send_html_head(200, NULL);
    send_nav("monitor");
    send_power_card();

    period_t now_period = { latest_aPlus, latest_aMinus, meter_seen };
    bool all_known = meter_seen
      && today_start.valid && yesterday_start.valid
      && thisweek_start.valid && lastweek_start.valid
      && thismonth_start.valid && lastmonth_start.valid
      && thisyear_start.valid && lastyear_start.valid;
    web_server.sendContent_P(PSTR(
      "<div class=\"card\"><h2><span class=\"cap\">Consumption</span> (kWh)</h2>"
      "<table><thead><tr><th>Period</th><th>Used (A+)</th><th>Fed (A-)</th></tr></thead><tbody>"));
    emit_period_row("Today",      "today",     &now_period,      &today_start);
    emit_period_row("Yesterday",  "yesterday", &today_start,     &yesterday_start);
    emit_period_row("This week",  "thisweek",  &now_period,      &thisweek_start);
    emit_period_row("Last week",  "lastweek",  &thisweek_start,  &lastweek_start);
    emit_period_row("This month", "thismonth", &now_period,      &thismonth_start);
    emit_period_row("Last month", "lastmonth", &thismonth_start, &lastmonth_start);
    emit_period_row("This year",  "thisyear",  &now_period,      &thisyear_start);
    emit_period_row("Last year",  "lastyear",  &thisyear_start,  &lastyear_start);
    if( all_known ) {
      web_server.sendContent_P(PSTR("</tbody></table></div>"));
    }
    else {
      web_server.sendContent_P(PSTR(
        "</tbody></table>"
        "<p class=\"muted\" style=\"font-size:.8rem\">"
        "A dash (&mdash;) means no baseline value is available yet for that period."
        "</p></div>"));
    }

    web_server.sendContent_P(poll_script);
    send_html_foot();
  });

  // Index page
  web_server.on("/", []() {
    send_main_page();
  });

  // 404
  web_server.onNotFound([]() {
    send_html_head(404, NULL);
    send_nav("");
    web_server.sendContent_P(PSTR(
      "<div class=\"card\"><h2>404 Not Found</h2>"
      "<p class=\"muted\">The page you requested does not exist.</p>"
      "</div>"));
    send_html_foot();
  });

  web_server.begin();

  MDNS.addService("http", "tcp", WEBSERVER_PORT);
  syslog.logf(LOG_NOTICE, "Serving HTTP on port %d", WEBSERVER_PORT);
}

void setup() {
  WiFi.mode(WIFI_STA);
  WiFi.hostname(HOSTNAME);

  pinMode(DB_LED_PIN, OUTPUT);
  digitalWrite(DB_LED_PIN, DB_LED_ON);
  analogWriteRange(PWMRANGE);

  Serial.begin(SERIAL_SPEED);
  Serial.println("\nStarting " PROGNAME " v" VERSION " " __DATE__ " " __TIME__);

  mirror.begin(SERIAL_SPEED, EspSoftwareSerial::SWSERIAL_8N1, -1, IR_LED_PIN, true);

  // Syslog setup
  syslog.server(SYSLOG_SERVER, SYSLOG_PORT);
  syslog.deviceHostname(HOSTNAME);
  syslog.appName("Joba1");
  syslog.defaultPriority(LOG_KERN);

  digitalWrite(DB_LED_PIN, DB_LED_OFF);

  WiFiManager wm;
  // wm.resetSettings();
  if (!wm.autoConnect()) {
    Serial.println("Failed to connect WLAN");
    for (int i = 0; i < 1000; i += 200) {
      digitalWrite(DB_LED_PIN, DB_LED_ON);
      delay(100);
      digitalWrite(DB_LED_PIN, DB_LED_OFF);
      delay(100);
    }
    ESP.restart();
    while (true)
      ;
  }

  digitalWrite(DB_LED_PIN, DB_LED_ON);
  char msg[80];
  snprintf(msg, sizeof(msg), "%s Version %s, WLAN IP is %s", PROGNAME, VERSION,
           WiFi.localIP().toString().c_str());
  Serial.printf(msg);
  syslog.logf(LOG_NOTICE, msg);

  ntp.begin();

  MDNS.begin(HOSTNAME);

  esp_updater.setup(&web_server);
  setup_webserver();

#ifdef DTU_TOPIC
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(mqtt_callback);
#endif

  // Test wled status info
  // itron.valid = 0x3f;
  // for( uint16_t w = 0; w < 900; w++ ) {
  //   itron.uptime += 3600;
  //   itron.aMinus += w;
  //   send_wled();
  //   delay(50);
  // }
  // itron.uptime += 3600;
  // itron.aPlus += 5000;
  // send_wled();
  // itron.uptime = 0;
  // itron.aPlus = 0;
  // itron.aMinus = 0;
  // itron.valid = 0x0;

  last_counter_reset = millis();
}

bool check_ntptime() {
  static bool have_time = false;
  if (!have_time && ntp.getEpochTime()) {
    have_time = true;
    time_t now = time(NULL);
    strftime(start_time, sizeof(start_time), "%FT%T%Z", localtime(&now));
    syslog.logf(LOG_NOTICE, "Booted at %s", start_time);
  }
  return have_time;
}

void breathe() {
  static uint32_t start = 0;
  static uint32_t min_duty = PWMRANGE / 10; // limit min brightness
  static uint32_t max_duty = PWMRANGE / 2; // limit max brightness
  static uint32_t prev_duty = 0;

  uint32_t now = millis();
  uint32_t elapsed = now - start;
  if (elapsed > breathe_interval) {
    start = now;
    elapsed -= breathe_interval;
  }

  uint32_t duty = (max_duty - min_duty) * elapsed * 2 / breathe_interval + min_duty;
  if (duty > max_duty) {
    duty = 2 * max_duty - duty;
  }

  duty = duty * duty / max_duty;

  if (duty != prev_duty) {
    prev_duty = duty;
    analogWrite(DB_LED_PIN, PWMRANGE - duty);
  }
}

typedef enum { SML_NONE=0, SML_OPEN=0x0101, SML_LIST=0x0701, SML_CLOSE=0x0201 } sml_message_t;

// Validate meter reading is within configured limits
// Compares power calculated from energy delta against max thresholds
// Returns false if power exceeds limits
// A+ = consumption from grid (use USAGE_KW_MAX)
// A- = production/feed-in to grid (use PROD_KW_MAX)
bool is_power_valid( uint64_t current_reading_1_10Wh, uint64_t previous_reading_1_10Wh, uint32_t delta_time_s, bool is_aplus ) {
  if( delta_time_s == 0 ) return true;  // skip validation on first reading
  
  // Calculate power in W from energy delta over time
  // (reading1 - reading0) * (1/10 Wh) / time_h * 3600 = power_W
  // = (reading1 - reading0) * 360 / time_s
  uint64_t power_W = (current_reading_1_10Wh - previous_reading_1_10Wh) * 360 / delta_time_s;
  
  uint32_t max_power_W = is_aplus ? USAGE_KW_MAX * 1000 : PROD_KW_MAX * 1000;
  
  return power_W <= max_power_W;
}

char *itronString( itron_3hz_t *itron ) {
  static char msg[200];

  char *serial = to_hex(itron->serial, sizeof(itron->serial), '-');
  serial[sizeof(itron->serial) * 3 - 1] = '\0'; // cut last separator 

  snprintf(msg, sizeof(msg), 
    "valid[0x3f]=0x%02x, detailed=%s, id='%3.3s', serial='%s', record=%llu, uptime[s]=%u, A+[Wh]=%.1f, A-[Wh]=%.1f",
    itron->valid, itron->detailed ? "true" : "false", itron->id, serial, itron->file, itron->uptime, itron->aPlus/10.0, itron->aMinus/10.0);

  return msg;
}

uint64_t pow10( uint64_t val, int8_t exp ) {
  if( exp < 0 ) {
    while( exp++ ) {
      val /= 10;
    }
  }
  else {
    while( exp-- ) {
      val *= 10;
    }
  }
  return val;
}

/*
Parse relevant data from Itron 3.Hz meter
 itron: pointer to structure with relevant values
 level: list level
 pos:   in current sml value structure
 type:  data type (0, 4, 5, 6 from SML)
 data:  data of given type or 0 for end marker
 */
void parse_itron_3hz( itron_3hz_t *itron, size_t level, size_t pos, size_t type, const void *data ) {
  static bool fileOpen = false;
  static sml_message_t messageType = SML_NONE;
  static bool isMeterId = false;
  static bool isMeterSerial = false;
  static bool isMeterAplus = false;
  static bool isMeterAminus = false;
  static uint8_t unit = 0;
  static int8_t scale = 0;
  
  if( level == 2 && pos == 0 && type == 6 ) {  // SML message type
    messageType = (sml_message_t)*(uint64_t *)data;
    if( messageType == SML_OPEN ) {
      fileOpen = true;
    }
    else if( messageType == SML_CLOSE ) {
      fileOpen = false;
    }
  }
  else if( messageType == SML_OPEN && level == 3 && pos == 2 && type == 0 ) {  // file id
    size_t len = sizeof(uint64_t);
    uint8_t *record = (uint8_t *)data;
    while( len-- ) {
      itron->file = (itron->file << 8) | *(record++);
    }
    itron->valid |= 4;
  }
  else if( fileOpen && messageType == SML_LIST ) {
    if( level == 4 && pos == 1 && type == 6 ) {  // uptime
      itron->uptime = *(uint64_t *)data;
      itron->valid |= 8;
    }
    else if( level == 5 ) {  // SML value structure
      if( pos == 0 && type == 0 ) {  // obis id
        char *obis = (char *)data;
        if( obis[0] == 0x01 && obis[1] == 0 ) {
          if( obis[2] == 0x60 && obis[3] == 0x32 && obis[4] == 0x01) {
            isMeterId = true;
          }
          else if( obis[2] == 0x60 && obis[3] == 0x01 && obis[4] == 0x00) {
            isMeterSerial = true;
          }
          else if( obis[2] == 0x01 && obis[3] == 0x08 && obis[4] == 0x00) {
            isMeterAplus = true;
          }
          else if( obis[2] == 0x02 && obis[3] == 0x08 && obis[4] == 0x00) {
            isMeterAminus = true;
          }
        }
      }
      else if( (isMeterAplus || isMeterAminus) && pos == 3 && type == 6 ) {  // unit
        unit = *(uint64_t *)data;
      }
      else if( (isMeterAplus || isMeterAminus) && pos == 4 && type == 5 ) {  // scale
        scale = *(int64_t *)data;
        // scale ==  3: coarse kWh readings after power failure
        // scale == -1: fine 1/10Wh readings (needs itr pin and menu setting)
        itron->detailed = (scale == 3) ? false : true;
      }
      else if( pos == 5 ) {  // SML value
        if( isMeterId && type == 0 ) {  // meter id
          memcpy(itron->id, data, sizeof(itron->id));
          itron->valid |= 1;
          isMeterId = false;
        }
        else if( isMeterSerial && type == 0 ) {  // meter serial
          memcpy(itron->serial, data, sizeof(itron->serial));
          itron->valid |= 2;
          isMeterSerial = false;
        }
        else if( isMeterAplus && type == 6 ) {  // A+ value
          if( unit == 30 ) {  // expecting [Wh]
            itron->aPlus = pow10(*(uint64_t *)data, scale+1);
            itron->valid |= 16;
          }
          unit = 0;
          scale = 0;
          isMeterAplus = false;
        }
        else if( isMeterAminus && type == 6 ) {  // A- value
          if( unit == 30 ) {  // expecting [Wh]
            itron->aMinus = pow10(*(uint64_t *)data, scale+1);
            itron->valid |= 32;
          }
          unit = 0;
          scale = 0;
          isMeterAminus = false;
        }
      }
    }
  }
}

char *spaces(char *s, size_t indent) {
  while( indent-- ) {
    *(s++) = ' ';
    *(s++) = ' ';
  }
  return s;
}

/*
SML parser (assuming valid SML 1.x)
 itron: pointer to structure to store relevant values
 data:  sml data of unknown length (whole record or list)
 items: list items (or high number if unknown)
 level: of nested lists
 */
char *read_sml( itron_3hz_t *itron, char *data, size_t items, size_t level ) {
  static char msg[1024];
  size_t pos = 0;

  while( items-- ) {
    size_t type = (*data >> 4) & 0x7;
    
    size_t len = *data & 0xf;
    while( *(data++) & 0x80 ) {
      len = (len << 4) + (*data & 0xf);
    }

    //size_t l = len;  // for syslog length
    uint64_t u = 0;
    int64_t i = 0;
    char *s;

    switch( type ) {
      case 0:  // octet
        if( len == 0 ) {
          parse_itron_3hz(itron, level, pos, type, 0);
          // print end
          s = spaces(msg, level);
          snprintf(s, 4, "end");
          //syslog.logf(LOG_DEBUG, "Sml[%2u,%2u,%2u]=%s\n", pos, type, l, msg);
          return data;
        }
        else {
          parse_itron_3hz(itron, level, pos, type, data);
          if( --len == 0 ) {
            // print default
            s = spaces(msg, level);
            snprintf(s, 8, "default");
            //syslog.logf(LOG_DEBUG, "Sml[%2u,%2u,%2u]=%s\n", pos, type, l, msg);
          } 
          else {
            // print string(len)
            s = spaces(msg, level);
            while( len-- ) {
              snprintf(s, 4, "%02x ", *(data++));
              s += 3;
            }
            //syslog.logf(LOG_DEBUG, "Sml[%2u,%2u,%2u]=%s\n", pos, type, l, msg);
          }
        }
        break;
      case 4:  // bool
        parse_itron_3hz(itron, level, pos, type, data);
        if( *(data++) ) {
          // print true
          s = spaces(msg, level);
          snprintf(s, 5, "true");
          //syslog.logf(LOG_DEBUG, "Sml[%2u,%2u,%2u]=%s\n", pos, type, l, msg);
        } else {
          // print false
          s = spaces(msg, level);
          snprintf(s, 6, "false");
          //syslog.logf(LOG_DEBUG, "Sml[%2u,%2u,%2u]=%s\n", pos, type, l, msg);
        }
        break;
      case 5:  // int
        while( len-- >= 2 ) {
          i = (i << 8) | *(data++);
        }
        parse_itron_3hz(itron, level, pos, type, &i);
        // print i
        s = spaces(msg, level);
        snprintf(s, 20, "%lld", i);
        //syslog.logf(LOG_DEBUG, "Sml[%2u,%2u,%2u]=%s\n", pos, type, l, msg);
        break;
      case 6:  // unsigned int
        while (len-- >= 2) {
          u = (u << 8) | *(data++);
        }
        parse_itron_3hz(itron, level, pos, type, &u);
        // print u
        s = spaces(msg, level);
        snprintf(s, 20, "%llu", u);
        //syslog.logf(LOG_DEBUG, "Sml[%2u,%2u,%2u]=%s\n", pos, type, l, msg);
        break;
      case 7:  // list
        s = spaces(msg, level);
        snprintf(s, 17, "list[%u]", len);
        //syslog.logf(LOG_DEBUG, "Sml[%2u,%2u,%2u]=%s\n", pos, type, l, msg);
        data = read_sml(itron, data, len, level + 1);
        break;
    }
    pos++;
  }
  return data;
}

void sml_data( char *data, size_t len ) {
  static const uint32_t max_count = 60;  // send ~once per minute
  static uint32_t count = max_count;
  static uint32_t last_uptime = 0;
  static uint64_t last_aPlus = 0;
  static uint64_t last_aMinus = 0;
  static uint8_t  backwards_count = 0;

  sml_len = min(len, (size_t)sizeof(sml_raw));
  memcpy(sml_raw, data, sml_len);
  
  memset(&itron, 0, sizeof(itron));
  read_sml(&itron, data, 0xffff, 0);
  if( itron.valid == 0x3f ) {
    recv_time = time(NULL);
    recv_detailed = itron.detailed;
    
    // Validate readings are within configured power limits
    if( last_uptime > 0 ) {
      if( itron.aPlus < last_aPlus || itron.aMinus < last_aMinus ) {
        // Backwards counter: could be a bit-error (common, single occurrence) or
        // a genuine meter reset after power loss (sustained run of backwards values).
        // Only declare a reset after 5 consecutive backwards readings so isolated
        // read errors are silently dropped without disturbing the baselines.
        if( ++backwards_count >= 5 ) {
          syslog.logf(LOG_WARNING,
            "Meter reset confirmed after %u backwards readings, new baseline A+=%llu A-=%llu",
            backwards_count, itron.aPlus, itron.aMinus);
          last_uptime = itron.uptime;
          last_aPlus  = itron.aPlus;
          last_aMinus = itron.aMinus;
          memset(&live_power, 0, sizeof(live_power));
          backwards_count = 0;
        }
        itron.valid = 0;  // always discard the backwards reading itself
      }
      else {
        backwards_count = 0;
        uint32_t delta_time_s = itron.uptime - last_uptime;
        if( delta_time_s > 0 ) {
          bool valid = true;
          if( !is_power_valid(itron.aPlus, last_aPlus, delta_time_s, true) ) {
            syslog.logf(LOG_WARNING, "Rejected: A+ delta=%llu in %u s exceeds USAGE_KW_MAX=%u",
                        itron.aPlus - last_aPlus, delta_time_s, USAGE_KW_MAX);
            valid = false;
          }
          if( !is_power_valid(itron.aMinus, last_aMinus, delta_time_s, false) ) {
            syslog.logf(LOG_WARNING, "Rejected: A- delta=%llu in %u s exceeds PROD_KW_MAX=%u",
                        itron.aMinus - last_aMinus, delta_time_s, PROD_KW_MAX);
            valid = false;
          }
          if( !valid ) {
            itron.valid = 0;
          }
        }
      }
    }
    
    // Store current values for next comparison (only if reading was valid)
    if( itron.valid == 0x3f ) {
      last_uptime = itron.uptime;
      last_aPlus = itron.aPlus;
      last_aMinus = itron.aMinus;
      latest_aPlus = itron.aPlus;
      latest_aMinus = itron.aMinus;
      meter_seen = true;
      update_period_baselines();
      update_power();
    }
  }

  count++;
  if( count > max_count ) {
    count = 0;
    if( itron.valid == 0x3f ) {  // all bits/entries set: publish itron data
      post_data();
      #ifdef DTU_TOPIC
      publish_data();
      #endif
      if( recv_detailed ) {
        syslog.logf(LOG_INFO, "Itron %s", itronString(&itron));
      }
      else {
        syslog.logf(LOG_WARNING, "Itron %s", itronString(&itron));
      }
    }
    else {
      syslog.logf(LOG_NOTICE, "Sml[%u]=%s", len, to_hex(data, len, ','));
      syslog.logf(LOG_NOTICE, "Itron invalid: %s", itronString(&itron));
    }
  }

  #ifdef DTU_TOPIC
  check_limit();
  #endif

  #ifdef WLED_LEDS
  send_wled();
  #endif
}

typedef enum { MODE_NONE, MODE_START, MODE_VER, MODE_DATA, MODE_END, MODE_FINISH } read_mode_t;

void read_serial_sml() {
  static char data[2560];  // enough for 2s at 9600 baud
  static read_mode_t mode = MODE_NONE;
  static size_t count = 0;
  int ch;

  while( (ch = Serial.read()) >= 0 ) {
    // Mirror all incoming data to IR LED output
    mirror.write(ch);
    
    switch( mode ) {
      case MODE_NONE:
        if( ch == 0x1b ) {
          mode = MODE_START;
          count = 1;
        }
        break;
      case MODE_START:
        if( ch == 0x1b ) {
          if( ++count == 4 ) {
            mode = MODE_VER;
            count = 0;
          }
        }
        else {
          mode = MODE_NONE;
        }
        break;
      case MODE_VER:
        if (ch == 0x01) {
          if (++count == 4) {
            mode = MODE_DATA;
            counter_events++;  // reset inactivity counter
            count = 0;
          }
        } else {
          mode = MODE_NONE;
        }
        break;
      case MODE_DATA:
        data[count++] = ch;
        if( count % 4 == 1 && ch == 0x1b ) {
          mode = MODE_END;
        }
        break;
      case MODE_END:
        data[count++] = ch;
        if( count % 4 != 0 && ch != 0x1b ) {
          mode = MODE_DATA;
        }
        else if (count % 4 == 0 ) {
          mode = MODE_FINISH;
          count -= 4;
        }
        break;
      case MODE_FINISH:
        if (ch == 0x1a) {
          sml_data(data, count);
        }
        mode = MODE_NONE;
        break;
    }
  }
}

void loop() {
  static uint32_t updateDelay = 0;
  static bool smlRead = false;

  ntp.update();
  if (check_ntptime()) {
    breathe();
  }
  else { // delay reading sml (for OTA update if reading sml causes reboots)
    updateDelay = millis();
  }

  if( millis() - updateDelay > 20000 ) { 
    smlRead = true;
  }

  if( smlRead ) {
    read_serial_sml();
  }

#ifdef DTU_TOPIC
  handle_mqtt();
#endif

  web_server.handleClient();
  delay(1);
}
