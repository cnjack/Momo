#include "audio_cache_manager.h"
#include "streaming_tts.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

AudioCacheManager::AudioCacheManager()
  : mutex_(nullptr)
  , downloadTaskHandle_(nullptr)
  , stopRequested_(false)
  , downloadBuffer_(nullptr) {
  mutex_ = xSemaphoreCreateMutex();
  pendingDownload_.active = false;
}

AudioCacheManager::~AudioCacheManager() {
  stopAll();
  if (downloadTaskHandle_) {
    vTaskDelete(downloadTaskHandle_);
    downloadTaskHandle_ = nullptr;
  }
  if (mutex_) {
    vSemaphoreDelete(mutex_);
  }
  free(downloadBuffer_);
}

bool AudioCacheManager::begin() {
  downloadBuffer_ = static_cast<uint8_t*>(malloc(kDownloadBufferSize));
  if (!downloadBuffer_) {
    Serial.println("[cache] buffer alloc failed");
    return false;
  }

  if (!initCacheDir()) {
    Serial.println("[cache] init cache dir failed");
    return false;
  }

  scanCacheDir();
  Serial.printf("[cache] scanned: stories=%d, riddles=%d\n",
                getCacheCount(AudioType::Story),
                getCacheCount(AudioType::Riddle));

  return true;
}

bool AudioCacheManager::initCacheDir() {
  if (!SD.begin(4, SPI, 40000000)) {
    Serial.println("[cache] SD mount failed");
    return false;
  }

  if (!SD.exists(kCacheDir)) {
    if (!SD.mkdir(kCacheDir)) {
      Serial.println("[cache] mkdir failed");
      return false;
    }
  }
  return true;
}

void AudioCacheManager::scanCacheDir() {
  File root = SD.open(kCacheDir);
  if (!root || !root.isDirectory()) {
    Serial.println("[cache] cannot open cache dir");
    return;
  }

  xSemaphoreTake(mutex_, portMAX_DELAY);
  for (auto& cache : caches_) {
    cache.clear();
  }
  xSemaphoreGive(mutex_);

  File file = root.openNextFile();
  while (file) {
    if (!file.isDirectory()) {
      String name = String(file.name());
      uint32_t size = file.size();

      AudioType type = AudioType::Count;
      if (name.startsWith("story_")) {
        type = AudioType::Story;
      } else if (name.startsWith("riddle_")) {
        type = AudioType::Riddle;
      }

      if (type != AudioType::Count && name.endsWith(".wav")) {
        String fullPath = String(kCacheDir) + "/" + name;
        addToIndex(type, fullPath, size);
      }
    }
    file = root.openNextFile();
  }
  root.close();
}

String AudioCacheManager::getTypePrefix(AudioType type) const {
  switch (type) {
    case AudioType::Story:  return "story";
    case AudioType::Riddle: return "riddle";
    default: return "";
  }
}

String AudioCacheManager::generateFilePath(AudioType type) {
  String prefix = getTypePrefix(type);
  int index = 1;
  String path;
  do {
    path = String(kCacheDir) + "/" + prefix + "_" + String(index) + ".wav";
    index++;
  } while (SD.exists(path));
  return path;
}

size_t AudioCacheManager::getCacheCount(AudioType type) const {
  size_t idx = static_cast<size_t>(type);
  if (idx >= static_cast<size_t>(AudioType::Count)) return 0;

  xSemaphoreTake(mutex_, portMAX_DELAY);
  size_t count = caches_[idx].size();
  xSemaphoreGive(mutex_);
  return count;
}

bool AudioCacheManager::needsRefill(AudioType type) const {
  return getCacheCount(type) < kMinCachePerType;
}

