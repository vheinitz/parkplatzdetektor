/*
 * Carsensor -- Fahrzeugerkennung ueber Magnetfeldstoerung
 * ESP32-C3 + GY-271 (QMC5883P / QMC5883L / HMC5883L)
 *
 * Verdrahtung (SDA und SCL sind gegenueber der ueblichen Reihenfolge
 * vertauscht, so ist es hier verloetet):
 *   VCC -> 3V3
 *   GND -> G / GND     (NICHT auf GPIO0)
 *   SDA -> GPIO 5
 *   SCL -> GPIO 4
 *
 * MESSGROESSE IST |B| = sqrt(Bx^2 + By^2 + Bz^2), nicht die einzelnen Achsen.
 *
 * Grund: |B| ist drehinvariant. Die Rohwerte der Achsen sind es nicht --
 * bei |B| ~ 1700 Zaehlern verschiebt schon 1 Grad Verkippung den Vektor um
 * 1700 * pi/180 ~ 30 Zaehler, also fast so viel wie ein ganzes Auto. Beim
 * Schieben des Sensors ueber den Boden ist das nicht vermeidbar. |B| aendert
 * sich dagegen nur, wenn wirklich Eisen in der Naehe ist.
 * Die Rohwerte werden weiter angezeigt, aber nur zur Information.
 *
 * Bedienung ueber Handy:
 *   1. Mit dem WLAN "Carsensor" verbinden (Passwort siehe unten).
 *      Die Meldung "kein Internetzugriff" bestaetigen und verbunden bleiben.
 *   2. Meist oeffnet sich die Seite von selbst (Captive Portal). Sonst im
 *      Browser  192.168.4.1/  eintippen -- ohne http://, mit Schraegstrich
 *      am Ende. Dank Portal fuehrt aber auch jede andere Adresse hierher.
 *
 * Kalibrieren (Startseite):
 *   "reset"            loescht Kalibrierung UND Messreihe.
 *   "leer: Start/Stop" mittelt |B| ohne Auto. Zwischen Stop und dem naechsten
 *                      Start liegt beliebig viel Zeit -- gemittelt wird nur
 *                      innerhalb des Fensters, die Anfahrt faellt heraus.
 *   "Auto: Start/Stop" dasselbe mit Auto darueber. Danach laeuft die
 *                      Erkennung und zeigt "Auto" oder "leer".
 *
 * Feldversuch (Seite /test):
 *   Referenz auf freier Flaeche setzen, Sensor an die Messposition bringen,
 *   Positionsknopf druecken. Gemessen wird d|B| = | |B|_mess - |B|_ref |,
 *   je 100 Einzelwerte mit Standardfehler. CSV-Export unter /csv.
 *
 * Serielle Konsole: n = reset, 1/2 = leer Start/Stop, 3/4 = Auto Start/Stop,
 *                   r = Referenz, m = Messung, l = Liste, x = Liste loeschen,
 *                   i = Info, t = Selbsttest
 */

#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

// Beim C3 haengt es an "Tools -> USB CDC On Boot", wohin Serial zeigt:
// Enabled -> USB-Buchse, Disabled -> UART0. Bei Disabled legt der Core gar
// keine globale USB-CDC-Instanz an, also erzeugen wir hier selbst eine.
#if defined(ARDUINO_USB_CDC_ON_BOOT) && !ARDUINO_USB_CDC_ON_BOOT
HWCDC UsbSerial;
#undef Serial
#define Serial UsbSerial
#endif

// ---------------------------------------------------------------- Konfiguration

#define PIN_SDA 5
#define PIN_SCL 4

#define AP_SSID "Carsensor"
#define AP_PASS "carsensor"          // mindestens 8 Zeichen

// Statt Accesspoint ins vorhandene WLAN: beide Zeilen einkommentieren.
// #define STA_SSID "MeinWLAN"
// #define STA_PASS "geheim"

#define SAMPLE_MS       50           // Messintervall
#define ALPHA_LANGSAM   0.02f        // ruhiges Mittel von |B| fuer die Anzeige (~2,5 s)
#define ALPHA_SCHNELL   0.05f        // Mittel von |B| fuer die Erkennung (~1 s)
#define MESS_PROBEN     100          // 100 x SAMPLE_MS = 5 s pro Feldmessung

// Hysterese um die Schwelle. 0.5 ist genau die Mitte zwischen leer und Auto.
#define SCHWELLE_EIN    0.60f
#define SCHWELLE_AUS    0.40f

// Mindestunterschied von |B| zwischen leer und Auto. Darunter ist die
// Kalibrierung nicht verwertbar. Das Grundrauschen eines 100er-Mittels
// liegt bei etwa 1,5 Zaehlern, 10 sind also rund sieben Sigma.
#define MIN_ABSTAND     10.0f

// ---------------------------------------------------------------- Sensor

#define ADDR_QMC5883L 0x0D
#define ADDR_HMC5883L 0x1E
#define ADDR_QMC5883P 0x2C

// Messbereich QMC5883P, Bits [3:2] von Register 0x0B
// 0x00 = 30 G (1000 LSB/G)   0x04 = 12 G (2500 LSB/G)
// 0x08 =  8 G (3750 LSB/G)   0x0C =  2 G (15000 LSB/G)
#define QMC5883P_RANGE 0x08
#define QMC5883P_LSB_PER_GAUSS 3750.0f

enum SensorType { SENSOR_NONE, SENSOR_QMC_L, SENSOR_HMC, SENSOR_QMC_P };

