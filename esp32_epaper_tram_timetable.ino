#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <U8g2_for_Adafruit_GFX.h>
#include "Secrets.h"
#include "ErrorBitmaps.h"
//#include "bitmaps/Bitmaps400x300.h" // 4.2"  b/w
//#include "Bitmaps.h"

//https://github.com/ZinggJM/GxEPD2/blob/master/examples/GxEPD2_WS_ESP32_Driver/GxEPD2_WS_ESP32_Driver.ino
//https://github.com/ZinggJM/GxEPD2/blob/master/examples/GxEPD2_U8G2_Fonts_Example/GxEPD2_U8G2_Fonts_Example.ino

// mapping of Waveshare ESP32 Driver Board
// BUSY -> 25, RST -> 26, DC -> 27, CS-> 15, CLK -> 13, DIN -> 14

// NOTE: this board uses "unusual" SPI pins and requires re-mapping of HW SPI to these pins in SPIClass
//       this example shows how this can be done easily, updated for use with HSPI
//
// The Wavehare ESP32 Driver Board uses uncommon SPI pins for the FPC connector. It uses HSPI pins, but SCK and MOSI are swapped.
// To use HW SPI with the ESP32 Driver Board, HW SPI pins need be re-mapped in any case. Can be done using either HSPI or VSPI.
// Other SPI clients can either be connected to the same SPI bus as the e-paper, or to the other HW SPI bus, or through SW SPI.
// The logical configuration would be to use the e-paper connection on HSPI with re-mapped pins, and use VSPI for other SPI clients.
// VSPI with standard VSPI pins is used by the global SPI instance of the Arduino IDE ESP32 package.

// uncomment next line to use HSPI for EPD (and VSPI for SD), e.g. with Waveshare ESP32 Driver Board
#define USE_HSPI_FOR_EPD
#define GxEPD2_DISPLAY_CLASS GxEPD2_BW
#define GxEPD2_DRIVER_CLASS GxEPD2_420     // GDEW042T2   400x300, UC8176 (IL0398), (WFT042CZ15)
#define GxEPD2_BW_IS_GxEPD2_BW true
#define IS_GxEPD(c, x) (c##x)
#define IS_GxEPD2_BW(x) IS_GxEPD(GxEPD2_BW_IS_, x)
#define MAX_DISPLAY_BUFFER_SIZE 65536ul
#define MAX_HEIGHT(EPD) (EPD::HEIGHT <= MAX_DISPLAY_BUFFER_SIZE / (EPD::WIDTH / 8) ? EPD::HEIGHT : MAX_DISPLAY_BUFFER_SIZE / (EPD::WIDTH / 8))

GxEPD2_DISPLAY_CLASS<GxEPD2_DRIVER_CLASS, MAX_HEIGHT(GxEPD2_DRIVER_CLASS)> display(GxEPD2_DRIVER_CLASS(/*CS=*/ 15, /*DC=*/ 27, /*RST=*/ 26, /*BUSY=*/ 25));
SPIClass hspi(HSPI);
U8G2_FOR_ADAFRUIT_GFX u8g2Fonts;

// date and time
char date[11];
char current_time[6];

// weather info
char weather_call[128];
char weather_info[32];

// tram timetables
const char* transport_call = "https://api.digitransit.fi/routing/v1/routers/hsl/index/graphql";
char transport_req[416];

const int max_timetable_offset_s = 14400; // 4h - display departures max s in the future
const int max_buffer = 68;

DynamicJsonDocument weather_doc(1024);
DynamicJsonDocument query_doc(1024);
DynamicJsonDocument transport_doc(2048);

struct Tram {
  char short_name[4];
  char full_name[max_buffer];
  char timetable[max_buffer];
};

HTTPClient http;

