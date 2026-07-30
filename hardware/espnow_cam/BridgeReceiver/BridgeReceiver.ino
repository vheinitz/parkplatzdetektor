// ESP32-C3 Bridge: empfaengt JPEG-Bilder per ESP-NOW und schiebt sie
// ueber USB-Serial zum Rechner.
//
// Rahmenformat auf der seriellen Leitung:
//   A5 5A A5 5A | uint32 Laenge (little endian) | JPEG-Daten
// Textausgaben (Statistik) laufen ueber die gleiche Leitung; das Python-Tool
// sucht deshalb nach der Magic-Sequenz und ignoriert alles andere.
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>

#define ESPNOW_CHANNEL 1
#define CHUNK_PAYLOAD  240
#define MAX_FRAME      120000       // VGA bei Qualitaet 12
#define MAX_CHUNKS     512          // 512 * 240 = 122880 Byte

struct __attribute__((packed)) ChunkHdr {
  uint8_t  magic;
  uint8_t  frame_id;
  uint16_t chunk_idx;
  uint16_t chunk_total;
  uint32_t frame_len;
};

// Nur EIN Puffer: zwei mal 120 kB passen auf dem C3 nicht neben den
// WLAN-Stack. Waehrend ein fertiges Bild ausgegeben wird, werden neu
// eintreffende Pakete verworfen — bei einem Bild pro Sekunde faellt das
// nicht ins Gewicht, die Ausgabe dauert nur wenige Millisekunden.
static uint8_t  frameBuf[MAX_FRAME];
static volatile uint32_t outLen = 0;
static volatile bool     outReady = false;

static uint8_t  curId = 0xFF;
static uint32_t curLen = 0;
static uint16_t curTotal = 0;
static bool     chunkSeen[MAX_CHUNKS];
static uint16_t chunkCount = 0;

static volatile uint32_t pktRx = 0, framesDone = 0, framesIncomplete = 0, pktDropped = 0;

static void resetFrame(uint8_t id, uint32_t len, uint16_t total) {
  curId = id;
  curLen = len;
  curTotal = total;
  chunkCount = 0;
  memset(chunkSeen, 0, sizeof(chunkSeen));
}

static void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len < (int)sizeof(ChunkHdr)) return;
  const ChunkHdr *h = (const ChunkHdr *)data;
  if (h->magic != 0xC7) return;
  if (h->frame_len == 0 || h->frame_len > MAX_FRAME) return;
  if (h->chunk_total == 0 || h->chunk_total > MAX_CHUNKS) return;
  if (h->chunk_idx >= h->chunk_total) return;

  // Ausgabe laeuft noch -> Puffer nicht anfassen.
  if (outReady) { pktDropped++; return; }
  pktRx++;

  if (h->frame_id != curId) {
    if (curId != 0xFF && chunkCount < curTotal) framesIncomplete++;
    resetFrame(h->frame_id, h->frame_len, h->chunk_total);
  }

  size_t off = (size_t)h->chunk_idx * CHUNK_PAYLOAD;
  size_t n = len - sizeof(ChunkHdr);
  if (off + n > MAX_FRAME) return;
  if (chunkSeen[h->chunk_idx]) return;

  memcpy(frameBuf + off, data + sizeof(ChunkHdr), n);
  chunkSeen[h->chunk_idx] = true;
  chunkCount++;

  if (chunkCount == curTotal) {
    outLen = curLen;
    outReady = true;              // loop() gibt das Bild aus
    framesDone++;
    curId = 0xFF;
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n\n=== ESP-NOW Bridge (ESP32-C3) ===");
  Serial.printf("Puffer %u Byte, max %u Chunks, freier Heap %u\n",
                MAX_FRAME, MAX_CHUNKS, ESP.getFreeHeap());

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  Serial.printf("Eigene MAC: %s, Kanal %d\n", WiFi.macAddress().c_str(), ESPNOW_CHANNEL);

  if (esp_now_init() != ESP_OK) {
    Serial.println("esp_now_init fehlgeschlagen - Neustart");
    delay(2000);
    ESP.restart();
  }
  esp_now_register_recv_cb(onRecv);
  Serial.println("Warte auf Bilder...");
}

void loop() {
  if (outReady) {
    static const uint8_t magic[4] = {0xA5, 0x5A, 0xA5, 0x5A};
    uint32_t len = outLen;
    Serial.write(magic, 4);
    Serial.write((uint8_t *)&len, 4);
    Serial.write(frameBuf, len);
    Serial.flush();
    outReady = false;
  }

  static unsigned long lastStat = 0;
  if (millis() - lastStat > 10000) {
    lastStat = millis();
    Serial.printf("\n[stat] Pakete %u, Bilder komplett %u, unvollstaendig %u, Pakete verworfen %u\n",
                  pktRx, framesDone, framesIncomplete, pktDropped);
  }
  delay(1);
}
