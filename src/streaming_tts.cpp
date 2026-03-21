#include "streaming_tts.h"
#include <mbedtls/base64.h>
#include <freertos/semphr.h>

namespace {

StreamingTts* gInstance = nullptr;

bool base64Decode(const char* input, size_t inputLen, uint8_t* output, size_t* outputLen) {
  size_t requiredLen = 0;
  int ret = mbedtls_base64_decode(nullptr, 0, &requiredLen, 
                                   reinterpret_cast<const unsigned char*>(input), inputLen);
  
  if (ret != 0 && ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
    return false;
  }
  
  ret = mbedtls_base64_decode(output, *outputLen, outputLen,
                              reinterpret_cast<const unsigned char*>(input), inputLen);
  return ret == 0;
}

}

StreamingTts::StreamingTts() {
  gInstance = this;
  mutex_ = xSemaphoreCreateMutex();
}

StreamingTts::~StreamingTts() {
  stop();
  if (playTaskHandle_) {
    vTaskDelete(playTaskHandle_);
    playTaskHandle_ = nullptr;
  }
  if (mutex_) {
    vSemaphoreDelete(mutex_);
  }
  if (client_) {
    client_->stop();
    delete client_;
    client_ = nullptr;
  }
  free(ringBuffer_);
  free(decodeBuffer_);
  free(sseLineBuf_);
  gInstance = nullptr;
}

void StreamingTts::begin() {
  Serial.printf("[tts] heap before alloc: %d\n", ESP.getFreeHeap());
  ringBuffer_ = static_cast<uint8_t*>(malloc(kRingBufferSize));
  decodeBuffer_ = static_cast<uint8_t*>(malloc(kDecodeBufferSize));
  sseLineBuf_ = static_cast<char*>(malloc(kSseLineBufSize));
  if (!ringBuffer_ || !decodeBuffer_ || !sseLineBuf_) {
    Serial.printf("[tts] ALLOC FAIL ring=%p decode=%p sse=%p\n",
                  ringBuffer_, decodeBuffer_, sseLineBuf_);
  }
  Serial.printf("[tts] heap after alloc: %d\n", ESP.getFreeHeap());
  memset(ringBuffer_, 0, kRingBufferSize);
}

void StreamingTts::loop() {
  // HTTP processing moved to dedicated FreeRTOS task
}

void StreamingTts::startHttpRequest(const String& text) {
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
    speaking_ = false;
    if (onStatus_) onStatus_("连接失败");
    return;
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
  request += "X-DashScope-SSE: enable\r\n";
  request += "Content-Length: ";
  request += body.length();
  request += "\r\n\r\n";
  request += body;
  
  client_->print(request);
  Serial.println("[tts] request sent");
  
  sseBuffer_.clear();
  sseLinePos_ = 0;
  headersDone_ = false;
}

void StreamingTts::processHttpResponse() {
  uint8_t readBuf[256];
  
  while (client_ && client_->available() && !stopRequested_) {
    int avail = client_->available();
    if (avail > (int)sizeof(readBuf)) avail = sizeof(readBuf);
    int bytesRead = client_->read(readBuf, avail);
    if (bytesRead <= 0) break;
    
    for (int i = 0; i < bytesRead; ++i) {
      char c = (char)readBuf[i];
      
      // Phase 1: Skip HTTP response headers
      // sseLinePos_ counts non-CR content chars on current line
      if (!headersDone_) {
        if (c == '\n') {
          if (sseLinePos_ == 0) {
            headersDone_ = true;
            Serial.println("[tts] headers done");
          }
          sseLinePos_ = 0;
        } else if (c != '\r') {
          sseLinePos_++;
        }
        continue;
      }
      
      // Phase 2: Build SSE lines in sseLineBuf_
      if (c == '\n') {
        if (sseLinePos_ > 0 && sseLineBuf_[sseLinePos_ - 1] == '\r') {
          sseLinePos_--;
        }
        
        if (sseLinePos_ == 0) continue;
        
        sseLineBuf_[sseLinePos_] = '\0';
        
        // Skip chunked transfer encoding size lines (hex digits only, ≤8 chars)
        bool isChunkSize = (sseLinePos_ <= 8);
        if (isChunkSize) {
          for (size_t j = 0; j < sseLinePos_; ++j) {
            char ch = sseLineBuf_[j];
            if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) {
              isChunkSize = false;
              break;
            }
          }
        }
        
        if (!isChunkSize) {
          const char* dataStart = nullptr;
          size_t dataLen = 0;
          if (sseLinePos_ > 5 && strncmp(sseLineBuf_, "data:", 5) == 0) {
            dataStart = sseLineBuf_ + 5;
            dataLen = sseLinePos_ - 5;
            while (dataLen > 0 && *dataStart == ' ') { dataStart++; dataLen--; }
          } else if (sseLineBuf_[0] == '{') {
            dataStart = sseLineBuf_;
            dataLen = sseLinePos_;
          }
          
          if (dataStart && dataLen > 0) {
            processSseLine(dataStart, dataLen);
          }
        }
        
        sseLinePos_ = 0;
      } else {
        if (sseLinePos_ < kSseLineBufSize - 1) {
          sseLineBuf_[sseLinePos_++] = c;
        }
      }
    }
  }
}

