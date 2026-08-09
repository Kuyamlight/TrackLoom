# TrackLoom｜织音

TrackLoom 是一款开源、Windows 优先、AI 原生的音乐创作工作站。项目采用 C++20、JUCE 与 CMake 构建；AI 服务使用 Python，并由用户自行配置的第三方 API 提供。

## 当前状态

项目目前处于基础核心建设阶段，已实现并测试工程模型、命令历史、基础音频/MIDI 播放调度与 JUCE 平台适配基础。录音、循环编曲器、插件宿主、AI 生成与完整 DAW 工作流仍在开发中，不应视为已可用功能。

## Windows 构建

前置条件：

- Windows 10 或更高版本
- Visual Studio 2022（C++ 桌面开发工具）或等价的 MSVC C++20 工具链
- CMake 3.22 或更高版本
- Ninja
- JUCE 8.0.14 源码树

在 Visual Studio Developer Command Prompt 中执行：

```powershell
cmake -S . -B build -G Ninja -DTRACKLOOM_JUCE_ROOT="C:\\path\\to\\JUCE"
cmake --build build
ctest --test-dir build --output-on-failure
```

也可通过 `JUCE_ROOT` 环境变量指定 JUCE 源码树。若仅构建不依赖 JUCE 的核心组件，请在配置时加入 `-DTRACKLOOM_ENABLE_JUCE_ADAPTERS=OFF`。

## 许可证与源码

TrackLoom 采用 **AGPL-3.0-only** 许可证；完整条款见 [LICENSE](LICENSE)，具体义务以许可证正文为准。以下说明仅用于项目发布准备，不构成法律意见。

每个发行物都应关联精确的 TrackLoom commit、tag 或源码包，不应把当前开发分支自动视为任意发行物的完整 Corresponding Source。发布二进制或通过网络提供修改后的版本时，应按 AGPLv3 为该发行物提供对应源码，并确保构建所用的 JUCE 8.0.14 精确版本源码可取得；发行材料还应包含复现构建所需的脚本与配置，以及实际使用依赖要求保留的适用许可证和版权通知。

第三方组件与许可证说明见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
