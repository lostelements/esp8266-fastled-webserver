/*
   ESP8266 + FastLED + IR Remote + MSGEQ7: https://github.com/jasoncoon/esp8266-fastled-webserver
   Copyright (C) 2015 Jason Coon

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#define FASTLED_ESP8266_RAW_PIN_ORDER
#include "FastLED.h"
FASTLED_USING_NAMESPACE

extern "C" {
#include "user_interface.h"
}

#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager
#include <FS.h>
#include <EEPROM.h>
#include "GradientPalettes.h"
#include <ArduinoJson.h>          //https://github.com/bblanchon/ArduinoJson
#include <PubSubClient.h> //on Lostelemnts Git

const bool apMode = false;

// AP mode password
const char WiFiAPPSK[] = "";

// Wi-Fi network to connect to (if not in AP mode)
const char* ssid =  "BTHub5-SZR2";    // Your  ssid cannot be longer than 32 characters!
const char* password =  "d4b2efef95";   // Your hub password

const char* mdns_hostname = "ledsign";

IPAddress ipmos(192, 168, 1, 76); // IP address of Mosquitto server should be loaded from Spiffs later
//define set name of your sign
String signname = "Room1"; //should be loaded from spiffs
//define mqtt message names
String thissign = "ledsign\\" + signname;
String allsigns = "ledsign\\all";


ESP8266WebServer server(80);

#define DATA_PIN      D5     
#define LED_TYPE      WS2812B
#define COLOR_ORDER   GRB
// Set your number of leds here!
#define NUM_LEDS      8

#define EEPROM_BRIGHTNESS      0
#define EEPROM_PATTERN      1
#define EEPROM_SOLID_R      2
#define EEPROM_SOLID_G      3
#define EEPROM_SOLID_B      4
#define EEPROM_PALETTE      5
#define EEPROM_LIT      6
#define EEPROM_BIG      7

#define MILLI_AMPS         1500     // IMPORTANT: set here the max milli-Amps of your power supply 5V 2A = 2000
#define AP_POWER_SAVE 	   1   // Set to 0 if you do not want the access point to shut down after 10 minutes of unuse
#define FRAMES_PER_SECOND  120 // here you can control the speed. With the Access Point / Web Server the animations run a bit slower.

CRGB leds[NUM_LEDS];
int lit = NUM_LEDS;

uint8_t patternIndex = 0;

const uint8_t brightnessCount = 5;
uint8_t brightnessMap[brightnessCount] = { 16, 32, 64, 128, 255 };
int brightnessIndex = 0;
uint8_t brightness = brightnessMap[brightnessIndex];

#define ARRAY_SIZE(A) (sizeof(A) / sizeof((A)[0]))

// ten seconds per color palette makes a good demo
// 20-120 is better for deployment
#define SECONDS_PER_PALETTE 10

///////////////////////////////////////////////////////////////////////

// Forward declarations of an array of cpt-city gradient palettes, and
// a count of how many there are.  The actual color palette definitions
// are at the bottom of this file.
extern const TProgmemRGBGradientPalettePtr gGradientPalettes[];
extern const uint8_t gGradientPaletteCount;

// Current palette number from the 'playlist' of color palettes
uint8_t gCurrentPaletteNumber = 0;

CRGBPalette16 gCurrentPalette( CRGB::Black);
CRGBPalette16 gTargetPalette( gGradientPalettes[0] );

uint8_t currentPatternIndex = 0; // Index number of which pattern is current
uint8_t currentPaletteIndex = 0;
uint8_t gHue = 0; // rotating "base color" used by many of the patterns

CRGB solidColor = CRGB::Black;

uint8_t power = 1;
uint8_t glitter = 0;
uint8_t big = 0;  // Activate led 0 as a "special" always lit slightly vibrating led, regardless of pattern

void setup(void) {
  Serial.begin(115200);
  delay(100);
  Serial.setDebugOutput(true);

  FastLED.addLeds<LED_TYPE, DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS);         // for WS2812 (Neopixel)
  //FastLED.addLeds<LED_TYPE,DATA_PIN,CLK_PIN,COLOR_ORDER>(leds, NUM_LEDS); // for APA102 (Dotstar)
  FastLED.setCorrection(TypicalLEDStrip);
  FastLED.setBrightness(brightness);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, MILLI_AMPS);
  fill_solid(leds, NUM_LEDS, solidColor);
  FastLED.show();

  EEPROM.begin(512);
  loadSettings();

  FastLED.setBrightness(brightness);

  Serial.println();
  Serial.print( F("Heap: ") ); Serial.println(system_get_free_heap_size());
  Serial.print( F("Boot Vers: ") ); Serial.println(system_get_boot_version());
  Serial.print( F("CPU: ") ); Serial.println(system_get_cpu_freq());
  Serial.print( F("SDK: ") ); Serial.println(system_get_sdk_version());
  Serial.print( F("Chip ID: ") ); Serial.println(system_get_chip_id());
  Serial.print( F("Flash ID: ") ); Serial.println(spi_flash_get_id());
  Serial.print( F("Flash Size: ") ); Serial.println(ESP.getFlashChipRealSize());
  Serial.print( F("Vcc: ") ); Serial.println(ESP.getVcc());
  Serial.println();

  SPIFFS.begin();
  {
    Dir dir = SPIFFS.openDir("/");
    while (dir.next()) {
      String fileName = dir.fileName();
      size_t fileSize = dir.fileSize();
      Serial.printf("FS File: %s, size: %s\n", fileName.c_str(), String(fileSize).c_str());
    }
    Serial.printf("\n");
  }
  
  WiFi.hostname(mdns_hostname);

  if (apMode) {
    WiFi.mode(WIFI_AP);

    // Do a little work to get a unique-ish name. Append the
    // last two bytes of the MAC (HEX'd) to "Thing-":
    uint8_t mac[WL_MAC_ADDR_LENGTH];
    WiFi.softAPmacAddress(mac);
    String macID = String(mac[WL_MAC_ADDR_LENGTH - 2], HEX) +
                   String(mac[WL_MAC_ADDR_LENGTH - 1], HEX);
    macID.toUpperCase();
    String AP_NameString = "Ledserver " + macID;

    char AP_NameChar[AP_NameString.length() + 1];
    memset(AP_NameChar, 0, AP_NameString.length() + 1);

    for (int i = 0; i < AP_NameString.length(); i++) {
      AP_NameChar[i] = AP_NameString.charAt(i);
    }

    WiFi.softAP(AP_NameChar, WiFiAPPSK);
    MDNS.begin(mdns_hostname);
    MDNS.addService("http", "tcp", 80);

    Serial.printf("Connect to Wi-Fi access point: %s\n", AP_NameChar);
    Serial.println("and open http://192.168.4.1 or http://" + String(mdns_hostname) + ".local in your browser");
  } else {
    WiFi.mode(WIFI_STA);
    Serial.printf("Connecting to %s\n", ssid);
    if (String(WiFi.SSID()) != String(ssid)) {
      Serial.println("I'm not sure what is going on here, but you need this message");
      WiFi.begin(ssid, password);
    }
    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
    }
    Serial.print("Connected! Open http://");
    Serial.print(WiFi.localIP());
    Serial.println(" in your browser");
  }

  //  server.serveStatic("/", SPIFFS, "/index.htm"); // ,"max-age=86400"

  server.on("/all", HTTP_GET, []() {
    sendAll();
  });

  server.on("/power", HTTP_GET, []() {
    sendPower();
  });

  server.on("/glitter", HTTP_GET, []() {
    sendGlitter();
  });

  server.on("/big", HTTP_GET, []() {
    sendBig();
  });
  
  server.on("/power", HTTP_POST, []() {
    String value = server.arg("value");
    setPower(value.toInt());
    sendPower();
  });

  server.on("/glitter", HTTP_POST, []() {
    String value = server.arg("value");
    setGlitter(value.toInt());
    sendGlitter();
  });

  server.on("/big", HTTP_POST, []() {
    String value = server.arg("value");
    setBig(value.toInt());
    sendBig();
  });

  server.on("/solidColor", HTTP_GET, []() {
    sendSolidColor();
  });

  server.on("/solidColor", HTTP_POST, []() {
    String r = server.arg("r");
    String g = server.arg("g");
    String b = server.arg("b");
    setSolidColor(r.toInt(), g.toInt(), b.toInt());
    sendSolidColor();
  });

  server.on("/pattern", HTTP_GET, []() {
    sendPattern();
  });

  server.on("/pattern", HTTP_POST, []() {
    String value = server.arg("value");
    setPattern(value.toInt());
    sendPattern();
  });
  
  server.on("/lit", HTTP_POST, []() {
    String value = server.arg("value");
    setLit(value.toInt());
    sendLit();
  });

  server.on("/brightness", HTTP_GET, []() {
    sendBrightness();
  });

  server.on("/brightness", HTTP_POST, []() {
    String value = server.arg("value");
    setBrightness(value.toInt());
    sendBrightness();
  });

  server.on("/palette", HTTP_GET, []() {
    sendPalette();
  });

  server.on("/palette", HTTP_POST, []() {
    String value = server.arg("value");
    setPalette(value.toInt());
    sendPalette();
  });

  server.serveStatic("/index.htm", SPIFFS, "/index.htm");
  server.serveStatic("/fonts", SPIFFS, "/fonts", "max-age=86400");
  server.serveStatic("/js", SPIFFS, "/js");
  server.serveStatic("/css", SPIFFS, "/css", "max-age=86400");
  server.serveStatic("/images", SPIFFS, "/images", "max-age=86400");
  server.serveStatic("/", SPIFFS, "/index.htm");

  server.begin();

  Serial.println("HTTP server started");
}

typedef void (*Pattern)();
typedef struct {
  Pattern pattern;
  String name;
} PatternAndName;
typedef PatternAndName PatternAndNameList[];

// List of patterns to cycle through.  Each is defined as a separate function below.
PatternAndNameList patterns = {
  { colorwaves, "Color Waves" },
  { palettetest, "Palette Test" },
  { pride, "Pride" },
  { rainbow, "Rainbow" },
  { rainbowWithGlitter, "Rainbow With Glitter" },
  { confetti, "Confetti" },
  { sinelon, "Sinelon" },
  { juggle, "Juggle" },
  { bpm, "BPM" },
  { jozef, "Jozef's pattern" },
  { police, "Da Police" },  
  { showSolidColor, "Solid Color" },
};

const uint8_t patternCount = ARRAY_SIZE(patterns);

typedef struct {
  CRGBPalette16 palette;
  String name;
} PaletteAndName;
typedef PaletteAndName PaletteAndNameList[];

const CRGBPalette16 palettes[] = {
  RainbowColors_p,
  RainbowStripeColors_p,
  CloudColors_p,
  LavaColors_p,
  OceanColors_p,
  ForestColors_p,
  PartyColors_p,
  HeatColors_p
};

const uint8_t paletteCount = ARRAY_SIZE(palettes);

const String paletteNames[paletteCount] = {
  "Rainbow",
  "Rainbow Stripe",
  "Cloud",
  "Lava",
  "Ocean",
  "Forest",
  "Party",
  "Heat",
};

void loop(void) {
  // Add entropy to random number generator; we use a lot of it.
  random16_add_entropy(random(65535));

  server.handleClient();

  if (power == 0) {
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    FastLED.show();
    FastLED.delay(15);
    return;
  }

  EVERY_N_MILLISECONDS( 20 ) {
    gHue++;  // slowly cycle the "base color" through the rainbow
  }

  // change to a new cpt-city gradient palette
  EVERY_N_SECONDS( SECONDS_PER_PALETTE ) {
    gCurrentPaletteNumber = addmod8( gCurrentPaletteNumber, 1, gGradientPaletteCount);
    gTargetPalette = gGradientPalettes[ gCurrentPaletteNumber ];
  }

  // slowly blend the current cpt-city gradient palette to the next
  EVERY_N_MILLISECONDS(40) {
    nblendPaletteTowardPalette( gCurrentPalette, gTargetPalette, 16);
  }

  
  EVERY_N_SECONDS( 600 ) {
    if(apMode && wifi_softap_get_station_num() == 0 && AP_POWER_SAVE) {
      WiFi.forceSleepBegin();
      Serial.println("Modem is sleeping now");
    }
  }
  
  // Call the current pattern function once, updating the 'leds' array
  patterns[currentPatternIndex].pattern();

  if(glitter) {
    addGlitter(3);
  }
  
  // If the big led (led 0) is activated
  if(big == 1) {
    leds[0] = CHSV(0,random(0,50),255);
  }


  FastLED.show();

  // insert a delay to keep the framerate modest
  FastLED.delay(1000 / FRAMES_PER_SECOND);
}


void loadSettings()
{
  brightness = EEPROM.read(EEPROM_BRIGHTNESS);

  currentPatternIndex = EEPROM.read(EEPROM_PATTERN);
  if (currentPatternIndex < 0)
    currentPatternIndex = 0;
  else if (currentPatternIndex >= patternCount)
    currentPatternIndex = patternCount - 1;

  byte r = EEPROM.read(EEPROM_SOLID_R);
  byte g = EEPROM.read(EEPROM_SOLID_G);
  byte b = EEPROM.read(EEPROM_SOLID_B);

  if (r == 0 && g == 0 && b == 0)
  {
  }
  else
  {
    solidColor = CRGB(r, g, b);
  }

  currentPaletteIndex = EEPROM.read(EEPROM_PALETTE);
  if (currentPaletteIndex < 0)
    currentPaletteIndex = 0;
  else if (currentPaletteIndex >= paletteCount)
    currentPaletteIndex = paletteCount - 1;
  big = EEPROM.read(EEPROM_BIG);
  lit = _min(EEPROM.read(EEPROM_LIT), NUM_LEDS);
 }

void sendAll()
{
  String json = "{";

  json += "\"power\":" + String(power) + ",";
  json += "\"glitter\":" + String(glitter) + ",";
  json += "\"big\":" + String(big) + ",";
  json += "\"lit\":" + String(lit) + ",";
  json += "\"numleds\":" + String(NUM_LEDS) + ",";
  json += "\"brightness\":" + String(brightness) + ",";

  json += "\"currentPattern\":{";
  json += "\"index\":" + String(currentPatternIndex);
  json += ",\"name\":\"" + patterns[currentPatternIndex].name + "\"}";

  json += ",\"currentPalette\":{";
  json += "\"index\":" + String(currentPaletteIndex);
  json += ",\"name\":\"" + paletteNames[currentPaletteIndex] + "\"}";

  json += ",\"solidColor\":{";
  json += "\"r\":" + String(solidColor.r);
  json += ",\"g\":" + String(solidColor.g);
  json += ",\"b\":" + String(solidColor.b);
  json += "}";

  json += ",\"patterns\":[";
  for (uint8_t i = 0; i < patternCount; i++)
  {
    json += "\"" + patterns[i].name + "\"";
    if (i < patternCount - 1)
      json += ",";
  }
  json += "]";

  json += ",\"palettes\":[";
  for (uint8_t i = 0; i < paletteCount; i++)
  {
    json += "\"" + paletteNames[i] + "\"";
    if (i < paletteCount - 1)
      json += ",";
  }
  json += "]";

  json += "}";

  server.send(200, "text/json", json);
  json = String();
}

void sendPower()
{
  String json = String(power);
  server.send(200, "text/json", json);
  json = String();
}

void sendGlitter()
{
  String json = String(glitter);
  server.send(200, "text/json", json);
  json = String();
}

void sendBig()
{
  String json = String(big);
  server.send(200, "text/json", json);
  json = String();
}

void sendLit()
{
  String json = String(lit);
  server.send(200, "text/json", json);
  json = String();
}

void sendPattern()
{
  String json = "{";
  json += "\"index\":" + String(currentPatternIndex);
  json += ",\"name\":\"" + patterns[currentPatternIndex].name + "\"";
  json += "}";
  server.send(200, "text/json", json);
  json = String();
}

void sendPalette()
{
  String json = "{";
  json += "\"index\":" + String(currentPaletteIndex);
  json += ",\"name\":\"" + paletteNames[currentPaletteIndex] + "\"";
  json += "}";
  server.send(200, "text/json", json);
  json = String();
}

void sendBrightness()
{
  String json = String(brightness);
  server.send(200, "text/json", json);
  json = String();
}

void sendSolidColor()
{
  String json = "{";
  json += "\"r\":" + String(solidColor.r);
  json += ",\"g\":" + String(solidColor.g);
  json += ",\"b\":" + String(solidColor.b);
  json += "}";
  server.send(200, "text/json", json);
  json = String();
}

void setPower(uint8_t value)
{
  power = value == 0 ? 0 : 1;
}

void setGlitter(uint8_t value)
{
  glitter = value == 0 ? 0 : 1;
}

void setBig(uint8_t value)
{
  big = value == 0 ? 0 : 1;
  Serial.println("Writing big " + big);
  EEPROM.write(EEPROM_BIG, big);
  EEPROM.commit();
}

void setLit(uint8_t value) {
  lit = _min(value, NUM_LEDS);
  Serial.println("Writing " + lit);
  EEPROM.write(EEPROM_LIT, lit);
  EEPROM.commit();
}

void setSolidColor(CRGB color)
{
  setSolidColor(color.r, color.g, color.b);
}

void setSolidColor(uint8_t r, uint8_t g, uint8_t b)
{
  solidColor = CRGB(r, g, b);

  EEPROM.write(EEPROM_SOLID_R, r);
  EEPROM.write(EEPROM_SOLID_G, g);
  EEPROM.write(EEPROM_SOLID_B, b);

  setPattern(patternCount - 1);
}

void setPattern(int value)
{
  // don't wrap around at the ends
  if (value < 0) {
    value = 0;
  } else if (value >= patternCount) {
    value = patternCount - 1;
  }
  currentPatternIndex = value;
  EEPROM.write(EEPROM_PATTERN, currentPatternIndex);
  EEPROM.commit();
}

void setPalette(int value)
{
  // don't wrap around at the ends
  if (value < 0) {
    value = 0;
  } else if (value >= paletteCount) {
    value = paletteCount - 1;
  }

  currentPaletteIndex = value;

  EEPROM.write(EEPROM_PALETTE, currentPaletteIndex);
  EEPROM.commit();
}

void setBrightness(int value)
{
  // don't wrap around at the ends
  if (value > 255)
    value = 255;
  else if (value < 0) value = 0;

  brightness = value;

  FastLED.setBrightness(brightness);

  EEPROM.write(EEPROM_BRIGHTNESS, brightness);
  EEPROM.commit();
}

void showSolidColor()
{
  fill_solid(leds, lit, solidColor);
}

void rainbow()
{
  // FastLED's built-in rainbow generator
  fill_rainbow( leds, lit, gHue, 10);
}

void rainbowWithGlitter()
{
  // built-in FastLED rainbow, plus some random sparkly glitter
  rainbow();
  addGlitter(80);
}

void addGlitter( fract8 chanceOfGlitter)
{
  if ( random8() < chanceOfGlitter) {
    int randomed = random16(lit);
    leds[randomed] += CRGB::White;
  }
}

void confetti()
{
  // random colored speckles that blink in and fade smoothly
  fadeToBlackBy( leds, NUM_LEDS, 10);
  int pos = random16(lit);
  //  leds[pos] += CHSV( gHue + random8(64), 200, 255);
  leds[pos] += ColorFromPalette(palettes[currentPaletteIndex], gHue + random8(64));
}

void sinelon()
{
  // a colored dot sweeping back and forth, with fading trails
  fadeToBlackBy( leds, NUM_LEDS, 20);
  if(lit > 2) {
    int pos = beatsin16(13, 0, lit - 1);
    //  leds[pos] += CHSV( gHue, 255, 192);
    leds[pos] += ColorFromPalette(palettes[currentPaletteIndex], gHue, 192);
  }
}


// Pattern made for someone named Jozef. This pattern slowly lights and fades random 
// numbers of leds
void jozef() {
  static int led_duration[NUM_LEDS];
  static CRGBPalette16 led_hue[NUM_LEDS];
  static int led_sat[NUM_LEDS];

  // This value sets how long leds remain lit, approximately. A random deviation is added
  // at each iteration
  static int DURATION = 550;
  
  // How often leds trigger. Higher = more often. Note that every iteration only triggers 
  // ONE led, irregardless of how many leds you have. If you want more you should rewrite 
  // this function to random a duration for every led. 
  static int FREQ_INV = 600;
  
  int a = random(FREQ_INV);
  if (a < lit && led_duration[a] == 0) {
    led_duration[a] = random(DURATION - (DURATION / 10), DURATION  + (DURATION / 10));
    led_sat[a] = gHue;
    led_hue[a] = palettes[currentPaletteIndex];
  }

  for(int i = 0; i < lit; i++) {
    if(led_duration[i]  > 0) {
      int bri = constrain(led_duration[i] > 255 ? DURATION - led_duration[i] : led_duration[i], 0, brightness);
      leds[i] = ColorFromPalette(led_hue[i], led_sat[i], bri);
      --led_duration[i];
    } else {
        leds[i] = CRGB::Black; 
    }
  }
}

void bpm()
{
  // colored stripes pulsing at a defined Beats-Per-Minute (BPM)
  uint8_t BeatsPerMinute = 62;
  CRGBPalette16 palette = palettes[currentPaletteIndex];
  uint8_t beat = beatsin8( BeatsPerMinute, 64, 255);
  for ( int i = 0; i < NUM_LEDS && i < lit; i++) { //9948
    leds[i] = ColorFromPalette(palette, gHue + (i * 2), beat - gHue + (i * 10));
  }
}

void juggle()
{
  // eight colored dots, weaving in and out of sync with each other
  fadeToBlackBy( leds, lit, 20);
  byte dothue = 0;
  for ( int i = 0; i < 8; i++)
  {
    //    leds[beatsin16(i + 7, 0, NUM_LEDS)] |= CHSV(dothue, 200, 255);
    leds[beatsin16(i + 7, 0, lit)] |= ColorFromPalette(palettes[currentPaletteIndex], dothue);
    dothue += 32;
  }
}

// Pride2015 by Mark Kriegsman: https://gist.github.com/kriegsman/964de772d64c502760e5
// This function draws rainbows with an ever-changing,
// widely-varying set of parameters.
void pride() {
  static uint16_t sPseudotime = 0;
  static uint16_t sLastMillis = 0;
  static uint16_t sHue16 = 0;

  uint8_t sat8 = beatsin88( 87, 220, 250);
  uint8_t brightdepth = beatsin88( 341, 96, 224);
  uint16_t brightnessthetainc16 = beatsin88( 203, (25 * 256), (40 * 256));
  uint8_t msmultiplier = beatsin88(147, 23, 60);

  uint16_t hue16 = sHue16;//gHue * 256;
  uint16_t hueinc16 = beatsin88(113, 1, 3000);

  uint16_t ms = millis();
  uint16_t deltams = ms - sLastMillis ;
  sLastMillis  = ms;
  sPseudotime += deltams * msmultiplier;
  sHue16 += deltams * beatsin88( 400, 5, 9);
  uint16_t brightnesstheta16 = sPseudotime;

  for ( uint16_t i = 0 ; i < lit; i++) {
    hue16 += hueinc16;
    uint8_t hue8 = hue16 / 256;

    brightnesstheta16  += brightnessthetainc16;
    uint16_t b16 = sin16( brightnesstheta16  ) + 32768;

    uint16_t bri16 = (uint32_t)((uint32_t)b16 * (uint32_t)b16) / 65536;
    uint8_t bri8 = (uint32_t)(((uint32_t)bri16) * brightdepth) / 65536;
    bri8 += (255 - brightdepth);

    CRGB newcolor = CHSV( hue8, sat8, bri8);

    nblend(leds[i], newcolor, 64);
  }
}

// ColorWavesWithPalettes by Mark Kriegsman: https://gist.github.com/kriegsman/8281905786e8b2632aeb
// This function draws color waves with an ever-changing,
// widely-varying set of parameters, using a color palette.
void colorwaves()
{
  static uint16_t sPseudotime = 0;
  static uint16_t sLastMillis = 0;
  static uint16_t sHue16 = 0;

  // uint8_t sat8 = beatsin88( 87, 220, 250);
  uint8_t brightdepth = beatsin88( 341, 96, 224);
  uint16_t brightnessthetainc16 = beatsin88( 203, (25 * 256), (40 * 256));
  uint8_t msmultiplier = beatsin88(147, 23, 60);

  uint16_t hue16 = sHue16;//gHue * 256;
  uint16_t hueinc16 = beatsin88(113, 300, 1500);

  uint16_t ms = millis();
  uint16_t deltams = ms - sLastMillis ;
  sLastMillis  = ms;
  sPseudotime += deltams * msmultiplier;
  sHue16 += deltams * beatsin88( 400, 5, 9);
  uint16_t brightnesstheta16 = sPseudotime;

  for ( uint16_t i = 0 ; i < lit; i++) {
    hue16 += hueinc16;
    uint8_t hue8 = hue16 / 256;
    uint16_t h16_128 = hue16 >> 7;
    if ( h16_128 & 0x100) {
      hue8 = 255 - (h16_128 >> 1);
    } else {
      hue8 = h16_128 >> 1;
    }

    brightnesstheta16  += brightnessthetainc16;
    uint16_t b16 = sin16( brightnesstheta16  ) + 32768;

    uint16_t bri16 = (uint32_t)((uint32_t)b16 * (uint32_t)b16) / 65536;
    uint8_t bri8 = (uint32_t)(((uint32_t)bri16) * brightdepth) / 65536;
    bri8 += (255 - brightdepth);

    uint8_t index = hue8;
    //index = triwave8( index);
    index = scale8( index, 240);

    CRGB newcolor = ColorFromPalette(gCurrentPalette, index, bri8);

    nblend(leds[i], newcolor, 128);
  }
}

void police() {
	static int colorstep = 0;
	static int flip = 1;
	static uint16_t reds[NUM_LEDS];
	static uint16_t init = 0;

	// This ugly amount of code is to get my leds in alternating reds and blues, 
	// I'm pretty sure there is a better way by bitshifting or such. 
	// This codes generates an array of alternating ones and zeroes. 
	if(init == 0) {
		for ( uint16_t i = 0 ; i < NUM_LEDS; i++) {
		  if(i % 2 == 1) { 
  			reds[i] = 1;
		  }
		}
		init = 1;
	}
  
    // We use 4x flip so the brightness ramps up a lot faster    
	colorstep = colorstep + (4 * flip);
	
	// If we hit boundaries, start powering the leds down
	if(colorstep >= 255) {
  	colorstep = 255;
		flip = -1;
    // And if the led is powered down, switch colors
	} else if (colorstep <= 0){
  	colorstep = 0;
		flip = 1;
		// Flip reds and blues
		for ( uint16_t i = 0 ; i < NUM_LEDS; i++) {
		  reds[i] = (reds[i] == 1 ? 0 : 1);
		}
	}

	// Actually color the leds
	for ( uint16_t i = 0 ; i < lit; i++) {
	    // ..if this led is red
		if(reds[i]) {
			leds[i] = CRGB(colorstep, 0, 0);
		// ..if this led is blue
		} else {
			leds[i] = CRGB(0, 0, colorstep);
		}
	}
	FastLED.setBrightness(brightness);
}

// Alternate rendering function just scrolls the current palette
// across the defined LED strip.
void palettetest()
{
  static uint8_t startindex = 0;
  startindex--;
  fill_palette( leds, NUM_LEDS, startindex, (256 / NUM_LEDS) + 1, gCurrentPalette, 255, LINEARBLEND);
}

