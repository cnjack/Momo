# M5Stack AI 小桌宠

一个给 `M5Stack Basic` 做的亲子 AI 小桌宠：

- 表情屏幕
- `A` 键切换心情
- `B` 键讲故事
- `C` 键出谜语
- 支持 Wi‑Fi 配网热点
- 聊天模型：`GLM-4.5-Air`
- 语音合成：`Qwen-TTS`
- 语音识别预留：`Qwen3-ASR-Flash`

## 使用

1. 编辑 [config.h](/Users/jack/workpath/jack/m5stack/include/config.h)
2. 编译：

```bash
.venv/bin/pio run
```

3. 烧录：

```bash
.venv/bin/pio run -t upload --upload-port /dev/cu.usbserial-017CB466
```

## 配网

如果设备连不上已保存的 Wi‑Fi，会自动开启热点：

- 热点名：`Momo-Setup`
- 密码：`momo1234`
- 地址：`192.168.4.1`

手机连上后打开浏览器，选择 Wi‑Fi 并输入密码即可。

## 当前状态

- 已支持屏幕表情和按钮互动
- 已支持联网聊天
- 已支持 TTS 音频下载并播放
- ASR 还没接进主流程，但模型已选为 `qwen3-asr-flash`
