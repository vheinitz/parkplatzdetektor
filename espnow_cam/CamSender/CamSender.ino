// ESP32-CAM -> ESP-NOW Sender
//
// Schickt JPEG-Bilder per ESP-NOW an die Bridge am Rechner. Umgeht damit
// Router, Repeater, DHCP und ARP komplett — es gibt keine IP-Ebene.
//
// Board-spezifisch (siehe ESP32Cam_Server):
//   - Echter PWDN-Power-Cycle vor esp_camera_init(), sonst schlaegt der Init
//     mit 0x106 ESP_ERR_NOT_SUPPORTED fehl. Ein Reset ueber RTS/EN startet
//     nur den ESP32 neu, nicht den OV2640.
//   - Dieses Board hat kein PSRAM -> Framebuffer im DRAM, kleine Aufloesung.
#include "esp_camera.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>

// AI-Thinker Pinbelegung
#define PWDN_GPIO_NUM  32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM   0
#define SIOD_GPIO_NUM  26
#define SIOC_GPIO_NUM  27
#define Y9_GPIO_NUM    35
#define Y8_GPIO_NUM    34
#define Y7_GPIO_NUM    39
#define Y6_GPIO_NUM    36
#define Y5_GPIO_NUM    21
#define Y4_GPIO_NUM    19
#define Y3_GPIO_NUM    18
#define Y2_GPIO_NUM     5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM  23
#define PCLK_GPIO_NUM  22
#define LED_GPIO_NUM    4

// MAC der Bridge (ESP32-C3 am Rechner)
static const uint8_t BRIDGE_MAC[6] = {0x0C, 0x4E, 0xA0, 0x30, 0x7A, 0xBC};

#define ESPNOW_CHANNEL 1
#define CHUNK_PAYLOAD  240          // 250 Byte ESP-NOW minus 10 Byte Header
#define MAX_FRAME      120000       // VGA bei Qualitaet 12 bleibt klar darunter
#define FRAME_INTERVAL 1000         // ein Bild pro Sekunde

// Zaehler als uint16 und Laenge als uint32: bei VGA fallen ueber 200 Pakete
// pro Bild an, uint8 (max. 255 Chunks = 62 kB) waere zu knapp.
struct __attribute__((packed)) ChunkHdr {
  uint8_t  magic;         // 0xC7
  uint8_t  frame_id;
  uint16_t chunk_idx;
  uint16_t chunk_total;
  uint32_t frame_len;     // Gesamtlaenge des JPEG
};

static volatile uint32_t sendOk = 0, sendFail = 0;

static void onSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  if (status == ESP_NOW_SEND_SUCCESS) sendOk++;
  else sendFail++;
}

static void cameraPowerCycle() {
  pinMode(PWDN_GPIO_NUM, OUTPUT);
  digitalWrite(PWDN_GPIO_NUM, HIGH);
  delay(600);
  digitalWrite(PWDN_GPIO_NUM, LOW);
  delay(200);
}

