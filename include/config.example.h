#pragma once

// Rename this file to config.h and fill in your real values.

namespace Config {

// Optional fallback Wi-Fi. WiFiManager portal is the preferred setup flow.
static const char* WIFI_SSID = "";
static const char* WIFI_PASSWORD = "";
static const char* WIFI_PORTAL_AP_NAME = "Momo-Setup";
static const char* WIFI_PORTAL_PASSWORD = "momo1234";
static const uint16_t WIFI_PORTAL_TIMEOUT_SECONDS = 180;

// Zhipu GLM chat.
// This path is inferred from the base URL you provided and the common
// chat-completions pattern.
static const char* CHAT_API_URL =
    "https://open.bigmodel.cn/api/coding/paas/v4/chat/completions";
static const char* CHAT_API_KEY = "PASTE_YOUR_GLM_API_KEY";
static const char* CHAT_MODEL = "glm-4.5-air";

// DashScope Qwen TTS.
static const char* TTS_API_URL =
    "https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation";
static const char* TTS_API_KEY = "PASTE_YOUR_DASHSCOPE_API_KEY";
static const char* TTS_MODEL = "qwen-tts-latest";
static const char* TTS_VOICE = "Cherry";
static const char* TTS_LANGUAGE_TYPE = "Chinese";
static const uint32_t TTS_SAMPLE_RATE = 24000;
static const char* TTS_RESPONSE_FORMAT = "wav";

// DashScope ASR.
// qwen3-asr-flash supports OpenAI-compatible and DashScope synchronous calls.
// For a toy project, use base64/data-url audio directly instead of filetrans.
static const char* ASR_API_URL =
    "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions";
static const char* ASR_API_KEY = "PASTE_YOUR_DASHSCOPE_API_KEY";
static const char* ASR_MODEL = "qwen3-asr-flash";

static const char* PET_NAME = "Momo";
static const bool ENABLE_TTS = false;
static const float SPEAKER_GAIN = 0.45f;

}  // namespace Config