void AudioCacheManager::clearCache() {
  File root = SD.open(kCacheDir);
  if (!root || !root.isDirectory()) {
    return;
  }

  std::vector<String> filesToDelete;
  File file = root.openNextFile();
  while (file) {
    if (!file.isDirectory()) {
      String name = String(file.name());
      if (name.endsWith(".wav")) {
        filesToDelete.push_back(String(kCacheDir) + "/" + name);
      }
    }
    file = root.openNextFile();
  }
  root.close();

  for (const auto& path : filesToDelete) {
    SD.remove(path);
    Serial.printf("[cache] deleted: %s\n", path.c_str());
  }

  xSemaphoreTake(mutex_, portMAX_DELAY);
  for (auto& cache : caches_) {
    cache.clear();
  }
  xSemaphoreGive(mutex_);

  Serial.printf("[cache] cleared %d files\n", filesToDelete.size());
}

bool AudioCacheManager::isDownloading() const {
  return pendingDownload_.active;
}

String AudioCacheManager::pickRandom(AudioType type) {
  size_t idx = static_cast<size_t>(type);
  if (idx >= static_cast<size_t>(AudioType::Count)) return "";

  xSemaphoreTake(mutex_, portMAX_DELAY);
  if (caches_[idx].empty()) {
    xSemaphoreGive(mutex_);
    return "";
  }

  size_t count = caches_[idx].size();
  uint32_t randIdx = esp_random() % count;
  String path = caches_[idx][randIdx].filePath;
  xSemaphoreGive(mutex_);

  return path;
}

void AudioCacheManager::addToIndex(AudioType type, const String& filePath, uint32_t size) {
  size_t idx = static_cast<size_t>(type);
  if (idx >= static_cast<size_t>(AudioType::Count)) return;

  xSemaphoreTake(mutex_, portMAX_DELAY);
  CacheEntry entry;
  entry.filePath = filePath;
  entry.fileSize = size;
  caches_[idx].push_back(entry);
  xSemaphoreGive(mutex_);
}

void AudioCacheManager::removeOldestIfFull(AudioType type) {
  size_t idx = static_cast<size_t>(type);
  if (idx >= static_cast<size_t>(AudioType::Count)) return;

  xSemaphoreTake(mutex_, portMAX_DELAY);
  if (caches_[idx].size() >= kMaxCachePerType) {
    String oldest = caches_[idx].front().filePath;
    caches_[idx].erase(caches_[idx].begin());
    xSemaphoreGive(mutex_);

    SD.remove(oldest);
    Serial.printf("[cache] removed oldest: %s\n", oldest.c_str());
  } else {
    xSemaphoreGive(mutex_);
  }
}

void AudioCacheManager::startDownload(AudioType type, const String& text) {
  if (pendingDownload_.active) {
    return;
  }

  removeOldestIfFull(type);

  pendingDownload_.type = type;
  pendingDownload_.text = text;
  pendingDownload_.active = true;

  if (downloadTaskHandle_ == nullptr) {
    xTaskCreatePinnedToCore(downloadTaskStub, "cache_dl", 8192, this, 2, &downloadTaskHandle_, 1);
  }
}

void AudioCacheManager::stopAll() {
  stopRequested_ = true;
  pendingDownload_.active = false;
}

void AudioCacheManager::setOnDownloadComplete(OnDownloadComplete cb) {
  onDownloadComplete_ = cb;
}

void AudioCacheManager::downloadTaskStub(void* arg) {
  static_cast<AudioCacheManager*>(arg)->downloadTaskFunc();
}

