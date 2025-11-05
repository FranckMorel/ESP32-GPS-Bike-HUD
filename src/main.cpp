
#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Adafruit_GPS.h>

//TFT_Display per SPI
#define TFT_CS   5
#define TFT_DC   17
#define TFT_RST  19
Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_RST);   // SCK = GPIO18, MOSI = GPIO23

// GPS per UART2
HardwareSerial GPSSerial(2); 
Adafruit_GPS   GPS(&GPSSerial);

// Zustände
uint32_t lastUI = 0, lastLog = 0, lastCoords = 0; // Zeitmarken für Rate Limits (UI, Log, Koordinaten)
double   vEMA = 0.0;              // Exponential Moving Average der Geschwindigkeit
const double ALPHA = 0.75;        // Filtergewicht
bool     fixMsgShown = false;     // Merker
bool     lastFix = true;         // letzter Fixzustand
int      lastSpeedInt = -999;     // zuletzt gezeigte km/h (Ganzzahl)
double   lastLat = 1e9, lastLon = 1e9; // zuletzt gezeigte Koordinaten


// statisches UI
static void drawStaticUI() {
  tft.fillScreen(ST77XX_BLACK);   // Display komplett löschen 
  tft.setTextWrap(false);         
  tft.fillRect(0, 0, 128, 14, ST77XX_BLACK); 
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_YELLOW);
  tft.setCursor(4, 3); tft.print("GPS Bike HUD"); // linker Titel

  tft.setTextColor(ST77XX_CYAN);
  tft.setCursor(4, 96);  tft.print("Lat:");   // Labels unten links
  tft.setCursor(4, 110); tft.print("Lon:");
}

// GPS Fix Anzeige
static void drawFix(bool fix) {
  if (fix == lastFix) return; // nur neu zeichnen, wenn sich der Status ändert
  lastFix = fix;
  tft.fillRect(96, 0, 32, 14, ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setCursor(104, 3);
  tft.setTextColor(fix ? ST77XX_GREEN : ST77XX_RED);
  tft.print(fix ? "F" : "X"); // F = Fix, X = kein Fix
}

// Anzeige Geschwindigkeit 
static void drawSpeedInt(int kmh) {
  if (kmh == lastSpeedInt) return; // gleiche Zahl -> nichts neu zeichnen
  lastSpeedInt = kmh;
  if (kmh < 0) kmh = 0; if (kmh > 199) kmh = 199; // simple Begrenzung

  tft.fillRect(0, 20, 128, 60, ST77XX_BLACK);     // Geschwindigkeitsbereich löschen
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(4);
  tft.setCursor(18, 26);
  char buf[6]; snprintf(buf, sizeof(buf), "%3d", kmh);
  tft.print(buf);
  tft.setTextSize(2);
  tft.setCursor(98, 38); tft.print("km/h");
}

// Anzeige GPS Koordinaten
static void drawCoords(double lat, double lon) {
  uint32_t now = millis();
  if (now - lastCoords < 500) return; // max. 2x pro Sekunde
  // nur updaten, wenn sich etwas merklich geändert hat (~1 m bei 1e-5 Grad)
  if (fabs(lat - lastLat) < 1e-5 && fabs(lon - lastLon) < 1e-5) return;
  lastCoords = now; lastLat = lat; lastLon = lon;

  tft.fillRect(32, 96, 96, 24, ST77XX_BLACK);    // Koordinatenbereich löschen
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_CYAN);
  tft.setCursor(32, 96);  tft.print(lat, 5);
  tft.setCursor(32, 110); tft.print(lon, 5);
}

// Fix Hinweis 
static void showFixHint(bool fix) {
  if (!fix && !fixMsgShown) {
    fixMsgShown = true;
    tft.fillRect(0, 84, 128, 12, ST77XX_BLACK);
    tft.setTextSize(1); tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(4, 84); tft.print("Warte auf GPS Fix...");
  }
  if (fix && fixMsgShown) {
    tft.fillRect(0, 84, 128, 12, ST77XX_BLACK);
    fixMsgShown = false;
  }
}

void setup() {
  Serial.begin(115200);                    

  SPI.begin(18, -1, 23, TFT_CS);           // Kein MISO Pin 
  tft.initR(INITR_BLACKTAB);               
  tft.setRotation(1);                      // Querformat
  drawStaticUI();                          // Header + Labels

  GPSSerial.begin(9600, SERIAL_8N1, 21, 16); 
  GPS.begin(9600);                         // Adafruit_GPS an 9600 koppeln
  GPS.sendCommand(PMTK_SET_BAUD_115200);   // GPS auf 115200 Baud umschalten
  delay(100);
  GPSSerial.updateBaudRate(115200);        // UART2 auf 115200 umschalten
  GPS.begin(115200);                       // Parser ebenfalls auf 115200

  // NMEA_Sätze & Rate:
  GPS.sendCommand(PMTK_SET_NMEA_OUTPUT_RMCGGA); // RMC (Speed/Kurs/Lat/Lon/Zeit) + GGA (Fix/HDOP/Alt)
  GPS.sendCommand(PMTK_SET_NMEA_UPDATE_10HZ);   // 10 Hz Update Rate
  GPS.sendCommand(PGCMD_ANTENNA);               // Antennenstatus

  showFixHint(false);                      // zu Beginn Hinweis zeigen 
}


void loop() {
  // Eingehende NMEA_Zeichen zügig auslesen und parsen:
  while (GPS.available()) {
    char c = GPS.read();
    if (GPS.newNMEAreceived()) GPS.parse(GPS.lastNMEA()); // Satz vollständig -> parsen
  }

  bool fix = GPS.fix;                      // 1 = gültiger Fix
  double v_raw = GPS.speed * 1.852;        // kn -> km/h
  if (!fix) v_raw = 0.0;                 

  // EMA-Filter 
  vEMA = ALPHA * v_raw + (1.0 - ALPHA) * vEMA;
  int v_int = (int)lrint(vEMA);            // Ganzzahl (km/h)

  uint32_t now = millis();
  if (now - lastUI >= 100) {               // UI alle ~100 ms
    lastUI = now;
    drawFix(fix);                          // F/X rechts oben
    showFixHint(fix);                      // Hinweis je nach Fix ein/aus
    drawSpeedInt(v_int);                   // Geschwindigkeit in der Mitte
    if (fix) drawCoords(GPS.latitudeDegrees, GPS.longitudeDegrees); // Koordinaten 
  }

  if (now - lastLog >= 500) {              // Debug alle 500 ms
    lastLog = now;
    Serial.print("Fix: "); Serial.print((int)fix);
    Serial.print("  v_raw: "); Serial.print(v_raw, 2);
    Serial.print("  vEMA: ");  Serial.print(vEMA, 2);
    Serial.print("  shown: "); Serial.print(v_int);
    if (fix) {
      Serial.print("  Lat: "); Serial.print(GPS.latitudeDegrees, 6);
      Serial.print("  Lon: "); Serial.print(GPS.longitudeDegrees, 6);
    }
    Serial.println();
  }
}