void setup()
{
  Serial.begin(115200);

  // special handling for Waveshare ESP32 Driver board
  hspi.begin(13, 12, 14, 15); // remap hspi for EPD (swap pins)
  display.epd2.selectSPI(hspi, SPISettings(4000000, MSBFIRST, SPI_MODE0));

  initWiFi();

  // configure time
  const char* ntp_server = "pool.ntp.org";
  const long gmt_offset_s = 7200;
  const int daylight_offset_s = 3600;
  configTime(gmt_offset_s, daylight_offset_s, ntp_server);

  // set up requests
  sprintf(weather_call, "https://api.openweathermap.org/data/2.5/weather?lat=%s&lon=%s&appid=%s&units=metric", SECRETS_LAT, SECRETS_LON, SECRETS_APP_ID);
  sprintf(
    transport_req,
    "{stop1: stop(id: \"%s\"){name code routes {shortName longName}stoptimesWithoutPatterns(numberOfDepartures: 30) {scheduledDeparture realtimeDeparture serviceDay headsign trip {route {shortName}}}},stop2: stop(id: \"%s\") {name code routes {shortName longName}stoptimesWithoutPatterns(numberOfDepartures: 30) {scheduledDeparture realtimeDeparture serviceDay headsign trip {route {shortName}}}}}",
    SECRETS_STOP1,
    SECRETS_STOP2
  );

  // set up display
  display.init(115200);
  u8g2Fonts.begin(display); // connect u8g2 procedures to Adafruit GFX
  // first update should be full refresh
  display.setFullWindow();
  display.setRotation(0);
  u8g2Fonts.setFontMode(1);                   // use u8g2 transparent mode (this is default)
  u8g2Fonts.setFontDirection(0);             // left to right (this is default)
  u8g2Fonts.setForegroundColor(GxEPD_BLACK);  // apply Adafruit GFX color
  u8g2Fonts.setBackgroundColor(GxEPD_WHITE);  // apply Adafruit GFX color
  u8g2Fonts.setFont(u8g2_font_profont12_tf); // 8px height; select u8g2 font from here: https://github.com/olikraus/u8g2/wiki/fntlistall u8g2_font_luRS10_tf u8g2_font_unifont_tf

  Serial.println("Setup done");
}

