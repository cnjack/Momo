#include <Arduino.h>
#include <driver/i2s.h>
#include <FS.h>
#include <HTTPClient.h>
#include <M5Stack.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>

#include "config.h"

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
  String lastReply = "按 A 换心情\n按 B 讲故事\n按 C 出谜语";
  String status = "启动中...";
  bool wifiReady = false;
  bool ttsReady = false;
  bool spiffsReady = false;
  bool portalMode = false;
};

PetState gState;
constexpr const char* kTtsTempFile = "/tts.wav";
String gPortalApName;

struct WavInfo {
  uint32_t sampleRate = 24000;
  uint16_t channels = 1;
  uint16_t bitsPerSample = 16;
  uint32_t dataOffset = 0;
  uint32_t dataSize = 0;
};

const char* moodName(Mood mood) {
  switch (mood) {
    case Mood::Happy:
      return "开心";
    case Mood::Sleepy:
      return "困困";
    case Mood::Curious:
      return "好奇";
    case Mood::Excited:
      return "兴奋";
    case Mood::Calm:
      return "安静";
    default:
      return "未知";
  }
}

uint16_t moodColor(Mood mood) {
  switch (mood) {
    case Mood::Happy:
      return YELLOW;
    case Mood::Sleepy:
      return BLUE;
    case Mood::Curious:
      return CYAN;
    case Mood::Excited:
      return ORANGE;
    case Mood::Calm:
      return GREEN;
    default:
      return WHITE;
  }
}

String moodEmoji(Mood mood) {
  switch (mood) {
    case Mood::Happy:
      return "^_^";
    case Mood::Sleepy:
      return "-_-";
    case Mood::Curious:
      return "o_o";
    case Mood::Excited:
      return "*_*";
    case Mood::Calm:
      return "^o^";
    default:
      return ":)";
  }
}

void drawFace(Mood mood) {
  const uint16_t color = moodColor(mood);
  M5.Lcd.fillRoundRect(24, 46, 192, 150, 20, BLACK);
  M5.Lcd.drawRoundRect(24, 46, 192, 150, 20, color);

  M5.Lcd.fillCircle(78, 102, 16, color);
  M5.Lcd.fillCircle(162, 102, 16, color);

  switch (mood) {
    case Mood::Happy:
      M5.Lcd.drawLine(72, 150, 96, 170, color);
      M5.Lcd.drawLine(96, 170, 144, 170, color);
      M5.Lcd.drawLine(144, 170, 168, 150, color);
      break;
    case Mood::Sleepy:
      M5.Lcd.drawFastHLine(84, 160, 72, color);
      break;
    case Mood::Curious:
      M5.Lcd.drawCircle(120, 160, 20, color);
      break;
    case Mood::Excited:
      M5.Lcd.drawLine(84, 144, 156, 176, color);
      M5.Lcd.drawLine(84, 176, 156, 144, color);
      break;
    case Mood::Calm:
      M5.Lcd.drawLine(84, 160, 156, 160, color);
      M5.Lcd.drawPixel(82, 159, color);
      M5.Lcd.drawPixel(158, 159, color);
      break;
    default:
      break;
  }

  M5.Lcd.setTextDatum(MC_DATUM);
  M5.Lcd.setTextColor(color, BLACK);
  M5.Lcd.setTextSize(2);
  M5.Lcd.drawString(moodEmoji(mood), 120, 186);
}

void drawWrappedText(const String& text, int x, int y, int maxWidth, uint16_t color) {
  M5.Lcd.setTextColor(color, BLACK);
  M5.Lcd.setTextSize(2);

  String line;
  int cursorY = y;
  for (size_t i = 0; i < text.length(); ++i) {
    line += text[i];
    if (M5.Lcd.textWidth(line) > maxWidth || text[i] == '\n') {
      String out = line;
      if (text[i] != '\n') {
        out.remove(out.length() - 1);
        --i;
      }
      M5.Lcd.drawString(out, x, cursorY);
      line = "";
      cursorY += 18;
      if (cursorY > 232) {
        break;
      }
    }
  }
  if (!line.isEmpty() && cursorY <= 232) {
    M5.Lcd.drawString(line, x, cursorY);
  }
}

void renderUi() {
  M5.Lcd.fillScreen(BLACK);

  M5.Lcd.setTextDatum(TL_DATUM);
  M5.Lcd.setTextColor(WHITE, BLACK);
  M5.Lcd.setTextSize(2);
  M5.Lcd.drawString(String(Config::PET_NAME), 10, 10);
  M5.Lcd.drawRightString(moodName(gState.mood), 230, 10, 2);

  M5.Lcd.drawFastHLine(10, 32, 220, DARKGREY);

  drawFace(gState.mood);

  M5.Lcd.drawRect(10, 210, 220, 68, DARKGREY);
  drawWrappedText(gState.lastReply, 18, 220, 204, WHITE);

  M5.Lcd.setTextColor(LIGHTGREY, BLACK);
  M5.Lcd.drawString("A心情  B故事  C谜语", 12, 288);
  M5.Lcd.drawRightString(gState.status, 228, 288, 2);
}