static bool initCamera() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM; c.pin_d1 = Y3_GPIO_NUM;
  c.pin_d2 = Y4_GPIO_NUM; c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM; c.pin_d5 = Y7_GPIO_NUM;
  c.pin_d6 = Y8_GPIO_NUM; c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM; c.pin_pclk = PCLK_GPIO_NUM;
  c.pin_vsync = VSYNC_GPIO_NUM; c.pin_href = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM; c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM; c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size = FRAMESIZE_VGA;         // 640x480, Framebuffer im DRAM
  c.fb_location = CAMERA_FB_IN_DRAM;
  c.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  c.jpeg_quality = 12;
  c.fb_count = 1;

  // Init gelingt auf diesem Board nur zu ca. 60 % pro Versuch -> wiederholen.
  for (int t = 1; t <= 15; t++) {
    esp_err_t err = esp_camera_init(&c);
    if (err == ESP_OK) {
      Serial.printf("Kamera OK nach %d Versuch(en)\n", t);
      sensor_t *s = esp_camera_sensor_get();
      s->set_framesize(s, FRAMESIZE_VGA);    // 640x480
      s->set_quality(s, 12);
      return true;
    }
    Serial.printf("Kamera-Init %d/15: 0x%x (%s)\n", t, err, esp_err_to_name(err));
    esp_camera_deinit();
    cameraPowerCycle();
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(1200);
  Serial.println("\n\n=== ESP32-CAM ESP-NOW Sender ===");

  pinMode(LED_GPIO_NUM, OUTPUT);
  digitalWrite(LED_GPIO_NUM, LOW);       // Blitz-LED aus

  cameraPowerCycle();
  if (!initCamera()) {
    Serial.println("Kamera nicht initialisierbar - Neustart in 5 s");
    delay(5000);
    ESP.restart();
  }

  // ESP-NOW braucht WLAN im STA-Modus, aber KEINE Verbindung zu einem AP.
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_max_tx_power(84);
  Serial.printf("Eigene MAC: %s, Kanal %d\n", WiFi.macAddress().c_str(), ESPNOW_CHANNEL);

  if (esp_now_init() != ESP_OK) {
    Serial.println("esp_now_init fehlgeschlagen - Neustart");
    delay(2000);
    ESP.restart();
  }
  esp_now_register_send_cb(onSent);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BRIDGE_MAC, 6);
  peer.channel = ESPNOW_CHANNEL;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  esp_err_t pe = esp_now_add_peer(&peer);
  Serial.printf("Peer %02X:%02X:%02X:%02X:%02X:%02X: %s\n",
                BRIDGE_MAC[0], BRIDGE_MAC[1], BRIDGE_MAC[2],
                BRIDGE_MAC[3], BRIDGE_MAC[4], BRIDGE_MAC[5],
                pe == ESP_OK ? "hinzugefuegt" : esp_err_to_name(pe));
  Serial.println("Sende Bilder...");
}

void loop() {
  static uint8_t frame_id = 0;
  static unsigned long lastStat = 0, lastFrame = 0;
  static uint32_t framesSent = 0, framesDropped = 0;

  // Feste Bildrate: ein Bild pro Sekunde.
  if (millis() - lastFrame < FRAME_INTERVAL) {
    delay(10);
    return;
  }
  lastFrame = millis();

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    framesDropped++;
    return;
  }

  if (fb->len > MAX_FRAME || fb->len == 0) {
    Serial.printf("Bild uebersprungen: %u Bytes (Grenze %u)\n", fb->len, MAX_FRAME);
    esp_camera_fb_return(fb);
    framesDropped++;
    return;
  }

  uint16_t total = (fb->len + CHUNK_PAYLOAD - 1) / CHUNK_PAYLOAD;
  unsigned long tStart = millis();
  bool allSent = true;
  uint8_t pkt[sizeof(ChunkHdr) + CHUNK_PAYLOAD];
  ChunkHdr *h = (ChunkHdr *)pkt;
  h->magic = 0xC7;
  h->frame_id = frame_id;
  h->chunk_total = total;
  h->frame_len = (uint32_t)fb->len;

  for (uint16_t i = 0; i < total; i++) {
    size_t off = (size_t)i * CHUNK_PAYLOAD;
    size_t n = fb->len - off;
    if (n > CHUNK_PAYLOAD) n = CHUNK_PAYLOAD;
    h->chunk_idx = i;
    memcpy(pkt + sizeof(ChunkHdr), fb->buf + off, n);

    // Die Sendequeue ist klein. Bei NO_MEM kurz warten und erneut versuchen.
    esp_err_t r = ESP_FAIL;
    for (int a = 0; a < 20; a++) {
      r = esp_now_send(BRIDGE_MAC, pkt, sizeof(ChunkHdr) + n);
      if (r == ESP_OK) break;
      delay(2);
    }
    if (r != ESP_OK) { allSent = false; break; }
  }

  uint32_t len = fb->len;
  uint16_t w = fb->width, hgt = fb->height;
  esp_camera_fb_return(fb);
  if (allSent) framesSent++; else framesDropped++;
  frame_id++;

  Serial.printf("Bild %u: %ux%u, %u Bytes, %u Pakete in %lu ms%s\n",
                framesSent, w, hgt, len, total, millis() - tStart,
                allSent ? "" : "  (ABGEBROCHEN)");

  if (millis() - lastStat > 10000) {
    lastStat = millis();
    Serial.printf("[stat] Frames gesendet %u, verworfen %u | Pakete ok %u, fehl %u\n",
                  framesSent, framesDropped, sendOk, sendFail);
  }
}
