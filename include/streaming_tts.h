#pragma once

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <driver/i2s.h>
#include <ArduinoJson.h>
#include <freertos/semphr.h>
#include "config.h"

class StreamingTts {
public:
  using OnStatusCallback = std::function<void(const String&)>;

  StreamingTts();
  ~StreamingTts();

  void begin();
  void loop();
  bool speak(const String& text);
  void stop();
  bool isSpeaking() const;
  void setOnStatus(OnStatusCallback cb);

private:
  static constexpr size_t kRingBufferSize = 49152;
  static constexpr size_t kDecodeBufferSize = 16384;
  static constexpr size_t kPreBufferThreshold = 30720;
  static constexpr size_t kSseLineBufSize = 24576;

  void processSseLine(const char* line, size_t len);
  void writeToRingBuffer(const uint8_t* data, size_t len);
  size_t readFromRingBuffer(uint8_t* data, size_t len);
  void playTask();
  static void playTaskStub(void* arg);
  void httpTask();
  static void httpTaskStub(void* arg);
  void startHttpRequest(const String& text);
  void processHttpResponse();

  WiFiClientSecure* client_ = nullptr;
  volatile bool speaking_ = false;
  volatile bool stopRequested_ = false;
  volatile bool httpResponseComplete_ = false;
  volatile bool preBuffering_ = true;
  volatile bool headersDone_ = false;
  String pendingText_;
  String sseBuffer_;
  OnStatusCallback onStatus_;

  uint8_t* ringBuffer_ = nullptr;
  uint8_t* decodeBuffer_ = nullptr;
  char* sseLineBuf_ = nullptr;
  size_t sseLinePos_ = 0;
  volatile size_t ringHead_ = 0;
  volatile size_t ringTail_ = 0;
  volatile size_t ringCount_ = 0;

  SemaphoreHandle_t mutex_ = nullptr;
  TaskHandle_t playTaskHandle_ = nullptr;
  TaskHandle_t httpTaskHandle_ = nullptr;
};