void renderPortalUi(const String& line1, const String& line2, const String& line3) {
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setTextDatum(TL_DATUM);
  M5.Lcd.setTextColor(CYAN, BLACK);
  M5.Lcd.setTextSize(2);
  M5.Lcd.drawString("Wi-Fi 配网", 10, 10);
  M5.Lcd.drawFastHLine(10, 32, 220, DARKGREY);

  M5.Lcd.setTextColor(WHITE, BLACK);
  M5.Lcd.drawString(line1, 10, 52);
  M5.Lcd.setTextColor(YELLOW, BLACK);
  M5.Lcd.drawString(line2, 10, 88);
  M5.Lcd.setTextColor(WHITE, BLACK);
  M5.Lcd.drawString("地址:", 10, 136);
  M5.Lcd.setTextColor(YELLOW, BLACK);
  M5.Lcd.drawString(line3, 10, 164);

  M5.Lcd.setTextColor(LIGHTGREY, BLACK);
  M5.Lcd.setTextSize(1);
  M5.Lcd.drawString("手机连热点后打开浏览器", 10, 270);
  M5.Lcd.drawString("选 Wi-Fi 并输入密码", 10, 286);
}

void setStatus(const String& status) {
  gState.status = status;
  renderUi();
}

void onPortalStarted(WiFiManager* wm) {
  gState.portalMode = true;
  gPortalApName = wm->getConfigPortalSSID();
  Serial.print("[wifi] portal ap: ");
  Serial.println(gPortalApName);
  renderPortalUi("连接设备热点:", gPortalApName,
                 "192.168.4.1");
}

void connectWifi() {
  setStatus("连 Wi-Fi");
  Serial.println("[wifi] connecting...");

  WiFi.mode(WIFI_STA);
  WiFiManager wm;
  wm.setAPCallback(onPortalStarted);
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
    renderPortalUi("正在尝试连接已保存 Wi-Fi", "如果失败会自动开启配网页面",
                   "192.168.4.1");
    connected = wm.autoConnect(Config::WIFI_PORTAL_AP_NAME, Config::WIFI_PORTAL_PASSWORD);
  }

  gState.wifiReady = connected;
  if (gState.wifiReady) {
    gState.portalMode = false;
    Serial.print("[wifi] connected, ip=");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("[wifi] failed");
  }
  setStatus(gState.wifiReady ? "已联网" : "离线");
}

void initAudio() {
  gState.spiffsReady = SPIFFS.begin(true);
  if (!gState.spiffsReady) {
    setStatus("SPIFFS失败");
    return;
  }

  const i2s_config_t i2sConfig = {
      .mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN),
      .sample_rate = static_cast<int>(Config::TTS_SAMPLE_RATE),
      .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
      .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
      .communication_format = I2S_COMM_FORMAT_STAND_MSB,
      .intr_alloc_flags = 0,
      .dma_buf_count = 8,
      .dma_buf_len = 256,
      .use_apll = false,
      .tx_desc_auto_clear = true,
      .fixed_mclk = 0,
  };

  i2s_driver_install(I2S_NUM_0, &i2sConfig, 0, nullptr);
  i2s_set_pin(I2S_NUM_0, nullptr);
  i2s_set_dac_mode(I2S_DAC_CHANNEL_LEFT_EN);
  i2s_zero_dma_buffer(I2S_NUM_0);
}

bool downloadFileToSpiffs(const String& url, const char* path) {
  if (!gState.spiffsReady) {
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, url)) {
    return false;
  }

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  File file = SPIFFS.open(path, FILE_WRITE);
  if (!file) {
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buffer[1024];
  while (http.connected()) {
    const size_t available = stream->available();
    if (available == 0) {
      delay(1);
      continue;
    }

    const int readBytes = stream->readBytes(buffer, min(available, sizeof(buffer)));
    if (readBytes <= 0) {
      break;
    }
    file.write(buffer, static_cast<size_t>(readBytes));
  }

  file.close();
  http.end();
  return true;
}

bool readExact(File& file, uint8_t* buffer, size_t size) {
  return file.read(buffer, size) == static_cast<int>(size);
}

