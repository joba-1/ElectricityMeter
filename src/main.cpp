#include <Arduino.h>

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

#ifndef PWMRANGE
#define PWMRANGE 1023
#endif

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(*(a)))

#define DB_LED_PIN D4
#define IR_LED_PIN D1

#define DB_LED_ON LOW
#define DB_LED_OFF HIGH
#define IR_LED_ON HIGH
#define IR_LED_OFF LOW

#define WEBSERVER_PORT 80

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
  uint64_t aPlus;
  uint64_t aMinus;
} itron_3hz_t;

itron_3hz_t itron = {0};
time_t recv_time = 0;

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

  char fmt[] = "energy,meter=%s watt=%llu,watt_out=%llu\n";
  char msg[sizeof(fmt) + 30 + 2 * 10];
  char *serial = to_hex(itron.serial, sizeof(itron.serial), '-');
  snprintf(msg, sizeof(msg), fmt, serial, itron.aPlus, itron.aMinus);

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

// Define web pages for update, reset or for event infos
void setup_webserver() {
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
                              "  \"uptime\": %u,\n"
                              "  \"aplus\": %llu,\n"
                              "  \"aminus\": %llu\n"
                              " }\n"
                              "}\n";
    static char msg[sizeof(fmt) + 2 * 20 + 5 * 10];
    static char inf_time[30];
    static char rec_time[30];
    strftime(inf_time, sizeof(inf_time), "%FT%T%Z", localtime(&post_time));
    strftime(rec_time, sizeof(rec_time), "%FT%T%Z", localtime(&recv_time));
    char *serial = to_hex(itron.serial, sizeof(itron.serial), '-');
    snprintf(msg, sizeof(msg), fmt, start_time, inf_time, rec_time,
             itron.id, serial, itron.uptime, itron.aPlus, itron.aMinus);
    web_server.send(200, "application/json", msg);
  });

  // download last raw SML record
  web_server.on("/sml", []() {
    web_server.send(200, "application/octet-stream", sml_raw, sml_len);
  });

  // Call this page to reset the ESP
  web_server.on("/reset", HTTP_POST, []() {
    syslog.log(LOG_NOTICE, "RESET");
    web_server.send(200, "text/html",
                    "<html>\n"
                    " <head>\n"
                    "  <title>" PROGNAME " v" VERSION "</title>\n"
                    "  <meta http-equiv=\"refresh\" content=\"7; url=/\"> \n"
                    " </head>\n"
                    " <body>Resetting...</body>\n"
                    "</html>\n");
    delay(200);
    ESP.restart();
  });

  // Standard page
  static const char fmt[] =
      "<html>\n"
      " <head>\n"
      "  <title>" PROGNAME " v" VERSION "</title>\n"
      "  <meta http-equiv=\"expires\" content=\"5\">\n"
      " </head>\n"
      " <body>\n"
      "  <h1>" PROGNAME " v" VERSION "</h1>\n"
      "  <table><tr>\n"
      "   <td><form action=\"json\">\n"
      "    <input type=\"submit\" name=\"json\" value=\"JSON\" />\n"
      "   </form></td>\n"
      "   <td><form action=\"sml\">\n"
      "    <input type=\"submit\" name=\"sml\" value=\"SML\" />\n"
      "   </form></td>\n"
      "   <td><form action=\"reset\" method=\"post\">\n"
      "    <input type=\"submit\" name=\"reset\" value=\"Reset\" />\n"
      "   </form></td>\n"
      "  </tr></table>\n"
      "  <div>Post firmware image to /update<div>\n"
      "  <div>Influx status: %d<div>\n"
      " </body>\n"
      "</html>\n";
  static char page[sizeof(fmt) + 10] = "";

  // Index page
  web_server.on("/", []() {
    snprintf(page, sizeof(page), fmt, influx_status);
    web_server.send(200, "text/html", page);
  });

  // Catch all page
  web_server.onNotFound([]() {
    snprintf(page, sizeof(page), fmt, influx_status);
    web_server.send(404, "text/html", page);
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

void on_interval_elapsed(uint32_t elapsed, uint32_t counts) {
  if ( !counts ) {
    syslog.logf(LOG_NOTICE, "No SML record since %u seconds", elapsed / 1000);
  }
}

void check_events() {
  static const uint32_t max_interval = 600 * 1000; // ms to wait for Serial Rx

  uint32_t now = millis();
  uint32_t elapsed = now - last_counter_reset;
  if (counter_events || elapsed >= max_interval) {
    uint32_t events = counter_events;

    counter_events = 0;
    last_counter_reset += max_interval;

    // either Serial Rx happened or log interval elapsed
    on_interval_elapsed(elapsed, events);
  }
}

void breathe() {
  static uint32_t start = 0;
  static uint32_t max_duty = PWMRANGE / 2; // limit max brightness
  static uint32_t prev_duty = 0;

  uint32_t now = millis();
  uint32_t elapsed = now - start;
  if (elapsed > breathe_interval) {
    start = now;
    elapsed -= breathe_interval;
  }

  uint32_t duty = max_duty * elapsed * 2 / breathe_interval;
  if (duty > max_duty) {
    duty = 2 * max_duty - duty;
  }

  duty = duty * duty / max_duty;

  if (duty != prev_duty) {
    prev_duty = duty;
    analogWrite(DB_LED_PIN, PWMRANGE - duty);
  }
}

typedef struct config {
  SerialConfig bits;
  char name[4];
} config_t;

config_t configs[] = {
  { SERIAL_5N1, "5N1" },
  { SERIAL_6N1, "6N1" },
  { SERIAL_7N1, "7N1" },
  { SERIAL_8N1, "8N1" },
  { SERIAL_5N2, "5N2" },
  { SERIAL_6N2, "6N2" },
  { SERIAL_7N2, "7N2" },
  { SERIAL_8N2, "8N2" },
  { SERIAL_5E1, "5E1" },
  { SERIAL_6E1, "6E1" },
  { SERIAL_7E1, "7E1" },
  { SERIAL_8E1, "8E1" },
  { SERIAL_5E2, "5E2" },
  { SERIAL_6E2, "6E2" },
  { SERIAL_7E2, "7E2" },
  { SERIAL_8E2, "8E2" },
  { SERIAL_5O1, "501" },
  { SERIAL_6O1, "601" },
  { SERIAL_7O1, "701" },
  { SERIAL_8O1, "801" },
  { SERIAL_5O2, "502" },
  { SERIAL_6O2, "602" },
  { SERIAL_7O2, "7O2" },
  { SERIAL_8O2, "8O2" }
};

long speeds[] = {
  110, 300, 600, 1200, 2400, 4800, 9600, 14400, 19200, 38400, 57600, 115200, 128000, 256000
};

void check_serial() {
  static uint32_t changed = 0;
  static size_t config = ARRAY_SIZE(configs);
  static size_t speed = ARRAY_SIZE(speeds);
  static size_t pos = 0;
  static char msg[160+1];

  // check for speed or config change
  uint32_t now = millis();
  if( now - changed > 4000 ) {
    changed = now;

    if( pos > 0 ) {
      msg[pos] = '\0';
      syslog.logf(LOG_INFO, "Msg=%s\n", msg);
      syslog.logf(LOG_INFO, "Hex=%s\n", to_hex(msg, pos, ' '));
      pos = 0;
    }
    config++;
    if( config >= ARRAY_SIZE(configs) ) {
      config = 0;
      speed++;
      if( speed >= ARRAY_SIZE(speeds) ) {
        speed = 0;
      }
    }
    syslog.logf(LOG_INFO, "Speed=%ld, Config=%s\n", speeds[speed], configs[config].name);
    Serial.end();
    Serial.begin(speeds[speed], configs[config].bits);
  }

  // try reading from serial
  int ch = Serial.read();
  if( ch >= 0 ) {
    counter_events++;
    msg[pos++] = ch;
    if( pos >= sizeof(msg) - 1 ) {
      msg[pos] = '\0';
      syslog.logf(LOG_INFO, "Msg=%s\n", msg);
      syslog.logf(LOG_INFO, "Hex=%s\n", to_hex(msg, pos, ' '));
      pos = 0;
    }
  }
}

typedef enum { SML_NONE=0, SML_OPEN=0x0101, SML_LIST=0x0701, SML_CLOSE=0x0201 } sml_message_t;

char *itronString( itron_3hz_t *itron ) {
  static char msg[200];

  char *serial = to_hex(itron->serial, sizeof(itron->serial), '-');
  serial[sizeof(itron->serial) * 3 - 1] = '\0'; // cut last separator 

  snprintf(msg, sizeof(msg), 
    "valid[0x3f]=0x%02x, id='%3.3s', serial='%s', record=%llu, uptime[s]=%u, A+[Wh]=%llu, A-[Wh]=%llu",
    itron->valid, itron->id, serial, itron->file, itron->uptime, itron->aPlus, itron->aMinus);

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
          if( unit == 30 ) { // expecting [Wh]
            itron->aPlus = pow10(*(uint64_t *)data, scale);
            itron->valid |= 16;
          }
          unit = 0;
          scale = 0;
          isMeterAplus = false;
        }
        else if( isMeterAminus && type == 6 ) {  // A- value
          if( unit == 30 ) { // expecting [Wh]
            itron->aMinus = pow10(*(uint64_t *)data, scale);
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

  sml_len = min(len, (size_t)sizeof(sml_raw));
  memcpy(sml_raw, data, sml_len);
  
  memset(&itron, 0, sizeof(itron));
  read_sml(&itron, data, 0xffff, 0);
  if( itron.valid == 0x3f ) {
    recv_time = time(NULL);
  }

  count++;
  if( count > max_count ) {
    count = 0;
    if( itron.valid == 0x3f ) {  // all bits/entries set: publish itron data
      post_data();
      syslog.logf(LOG_INFO, "Itron %s", itronString(&itron));
    }
    else {
      syslog.logf(LOG_NOTICE, "Sml[%u]=%s", len, to_hex(data, len, ','));
      syslog.logf(LOG_NOTICE, "Itron invalid: %s", itronString(&itron));
    }
  }
}

typedef enum { MODE_NONE, MODE_START, MODE_VER, MODE_DATA, MODE_END, MODE_FINISH } read_mode_t;

void read_serial_sml() {
  static char data[2560];  // enough for 2s at 9600 baud
  // static char raw[1024];
  static read_mode_t mode = MODE_NONE;
  static size_t count = 0;
  // static size_t len = 0;
  int ch;

  while( (ch = Serial.read()) >= 0 ) {
    // if( len ) {
    //   raw[len++] = ch;
    // }
    switch( mode ) {
      case MODE_NONE:
        if( ch == 0x1b ) {
          mode = MODE_START;
          //syslog.logf(LOG_INFO, "NONE->START");
          count = 1;
          // if( len ) {
          //   syslog.logf(LOG_INFO, "Raw[%u]=%s", len-1, to_hex(raw, len-1));
          //   len = 0;
          // }
          // else {
          //   raw[len++] = ch;
          // }
        }
        break;
      case MODE_START:
        if( ch == 0x1b ) {
          if( ++count == 4 ) {
            mode = MODE_VER;
            //syslog.logf(LOG_INFO, "START->VER");
            count = 0;
          }
        }
        else {
          //syslog.logf(LOG_INFO, "START->NONE");
          mode = MODE_NONE;
        }
        break;
      case MODE_VER:
        if (ch == 0x01) {
          if (++count == 4) {
            //syslog.logf(LOG_INFO, "VER->DATA");
            mode = MODE_DATA;
            counter_events++;  // reset inactivity counter
            count = 0;
          }
        } else {
          //syslog.logf(LOG_INFO, "VER->NONE");
          mode = MODE_NONE;
        }
        break;
      case MODE_DATA:
        data[count++] = ch;
        if( count % 4 == 1 && ch == 0x1b ) {
          //syslog.logf(LOG_INFO, "DATA->END");
          mode = MODE_END;
        }
        break;
      case MODE_END:
        data[count++] = ch;
        if( count % 4 != 0 && ch != 0x1b ) {
          //syslog.logf(LOG_INFO, "END->DATA");
          mode = MODE_DATA;
        }
        else if (count % 4 == 0 ) {
          //syslog.logf(LOG_INFO, "END->FINISH");
          mode = MODE_FINISH;
          count -= 4;
        }
        break;
      case MODE_FINISH:
        if (ch == 0x1a) {
          sml_data(data, count);
        }
        //syslog.logf(LOG_INFO, "FINISH->NONE");
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

  //check_serial();
  //check_events();
  web_server.handleClient();
  delay(1);
}
