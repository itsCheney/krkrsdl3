# krkrsdl3（Mikage 分支）

简体中文 · [English](README.en.md)

这是 [krkrsdl3](https://github.com/krkrsdl3/krkrsdl3) 核心源码的分支，作为 iOS 视觉小说播放器 [Mikage Next](https://github.com/itsCheney/Mikage-Next) 的 KiriKiri 运行时使用。krkrsdl3 是基于 SDL3 重写的跨平台吉里吉里（KiriKiri / KAG / TJS2）引擎。

本仓库只包含引擎源码，不能单独构建。构建入口是 [krkrsdl3_build](https://github.com/itsCheney/krkrsdl3_build)，它把本仓库作为 `cpp/` 子模块引入。

## 分支

| 分支 | 用途 |
| --- | --- |
| `mikage` | Mikage `main` 使用的稳定版运行时 |
| `mikage-beta` | Mikage `beta` 使用的测试版运行时 |
| `main` | 跟踪上游 krkrsdl3 |

合并方向固定为 `mikage-beta` → `mikage`。

## 相对上游的主要改动

**嵌入式运行**
- 引擎可以作为 iOS framework 嵌入宿主 App 运行，而不是独立的 SDL 程序。
- 同一进程内可以反复启动、退出、切换游戏：图形、事件、媒体、脚本缓存等状态在会话之间完全重置。
- 游戏调用 `System.exit` 或脚本出错时只结束当前游戏，不会退出宿主 App。
- 退出时保留游戏自己的退出确认与存档回调。

**Metal 渲染**
- 原生 Metal 渲染后端，负责画面呈现。
- 普通 Layer 合成可以在 GPU 上完成，包括常见混合模式、遮罩、模糊、缩小复制、仿射/透视变换、span 合成和内置 extrans 转场；不支持的操作自动回退到软件路径。
- 只上传图层中变化的区域；整层被覆盖时跳过 GPU 回读；回读改为异步。
- 画面没有变化时跳过呈现，降低空闲时的 CPU 与发热。

**Emote（E-mote / PSB）**
- 新的 Emote 动画实现（实验性，默认关闭，仍使用原有实现），参考 AetherKrkr 的时间轴行为。
- 共享资源缓存按设备内存调整大小，低内存时自动清理；PSB 解码减少读取次数；可直接渲染到图层纹理。

**媒体与其他**
- 宿主可以指定要跳过的视频（例如汉化组片头），被跳过的视频立即报告播放结束。
- 音频解码线程池改为等待而不是轮询；空闲时系统变量最多每分钟保存一次。
- 帧时间、图层工作量、CPU 读取来源等诊断数据，供宿主的性能面板和诊断日志使用。

## 目录结构

| 路径 | 内容 |
| --- | --- |
| `core/archive/` | XP3 等数据包格式 |
| `core/main/` | 引擎内核：窗口、事件循环、线程、会话生命周期 |
| `core/media/` | 字体、图片、视频、音频解码 |
| `core/render/` | 图层系统与渲染；`backend/` 下是 Metal、OpenGL、Vulkan 和软件后端 |
| `core/script/` | TJS2 原生绑定 |
| `environ/` | 不同系统与芯片架构的差异代码 |
| `plugins/` | 扩展插件，包括 `emoteplayer/`、`extrans/` 等 |
| `tjs2/` | TJS2 脚本语言内核 |

## 依赖

由 krkrsdl3_build 通过 vcpkg 提供：SDL3、FFmpeg、zlib、oniguruma、libpng、libwebp、libjpeg-turbo、libvorbis、opusfile、FreeType、plutosvg / plutovg。

## 许可证

- 引擎源码沿用 KiriKiri / 吉里吉里Z 的许可证及 krkrsdl3 补充条款，见 [LICENSE](LICENSE)。补充条款要求：分发用本软件运行的、并非基于本软件开发的商业游戏移植版二进制时，如有修改，修改部分必须全部开源。
- `plugins/emoteplayer/` 中新的 Emote 动画实现以 GPL-3.0-or-later 发布，见 [AETHER-NOTICE.md](plugins/emoteplayer/AETHER-NOTICE.md) 与 [LICENSE-AETHER-GPL-3.0.txt](plugins/emoteplayer/LICENSE-AETHER-GPL-3.0.txt)。关闭该实现不会把这部分代码从二进制中移除。
- `core/render/backend/` 中的 span 混合着色器部分源自 plutovg（MIT），见 [MetalSpanComposite-LICENSE.txt](core/render/backend/MetalSpanComposite-LICENSE.txt)。

感谢 KiriKiri、吉里吉里Z 与 krkrsdl3 的所有贡献者。