bool parseWavFile(File& file, WavInfo& info) {
  char riff[4];
  uint32_t chunkSize = 0;
  char wave[4];

  if (!readExact(file, reinterpret_cast<uint8_t*>(riff), sizeof(riff)) ||
      !readExact(file, reinterpret_cast<uint8_t*>(&chunkSize), sizeof(chunkSize)) ||
      !readExact(file, reinterpret_cast<uint8_t*>(wave), sizeof(wave))) {
    return false;
  }

  if (strncmp(riff, "RIFF", 4) != 0 || strncmp(wave, "WAVE", 4) != 0) {
    return false;
  }

  bool fmtFound = false;
  bool dataFound = false;

  while (file.available()) {
    char chunkId[4];
    uint32_t size = 0;
    if (!readExact(file, reinterpret_cast<uint8_t*>(chunkId), sizeof(chunkId)) ||
        !readExact(file, reinterpret_cast<uint8_t*>(&size), sizeof(size))) {
      return false;
    }

    if (strncmp(chunkId, "fmt ", 4) == 0) {
      uint16_t audioFormat = 0;
      if (!readExact(file, reinterpret_cast<uint8_t*>(&audioFormat), sizeof(audioFormat)) ||
          !readExact(file, reinterpret_cast<uint8_t*>(&info.channels), sizeof(info.channels)) ||
          !readExact(file, reinterpret_cast<uint8_t*>(&info.sampleRate), sizeof(info.sampleRate))) {
        return false;
      }

      file.seek(file.position() + 6);  // byteRate + blockAlign
      if (!readExact(file, reinterpret_cast<uint8_t*>(&info.bitsPerSample), sizeof(info.bitsPerSample))) {
        return false;
      }

      if (audioFormat != 1) {
        return false;
      }

      if (size > 16) {
        file.seek(file.position() + (size - 16));
      }
      fmtFound = true;
      continue;
    }

    if (strncmp(chunkId, "data", 4) == 0) {
      info.dataOffset = file.position();
      info.dataSize = size;
      dataFound = true;
      break;
    }

    file.seek(file.position() + size);
  }

  return fmtFound && dataFound && info.bitsPerSample == 16;
}

bool playWavFromSpiffs(const char* path) {
  if (!SPIFFS.exists(path)) {
    return false;
  }

  File file = SPIFFS.open(path, FILE_READ);
  if (!file) {
    return false;
  }

  WavInfo info;
  if (!parseWavFile(file, info)) {
    file.close();
    return false;
  }

  i2s_set_clk(I2S_NUM_0, info.sampleRate, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
  file.seek(info.dataOffset);

  constexpr size_t kInputBufferBytes = 1024;
  uint8_t inputBuffer[kInputBufferBytes];
  uint16_t outputBuffer[kInputBufferBytes / 2];

  uint32_t remaining = info.dataSize;
  while (remaining > 0) {
    const size_t bytesToRead = min<size_t>(remaining, sizeof(inputBuffer));
    const int bytesRead = file.read(inputBuffer, bytesToRead);
    if (bytesRead <= 0) {
      break;
    }

    const int sampleCount = bytesRead / 2;
    const int16_t* samples = reinterpret_cast<const int16_t*>(inputBuffer);
    for (int i = 0; i < sampleCount; ++i) {
      const uint8_t dacValue = static_cast<uint8_t>((static_cast<int32_t>(samples[i]) + 32768) >> 8);
      outputBuffer[i] = static_cast<uint16_t>(dacValue) << 8;
    }

    size_t bytesWritten = 0;
    i2s_write(I2S_NUM_0, outputBuffer, sampleCount * sizeof(uint16_t), &bytesWritten, portMAX_DELAY);
    remaining -= static_cast<uint32_t>(bytesRead);
    delay(1);
  }

  i2s_zero_dma_buffer(I2S_NUM_0);
  file.close();
  return true;
}

String buildSystemPrompt() {
  String prompt =
      "你是一个陪小朋友互动的小桌宠，名字叫";
  prompt += Config::PET_NAME;
  prompt +=
      "。语气温柔、活泼、简短。避免恐怖内容。回答尽量控制在80字以内。"
      "如果是讲故事，要讲适合儿童的小故事；如果是出谜语，要先出题，下一次再解释答案。";
  return prompt;
}

String buildUserPrompt(const String& actionPrompt) {
  String prompt = "当前心情是";
  prompt += moodName(gState.mood);
  prompt += "。";
  prompt += actionPrompt;
  return prompt;
}

bool postJson(const char* url, const char* authKey, const String& body, String& response) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, url)) {
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  if (authKey && strlen(authKey) > 0) {
    http.addHeader("Authorization", String("Bearer ") + authKey);
  }

  const int httpCode = http.POST(body);
  if (httpCode <= 0) {
    http.end();
    return false;
  }

  response = http.getString();
  http.end();
  return httpCode >= 200 && httpCode < 300;
}