// Enums und Strukturen muessen vor die erste Funktion: die Arduino-IDE
// erzeugt die Prototypen automatisch am Dateianfang.
enum Zweck { ZWECK_NICHTS, ZWECK_REF, ZWECK_MESS, ZWECK_KAL_LEER, ZWECK_KAL_AUTO };
enum Phase { PHASE_LEER, PHASE_AUTO, PHASE_ERKENNUNG };

#define MAX_MESSUNGEN 80
#define NAME_LEN      32
#define SERIE_MAGIC   0x53455234   // "SER4"
#define KAL_MAGIC     0x43415232   // "CAR2"

struct Messung {
  char     name[NAME_LEN];
  float    bRef, bWert;        // |B| der Referenz und der Messung
  float    sigma, sigmaRef;    // Streuung der |B|-Einzelwerte
  float    ref[3], wert[3];    // Vektormittel, nur zur Nachauswertung in der CSV
  uint16_t n;                  // Zahl der gemittelten Einzelwerte
  uint32_t t;
};

struct Serie {
  uint32_t magic;
  uint16_t n;
  Messung  m[MAX_MESSUNGEN];
};

struct Kalibrierung {
  uint32_t magic;
  float    bLeer, bAuto;       // |B| ohne und mit Auto
  float    sLeer, sAuto;       // zugehoerige Streuungen
};

SensorType sensor = SENSOR_NONE;
const char *sensorName = "-";
float scale_uT = 0.0f;

bool writeReg(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool readRegs(uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)addr, (int)len) != len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

// Eine Adresse gilt nur als belegt, wenn sie mehrfach hintereinander
// antwortet. Ein einzelner Treffer kann auf einem gestoerten Bus ein
// Phantom sein.
bool devicePresent(uint8_t addr, uint8_t versuche = 3) {
  for (uint8_t i = 0; i < versuche; i++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() != 0) return false;
    delay(2);
  }
  return true;
}

bool initQMC_P() {
  if (!writeReg(ADDR_QMC5883P, 0x0B, 0x80)) return false;   // Soft-Reset
  delay(30);
  // 0x0B muss zweimal beschrieben werden: der erste Schreibvorgang loescht
  // nur das Reset-Bit. Setzt man den Messbereich schon dort, wird er
  // ignoriert und der Chip misst stillschweigend weiter im 30-G-Bereich.
  if (!writeReg(ADDR_QMC5883P, 0x0B, 0x00)) return false;
  if (!writeReg(ADDR_QMC5883P, 0x0B, QMC5883P_RANGE)) return false;
  writeReg(ADDR_QMC5883P, 0x29, 0x06);                      // Achsenvorzeichen
  if (!writeReg(ADDR_QMC5883P, 0x0A, 0x0F)) return false;   // continuous, 200 Hz
  delay(100);
  scale_uT = 100.0f / QMC5883P_LSB_PER_GAUSS;
  return true;
}

bool initQMC_L() {
  writeReg(ADDR_QMC5883L, 0x0A, 0x80);
  delay(10);
  if (!writeReg(ADDR_QMC5883L, 0x0B, 0x01)) return false;
  if (!writeReg(ADDR_QMC5883L, 0x09, 0x1D)) return false;
  scale_uT = 100.0f / 3000.0f;
  return true;
}

bool initHMC() {
  if (!writeReg(ADDR_HMC5883L, 0x00, 0x70)) return false;
  if (!writeReg(ADDR_HMC5883L, 0x01, 0x20)) return false;
  if (!writeReg(ADDR_HMC5883L, 0x02, 0x00)) return false;
  scale_uT = 100.0f / 1090.0f;
  return true;
}

bool detectSensor() {
  if (devicePresent(ADDR_QMC5883P) && initQMC_P()) {
    sensor = SENSOR_QMC_P; sensorName = "QMC5883P"; return true;
  }
  if (devicePresent(ADDR_QMC5883L) && initQMC_L()) {
    sensor = SENSOR_QMC_L; sensorName = "QMC5883L"; return true;
  }
  if (devicePresent(ADDR_HMC5883L) && initHMC()) {
    sensor = SENSOR_HMC; sensorName = "HMC5883L"; return true;
  }
  return false;
}

bool readRaw(int16_t &x, int16_t &y, int16_t &z) {
  uint8_t b[6];
  switch (sensor) {
    case SENSOR_QMC_P: {
      uint8_t status;
      if (!readRegs(ADDR_QMC5883P, 0x09, &status, 1)) return false;
      if (!(status & 0x01)) return false;
      if (!readRegs(ADDR_QMC5883P, 0x01, b, 6)) return false;
      x = (int16_t)(b[0] | (b[1] << 8));
      y = (int16_t)(b[2] | (b[3] << 8));
      z = (int16_t)(b[4] | (b[5] << 8));
      return true;
    }
    case SENSOR_QMC_L: {
      uint8_t status;
      if (!readRegs(ADDR_QMC5883L, 0x06, &status, 1)) return false;
      if (!(status & 0x01)) return false;
      if (!readRegs(ADDR_QMC5883L, 0x00, b, 6)) return false;
      x = (int16_t)(b[0] | (b[1] << 8));
      y = (int16_t)(b[2] | (b[3] << 8));
      z = (int16_t)(b[4] | (b[5] << 8));
      return true;
    }
    case SENSOR_HMC: {
      if (!readRegs(ADDR_HMC5883L, 0x03, b, 6)) return false;
      x = (int16_t)((b[0] << 8) | b[1]);
      z = (int16_t)((b[2] << 8) | b[3]);
      y = (int16_t)((b[4] << 8) | b[5]);
      if (x == -4096 || y == -4096 || z == -4096) return false;
      return true;
    }
    default:
      return false;
  }
}

