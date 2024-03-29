# Energy Meter Gateway with ESP8266

* Receive messages from IR serial interface and post them on syslog and influx database
* Optionally send power status (feeding to grid or high load) to WLED with UDP
* Optionally set limit of an OpenDTU inverter via MQTT to avoid high feed to grid

## Hardware

* Wemos Mini D1 ESP8266
* IR LED (940nm) as receiver
* NPN transistor (e.g. BC546)

```
GND -- -_LED_+ -- NPN_B
GND -- E_NPN_C -- R_10k -- V_3.3
NPN_C -- GPIO_Rx
```
## Grafana Dashboard

![image](https://user-images.githubusercontent.com/32450554/144091536-94630249-3fab-48d6-807d-f92a7e7a44a1.png)

## WLED Status

Enabled if WLED_LEDS is #defined
If there is either a very high power consumption or power is fed back to the grid, switch on a color coded wled via UDP packets
Uses the WLED protocol DRGB.
The wled status can be seen on the main web page.

## Dynamic OpenDTU Inverter Limit

Enabled if DTU_TOPIC is #defined (Topic must match what you used in your OpenDTU firmware). Also set INVERTER_SERIAL, e.g. by adapting inverter_template.ini to your inverters serial and set MQTT topic DTU_TOPIC/INVERTER_SERIAL/status/limit_dynamic to 1.

If enabled, it will try to avoid high backfeed to the grid by adjusting the OpenDTU production power limit of your inverter.
Seems to work fine with my TSOL-M800. Should work with all inverters where you can adjust the limit with MQTT topic DTU_TOPIC/INVERTER_SERIAL/cmd/limit_nonpersistent_absolute.
The status of the dynamic limit can be seen on the main web page.

## SML Messages
Example message from my itron electricity meter
```
//0 1   2   3   4   5  level   
76,
    09, ae,01,00,00, 00,10,b6,88,
    62, 00,
    62, 00,
    72,
        65, 00,00,01,01,  // SML open()
        76,
            01,
            01,
            09, 00,00,00,00, 00,05,93,db,
            0b, 0a,01,49,54, 52,xx,xx,xx, xx,xx,
            72,
                62, 01,
                65, 00,05,93,dc,
            01,
    63, f5,44,
    00,
76,
    09, ae,01,00,00, 00,10,b6,89,
    62, 00,
    62, 00,
    72,
        65, 00,00,07,01,  // SML get_list()
        77,
            01,
            0b, 0a,01,49,54, 52,xx,xx,xx, xx,xx,
            07, 01,00,62,0a, ff,ff,
            72,
                62, 01,
                65, 00,05,93,dc,
            74,
                77,
                    07, 01,00,60,32, 01,01,  // obis meter id
                    01,
                    01,
                    01,
                    01,
                    04, 49,54,52,  // "ITR"
                    01,
                77,
                    07, 01,00,60,01, 00,ff,  // 07=6 bytes obis serial no
                    01,  // 01=no data, means default
                    01,
                    01,
                    01,
                    0b, 0a,01,49,54, 52,xx,xx,xx, xx,xx,
                    01,
                77,
                    07, 01,00,01,08, 00,ff,  // obis A+
                    65, 00,1c,01,04,
                    01,
                    62, 1e,  // 6=unsigned, 2=itemLen -> byte data: 0x1e=30 = unit
                    52, 03,  // 5=signed: 0x03=3 = scale
                    69, 00,00,00,00, 00,00,00,5a,  // 6=unsigned, 9=itemLen -> dword: 0x5a
                    01,
                77,
                    07, 01,00,02,08, 00,ff,  // obis A-
                    01,
                    01,
                    62, 1e,
                    52, 03,
                    69, 00,00,00,00, 00,00,00,00,
                    01,
            01,
            01,
    63, 3c,dc, 
    00,
76,
    09, ae,01,00,00, 00,10,b6,8a,
    62, 00,
    62, 00,
    72,
        65, 00,00,02,01,  // SML close()
        71,
            01,
    63, 67,a9,
    00,
00,00,
```

## Documentation
* http://www.schatenseite.de/2016/05/30/smart-message-language-stromzahler-auslesen/ 
* https://www.bsi.bund.de/SharedDocs/Downloads/DE/BSI/Publikationen/TechnischeRichtlinien/TR03109/TR-03109-1_Anlage_Feinspezifikation_Drahtgebundene_LMN-Schnittstelle_Teilb.pdf?__blob=publicationFile
* https://www.edi-energy.de/index.php?id=38&tx_bdew_bdew%5Buid%5D=1158&tx_bdew_bdew%5Baction%5D=download&tx_bdew_bdew%5Bcontroller%5D=Dokument&cHash=4ee767287a68d3f0fbbc3caf2974d97f
* https://www.itron.com/-/media/feature/products/documents/brochure/openway_kl.pdf
* https://www.devolo.ch/fileadmin/Web-Content/DE/products/sg/3hz-basiszaehler/documents/de/Handbuch_devolo_3.HZ_Basiszaehler_D3B60_D3B100_0419_Online_DE.pdf
