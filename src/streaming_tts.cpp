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
  memset(ringBuffer_, 0, sizeof(ringBuffer_));
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
  gInstance = nullptr;
}

void StreamingTts::begin() {
}

void StreamingTts::loop() {
  if (client_ && client_->connected()) {
    processHttpResponse();
  }
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
}

void StreamingTts::processHttpResponse() {
  while (client_ && client_->available()) {
    String line = client_->readStringUntil('\n');
    
    if (stopRequested_) {
      break;
    }
    
    if (line.startsWith("data:")) {
      String jsonStr = line.substring(5);
      jsonStr.trim();
      if (jsonStr.length() > 0) {
        processSseLine(jsonStr);
      }
    } else if (line.length() == 1 && line[0] == '\r') {
      continue;
    } else if (line.startsWith("{") || line.startsWith("[")) {
      processSseLine(line);
    }
  }
  
  if (client_ && !client_->connected()) {
    httpResponseComplete_ = true;
    Serial.println("[tts] response complete");
  }
}

void StreamingTts::processSseLine(const String& json) {
  if (json.indexOf("\"finish_reason\":\"stop\"") >= 0) {
    Serial.println("[tts] finish: stop");
    return;
  }
  
  int dataPos = json.indexOf("\"data\":\"");
  if (dataPos < 0) {
    return;
  }
  
  dataPos += 8;
  int endPos = json.indexOf("\"", dataPos);
  if (endPos <= dataPos) {
    return;
  }
  
  int dataLen = endPos - dataPos;
  if (dataLen < 10) {
    return;
  }
  
  const char* audioBase64 = json.c_str() + dataPos;
  size_t decodedLen = kDecodeBufferSize;
  
  if (base64Decode(audioBase64, dataLen, decodeBuffer_, &decodedLen)) {
    writeToRingBuffer(decodeBuffer_, decodedLen);
    Serial.printf("[tts] decoded %d bytes\n", decodedLen);
  } else {
    Serial.println("[tts] base64 decode error");
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
    
    xSemaphoreGive(mutex_);
  }
}

size_t StreamingTts::readFromRingBuffer(uint8_t* data, size_t len) {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  
  size_t readCount = 0;
  while (readCount < len && ringCount_ > 0) {
    data[readCount] = ringBuffer_[ringTail_];
    ringTail_ = (ringTail_ + 1) % kRingBufferSize;
    --ringCount_;
    ++readCount;
  }
  
  xSemaphoreGive(mutex_);
  return readCount;
}

bool StreamingTts::speak(const String& text) {
  if (speaking_) {
    stop();
    delay(100);
  }
  
  stopRequested_ = false;
  speaking_ = true;
  httpResponseComplete_ = false;
  preBuffering_ = true;
  ringHead_ = ringTail_ = ringCount_ = 0;
  
  if (playTaskHandle_ == nullptr) {
    xTaskCreate(playTaskStub, "tts_play", 8192, this, 5, &playTaskHandle_);
  }
  
  startHttpRequest(text);
  return true;
}

void StreamingTts::stop() {
  stopRequested_ = true;
  speaking_ = false;
  httpResponseComplete_ = true;
  preBuffering_ = true;
  
  xSemaphoreTake(mutex_, portMAX_DELAY);
  ringHead_ = ringTail_ = ringCount_ = 0;
  xSemaphoreGive(mutex_);
  
  i2s_zero_dma_buffer(I2S_NUM_0);
  
  if (client_) {
    client_->stop();
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
  int16_t sampleBuffer[512];
  uint8_t dacBuffer[1024];
  uint8_t emptyCount = 0;
  static constexpr uint8_t kEmptyThreshold = 15;
  
  while (true) {
    if (stopRequested_) {
      emptyCount = 0;
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    
    if (preBuffering_) {
      if (ringCount_ < kPreBufferThreshold && !httpResponseComplete_) {
        vTaskDelay(pdMS_TO_TICKS(20));
        continue;
      }
      preBuffering_ = false;
      Serial.printf("[tts] prebuffer done: %d bytes\n", ringCount_);
    }
    
    size_t available = readFromRingBuffer(reinterpret_cast<uint8_t*>(sampleBuffer), sizeof(sampleBuffer));
    
    if (available > 0) {
      emptyCount = 0;
      size_t sampleCount = available / 2;
      
      for (size_t i = 0; i < sampleCount; ++i) {
        int32_t sample = static_cast<int32_t>(sampleBuffer[i]);
        uint8_t dacValue = static_cast<uint8_t>((sample + 32768) >> 8);
        dacBuffer[i * 2] = 0x00;
        dacBuffer[i * 2 + 1] = dacValue;
      }
      
      size_t written = 0;
      esp_err_t err = i2s_write(I2S_NUM_0, dacBuffer, sampleCount * 2, &written, pdMS_TO_TICKS(100));
      if (err != ESP_OK || written == 0) {
        Serial.printf("[tts] i2s write error: %d\n", err);
      }
    } else {
      if (httpResponseComplete_ && !stopRequested_) {
        emptyCount++;
        if (emptyCount >= kEmptyThreshold) {
          i2s_zero_dma_buffer(I2S_NUM_0);
          speaking_ = false;
          if (onStatus_) onStatus_("播放完成");
          Serial.println("[tts] playback finished");
          emptyCount = 0;
        }
      }
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
}