// ---------------------------------------------------------------- Zustand

Preferences prefs;
WebServer server(80);
DNSServer dns;
bool apModus = false;

Serie serie;
Kalibrierung kal;
Phase phase = PHASE_LEER;

int16_t roh[3] = {0, 0, 0};       // letzte Rohwerte, nur zur Anzeige
float   bJetzt = 0.0f;            // |B| des letzten Einzelwerts
float   bLangsam = 0.0f;          // ruhiges Mittel von |B|
float   bSchnell = 0.0f;          // Mittel von |B| fuer die Erkennung
bool    avgInit = false;
bool    sensorOk = false;

float aktuellS = 0.0f;            // 0 = wie leer, 1 = wie mit Auto
bool  autoDa = false;

// Referenz der Messreihe
float bRef = 0.0f, bRefSigma = 0.0f, refVek[3] = {0, 0, 0};
bool  refGesetzt = false;

// Laufende Mittelung. Laeuft nebenher, damit der Webserver bedienbar
// bleibt. messZiel = 0 heisst offenes Ende, Abschluss per Stop.
Zweck    messZweck = ZWECK_NICHTS;
uint16_t messN = 0, messZiel = 0;
double   sumB = 0, sumBQ = 0, sumV[3] = {0, 0, 0};
char     messName[NAME_LEN] = "";

float betrag(float x, float y, float z) {
  return sqrtf(x * x + y * y + z * z);
}

// ---------------------------------------------------------------- Speicher

void kalLoeschen() {
  memset(&kal, 0, sizeof(kal));
  phase = PHASE_LEER;
  autoDa = false;
}

void kalSpeichern() {
  kal.magic = KAL_MAGIC;
  prefs.begin("carsensor", false);
  prefs.putBytes("kal", &kal, sizeof(kal));
  prefs.end();
}

bool kalLaden() {
  prefs.begin("carsensor", true);
  size_t n = prefs.getBytes("kal", &kal, sizeof(kal));
  prefs.end();
  if (n != sizeof(kal) || kal.magic != KAL_MAGIC) { memset(&kal, 0, sizeof(kal)); return false; }
  return true;
}

void serieSpeichern() {
  serie.magic = SERIE_MAGIC;
  prefs.begin("carsensor", false);
  size_t n = prefs.putBytes("serie", &serie, sizeof(serie));
  prefs.end();
  if (n != sizeof(serie)) Serial.println(F("  FEHLER: Messreihe konnte nicht gespeichert werden!"));
}

void serieLaden() {
  prefs.begin("carsensor", true);
  size_t n = prefs.getBytes("serie", &serie, sizeof(serie));
  prefs.end();
  if (n != sizeof(serie) || serie.magic != SERIE_MAGIC || serie.n > MAX_MESSUNGEN) {
    memset(&serie, 0, sizeof(serie));
    serie.magic = SERIE_MAGIC;
  }
}

// Abstand der beiden Kalibrierpunkte in |B|
float kalAbstand() {
  if (kal.magic != KAL_MAGIC) return 0.0f;
  return fabsf(kal.bAuto - kal.bLeer);
}

bool kalBrauchbar() {
  return kal.magic == KAL_MAGIC && kalAbstand() >= MIN_ABSTAND;
}

// Schwelle liegt genau zwischen leer und Auto
float schwelleB() {
  return (kal.bLeer + kal.bAuto) * 0.5f;
}

// ---------------------------------------------------------------- Mittelung

void mittelungStarten(Zweck z, const String &name, uint16_t ziel) {
  messZweck = z;
  messZiel = ziel;
  messN = 0;
  sumB = 0; sumBQ = 0;
  for (uint8_t i = 0; i < 3; i++) sumV[i] = 0;
  memset(messName, 0, sizeof(messName));
  strncpy(messName, name.c_str(), NAME_LEN - 1);
  for (uint8_t i = 0; i < NAME_LEN; i++)
    if (messName[i] == ';' || messName[i] == '\n' || messName[i] == '\r') messName[i] = '_';
}

void mittelungAbbrechen() { messZweck = ZWECK_NICHTS; }