String AudioCacheManager::callLlmApi(const String& prompt) {
  Serial.printf("[cache] calling LLM with prompt: %s\n", prompt.c_str());

  String url = Config::CHAT_API_URL;
  int hostStart = url.indexOf("://");
  if (hostStart >= 0) hostStart += 3;
  else hostStart = 0;
  int pathStart = url.indexOf('/', hostStart);
  String host = (pathStart > 0) ? url.substring(hostStart, pathStart) : url.substring(hostStart);
  String path = (pathStart > 0) ? url.substring(pathStart) : "/";

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(30);

  if (!client.connect(host.c_str(), 443)) {
    Serial.println("[cache] LLM connect failed");
    return "";
  }

  JsonDocument doc;
  doc["model"] = Config::CHAT_MODEL;
  doc["messages"][0]["role"] = "user";
  doc["messages"][0]["content"] = prompt;
  doc["max_tokens"] = 512;

  String body;
  serializeJson(doc, body);

  String request = "POST " + path + " HTTP/1.1\r\n";
  request += "Host: " + host + "\r\n";
  request += "Authorization: Bearer ";
  request += Config::CHAT_API_KEY;
  request += "\r\n";
  request += "Content-Type: application/json\r\n";
  request += "Content-Length: ";
  request += body.length();
  request += "\r\n\r\n";
  request += body;

  client.print(request);

  String response;
  uint32_t startTime = millis();
  bool gotData = false;
  while (millis() - startTime < 30000 && !stopRequested_) {
    while (client.available()) {
      char c = client.read();
      response += c;
      gotData = true;
      startTime = millis();
      if (response.length() > 8000) break;
    }
    if (!client.connected() && !client.available()) {
      if (gotData) break;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  client.stop();
  Serial.printf("[cache] LLM response len: %d\n", response.length());

  int jsonStart = response.indexOf('{');
  if (jsonStart < 0) {
    Serial.println("[cache] LLM no json in response");
    Serial.printf("[cache] raw response: %.200s\n", response.c_str());
    return "";
  }

  String jsonStr = response.substring(jsonStart);
  JsonDocument respDoc;
  DeserializationError err = deserializeJson(respDoc, jsonStr);
  if (err) {
    Serial.printf("[cache] LLM json parse error: %s\n", err.c_str());
    Serial.printf("[cache] json str: %.200s\n", jsonStr.c_str());
    return "";
  }

  if (respDoc["error"]) {
    const char* errMsg = respDoc["error"]["message"];
    Serial.printf("[cache] LLM API error: %s\n", errMsg ? errMsg : "unknown");
    return "";
  }

  const char* content = respDoc["choices"][0]["message"]["content"];
  if (!content || strlen(content) == 0) {
    Serial.println("[cache] LLM no content");
    JsonArray choices = respDoc["choices"].as<JsonArray>();
    Serial.printf("[cache] choices size: %d\n", choices.size());
    if (choices.size() > 0) {
      JsonObject first = choices[0];
      if (first["message"]) {
        const char* msgContent = first["message"]["content"];
        Serial.printf("[cache] msg content null: %d\n", msgContent == nullptr);
      } else if (first["delta"]) {
        Serial.println("[cache] found delta instead of message (streaming response)");
      }
    }
    return "";
  }

  String result = String(content);
  Serial.printf("[cache] LLM response: %.100s%s\n", result.c_str(), result.length() > 100 ? "..." : "");
  return result;
}

void AudioCacheManager::downloadTaskFunc() {
  while (!stopRequested_) {
    if (!pendingDownload_.active) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    AudioType type = pendingDownload_.type;
    String prompt = pendingDownload_.text;

    Serial.printf("[cache] processing %s request\n", getTypePrefix(type).c_str());

    String llmContent = callLlmApi(prompt);
    if (llmContent.isEmpty() || stopRequested_) {
      Serial.println("[cache] LLM failed or stopped");
      pendingDownload_.active = false;
      if (onDownloadComplete_) onDownloadComplete_(type, false);
      vTaskDelay(pdMS_TO_TICKS(10000));
      continue;
    }

    Serial.printf("[cache] requesting TTS for: %s\n", getTypePrefix(type).c_str());

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(30);

    if (!client.connect("dashscope.aliyuncs.com", 443)) {
      Serial.println("[cache] connect failed");
      pendingDownload_.active = false;
      if (onDownloadComplete_) onDownloadComplete_(type, false);
      continue;
    }

    JsonDocument doc;
    doc["model"] = "qwen3-tts-flash";
    doc["input"]["text"] = llmContent;
    doc["input"]["voice"] = Config::TTS_VOICE;
    doc["input"]["language_type"] = "Chinese";

    String body;
    serializeJson(doc, body);

    String request = "POST /api/v1/services/aigc/multimodal-generation/generation HTTP/1.1\r\n";
    request += "Host: dashscope.aliyuncs.com\r\n";
    request += "Authorization: Bearer ";
    request += Config::TTS_API_KEY;
    request += "\r\n";
    request += "Content-Type: application/json\r\n";
    request += "Content-Length: ";
    request += body.length();
    request += "\r\n\r\n";
    request += body;

    client.print(request);

    String response;
    uint32_t startTime = millis();
    while (millis() - startTime < 30000 && !stopRequested_) {
      if (client.available()) {
        char c = client.read();
        response += c;
        if (response.length() > 100000) break;
      }
      if (!client.connected() && !client.available()) break;
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    client.stop();

    int jsonStart = response.indexOf('{');
    if (jsonStart < 0) {
      Serial.println("[cache] no json in response");
      pendingDownload_.active = false;
      if (onDownloadComplete_) onDownloadComplete_(type, false);
      continue;
    }

    String jsonStr = response.substring(jsonStart);
    JsonDocument respDoc;
    DeserializationError err = deserializeJson(respDoc, jsonStr);
    if (err) {
      Serial.printf("[cache] json parse error: %s\n", err.c_str());
      pendingDownload_.active = false;
      if (onDownloadComplete_) onDownloadComplete_(type, false);
      continue;
    }

    const char* url = respDoc["output"]["audio"]["url"];
    if (!url || strlen(url) == 0) {
      Serial.println("[cache] no audio url");
      pendingDownload_.active = false;
      if (onDownloadComplete_) onDownloadComplete_(type, false);
      continue;
    }

    String audioUrl = String(url);
    String filePath = generateFilePath(type);
    String tmpPath = filePath + ".tmp";

    bool success = downloadToCache(audioUrl, tmpPath);

    if (success && !stopRequested_) {
      SD.rename(tmpPath, filePath);
      File f = SD.open(filePath, FILE_READ);
      uint32_t fsize = f.size();
      f.close();
      addToIndex(type, filePath, fsize);
      Serial.printf("[cache] saved: %s (%u bytes)\n", filePath.c_str(), fsize);
    } else {
      SD.remove(tmpPath);
      Serial.println("[cache] download failed");
    }

    pendingDownload_.active = false;
    if (onDownloadComplete_) onDownloadComplete_(type, success);
  }
}

bool AudioCacheManager::downloadToCache(const String& url, const String& filePath) {
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(60000);

  if (!http.begin(url)) {
    Serial.println("[cache] http begin failed");
    return false;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[cache] http get failed: %d\n", httpCode);
    http.end();
    return false;
  }

  int contentLen = http.getSize();
  Serial.printf("[cache] content length: %d\n", contentLen);

  WiFiClient* stream = http.getStreamPtr();
  if (!stream) {
    http.end();
    return false;
  }

  File file = SD.open(filePath, FILE_WRITE);
  if (!file) {
    Serial.println("[cache] create file failed");
    http.end();
    return false;
  }

  uint32_t totalRead = 0;
  while (http.connected() && !stopRequested_ && (contentLen < 0 || totalRead < (uint32_t)contentLen)) {
    size_t available = stream->available();
    if (available > 0) {
      size_t toRead = (available > kDownloadBufferSize) ? kDownloadBufferSize : available;
      int bytesRead = stream->readBytes(reinterpret_cast<char*>(downloadBuffer_), toRead);
      if (bytesRead > 0) {
        file.write(downloadBuffer_, bytesRead);
        totalRead += bytesRead;
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }

  file.close();
  http.end();

  return totalRead > 0;
}

void AudioCacheManager::loop() {
}
