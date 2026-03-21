#pragma once

#include <Arduino.h>
#include <vector>
#include <freertos/semphr.h>
#include <SD.h>

enum class AudioType : uint8_t {
  Story = 0,
  Riddle,
  Count
};

struct CacheEntry {
  String filePath;
  uint32_t fileSize;
};

class AudioCacheManager {
public:
  static constexpr size_t kMinCachePerType = 3;
  static constexpr size_t kMaxCachePerType = 8;
  static constexpr const char* kCacheDir = "/tts_cache";

  AudioCacheManager();
  ~AudioCacheManager();

  bool begin();
  void loop();
  void clearCache();

  size_t getCacheCount(AudioType type) const;
  String pickRandom(AudioType type);
  bool needsRefill(AudioType type) const;
  bool isDownloading() const;

  void startDownload(AudioType type, const String& text);
  void stopAll();

  using OnDownloadComplete = std::function<void(AudioType, bool success)>;
  void setOnDownloadComplete(OnDownloadComplete cb);

private:
  struct DownloadTask {
    AudioType type;
    String text;
    bool active;
  };

  bool initCacheDir();
  void scanCacheDir();
  String getTypePrefix(AudioType type) const;
  String generateFilePath(AudioType type);
  bool downloadToCache(const String& url, const String& filePath);
  void addToIndex(AudioType type, const String& filePath, uint32_t size);
  void removeOldestIfFull(AudioType type);
  String callLlmApi(const String& prompt);
  void downloadTaskFunc();

  static void downloadTaskStub(void* arg);

  std::vector<CacheEntry> caches_[static_cast<size_t>(AudioType::Count)];
  mutable SemaphoreHandle_t mutex_;
  TaskHandle_t downloadTaskHandle_;
  DownloadTask pendingDownload_;
  volatile bool stopRequested_;
  OnDownloadComplete onDownloadComplete_;

  uint8_t* downloadBuffer_;
  static constexpr size_t kDownloadBufferSize = 4096;
};