void mittelungFertig() {
  if (messN < 2) { messZweck = ZWECK_NICHTS; return; }

  float mb = sumB / messN;
  double v = sumBQ / messN - (double)mb * mb;                             // E[x^2] - E[x]^2
  float sigma = v > 0 ? sqrtf((float)(v * messN / (messN - 1))) : 0.0f;   // Bessel-Korrektur
  // Der Fehler des MITTELWERTS ist um sqrt(N) kleiner als die Streuung der
  // Einzelwerte -- das ist der Fehlerbalken.
  float fehler = sigma / sqrtf((float)messN);
  float mv[3];
  for (uint8_t i = 0; i < 3; i++) mv[i] = sumV[i] / messN;

  switch (messZweck) {
    case ZWECK_REF:
      bRef = mb; bRefSigma = sigma;
      for (uint8_t i = 0; i < 3; i++) refVek[i] = mv[i];
      refGesetzt = true;
      Serial.printf("[ref] |B|=%.1f +-%.2f  (sigma einzel %.1f, %u Werte)\n",
                    mb, fehler, sigma, messN);
      break;

    case ZWECK_MESS:
      if (serie.n < MAX_MESSUNGEN) {
        Messung &m = serie.m[serie.n];
        memset(&m, 0, sizeof(m));
        strncpy(m.name, messName, NAME_LEN - 1);
        m.bRef = bRef; m.bWert = mb;
        m.sigma = sigma; m.sigmaRef = bRefSigma;
        for (uint8_t i = 0; i < 3; i++) { m.ref[i] = refVek[i]; m.wert[i] = mv[i]; }
        m.n = messN;
        m.t = millis();
        serie.n++;
        serieSpeichern();
        Serial.printf("[mess %u] %-24s d|B|=%7.1f +-%.2f  (|B| %.1f gegen %.1f)\n",
                      serie.n, m.name, fabsf(m.bWert - m.bRef),
                      sqrtf(m.sigma * m.sigma + m.sigmaRef * m.sigmaRef) / sqrtf((float)m.n),
                      m.bWert, m.bRef);
      }
      break;

    case ZWECK_KAL_LEER:
      kal.bLeer = mb; kal.sLeer = sigma;
      phase = PHASE_AUTO;
      Serial.printf("[leer stop] |B|=%.1f +-%.2f  (sigma %.1f, %u Werte)\n",
                    mb, fehler, sigma, messN);
      Serial.println(F("  Jetzt Auto darueber, dann 'Auto: Start' / 'Auto: Stop'."));
      break;

    case ZWECK_KAL_AUTO:
      kal.bAuto = mb; kal.sAuto = sigma;
      kalSpeichern();
      phase = PHASE_ERKENNUNG;
      Serial.printf("[auto stop] |B|=%.1f +-%.2f  (sigma %.1f, %u Werte)\n",
                    mb, fehler, sigma, messN);
      Serial.printf("  Unterschied |B|: %.1f   Schwelle bei %.1f\n",
                    kalAbstand(), schwelleB());
      if (!kalBrauchbar())
        Serial.printf("  WARNUNG: Unterschied unter %.0f -- Erkennung unzuverlaessig.\n",
                      (float)MIN_ABSTAND);
      break;

    default:
      break;
  }
  messZweck = ZWECK_NICHTS;
}

// ---------------------------------------------------------------- Bedienung

void aktionReset() {
  kalLoeschen();
  serie.n = 0;
  serieSpeichern();
  prefs.begin("carsensor", false);
  prefs.remove("kal");
  prefs.end();
  refGesetzt = false;
  mittelungAbbrechen();
  avgInit = false;
  Serial.println(F("[reset] Kalibrierung und Messreihe geloescht."));
}

void kalStart(bool leer) {
  mittelungStarten(leer ? ZWECK_KAL_LEER : ZWECK_KAL_AUTO,
                   leer ? "kal_leer" : "kal_auto", 0);
  Serial.printf("[%s start] mittle...\n", leer ? "leer" : "auto");
}

void kalStop() {
  if (messZweck != ZWECK_KAL_LEER && messZweck != ZWECK_KAL_AUTO) return;
  if (messN < 10) {
    Serial.printf("[stop] nur %u Werte -- verworfen, bitte laenger mitteln.\n", messN);
    mittelungAbbrechen();
    return;
  }
  mittelungFertig();
}

// ---------------------------------------------------------------- Erkennung

// s normiert |B| auf die Strecke zwischen den beiden Kalibrierpunkten:
// 0 = wie ohne Auto, 1 = wie mit Auto. Funktioniert unabhaengig davon, ob
// das Auto |B| vergroessert oder verkleinert.
float diskriminante(float b) {
  float d = kal.bAuto - kal.bLeer;
  if (fabsf(d) < 1e-6f) return 0.0f;
  return (b - kal.bLeer) / d;
}

void erkennungAktualisieren() {
  aktuellS = diskriminante(bSchnell);
  if (!autoDa && aktuellS > SCHWELLE_EIN) autoDa = true;
  else if (autoDa && aktuellS < SCHWELLE_AUS) autoDa = false;
}

const char *ergebnisText() {
  if (phase != PHASE_ERKENNUNG) return "not-ready";
  if (!sensorOk) return "sensorfehler";
  // Zu dicht beieinander liegende Kalibrierpunkte werten nur Rauschen aus.
  if (!kalBrauchbar()) return "not-ready";
  return autoDa ? "Auto" : "leer";
}

const char *phaseText() {
  switch (phase) {
    case PHASE_LEER:
      if (messZweck == ZWECK_KAL_LEER) return "mittelt ohne Auto -- Stop druecken wenn genug";
      return "Schritt 1: Platz frei? Dann 'leer: Start'";
    case PHASE_AUTO:
      if (messZweck == ZWECK_KAL_AUTO) return "mittelt mit Auto -- Stop druecken wenn genug";
      return "Schritt 2: Auto steht? Dann 'Auto: Start'";
    default:
      if (!kalBrauchbar()) return "Kalibrierung unbrauchbar: Unterschied zu klein -- reset";
      return "Erkennung laeuft";
  }
}

