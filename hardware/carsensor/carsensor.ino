/*
 * Carsensor -- Belegungserkennung ueber Magnetfeldstoerung
 * ESP32-C3 + GY-271 (QMC5883P / QMC5883L / HMC5883L)
 *
 * Verdrahtung (SDA und SCL sind gegenueber der ueblichen Reihenfolge
 * vertauscht, so ist es hier verloetet):
 *   VCC -> 3V3
 *   GND -> G / GND     (NICHT auf GPIO0)
 *   SDA -> GPIO 5
 *   SCL -> GPIO 4
 *
 * ------------------------------------------------------------------------
 * MESSGROESSE: der Stoervektor dB = B - B0 gegen eine mitlaufende
 * Leerfeld-Referenz B0, gerechnet in Mikrotesla.
 *
 * Frueher wurde |B| gegen zwei eingelernte Punkte (leer / Auto) verglichen.
 * Das war drehinvariant, aber ortsgebunden: der Absolutwert von |B| haengt
 * am Bewehrungsstahl des jeweiligen Platzes, also musste jeder Sensor vor
 * Ort eingelernt werden. Jetzt wird B0 selbst laufend nachgefuehrt und nur
 * noch die Abweichung bewertet -- damit gilt eine Schwelle in Mikrotesla
 * ueberall gleich, und Lerndaten sind zwischen Parkplaetzen uebertragbar.
 *
 * DREHINVARIANZ bleibt erhalten, wird aber nicht mehr durch Wegwerfen der
 * Richtung erkauft. Aus dB und B0 lassen sich Groessen bilden, die von der
 * Einbaudrehung des Sensors unabhaengig sind, weil beide Vektoren im selben
 * Rahmen gemessen werden:
 *
 *   dAbs  = |dB|                        Staerke der Stoerung
 *   dPar  = dB * B0hut                  Anteil laengs des Referenzfeldes
 *   dPerp = |dB - dPar * B0hut|         Anteil quer dazu
 *   theta = atan2(dPerp, dPar)          Winkel zwischen Stoerung und B0
 *
 * dPar allein ist ungefaehr das alte d|B| -- und zeigt, warum das alte Mass
 * zu schwach war: steht die Stoerung quer zum Erdfeld, ist dPar ~ 0,
 * waehrend dAbs voll ausschlaegt.
 *
 * SPLASHOVER (Nachbarfeldansprechen) ist der eigentliche Gegner: ein Lkw
 * drei Meter daneben erzeugt dieselbe Amplitude wie ein Kleinwagen direkt
 * darueber. Ueber dAbs allein ist das grundsaetzlich nicht trennbar --
 * unterschiedlich ist die RICHTUNG der Stoerung, also theta. Deshalb wird
 * (dAbs, theta) als Merkmalspaar gefuehrt und im Korpus mitgeschrieben.
 *
 * ZWEI GRENZEN, beide wichtig:
 *
 * 1. theta ist drehinvariant und trennt deshalb links nicht von rechts --
 *    beide liegen spiegelsymmetrisch zum Erdfeld. Dafuer gibt es die
 *    Einbaulage: wird der Sensor nach EINBAU_HINWEIS ausgerichtet, tragen
 *    zusaetzlich die Komponenten von dB in Sensorkoordinaten (im Korpus als
 *    d_x/d_y/d_z mitgeschrieben).
 *
 * 2. Drehinvariant heisst hier: gegen eine GEMEINSAME Drehung von B und B0,
 *    also gegen die Einbaulage. NICHT dagegen, dass der Sensor zwischen
 *    Referenz und Messung verkippt -- bei |B| ~ 50 uT sind das rund
 *    0,87 uT je Grad, fuenf Grad also mehr als die Erkennungsschwelle. Fuer
 *    den festverbauten Detektor spielt das keine Rolle. Wird der Sensor von
 *    Hand unter ein Auto geschoben, misst dAbs dagegen die Schieflage statt
 *    des Fahrzeugs.
 *
 *    Deshalb laeuft das alte Mass als dBetrag = |B| - |B0| weiter mit: der
 *    Betrag aendert sich unter Drehung nicht, ist also kippfest, sieht dafuer
 *    nur den Anteil laengs des Erdfelds. Jede Beobachtung im Korpus traegt
 *    die Aufnahmeart (fest / handgefuehrt); beim Training duerfen die
 *    Vektormerkmale nur aus festen Aufnahmen kommen.
 * ------------------------------------------------------------------------
 *
 * Bedienung ueber Handy:
 *   1. Mit dem WLAN "Carsensor" verbinden (Passwort siehe unten).
 *      Die Meldung "kein Internetzugriff" bestaetigen und verbunden bleiben.
 *   2. Meist oeffnet sich die Seite von selbst (Captive Portal). Sonst im
 *      Browser  192.168.4.1/  eintippen -- ohne http://, mit Schraegstrich
 *      am Ende. Dank Portal fuehrt aber auch jede andere Adresse hierher.
 *
 * Startseite: zeigt frei/belegt und die Referenz. Kein Einlernen mehr --
 * der Sensor bestimmt B0 bei JEDEM Start selbst: nach Einschalten oder Reset
 * gilt der Platz als frei, das Feld der ersten 10 s ist die Referenz. Danach
 * wird sie gegen Temperatur- und Tagesgang nachgefuehrt. "Referenz neu"
 * erzwingt eine Neubestimmung, wenn der Platz frei ist.
 *
 * Korpus (Seite /korpus): Lerndaten aufnehmen. Ort, Sensor, Aufnahmeart und
 * Fahrzeugklasse eintragen, dann den Knopf fuer den Belegungsfall druecken.
 * Je 100 Einzelwerte mit Standardfehler. CSV-Export unter /csv -- die CSVs
 * mehrerer Parkplaetze bilden zusammen den Trainingssatz.
 *
 * Aus diesem Trainingssatz entsteht die Tabelle MODELL[] weiter unten: je
 * Klasse und Belegungsfall der Schwerpunkt in (dAbs, theta). Solange sie
 * leer ist, sagt der Sensor nur frei/belegt und kann Splashover nicht
 * verwerfen.
 *
 * Serielle Konsole, zeilenweise (Befehl, dann Enter -- CR oder LF genuegt):
 *   n = reset, r = Referenz neu, m = Beobachtung, l = Liste,
 *   x = Liste loeschen, i = Info, s = Selbsttest, f = LoRa-Meldung erzwingen,
 *   t <uT> = Schwelle setzen (ohne Wert: anzeigen), bleibt gespeichert,
 *   a <alpha> = Gewicht des neuen Werts im Mittel (Vorgabe 0.1), gespeichert.
 * Sobald ein Zeichen ankommt, pausiert die Sekundenzeile bis zum Enter,
 * damit die Eingabe nicht zwischen die Ausgaben geraet.
 *
 * ERKENNUNG (Wechseldetektor): ein laufendes Mittel je Achse,
 *   mittel = 0,9 * mittel + 0,1 * neu   (ALPHA_MITTEL, per "a <wert>" aenderbar),
 * laeuft vom ersten Sample an und immer weiter, auch waehrend und nach einem
 * Ereignis. Bewertet wird der Abstand des AKTUELLEN Werts zum Mittel davor,
 * d = neu - mittel, dAbs = |d|. Ueberschreitet dAbs die Schwelle, ist das ein
 * Ereignis und der Zustand kippt: leer -> Auto -> leer. Nach dem Einschalten
 * gilt "leer". Weil das Mittel dem Fahrzeug nachlaeuft, faellt dAbs danach
 * wieder auf 0; das Wegfahren ist der naechste Ausschlag. Die ersten
 * ENTSCHEIDUNG_AB Werte loesen kein Ereignis aus (Einschwingen des Mittels).
 * Grenze: ein sehr langsam heranrollendes Fahrzeug wird vom Mittel "mitgelernt"
 * -- dann alpha verkleinern (a 0.02), das Mittel wird traeger.
 *
 * Sekundenzeile: x y z = laufendes Mittel je Achse (uT), dx dy dz = aktueller
 * Wert minus Mittel, abs = |Mittel|, dAbs = |d| = sqrt(dx^2+dy^2+dz^2).
 *
 * MELDUNG AN DAS GATEWAY (optional, USE_LORA):
 *   Mit angeloetetem SX1278 (433 MHz) meldet der Sensor jeden Wechsel
 *   belegt/frei per LoRa an das Gateway, dazu alle 15 min ein Lebenszeichen.
 *   Verdrahtung und Protokoll stehen in ../../gateway/README.md. Ohne
 *   Funkmodul einfach USE_LORA auf 0 setzen -- dann bleibt es beim
 *   Captive Portal wie bisher.
 */

#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <math.h>

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

// Einbaukonvention. Nur noetig, damit links/rechts trennbar wird -- die
// drehinvarianten Merkmale gelten auch ohne sie.
#define EINBAU_HINWEIS "X-Achse zeigt in Fahrtrichtung, Z nach oben"

// --- LoRa: Meldung an das Gateway ---------------------------------------
// Auf 0 setzen, wenn kein Funkmodul angeloetet ist -- der Sensor laeuft dann
// wie bisher, nur ohne zu melden. Alles Weitere: ../../gateway/README.md
#define USE_LORA 1

#define LORA_KNOTEN  ""              // leer = Kennung aus der MAC ("PS-A1B2C3")
#define LORA_HEARTBEAT_S    900      // Lebenszeichen alle 15 min
#define LORA_WIEDERHOLUNGEN 3        // je Ereignis, es gibt keine Bestaetigung
#define LORA_WIEDERHOL_MS   1500     // Grundabstand der Wiederholungen

