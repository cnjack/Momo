#include "streaming_tts.h"

StreamingTts::StreamingTts() {
  mutex_ = xSemaphoreCreateMutex();
}

StreamingTts::~StreamingTts() {
  stop();
  if (ttsTaskHandle_) {
    vTaskDelete(ttsTaskHandle_);
    ttsTaskHandle_ = nullptr;
  }
  if (mutex_) {
    vSemaphoreDelete(mutex_);
  }
  if (client_) {
    client_->stop();
    delete client_;
    client_ = nullptr;
  }
  free(downloadBuffer_);
}

void StreamingTts::begin() {
  Serial.printf("[tts] heap before alloc: %d\n", ESP.getFreeHeap());
  downloadBuffer_ = static_cast<uint8_t*>(malloc(kDownloadBufferSize));
  if (!downloadBuffer_) {
    Serial.println("[tts] download buffer alloc failed");
  }
  Serial.printf("[tts] heap after alloc: %d\n", ESP.getFreeHeap());
  
  initSdCard();
}

bool StreamingTts::initSdCard() {
  if (sdCardReady_) {
    return true;
  }
  
  Serial.println("[tts] initializing SD card...");
  
  if (!SD.begin(4, SPI, 40000000)) {
    Serial.println("[tts] SD card mount failed");
    sdCardReady_ = false;
    return false;
  }
  
  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("[tts] no SD card attached");
    sdCardReady_ = false;
    return false;
  }
  
  Serial.printf("[tts] SD card type: %d\n", cardType);
  Serial.printf("[tts] SD card size: %llu MB\n", SD.cardSize() / (1024 * 1024));
  sdCardReady_ = true;
  return true;
}

void StreamingTts::loop() {
}