// Selbsttest der Erkennungslogik mit bekannten Werten, unabhaengig vom Sensor.
void selbsttest() {
  Kalibrierung sicherung = kal;
  kal.magic = KAL_MAGIC; kal.bLeer = 1000.0f; kal.bAuto = 1100.0f;

  struct Fall { const char *name; float b; float soll; };
  const Fall faelle[] = {
    { "wie leer",        1000.0f, 0.0f },
    { "wie Auto",        1100.0f, 1.0f },
    { "Mitte",           1050.0f, 0.5f },
    { "darueber hinaus", 1150.0f, 1.5f },
  };
  Serial.printf("Selbsttest (leer %.0f, Auto %.0f, Schwelle %.0f):\n",
                kal.bLeer, kal.bAuto, schwelleB());
  bool ok = true;
  for (const Fall &f : faelle) {
    float s = diskriminante(f.b);
    bool gut = fabsf(s - f.soll) < 0.001f;
    ok &= gut;
    Serial.printf("  %-16s |B|=%7.1f -> s=%6.3f (soll %5.3f)  %s\n",
                  f.name, f.b, s, f.soll, gut ? "ok" : "FEHLER");
  }
  // Gegenprobe: Auto senkt das Feld statt es zu erhoehen
  kal.bLeer = 1100.0f; kal.bAuto = 1000.0f;
  float s = diskriminante(1050.0f);
  bool gut = fabsf(s - 0.5f) < 0.001f;
  ok &= gut;
  Serial.printf("  %-16s |B|=%7.1f -> s=%6.3f (soll 0.500)  %s\n",
                "fallendes |B|", 1050.0f, s, gut ? "ok" : "FEHLER");
  Serial.println(ok ? F("  -> bestanden") : F("  -> FEHLGESCHLAGEN"));

  kal = sicherung;
}

// ---------------------------------------------------------------- Messung

void messen() {
  int16_t x, y, z;
  if (!readRaw(x, y, z)) { sensorOk = false; return; }
  sensorOk = true;
  roh[0] = x; roh[1] = y; roh[2] = z;

  bJetzt = betrag(x, y, z);
  if (!avgInit) {
    bLangsam = bSchnell = bJetzt;
    avgInit = true;
  } else {
    bLangsam += ALPHA_LANGSAM * (bJetzt - bLangsam);
    bSchnell += ALPHA_SCHNELL * (bJetzt - bSchnell);
  }

  if (messZweck != ZWECK_NICHTS) {
    sumB  += bJetzt;
    sumBQ += (double)bJetzt * bJetzt;
    sumV[0] += x; sumV[1] += y; sumV[2] += z;
    messN++;
    if (messZiel && messN >= messZiel) mittelungFertig();
  }

  if (phase == PHASE_ERKENNUNG) erkennungAktualisieren();
}

// Ruhig? Liegen schnelles und langsames Mittel dicht beieinander, hat sich
// nichts mehr bewegt und eine Messung ist verwertbar.
float unruhe() { return fabsf(bSchnell - bLangsam); }

// ---------------------------------------------------------------- Webseiten

