#pragma once

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <driver/i2s.h>
#include <ArduinoJson.h>
#include <freertos/semphr.h>
#include <FS.h>
#include <SD.h>
#include <HTTPClient.h>
#include "config.h"

class StreamingTts {
public:
  using OnStatusCallback = std::function<void(const String&)>;

  StreamingTts();
  ~StreamingTts();

  void begin();
  void loop();
  bool speak(const String& text);
  bool playFromFile(const String& filePath);
  void stop();
  bool isSpeaking() const;
  void setOnStatus(OnStatusCallback cb);

private:
  static constexpr const char* kSdFilePath = "/tts_audio.wav";
  static constexpr size_t kDownloadBufferSize = 4096;

  bool initSdCard();
  String requestTts(const String& text);
  bool downloadToSdCard(const String& url);
  void playFromSdCard();
  void playFromFileInternal(const String& filePath);
  void ttsTask();
  static void ttsTaskStub(void* arg);

  WiFiClientSecure* client_ = nullptr;
  volatile bool speaking_ = false;
  volatile bool stopRequested_ = false;
  String pendingText_;
  OnStatusCallback onStatus_;

  uint8_t* downloadBuffer_ = nullptr;
  volatile uint32_t audioSize_ = 0;
  volatile bool sdCardReady_ = false;

  SemaphoreHandle_t mutex_ = nullptr;
  TaskHandle_t ttsTaskHandle_ = nullptr;
};