// SX1278 (433 MHz). GPIO 2, 8 und 9 bleiben frei -- Strapping-Pins, ein falscher
// Pegel beim Einschalten verhindert den Start. GPIO 4 und 5 hat das
// Magnetometer.
#define LORA_SCK   6
#define LORA_MISO  1
#define LORA_MOSI  7
#define LORA_NSS  10
#define LORA_RST   3
#define PIN_VBAT  -1                 // GPIO des Spannungsteilers, -1 = keiner

// SX1278 = 433-MHz-Baureihe (137..525 MHz). Nicht 868 -- das kann der Chip
// gar nicht. 433,5 statt der ueblichen 433,92: dort sitzen Autoschluessel,
// Funkthermometer und Garagentore, und die stoeren sonst dauernd.
#define LORA_FREQ     433.5E6
#define LORA_SF       7
#define LORA_BW       125E3
#define LORA_CR       5
#define LORA_SYNCWORD 0x2A           // muss zum Gateway passen
// 10 dBm = 10 mW: die Obergrenze im 433-MHz-Band. Mehr kann der Chip, darf
// aber nicht. Siehe ../../gateway/README.md.
#define LORA_TX_DBM   10

// Erst hier, nicht oben bei den uebrigen Includes: davor ist USE_LORA noch
// nicht definiert, und die Bedingung waere immer falsch.
#if USE_LORA
#include <SPI.h>
#include <LoRa.h>                    // arduino-cli lib install LoRa
#endif

#define SAMPLE_MS       50           // Messintervall -> 20 Hz
#define ALPHA_LANGSAM   0.02f        // ruhiges Mittel, nur fuer die Unruhe
#define ALPHA_MITTEL    0.1f         // Erkennung: mittel = 0,9*alt + 0,1*neu; "a <wert>"
#define MESS_PROBEN     100          // 100 x SAMPLE_MS = 5 s pro Beobachtung

// Erst ab so vielen Messungen wird entschieden. Das Mittel startet auf dem
// ersten Einzelwert und ist davor noch nicht eingeschwungen -- ein Ausreisser
// im ersten Sample wuerde sonst sofort als Fahrzeug gemeldet.
#define ENTSCHEIDUNG_AB 10

// Erkennungsschwelle in Mikrotesla, mit Hysterese. Ortsunabhaengig, weil
// sie auf der Aenderung gegen das laufende Mittel sitzt, nicht auf |B| selbst.
// Ein Pkw ueber dem Sensor liegt bei einigen zehn uT, das Erdfeld bei ~50.
#define SCHWELLE_EIN_UT  3.0f        // Vorgabe; zur Laufzeit mit "t <uT>" aenderbar
#define SCHWELLE_AUS_UT  2.0f        // legt nur das Verhaeltnis aus/ein fest

// Zusaetzlich eine rauschabhaengige Untergrenze: an Plaetzen mit Bahnstrom
// oder Trafo ist die Referenz unruhiger, dort muss die Schwelle mitwachsen.
#define SCHWELLE_SIGMA   6.0f

// Nachfuehrung der Referenz. Zeitkonstante 10 min -- langsam genug, dass ein
// einparkendes Fahrzeug nicht mitgelernt wird, schnell genug fuer den
// Tagesgang des Erdfelds (typ. 20..50 nT) und den Temperaturgang des Chips.
#define REF_TAU_S        600.0f
#define REF_ERFASSUNG_MS 10000UL     // Erstbestimmung beim Start
#define REF_RUHE_UT      0.5f        // nachfuehren nur, wenn es ruhig ist


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
enum Zweck { ZWECK_NICHTS, ZWECK_REFERENZ, ZWECK_BEOBACHTUNG };

// Fahrzeugklassen, angelehnt an die TLS-Klassen der BASt (5+1), ergaenzt um
// die Unterteilung des Pkw, auf die es hier ankommt: ein Kleinwagen bringt
// deutlich weniger Eisen mit als ein SUV.
enum Klasse : uint8_t {
  KL_FREI, KL_KRAD, KL_PKW_KLEIN, KL_PKW, KL_TRANSPORTER, KL_LKW, KL_BUS, KL_SONST,
  KL_ANZAHL
};
const char *const KLASSE_NAME[KL_ANZAHL] = {
  "frei", "Krad", "Pkw-klein", "Pkw", "Transporter", "Lkw", "Bus", "sonstiges"
};

// Belegungsfall: die geometrische Lage der Masse zum Sensor. Das ist das
// Etikett, an dem sich Splashover ueberhaupt erst lernen laesst -- ohne die
// "neben"-Faelle im Korpus kann kein Modell sie spaeter verwerfen.
enum Fall : uint8_t {
  FA_FREI, FA_AUF_MITTE, FA_AUF_VORNE, FA_AUF_HINTEN,
  FA_NEBEN_LINKS, FA_NEBEN_RECHTS, FA_DAVOR, FA_DAHINTER, FA_NACHBARPLATZ,
  FA_ANZAHL
};
const char *const FALL_NAME[FA_ANZAHL] = {
  "frei", "auf-mitte", "auf-vorne", "auf-hinten",
  "neben-links", "neben-rechts", "davor", "dahinter", "nachbarplatz"
};

// 64 statt mehr wegen der NVS-Partition: ein Blob wird beim Aktualisieren
// erst neu geschrieben und dann der alte geloescht, es liegen also kurz
// beide da. sizeof(Korpus) = 5896 B -> ~11,8 KB kurzzeitig, gegen rund
// 16 KB nutzbaren Raum in der 20-KB-Partition "nvs". 80 waeren zu knapp.
#define MAX_BEOBACHTUNGEN 64
#define ORT_LEN           24
#define SENSOR_LEN        12
#define KORPUS_MAGIC      0x4B4F5231   // "KOR1"
#define REF_MAGIC         0x52454631   // "REF1"

// Drehinvariante Merkmale. Genau diese vier Zahlen sind zwischen
// Parkplaetzen vergleichbar -- alles andere haengt an Ort und Einbaulage.
struct Merkmale {
  float dAbs;     // |dB| in uT
  float dPar;     // Anteil laengs B0, in uT (kann negativ sein)
  float dPerp;    // Anteil quer zu B0, in uT (immer >= 0)
  float theta;    // Winkel zwischen dB und B0, in Grad (0..180)
  float dBetrag;  // |B| - |B0| in uT -- das alte Mass, siehe unten
};

// Aufnahmeart. Entscheidet, welche Merkmale einer Zeile ueberhaupt gelten.
//
// dAbs, dPar, dPerp und theta sind drehinvariant gegen eine GEMEINSAME
// Drehung von B und B0 -- also gegen die Einbaulage. Sie sind NICHT
// invariant dagegen, dass der Sensor zwischen Referenz und Messung
// verkippt: bei |B| ~ 50 uT verschiebt 1 Grad den Vektor um rund 0,87 uT,
// fuenf Grad also um mehr als die Erkennungsschwelle. Wird der Sensor von
// Hand unter ein Auto geschoben, misst dAbs die Schieflage statt des Autos.
//
// dBetrag = |B| - |B0| ist dagegen kippfest, weil der Betrag sich unter
// Drehung nicht aendert -- dafuer sieht es nur den Anteil der Stoerung
// laengs des Erdfelds und ist entsprechend schwach.
//
// Deshalb steht in jeder Zeile beides, und daneben, wie sie entstanden ist:
// beim Training duerfen die Vektormerkmale nur aus AUF_FEST kommen.
enum Aufnahme : uint8_t { AUF_FEST, AUF_HAND, AUF_ANZAHL };
const char *const AUFNAHME_NAME[AUF_ANZAHL] = { "fest", "handgefuehrt" };

struct Referenz {
  uint32_t magic;
  float    b0[3];      // Leerfeld in uT, Sensorkoordinaten
  float    rauschen;   // Streuung von |dB| im freien Zustand, uT
  uint32_t stand;      // millis der letzten Neubestimmung
};

struct Beobachtung {
  char     ort[ORT_LEN];
  char     sensorId[SENSOR_LEN];
  uint8_t  klasse;             // Klasse
  uint8_t  fall;               // Fall
  uint8_t  art;                // Aufnahme
  float    b0[3];              // Referenzfeld zur Zeit der Aufnahme, uT
  float    b[3];               // gemessenes Feld, uT
  Merkmale m;
  float    sigma;              // Streuung der |B|-Einzelwerte, uT
  uint16_t n;                  // Zahl der gemittelten Einzelwerte
  uint32_t t;
};

struct Korpus {
  uint32_t    magic;
  uint16_t    n;
  Beobachtung b[MAX_BEOBACHTUNGEN];
};

// Belegt der Fall den eigenen Platz? Alles ab FA_NEBEN_LINKS ist Splashover
// und muss als "frei" gelten, auch wenn es kraeftig ausschlaegt.
inline bool fallIstBelegt(uint8_t f) { return f >= FA_AUF_MITTE && f <= FA_AUF_HINTEN; }

SensorType sensor = SENSOR_NONE;
const char *sensorName = "-";
float scale_uT = 0.0f;               // Umrechnung Zaehler -> Mikrotesla

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
  scale_uT = 100.0f / QMC5883P_LSB_PER_GAUSS;               // 1 G = 100 uT
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

enum RefStatus { REF_FEHLT, REF_ERFASSUNG, REF_GUELTIG };

Korpus   korpus;
Referenz ref;
RefStatus refStatus = REF_FEHLT;