String parseChatReply(const String& payload) {
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    return "我刚刚有点走神啦，再试一次吧。";
  }

  // Compatible with many OpenAI-style chat completion responses.
  if (doc["choices"][0]["message"]["content"].is<const char*>()) {
    return String(doc["choices"][0]["message"]["content"].as<const char*>());
  }

  return "我还没学会读这个接口的返回格式。";
}

String requestChatReply(const String& actionPrompt) {
  if (!gState.wifiReady) {
    return "我现在没连上网，不过我们也可以先玩表情游戏。";
  }

  DynamicJsonDocument doc(2048);
  doc["model"] = Config::CHAT_MODEL;
  JsonArray messages = doc["messages"].to<JsonArray>();

  JsonObject sys = messages.add<JsonObject>();
  sys["role"] = "system";
  sys["content"] = buildSystemPrompt();

  JsonObject user = messages.add<JsonObject>();
  user["role"] = "user";
  user["content"] = buildUserPrompt(actionPrompt);

  doc["temperature"] = 0.8;

  String body;
  serializeJson(doc, body);

  String response;
  if (!postJson(Config::CHAT_API_URL, Config::CHAT_API_KEY, body, response)) {
    return "我刚刚没连上 AI 大脑，等一下再试试。";
  }

  return parseChatReply(response);
}

String parseTtsAudioUrl(const String& payload) {
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    return "";
  }

  if (doc["output"]["audio"]["url"].is<const char*>()) {
    return String(doc["output"]["audio"]["url"].as<const char*>());
  }

  return "";
}

bool requestTts(const String& text, String& audioUrl) {
  if (!Config::ENABLE_TTS || strlen(Config::TTS_API_URL) == 0 ||
      strlen(Config::TTS_API_KEY) == 0) {
    return false;
  }

  DynamicJsonDocument doc(1024);
  doc["model"] = Config::TTS_MODEL;
  JsonObject input = doc["input"].to<JsonObject>();
  input["text"] = text;
  input["voice"] = Config::TTS_VOICE;
  input["language_type"] = Config::TTS_LANGUAGE_TYPE;
  JsonObject parameters = doc["parameters"].to<JsonObject>();
  parameters["sample_rate"] = Config::TTS_SAMPLE_RATE;
  parameters["response_format"] = Config::TTS_RESPONSE_FORMAT;

  String body;
  serializeJson(doc, body);

  String response;
  if (!postJson(Config::TTS_API_URL, Config::TTS_API_KEY, body, response)) {
    return false;
  }

  audioUrl = parseTtsAudioUrl(response);
  return !audioUrl.isEmpty();
}

void askPet(const String& actionPrompt) {
  setStatus("思考中");
  Serial.print("[pet] prompt: ");
  Serial.println(actionPrompt);
  gState.lastReply = requestChatReply(actionPrompt);
  Serial.print("[pet] reply: ");
  Serial.println(gState.lastReply);
  renderUi();

  if (Config::ENABLE_TTS) {
    setStatus("生成语音");
    String audioUrl;
    gState.ttsReady = requestTts(gState.lastReply, audioUrl);
    if (gState.ttsReady) {
      setStatus("下载语音");
      if (downloadFileToSpiffs(audioUrl, kTtsTempFile)) {
        gState.lastReply += "\n[播放中]";
        renderUi();
        if (playWavFromSpiffs(kTtsTempFile)) {
          setStatus("已回复+播放");
        } else {
          gState.lastReply += "\n[播放失败]";
          setStatus("仅文本");
        }
      } else {
        gState.lastReply += "\n[语音下载失败]";
        setStatus("仅文本");
      }
    } else {
      setStatus("仅文本");
    }
    renderUi();
    return;
  }

  setStatus("已回复");
}

void nextMood() {
  const uint8_t next = (static_cast<uint8_t>(gState.mood) + 1) %
                       static_cast<uint8_t>(Mood::Count);
  gState.mood = static_cast<Mood>(next);
  gState.lastReply = String("我现在是") + moodName(gState.mood) + "模式。";
  setStatus("换心情");
}

}  // namespace

void setup() {
  M5.begin();
  M5.Power.begin();
  Serial.begin(115200);
  Serial.println();
  Serial.println("[boot] desk pet starting");
  M5.Lcd.setRotation(0);
  M5.Lcd.setTextFont(2);

  renderUi();
  initAudio();
  connectWifi();
}

void loop() {
  M5.update();

  if (M5.BtnA.wasPressed()) {
    nextMood();
  }

  if (M5.BtnB.wasPressed()) {
    askPet("请讲一个适合5到8岁小朋友的小故事。");
  }

  if (M5.BtnC.wasPressed()) {
    askPet("请出一个适合小朋友的谜语，不要立刻说答案。");
  }

  delay(20);
}