void loop()
{
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wifi not connected");
    //display_error(epd_bitmap_wifi_off_48);
    return;
  }

  // get time and date
  struct tm timeinfo;
  getLocalTime(&timeinfo);
  const char* datetime = asctime(&timeinfo);
  //get date: Www Mmm dd
  snprintf(date, 11, "%s", datetime);
  //get time: hh:mm
  snprintf(current_time, 6, "%s", datetime + 11);
  // current time
  time_t timestamp = mktime(&timeinfo);
 
  // fetch weather data
  http.begin(weather_call);
  int weather_res = http.GET();
  String weather_json = http.getString();
  http.end();
  bool weather_error = deserializeJson(weather_doc, weather_json) && weather_res != 200;

  if (weather_error) {
    Serial.println("Error in getting weather info");
    Serial.println(weather_res);
    return;
  }

  // get float temps, round, prep to display
  JsonObject main = weather_doc["main"];
  int int_temp = round_temp(main["temp"]);
  int int_feels_like = round_temp(main["feels_like"]);
  sprintf(weather_info, "%d°C (%d°C)", int_temp, int_feels_like);

  // get tram timetables
  // form query as json
  query_doc["query"] = transport_req;
  char query[768];
  serializeJson(query_doc, query);

  http.begin(transport_call);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Cache-Control", "no-cache");
  http.addHeader("digitransit-subscription-key", SECRETS_KEY);
  int transport_res = http.POST(query);
  String transport_json = http.getString();
  http.end();

  bool transport_error = deserializeJson(transport_doc, transport_json) && transport_res != 200;

  if (transport_error) {
    Serial.println("Error in getting transport info");
    Serial.println(transport_res);
    return;
  }

  // set vars for printing
  JsonObject stop1 = transport_doc["data"]["stop1"];
  JsonObject stop2 = transport_doc["data"]["stop2"];

  JsonArray stop1_stop_times = stop1["stoptimesWithoutPatterns"];
  JsonArray stop2_stop_times = stop2["stoptimesWithoutPatterns"];
  int stop1_size = stop1_stop_times.size();
  int stop2_size = stop2_stop_times.size();
  int stoptime_counts[] = {stop1_size, stop2_size};

  JsonArray stop1_routes = stop1["routes"];
  JsonArray stop2_routes = stop2["routes"];
  int stop1_route_count = stop1_routes.size();
  int stop2_route_count = stop2_routes.size();
  int route_counts[] = {stop1_route_count, stop2_route_count};

  // create structs
  struct Tram trams[8];
  int tram_count = 0;

 // RAW "stoptimesWithoutPatterns": [{"scheduledDeparture":78060,"realtimeDeparture":78060,"serviceDay":1713301200,"headsign":"Place via place","trip":{"route":{"shortName":"1"}}}, ...]

  for (int stop = 0; stop < 2; stop++) {
    JsonObject current_stop = stop == 0 ? stop1 : stop2;

    for (int i = 0; i < stoptime_counts[stop]; i++) {
      //get departure time
      int departure = current_stop["stoptimesWithoutPatterns"][i]["realtimeDeparture"];
      int day = current_stop["stoptimesWithoutPatterns"][i]["serviceDay"];
      time_t dep_timestamp = (time_t) departure + day;
      struct tm* timeinfo = localtime(&dep_timestamp);
      
      if ((int)dep_timestamp - (int)timestamp > max_timetable_offset_s) {
        break; // don't display tram timetables too far in the future
      }

      const char* short_name = current_stop["stoptimesWithoutPatterns"][i]["trip"]["route"]["shortName"];
      // check if struct for tram already created
      int existsAt = -1;

      for (int j = 0; j < tram_count; j++) {
        Serial.println(short_name);
        if (strcmp(trams[j].short_name, short_name) == 0) {
          existsAt = j;
          break;
        }
      }

      // create new tram struct
      if (existsAt < 0) {
        struct Tram new_tram;
        sprintf(new_tram.short_name, "%s", short_name);

        for (int k = 0; k < route_counts[stop]; k++) {
          const char* name = current_stop["routes"][k]["shortName"];
          if(strcmp(name, short_name) == 0) {
            //get full route name (delete spaces between dashes from long name due to line length)
            const char* long_name = current_stop["routes"][k]["longName"];
            char full_name[max_buffer];
            int char_count = 0;
            int l = 0;
            while(long_name[l] != '\0') {
              if (long_name[l] != ' ' || (long_name[l-1] != '-' && long_name[l+1] != '-')) {
                full_name[char_count++] = long_name[l];
              }
              l++;
            }
            full_name[char_count] = '\0';
            snprintf(new_tram.full_name, max_buffer, "%s (%s)", short_name, full_name);
            Serial.println(new_tram.full_name);
            break;
          }
        }

        //add new tram with one departure time
        sprintf(new_tram.timetable, "%02d:%02d ", timeinfo->tm_hour, timeinfo->tm_min);
        trams[tram_count] = new_tram;
        tram_count++;
        Serial.println(tram_count);

      } else {
        if ((strlen(trams[existsAt].timetable) + 7) <= max_buffer) {
          //add time to existing tram struct
          sprintf(trams[existsAt].timetable + strlen(trams[existsAt].timetable), "%02d:%02d ", timeinfo->tm_hour, timeinfo->tm_min);
        }
      }
    }
  }

  // sort trams from stop1->from soonest departing, stop2->from soonest departing to alphabetical
  qsort(trams, tram_count, sizeof(Tram), compare);
  
  display.firstPage();

  do
  {
    const uint16_t line_height = 16; // font size * 2
    const uint16_t margin = 5; // screen width pixels are off
    uint16_t line_number = 1;

    /* display pattern:

    date  time  temp(feels like temp)

    tram no. (route description)
    time time ...
    
    repeat trams */

    display.fillScreen(GxEPD_WHITE);
    
    //date
    u8g2Fonts.setCursor(0, line_number * line_height);
    u8g2Fonts.print(date);
    
    //center time, text box bounds
    int16_t tbx, tby;
    uint16_t tbw, tbh;
    display.getTextBounds(current_time, 0, 0, &tbx, &tby, &tbw, &tbh);
    int16_t x_time = ((display.width() - tbw) / 2) - tbx;
    u8g2Fonts.setCursor(x_time, line_number * line_height);
    u8g2Fonts.print(current_time);

    //temperature
    display.getTextBounds(weather_info, 0, 0, &tbx, &tby, &tbw, &tbh);
    int16_t x_weather = display.width() - tbw - tbx - margin;
    u8g2Fonts.setCursor(x_weather, line_number * line_height);
    u8g2Fonts.print(weather_info);

    line_number += 2;

    //print all trams with departure times
    for (int i = 0; i < tram_count; i++) {
      u8g2Fonts.setCursor(0, line_number * line_height);
      u8g2Fonts.println(trams[i].full_name);

      line_number++;

      u8g2Fonts.setCursor(0, line_number * line_height);
      u8g2Fonts.println(trams[i].timetable);

      line_number += 2;
    }

    display.display();
  }
  while (display.nextPage());
  
  // repeat in 30s
  display.hibernate();
  delay(30000);
}

void initWiFi()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(SECRETS_SSID, SECRETS_PWD);
  if (WiFi.status() != WL_CONNECTED) {
    delay(500);
  } 
}

