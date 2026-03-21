#include <Arduino.h>
#include <driver/i2s.h>
#include <HTTPClient.h>
#include <M5Stack.h>
#include <SD.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>

#include "config.h"
#include "streaming_tts.h"
#include "audio_cache_manager.h"

namespace {

enum class Mood : uint8_t {
  Happy = 0,
  Sleepy,
  Curious,
  Excited,
  Calm,
  Count
};

struct PetState {
  Mood mood = Mood::Happy;
  bool wifiReady = false;
  size_t storyCount = 0;
  size_t riddleCount = 0;
};

PetState gState;
StreamingTts gTts;
AudioCacheManager gCache;

uint16_t moodColor(Mood mood) {
  switch (mood) {
    case Mood::Happy:
      return TFT_YELLOW;
    case Mood::Sleepy:
      return 0x7B5C;
    case Mood::Curious:
      return TFT_CYAN;
    case Mood::Excited:
      return 0xFBEF;
    case Mood::Calm:
      return TFT_GREEN;
    default:
      return TFT_WHITE;
  }
}

int8_t gBlinkTimer = 0;
uint8_t gBlinkState = 0;
uint8_t gAnimPhase = 0;
uint32_t gLastAnimTime = 0;
int gLastOffsetY = 0;

void drawFace(Mood mood, int offsetY = 0, bool blink = false) {
  const uint16_t color = moodColor(mood);
  const uint16_t bgColor = TFT_BLACK;
  int cx = 160;
  int cy = 115 + offsetY;

  M5.Lcd.fillCircle(cx, cy, 55, bgColor);
  M5.Lcd.drawCircle(cx, cy, 55, color);

  int eyeY = cy - 8;
  int eyeR = blink ? 2 : 8;

  if (blink) {
    M5.Lcd.drawFastHLine(cx - 28, eyeY, 16, color);
    M5.Lcd.drawFastHLine(cx + 12, eyeY, 16, color);
  } else {
    M5.Lcd.fillCircle(cx - 20, eyeY, eyeR, color);
    M5.Lcd.fillCircle(cx + 20, eyeY, eyeR, color);
    M5.Lcd.fillCircle(cx - 18, eyeY - 3, 2, bgColor);
    M5.Lcd.fillCircle(cx + 22, eyeY - 3, 2, bgColor);
  }

  int mouthY = cy + 18;
  switch (mood) {
    case Mood::Happy:
      M5.Lcd.drawLine(cx - 18, mouthY, cx - 10, mouthY + 8, color);
      M5.Lcd.drawLine(cx - 10, mouthY + 8, cx + 10, mouthY + 8, color);
      M5.Lcd.drawLine(cx + 10, mouthY + 8, cx + 18, mouthY, color);
      if (blink == false) {
        M5.Lcd.fillTriangle(cx - 30, cy - 35, cx - 22, cy - 40, cx - 22, cy - 32, color);
        M5.Lcd.fillTriangle(cx + 30, cy - 35, cx + 22, cy - 40, cx + 22, cy - 32, color);
      }
      break;
    case Mood::Sleepy:
      M5.Lcd.drawFastHLine(cx - 12, mouthY, 24, color);
      if (!blink) {
        M5.Lcd.drawString("z", cx + 35, cy - 30);
        M5.Lcd.drawString("z", cx + 48, cy - 45);
      }
      break;
    case Mood::Curious:
      M5.Lcd.drawCircle(cx, mouthY + 2, 6, color);
      if (!blink) {
        M5.Lcd.drawCircle(cx - 35, cy - 20, 3, color);
        M5.Lcd.drawCircle(cx + 35, cy - 20, 3, color);
      }
      break;
    case Mood::Excited:
      M5.Lcd.drawLine(cx - 20, mouthY, cx - 10, mouthY + 10, color);
      M5.Lcd.drawLine(cx - 10, mouthY + 10, cx + 10, mouthY + 10, color);
      M5.Lcd.drawLine(cx + 10, mouthY + 10, cx + 20, mouthY, color);
      if (!blink) {
        M5.Lcd.drawCircle(cx - 32, cy - 30, 5, color);
        M5.Lcd.drawCircle(cx + 32, cy - 30, 5, color);
      }
      break;
    case Mood::Calm:
      M5.Lcd.drawFastHLine(cx - 12, mouthY, 24, color);
      M5.Lcd.drawPixel(cx - 13, mouthY - 1, color);
      M5.Lcd.drawPixel(cx + 13, mouthY - 1, color);
      break;
    default:
      break;
  }
}

void drawFaceArea(int offsetY, bool blink) {
  M5.Lcd.fillRect(80, 40, 160, 160, TFT_BLACK);
  drawFace(gState.mood, offsetY, blink);
}

void renderUi() {
  M5.Lcd.fillScreen(TFT_BLACK);

  M5.Lcd.setTextDatum(TL_DATUM);
  M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Lcd.setTextSize(2);
  M5.Lcd.drawString(String(Config::PET_NAME), 10, 10);

  drawFace(gState.mood, 0, false);
  gLastOffsetY = 0;
}

void animateFace() {
  uint32_t now = millis();
  if (now - gLastAnimTime < 50) return;
  gLastAnimTime = now;

  gAnimPhase++;
  gBlinkTimer--;

  if (gBlinkTimer <= 0) {
    if (gBlinkState == 0 && (esp_random() % 60 == 0)) {
      gBlinkState = 1;
      gBlinkTimer = 2;
    } else if (gBlinkState == 1) {
      gBlinkState = 2;
      gBlinkTimer = 2;
    } else if (gBlinkState == 2) {
      gBlinkState = 0;
      gBlinkTimer = 60 + esp_random() % 60;
    }
  }

  int bounceY = sin(gAnimPhase * 0.08) * 4;

  if (bounceY != gLastOffsetY || gBlinkState == 1 || gBlinkState == 2) {
    bool blink = (gBlinkState == 1);
    drawFaceArea(bounceY, blink);
    gLastOffsetY = bounceY;
  }
}

void renderPortalUi(const String& line1, const String& line2, const String& line3) {
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setTextDatum(TL_DATUM);
  M5.Lcd.setTextColor(CYAN, BLACK);
  M5.Lcd.setTextSize(2);
  M5.Lcd.drawString("Wi-Fi Setup", 10, 10);
  M5.Lcd.drawFastHLine(10, 32, 300, DARKGREY);

  M5.Lcd.setTextColor(WHITE, BLACK);
  M5.Lcd.drawString(line1, 10, 50);
  M5.Lcd.setTextColor(YELLOW, BLACK);
  M5.Lcd.drawString(line2, 10, 80);
  M5.Lcd.setTextColor(WHITE, BLACK);
  M5.Lcd.drawString("IP:", 10, 120);
  M5.Lcd.setTextColor(YELLOW, BLACK);
  M5.Lcd.drawString(line3, 10, 145);

  M5.Lcd.setTextColor(LIGHTGREY, BLACK);
  M5.Lcd.setTextSize(1);
  M5.Lcd.drawString("Connect to hotspot, open browser", 10, 200);
  M5.Lcd.drawString("Select Wi-Fi and enter password", 10, 215);
}

void updateCacheCounts() {
  gState.storyCount = gCache.getCacheCount(AudioType::Story);
  gState.riddleCount = gCache.getCacheCount(AudioType::Riddle);
}

void connectWifi() {
  Serial.println("[wifi] connecting...");

  WiFi.mode(WIFI_STA);
  WiFiManager wm;
  String portalApName;
  wm.setAPCallback([&portalApName](WiFiManager* wm) {
    portalApName = wm->getConfigPortalSSID();
    Serial.print("[wifi] portal ap: ");
    Serial.println(portalApName);
    renderPortalUi("Connect to hotspot:", portalApName, "192.168.4.1");
  });
  wm.setConfigPortalTimeout(Config::WIFI_PORTAL_TIMEOUT_SECONDS);
  wm.setConnectTimeout(15);
  wm.setShowInfoErase(false);
  wm.setShowInfoUpdate(false);

  bool connected = false;
  if (strlen(Config::WIFI_SSID) > 0) {
    WiFi.begin(Config::WIFI_SSID, Config::WIFI_PASSWORD);
    for (uint8_t attempt = 0; attempt < 30 && WiFi.status() != WL_CONNECTED; ++attempt) {
      delay(500);
    }
    connected = (WiFi.status() == WL_CONNECTED);
  }

  if (!connected) {
    renderPortalUi("Trying saved WiFi...", "Portal opens if failed", "192.168.4.1");
    connected = wm.autoConnect(Config::WIFI_PORTAL_AP_NAME, Config::WIFI_PORTAL_PASSWORD);
  }

  gState.wifiReady = connected;
  if (gState.wifiReady) {
    Serial.print("[wifi] connected, ip=");
    Serial.println(WiFi.localIP());
    gTts.begin();
    gCache.begin();
    gCache.setOnDownloadComplete([](AudioType type, bool success) {
      updateCacheCounts();
      if (success) {
        Serial.printf("[cache] download complete, type=%d\n", (int)type);
      }
      renderUi();
    });
    updateCacheCounts();
  } else {
    Serial.println("[wifi] failed");
  }
}

void initAudio() {
  const i2s_config_t i2sConfig = {
      .mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN),
      .sample_rate = static_cast<int>(Config::TTS_SAMPLE_RATE),
      .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
      .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
      .communication_format = I2S_COMM_FORMAT_STAND_MSB,
      .intr_alloc_flags = 0,
      .dma_buf_count = 8,
      .dma_buf_len = 512,
      .use_apll = false,
      .tx_desc_auto_clear = true,
      .fixed_mclk = 0,
  };