void StreamingTts::processSseLine(const char* json, size_t jsonLen) {
  Serial.printf("[tts] sse line len=%d\n", jsonLen);
  
  // Check finish
  if (memmem(json, jsonLen, "\"finish_reason\":\"stop\"", 21) != nullptr) {
    Serial.println("[tts] finish: stop");
    httpResponseComplete_ = true;
    return;
  }
  
  // Find "data":" in the JSON
  const char* needle = "\"data\":\"";
  const size_t needleLen = 8;
  const char* found = (const char*)memmem(json, jsonLen, needle, needleLen);
  if (!found) return;
  
  const char* audioStart = found + needleLen;
  size_t remaining = jsonLen - (audioStart - json);
  
  // Find closing quote
  const char* audioEnd = (const char*)memchr(audioStart, '"', remaining);
  if (!audioEnd || audioEnd <= audioStart) return;
  
  size_t audioLen = audioEnd - audioStart;
  if (audioLen < 10) return;
  
  size_t decodedLen = kDecodeBufferSize;
  if (base64Decode(audioStart, audioLen, decodeBuffer_, &decodedLen)) {
    writeToRingBuffer(decodeBuffer_, decodedLen);
    Serial.printf("[tts] decoded %d bytes\n", decodedLen);
  } else {
    Serial.printf("[tts] base64 err len=%d\n", audioLen);
  }
}

void StreamingTts::writeToRingBuffer(const uint8_t* data, size_t len) {
  size_t written = 0;
  while (written < len && !stopRequested_) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    
    size_t available = kRingBufferSize - ringCount_;
    
    if (available == 0) {
      xSemaphoreGive(mutex_);
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    
    size_t toWrite = (len - written) < available ? (len - written) : available;
    
    for (size_t i = 0; i < toWrite; ++i) {
      ringBuffer_[ringHead_] = data[written];
      ringHead_ = (ringHead_ + 1) % kRingBufferSize;
      ++ringCount_;
      ++written;
    }
    
    Serial.printf("[tts] write: toWrite=%d, ringCount_=%d, written=%d/%d\n", toWrite, ringCount_, written, len);
    xSemaphoreGive(mutex_);
  }
}

size_t StreamingTts::readFromRingBuffer(uint8_t* data, size_t len) {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  size_t countBefore = ringCount_;
  
  size_t readCount = 0;
  while (readCount < len && ringCount_ > 0) {
    data[readCount] = ringBuffer_[ringTail_];
    ringTail_ = (ringTail_ + 1) % kRingBufferSize;
    --ringCount_;
    ++readCount;
  }
  
  xSemaphoreGive(mutex_);
  if (countBefore > 0 || readCount > 0) {
    Serial.printf("[tts] read: countBefore=%d, readCount=%d\n", countBefore, readCount);
  }
  return readCount;
}