void display_error(const unsigned char* error_bitmap)
{
  uint16_t x = display.width() / 2 - 24;
  uint16_t y = display.height() / 2 - 24;
  const char msg[] = "network error...";
  display.setFullWindow();
  display.firstPage();

  do {
    display.setRotation(0);
    display.setFont(&FreeMonoBold9pt7b);
    display.setTextColor(GxEPD_BLACK);
    display.fillScreen(GxEPD_WHITE);
    display.setCursor(x, y);
    display.print(msg);
    display.drawBitmap(x, y, error_bitmap, 48, 48, GxEPD_WHITE, GxEPD_BLACK);
  }
  while (display.nextPage());
  delay(30000);
}

int round_temp(float temp)
{
  return (temp < 0) ? (temp - 0.5) : (temp + 0.5);
}

int compare(const void* a, const void* b)
{
  Tram *tramA = (Tram *)a;
  Tram *tramB = (Tram *)b;
  return strcmp(tramA->short_name, tramB->short_name);
}

void deepSleepTest()
{
  //Serial.println("deepSleepTest");
  const char hibernating[] = "hibernating ...";
  const char wokeup[] = "woke up";
  const char from[] = "from deep sleep";
  const char again[] = "again";
  display.setRotation(1);
  display.setFont(&FreeMonoBold9pt7b);
  display.setTextColor(GxEPD_BLACK);
  int16_t tbx, tby; uint16_t tbw, tbh;
  // center text
  display.getTextBounds(hibernating, 0, 0, &tbx, &tby, &tbw, &tbh);
  uint16_t x = ((display.width() - tbw) / 2) - tbx;
  uint16_t y = ((display.height() - tbh) / 2) - tby;
  display.setFullWindow();
  display.firstPage();
  do
  {
    display.fillScreen(GxEPD_WHITE);
    display.setCursor(x, y);
    display.print(hibernating);
  }
  while (display.nextPage());
  display.hibernate();
  delay(5000);
  display.getTextBounds(wokeup, 0, 0, &tbx, &tby, &tbw, &tbh);
  uint16_t wx = (display.width() - tbw) / 2;
  uint16_t wy = (display.height() / 3) + tbh / 2; // y is base line!
  display.getTextBounds(from, 0, 0, &tbx, &tby, &tbw, &tbh);
  uint16_t fx = (display.width() - tbw) / 2;
  uint16_t fy = (display.height() * 2 / 3) + tbh / 2; // y is base line!
  display.firstPage();
  do
  {
    display.fillScreen(GxEPD_WHITE);
    display.setCursor(wx, wy);
    display.print(wokeup);
    display.setCursor(fx, fy);
    display.print(from);
  }
  while (display.nextPage());
  delay(5000);
  display.getTextBounds(hibernating, 0, 0, &tbx, &tby, &tbw, &tbh);
  uint16_t hx = (display.width() - tbw) / 2;
  uint16_t hy = (display.height() / 3) + tbh / 2; // y is base line!
  display.getTextBounds(again, 0, 0, &tbx, &tby, &tbw, &tbh);
  uint16_t ax = (display.width() - tbw) / 2;
  uint16_t ay = (display.height() * 2 / 3) + tbh / 2; // y is base line!
  display.firstPage();
  do
  {
    display.fillScreen(GxEPD_WHITE);
    /*display.setCursor(hx, hy);
    display.print(hibernating);
    display.setCursor(ax, ay);
    display.print(again);*/
  }
  while (display.nextPage());
  delay(10000);
  display.hibernate();
  //Serial.println("deepSleepTest done");
}



void drawBitmaps()
{
  display.setFullWindow();

#ifdef _GxBitmaps400x300_H_
  drawBitmaps400x300();
#endif

}

#ifdef _GxBitmaps400x300_H_
void drawBitmaps400x300()
{
#if !defined(__AVR)
  const unsigned char* bitmaps[] =
  {
    Bitmap400x300_1, Bitmap400x300_2
  };
#else
  const unsigned char* bitmaps[] = {}; // not enough code space
#endif
  if (display.epd2.panel == GxEPD2::GDEW042T2)
  {
    for (uint16_t i = 0; i < sizeof(bitmaps) / sizeof(char*); i++)
    {
      display.firstPage();
      do
      {
        display.fillScreen(GxEPD_WHITE);
        display.drawInvertedBitmap(0, 0, bitmaps[i], display.epd2.WIDTH, display.epd2.HEIGHT, GxEPD_BLACK);
      }
      while (display.nextPage());
      delay(2000);
    }
  }
}
#endif

