/*
 * LoRa-Gateway -- Empfaenger der Parksensoren, Bruecke zum Rechner
 *
 * Haelt nichts fest und entscheidet nichts. Es nimmt LoRa-Pakete an, prueft
 * die Pruefsumme und schreibt jedes gute Paket als eine Zeile auf die serielle
 * Schnittstelle. Alles Weitere macht gateway.py auf dem Rechner.
 *
 * Diese Arbeitsteilung ist Absicht: der Mikrocontroller hat keine Uhr, kein
 * WLAN-Zertifikat und keinen Speicher fuer eine Warteschlange. Der Rechner hat
 * alles davon. Faellt die Verbindung zum Server aus, sammelt der Rechner --
 * nicht dieser Sketch.
 *
 * Funkmodul: SX1276/RFM95 (868 MHz, Europa), Bibliothek "LoRa" von
 * Sandeep Mistry:   arduino-cli lib install LoRa
 *
 * Verdrahtung (ESP32-C3, frei waehlbar -- der C3 routet SPI ueber die Matrix):
 *   RFM95        ESP32-C3
 *   VCC   ->     3V3          ACHTUNG: 3,3 V, das Modul ist nicht 5-V-fest
 *   GND   ->     GND
 *   SCK   ->     GPIO 6
 *   MISO  ->     GPIO 1
 *   MOSI  ->     GPIO 7
 *   NSS   ->     GPIO 10
 *   RST   ->     GPIO 3
 *   DIO0  ->     nicht noetig (siehe unten)
 *   ANT   ->     8,6 cm Draht genuegt fuer erste Versuche
 *
 * GPIO 2, 8 und 9 bleiben frei: das sind Strapping-Pins. Zieht das Funkmodul
 * einen davon beim Einschalten auf den falschen Pegel, startet der C3 nicht
 * oder landet im Bootloader.
 *
 * DIO0 wird nicht gebraucht, weil loop() ohnehin alle paar Millisekunden
 * LoRa.parsePacket() aufruft und damit das Interrupt-Register pollt. Das spart
 * eine Leitung und einen Strapping-freien Pin. Wer das Modul fest verbaut,
 * kann DIO0 anschliessen und unten eintragen.
 *
 * Boards mit fest verbautem Funkmodul -- dann nur diese Werte eintragen:
 *   Heltec WiFi LoRa 32 V2:  NSS 18, RST 14, DIO0 26, SCK 5, MISO 19, MOSI 27
 *   TTGO LoRa32 V1:          NSS 18, RST 14, DIO0 26, SCK 5, MISO 19, MOSI 27
 *   Arduino Uno + RFM95:     NSS 10, RST  9, DIO0  2, SPI liegt auf 13/12/11
 *
 * Serielle Ausgabe, eine Zeile je Ereignis:
 *   RX,PS1,<knoten>,<F|B|?>,<mV>,<seq>,<crc>,<rssi>,<snr>
 * Zeilen mit '#' am Anfang sind Kommentar und duerfen ignoriert werden.
 *
 * Befehle vom Rechner (eine Zeile, Gross-/Kleinschreibung egal):
 *   POLL   gibt den Ringpuffer noch einmal aus (Rechner war noch nicht da)
 *   STAT   Zaehler: empfangen, verworfen, Laufzeit
 *   PING   antwortet PONG -- damit findet gateway.py den richtigen Port
 */

#include <SPI.h>
#include <LoRa.h>

// ---------------------------------------------------------------- Konfiguration

#define PIN_SCK   6
#define PIN_MISO  1
#define PIN_MOSI  7
#define PIN_NSS  10
#define PIN_RST   3
#define PIN_DIO0 -1            // -1 = pollen statt Interrupt

#define LORA_FREQ     868E6    // Europa. USA: 915E6, Asien teils 433E6
#define LORA_SF       7        // 7 = schnell und stromsparend, 12 = weiteste Reichweite
#define LORA_BW       125E3
#define LORA_CR       5        // 4/5
#define LORA_SYNCWORD 0x2A     // eigenes Netz; fremder Verkehr wird ignoriert

#define RING_N   16            // gepufferte Pakete fuer POLL
#define ZEILE_N  64

// ---------------------------------------------------------------- Zustand

char     ring[RING_N][ZEILE_N];
uint8_t  ringSchreib = 0;
uint8_t  ringAnzahl  = 0;

uint32_t nEmpfangen = 0;       // Pakete mit gueltiger Pruefsumme
uint32_t nVerworfen = 0;       // Pruefsumme oder Format falsch
bool     funkOk = false;

// ---------------------------------------------------------------- Pruefsumme