  i2s_driver_install(I2S_NUM_0, &i2sConfig, 0, nullptr);
  i2s_set_pin(I2S_NUM_0, nullptr);
  i2s_set_dac_mode(I2S_DAC_CHANNEL_LEFT_EN);
  i2s_zero_dma_buffer(I2S_NUM_0);
}

String generateStoryPrompt() {
  const char* prompts[] = {
    "请讲一个关于小动物的温馨故事。",
    "请讲一个关于友谊的儿童故事。",
    "请讲一个关于勇敢的小故事。",
    "请讲一个关于魔法的故事。",
    "请讲一个关于森林的故事。"
  };
  int idx = esp_random() % 5;
  return String(prompts[idx]);
}

String generateRiddlePrompt() {
  const char* prompts[] = {
    "请出一个关于动物的谜语。",
    "请出一个关于水果的谜语。",
    "请出一个关于自然现象的谜语。",
    "请出一个关于日常用品的谜语。",
    "请出一个关于交通工具的谜语。"
  };
  int idx = esp_random() % 5;
  return String(prompts[idx]);
}

void backgroundDownloadTask() {
  static uint32_t lastCheck = 0;
  const uint32_t checkInterval = 3000;

  if (!gState.wifiReady) return;

  uint32_t now = millis();
  if (now - lastCheck < checkInterval) return;
  lastCheck = now;

  if (!gCache.isDownloading()) {
    if (gCache.needsRefill(AudioType::Story)) {
      Serial.println("[bg] downloading story...");
      gCache.startDownload(AudioType::Story, generateStoryPrompt());
    } else if (gCache.needsRefill(AudioType::Riddle)) {
      Serial.println("[bg] downloading riddle...");
      gCache.startDownload(AudioType::Riddle, generateRiddlePrompt());
    }
  }
}

