# M5Stack Basic AI 小桌宠

这是一个给 `M5Stack Basic` 准备的最小可用桌宠工程：

- 屏幕显示表情
- `A` 键切换心情
- `B` 键让它讲故事
- `C` 键让它出谜语
- 通过 Wi-Fi 直连 `GLM-4.5-Air`
- 直连阿里云 `Qwen-TTS`
- 下载 `WAV` 到本地并通过扬声器播放
- 预留阿里云 `Qwen-ASR` 配置位

## 适合当前硬件的做法

`M5Stack Basic` 很适合先做：

- 表情动画
- 按键互动
- 联网调用 AI
- 文本显示
- 先打通 TTS 生成

需要注意：

- `Basic` 本体更适合先做“会显示、会联网、会回应”
- 如果你想做真正的语音输入，通常需要额外的麦克风模块
- 阿里云录音文件转写 REST 接口要求传 `file_urls`，更适合“先上传文件，再转写”
- 所以这个版本先把“按钮触发 AI 对话 + 生成语音”跑起来，成功率最高

## 文件说明

- `platformio.ini`：PlatformIO 配置
- `include/config.example.h`：配置模板
- `src/main.cpp`：主程序

## 使用步骤

1. 安装 `PlatformIO`
   - 推荐直接使用项目内虚拟环境：
   - `python3 -m venv .venv`
   - `.venv/bin/python -m pip install platformio`
2. 复制 `include/config.example.h` 为 `include/config.h`
3. 填入：
   - Wi-Fi 名称和密码
   - `GLM` API key
   - `DashScope` API key
   - 如需播报，把 `ENABLE_TTS` 改成 `true`
4. 编译并烧录

## 当前直连接口

聊天接口默认使用：

- `https://open.bigmodel.cn/api/coding/paas/v4/chat/completions`
- `model: glm-4.5-air`

代码按常见 `chat/completions` 请求体发送：

```json
{
  "model": "your-model",
  "messages": [
    {"role": "system", "content": "system prompt"},
    {"role": "user", "content": "user prompt"}
  ],
  "temperature": 0.8
}
```

并默认从下面这个字段读取回复：

```json
choices[0].message.content
```

当前默认从 `choices[0].message.content` 读取回复，代码位置在 `parseChatReply()`。

TTS 默认使用：

- `https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation`
- `model: qwen-tts-latest`

请求体大致是：

```json
{
  "model": "qwen-tts-latest",
  "input": {
    "text": "你好呀",
    "voice": "Cherry",
    "language_type": "Chinese"
  }
}
```

当前代码会直接请求 TTS，并尝试读取 `output.audio.url`。这一步已经接好了“生成语音”的 API，但还没有把这个 URL 下载并播放到 M5Stack 扬声器上。
当前工程已经会把音频 URL 下载到 `SPIFFS` 的 `/tts.wav`，再通过 ESP32 内置 DAC 直接播放到 `M5Stack Basic` 的喇叭。

TTS 参数额外固定了：

```json
{
  "parameters": {
    "sample_rate": 24000,
    "response_format": "wav"
  }
}
```

这样更适合在设备端直接播放。

## 实时流式 TTS

阿里云的实时 TTS 文档显示它使用 `WebSocket` 连接，典型模型是 `qwen3-tts-flash-realtime`，服务端会持续返回音频事件，延迟理论上会比“先生成完整音频 URL 再下载播放”更低。

但对 `M5Stack Basic` 来说，实时版要额外处理：

- `wss://` 连接
- 实时事件协议
- Base64/音频分片解析
- 边收边播的缓冲队列

所以当前工程先落了“非流式但稳定”的版本，更容易先跑通。等这个版本在你手上确认没问题后，再切到实时版会更稳。

## ASR 怎么接

如果后面你给 `M5Stack Basic` 接了麦克风模块，可以继续扩展，但这里有个现实限制：

- 阿里云官方录音文件转写 REST 接口是异步任务
- 提交任务时要传 `input.file_urls`
- 也就是你要先把录音上传到公网 URL 或 OSS，再调用转写

所以更稳的做法是：

1. `M5Stack` 录音
2. 上传到你自己的服务或 OSS
3. 服务端调用 ASR
4. 返回文字给桌宠

这个工程里已经预留了配置项，但没有默认启用这条链路，因为它和你的麦克风硬件、上传方式强相关。

## 下一步建议

最适合下一步继续加的功能有：

1. 加待机呼吸动画
2. 给不同心情配不同表情
3. 增加“夸夸我”“唱一句”“晚安模式”
4. 把 TTS 音频 URL 下载下来并接到扬声器播放
5. 确认你手上的麦克风模块后，再补语音输入

## 小朋友一起玩的玩法

- 轮流按按钮，让桌宠讲故事接龙
- 让孩子先选心情，再看看桌宠会说什么
- 用谜语模式做亲子问答
- 把桌宠当“睡前陪聊机器人”