// CRC-16/CCITT-FALSE, Polynom 0x1021, Startwert 0xFFFF.
// Dieselbe Funktion steckt im Sensor und in gateway.py -- Testvektor:
// "123456789" ergibt 0x29B1.
uint16_t crc16(const char *daten, size_t laenge) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < laenge; i++) {
    crc ^= (uint16_t)(uint8_t)daten[i] << 8;
    for (uint8_t b = 0; b < 8; b++)
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

// ---------------------------------------------------------------- Ringpuffer

void ringAblegen(const char *zeile) {
  strncpy(ring[ringSchreib], zeile, ZEILE_N - 1);
  ring[ringSchreib][ZEILE_N - 1] = '\0';
  ringSchreib = (ringSchreib + 1) % RING_N;
  if (ringAnzahl < RING_N) ringAnzahl++;
}

void ringAusgeben() {
  Serial.printf("# poll %u\n", ringAnzahl);
  uint8_t start = (ringSchreib + RING_N - ringAnzahl) % RING_N;
  for (uint8_t i = 0; i < ringAnzahl; i++)
    Serial.println(ring[(start + i) % RING_N]);
}

// ---------------------------------------------------------------- Empfang

// Erwartet PS1,<knoten>,<status>,<mV>,<seq>,<crc>. Geprueft wird nur die
// Pruefsumme -- die Felder selbst wertet der Rechner aus. Ein Gateway, das
// den Inhalt versteht, muesste bei jeder Protokollaenderung neu geflasht
// werden; eines, das nur weiterreicht, nicht.
bool paketPruefen(const char *nutzlast) {
  const char *letztesKomma = strrchr(nutzlast, ',');
  if (!letztesKomma) return false;
  if (strncmp(nutzlast, "PS1,", 4) != 0) return false;

  size_t rumpf = (size_t)(letztesKomma - nutzlast);
  uint16_t soll = (uint16_t)strtoul(letztesKomma + 1, nullptr, 16);
  return crc16(nutzlast, rumpf) == soll;
}

void paketVerarbeiten(int laenge) {
  char nutzlast[ZEILE_N];
  int n = 0;
  while (LoRa.available() && n < (int)sizeof(nutzlast) - 1) {
    char c = (char)LoRa.read();
    if (c == '\r' || c == '\n') break;
    nutzlast[n++] = c;
  }
  nutzlast[n] = '\0';
  while (LoRa.available()) LoRa.read();          // Rest verwerfen

  int   rssi = LoRa.packetRssi();
  float snr  = LoRa.packetSnr();

  if (!paketPruefen(nutzlast)) {
    nVerworfen++;
    Serial.printf("# verworfen len=%d rssi=%d \"%s\"\n", laenge, rssi, nutzlast);
    return;
  }

  nEmpfangen++;
  char zeile[ZEILE_N];
  snprintf(zeile, sizeof(zeile), "RX,%s,%d,%.1f", nutzlast, rssi, snr);
  ringAblegen(zeile);
  Serial.println(zeile);
}

// ---------------------------------------------------------------- Befehle

void befehl(const String &roh) {
  String b = roh;
  b.trim();
  b.toUpperCase();
  if (b == "PING")      Serial.println(F("PONG carsensor-gateway 1"));
  else if (b == "POLL") ringAusgeben();
  else if (b == "STAT")
    Serial.printf("# stat empfangen=%lu verworfen=%lu laufzeit_s=%lu funk=%s\n",
                  (unsigned long)nEmpfangen, (unsigned long)nVerworfen,
                  (unsigned long)(millis() / 1000), funkOk ? "ok" : "fehler");
  else if (b.length())  Serial.printf("# unbekannt \"%s\"\n", b.c_str());
}

void konsole() {
  static String puffer;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n') { befehl(puffer); puffer = ""; }
    else if (c != '\r' && puffer.length() < 32) puffer += c;
  }
}

// ---------------------------------------------------------------- Setup/Loop

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println();
  Serial.println(F("# carsensor-gateway 1"));

  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_NSS);
  LoRa.setPins(PIN_NSS, PIN_RST, PIN_DIO0);

  if (!LoRa.begin(LORA_FREQ)) {
    Serial.println(F("# FEHLER Funkmodul antwortet nicht -- Verdrahtung und 3,3 V pruefen"));
  } else {
    LoRa.setSpreadingFactor(LORA_SF);
    LoRa.setSignalBandwidth(LORA_BW);
    LoRa.setCodingRate4(LORA_CR);
    LoRa.setSyncWord(LORA_SYNCWORD);
    LoRa.enableCrc();
    LoRa.receive();
    funkOk = true;
    Serial.printf("# bereit %.1f MHz SF%d BW%.0f kHz sync 0x%02X\n",
                  LORA_FREQ / 1e6, LORA_SF, LORA_BW / 1e3, LORA_SYNCWORD);
  }
  Serial.println(F("# befehle: PING POLL STAT"));
}

void loop() {
  konsole();

  int laenge = LoRa.parsePacket();
  if (laenge > 0) paketVerarbeiten(laenge);

  // Lebenszeichen, damit am Rechner erkennbar ist, dass die Bruecke laeuft
  // und nicht nur gerade niemand funkt.
  static uint32_t tHeartbeat = 0;
  if (millis() - tHeartbeat >= 60000) {
    tHeartbeat = millis();
    Serial.printf("# alive empfangen=%lu verworfen=%lu\n",
                  (unsigned long)nEmpfangen, (unsigned long)nVerworfen);
  }
}