bool StreamingTts::speak(const String& text) {
  if (speaking_) {
    stop();
  }
  
  if (client_) {
    client_->stop();
    delete client_;
    client_ = nullptr;
  }
  
  stopRequested_ = false;
  speaking_ = true;
  httpResponseComplete_ = false;
  preBuffering_ = true;
  headersDone_ = false;
  sseLinePos_ = 0;
  ringHead_ = ringTail_ = ringCount_ = 0;
  pendingText_ = text;
  
  if (playTaskHandle_ == nullptr) {
    xTaskCreatePinnedToCore(playTaskStub, "tts_play", 4096, this, 3, &playTaskHandle_, 1);
  }
  
  xTaskCreatePinnedToCore(httpTaskStub, "tts_http", 8192, this, 6, &httpTaskHandle_, 0);
  return true;
}

void StreamingTts::httpTaskStub(void* arg) {
  static_cast<StreamingTts*>(arg)->httpTask();
}

void StreamingTts::httpTask() {
  startHttpRequest(pendingText_);
  pendingText_.clear();
  
  while (!stopRequested_ && !httpResponseComplete_) {
    if (!client_ || !client_->connected()) {
      Serial.println("[tts] http disconnected");
      if (client_ && client_->available()) {
        processHttpResponse();
      }
      break;
    }
    if (client_->available()) {
      processHttpResponse();
    } else {
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
  
  httpResponseComplete_ = true;
  Serial.println("[tts] http task done");
  httpTaskHandle_ = nullptr;
  vTaskDelete(nullptr);
}

void StreamingTts::stop() {
  stopRequested_ = true;
  speaking_ = false;
  httpResponseComplete_ = true;
  preBuffering_ = true;
  
  if (client_) {
    client_->stop();
  }
  
  uint8_t waitCount = 0;
  while (httpTaskHandle_ != nullptr && waitCount++ < 50) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (httpTaskHandle_ != nullptr) {
    vTaskDelete(httpTaskHandle_);
    httpTaskHandle_ = nullptr;
  }
  
  xSemaphoreTake(mutex_, portMAX_DELAY);
  ringHead_ = ringTail_ = ringCount_ = 0;
  xSemaphoreGive(mutex_);
  
  i2s_zero_dma_buffer(I2S_NUM_0);
  
  if (client_) {
    delete client_;
    client_ = nullptr;
  }
}

bool StreamingTts::isSpeaking() const {
  return speaking_ || ringCount_ > 0;
}

void StreamingTts::setOnStatus(OnStatusCallback cb) {
  onStatus_ = cb;
}

void StreamingTts::playTaskStub(void* arg) {
  static_cast<StreamingTts*>(arg)->playTask();
}

void StreamingTts::playTask() {
  int16_t sampleBuffer[256];
  uint8_t dacBuffer[1024];
  uint8_t emptyCount = 0;
  static constexpr uint8_t kEmptyThreshold = 25;
  
  while (true) {
    if (!speaking_ || stopRequested_) {
      emptyCount = 0;
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    
    if (preBuffering_) {
      xSemaphoreTake(mutex_, portMAX_DELAY);
      size_t currentCount = ringCount_;
      bool httpDone = httpResponseComplete_;
      xSemaphoreGive(mutex_);
      
      if (currentCount >= kPreBufferThreshold || (httpDone && currentCount > 0)) {
        preBuffering_ = false;
        Serial.printf("[tts] prebuffer done: %d bytes, httpDone=%d\n", currentCount, httpDone);
      } else {
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
    }
    
    size_t available = readFromRingBuffer(reinterpret_cast<uint8_t*>(sampleBuffer), sizeof(sampleBuffer));
    
    if (available > 0) {
      emptyCount = 0;
      size_t sampleCount = available / 2;
      
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
    } else {
      xSemaphoreTake(mutex_, portMAX_DELAY);
      bool httpDone = httpResponseComplete_;
      size_t currentCount = ringCount_;
      xSemaphoreGive(mutex_);
      
      if (httpDone) {
        emptyCount++;
        if (emptyCount >= kEmptyThreshold) {
          speaking_ = false;
          if (onStatus_) onStatus_("播放完成");
          Serial.println("[tts] playback finished");
          emptyCount = 0;
          preBuffering_ = true;
        }
      } else {
        Serial.printf("[tts] underrun, ring=%d, waiting...\n", currentCount);
        vTaskDelay(pdMS_TO_TICKS(5));
      }
    }
  }
}