int16_t roh[3] = {0, 0, 0};       // letzte Rohwerte, nur zur Anzeige
float   bVek[3]     = {0, 0, 0};  // letzter Einzelwert in uT
float   bMittel[3]  = {0, 0, 0};  // laufendes Mittel fuer die Erkennung (ALPHA_MITTEL)
float   dVek[3]     = {0, 0, 0};  // aktueller Wert minus Mittel davor, uT
float   dAbsAkt     = 0;          // |dVek|
float   alphaMittel = ALPHA_MITTEL;  // per Konsole "a <wert>", in NVS gehalten
float   bLangsam[3] = {0, 0, 0};  // ruhiges Mittel, nur fuer die Unruhe
bool    avgInit  = false;
uint16_t nMessungen = 0;          // seit avgInit, saettigend -- fuer ENTSCHEIDUNG_AB
bool    sensorOk = false;

Merkmale merk = {0, 0, 0, 0, 0};  // Merkmale des aktuellen Zustands
bool     belegt = false;
bool     ausschlag = false;       // dAbs ueber der Schwelle, bis es unter "aus" faellt

uint8_t  klasseErkannt = KL_FREI;
uint8_t  fallErkannt   = FA_FREI;
bool     modellTraf    = false;

// Beschriftung fuer neue Beobachtungen
char ortName[ORT_LEN]       = "";
char sensorId[SENSOR_LEN]   = "";

// Laufende Mittelung. Laeuft nebenher, damit der Webserver bedienbar bleibt.
Zweck    messZweck = ZWECK_NICHTS;
uint16_t messN = 0, messZiel = 0;
double   sumV[3] = {0, 0, 0}, sumQ[3] = {0, 0, 0};
uint8_t  messKlasse = KL_FREI, messFall = FA_FREI, messArt = AUF_FEST;