String StreamingTts::requestTts(const String& text) {
  if (client_) {
    client_->stop();
    delete client_;
    client_ = nullptr;
  }
  
  client_ = new WiFiClientSecure();
  client_->setInsecure();
  client_->setTimeout(30);
  
  if (!client_->connect("dashscope.aliyuncs.com", 443)) {
    Serial.println("[tts] connect failed");
    return "";
  }
  
  JsonDocument doc;
  doc["model"] = "qwen3-tts-flash";
  doc["input"]["text"] = text;
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
  
  client_->print(request);
  Serial.println("[tts] request sent");
  
  String response;
  uint32_t startTime = millis();
  while (millis() - startTime < 30000) {
    if (client_->available()) {
      char c = client_->read();
      response += c;
      
      if (response.length() > 100000) {
        Serial.println("[tts] response too large");
        break;
      }
    }
    
    if (!client_->connected() && !client_->available()) {
      break;
    }
    
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  
  client_->stop();
  delete client_;
  client_ = nullptr;
  
  int jsonStart = response.indexOf('{');
  if (jsonStart < 0) {
    Serial.println("[tts] no json in response");
    return "";
  }
  
  String jsonStr = response.substring(jsonStart);
  Serial.printf("[tts] response json len=%d\n", jsonStr.length());
  
  JsonDocument respDoc;
  DeserializationError err = deserializeJson(respDoc, jsonStr);
  if (err) {
    Serial.printf("[tts] json parse error: %s\n", err.c_str());
    return "";
  }
  
  const char* url = respDoc["output"]["audio"]["url"];
  if (!url || strlen(url) == 0) {
    Serial.println("[tts] no audio url in response");
    return "";
  }
  
  Serial.printf("[tts] got audio url: %.80s...\n", url);
  return String(url);
}

bool StreamingTts::downloadToSdCard(const String& url) {
  if (!sdCardReady_) {
    Serial.println("[tts] SD card not ready");
    return false;
  }
  
  Serial.printf("[tts] heap before download: %d\n", ESP.getFreeHeap());
  
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(60000);
  
  if (!http.begin(url)) {
    Serial.println("[tts] http begin failed");
    return false;
  }
  
  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[tts] http get failed: %d\n", httpCode);
    http.end();
    return false;
  }
  
  int contentLen = http.getSize();
  Serial.printf("[tts] content length: %d\n", contentLen);
  
  WiFiClient* stream = http.getStreamPtr();
  if (!stream) {
    Serial.println("[tts] get stream failed");
    http.end();
    return false;
  }
  
  SD.remove(kSdFilePath);
  File file = SD.open(kSdFilePath, FILE_WRITE);
  if (!file) {
    Serial.println("[tts] create file failed");
    http.end();
    return false;
  }
  
  uint32_t totalRead = 0;
  uint32_t lastProgress = 0;
  
  while (http.connected() && (contentLen < 0 || totalRead < (uint32_t)contentLen)) {
    size_t available = stream->available();
    if (available > 0) {
      size_t toRead = (available > kDownloadBufferSize) ? kDownloadBufferSize : available;
      int bytesRead = stream->readBytes(reinterpret_cast<char*>(downloadBuffer_), toRead);
      
      if (bytesRead > 0) {
        size_t written = file.write(downloadBuffer_, bytesRead);
        if (written != (size_t)bytesRead) {
          Serial.printf("[tts] write error: %d of %d\n", written, bytesRead);
          break;
        }
        totalRead += bytesRead;
        
        if (totalRead - lastProgress > 10240) {
          Serial.printf("[tts] downloaded: %u bytes\n", totalRead);
          lastProgress = totalRead;
        }
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    if (contentLen > 0 && totalRead >= (uint32_t)contentLen) {
      break;
    }
  }
  
  file.close();
  http.end();
  
  audioSize_ = totalRead;
  Serial.printf("[tts] download complete: %u bytes\n", totalRead);
  Serial.printf("[tts] heap after download: %d\n", ESP.getFreeHeap());
  
  return totalRead > 0;
}

void StreamingTts::playFromSdCard() {
  playFromFileInternal(kSdFilePath);
}

void StreamingTts::playFromFileInternal(const String& filePath) {
  if (!sdCardReady_) {
    Serial.println("[tts] SD card not ready");
    speaking_ = false;
    if (onStatus_) onStatus_("SD卡不可用");
    return;
  }

  File file = SD.open(filePath, FILE_READ);
  if (!file) {
    Serial.printf("[tts] open file failed: %s\n", filePath.c_str());
    speaking_ = false;
    if (onStatus_) onStatus_("无法打开音频文件");
    return;
  }

  uint32_t fileSize = file.size();
  file.seek(44);

  Serial.printf("[tts] playing from %s, size=%u\n", filePath.c_str(), fileSize);
  if (onStatus_) onStatus_("播放中");

  int16_t sampleBuffer[256];
  uint8_t dacBuffer[1024];
  uint32_t bytesPlayed = 0;
  uint32_t audioDataSize = fileSize - 44;

  while (!stopRequested_ && bytesPlayed < audioDataSize) {
    uint32_t remaining = audioDataSize - bytesPlayed;
    uint32_t toRead = (remaining < sizeof(sampleBuffer)) ? remaining : sizeof(sampleBuffer);

    int bytesRead = file.read(reinterpret_cast<uint8_t*>(sampleBuffer), toRead);
    if (bytesRead <= 0) {
      Serial.println("[tts] SD read error");
      break;
    }

    bytesPlayed += bytesRead;
    size_t sampleCount = bytesRead / 2;

    for (size_t i = 0; i < sampleCount; ++i) {
      int32_t sample = static_cast<int32_t>(sampleBuffer[i]);
      uint8_t dacValue = static_cast<uint8_t>((sample + 32768) >> 8);
      dacBuffer[i * 4]     = 0x00;
      dacBuffer[i * 4 + 1] = dacValue;
      dacBuffer[i * 4 + 2] = 0x00;
      dacBuffer[i * 4 + 3] = dacValue;
    }

    size_t totalToWrite = sampleCount * 4;
    size_t totalWritten = 0;
    while (totalWritten < totalToWrite && !stopRequested_) {
      size_t written = 0;
      esp_err_t err = i2s_write(I2S_NUM_0, dacBuffer + totalWritten,
                                totalToWrite - totalWritten, &written,
                                pdMS_TO_TICKS(200));
      if (err != ESP_OK) {
        Serial.printf("[tts] i2s err: %d\n", err);
        break;
      }
      totalWritten += written;
    }
  }

  file.close();
  Serial.printf("[tts] playback done, played=%u bytes\n", bytesPlayed);
}

bool StreamingTts::playFromFile(const String& filePath) {
  if (speaking_) {
    stop();
  }

  if (!sdCardReady_) {
    Serial.println("[tts] SD card not ready");
    if (onStatus_) onStatus_("SD卡不可用");
    return false;
  }

  stopRequested_ = false;
  speaking_ = true;

  playFromFileInternal(filePath);

  speaking_ = false;
  if (!stopRequested_ && onStatus_) {
    onStatus_("播放完成");
  }

  return true;
}

bool StreamingTts::speak(const String& text) {
  if (speaking_) {
    stop();
  }
  
  if (!sdCardReady_) {
    Serial.println("[tts] SD card not ready");
    if (onStatus_) onStatus_("SD卡不可用");
    return false;
  }
  
  stopRequested_ = false;
  speaking_ = true;
  pendingText_ = text;
  
  if (ttsTaskHandle_ == nullptr) {
    xTaskCreatePinnedToCore(ttsTaskStub, "tts_task", 8192, this, 3, &ttsTaskHandle_, 1);
  } else {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    xSemaphoreGive(mutex_);
  }
  
  return true;
}

void StreamingTts::ttsTaskStub(void* arg) {
  static_cast<StreamingTts*>(arg)->ttsTask();
}

void StreamingTts::ttsTask() {
  while (true) {
    if (!speaking_ || stopRequested_) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    
    xSemaphoreTake(mutex_, portMAX_DELAY);
    String text = pendingText_;
    pendingText_.clear();
    xSemaphoreGive(mutex_);
    
    if (text.length() == 0) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    
    if (onStatus_) onStatus_("请求中");
    
    String url = requestTts(text);
    if (url.length() == 0 || stopRequested_) {
      Serial.println("[tts] request failed or stopped");
      speaking_ = false;
      if (onStatus_) onStatus_("请求失败");
      continue;
    }
    
    if (onStatus_) onStatus_("下载中");
    
    if (!downloadToSdCard(url) || stopRequested_) {
      Serial.println("[tts] download failed or stopped");
      speaking_ = false;
      if (onStatus_) onStatus_("下载失败");
      continue;
    }
    
    playFromSdCard();
    
    speaking_ = false;
    if (!stopRequested_ && onStatus_) {
      onStatus_("播放完成");
    }
    Serial.println("[tts] task finished");
  }
}

void StreamingTts::stop() {
  stopRequested_ = true;
  speaking_ = false;
  
  if (client_) {
    client_->stop();
  }
  
  i2s_zero_dma_buffer(I2S_NUM_0);
}

bool StreamingTts::isSpeaking() const {
  return speaking_;
}

void StreamingTts::setOnStatus(OnStatusCallback cb) {
  onStatus_ = cb;
}