void playCachedAudio(AudioType type) {
  String path = gCache.pickRandom(type);

  if (path.isEmpty()) {
    if (!gCache.isDownloading() && gState.wifiReady) {
      gCache.startDownload(type,
        type == AudioType::Story ? generateStoryPrompt() : generateRiddlePrompt());
    }
    return;
  }

  gTts.playFromFile(path);
}

void nextMood() {
  const uint8_t next = (static_cast<uint8_t>(gState.mood) + 1) %
                       static_cast<uint8_t>(Mood::Count);
  gState.mood = static_cast<Mood>(next);
}

}  // namespace

void initSdCard() {
  Serial.println("[sd] initializing SD card");
  SPI.begin(18, 19, 23, 4);
  if (!SD.begin(4, SPI)) {
    Serial.println("[sd] init failed!");
    return;
  }
  Serial.printf("[sd] card size: %llu MB\n", SD.totalBytes() / (1024 * 1024));
}

void setup() {
  M5.begin();
  M5.Power.begin();
  Serial.begin(115200);
  Serial.println();
  Serial.println("[boot] desk pet starting");
  M5.Lcd.setRotation(1);
  M5.Lcd.setTextSize(2);

  renderUi();
  initSdCard();
  initAudio();
  connectWifi();
}

void loop() {
  M5.update();
  gTts.loop();
  gCache.loop();

  animateFace();
  updateCacheCounts();

  if (M5.BtnA.wasPressed()) {
    nextMood();
    renderUi();
  }

  if (M5.BtnB.wasPressed()) {
    playCachedAudio(AudioType::Story);
  }

  if (M5.BtnC.wasPressed()) {
    playCachedAudio(AudioType::Riddle);
  }

  backgroundDownloadTask();

  delay(20);
}