const char SEITE[] PROGMEM = R"HTML(<!doctype html>
<html lang="de"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Carsensor</title><style>
*{box-sizing:border-box}
body{font-family:system-ui,sans-serif;margin:0;padding:1.2rem;
     background:#111;color:#eee;-webkit-text-size-adjust:100%}
h1{font-size:1rem;font-weight:600;color:#888;margin:0 0 1rem}
a{color:#6aa9ff}
#ergebnis{font-size:3.2rem;font-weight:700;text-align:center;
          padding:1.6rem 0;border-radius:14px;background:#1c1c1c;margin-bottom:.5rem}
.auto{color:#38d66b}.leer{color:#888}.notready{color:#e8b93b}.fehler{color:#e05252}
#phase{text-align:center;color:#999;font-size:.9rem;margin-bottom:1.2rem;min-height:1.2em}
button{width:100%;padding:1rem;font-size:1.05rem;font-weight:600;
       border:0;border-radius:12px;background:#2a2a2a;color:#eee;cursor:pointer}
button:active{background:#3a3a3a}
#reset{background:#7c2d2d;margin-bottom:.7rem}
.paar{display:grid;grid-template-columns:1fr 1fr;gap:.5rem;margin-bottom:.7rem}
table{width:100%;border-collapse:collapse;font-size:.85rem;margin-top:1.2rem;
      font-variant-numeric:tabular-nums}
td{padding:.35rem 0;border-bottom:1px solid #262626;color:#bbb}
td:last-child{text-align:right;color:#eee}
</style></head><body>
<h1>Carsensor &nbsp;|&nbsp; <a href="/test">Messreihe</a></h1>
<div id="ergebnis" class="notready">--</div>
<div id="phase"></div>
<button id="reset" onclick="cmd('reset')">reset &mdash; alles zuruecksetzen</button>
<div class="paar">
  <button onclick="cmd('leerstart')">leer: Start</button>
  <button onclick="cmd('leerstop')">leer: Stop</button>
</div>
<div class="paar">
  <button onclick="cmd('autostart')">Auto: Start</button>
  <button onclick="cmd('autostop')">Auto: Stop</button>
</div>
<table id="werte"></table>
<script>
function cmd(c){fetch('/'+c).then(hole)}
function z(v){return (v===undefined||v===null)?'-':(Math.round(v*10)/10)}
function hole(){
 fetch('/status').then(function(r){return r.json()}).then(function(d){
  var e=document.getElementById('ergebnis');
  e.textContent=d.ergebnis;
  e.className={'Auto':'auto','leer':'leer','not-ready':'notready'}[d.ergebnis]||'fehler';
  document.getElementById('phase').textContent=
    d.phase+(d.mittelt?'  ('+d.mittelN+' Werte)':'');
  document.getElementById('werte').innerHTML=
   '<tr><td>|B| aktuell</td><td>'+z(d.b)+'</td></tr>'+
   '<tr><td>|B| leer</td><td>'+z(d.bLeer)+'</td></tr>'+
   '<tr><td>|B| Auto</td><td>'+z(d.bAuto)+'</td></tr>'+
   '<tr><td>Schwelle</td><td>'+z(d.schwelle)+'</td></tr>'+
   '<tr><td>Unterschied</td><td>'+z(d.abstand)+'</td></tr>'+
   '<tr><td>s (0=leer, 1=Auto)</td><td>'+(Math.round(d.s*1000)/1000)+'</td></tr>'+
   '<tr><td>Sensor</td><td>'+d.sensor+'</td></tr>'+
   '<tr><td>Messungen</td><td>'+d.anzahl+'</td></tr>'+
   '<tr><td>roh X/Y/Z (nur Info)</td><td>'+d.roh.join(' / ')+'</td></tr>';
 }).catch(function(){});
}
hole();setInterval(hole,500);
</script></body></html>)HTML";

const char SEITE_TEST[] PROGMEM = R"HTML(<!doctype html>
<html lang="de"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Carsensor Messreihe</title><style>
*{box-sizing:border-box}
body{font-family:system-ui,sans-serif;margin:0;padding:1rem;
     background:#111;color:#eee;-webkit-text-size-adjust:100%}
h1{font-size:1rem;font-weight:600;color:#888;margin:0 0 .8rem}
a{color:#6aa9ff}
#dv{font-size:3rem;font-weight:700;text-align:center;padding:1rem 0;
    border-radius:14px;background:#1c1c1c}
#dv small{display:block;font-size:.75rem;font-weight:400;color:#888;margin-top:.3rem}
#ruhe{text-align:center;font-size:.85rem;margin:.4rem 0 .9rem;min-height:1.2em}
.ruhig{color:#38d66b}.unruhig{color:#e8b93b}.keineref{color:#e05252}
button{padding:.85rem;font-size:1rem;font-weight:600;border:0;border-radius:11px;
       background:#2a2a2a;color:#eee;cursor:pointer;width:100%}
button:active{background:#3a3a3a}
#ref{background:#1d4ed8;margin-bottom:.6rem}
input{width:100%;padding:.8rem;font-size:1rem;border-radius:11px;border:1px solid #333;
      background:#1c1c1c;color:#eee;margin-bottom:.6rem}
.gitter{display:grid;grid-template-columns:1fr 1fr;gap:.5rem;margin-bottom:.9rem}
table{width:100%;border-collapse:collapse;font-size:.8rem;
      font-variant-numeric:tabular-nums}
th{text-align:left;color:#777;font-weight:600;border-bottom:1px solid #333;padding:.3rem 0}
td{padding:.3rem 0;border-bottom:1px solid #242424;color:#ccc}
td:nth-child(2){text-align:right}
.fuss{display:flex;gap:.5rem;margin:.9rem 0}
</style></head><body>
<h1>Messreihe &nbsp;|&nbsp; <a href="/">zur Erkennung</a></h1>
<div id="dv">--<small>d|B| Unterschied zur Referenz</small></div>
<div id="ruhe"></div>
<button id="ref" onclick="ref()">Referenz setzen (freie Flaeche)</button>
<input id="ort" placeholder="Ort, z.B. Marktplatz" onchange="merk()">
<input id="auto" placeholder="Fahrzeug, z.B. Golf7" onchange="merk()">
<div class="gitter">
  <button onclick="sp('unter-vorne')">unter vorne</button>
  <button onclick="sp('unter-mitte')">unter mitte</button>
  <button onclick="sp('unter-hinten')">unter hinten</button>
  <button onclick="sp('neben-links')">neben links</button>
  <button onclick="sp('neben-rechts')">neben rechts</button>
  <button onclick="sp('nachbarplatz')">Nachbarplatz</button>
  <button onclick="sp('leer')">leerer Platz</button>
  <button onclick="frei()">frei...</button>
</div>
<div class="fuss">
  <button onclick="location='/csv'">CSV laden</button>
  <button onclick="if(confirm('Alle Messungen loeschen?'))fetch('/clear').then(hole)">loeschen</button>
</div>
<table><thead><tr><th>Bezeichnung</th><th>d|B|</th></tr></thead>
<tbody id="liste"></tbody></table>
<script>
function merk(){localStorage.auto=document.getElementById('auto').value;
                localStorage.ort=document.getElementById('ort').value}
if(localStorage.auto)document.getElementById('auto').value=localStorage.auto;
if(localStorage.ort)document.getElementById('ort').value=localStorage.ort;
function ref(){fetch('/ref').then(hole)}
function frei(){var p=prompt('Bezeichnung?');if(p)sp(p)}
function sp(pos){
 var a=document.getElementById('auto').value.trim();
 var o=document.getElementById('ort').value.trim();
 fetch('/save?name='+encodeURIComponent((o?o+'_':'')+(a?a+'_':'')+pos))
  .then(function(r){return r.text()}).then(function(t){
   if(t!=='ok')alert(t); hole();});
}
function hole(){
 fetch('/messstatus').then(function(r){return r.json()}).then(function(d){
  document.getElementById('dv').innerHTML=
    (d.ref?Math.round(d.dv*10)/10:'--')+'<small>d|B| Unterschied zur Referenz</small>';
  var r=document.getElementById('ruhe');
  if(d.laeuft){r.textContent='misst... '+d.fortschritt+'/'+d.proben+' -- nicht bewegen';r.className='unruhig';}
  else if(!d.ref){r.textContent='keine Referenz gesetzt';r.className='keineref';}
  else if(d.unruhe>3){r.textContent='noch unruhig ('+(Math.round(d.unruhe*10)/10)+') - stillhalten';r.className='unruhig';}
  else {r.textContent='ruhig  |  Referenz |B| = '+(Math.round(d.bRef*10)/10);r.className='ruhig';}
  document.getElementById('liste').innerHTML=d.liste.map(function(m){
    return '<tr><td>'+m.n+'</td><td>'+(Math.round(m.dv*10)/10)+
           ' &plusmn;'+(Math.round(m.s*100)/100)+'</td></tr>';
  }).reverse().join('');
 }).catch(function(){});
}
hole();setInterval(hole,500);
</script></body></html>)HTML";

// ---------------------------------------------------------------- Endpunkte

float messDv(const Messung &m)  { return fabsf(m.bWert - m.bRef); }
float messSem(const Messung &m) {
  if (!m.n) return 0.0f;
  return sqrtf(m.sigma * m.sigma + m.sigmaRef * m.sigmaRef) / sqrtf((float)m.n);
}

void handleStatus() {
  String j = "{";
  j += "\"ergebnis\":\"" + String(ergebnisText()) + "\",";
  j += "\"phase\":\"" + String(phaseText()) + "\",";
  j += "\"roh\":[" + String(roh[0]) + "," + String(roh[1]) + "," + String(roh[2]) + "],";
  j += "\"b\":" + String(bLangsam, 1) + ",";
  j += "\"bLeer\":" + String(kal.bLeer, 1) + ",";
  j += "\"bAuto\":" + String(kal.bAuto, 1) + ",";
  j += "\"schwelle\":" + String(kal.magic == KAL_MAGIC ? schwelleB() : 0.0f, 1) + ",";
  j += "\"abstand\":" + String(kalAbstand(), 1) + ",";
  j += "\"s\":" + String(aktuellS, 3) + ",";
  j += "\"mittelt\":" + String(messZweck == ZWECK_KAL_LEER || messZweck == ZWECK_KAL_AUTO ? 1 : 0) + ",";
  j += "\"mittelN\":" + String(messN) + ",";
  j += "\"sensor\":\"" + String(sensorOk ? sensorName : "FEHLER") + "\",";
  j += "\"anzahl\":" + String(serie.n);
  j += "}";
  server.send(200, "application/json", j);
}

void handleMessStatus() {
  String j = "{";
  j += "\"ref\":" + String(refGesetzt ? 1 : 0) + ",";
  j += "\"bRef\":" + String(bRef, 1) + ",";
  j += "\"dv\":" + String(refGesetzt ? fabsf(bLangsam - bRef) : 0.0f, 1) + ",";
  j += "\"unruhe\":" + String(unruhe(), 1) + ",";
  j += "\"laeuft\":" + String(messZweck == ZWECK_REF || messZweck == ZWECK_MESS ? 1 : 0) + ",";
  j += "\"fortschritt\":" + String(messN) + ",";
  j += "\"proben\":" + String(MESS_PROBEN) + ",";
  j += "\"liste\":[";
  uint16_t von = serie.n > 15 ? serie.n - 15 : 0;     // nur die letzten 15
  for (uint16_t i = von; i < serie.n; i++) {
    if (i > von) j += ",";
    j += "{\"n\":\"" + String(serie.m[i].name) + "\",";
    j += "\"dv\":" + String(messDv(serie.m[i]), 1) + ",";
    j += "\"s\":" + String(messSem(serie.m[i]), 2) + "}";
  }
  j += "],\"anzahl\":" + String(serie.n) + "}";
  server.send(200, "application/json", j);
}

void handleCsv() {
  String c = "nr;name;b_ref;b_mess;d_b;sigma_mess;sigma_ref;sem;n;"
             "ref_x;ref_y;ref_z;mess_x;mess_y;mess_z;t_ms\n";
  for (uint16_t i = 0; i < serie.n; i++) {
    const Messung &m = serie.m[i];
    c += String(i + 1) + ";" + m.name + ";";
    c += String(m.bRef, 1) + ";" + String(m.bWert, 1) + ";" + String(messDv(m), 1) + ";";
    c += String(m.sigma, 1) + ";" + String(m.sigmaRef, 1) + ";";
    c += String(messSem(m), 2) + ";" + String(m.n) + ";";
    for (uint8_t k = 0; k < 3; k++) c += String(m.ref[k], 1) + ";";
    for (uint8_t k = 0; k < 3; k++) c += String(m.wert[k], 1) + ";";
    c += String(m.t) + "\n";
  }
  server.sendHeader("Content-Disposition", "attachment; filename=carsensor.csv");
  server.send(200, "text/csv; charset=utf-8", c);
}

void webStart() {
#if defined(STA_SSID)
  WiFi.mode(WIFI_STA);
  WiFi.begin(STA_SSID, STA_PASS);
  Serial.printf("Verbinde mit WLAN \"%s\"", STA_SSID);
  for (uint8_t i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) { delay(250); Serial.print("."); }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) Serial.printf("Adresse: http://%s\n", WiFi.localIP().toString().c_str());
  else Serial.println(F("WLAN fehlgeschlagen -- Bedienung ueber die serielle Konsole."));
#else
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  apModus = true;
  // Captive Portal: jeder Hostname wird auf uns aufgeloest, damit auch eine
  // Suchanfrage aus der Adressleiste hier landet.
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", WiFi.softAPIP());
  Serial.printf("Accesspoint \"%s\" (Passwort \"%s\")\n", AP_SSID, AP_PASS);
  Serial.printf("Adresse: http://%s  (Captive Portal aktiv)\n", WiFi.softAPIP().toString().c_str());
#endif

  server.on("/",           []() { server.send_P(200, "text/html", SEITE); });
  server.on("/test",       []() { server.send_P(200, "text/html", SEITE_TEST); });
  server.on("/status",     handleStatus);
  server.on("/messstatus", handleMessStatus);
  server.on("/csv",        handleCsv);

  server.on("/reset",     []() { aktionReset();   handleStatus(); });
  server.on("/leerstart", []() { kalStart(true);  handleStatus(); });
  server.on("/leerstop",  []() { kalStop();       handleStatus(); });
  server.on("/autostart", []() { kalStart(false); handleStatus(); });
  server.on("/autostop",  []() { kalStop();       handleStatus(); });

  server.on("/ref", []() {
    mittelungStarten(ZWECK_REF, "referenz", MESS_PROBEN);
    handleMessStatus();
  });
  server.on("/save", []() {
    String name = server.hasArg("name") ? server.arg("name") : String("ohne_name");
    if (!refGesetzt)                    server.send(200, "text/plain", "Erst Referenz setzen!");
    else if (serie.n >= MAX_MESSUNGEN)  server.send(200, "text/plain", "Liste voll - CSV laden und loeschen");
    else if (messZweck != ZWECK_NICHTS) server.send(200, "text/plain", "Messung laeuft noch");
    else { mittelungStarten(ZWECK_MESS, name, MESS_PROBEN); server.send(200, "text/plain", "ok"); }
  });
  server.on("/clear", []() {
    serie.n = 0;
    serieSpeichern();
    Serial.println(F("[clear] Messreihe geloescht."));
    handleMessStatus();
  });

  // Alles Unbekannte auf die Startseite umlenken. Das faengt zugleich die
  // Erreichbarkeitspruefungen der Handys ab (Android ruft
  // connectivitycheck.gstatic.com/generate_204 auf, iOS captive.apple.com):
  // eine 302 statt der erwarteten Antwort signalisiert "Portal".
  server.onNotFound([]() {
    if (apModus) {
      server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
      server.send(302, "text/plain", "");
    } else {
      server.send(404, "text/plain", "?");
    }
  });
  server.begin();
}

// ---------------------------------------------------------------- Konsole

void konsole() {
  while (Serial.available()) {
    switch (Serial.read()) {
      case 'n': aktionReset();   break;
      case '1': kalStart(true);  break;
      case '2': kalStop();       break;
      case '3': kalStart(false); break;
      case '4': kalStop();       break;
      case 'r': mittelungStarten(ZWECK_REF, "referenz", MESS_PROBEN); break;
      case 'm': mittelungStarten(ZWECK_MESS, "konsole", MESS_PROBEN); break;
      case 'x': serie.n = 0; serieSpeichern(); Serial.println(F("[clear] Messreihe geloescht.")); break;
      case 't': selbsttest();    break;
      case 'l':
        Serial.printf("Messreihe (%u):\n", serie.n);
        for (uint16_t k = 0; k < serie.n; k++)
          Serial.printf("  %2u %-24s d|B|=%8.1f +-%.2f\n",
                        k + 1, serie.m[k].name, messDv(serie.m[k]), messSem(serie.m[k]));
        break;
      case 'i':
        Serial.printf("Phase: %s | Ergebnis: %s | s=%.3f\n", phaseText(), ergebnisText(), aktuellS);
        Serial.printf("  |B| leer %.1f (sigma %.1f) | Auto %.1f (sigma %.1f) | Unterschied %.1f | Schwelle %.1f\n",
                      kal.bLeer, kal.sLeer, kal.bAuto, kal.sAuto, kalAbstand(), schwelleB());
        Serial.printf("  |B| aktuell %.1f | Messungen %u | Referenz %s %.1f\n",
                      bLangsam, serie.n, refGesetzt ? "gesetzt" : "fehlt", bRef);
        break;
      default: break;
    }
  }
}

// ---------------------------------------------------------------- Setup/Loop

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println();
  Serial.println(F("=== Carsensor ==="));
  Serial.printf("SDA=GPIO%d  SCL=GPIO%d\n", PIN_SDA, PIN_SCL);

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000);
  delay(100);

  if (detectSensor()) Serial.printf("Sensor: %s\n", sensorName);
  else                Serial.println(F("Kein Magnetometer gefunden (0x2C / 0x0D / 0x1E)."));

  if (kalLaden()) {
    phase = PHASE_ERKENNUNG;
    Serial.printf("Kalibrierung geladen: leer %.1f, Auto %.1f, Unterschied %.1f%s\n",
                  kal.bLeer, kal.bAuto, kalAbstand(), kalBrauchbar() ? "" : "  ACHTUNG: zu klein");
  } else {
    phase = PHASE_LEER;
    Serial.println(F("Keine Kalibrierung gespeichert -- bitte kalibrieren."));
  }

  serieLaden();
  Serial.printf("Messreihe: %u gespeicherte Messungen\n", serie.n);

  webStart();
  Serial.println(F("Konsole: n = reset, 1/2 = leer Start/Stop, 3/4 = Auto Start/Stop"));
  Serial.println(F("         r = Referenz, m = Messung, l = Liste, x = Liste loeschen"));
  Serial.println(F("         i = Info, t = Selbsttest"));
}

void loop() {
  if (apModus) dns.processNextRequest();
  server.handleClient();
  konsole();

  static uint32_t tSample = 0, tPrint = 0;
  uint32_t jetzt = millis();

  if (jetzt - tSample >= SAMPLE_MS) {
    tSample = jetzt;
    messen();
  }

  if (jetzt - tPrint >= 1000) {
    tPrint = jetzt;
    Serial.printf("%-9s | |B|=%8.1f | s=%6.3f | roh %6d %6d %6d\n",
                  ergebnisText(), bLangsam, aktuellS, roh[0], roh[1], roh[2]);
  }
}