float betrag3(const float v[3]) {
  return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

float punkt3(const float a[3], const float b[3]) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// ---------------------------------------------------------------- Merkmale

// Zerlegt die Stoerung in den Anteil laengs des Referenzfeldes und den quer
// dazu. Beide Vektoren stecken im selben Sensorrahmen, deshalb faellt eine
// gemeinsame Drehung heraus -- das Ergebnis haengt nicht an der Einbaulage.
Merkmale merkmale(const float b[3], const float b0[3]) {
  Merkmale m = {0, 0, 0, 0, 0};
  float d[3] = { b[0] - b0[0], b[1] - b0[1], b[2] - b0[2] };
  m.dAbs = betrag3(d);

  float n0 = betrag3(b0);
  m.dBetrag = betrag3(b) - n0;      // kippfest, aber schwach
  if (n0 < 1e-6f) return m;               // ohne Referenz keine Zerlegung
  float e[3] = { b0[0] / n0, b0[1] / n0, b0[2] / n0 };

  m.dPar = punkt3(d, e);
  float q[3] = { d[0] - m.dPar * e[0], d[1] - m.dPar * e[1], d[2] - m.dPar * e[2] };
  m.dPerp = betrag3(q);
  m.theta = atan2f(m.dPerp, m.dPar) * 57.29578f;
  return m;
}

// ---------------------------------------------------------------- Speicher

void refSpeichern() {
  ref.magic = REF_MAGIC;
  prefs.begin("carsensor", false);
  prefs.putBytes("ref", &ref, sizeof(ref));
  prefs.end();
}

// Gespeichert wird die Referenz weiterhin (Einsicht, Nachfuehrstand), aber
// beim Start nicht mehr geladen -- siehe setup(): der Platz gilt dann als frei.

void korpusSpeichern() {
  korpus.magic = KORPUS_MAGIC;
  prefs.begin("carsensor", false);
  size_t n = prefs.putBytes("korpus", &korpus, sizeof(korpus));
  prefs.end();
  if (n != sizeof(korpus)) Serial.println(F("  FEHLER: Korpus konnte nicht gespeichert werden!"));
}

void korpusLaden() {
  prefs.begin("carsensor", true);
  size_t n = prefs.getBytes("korpus", &korpus, sizeof(korpus));
  prefs.end();
  if (n != sizeof(korpus) || korpus.magic != KORPUS_MAGIC || korpus.n > MAX_BEOBACHTUNGEN) {
    memset(&korpus, 0, sizeof(korpus));
    korpus.magic = KORPUS_MAGIC;
  }
}

void beschriftungSpeichern() {
  prefs.begin("carsensor", false);
  prefs.putString("ort", ortName);
  prefs.putString("sid", sensorId);
  prefs.end();
}

void beschriftungLaden() {
  prefs.begin("carsensor", true);
  String o = prefs.getString("ort", "");
  String s = prefs.getString("sid", "");
  prefs.end();
  strncpy(ortName,  o.c_str(), ORT_LEN - 1);
  strncpy(sensorId, s.c_str(), SENSOR_LEN - 1);
}

// ---------------------------------------------------------------- Schwelle

// Feste Schwelle in uT, aber nie unter dem Rauschen des Platzes: an einer
// Strassenbahn oder einem Trafo ist die Referenz unruhiger, dort muss die
// Schwelle mitwachsen, sonst spricht der Detektor auf Netzstoerungen an.
float schwelleUT = SCHWELLE_EIN_UT;  // per Konsole "t <uT>", in NVS gehalten

float schwelleEin() {
  float s = SCHWELLE_SIGMA * ref.rauschen;
  return s > schwelleUT ? s : schwelleUT;
}

void schwelleSpeichern() {
  prefs.begin("carsensor", false);
  prefs.putFloat("schw", schwelleUT);
  prefs.end();
}

void schwelleLaden() {
  prefs.begin("carsensor", true);
  schwelleUT = prefs.getFloat("schw", SCHWELLE_EIN_UT);
  prefs.end();
  if (!(schwelleUT > 0.0f) || schwelleUT > 1000.0f) schwelleUT = SCHWELLE_EIN_UT;
}

void alphaSpeichern() {
  prefs.begin("carsensor", false);
  prefs.putFloat("alpha", alphaMittel);
  prefs.end();
}

void alphaLaden() {
  prefs.begin("carsensor", true);
  alphaMittel = prefs.getFloat("alpha", ALPHA_MITTEL);
  prefs.end();
  if (!(alphaMittel > 0.0f) || alphaMittel > 1.0f) alphaMittel = ALPHA_MITTEL;
}

float schwelleAus() {
  return schwelleEin() * (SCHWELLE_AUS_UT / SCHWELLE_EIN_UT);
}

// ---------------------------------------------------------------- Modell

// Prototypen fuer die Zuordnung von Klasse und Belegungsfall. Der Inhalt
// entsteht NICHT hier, sondern aus den CSVs mehrerer Parkplaetze: je Klasse
// und Fall der Schwerpunkt in (dAbs, theta). Solange MODELL_N 0 ist, laeuft
// nur die Schwellenerkennung -- der Detektor sagt frei/belegt, aber nichts
// ueber das Fahrzeug, und Splashover wird nicht verworfen.
struct Prototyp {
  uint8_t klasse;
  uint8_t fall;
  float   dAbs;      // uT
  float   theta;     // Grad
};

const Prototyp MODELL[] = {
  { KL_PKW, FA_AUF_MITTE, 0.0f, 0.0f },   // Platzhalter, siehe MODELL_N
};
const uint8_t MODELL_N = 0;

// Merkmale haben verschiedene Einheiten, also wird vor dem Abstand skaliert.
// Die beiden Werte sind die Streubreite, die man je Klasse im Korpus sieht;
// sie gehoeren beim Training mit bestimmt.
#define MODELL_SKAL_DABS   2.0f      // uT
#define MODELL_SKAL_THETA  15.0f     // Grad
#define MODELL_MAX_DIST    2.0f      // darueber: unbekannt

// Naechster Nachbar in (dAbs, theta). Bewusst kein Netz: 80 Beobachtungen
// je Platz sind zu wenig fuer mehr, und ein kNN laesst sich aus der CSV
// nachvollziehen, ohne die Firmware neu zu denken.
bool klassifiziere(const Merkmale &m, uint8_t &klasse, uint8_t &fall) {
  if (MODELL_N == 0) return false;
  float best = 1e30f;
  uint8_t bi = 0;
  for (uint8_t i = 0; i < MODELL_N; i++) {
    float a = (m.dAbs  - MODELL[i].dAbs)  / MODELL_SKAL_DABS;
    float b = (m.theta - MODELL[i].theta) / MODELL_SKAL_THETA;
    float d = a * a + b * b;
    if (d < best) { best = d; bi = i; }
  }
  if (sqrtf(best) > MODELL_MAX_DIST) return false;
  klasse = MODELL[bi].klasse;
  fall   = MODELL[bi].fall;
  return true;
}

// ---------------------------------------------------------------- Mittelung

void mittelungStarten(Zweck z, uint16_t ziel, uint8_t klasse = KL_FREI,
                      uint8_t fall = FA_FREI, uint8_t art = AUF_FEST) {
  messZweck = z;
  messZiel  = ziel;
  messN     = 0;
  messKlasse = klasse;
  messFall   = fall;
  messArt    = art;
  for (uint8_t i = 0; i < 3; i++) { sumV[i] = 0; sumQ[i] = 0; }
}

void mittelungAbbrechen() { messZweck = ZWECK_NICHTS; }

void referenzNeu() {
  refStatus = REF_ERFASSUNG;
  mittelungStarten(ZWECK_REFERENZ, REF_ERFASSUNG_MS / SAMPLE_MS);
  Serial.println(F("[referenz] bestimme Leerfeld -- Platz muss frei sein."));
}

void mittelungFertig() {
  if (messN < 2) { messZweck = ZWECK_NICHTS; return; }

  float mv[3], var[3];
  for (uint8_t i = 0; i < 3; i++) {
    mv[i] = sumV[i] / messN;
    double v = sumQ[i] / messN - (double)mv[i] * mv[i];        // E[x^2] - E[x]^2
    var[i] = v > 0 ? (float)(v * messN / (messN - 1)) : 0.0f;  // Bessel-Korrektur
  }
  // Streuung des Vektors als Ganzes: die Wurzel aus der Summe der
  // Komponentenvarianzen ist der Effektivwert von |Einzelwert - Mittel|,
  // also genau das Rauschen, gegen das dAbs bewertet wird.
  float sigma = sqrtf(var[0] + var[1] + var[2]);
  // Der Fehler des MITTELWERTS ist um sqrt(N) kleiner als die Streuung der
  // Einzelwerte -- das ist der Fehlerbalken.
  float fehler = sigma / sqrtf((float)messN);

  switch (messZweck) {
    case ZWECK_REFERENZ:
      for (uint8_t i = 0; i < 3; i++) ref.b0[i] = mv[i];
      ref.rauschen = fehler;      // Rauschen des 100er-Mittels, nicht des Einzelwerts
      ref.stand = millis();
      refSpeichern();
      refStatus = REF_GUELTIG;
      Serial.printf("[referenz] B0 = %.2f %.2f %.2f uT  |B0|=%.2f  Rauschen %.3f uT\n",
                    ref.b0[0], ref.b0[1], ref.b0[2], betrag3(ref.b0), ref.rauschen);
      Serial.printf("  Schwelle: ein %.2f uT, aus %.2f uT\n", schwelleEin(), schwelleAus());
      break;

    case ZWECK_BEOBACHTUNG:
      if (korpus.n < MAX_BEOBACHTUNGEN) {
        Beobachtung &o = korpus.b[korpus.n];
        memset(&o, 0, sizeof(o));
        strncpy(o.ort, ortName, ORT_LEN - 1);
        strncpy(o.sensorId, sensorId, SENSOR_LEN - 1);
        o.klasse = messKlasse;
        o.fall   = messFall;
        o.art    = messArt;
        for (uint8_t i = 0; i < 3; i++) { o.b0[i] = ref.b0[i]; o.b[i] = mv[i]; }
        o.m     = merkmale(mv, ref.b0);
        o.sigma = sigma;
        o.n     = messN;
        o.t     = millis();
        korpus.n++;
        korpusSpeichern();
        Serial.printf("[korpus %u] %-12s %-13s %-12s dAbs=%7.2f +-%.3f uT  theta=%6.1f  d|B|=%7.2f\n",
                      korpus.n, KLASSE_NAME[o.klasse], FALL_NAME[o.fall],
                      AUFNAHME_NAME[o.art], o.m.dAbs, fehler, o.m.theta, o.m.dBetrag);
        if (o.art == AUF_HAND)
          Serial.println(F("  Hinweis: handgefuehrt -- nur d|B| ist kippfest, dAbs/theta nicht."));
      }
      break;

    default:
      break;
  }
  messZweck = ZWECK_NICHTS;
}

// ---------------------------------------------------------------- Erkennung

// Ruhig? Liegen schnelles und langsames Mittel dicht beieinander, hat sich
// nichts mehr bewegt -- eine Beobachtung ist verwertbar und die Referenz
// darf nachgefuehrt werden.
float unruhe() {
  float d[3] = { bMittel[0] - bLangsam[0], bMittel[1] - bLangsam[1], bMittel[2] - bLangsam[2] };
  return betrag3(d);
}

void referenzNachfuehren() {
  if (refStatus != REF_GUELTIG) return;
  // Nur im ruhigen freien Zustand nachfuehren, sonst lernt die Referenz das
  // einparkende Fahrzeug mit und der Detektor wird blind.
  if (ausschlag || unruhe() > REF_RUHE_UT || merk.dAbs > schwelleAus()) return;

  const float alpha = (SAMPLE_MS / 1000.0f) / REF_TAU_S;
  for (uint8_t i = 0; i < 3; i++) ref.b0[i] += alpha * (bMittel[i] - ref.b0[i]);

  // Nicht bei jedem Schritt in den Flash -- das waere Verschleiss ohne Nutzen.
  static uint32_t tSpeicher = 0;
  if (millis() - tSpeicher > 3600000UL) { tSpeicher = millis(); refSpeichern(); }
}

void erkennungAktualisieren() {
  // Merkmale gegen B0 laufen fuer Korpus, Anzeige und Modell weiter mit;
  // die Entscheidung haengt nicht mehr daran.
  if (refStatus == REF_GUELTIG) {
    merk = merkmale(bMittel, ref.b0);
    modellTraf = klassifiziere(merk, klasseErkannt, fallErkannt);
    if (!modellTraf) { klasseErkannt = KL_FREI; fallErkannt = FA_FREI; }
  }

  if (nMessungen < ENTSCHEIDUNG_AB) return;   // Mittel noch nicht eingeschwungen

  // Wechseldetektor: ein Ausschlag von dAbs ueber die Schwelle ist ein
  // Ereignis und kippt den Zustand. Hysterese: erst wenn dAbs wieder unter
  // "aus" gefallen ist, darf der naechste Ausschlag zaehlen -- sonst wuerde
  // ein einziges Einparken mehrfach kippen.
  if (!ausschlag && dAbsAkt > schwelleEin()) {
    ausschlag = true;
    belegt = !belegt;
    Serial.printf("[ereignis] %s  dAbs=%.2f uT  d=(%.2f %.2f %.2f)  Schwelle %.2f\n",
                  belegt ? "Auto" : "leer", dAbsAkt, dVek[0], dVek[1], dVek[2], schwelleEin());
  } else if (ausschlag && dAbsAkt < schwelleAus()) {
    ausschlag = false;
  }
}

const char *zustandText() {
  if (!sensorOk)                    return "sensorfehler";
  if (nMessungen < ENTSCHEIDUNG_AB) return "einschwingen";
  return belegt ? "belegt" : "frei";
}

const char *hinweisText() {
  if (!sensorOk)                  return "Magnetometer antwortet nicht";
  if (refStatus == REF_ERFASSUNG) return "bestimme Leerfeld -- Platz frei halten";
  if (refStatus != REF_GUELTIG)   return "noch keine Referenz -- 'Referenz neu' bei freiem Platz";
  if (MODELL_N == 0)              return "kein Modell geladen: nur frei/belegt, kein Splashover-Filter";
  return "Erkennung laeuft";
}

// ---------------------------------------------------------------- Messung

void messen() {
  int16_t x, y, z;
  if (!readRaw(x, y, z)) { sensorOk = false; return; }
  sensorOk = true;
  roh[0] = x; roh[1] = y; roh[2] = z;

  bVek[0] = x * scale_uT;
  bVek[1] = y * scale_uT;
  bVek[2] = z * scale_uT;

  if (!avgInit) {
    for (uint8_t i = 0; i < 3; i++) { bMittel[i] = bLangsam[i] = bVek[i]; dVek[i] = 0; }
    dAbsAkt = 0;
    avgInit = true;
    nMessungen = 1;
  } else {
    // Erst die Abweichung des neuen Werts vom bisherigen Mittel, dann das
    // Mittel nachziehen -- es laeuft immer, auch mit Fahrzeug darueber.
    for (uint8_t i = 0; i < 3; i++) {
      dVek[i] = bVek[i] - bMittel[i];
      bMittel[i] += alphaMittel * dVek[i];
      bLangsam[i] += ALPHA_LANGSAM * (bVek[i] - bLangsam[i]);
    }
    dAbsAkt = betrag3(dVek);
    if (nMessungen < 0xFFFF) nMessungen++;
  }

  if (messZweck != ZWECK_NICHTS) {
    for (uint8_t i = 0; i < 3; i++) {
      sumV[i] += bVek[i];
      sumQ[i] += (double)bVek[i] * bVek[i];
    }
    messN++;
    if (messZiel && messN >= messZiel) mittelungFertig();
  }

  erkennungAktualisieren();                       // wartet selbst ENTSCHEIDUNG_AB ab
  if (refStatus == REF_GUELTIG) referenzNachfuehren();
}

// Selbsttest der Merkmalsrechnung mit bekannten Vektoren, ohne Sensor.
void selbsttest() {
  bool ok = true;
  Serial.println(F("Selbsttest Merkmale:"));

  struct Fall2 { const char *name; float b0[3]; float b[3];
                 float dAbs, dPar, dPerp, theta; };
  const Fall2 faelle[] = {
    // Referenz 50 uT laengs z. Stoerung parallel dazu:
    { "laengs B0",  {0,0,50}, {0,0,53},   3.0f, 3.0f, 0.0f,  0.0f },
    // Stoerung quer dazu -- der Fall, an dem das alte d|B| scheitert:
    { "quer zu B0", {0,0,50}, {4,0,50},   4.0f, 0.0f, 4.0f, 90.0f },
    // Dasselbe, aber Sensor um 90 Grad um y gedreht: (x,y,z)->(z,y,-x).
    // Muss dieselben Merkmale liefern -- das ist die Drehinvarianz.
    { "quer, gedreht", {50,0,0}, {50,0,-4}, 4.0f, 0.0f, 4.0f, 90.0f },
    // Stoerung, die das Feld schwaecht: dPar negativ, theta > 90
    { "schwaechend", {0,0,50}, {0,0,47},  3.0f, -3.0f, 0.0f, 180.0f },
  };

  for (const Fall2 &f : faelle) {
    Merkmale m = merkmale(f.b, f.b0);
    bool gut = fabsf(m.dAbs  - f.dAbs)  < 0.01f &&
               fabsf(m.dPar  - f.dPar)  < 0.01f &&
               fabsf(m.dPerp - f.dPerp) < 0.01f &&
               fabsf(m.theta - f.theta) < 0.1f;
    ok &= gut;
    Serial.printf("  %-14s dAbs=%6.2f dPar=%7.2f dPerp=%6.2f theta=%6.1f  %s\n",
                  f.name, m.dAbs, m.dPar, m.dPerp, m.theta, gut ? "ok" : "FEHLER");
  }

  // Gegenprobe 1: was das kippfeste d|B| bei der Querstoerung sieht --
  // fast nichts. Das ist der Preis seiner Kippfestigkeit.
  float b0[3] = {0, 0, 50}, b[3] = {4, 0, 50};
  Merkmale q = merkmale(b, b0);
  Serial.printf("  quer: dAbs %.2f uT gegen d|B| %.2f uT  (Faktor %.0f)\n",
                q.dAbs, fabsf(q.dBetrag), q.dAbs / fmaxf(fabsf(q.dBetrag), 1e-6f));

  // Gegenprobe 2: 5 Grad Kippen OHNE Fahrzeug. Genau umgekehrt -- dAbs
  // schlaegt ueber die Schwelle aus, d|B| bleibt bei null. Das ist der
  // Grund fuer das Feld "Aufnahmeart" im Korpus.
  const float w = 5.0f * 0.0174533f;
  float bk[3] = { sinf(w) * 50.0f, 0.0f, cosf(w) * 50.0f };
  Merkmale k = merkmale(bk, b0);
  bool kipp = k.dAbs > SCHWELLE_EIN_UT && fabsf(k.dBetrag) < 0.01f;
  ok &= kipp;
  Serial.printf("  %-14s 5 Grad Kippen ohne Auto: dAbs %.2f uT (Schwelle %.2f), d|B| %.2f uT  %s\n",
                "Kippen", k.dAbs, (float)SCHWELLE_EIN_UT, k.dBetrag, kipp ? "ok" : "FEHLER");

  // Splashover-Regel
  bool sp = !fallIstBelegt(FA_NEBEN_LINKS) && !fallIstBelegt(FA_NACHBARPLATZ) &&
             fallIstBelegt(FA_AUF_MITTE)   &&  fallIstBelegt(FA_AUF_HINTEN);
  ok &= sp;
  Serial.printf("  %-14s %s\n", "Splashover", sp ? "ok" : "FEHLER");

  Serial.println(ok ? F("  -> bestanden") : F("  -> FEHLGESCHLAGEN"));
}

// ---------------------------------------------------------------- LoRa-Sender
#if USE_LORA

// Gesendet wird **nur bei Zustandswechsel**, dazu ein Lebenszeichen. Ein
// Sensor, der im Takt sendet, verbraucht Strom und Funkzeit fuer eine
// Nachricht, die der Server schon kennt: eine Parkluecke aendert sich ein
// paar Mal am Tag, nicht ein paar Mal pro Minute.
//
// Es gibt keine Bestaetigung vom Gateway. Der Sensor koennte danach horchen,
// aber das kostet Wachzeit und eine zweite Funkrichtung; stattdessen geht
// jedes Ereignis LORA_WIEDERHOLUNGEN mal raus. Alle Wiederholungen tragen
// dieselbe laufende Nummer, und das Gateway wirft Doppelte weg. Das ist die
// uebliche Bauform fuer batteriebetriebene Einwegknoten.
//
// Funkzeit (Sendedauer): bei SF7/BW125 braucht ein Paket dieser Laenge rund
// 60 ms. Im 433-MHz-Band sind 10 % der Zeit erlaubt, also etwa 6 min je
// Stunde. Drei Wiederholungen sind 0,18 s -- selbst 100 Ereignisse pro Stunde
// blieben weit darunter. Bei SF12 waere ein Paket rund 25x laenger; auch das
// reicht noch, aber dann lohnt das Nachrechnen.

static uint16_t loraSeq = 0;
static char     loraZuletzt = 0;          // zuletzt gemeldeter Zustand
static uint8_t  loraOffen = 0;            // ausstehende Wiederholungen
static uint32_t loraNaechste = 0;
static uint32_t loraLetzteMeldung = 0;
static char     loraNutzlast[56];
static char     loraKnoten[16] = "";
static bool     loraOk = false;

uint16_t crc16(const char *daten, size_t laenge) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < laenge; i++) {
    crc ^= (uint16_t)(uint8_t)daten[i] << 8;
    for (uint8_t b = 0; b < 8; b++)
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

// Jeder Sensor braucht eine eigene Kennung. Sie aus der MAC-Adresse
// abzuleiten spart, 50 Geraete einzeln mit verschiedenen Quelltexten zu
// flashen -- jedes bekommt beim ersten Start automatisch seine eigene.
void loraKennung() {
  if (strlen(LORA_KNOTEN)) {
    strncpy(loraKnoten, LORA_KNOTEN, sizeof(loraKnoten) - 1);
    return;
  }
  // eFuse-MAC statt WiFi.macAddress(): letztere ist die STA-Adresse und im
  // reinen Accesspoint-Betrieb noch nicht gesetzt -> "PS-000000" fuer alle.
  uint64_t mac = ESP.getEfuseMac();    // 48 Bit, Basis-MAC des Chips
  snprintf(loraKnoten, sizeof(loraKnoten), "PS-%02X%02X%02X",
           (uint8_t)(mac >> 24), (uint8_t)(mac >> 32), (uint8_t)(mac >> 40));
}

int16_t batterieMV() {
#if PIN_VBAT >= 0
  // Spannungsteiler halbiert, damit die Zelle in den Messbereich passt.
  return (int16_t)(analogReadMilliVolts(PIN_VBAT) * 2);
#else
  return -1;                            // kein Teiler bestueckt
#endif
}

char statusZeichen() {
  const char *z = zustandText();
  if (!strcmp(z, "belegt")) return 'B';
  if (!strcmp(z, "frei"))   return 'F';
  return '?';                           // keine Referenz, einschwingen oder Sensorfehler
}

// Ereignis einreihen. Die laufende Nummer steigt je Ereignis, nicht je
// Wiederholung -- daran erkennt das Gateway die Doppelten.
void loraEreignis(char status) {
  loraSeq++;
  char rumpf[48];
  snprintf(rumpf, sizeof(rumpf), "PS1,%s,%c,%d,%u",
           loraKnoten, status, batterieMV(), loraSeq);
  snprintf(loraNutzlast, sizeof(loraNutzlast), "%s,%04X",
           rumpf, crc16(rumpf, strlen(rumpf)));
  loraOffen = LORA_WIEDERHOLUNGEN;
  loraNaechste = millis();
  loraLetzteMeldung = millis();
}

void loraSetup() {
  loraKennung();
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
  LoRa.setPins(LORA_NSS, LORA_RST, -1);      // ohne DIO0: es wird nur gesendet
  if (!LoRa.begin(LORA_FREQ)) {
    Serial.println(F("LoRa: Funkmodul antwortet nicht -- Verdrahtung und 3,3 V pruefen."));
    return;
  }
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(LORA_BW);
  LoRa.setCodingRate4(LORA_CR);
  LoRa.setSyncWord(LORA_SYNCWORD);
  LoRa.setTxPower(LORA_TX_DBM);
  LoRa.enableCrc();
  loraOk = true;
  Serial.printf("LoRa: Knoten %s, %.1f MHz, SF%d, %d dBm\n",
                loraKnoten, LORA_FREQ / 1e6, LORA_SF, LORA_TX_DBM);
}

void loraPflegen() {
  if (!loraOk) return;
  uint32_t jetzt = millis();

  // 1) Zustandswechsel -- der eigentliche Anlass zu senden
  char status = statusZeichen();
  if (status != loraZuletzt) {
    loraZuletzt = status;
    loraEreignis(status);
    Serial.printf("LoRa: Wechsel -> %c, sende %u mal\n", status, LORA_WIEDERHOLUNGEN);
  }

  // 2) Lebenszeichen: ohne das koennte der Server einen stummen Sensor nicht
  //    von einem dauerhaft belegten Platz unterscheiden.
  if (jetzt - loraLetzteMeldung >= (uint32_t)LORA_HEARTBEAT_S * 1000UL) {
    loraEreignis(loraZuletzt ? loraZuletzt : statusZeichen());
    loraOffen = 1;                      // Lebenszeichen einmal genuegt
  }

  // 3) Ausstehende Sendungen abarbeiten
  if (loraOffen && (int32_t)(jetzt - loraNaechste) >= 0) {
    LoRa.beginPacket();
    LoRa.print(loraNutzlast);
    LoRa.endPacket();                   // blockiert ~60 ms
    LoRa.sleep();                       // Funkmodul danach schlafen legen
    loraOffen--;
    // Zufaellige Pause: senden mehrere Sensoren gleichzeitig los (ein Auto
    // faehrt an mehreren Luecken vorbei), wuerden gleiche Abstaende dafuer
    // sorgen, dass sich auch die Wiederholungen wieder ueberlagern.
    loraNaechste = millis() + LORA_WIEDERHOL_MS + random(0, 500);
  }
}

#endif  // USE_LORA


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
#zustand{font-size:3.2rem;font-weight:700;text-align:center;
         padding:1.6rem 0;border-radius:14px;background:#1c1c1c;margin-bottom:.5rem}
.belegt{color:#38d66b}.frei{color:#888}.wartet{color:#e8b93b}.fehler{color:#e05252}
#hinweis{text-align:center;color:#999;font-size:.9rem;margin-bottom:1.2rem;min-height:2.4em}
button{width:100%;padding:1rem;font-size:1.05rem;font-weight:600;
       border:0;border-radius:12px;background:#2a2a2a;color:#eee;cursor:pointer}
button:active{background:#3a3a3a}
#neu{background:#1d4ed8;margin-bottom:.7rem}
#reset{background:#7c2d2d;margin-bottom:.7rem}
table{width:100%;border-collapse:collapse;font-size:.85rem;margin-top:1.2rem;
      font-variant-numeric:tabular-nums}
td{padding:.35rem 0;border-bottom:1px solid #262626;color:#bbb}
td:last-child{text-align:right;color:#eee}
</style></head><body>
<h1>Carsensor &nbsp;|&nbsp; <a href="/korpus">Lerndaten</a></h1>
<div id="zustand" class="wartet">--</div>
<div id="hinweis"></div>
<button id="neu" onclick="if(confirm('Platz frei? Referenz wird neu bestimmt.'))cmd('referenz')">Referenz neu (Platz frei)</button>
<button id="reset" onclick="if(confirm('Referenz UND Lerndaten loeschen?'))cmd('reset')">reset &mdash; alles zuruecksetzen</button>
<table id="werte"></table>
<script>
function cmd(c){fetch('/'+c).then(hole)}
function z(v,k){return (v===undefined||v===null)?'-':v.toFixed(k===undefined?2:k)}
function hole(){
 fetch('/status').then(function(r){return r.json()}).then(function(d){
  var e=document.getElementById('zustand');
  e.textContent=d.zustand;
  e.className = d.zustand=='belegt' ? 'belegt'
              : d.zustand=='frei'   ? 'frei'
              : d.zustand=='sensorfehler' ? 'fehler' : 'wartet';
  document.getElementById('hinweis').textContent=d.hinweis;
  var r=[['Abweichung |dB|',z(d.dAbs)+' uT'],
         ['davon laengs B0',z(d.dPar)+' uT'],
         ['davon quer',z(d.dPerp)+' uT'],
         ['Winkel theta',z(d.theta,1)+' Grad'],
         ['kippfest d|B|',z(d.dBetrag)+' uT'],
         ['Schwelle ein / aus',z(d.sEin)+' / '+z(d.sAus)+' uT'],
         ['Referenz |B0|',z(d.b0)+' uT'],
         ['Rauschen',z(d.rauschen,3)+' uT'],
         ['Modell',d.modell?(d.klasse+' / '+d.fall):(d.modellN?'kein Treffer':'nicht geladen')],
         ['Sensor',d.sensor],
         ['Lerndaten',d.anzahl+' Beobachtungen']];
  document.getElementById('werte').innerHTML=
    r.map(function(x){return '<tr><td>'+x[0]+'</td><td>'+x[1]+'</td></tr>'}).join('');
 })
}
hole();setInterval(hole,500);
</script></body></html>)HTML";

const char SEITE_KORPUS[] PROGMEM = R"HTML(<!doctype html>
<html lang="de"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Carsensor Lerndaten</title><style>
*{box-sizing:border-box}
body{font-family:system-ui,sans-serif;margin:0;padding:1rem;
     background:#111;color:#eee;-webkit-text-size-adjust:100%}
h1{font-size:1rem;font-weight:600;color:#888;margin:0 0 .8rem}
h2{font-size:.8rem;font-weight:600;color:#666;margin:1rem 0 .4rem;text-transform:uppercase;
   letter-spacing:.05em}
a{color:#6aa9ff}
#dv{font-size:2.6rem;font-weight:700;text-align:center;padding:.9rem 0;
    border-radius:14px;background:#1c1c1c}
#dv small{display:block;font-size:.7rem;font-weight:400;color:#888;margin-top:.3rem}
#ruhe{text-align:center;font-size:.85rem;margin:.4rem 0 .5rem;min-height:1.2em}
.ruhig{color:#38d66b}.unruhig{color:#e8b93b}.keineref{color:#e05252}
button{padding:.8rem;font-size:.95rem;font-weight:600;border:0;border-radius:11px;
       background:#2a2a2a;color:#eee;cursor:pointer;width:100%}
button:active{background:#3a3a3a}
button.auf{background:#1f4d2e}
input,select{width:100%;padding:.7rem;font-size:1rem;border-radius:11px;border:1px solid #333;
      background:#1c1c1c;color:#eee;margin-bottom:.5rem}
.gitter{display:grid;grid-template-columns:1fr 1fr;gap:.45rem;margin-bottom:.5rem}
table{width:100%;border-collapse:collapse;font-size:.78rem;
      font-variant-numeric:tabular-nums}
th{text-align:left;color:#777;font-weight:600;border-bottom:1px solid #333;padding:.3rem 0}
td{padding:.3rem 0;border-bottom:1px solid #242424;color:#ccc}
td:nth-child(2),td:nth-child(3){text-align:right}
.fuss{display:flex;gap:.5rem;margin:.9rem 0}
#artwarn{font-size:.78rem;color:#e8b93b;margin:-.2rem 0 .5rem;line-height:1.35}
</style></head><body>
<h1>Lerndaten &nbsp;|&nbsp; <a href="/">zur Erkennung</a></h1>
<div id="dv">--<small>|dB| Abweichung von der Referenz</small></div>
<div id="ruhe"></div>

<h2>Beschriftung</h2>
<input id="ort" placeholder="Ort, z.B. Marktplatz P3" onchange="merk()">
<input id="sid" placeholder="Sensor-ID, z.B. S07" onchange="merk()">
<select id="klasse"></select>
<select id="art" onchange="artHinweis()">
  <option value="0">fest verbaut (Sensor liegt seit der Referenz unbewegt)</option>
  <option value="1">handgefuehrt (Sensor wurde bewegt)</option>
</select>
<div id="artwarn"></div>

<h2>Belegungsfall aufnehmen</h2>
<div class="gitter">
  <button class="auf" onclick="sp(1)">auf mitte</button>
  <button class="auf" onclick="sp(2)">auf vorne</button>
  <button class="auf" onclick="sp(3)">auf hinten</button>
  <button onclick="sp(4)">neben links</button>
  <button onclick="sp(5)">neben rechts</button>
  <button onclick="sp(6)">davor</button>
  <button onclick="sp(7)">dahinter</button>
  <button onclick="sp(8)">Nachbarplatz</button>
</div>
<button onclick="sp(0)">leerer Platz (Gegenprobe)</button>

<div class="fuss">
  <button onclick="location='/csv'">CSV laden</button>
  <button onclick="if(confirm('Alle Lerndaten loeschen?'))fetch('/clear').then(hole)">loeschen</button>
</div>
<table><thead><tr><th>Klasse / Fall</th><th>|dB|</th><th>theta</th></tr></thead>
<tbody id="liste"></tbody></table>
<script>
var KL=[];
function merk(){
 fetch('/beschriftung?ort='+encodeURIComponent(document.getElementById('ort').value)
       +'&sid='+encodeURIComponent(document.getElementById('sid').value));
}
function artHinweis(){
 var a=document.getElementById('art').value;
 document.getElementById('artwarn').textContent = a=='1'
  ? 'Kippt der Sensor zwischen Referenz und Messung, wandert das in |dB| und theta '
   +'(rund 0,87 uT je Grad). Verwertbar bleibt dann nur d|B|. Die Zeile wird '
   +'entsprechend gekennzeichnet.' : '';
}
function sp(fall){
 var k=document.getElementById('klasse').value;
 var a=document.getElementById('art').value;
 fetch('/beobachtung?klasse='+k+'&fall='+fall+'&art='+a)
  .then(function(r){return r.text()})
  .then(function(t){if(t!='ok')alert(t);hole()});
}
function hole(){
 fetch('/korpusstatus').then(function(r){return r.json()}).then(function(d){
  if(KL.length==0&&d.klassen){KL=d.klassen;
   document.getElementById('klasse').innerHTML=
    KL.map(function(n,i){return '<option value="'+i+'">'+n+'</option>'}).join('');
   document.getElementById('klasse').value=3;artHinweis();}
  if(document.activeElement.id!='ort')document.getElementById('ort').value=d.ort;
  if(document.activeElement.id!='sid')document.getElementById('sid').value=d.sensor;
  var dv=document.getElementById('dv');
  dv.innerHTML=(d.ref?d.dAbs.toFixed(2):'--')+
   '<small>|dB| Abweichung von der Referenz, uT</small>';
  var r=document.getElementById('ruhe');
  if(d.laeuft){r.textContent='nimmt auf... '+d.fortschritt+'/'+d.proben;r.className='unruhig'}
  else if(!d.ref){r.textContent='keine Referenz -- erst auf der Startseite setzen';r.className='keineref'}
  else if(d.unruhe>0.3){r.textContent='unruhig ('+d.unruhe.toFixed(2)+' uT) -- warten';r.className='unruhig'}
  else {r.textContent='ruhig -- Aufnahme moeglich';r.className='ruhig'}
  document.getElementById('liste').innerHTML=d.liste.map(function(m){
   return '<tr><td>'+m.k+' / '+m.f+(m.h?' <span style="color:#e8b93b">(hand)</span>':'')
        +'</td><td>'+m.d.toFixed(2)+'</td><td>'+m.t.toFixed(0)+'</td></tr>'
  }).join('')+'<tr><td colspan="3" style="color:#666">'+d.anzahl+' von '+d.max+'</td></tr>';
 })
}
hole();setInterval(hole,500);
</script></body></html>)HTML";

// ---------------------------------------------------------------- Endpunkte

float beobSem(const Beobachtung &o) {
  if (!o.n) return 0.0f;
  return o.sigma / sqrtf((float)o.n);
}

void handleStatus() {
  String j = "{";
  j += "\"zustand\":\"" + String(zustandText()) + "\",";
  j += "\"hinweis\":\"" + String(hinweisText()) + "\",";
  j += "\"dAbs\":"  + String(merk.dAbs, 3) + ",";
  j += "\"dPar\":"  + String(merk.dPar, 3) + ",";
  j += "\"dPerp\":" + String(merk.dPerp, 3) + ",";
  j += "\"theta\":" + String(merk.theta, 1) + ",";
  j += "\"dBetrag\":" + String(merk.dBetrag, 3) + ",";
  j += "\"sEin\":"  + String(schwelleEin(), 2) + ",";
  j += "\"sAus\":"  + String(schwelleAus(), 2) + ",";
  j += "\"b0\":"    + String(betrag3(ref.b0), 2) + ",";
  j += "\"rauschen\":" + String(ref.rauschen, 3) + ",";
  j += "\"roh\":[" + String(roh[0]) + "," + String(roh[1]) + "," + String(roh[2]) + "],";
  j += "\"modell\":" + String(modellTraf ? 1 : 0) + ",";
  j += "\"modellN\":" + String(MODELL_N) + ",";
  j += "\"klasse\":\"" + String(KLASSE_NAME[klasseErkannt]) + "\",";
  j += "\"fall\":\"" + String(FALL_NAME[fallErkannt]) + "\",";
  j += "\"sensor\":\"" + String(sensorOk ? sensorName : "FEHLER") + "\",";
  j += "\"anzahl\":" + String(korpus.n);
  j += "}";
  server.send(200, "application/json", j);
}

void handleKorpusStatus() {
  String j = "{";
  j += "\"ref\":" + String(refStatus == REF_GUELTIG ? 1 : 0) + ",";
  j += "\"dAbs\":" + String(merk.dAbs, 2) + ",";
  j += "\"unruhe\":" + String(unruhe(), 2) + ",";
  j += "\"laeuft\":" + String(messZweck == ZWECK_BEOBACHTUNG ? 1 : 0) + ",";
  j += "\"fortschritt\":" + String(messN) + ",";
  j += "\"proben\":" + String(MESS_PROBEN) + ",";
  j += "\"ort\":\"" + String(ortName) + "\",";
  j += "\"sensor\":\"" + String(sensorId) + "\",";
  j += "\"klassen\":[";
  for (uint8_t i = 0; i < KL_ANZAHL; i++) {
    if (i) j += ",";
    j += "\"" + String(KLASSE_NAME[i]) + "\"";
  }
  j += "],\"liste\":[";
  uint16_t von = korpus.n > 12 ? korpus.n - 12 : 0;     // nur die letzten 12
  for (uint16_t i = von; i < korpus.n; i++) {
    if (i > von) j += ",";
    j += "{\"k\":\"" + String(KLASSE_NAME[korpus.b[i].klasse]) + "\",";
    j += "\"f\":\"" + String(FALL_NAME[korpus.b[i].fall]) + "\",";
    j += "\"d\":" + String(korpus.b[i].m.dAbs, 2) + ",";
    j += "\"t\":" + String(korpus.b[i].m.theta, 1) + ",";
    j += "\"h\":" + String(korpus.b[i].art == AUF_HAND ? 1 : 0) + "}";
  }
  j += "],\"anzahl\":" + String(korpus.n) + ",\"max\":" + String(MAX_BEOBACHTUNGEN) + "}";
  server.send(200, "application/json", j);
}

// Der Trainingssatz. Eine Zeile je Beobachtung, alles in Mikrotesla, damit
// die CSVs verschiedener Sensoren und Plaetze zusammengeworfen werden
// koennen.
//
// Welche Spalten gelten, haengt an der Aufnahmeart:
//   d_betrag            immer -- kippfest, aber schwach
//   d_abs/d_par/        nur bei aufnahme=fest, sonst steckt der Kippwinkel
//   d_perp/theta        des Sensors darin und nicht das Fahrzeug
//   d_x/d_y/d_z         nur bei aufnahme=fest UND eingehaltener Einbaulage
//                       -- dafuer trennen sie links von rechts
void handleCsv() {
  String c = "nr;ort;sensor;aufnahme;klasse;fall;belegt_soll;"
             "b0_x;b0_y;b0_z;b_x;b_y;b_z;d_x;d_y;d_z;"
             "d_abs;d_par;d_perp;theta;d_betrag;sigma;sem;n;t_ms\n";
  for (uint16_t i = 0; i < korpus.n; i++) {
    const Beobachtung &o = korpus.b[i];
    c += String(i + 1) + ";" + o.ort + ";" + o.sensorId + ";";
    c += String(AUFNAHME_NAME[o.art]) + ";";
    c += String(KLASSE_NAME[o.klasse]) + ";" + String(FALL_NAME[o.fall]) + ";";
    c += String(fallIstBelegt(o.fall) ? 1 : 0) + ";";
    for (uint8_t k = 0; k < 3; k++) c += String(o.b0[k], 3) + ";";
    for (uint8_t k = 0; k < 3; k++) c += String(o.b[k], 3) + ";";
    for (uint8_t k = 0; k < 3; k++) c += String(o.b[k] - o.b0[k], 3) + ";";
    c += String(o.m.dAbs, 3) + ";" + String(o.m.dPar, 3) + ";";
    c += String(o.m.dPerp, 3) + ";" + String(o.m.theta, 2) + ";";
    c += String(o.m.dBetrag, 3) + ";";
    c += String(o.sigma, 3) + ";" + String(beobSem(o), 4) + ";" + String(o.n) + ";";
    c += String(o.t) + "\n";
  }
  server.sendHeader("Content-Disposition", "attachment; filename=carsensor.csv");
  server.send(200, "text/csv; charset=utf-8", c);
}

void aktionReset() {
  memset(&ref, 0, sizeof(ref));
  refStatus = REF_FEHLT;
  belegt = false; ausschlag = false;
  korpus.n = 0;
  korpusSpeichern();
  prefs.begin("carsensor", false);
  prefs.remove("ref");
  prefs.end();
  mittelungAbbrechen();
  avgInit = false;
  Serial.println(F("[reset] Referenz und Lerndaten geloescht."));
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

  // Feste AP-Adresse. Zugleich als Gateway und als DNS-Server an die
  // Clients ausgeben: ohne DNS-Server im DHCP-Angebot kann iOS
  // captive.apple.com nicht aufloesen und zeigt kein Portal an
  // (Android ist hier toleranter).
  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0), IPAddress(0, 0, 0, 0), apIP);
  WiFi.softAP(AP_SSID, AP_PASS);

  // RFC 8910 / DHCP-Option 114: nennt iOS (ab iOS 13) und Android die
  // Portal-URL schon beim Verbindungsaufbau. Das ist der zuverlaessigste
  // Weg, dass das iPhone die Seite von selbst oeffnet -- ohne diese Option
  // verlassen sich die Geraete auf die DNS-/HTTP-Abfrage, die iOS strenger
  // behandelt als Android.
  WiFi.AP.enableDhcpCaptivePortal();

  apModus = true;
  // Captive Portal: jeder Hostname wird auf uns aufgeloest, damit auch eine
  // Suchanfrage aus der Adressleiste hier landet.
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", WiFi.softAPIP());
  Serial.printf("Accesspoint \"%s\" (Passwort \"%s\")\n", AP_SSID, AP_PASS);
  Serial.printf("Adresse: http://%s  (Captive Portal aktiv)\n", WiFi.softAPIP().toString().c_str());
#endif

  server.on("/",             []() { server.send_P(200, "text/html", SEITE); });
  server.on("/korpus",       []() { server.send_P(200, "text/html", SEITE_KORPUS); });
  server.on("/status",       handleStatus);
  server.on("/korpusstatus", handleKorpusStatus);
  server.on("/csv",          handleCsv);

  server.on("/reset",    []() { aktionReset();  handleStatus(); });
  server.on("/referenz", []() { referenzNeu();  handleStatus(); });

  server.on("/beschriftung", []() {
    if (server.hasArg("ort")) {
      memset(ortName, 0, ORT_LEN);
      strncpy(ortName, server.arg("ort").c_str(), ORT_LEN - 1);
    }
    if (server.hasArg("sid")) {
      memset(sensorId, 0, SENSOR_LEN);
      strncpy(sensorId, server.arg("sid").c_str(), SENSOR_LEN - 1);
    }
    // Semikolon wuerde die CSV zerreissen.
    for (uint8_t i = 0; i < ORT_LEN; i++)
      if (ortName[i] == ';' || ortName[i] == '\n' || ortName[i] == '\r') ortName[i] = '_';
    for (uint8_t i = 0; i < SENSOR_LEN; i++)
      if (sensorId[i] == ';' || sensorId[i] == '\n' || sensorId[i] == '\r') sensorId[i] = '_';
    beschriftungSpeichern();
    server.send(200, "text/plain", "ok");
  });

  server.on("/beobachtung", []() {
    // toInt() liefert long; ein negatives Argument wird beim Zuweisen auf
    // uint8_t gross und faellt gleich darunter in die Gueltigkeitspruefung.
    uint8_t k = server.hasArg("klasse") ? (uint8_t)server.arg("klasse").toInt() : (uint8_t)KL_PKW;
    uint8_t f = server.hasArg("fall")   ? (uint8_t)server.arg("fall").toInt()   : (uint8_t)FA_AUF_MITTE;
    uint8_t a = server.hasArg("art")    ? (uint8_t)server.arg("art").toInt()    : (uint8_t)AUF_FEST;
    if (k >= KL_ANZAHL || f >= FA_ANZAHL || a >= AUF_ANZAHL)
                                              server.send(200, "text/plain", "ungueltig");
    else if (refStatus != REF_GUELTIG)        server.send(200, "text/plain", "Erst Referenz setzen!");
    else if (korpus.n >= MAX_BEOBACHTUNGEN)   server.send(200, "text/plain", "Voll - CSV laden und loeschen");
    else if (messZweck != ZWECK_NICHTS)       server.send(200, "text/plain", "Aufnahme laeuft noch");
    else { mittelungStarten(ZWECK_BEOBACHTUNG, MESS_PROBEN, k, f, a);
           server.send(200, "text/plain", "ok"); }
  });

  server.on("/clear", []() {
    korpus.n = 0;
    korpusSpeichern();
    Serial.println(F("[clear] Lerndaten geloescht."));
    handleKorpusStatus();
  });

  // Alles Unbekannte auf die Startseite umlenken. Das faengt zugleich die
  // Erreichbarkeitspruefungen der Handys ab (Android ruft
  // connectivitycheck.gstatic.com/generate_204 auf, iOS captive.apple.com):
  // eine 302 statt der erwarteten Antwort signalisiert "Portal".
  server.onNotFound([]() {
    if (apModus) {
      server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
      server.send(302, "text/plain", "redirect");
    } else {
      server.send(404, "text/plain", "?");
    }
  });
  server.begin();
}

// ---------------------------------------------------------------- Konsole

// Zeilenweise: erst beim Enter (CR oder LF) wird ausgefuehrt. Solange eine
// Eingabe angefangen ist, schweigt die Sekundenzeile (konsoleTippt), sonst
// geraet das Getippte zwischen die Ausgaben.
static char    konsoleZeile[32];
static uint8_t konsoleN = 0;
bool           konsoleTippt = false;

void befehl(const char *z) {
  char cmd = z[0];
  const char *arg = z + 1;
  while (*arg == ' ') arg++;

  switch (cmd) {
    case 'n': aktionReset();  break;
    case 'r': referenzNeu();  break;
    case 's': selbsttest();   break;
    case 'a':
      if (*arg) {
        float v = atof(arg);
        if (v > 0.0f && v <= 1.0f) { alphaMittel = v; alphaSpeichern(); }
        else Serial.println(F("alpha: 0 < a <= 1"));
      }
      Serial.printf("alpha %.3f (mittel = %.3f*alt + %.3f*neu, ~%.1f s bei %d ms Takt)\n",
                    alphaMittel, 1.0f - alphaMittel, alphaMittel,
                    SAMPLE_MS / 1000.0f / alphaMittel, SAMPLE_MS);
      break;
    case 't':
      if (*arg) {
        float v = atof(arg);
        if (v > 0.0f && v <= 1000.0f) { schwelleUT = v; schwelleSpeichern(); }
        else Serial.println(F("Schwelle: Wert in uT, 0 < t <= 1000"));
      }
      Serial.printf("Schwelle %.2f uT (ein %.2f / aus %.2f, Rauschen %.3f)\n",
                    schwelleUT, schwelleEin(), schwelleAus(), ref.rauschen);
      break;
#if USE_LORA
    // Fuer den Reichweitenversuch: Meldung ausloesen, ohne auf ein Auto
    // zu warten. Am Gateway laesst sich dann RSSI und SNR ablesen.
    case 'f':
      loraEreignis(statusZeichen());
      Serial.printf("[lora] Meldung %u erzwungen: %s\n", loraSeq, loraNutzlast);
      break;
#endif
    case 'm':
      if (refStatus != REF_GUELTIG) Serial.println(F("Erst Referenz setzen (r)."));
      else mittelungStarten(ZWECK_BEOBACHTUNG, MESS_PROBEN, KL_SONST, FA_AUF_MITTE, AUF_FEST);
      break;
    case 'x': korpus.n = 0; korpusSpeichern(); Serial.println(F("[clear] Lerndaten geloescht.")); break;
    case 'l':
      Serial.printf("Lerndaten (%u):\n", korpus.n);
      for (uint16_t k = 0; k < korpus.n; k++) {
        const Beobachtung &o = korpus.b[k];
        Serial.printf("  %2u %-14s %-12s %-13s %-12s dAbs=%7.2f +-%.3f theta=%6.1f d|B|=%7.2f\n",
                      k + 1, o.ort, KLASSE_NAME[o.klasse], FALL_NAME[o.fall],
                      AUFNAHME_NAME[o.art], o.m.dAbs, beobSem(o), o.m.theta, o.m.dBetrag);
      }
      break;
    case 'i':
      Serial.printf("Zustand: %s | %s\n", zustandText(), hinweisText());
      Serial.printf("  Mittel = %.2f %.2f %.2f uT  |Mittel| = %.2f  alpha %.3f  dAbs %.2f%s\n",
                    bMittel[0], bMittel[1], bMittel[2], betrag3(bMittel), alphaMittel, dAbsAkt,
                    ausschlag ? " (Ausschlag)" : "");
      Serial.printf("  B0 = %.2f %.2f %.2f uT  |B0| = %.2f  Rauschen %.3f\n",
                    ref.b0[0], ref.b0[1], ref.b0[2], betrag3(ref.b0), ref.rauschen);
      Serial.printf("  gegen B0: dAbs %.2f (par %.2f, perp %.2f, theta %.1f)  Schwelle %.2f/%.2f uT\n",
                    merk.dAbs, merk.dPar, merk.dPerp, merk.theta, schwelleEin(), schwelleAus());
      Serial.printf("  Modell: %u Prototypen%s | Lerndaten %u | Ort \"%s\" Sensor \"%s\"\n",
                    MODELL_N, modellTraf ? " (Treffer)" : "", korpus.n, ortName, sensorId);
      Serial.printf("  Einbau: %s\n", EINBAU_HINWEIS);
#if USE_LORA
      Serial.printf("  LoRa: %s | Knoten %s | gemeldet '%c' | Nummer %u | offen %u\n",
                    loraOk ? "bereit" : "kein Modul", loraKnoten,
                    loraZuletzt ? loraZuletzt : '-', loraSeq, loraOffen);
#endif
      break;
    default:
      Serial.printf("? %s\n", z);
      break;
  }
}

void konsole() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r' || c == '\n') {
      konsoleZeile[konsoleN] = 0;
      if (konsoleN) befehl(konsoleZeile);   // CR+LF: das zweite Zeichen ist leer
      konsoleN = 0;
      konsoleTippt = false;
    } else if (c >= ' ') {
      konsoleTippt = true;
      if (konsoleN < sizeof(konsoleZeile) - 1) konsoleZeile[konsoleN++] = c;
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

  if (detectSensor()) Serial.printf("Sensor: %s  (%.4f uT je Zaehler)\n", sensorName, scale_uT);
  else                Serial.println(F("Kein Magnetometer gefunden (0x2C / 0x0D / 0x1E)."));

  korpusLaden();
  beschriftungLaden();
  Serial.printf("Lerndaten: %u Beobachtungen | Ort \"%s\" Sensor \"%s\"\n",
                korpus.n, ortName, sensorId);

  schwelleLaden();
  alphaLaden();
  Serial.printf("Schwelle %.2f uT (t <uT>), alpha %.3f (a <wert>)\n", schwelleUT, alphaMittel);

  // Nach Einschalten oder Reset gilt der Platz als frei: die Referenz wird
  // immer neu bestimmt, eine gespeicherte wird bewusst nicht geladen. Sie
  // koennte von einem anderen Ort, einem anderen Messbereich oder mit
  // Fahrzeug entstanden sein -- dann meldete der Sensor dauerhaft "belegt".
  Serial.println(F("Start: Platz gilt als frei -- bestimme Referenz."));
  referenzNeu();

  webStart();
#if USE_LORA
  loraSetup();
#endif
  Serial.println(F("Konsole (Befehl + Enter): n = reset, r = Referenz neu, m = Beobachtung"));
  Serial.println(F("  l = Liste, x = Liste loeschen, i = Info, s = Selbsttest, t <uT> = Schwelle, a <alpha>"));
#if USE_LORA
  Serial.println(F("  f = LoRa-Meldung erzwingen"));
#endif
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

#if USE_LORA
  loraPflegen();
#endif

  if (jetzt - tPrint >= 1000 && !konsoleTippt) {
    tPrint = jetzt;
    // Laufendes Mittel je Achse, Abweichung des letzten Werts davon, Betraege.
    Serial.printf("%-12s x=%7.2f y=%7.2f z=%7.2f  dx=%7.2f dy=%7.2f dz=%7.2f  abs=%6.2f dAbs=%6.2f  S=%.2f\n",
                  zustandText(), bMittel[0], bMittel[1], bMittel[2],
                  dVek[0], dVek[1], dVek[2], betrag3(bMittel), dAbsAkt, schwelleEin());
  }
}
