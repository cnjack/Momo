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
  static constexpr size_t kDecodeBufferSize = 24576;
  static constexpr size_t kPreBufferThreshold = 12288;

  void processSseLine(const String& line);
  void writeToRingBuffer(const uint8_t* data, size_t len);
  size_t readFromRingBuffer(uint8_t* data, size_t len);
  void playTask();
  static void playTaskStub(void* arg);
  void startHttpRequest(const String& text);
  void processHttpResponse();

  WiFiClientSecure* client_ = nullptr;
  volatile bool speaking_ = false;
  volatile bool stopRequested_ = false;
  volatile bool httpResponseComplete_ = false;
  volatile bool preBuffering_ = true;
  String pendingText_;
  String sseBuffer_;
  OnStatusCallback onStatus_;

  uint8_t ringBuffer_[kRingBufferSize];
  uint8_t decodeBuffer_[kDecodeBufferSize];
  volatile size_t ringHead_ = 0;
  volatile size_t ringTail_ = 0;
  volatile size_t ringCount_ = 0;

  SemaphoreHandle_t mutex_ = nullptr;
  TaskHandle_t playTaskHandle_ = nullptr;
};
