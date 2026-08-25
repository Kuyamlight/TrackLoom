# TrackLoom｜织音 最终需求与规划

- 文档状态：唯一需求与规划基线
- 最后整理日期：2026-08-13
- 适用范围：产品需求、架构边界、阶段路线、验收标准、未定决策
- 维护规则：后续所有需求与规划变更只改本文档；`AGENTS.md` 只记录协作约定、工程原则和经验证实的长期教训；本文档纳入 Git 版本控制。

## 1. 文档使用规则

本文档是 TrackLoom 需求与规划的唯一来源。历史阶段性 `specs` 和 `plans` 中仍有价值的内容已经合并到本文档，后续不再为普通需求变更新建分散的规划草稿。

修改本文档时必须遵守：

1. 已实现、正在实现、计划实现必须明确区分。
2. 规划使用绝对日期或阶段名称，避免只写“以后”“近期”等模糊词。
3. 需求变化应同时检查架构边界、隐私边界、测试要求和阶段验收是否需要调整。
4. 没有验证过的能力不得写成已完成能力。
5. 本文档保持面向项目长期开发，不记录完整聊天过程和一次性命令。
6. 本文档必须随重要需求、阶段路线或验收标准变更一起提交到 Git。

## 2. 项目信息

- 项目名称：TrackLoom｜织音
- 项目定位：开源、Windows 优先、AI 原生音乐创作工作站
- 核心语言：C++20
- 桌面与音频框架：JUCE
- 构建系统：CMake
- AI 服务语言：Python
- 项目许可证：`AGPL-3.0-only`（SPDX）
- 首发平台：Windows
- 目标用户：专业音乐人、音乐创作者、对音乐创作感兴趣但缺少乐理基础的用户

TrackLoom 的目标不是做一次性音乐生成网页，也不是复制现有 DAW 的全部历史功能。它要提供一个可保存、可编辑、可恢复、可解释的本地音乐工程环境，并让 AI 能通过受控命令参与创作。

## 3. 产品边界

TrackLoom 要做：

- 支持外设演奏、录音、循环编曲、音频导入、MIDI 编辑和 AI 生成等创作入口。
- 支持可编辑的多轨音频、MIDI、插件、自动化和工程结构。
- 提供低门槛的循环编曲器，让非专业用户也能从少量素材开始完成短编曲。
- 支持单音色转录、完整音乐分离、AI 辅助扒谱和后续 AI 编曲工具。
- 通过 VST3 接入乐器、采样器、工具、效果器和分析器。
- 在无 AI 配置时仍能使用基础 DAW 功能。
- 支持工程文件夹、可分享单文件工程包、标准 MIDI、音频 stems 和开放工程交换格式导入导出。
- 按项目分类管理 AI 对话、生成内容和可追溯来源。
- 支持不依赖官方服务器的用户存储同步、备份导出和数据恢复。

TrackLoom 不做：

- 不建设 TrackLoom 官方服务器、官方云盘或 AI 请求中转服务。
- 不承诺直接读写主流 DAW 的私有原生工程文件。
- 不把完整工程默认上传到云端。
- 不把 API key、账号凭据、个人录音或无权分发素材提交到仓库。
- 不把 AI 生成内容伪装成可靠扒谱或人工录入内容。

## 4. 用户与核心场景

TrackLoom 面向三类用户：

1. 专业音乐人：需要稳定编辑、多轨工程、插件、导入导出和可控 AI 辅助。
2. 半专业创作者：需要快速从片段、循环、MIDI 和 AI 草稿发展成可修改作品。
3. 初学者：缺少乐理和制作经验，但希望通过低门槛入口完成可听、可改、可导出的音乐。

核心场景：

- 录入或导入一段旋律，自动生成可编辑的节奏、伴奏、和声或结构建议。
- 拖入音频，进行分离、转录、扒谱和工程化整理。
- 用循环编曲器把多个片段组织成稳定节奏和段落。
- 用 AI 修改已有工程，例如“把副歌鼓点加强”“把这段贝斯改得更简单”。
- 导出 MIDI、音频 stems 或开放工程交换格式，进入其他工具继续制作。
- 在没有网络、没有 AI 配置或外部服务失败时，继续完成基础播放、编辑、保存和导出。

## 5. 核心原则

1. 稳定核心优先：工程打开、保存、播放和编辑不能被高级功能破坏。
2. 非破坏性编辑：原始素材必须保留，合并、冻结、AI 补全和生成默认产生可恢复结果。
3. 统一工程命令：用户和 AI 通过同一命令系统修改工程，命令必须可验证、可撤销、可记录。
4. 实时线程安全：音频线程不得执行网络请求、AI 推理、阻塞磁盘操作、不可控锁等待或高延迟任务。
5. 模块可替换：核心不得依赖具体 AI 供应商、模型或插件格式，外部能力通过版本化接口接入。
6. 来源透明：AI 识别、AI 修复和 AI 生成内容必须分别标记。
7. 隐私边界清晰：云端 AI 调用、用户存储同步、离线备份和 AI 本地访问是不同数据操作，必须分别授权。
8. 长期兼容：工程格式必须版本化，数据结构变更必须考虑迁移和测试。
9. 故障隔离：AI、网络、同步和第三方插件失败不得导致基础 DAW 不可用。
10. 证据后完成：声称完成、修复或通过前必须运行与风险相称的验证。
11. 分阶段交付：长期能力可以逐步实现，每个阶段必须有可运行成果和验收边界。

## 6. 总体架构

TrackLoom 分为本地桌面主程序、本地 AI 服务、工程数据层、实时音频层、插件层、导入导出层和可选同步备份层。

### 6.1 C++/JUCE 主程序

主程序负责：

- 图形界面；
- 工程模型；
- 实时音频与 MIDI；
- 循环编曲；
- 插件宿主；
- 任务调度；
- 用户命令与 AI 命令执行。

音频引擎不得直接依赖 AI 服务或具体 UI。UI、快捷键、设备层和 AI 工具都必须通过稳定命令边界修改工程或控制播放。

### 6.2 Python AI 服务

Python 服务负责：

- 对接用户自行配置的第三方云端 AI API；
- 为未来本地模型保留适配接口；
- 执行非实时 AI 任务；
- 返回结构化结果供主程序验证。

主程序不得把 AI 返回结果直接写入工程。所有结果必须经过格式验证、权限检查、工程版本检查和命令系统。

### 6.3 数据与同步

TrackLoom 不建设官方服务器。可选同步由客户端连接用户指定位置：

- 本地同步目录；
- HTTPS WebDAV；
- SFTP。

不支持未加密 FTP 或明文 WebDAV。同步默认关闭。同步凭据必须存入系统安全凭据，不写入同步数据、备份包或普通日志。

## 7. 工程模型

TrackLoom 自有工程格式是权威保存格式。工程模型必须使用稳定 ID 管理轨道、片段、插件、派生结果、对话引用和生成文件引用。

工程应支持：

- 工程文件夹；
- 可分享的单文件工程包；
- 版本化保存格式；
- 事务性保存；
- 缺失素材检查；
- 向后兼容迁移；
- 工程引用完整性检查。

工程数据概念：

- 文件夹轨只负责分类和折叠。
- 组总线负责音频汇总。
- 两者不得混为同一数据概念。
- 轨道的静音、独奏、隐藏、禁用、冻结和隐私权限是独立状态。
- 隐藏只影响显示，不能隐式静音。
- 结构组合、片段整理、渲染合并和共享处理链是不同操作，不能统一叫作“合并”。

## 8. 轨道、片段与时间线

首期轨道类型：

- 音频轨；
- 乐器轨；
- 文件夹轨。

普通乐器轨默认使用一个主要音源或采样器，可串联多个 MIDI 工具、效果器和分析器。多音源乐器机架是后续功能，当前设计不能阻碍未来接入。

时间线应支持：

- 音频片段；
- MIDI 片段；
- 片段创建、删除、复制、拆分、修剪、跨轨移动；
- 轨道创建、删除、重排；
- tempo map；
- time signature map；
- timeline markers；
- 保存和恢复上述结构。

## 9. 播放、音频与 MIDI 核心

截至 2026-06-28，已经完成的本地核心基础包括：

- CMake/C++20 核心库和 CTest 测试入口；
- `Project` 工程模型；
- `Command` / `CommandStack` 工程修改命令栈；
- `ProjectSerializer` / `ProjectFile` 文本工程保存与事务性保存；
- `Transport` 播放头和播放状态；
- `AudioEngine` block 渲染骨架；
- 音频源、增益、声像、静音、独奏、禁用和基础混音；
- `AudioProjectGraph` 项目级音频图；
- MIDI 音符片段；
- tempo、拍号和 marker 模型；
- `PlaybackClock` tick/sample 窗口换算；
- `MidiPlayback` 确定性 MIDI 调度；
- scheduled MIDI sample offset；
- `MidiDispatch` MIDI 消息转换和发送抽象；
- `MidiTrackRouter` 按轨道路由；
- `ProjectMidiOutputGraph` 项目级 MIDI 输出图；
- `MidiOutputSession` 活动音符管理；
- `ProjectPlaybackSession` 音频渲染与 MIDI 输出协调；
- 循环播放 MIDI 窗口切分；
- 循环边界长音符释放；
- 一次性 MIDI chase；
- 安全停止、跳转和 MIDI 输出重建；
- `PlaybackControlCommand` 播放控制命令层；
- JUCE MIDI 输出端口枚举、打开、关闭和立即发送适配层；
- JUCE MIDI 输出设备管理器，可把轨道到设备的运行态配置安全接入项目播放会话。

当前播放核心边界：

- 当前 MIDI chase 只覆盖 Note On。
- 当前 MIDI 转换主要覆盖 Note On 和 Note Off。
- 当前循环能力保证 MIDI 调度和分发循环化。
- 当前不能宣称已经完成音频素材循环、插件尾音、延迟补偿、控制器 chase、踏板 chase、Pitch Bend chase、Aftertouch chase 或插件状态 chase。
- 当前 JUCE MIDI 输出端口适配层已能枚举系统输出设备、按稳定 id 打开和关闭端口，并发送核心层生成的三字节 MIDI 消息。
- 当前 JUCE MIDI 输出设备管理器已能把 `trackId -> MidiOutputDeviceInfo` 的运行态配置转换成播放会话的安全 MIDI 输出路由，覆盖打开失败保留旧设备、切换前释放活动音符、清空输出和非法轨道不打开设备。
- 当前 JUCE MIDI 输出普通测试不依赖真实外设；已有默认跳过的外接硬件冒烟测试入口，但尚未在真实外接设备上完成实机验证。
- 当前已有 MIDI 输出设备快照差异识别、手动刷新缓存入口、设备移除绑定策略计划、刷新结果到设备管理器安全重建的连接层，以及无 UI 的设备选择控制层；设备消失时可安全收缩已应用的运行态路由，设备重新出现时可恢复已保留选择的运行态路由，同一设备改绑到另一条轨道时会重建轨道路由，释放失败时保留旧路由并返回失败原因；当前只读路由快照可生成展示状态分类，可判断整体路由状态是否需要应用更改，可在应用当前选择时跳过无变化重建或安全移除已清除的旧路由，并可把设备刷新结果汇总成 UI/诊断面板可直接读取的注意事项；未应用的离线选择会保留给 UI 和重连提示，不要求未准备的播放会话执行安全重建；还没有后台轮询、用户可操作的 UI 设备选择、真实设备热插拔处理、插件乐器输出或采样器输出接入。

后续接入规则：

- 真实 MIDI 设备、插件乐器和采样器必须走 `MidiDispatch`、轨道路由、项目输出图和输出会话边界。
- 停止、跳转、切换输出目标前必须释放活动 MIDI 音符。
- 释放失败不得移动播放头、停止播放或替换输出路由。
- 音频渲染失败不得发送该 block 的 MIDI。
- MIDI 输出失败不得回滚已经成功渲染的音频或播放头推进。

## 10. 循环编曲器

循环编曲器是当前唯一确定的特色快速编曲方式。它应帮助用户用少量片段快速构造可听、可编辑、可重复推敲的音乐结构。

循环编曲器要求：

- 通过独立接口接入，不写死为唯一编曲形式。
- 持久化音乐时间，而不是固定采样位置。
- tempo 变化、参数生效边界、跨循环 Note Off、重叠和效果尾音必须有确定性规则。
- 固定的循环变体序列可表达为更长的循环内容，不增加冗余抽象。
- 用户应能把循环编曲结果转成普通时间线内容继续编辑。

首期目标：

- 建立循环片段和时间线播放的统一时间语义。
- 支持基础 MIDI 循环播放和边界释放。
- 在 UI 和音频素材循环进入前，先保持底层调度可测试。

## 11. 插件与 VST3

首期只承诺 VST3。插件格式必须通过适配层接入，核心不得依赖某个具体插件 SDK 的细节。

插件规则：

- VST3 扫描从首次实现起使用辅助进程。
- 稳定版本中的第三方插件通过独立宿主进程运行。
- 插件崩溃不得拖垮基础工程编辑和保存。
- 插件状态、版本、路由、声道、自动化和素材依赖必须可保存和检查。
- API 密钥、账号凭据和私有授权数据不得进入工程文件。

共享处理链不能简单按插件名称判断等价。除插件版本、状态、路由、声道、处理链和自动化外，还必须考虑随机状态、复音分配、非线性处理、侧链、历史缓冲和效果尾音。无法证明声音等价时，不得宣称无损，只能保留独立处理链或进行非破坏性渲染。

## 12. AI 系统

AI 是 TrackLoom 的原生能力，但不能成为基础 DAW 可用性的前提。

AI 系统要求：

- 通用模型负责理解、规划和调用工具。
- 专业模型负责具体音乐、音频或转录能力。
- 只配置一个模型时，不得假设它具备未声明能力。
- 多模型选择依据能力、质量、速度、费用、隐私和用户偏好。
- AI 可以调用合法工程命令，但不能绕过数据验证、隐私权限、实时线程边界和操作历史。
- AI 返回的结构化结果必须验证后才能写入工程。
- AI 异步任务必须携带工程版本和对象前置条件。
- 结果返回时如果工程发生相关冲突，必须重新规划、显示差异或请求确认，不得覆盖用户新修改。
- 完全代理模式必须支持取消，并限制最大步骤、费用、执行时间和外部数据传输。

AI 内容来源标记：

- AI 识别：模型从已有音频或 MIDI 推断出的内容。
- AI 修复：模型基于已有材料做的校正或补全。
- AI 生成：模型新生成的音乐、音频、MIDI 或文本。

三者必须分别标记，不能混用。

## 13. 隐私、安全与数据生命周期

云端 AI 调用只指用户主动调用第三方 AI 服务时，发送完成任务所必需的数据。它不代表把完整工程上传到 TrackLoom 服务器。

隐私规则：

- 未配置服务或用户未主动发起任务时，不得发生云端 AI 数据传输。
- 云端 AI 调用前检查工程、轨道、素材和派生依赖权限。
- 禁止发送的内容不得通过缓存、混音、日志或临时导出间接传输。
- 日志只记录诊断所需信息，不记录不必要的完整提示词、原始音频或密钥。
- 外部服务和网页内容是不可信输入，必须验证后使用。

对话和生成内容：

- AI 对话按项目稳定 ID 分类。
- 对话和生成文件与用户主动保存的工程文件保持独立。
- 只有明确应用的 AI 命令才可修改工程。
- 生成文件加入工程后必须进入工程素材目录，不能继续依赖对话缓存。
- 删除对话或生成文件前必须检查工程引用。
- 同步删除使用删除标记，避免其他设备恢复已删除内容。

备份：

- 备份包必须版本化。
- 备份包必须带文件清单和校验信息。
- 恢复前允许预览、选择、合并或作为副本恢复。
- API key 和系统安全凭据默认不进入备份包。

## 14. 导入导出与 DAW 互通

TrackLoom 自有工程格式是权威保存格式。与其他 DAW 互通通过标准交换格式实现，而不是直接兼容私有原生工程文件。

支持方向：

- Standard MIDI File；
- 音频文件；
- stems；
- DAWproject；
- AAF/OMF 等开放或行业交换格式。

MIDI 规则：

- MIDI 导出默认使用 Standard MIDI File Type 1 保留多轨结构。
- 提供 Type 0 兼容选项。
- 导出不得默认量化或改写用户演奏时间。
- MIDI 可保存 MIDI 事件和有限元数据。
- MIDI 不能保存 VST3 状态、采样器素材、音频效果链、AI 来源信息或 TrackLoom 特有工程历史。
- MIDI 导入应尽量保留 tempo map、拍号、marker 和 channel 信息。
- 未知 meta 或 SysEx 信息应提示用户，不能静默丢弃。

外部工程交换导入时必须标记：

- 来源格式；
- 缺失信息；
- 可能改变声音的内容；
- 无法恢复的插件、路由、自动化或素材依赖。

## 15. 测试与验收

不同改动必须匹配不同验证：

- 工程模型、命令和迁移：单元测试。
- MIDI、循环调度和音频算法：确定性测试和边界测试。
- 播放、录音、导入导出和插件：集成测试。
- 实时音频：延迟、爆音、丢帧和压力测试。
- 保存与恢复：异常关闭、缺失素材、插件崩溃和网络中断测试。
- AI：适配器契约、格式验证、隐私阻断和失败降级测试。
- 数据管理：对话增删归档、生成文件引用、备份恢复、同步冲突、删除传播、断点恢复和损坏检测。
- 用户流程：非乐理用户完成短编曲、修改和导出。

修复缺陷时优先添加能够复现问题的测试。阶段性完成标准必须包含目标环境、性能预算、连续运行时间、保存恢复条件和用户任务指标。

当前开发环境验证规则：

- Windows/MSVC 构建含中文注释源码时，CMake 目标必须保留 `/utf-8`。
- 在普通 PowerShell 中运行 MSVC/Ninja 构建前，必须先加载 Visual Studio Build Tools 开发环境，或用 `VsDevCmd.bat` 包裹构建命令。
- 文档整理完成后至少运行文档结构检查和 `git diff --check`。

## 16. 2027 年毕业设计交付基线

- 基线确认日期：2026-08-08
- 毕业设计题目：基于 JUCE 的 AI 辅助循环编曲工作站设计与实现
- 最终验收截止日期：2027-05-31

本基线从长期 A–H 阶段中抽取一条可答辩、可演示、可复现的纵向闭环，用于界定 2027 年毕业设计必须交付的范围。它不删除、取代或缩短本文档的长期愿景与 A–H 路线；未进入毕业验收的能力仍按后续阶段开发。

### 16.1 毕业纵向闭环

毕业版必须让用户在 Windows 上完成一次“创建或打开工程 → 组织并可听播放循环 → 完成一次 AI 辅助 MIDI 修改 → 人工继续编辑或撤销 → 保存并重新打开 → 导出 MIDI 和可听 WAV”的完整流程。必须项为：

1. **Windows 可听播放**：接入 JUCE 音频设备回调和输出设备选择，提供至少一种不依赖 VST3、外接 MIDI 硬件或网络服务的可分发声音路径，使 MIDI 循环和导入音频可听播放。
2. **循环编曲**：在可视界面中创建、排列、编辑和循环播放 MIDI 片段，支持将循环展开到普通时间线后继续编辑，并保持 tempo、拍号、跨边界 Note Off 和撤销/重做语义一致。
3. **MIDI/WAV 交换**：至少支持 Standard MIDI File Type 1 导入和导出、WAV 导入为非破坏性音频片段，以及将主输出离线渲染为 WAV；导出 MIDI 不得默认量化或改写用户演奏时间。
4. **单一 AI MIDI 工作流**：只将“根据用户明确指令改写所选 MIDI 循环”列为毕业必须的 AI 创作能力。用户必须主动发起调用；Python 本地服务只直连用户自行配置的第三方 API；只发送完成该次任务所必需的指令、所选 MIDI 数据和必要上下文。返回结果必须通过 schema、音乐时间范围、工程版本、对象前置条件和隐私权限验证，再通过统一工程命令应用，且可撤销、可重做、可记录来源。
5. **保存与恢复**：保存循环、展开后时间线、MIDI/音频素材引用、AI 来源和操作结果；重新打开后可继续播放、编辑和导出。写入失败或模拟异常中断不得破坏上一次成功保存的工程；毕业验收只承诺恢复到最近一次成功保存，不把完整崩溃日志或未保存操作恢复列为必须项。
6. **证据交付**：每项毕业验收要求都必须能对应到可重复的构建或测试命令、测试结果、演示工程或用户任务记录；论文、答辩演示和发布标签使用同一功能边界，不将设想写成已完成。

没有配置 AI 时，除第 4 项外的整个本地闭环必须仍然可用；第三方 API 失败、超时、返回非法结构或与新工程版本冲突时，不得修改工程、阻塞音频线程或破坏播放、保存和导出。毕业交付不得新增 TrackLoom 官方服务器或 AI 请求中转；API key 必须保存在 Windows 系统安全凭据中，不得进入仓库、工程文件、普通日志或验收证据包。发布候选版应公开对应源码和第三方依赖清单，与项目许可证 `AGPL-3.0-only`（SPDX）保持兼容，不携带无权分发的插件、模型或音色素材。

### 16.2 毕业验收指标

验收对象是 2027-05-31 形成的 Windows x64 发布候选版。默认基准为 48 kHz、256 samples、立体声输出；如答辩设备不支持该配置，必须在证据中记录实际设备、驱动、采样率、block 大小和偏差理由。所有自动化音频验收必须使用版本化、可取得的固定参考工程及其不可变输入素材、预期结果清单和内容签名；清单至少声明目标采样率、声道数、目标总时长、预期起止位置与非静音活动区间、峰值上限，以及参考渲染比较方式和容差。候选版不得以临时新建工程、替换素材或事后调整清单逃避这些断言。

1. **音频与播放**：在答辩基准机上连续循环播放固定参考工程 30 分钟，应用无崩溃，音频后端可用的 xrun/underrun 诊断计数为 0；如后端不提供该计数，则应用自有的音频回调超时计数必须为 0。人工监听记录中不得出现可复现的爆音、悬挂音符或循环边界丢拍。固定参考工程的离线 WAV 必须为清单声明的采样率和声道数，时长与目标范围偏差不超过 1 sample；在声明的活动区间内必须满足清单规定的非静音阈值，在活动区间外及预期起止边界不得出现未声明的音频，并满足清单规定的无削波峰值上限。对同一候选版和固定输入，重复内部离线渲染必须按清单声明达到 PCM 帧等价；再与版本化参考结果进行 sample 对齐比较，帧级最大误差和 RMS 误差不得超过清单预先声明的合理容差。允许容差适配已批准的渲染算法，但必须随参考工程版本固定并记录，不能在验收后放宽。
2. **保存与恢复**：固定回归工程连续执行 20 次“编辑—保存—关闭—重新打开”后，轨道、片段、循环、tempo/拍号、素材引用和 AI 来源检查全部一致；模拟 10 次保存写入失败或中断，上一次成功保存的文件必须 10/10 可打开且通过引用完整性检查。
3. **AI 验证与故障隔离**：至少 20 个固定自动化样例覆盖合法结果、schema 非法、越界音符、过期工程版本、前置条件冲突、隐私拒绝、超时和第三方服务失败；所有非法或失败结果都必须被拒绝且工程序列化结果保持字节等价，所有合法应用都必须能一次撤销并一次重做回到对应状态。
4. **导入导出**：固定样例集中的 MIDI Type 1 和 WAV 全部能导入、保存、重新打开和导出；MIDI round-trip 后的轨道数、音符开始、长度、音高和力度与预期一致，不支持的 meta/SysEx 信息必须有可见提示；WAV 导入不改写原文件。
5. **初学者任务**：至少邀请 5 名没有 DAW 使用经验的参与者，允许阅读统一的一页快速指南，但测试期间不接受开发者操作代劳。至少 4/5 参与者必须在 30 分钟内独立完成 8 小节 MIDI 循环、一次 AI 改写并审听或撤销、保存重开，以及 MIDI 和 WAV 导出；记录完成时间、失败步骤和是否需要语言提示。

第 16.1 节的每一项毕业必须项和本节每一项验收指标均为同时成立的通过门槛。任一必须项、数值阈值、固定参考工程内容断言或证据缺失时，毕业基线不得判定通过；报告、论文和答辩材料必须如实标为未通过或未达标，说明原因不改变该失败结论。

### 16.3 绝对日期里程碑

| 截止日期 | 必须形成的可验证结果 |
| --- | --- |
| 2026-08-31 | 锁定毕业纵向闭环、验收样例目录、答辩基准机和证据矩阵；对每个必须项标记已实现、部分实现或未实现。 |
| 2026-10-31 | 完成 JUCE/Windows 音频设备回调、输出设备选择和不依赖 VST3 的最小可听路径，用固定 MIDI 工程记录 10 分钟连续播放证据。 |
| 2026-12-31 | 完成可视 MIDI 循环创建、编辑、循环播放和展开到普通时间线，为循环边界、tempo/拍号和撤销/重做补齐确定性测试。 |
| 2027-02-28 | 完成 MIDI Type 1 与 WAV 导入、MIDI Type 1 与主输出 WAV 导出，完成毕业闭环数据的保存重开和写入失败保护测试。 |
| 2027-03-31 | 完成单一 AI MIDI 改写工作流，打通用户主动调用、第三方 API 适配、最小数据发送、结果验证、工程命令、撤销/重做、来源标记和失败隔离。 |
| 2027-04-30 | 完成 30 分钟连续播放、导入导出、保存恢复、AI 固定样例集和至少一轮初学者任务测试，关闭所有阻断毕业闭环的已知高优先级问题。 |
| 2027-05-31 | 完成 Windows x64 发布候选版、可重复演示工程、测试与用户任务证据包、论文功能对应表和答辩演示流程；任一毕业必须项或验收指标未达标即不得判定毕业基线通过，未达标项必须如实标注，不得用长期计划代替验收证据。 |

### 16.4 2026-10-31 音频设备与内置发声实施设计

- 设计确认日期：2026-08-09
- 已确认选择：独立实时播放运行时、WASAPI 共享模式、可替换的轻量复音合成器、播放前不可变 MIDI 快照。

本节细化第 16.3 节的 2026-10-31 里程碑，只定义第一条真实可听 MIDI 垂直切片。它不得被表述为 WAV 导入播放、录音、VST3、SoundFont、ASIO 或完整音频引擎已经完成；这些能力继续按后续里程碑和长期路线接入同一实时输出边界。

#### 16.4.1 模块和线程边界

1. 核心层新增平台无关的 `PreparedMidiPlaybackPlan` 及 builder。消息线程捕获当前工程与设备格式快照，非实时 worker 根据该快照、tempo map、循环范围和播放起点生成不可变计划；计划只保存预先排序的固定大小数值数据，不把轨道名称、片段名称或其他需要在回调中复制的字符串带入实时线程。非循环计划使用工程原点起算的绝对采样位置；循环计划保存单次规范循环内相对 `loopStartSample` 的采样偏移、循环长度和独立边界事件表。
2. 每个计划事件至少包含采样坐标、Note On/Note Off、MIDI 通道、音高、力度、紧凑基础 `noteInstanceId`、稳定 `eventOrdinal` 和乐器槽位。builder 按稳定源音符身份为当前计划分配稠密、无碰撞的 `noteInstanceId`，不得截断字符串哈希冒充唯一标识；在相同 sample 先排 Note Off、后排 Note On，再沿用轨道、片段和音符的稳定顺序分配 `eventOrdinal`。乐器槽位保存播放开始时冻结的轨道增益、声像及路由信息；首版所有兼容乐器轨使用同一种内置音色，但不得把单音色写死为永久架构。
3. JUCE 平台层新增独立 `JuceAudioHost`，直接独占 `juce::AudioIODeviceType`（`WASAPIDeviceMode::shared`）、当前 `juce::AudioIODevice`、音频 callback、当前计划、内置合成器和实时诊断状态。应用消息线程拥有并控制 host；音频 callback 不读取可变 `AppProjectSession`、`Project` 或 UI 对象。设备枚举和热插拔通知通过该专用 device type 完成，不让未支持的后端进入正式路径。
4. 音频 callback 通过固定容量的非拥有声道视图写入 JUCE 提供的逐声道输出指针，不得把 `float**` 错当成当前要求单块连续平面内存的 `AudioBlock`。所有 voice、包络、游标、声道视图和必要 scratch 必须在设备启动或计划安装边界预分配。跨线程状态只允许使用经 `std::atomic<T>::is_always_lock_free` 静态断言确认的整数或枚举原子，不得在 callback 中使用 `atomic<shared_ptr>` 或其他可能退化为内部锁的原子类型。
5. 现有 `ProjectPlaybackSession` 继续承担平台无关调度测试、现有外部 MIDI 输出和非实时逻辑，不直接进入声卡 callback。后续只有在其动态分配、工程读取和并发所有权边界得到独立验证后，才能合并实时路径。
6. 当前 30 Hz JUCE `Timer` 只读取原子播放位置、设备状态和诊断计数并刷新界面，不再推进 Transport 或渲染静音 block。
7. host 的关闭顺序固定为停止播放、调用 `AudioIODevice::stop()` 并等待 pending callback 清空、关闭设备、释放计划和合成器。设备切换、计划构建、音频图 rebuild、磁盘访问和设置保存全部位于消息线程或其他非实时线程。

#### 16.4.2 计划生成和实时播放规则

1. 用户发起播放时，消息线程捕获不可变工程播放快照、运行态 `projectEditGeneration` 和完整 `deviceFormatGeneration`；后者在设备实例、设备 id、实际采样率、当前或已准备最大 block、输出声道数或输出声道掩码任一变化时递增。实际 MIDI 收集与排序在可取消的非实时 worker 上执行，期间 UI 显示 `Preparing` 且继续响应。结果回到消息线程后必须再次核对工程 generation、完整设备格式 generation、循环范围和播放起点；任一条件变化都丢弃过期结果。builder 复用已有确定性 MIDI、tempo、循环、静音、独奏和禁用规则；隐藏状态只影响显示，不得过滤声音。计划构建失败或过期时不得开始播放，也不得静默复用旧计划。
2. 计划事件数设显式上限，首版总上限为 1,000,000 个事件，每次 callback 最多处理 4,096 个计划事件。builder 必须按已准备的最大 block 验证普通窗口和循环回绕窗口的事件密度；运行时也必须在修改 voice 前预检当前 block。超过总量或单 callback 上限、采样率非法、tempo/循环范围非法或事件换算溢出时必须返回稳定失败原因并保持整块静音，不得截断、部分应用事件后继续播放。
3. 首版只允许在停止状态替换计划。安装计划时，消息线程调用 `AudioIODevice::stop()`；该 API 必须在 pending callback 全部退出后才返回。随后替换拥有对象并重置 voice 和事件游标，再用 `AudioIODevice::start(callback)` 恢复设备回调；计划在整个播放期间保持地址和内容稳定。
4. 播放中的工程修改正常进入工程命令历史和保存状态，但声音在下次停止并重新播放后才更新。首版播放中跳转采用“安全停止、设置新位置、重新生成或重置计划、重新开始”，不实现 block 边界无锁热替换。
5. callback 每次先清零全部有效输出；只有设备、计划和播放状态均有效时，才消费当前半开采样窗口内的事件。它在相邻事件之间分段渲染 voice，处理事件后继续渲染。运行态分别维护只增不回绕的 `renderedSampleCount` 和可在循环边界回绕的 `projectSamplePosition`；UI 显示后者，连续运行与诊断证据使用前者，不能把两种时间混为同一计数。
6. callback 跨循环右边界时必须把 block 拆为循环尾部和循环头部两个半开子窗口：先渲染 `[projectSamplePosition, loopEndSample)`，再处理边界 Note Off；随后递增 `loopIteration`、把工程位置和事件游标重置到循环起点，并在渲染第一个循环头样本前处理该轮 chase。若 block 恰好结束于右边界，边界 Note Off 和迭代递增在该 block 结束时完成，下一轮 chase 延迟到下一 callback 的 sample offset 0。单个 block 跨越多次短循环时重复同一规则，处理量仍受 block 帧数和每 callback 事件上限约束。
7. voice 实例身份是 `(noteInstanceId, loopIteration)`；非循环播放的 `loopIteration` 固定为 0。上一轮仍处于 release 的 voice 与下一轮相同基础音符可以并存，Note Off 只能匹配同一轮实例。循环计划继续沿用半开窗口、右边界释放和起点 chase 规则，并保证连续 block 不重复 chase。
8. 正常用户停止使用显式 `Stopping` 状态：callback 在安全边界停止消费后续计划事件，让当时所有活动 voice 进入既定 30 ms release，并只继续渲染这些尾音，直到 voice 提前全部静音或达到 `ceil(sampleRate * 0.030)` 帧的硬上限；随后硬重置 voice、发布 `Stopped`，只有此后的 callback 才保证全零。没有活动 voice 时可以立即进入 `Stopped`。新计划安装和重新播放必须等待 `Stopped`，不能把 release 尾音误当成已经停止。设备已经停止、断开或发生不可恢复错误时无法保证渲染尾音，host 必须在非实时清理路径立即硬重置 voice 和运行状态，不能等待永远不会再次到来的 callback。
9. JUCE 8.0.14 的 `juce::AudioDeviceManager::audioDeviceIOCallbackInt()` 会在回调聚合路径取得 `audioCallbackLock`，`juce::Synthesiser::renderNextBlock()` 也会取得内部 `CriticalSection`。首版因此不通过 `AudioDeviceManager` 聚合实时 callback，也不使用 `juce::Synthesiser`；host 直接启动当前 `AudioIODevice`，并使用固定容量、无互斥锁的 `BuiltInPolySynth` 直接消费计划事件，不在 callback 中创建 `juce::MidiBuffer`。
10. `BuiltInPolySynth` 使用 16 个固定 voice、正弦振荡器、力度缩放和固定 ADSR；初始目标为 5 ms attack、20 ms decay、0.8 sustain、30 ms release。每个 voice 的最大峰值为 0.045，使 16 voice 理论同相峰值不超过 0.72。Note On 按稳定 `eventOrdinal` 处理：存在空闲 voice 时选择最低 voice index；否则选择 `voiceStartSerial` 最小的 voice，并以最低 voice index 作为防御性并列规则。每个成功 Note On 获得单调递增的 `voiceStartSerial`。窃取时先使旧 `(noteInstanceId, loopIteration)` 失效，再把 voice 分配给新实例并递增 voice stealing 计数；旧实例后来到达的 Note Off 只能被忽略并递增 stale Note Off 计数，不得关闭该槽位的新实例。
11. 音频 callback 禁止动态分配、排序、阻塞锁、文件或网络访问、外部 MIDI `sendMessageNow()`、不可控异常和 UI 调用。回调边界必须防止异常逃逸，并在任何无效状态下优先输出静音。

#### 16.4.3 设备设置、状态和故障降级

1. 首期只承诺 Windows x64 上的 WASAPI 共享模式。首次或缺失本机设置时默认请求 0 路输入、2 路输出、48 kHz、256 samples 和立体声；允许用户选择 1–2 路输出。正式入口只允许 JUCE 设备类型 `Windows Audio`，必须隐藏或拒绝 DirectSound、`Windows Audio (Exclusive Mode)`、低延迟实验模式和 ASIO，避免把未验收后端误展示为已支持。驱动不支持请求格式时可以接受 JUCE/驱动协商的实际格式，但必须显示并在证据中记录请求、实际设备、驱动、采样率、block 大小、声道数和偏差理由；不得伪造 256 samples 或把单一可用 buffer 选项当成错误。ASIO 保留为后续适配器，不是本里程碑前置条件。
2. “工具 → 音频设置…”打开专用 WASAPI 共享输出设置界面，使用 JUCE 控件展示该 device type 的输出设备、采样率、buffer 大小和声道选项，并提供低音量测试音。该界面不使用会暴露其他后端的通用 `AudioDeviceSelectorComponent`。下拉框只编辑候选配置：选择设备或格式绝不自动关闭、打开或改写 Windows 默认路由；只有用户按“应用”才允许切换输出。候选与当前已打开的设备或实际格式不一致时，界面必须明确显示“待应用”，并禁用测试音；测试音只在工程播放停止、没有待应用候选且当前输出可用时可用，只验证当前实际输出，不修改工程、播放头或命令历史。
3. 音频设备状态是本机运行配置，保存到用户应用数据目录，不写入 `.trackloom` 工程。首期只使用一份 `AppAudioSettings`：缺失设置仍请求 48 kHz、256 samples、2 路输出；每次成功“应用”后，以 host `actualFormat` 覆盖这份“下次启动期望/上次成功实际”快照并持久化设备显示名、采样率、block 和输出声道数，不能把当次候选值冒充实际值。设置 UI 可以在当次操作中显示 request → actual 偏差。本轮不把 `Windows Audio/<display name>` 形式的 `deviceId` 当作跨重插稳定 endpoint ID，也不把 deviceId 或声道 mask 写入本机设置；证据日志仍可记录当次 `deviceId` 和 mask，但必须明确不声称它们跨重插稳定。设置恢复失败时回退到可用默认设备并显示警告，不得阻止用户打开和编辑工程。
4. 无设备或初始化失败时，应用继续提供工程编辑和保存，播放入口禁用并显示稳定原因；设备断开、移除或重启时立即静音、停止播放并使当前计划失效。`Faulted`/`DeviceError` 诊断必须保留到一次成功的 `openOutput` 替换设备实例后才清除；故障状态下设置界面仍必须允许在非实时线程枚举、选择和“应用”新设备恢复。采样率变化后必须重新生成计划才能播放。
5. callback 收到超过已准备上限的 block 时整块静音并计数，由消息线程重新准备；输出指针为空、声道数量变化或其他格式异常时只处理有效指针，不越界、不保留旧缓冲内容。
6. callback 不构造诊断字符串，只发布稳定错误枚举和原子数字。UI 至少展示当前设备、实际采样率、block 大小、输出声道数、callback 次数、callback 超时次数、超大 block 次数、xrun/underrun（后端可用时）、voice stealing 次数和 stale Note Off 次数。
7. callback 执行时间达到或超过当前 block 对应的实时期限时递增超时计数；xrun/underrun 查询应在非实时线程读取后端状态。诊断失败本身不得阻塞或终止音频线程。
8. JUCE 的 `audioDeviceError(const String&)` 可能从任意线程调用；该入口只发布稳定设备错误标志，不复制或格式化错误字符串。消息线程随后读取设备错误并生成用户反馈。设备列表变化通知同样只能递增单调 device-list revision/generation 并请求消息线程刷新，不能在通知线程直接扫描、关闭、打开或替换设备。设置 UI 只消费该 revision 的最新值进行一次刷新；不得以 30 Hz 轮询扫描设备，也不得让多个 `Timer` 竞争消费一个可丢失的 pending 标志。

#### 16.4.4 自动测试、实机证据和完成门槛

1. 核心计划测试覆盖 tick 到 sample 换算、tempo 变化、事件稳定排序、轨道增益和声像快照、静音/独奏/禁用、隐藏不影响声音、重叠同音高、非法输入、总事件和单 callback 密度上限、半开窗口、循环尾/头 block 切分、恰好结束于右边界、单 block 多次回绕、边界 Note Off、循环起点 chase、连续 block 不重复 chase、`loopIteration` 实例隔离，以及单调 `renderedSampleCount` 与回绕 `projectSamplePosition`。合法事件位置与预期偏差不得超过 1 sample。
2. 合成器离线测试覆盖 44.1/48/96 kHz、64/256/512 samples block、Note On 精确起点、跨 block 连续性、Note Off 与 release、同音高和跨循环实例释放、16 voice、空闲 voice 最低 index、同 sample 稳定事件顺序、按 `voiceStartSerial` 窃取、并列防御规则、被窃取实例的迟到 Note Off 忽略、正常停止最多 30 ms release 后全零、设备错误硬重置和峰值上限。A4 稳态频率应在预先固定的容差内接近 440 Hz；设计理论同相峰值不超过 0.72，固定清单的验收峰值上限为 0.8。
3. callback 和 fake backend 测试覆盖未播放清零、正常非零输出、实际 `numSamples` 推进、`Playing → Stopping → Stopped`、设备 `stop()` 清空 pending callback 后才允许替换计划、设备启动/停止/重启、采样率变化、空输出指针、单/双声道、超大 block、事件密度超限整块静音、初始化失败和 callback 异常隔离。完成预热后，受测 callback 自有路径的堆分配次数必须为 0，并通过代码审查确认没有锁等待和实时禁用操作。
4. 应用层与 JUCE 测试覆盖音频设置菜单、命令分发、设备状态、设置恢复、测试音、播放禁用、`Preparing` 期间消息线程继续响应、取消构建、过期 `projectEditGeneration` 或设备格式结果被丢弃、中文反馈和 UI Timer 不再推进播放头；还必须覆盖候选选择不改当前输出、候选与实际格式不一致时“待应用”与测试音禁用、成功应用后保存/显示 host `actualFormat`、UTF-8 中文按钮文本、单一 buffer 的有效展示、revision 驱动的一次设备列表刷新、设备移除后的 `Faulted`/`DeviceError` 保留以及从设置和 controller 两条路径恢复。桌面应用继续通过隐藏启动和关闭烟测。
5. 固定 MIDI 参考工程、预期事件及音频属性清单和内容签名放入版本控制；建议目录为 `tests/fixtures/audio/minimum-audible-midi/`。在答辩基准机上记录至少 10 次参考工程计划构建耗时，单次目标不超过 250 ms；未达标时必须保留 worker 隔离和可取消 UI，不得改回消息线程同步排序。每次实机证据至少记录 commit、Windows 版本、设备、驱动、采样率、block、声道、计划构建耗时、持续时间、xrun/超时计数和人工监听结论，建议保存到 `tests/evidence/audio/`。
6. 2026-10-31 里程碑只有在固定 MIDI 工程通过内置合成器实际发声、输出设备可选择、所有相关自动测试通过，并以 48 kHz、256 samples、立体声作为默认请求连续循环 10 分钟且应用无崩溃、xrun/underrun 为 0（后端不提供时 callback 超时为 0）、无可复现爆音、悬挂音符或循环边界丢拍时才可标为完成。若设备不支持默认请求，runner 必须先打印完整 request → actual 偏差并拒绝开始 10 分钟运行，直到用户显式设置 `TRACKLOOM_ACCEPT_AUDIO_FORMAT_DEVIATION=1`；无偏差时不得要求该变量。获确认后，以 JUCE/驱动返回的实际格式完成同一时长运行，并按第 16.2 节记录请求、实际值、用户确认和偏差；不得因未能取得 256 而伪造格式或省略证据。
7. 本里程碑的 10 分钟证据不能替代 2027-05-31 候选版的 30 分钟音频验收；WAV 导入播放、离线 WAV 渲染和对应参考 PCM 验收继续按 2027-02-28 里程碑完成。

#### 16.4.5 可执行实施计划

> **执行要求：** 实施代理必须使用 `superpowers:subagent-driven-development`（推荐）或 `superpowers:executing-plans` 逐任务执行；每个任务必须先取得与该任务缺失能力一致的 RED，再写最小生产实现，随后运行定向 GREEN、相关回归和 `git diff --check`。不得把多个任务压成一次大提交。

**目标：** 在不让现有 `ProjectPlaybackSession`、UI `Timer` 或可变工程对象进入声卡 callback 的前提下，交付 Windows x64、WASAPI shared、固定 16 voice 内置正弦合成器的首条真实可听 MIDI 播放闭环，并留下可重复的自动测试与 10 分钟实机证据。

**架构：** `PreparedMidiPlaybackPlan` 在非实时 worker 上从工程副本生成；平台无关 `BuiltInPolySynth` 和 `PreparedMidiPlaybackRuntime` 只消费不可变数值数据；`JuceAudioHost` 直接拥有 shared WASAPI `AudioIODeviceType`、当前 `AudioIODevice`、callback 和计划；`AppPlaybackController` 只协调 generation、worker、host 和 UI 状态。设备、计划和 UI 之间不得通过 `AudioDeviceManager`、`juce::Synthesiser` 或 callback 内共享可变 `Project` 连接。

**技术栈：** C++20、JUCE 8.0.14、CMake/Ninja、MSVC 19.44、CTest、WASAPI shared。

**全局执行约束：**

- 从 `e21724e` 或包含该提交的更新基线创建 `codex/realtime-audio-foundation` 隔离工作树；先确认主工作树只有用户已有的 `.gitignore` 修改，隔离工作树不得复制、暂存或覆盖该修改。
- 新工作树首次配置使用：

  ```powershell
  cmd.exe /d /s /c 'call "E:\Android\VS\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 && "E:\Android\VS\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DTRACKLOOM_JUCE_ROOT="E:\Android\DevTools\JUCE" -DBUILD_TESTING=ON'
  ```

- 下文的 `CORE_TEST`、`APP_TEST`、`JUCE_TEST` 分别表示以下定向命令；任一测试宣称通过前必须重新执行相应命令并读取退出码与失败数：

  ```powershell
  # CORE_TEST
  cmd.exe /d /s /c 'call "E:\Android\VS\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 && "E:\Android\VS\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --target trackloom_core_tests --parallel && "E:\Android\VS\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --test-dir build -R "^trackloom_core_tests$" --output-on-failure'

  # APP_TEST
  cmd.exe /d /s /c 'call "E:\Android\VS\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 && "E:\Android\VS\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --target trackloom_app_support_tests --parallel && "E:\Android\VS\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --test-dir build -R "^trackloom_app_support_tests$" --output-on-failure'

  # JUCE_TEST（Task 5 建立目标后可用）
  cmd.exe /d /s /c 'call "E:\Android\VS\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 && "E:\Android\VS\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --target trackloom_juce_audio_tests --parallel && "E:\Android\VS\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --test-dir build -R "^trackloom_juce_audio_tests$" --output-on-failure'
  ```

- 所有新增 MSVC target 保留 `/utf-8`；callback 所触达函数不得把测试中的 fake、分配计数器或日志分支编入正式实时路径。
- 每个任务结束都执行 `git diff --check`，只暂存任务列出的文件，核对 `git diff --cached --name-only` 后创建所列独立提交；未经明确授权不 push。

##### Task 1：生成确定性的非循环不可变 MIDI 计划

**文件：**

- 新建 `src/core/PreparedMidiPlaybackPlan.h`
- 新建 `src/core/PreparedMidiPlaybackPlan.cpp`
- 修改 `src/core/CMakeLists.txt`
- 修改 `tests/core_tests.cpp`

**公开接口：**

```cpp
enum class PreparedMidiEventType : std::uint8_t { NoteOff, NoteOn };
enum class PreparedMidiInstrumentKind : std::uint8_t { BuiltInSine };

struct PreparedMidiEvent {
    std::int64_t samplePosition = 0;
    std::uint32_t noteInstanceId = 0;
    std::uint32_t eventOrdinal = 0;
    std::uint32_t instrumentSlotIndex = 0;
    std::uint8_t channel = 1;
    std::uint8_t noteNumber = 60;
    std::uint8_t velocity = 0;
    PreparedMidiEventType type = PreparedMidiEventType::NoteOn;
};

struct PreparedMidiInstrumentSlot {
    PreparedMidiInstrumentKind kind = PreparedMidiInstrumentKind::BuiltInSine;
    float gain = 1.0f;
    float pan = 0.0f;
    std::uint32_t outputBusIndex = 0;
};

struct PreparedMidiLoop {
    std::int64_t loopStartSample = 0;
    std::int64_t loopLengthSamples = 0;
    std::vector<PreparedMidiEvent> boundaryNoteOffEvents;
    std::vector<PreparedMidiEvent> startChaseNoteOnEvents;
};

struct PreparedMidiPlaybackPlan {
    double sampleRate = 0.0;
    int maximumBlockFrames = 0;
    int outputChannelCount = 0;
    std::uint64_t outputChannelMask = 0;
    std::int64_t playbackStartSample = 0;
    std::vector<PreparedMidiInstrumentSlot> instrumentSlots;
    std::vector<PreparedMidiEvent> events;
    std::vector<PreparedMidiEvent> initialChaseNoteOnEvents;
    std::optional<PreparedMidiLoop> loop;
};

struct PreparedMidiPlaybackPlanBuildRequest {
    Project projectSnapshot;
    double sampleRate = 0.0;
    int maximumBlockFrames = 0;
    int outputChannelCount = 0;
    std::uint64_t outputChannelMask = 0;
    std::int64_t playbackStartSample = 0;
    std::optional<PlaybackLoopRange> loopRange;
};

enum class PreparedMidiPlaybackPlanBuildFailureReason {
    None,
    Cancelled,
    InvalidSampleRate,
    InvalidMaximumBlockFrames,
    InvalidOutputFormat,
    InvalidPlaybackStart,
    InvalidLoopRange,
    InvalidTrackMix,
    SamplePositionOverflow,
    EventLimitExceeded,
    CallbackEventLimitExceeded
};

struct PreparedMidiPlaybackPlanBuildResult {
    PreparedMidiPlaybackPlanBuildFailureReason failureReason =
        PreparedMidiPlaybackPlanBuildFailureReason::None;
    std::unique_ptr<const PreparedMidiPlaybackPlan> plan;
};

PreparedMidiPlaybackPlanBuildResult buildPreparedMidiPlaybackPlan(
    PreparedMidiPlaybackPlanBuildRequest request,
    std::stop_token stopToken = {});

namespace detail {
struct PreparedMidiPlaybackPlanLimits {
    std::size_t maximumTotalEvents = 1'000'000;
    std::size_t maximumCallbackEvents = 4'096;
};
PreparedMidiPlaybackPlanBuildResult buildPreparedMidiPlaybackPlanWithLimits(
    PreparedMidiPlaybackPlanBuildRequest request,
    std::stop_token stopToken,
    PreparedMidiPlaybackPlanLimits limits);
}
```

**步骤：**

1. 在 `core_tests.cpp` 先加入 `#include "PreparedMidiPlaybackPlan.h"` 和非循环测试。固定 48 kHz 工程包含 tempo 变化、两条乐器轨、重叠同音高、同 sample Note Off/Note On，以及 muted、soloed、disabled、hidden 四种轨道状态；断言合法事件与 `round(project.tickToSeconds(tick) * 48000)` 相差不超过 1 sample，hidden 仍产生事件，其他播放规则沿用现有 `MidiPlayback` 语义，slot 逐字冻结 gain/pan。
2. 运行 `CORE_TEST`，记录 RED：编译必须因缺少 `PreparedMidiPlaybackPlan.h` 或声明而失败；若失败来自测试语法、环境或旧回归，先修正测试/环境，不能进入生产实现。
3. 实现非循环 builder。先按工程轨道顺序建立兼容乐器 slot；按稳定 `(trackId, clipId, noteId)` 首次出现顺序分配从 0 开始的稠密 `noteInstanceId`；使用带溢出检查的 `llround(tickToSeconds * sampleRate)`；排序键固定为 `(samplePosition, NoteOff-before-NoteOn, track order, clip order, note order)`。原始 Note On 早于 `playbackStartSample` 且 Note Off 晚于该起点的音符，只在 `initialChaseNoteOnEvents` 合成一次起播 Note On；非循环 chase 使用绝对 `playbackStartSample`，循环首次 chase 使用相对 `loopStartSample` 的起播偏移。起点之前的普通事件不进入可消费区，后续真实 Note Off 仍保留。所有普通、initial chase、loop boundary 和 loop-start chase 事件合并按稳定来源顺序分配全局唯一 `eventOrdinal`；builder 不保存字符串到计划。
4. 对非有限或负 gain、非有限或不在 `[-1, 1]` 的 pan 返回 `InvalidTrackMix`；不在 `[1, 2]` 的声道数、与声道数不一致的低两位 mask、非有限或不大于 0 的采样率，以及不大于 0 的最大 block 均返回对应稳定失败原因。
5. 再运行 `CORE_TEST`；随后运行完整 `ctest --test-dir build --output-on-failure` 和 `git diff --check`。
6. 只提交上述四个文件，提交信息：`feat: build immutable MIDI playback plans`。

**可直接粘贴的首个 RED：** 把函数加入 `core_tests.cpp` 并在现有 `main()` 测试调用表注册；首次失败应为 `fatal error C1083: Cannot open include file: 'PreparedMidiPlaybackPlan.h'`，不是运行期断言。

```cpp
void preparedMidiPlanOrdersNoteOffBeforeNoteOn()
{
    trackloom::Project project("Prepared");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Phrase", trackloom::ClipType::Midi, 0, 1920);
    require(clip.has_value(), "fixture clip should exist");
    require(project.createMidiNote(clip->id, 0, 960, 60, 100, 1).has_value(), "first note should exist");
    require(project.createMidiNote(clip->id, 960, 960, 64, 100, 1).has_value(), "second note should exist");

    trackloom::PreparedMidiPlaybackPlanBuildRequest request;
    request.projectSnapshot = project;
    request.sampleRate = 48000.0;
    request.maximumBlockFrames = 256;
    request.outputChannelCount = 2;
    request.outputChannelMask = 3;
    const auto result = trackloom::buildPreparedMidiPlaybackPlan(std::move(request));

    require(result.plan != nullptr, "valid project should build a plan");
    require(result.plan->events.size() == 4, "two notes should create four events");
    require(result.plan->events[1].samplePosition == 24000, "note off should land at tick 960");
    require(result.plan->events[1].type == trackloom::PreparedMidiEventType::NoteOff, "off must sort first");
    require(result.plan->events[2].type == trackloom::PreparedMidiEventType::NoteOn, "on must sort second");
}
```

**最小 GREEN 核心：** 先只让上述固定排序成立，再逐条补本任务其余 RED；排序实现固定使用完整 tuple，不能依赖 enum 当前整数值。

```cpp
std::stable_sort(events.begin(), events.end(), [](const auto& left, const auto& right) {
    return std::tuple(left.samplePosition, left.type == PreparedMidiEventType::NoteOn,
                      left.trackOrder, left.clipOrder, left.noteOrder)
         < std::tuple(right.samplePosition, right.type == PreparedMidiEventType::NoteOn,
                      right.trackOrder, right.clipOrder, right.noteOrder);
});
```

##### Task 2：补齐循环计划、取消和容量预算

**文件：**

- 修改 `src/core/PreparedMidiPlaybackPlan.cpp`
- 修改 `tests/core_tests.cpp`

**步骤：**

1. 先添加循环 RED：`[0, 3840)` 循环用 `start=3360,length=960` 的音符证明跨右边界只产生 boundary Off、不会在 tick 0 错误 chase；独立的 `[960, 3840)` 循环用 `start=480,length=960` 的音符证明循环起点已持续音会 chase。另覆盖从循环中点开始播放、恰好落在右边界的 Note Off、短到一个 512-frame block 可回绕多次的循环。断言正常 `events` 使用相对 `loopStartSample` 的 `[0, loopLengthSamples)` 坐标，边界释放只在 `boundaryNoteOffEvents`，每次回绕 chase 只在 `startChaseNoteOnEvents`，首次从循环中点起播的 chase 只在计划级 `initialChaseNoteOnEvents`，三张表沿用同一基础 `noteInstanceId`。
2. 添加预算 RED：通过 `detail::buildPreparedMidiPlaybackPlanWithLimits` 把测试总上限降为 8，验证第 9 个事件返回 `EventLimitExceeded`，避免单元测试构造百万字符串对象；另用真实生产入口和 4,097 个同窗口事件验证正式 callback 上限没有被测试注入绕过。非法/零长度循环、sample 换算溢出分别返回稳定原因；预先请求停止的 `stop_token` 返回 `Cancelled` 且 `plan == nullptr`。
3. 运行 `CORE_TEST`，记录断言 RED；此时接口已存在，预期失败必须来自尚未实现的 loop/budget/cancel 语义。
4. 实现循环规范化：把非负 `playbackStartSample` 用安全 floor-mod 归一到 `[loopStartSample, loopStartSample + loopLengthSamples)`，计划保存归一后的工程位置但请求 key 保留原始起点用于过期比较；计划只保存一轮事件。跨右边界音符在边界表产生 Note Off；在循环起点仍持续的音符在 chase 表产生 Note On；从归一后中点起播的持续音只进入 initial chase。运行排序后再分配稳定 ordinal。每处理一条轨道、片段或音符，以及进入容量较大的排序/密度扫描前检查 `stopToken.stop_requested()`。
5. 公共 `buildPreparedMidiPlaybackPlan` 固定调用 `detail` 实现的 1,000,000/4,096 正式上限，调用方不能传入更大的值。总事件计数包含 `events + initialChaseNoteOnEvents + boundaryNoteOffEvents + startChaseNoteOnEvents`。密度验证使用已准备 `maximumBlockFrames` 的半开 sample 窗口；循环情况把完整轮次数、余数窗口、边界表和 chase 表合并计数，覆盖一个 block 多次回绕，不能只检查单轮相邻事件。
6. 运行 `CORE_TEST`、完整 CTest 和 `git diff --check`；提交：`feat: validate loop-aware MIDI playback plans`。

**可直接粘贴的首个 RED：** 复用 Task 1 的 request 初始化，把 clip 长度设为 4800 tick 并加入 `start=3360,length=960` 的音符；首次运行应以 `loop boundary note off should be explicit` 断言失败。

```cpp
request.loopRange = trackloom::PlaybackLoopRange { 0, 3840 };
const auto result = trackloom::buildPreparedMidiPlaybackPlan(std::move(request));
require(result.plan != nullptr, "valid loop should build");
require(result.plan->loop.has_value(), "plan should retain one normalized loop");
require(result.plan->loop->loopStartSample == 0, "loop should start at sample zero");
require(result.plan->loop->loopLengthSamples == 96000, "one 120 BPM bar should be 96000 samples");
require(result.plan->loop->boundaryNoteOffEvents.size() == 1,
        "loop boundary note off should be explicit");
require(result.plan->loop->startChaseNoteOnEvents.empty(),
        "note that begins near loop end must not be chased at tick zero");
```

**最小 GREEN 核心：** public 入口不得接收 limits；只有测试 detail 入口能缩小门槛。

```cpp
return detail::buildPreparedMidiPlaybackPlanWithLimits(
    std::move(request),
    stopToken,
    detail::PreparedMidiPlaybackPlanLimits { 1'000'000, 4'096 });
```

##### Task 3：实现固定 16 voice 内置合成器

**文件：**

- 新建 `src/core/BuiltInPolySynth.h`
- 新建 `src/core/BuiltInPolySynth.cpp`
- 修改 `src/core/CMakeLists.txt`
- 修改 `tests/core_tests.cpp`

**公开接口：**

```cpp
struct BuiltInPolySynthVoiceKey {
    std::uint32_t noteInstanceId = 0;
    std::uint64_t loopIteration = 0;
    bool operator==(const BuiltInPolySynthVoiceKey&) const = default;
};

enum class BuiltInPolySynthEventOutcome {
    Applied,
    VoiceStolen,
    StaleNoteOff
};

struct BuiltInPolySynthVoiceSnapshot {
    bool active = false;
    BuiltInPolySynthVoiceKey key;
    std::uint64_t voiceStartSerial = 0;
    std::uint32_t eventOrdinal = 0;
};

class BuiltInPolySynth final {
public:
    static constexpr std::size_t voiceCount = 16;
    bool prepare(double sampleRate) noexcept;
    void reset() noexcept;
    BuiltInPolySynthEventOutcome noteOn(
        const PreparedMidiEvent& event,
        std::uint64_t loopIteration,
        const PreparedMidiInstrumentSlot& instrument) noexcept;
    BuiltInPolySynthEventOutcome noteOff(
        const PreparedMidiEvent& event,
        std::uint64_t loopIteration) noexcept;
    void releaseAll() noexcept;
    void render(
        float* const* outputChannels,
        int channelCount,
        int startFrame,
        int frameCount) noexcept;
    bool hasActiveVoices() const noexcept;
    BuiltInPolySynthVoiceSnapshot voiceSnapshot(std::size_t index) const noexcept;
};

namespace detail {
struct BuiltInPolySynthVoiceSelectionState {
    bool active = false;
    std::uint64_t voiceStartSerial = 0;
};
std::size_t selectBuiltInPolySynthVoiceToSteal(
    std::span<const BuiltInPolySynthVoiceSelectionState> voices) noexcept;
}
```

**步骤：**

1. 先写 RED，覆盖 44.1/48/96 kHz、64/256/512 frames、A4 稳态频率 `abs(measuredHz - 440.0) <= 1.0`、Note On 精确 sample、跨 block 相位连续、5/20/30 ms ADSR 各允许舍入后 1 sample 误差、同音高不同 key、不同 `loopIteration`、16 voice、最低空闲 index、最老 `voiceStartSerial` 窃取、相同 serial 防御性最低 index、迟到 Note Off、mono/stereo pan 和峰值 `<= 0.045 + 1e-6`。
2. 运行 `CORE_TEST`，记录因缺少 `BuiltInPolySynth.h`/API 的 RED。
3. 用 `std::array<Voice, 16>` 实现，不使用 `juce::Synthesiser`、容器增长或锁。MIDI 频率固定为 `440.0 * exp2((noteNumber - 69) / 12.0)`；attack/decay/release 帧数分别为 `ceil(sampleRate * 0.005/0.020/0.030)`；新音符 phase 从 0 开始；release 从触发时当前包络值线性降到 0。
4. 每 voice 最终幅度用 `min(0.045f, 0.045f * velocity / 127.0f * max(gain, 0.0f))` 限制；stereo 沿用现有线性 pan 规则，mono 忽略 pan。这样用户 gain 仍可在安全包络内放大弱力度，但任一 voice 不超过 0.045。
5. `noteOn` 先找最低空闲 index；无空闲时调用纯函数 `detail::selectBuiltInPolySynthVoiceToSteal` 选择最小 `voiceStartSerial`，并列选最低 index。测试可用手工 span 构造并列 serial，不需要破坏 synth 的单调 serial 不变量。窃取先使旧 key 失效，再写新 key；`noteOff` 只匹配完整 key，找不到时返回 `StaleNoteOff`。
6. 运行 `CORE_TEST`、完整 CTest、`git diff --check`；提交：`feat: add fixed built-in poly synth`。

**可直接粘贴的首个 RED：** 首次应因缺少 `BuiltInPolySynth.h` 失败；接口出现后若输出仍全零，断言文本必须原样显示。

```cpp
void builtInSynthRendersA4()
{
    trackloom::BuiltInPolySynth synth;
    require(synth.prepare(48000.0), "48 kHz should prepare");
    trackloom::PreparedMidiEvent event;
    event.type = trackloom::PreparedMidiEventType::NoteOn;
    event.noteNumber = 69;
    event.velocity = 127;
    trackloom::PreparedMidiInstrumentSlot slot;
    require(synth.noteOn(event, 0, slot) == trackloom::BuiltInPolySynthEventOutcome::Applied,
            "first note should use a free voice");
    std::array<float, 512> left {};
    std::array<float, 512> right {};
    float* outputs[] { left.data(), right.data() };
    synth.render(outputs, 2, 0, 512);
    require(std::any_of(left.begin(), left.end(), [](float value) { return value != 0.0f; }),
            "prepared note should render non-zero audio");
}
```

**最小 GREEN 核心：** voice 选择只扫描固定数组；不要在实现中建立 active-voice vector。

```cpp
std::size_t selected = voiceCount;
for (std::size_t index = 0; index < voices_.size(); ++index) {
    if (!voices_[index].active) { selected = index; break; }
}
```

##### Task 4：实现平台无关实时播放运行时与 host 契约

**文件：**

- 新建 `src/core/PreparedMidiPlaybackRuntime.h`
- 新建 `src/core/PreparedMidiPlaybackRuntime.cpp`
- 新建 `src/core/RealtimePlaybackHost.h`
- 修改 `src/core/CMakeLists.txt`
- 修改 `tests/core_tests.cpp`

**关键接口：**

```cpp
enum class RealtimePlaybackState : std::uint32_t {
    Stopped, Playing, Stopping, Faulted
};

enum class RealtimeAudioError : std::uint32_t {
    None, InvalidFormat, OversizedBlock, EventDensityExceeded,
    CallbackException, DeviceError
};

struct RealtimeAudioDiagnosticsSnapshot {
    RealtimePlaybackState state = RealtimePlaybackState::Stopped;
    RealtimeAudioError lastError = RealtimeAudioError::None;
    std::uint64_t callbackCount = 0;
    std::uint64_t callbackTimeoutCount = 0;
    std::uint64_t callbackExceptionCount = 0;
    std::uint64_t oversizedBlockCount = 0;
    std::uint64_t voiceStealCount = 0;
    std::uint64_t staleNoteOffCount = 0;
    std::uint64_t largestObservedBlockFrames = 0;
    std::uint64_t renderedSampleCount = 0;
    std::uint64_t loopIteration = 0;
    std::int64_t projectSamplePosition = 0;
};

class PreparedMidiPlaybackRuntime final {
public:
    bool installPlan(const PreparedMidiPlaybackPlan* plan);
    bool start() noexcept;
    bool requestStop() noexcept;
    void hardReset(RealtimeAudioError error = RealtimeAudioError::None) noexcept;
    void processBlock(float* const* outputs, int channels, int frames);
    void recordCallbackTimeout() noexcept;
    void recordCallbackException() noexcept;
    RealtimeAudioDiagnosticsSnapshot snapshot() const noexcept;
};

enum class PreparedMidiPlaybackPlanValidationFailureReason {
    None,
    InvalidFormat,
    InvalidInstrumentSlot,
    InvalidEventValue,
    UnsortedEvents,
    InvalidEventOrdinal,
    InvalidLoopTable,
    EventLimitExceeded,
    CallbackEventLimitExceeded,
    ValidationResourceUnavailable
};

struct PreparedMidiPlaybackPlanValidationResult {
    bool valid = false;
    PreparedMidiPlaybackPlanValidationFailureReason failureReason =
        PreparedMidiPlaybackPlanValidationFailureReason::None;
};

PreparedMidiPlaybackPlanValidationResult validatePreparedMidiPlaybackPlan(
    const PreparedMidiPlaybackPlan& plan);
```

`RealtimePlaybackHost.h` 同时定义 `AudioDeviceFormatSnapshot`（`generation/deviceId/deviceName/sampleRate/maximumBlockFrames/outputChannelCount/outputChannelMask/available`）、`RealtimePlaybackHostSnapshot`（设备格式、运行时诊断、后端 xrun）、稳定 host 失败枚举，以及仅供消息线程调用的抽象方法：

```cpp
struct AudioDeviceFormatSnapshot {
    std::uint64_t generation = 0;
    std::string deviceId;
    std::string deviceName;
    double sampleRate = 0.0;
    int maximumBlockFrames = 0;
    int outputChannelCount = 0;
    std::uint64_t outputChannelMask = 0;
    bool available = false;
};

struct RealtimePlaybackHostSnapshot {
    AudioDeviceFormatSnapshot format;
    RealtimeAudioDiagnosticsSnapshot realtime;
    int xRunCount = -1;
    bool deviceListRefreshPending = false;
};

enum class RealtimePlaybackHostFailureReason {
    None,
    DeviceUnavailable,
    DeviceNotOpen,
    DeviceFormatMismatch,
    PlaybackNotStopped,
    InvalidPlan,
    DeviceStartFailed
};

struct RealtimePlaybackHostResult {
    bool success = false;
    RealtimePlaybackHostFailureReason failureReason =
        RealtimePlaybackHostFailureReason::None;
    std::string message;
};

class RealtimePlaybackHost {
public:
    virtual ~RealtimePlaybackHost() = default;
    virtual AudioDeviceFormatSnapshot deviceFormatSnapshot() const = 0;
    virtual RealtimePlaybackHostResult installAndStart(
        std::unique_ptr<const PreparedMidiPlaybackPlan> plan) = 0;
    virtual bool requestStop() noexcept = 0;
    virtual void serviceNonRealtime() = 0;
    virtual void hardStopAndReset() noexcept = 0;
    virtual RealtimePlaybackHostSnapshot snapshot() const = 0;
};
```

**步骤：**

1. 先写运行时 RED：未播放清零、首次 block 在普通事件前只应用一次 initial chase、事件间分段渲染、实际 `frames` 推进、单/双声道与空指针、非循环结束、循环尾/头、恰好结束于右边界、一个 block 多次回绕、边界 Off 后下一轮 chase、连续 block 不重复两类 chase、相同基础 key 的跨轮 release 共存、`renderedSampleCount` 单调和 `projectSamplePosition` 回绕。
2. 再写防御 RED：手工计划包含越界 `instrumentSlotIndex`、无序 sample/ordinal、非法通道/音高/力度、普通事件越出非循环/循环坐标、boundary 表含 Note On、chase 表含 Note Off、重复 ordinal、格式或容量不一致时，validator 和正式 `installPlan()` 必须在 callback 启动前拒绝且保留旧的 `Stopped` 状态；其中静态可判定的单 callback 4,097 个事件也必须在安装前以稳定密度原因拒绝。随后单独验证 callback 的纵深防御：测试文件定义 header 仅前向声明并授予 friend 的 `trackloom::detail::PreparedMidiPlaybackRuntimeTestAccess`，直接装入一个 4,097-event 计划与测试游标、刻意绕过正式 validator；该 seam 不提供生产可调用函数、不增加 callback 条件分支，也不进入 `trackloom_core` 实现文件。经此 seam 调用 callback 时，必须在修改任何 voice 前整块静音并进入 `Faulted/EventDensityExceeded`。另验证超大 block 整块静音、记录 `largestObservedBlockFrames` 并进入 `Faulted`；事件密度/超大 block fault 均不推进事件游标或两个 sample 计数；`Playing → Stopping → Stopped` 最多渲染 `ceil(sampleRate * 0.030)` 帧；设备错误立即硬清；预热后 10,000 个受测 block 的全局 `operator new` 计数增量为 0。
3. 运行 `CORE_TEST`，记录缺少 runtime/host 声明的编译 RED。
4. `validatePreparedMidiPlaybackPlan()` 位于非实时线程，重新检查完整格式、事件值、slot 索引、三张表的类型/坐标、全局 ordinal 唯一且严格有序、总量和每 callback 密度；不得信任只有 builder 才能产生计划。ordinal 唯一性允许在这里分配临时 bitmap，但必须捕获分配失败并返回 `ValidationResourceUnavailable`。`installPlan()` 因此不是 callback API，也不声明 `noexcept`；它只允许 `Stopped`，并在写 plan 指针、voice 或游标前调用 validator，失败不能部分安装。
5. 实现时只把验证后的稳定 `const PreparedMidiPlaybackPlan*` 和固定游标装入 runtime。每个 block 先清所有非空有效输出，再预数事件；只有总数不超过 4,096 才修改 synth。相同 sample 严格按 plan ordinal 应用；循环边界先 Off、递增 iteration、再在下一有效头样本 chase。非循环计划在所有事件已消费且 synth voice 全部静音后自动发布 `Stopped`；空计划在首个 callback 立即停止，测试音必须依靠该规则自动结束。
6. 跨线程只发布 `std::atomic<std::uint32_t>`、`std::atomic<std::uint64_t>` 和 `std::atomic<std::int64_t>`；对三者加入 `is_always_lock_free` 静态断言。计划指针、voice 和游标不使用原子，因为只在设备 stop 已清空 pending callback 后替换。
7. 运行 `CORE_TEST`、完整 CTest、`git diff --check`；提交：`feat: add realtime prepared playback runtime`。

**可直接粘贴的首个 RED：** 首次应因缺少 runtime/validator 声明失败；若实现错误地信任手工计划，应显示下面的精确断言。

```cpp
void runtimeRejectsInvalidInstrumentSlotBeforeInstall()
{
    trackloom::PreparedMidiPlaybackPlan plan;
    plan.sampleRate = 48000.0;
    plan.maximumBlockFrames = 256;
    plan.outputChannelCount = 2;
    plan.outputChannelMask = 3;
    plan.instrumentSlots.push_back(trackloom::PreparedMidiInstrumentSlot {});
    plan.events.push_back(trackloom::PreparedMidiEvent {});
    plan.events.front().instrumentSlotIndex = 1;
    plan.events.front().velocity = 100;
    trackloom::PreparedMidiPlaybackRuntime runtime;

    const auto validation = trackloom::validatePreparedMidiPlaybackPlan(plan);
    require(!validation.valid, "out-of-range instrument slot must be rejected");
    require(validation.failureReason ==
                trackloom::PreparedMidiPlaybackPlanValidationFailureReason::InvalidInstrumentSlot,
            "invalid slot should keep a stable failure reason");
    require(!runtime.installPlan(&plan), "invalid plan must not be installed");
    require(runtime.snapshot().state == trackloom::RealtimePlaybackState::Stopped,
            "failed install must preserve stopped runtime");
}
```

**最小 GREEN 核心：** validation 必须发生在任何状态写入之前。

```cpp
if (plan == nullptr
    || state_.load(std::memory_order_acquire)
        != static_cast<std::uint32_t>(RealtimePlaybackState::Stopped))
    return false;
if (!validatePreparedMidiPlaybackPlan(*plan).valid)
    return false;
plan_ = plan;
return true;
```

callback 密度 fault 的测试不得调用上述正式安装入口。`PreparedMidiPlaybackRuntime.h` 只前向声明 `detail::PreparedMidiPlaybackRuntimeTestAccess` 并将其设为 friend；完整类型及其直接设置 `plan_`、游标和 `Playing` 状态的静态方法只定义在 `tests/core_tests.cpp`。这样同一组测试分别证明“正式入口静态拒绝 4,097 events”和“假定内部不变量被破坏时 callback 仍在触碰 synth 前静音并 fault”，不会制造发布代码可绕过 validator 的 API。

##### Task 5：直连 JUCE shared WASAPI 设备和 callback

**文件：**

- 新建 `src/platform/juce/JuceAudioHost.h`
- 新建 `src/platform/juce/JuceAudioHost.cpp`
- 修改 `src/platform/juce/CMakeLists.txt`
- 新建 `tests/support/FakeJuceAudioDeviceType.h`
- 新建 `tests/juce_audio_tests.cpp`
- 修改 `tests/CMakeLists.txt`

**平台接口：**

```cpp
struct JuceAudioOpenRequest {
    std::string outputDeviceName;
    double requestedSampleRate = 48000.0;
    int requestedBufferFrames = 256;
    int requestedOutputChannels = 2;
};

struct JuceAudioOutputDeviceInfo {
    std::string id;       // 首版为 "Windows Audio/" + JUCE 设备名
    std::string name;
    bool isDefault = false;
    std::vector<double> sampleRates;
    std::vector<int> bufferSizes;
    int maximumOutputChannels = 0;
};

enum class JuceAudioHostFailureReason {
    None,
    UnsupportedPlatform,
    DeviceTypeUnavailable,
    DeviceNotFound,
    DeviceCreateFailed,
    DeviceOpenFailed,
    PlaybackActive
};

struct JuceAudioHostResult {
    bool success = false;
    JuceAudioHostFailureReason failureReason = JuceAudioHostFailureReason::None;
    std::string message;
    std::string warning;
    AudioDeviceFormatSnapshot actualFormat;
};

using JuceAudioDeviceTypeFactory =
    std::function<std::unique_ptr<juce::AudioIODeviceType>()>;
using JuceRealtimeTickOperation = std::int64_t (*)() noexcept;
using JuceRealtimeBlockOperation = void (*)(
    PreparedMidiPlaybackRuntime&,
    float* const*,
    int,
    int);

class JuceAudioHost final :
    public RealtimePlaybackHost,
    private juce::AudioIODeviceCallback,
    private juce::AudioIODeviceType::Listener {
public:
    explicit JuceAudioHost(
        JuceAudioDeviceTypeFactory factory = {},
        JuceRealtimeTickOperation tickOperation = nullptr,
        std::int64_t ticksPerSecond = 0,
        JuceRealtimeBlockOperation blockOperation = nullptr);
    ~JuceAudioHost() override;
    std::vector<JuceAudioOutputDeviceInfo> refreshOutputDevices();
    JuceAudioHostResult openOutput(const JuceAudioOpenRequest& request);
    void close() noexcept;
    JuceAudioHostResult playTestTone();
    AudioDeviceFormatSnapshot deviceFormatSnapshot() const override;
    RealtimePlaybackHostResult installAndStart(
        std::unique_ptr<const PreparedMidiPlaybackPlan> plan) override;
    bool requestStop() noexcept override;
    void serviceNonRealtime() override;
    void hardStopAndReset() noexcept override;
    RealtimePlaybackHostSnapshot snapshot() const override;

private:
    void audioDeviceIOCallbackWithContext(
        const float* const* inputs,
        int inputChannelCount,
        float* const* outputs,
        int outputChannelCount,
        int frames,
        const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceError(const juce::String& error) override;
    void audioDeviceListChanged() override;
};
```

**步骤：**

1. 在共享的 `tests/support/FakeJuceAudioDeviceType.h` 建 `FakeAudioIODeviceType` 和 `FakeAudioIODevice`，记录 scan/create/open/start/stop/close 顺序、open 的 input/output mask、pending callback、实际格式和 xrun；Task 5 与 Task 8 的 JUCE 测试都复用它。先测试默认 factory 只产生 `Windows Audio` type（不要求真实设备存在），其余测试全部使用 fake，覆盖输出枚举、默认设备、0 input、1/2 output、协商后实际格式、format generation、stop 返回后才能换计划、`runtime.start()` 发生在 `device.start()` 前、`device.isPlaying()==false` 的失败回滚、设备重启、`audioDeviceAboutToStart` 报告意外格式变化、列表变化通知只置请求标志、消息线程 service 发现当前设备被移除后 stop/reset/plan 失效/generation 递增、`audioDeviceError` 只置稳定错误、测试音只在工程播放停止时允许。再注入无分配的函数指针 fake tick，验证 callback 耗时恰好等于 deadline 时 timeout 计数加一；注入会抛异常的 block function pointer，验证异常不逃出 callback、输出重新全零且 exception 计数加一。
2. 修改 `tests/CMakeLists.txt` 建立独立 `trackloom_juce_audio_tests` 和同名 CTest；运行 `JUCE_TEST`，记录缺少 `JuceAudioHost.h`/target 的 RED。
3. 默认 factory 只调用 `juce::AudioIODeviceType::createAudioIODeviceType_WASAPI(juce::WASAPIDeviceMode::shared)`；不枚举其他 type。`src/platform/juce/CMakeLists.txt` 为相关 target 定义 `JUCE_WASAPI=1`、`JUCE_DIRECTSOUND=0`、`JUCE_ASIO=0` 并保留 `/utf-8`。
4. 每次 scan 后按名称 create 设备；查询可选采样率、buffer 和声道时只使用尚未 start 的临时 device。`open` 传空 input mask 和前 1/2 个 output bits；`Playing/Stopping` 时返回 `PlaybackActive`，不得暗中硬停。请求格式失败时只尝试该设备声明的默认/首个可用 shared 格式，并把实际偏差作为结构化 warning 返回；成功后读取实际 sample rate、buffer、active output mask，任一格式字段或设备实例变化都递增 generation。
5. `installAndStart` 先调用 `validatePreparedMidiPlaybackPlan()`，再比较 plan 携带的四个数值格式字段（sample rate、maximum block、output channel count、output mask）与当前设备快照，并在停止设备前确认 runtime 已是 `Stopped`；device instance/id/name/available 和 generation 不进入平台无关 plan，由 Task 6 controller 在调用 host 前用完整 `deviceFormatGeneration` 复核。任一检查失败都不得 stop/start 或替换旧 plan，尤其不能通过提前 stop 截断 `Stopping` 尾音。全部一致时严格执行 `device.stop()` 清空可能残留的 pending callback、再次确认 `Stopped`、替换拥有的 plan、`runtime.installPlan()`、`runtime.start()`、`device.start(this)`。JUCE `start()` 没有返回值，因此返回后必须检查 `device.isPlaying()`；false 时立即 `device.stop()`、runtime hard reset、释放失败的新 plan 并返回 `DeviceStartFailed`。`audioDeviceAboutToStart` 若观察到与已核对快照不同的采样率、block 或声道，只发布数值型 format-mismatch 原子；`start()` 返回后若该标志已置位，同样 stop/reset 并返回 `DeviceFormatMismatch`，后续不得放行 callback。正常 callback 用 `float* const*` 和默认/测试 block function pointer 调用 runtime，不构造 `AudioBlock/MidiBuffer/String`；入口清零、以默认 `juce::Time::getHighResolutionTicks()` 或测试函数指针计时并 `catch (...)`，达到当前 block 实时期限时调用 `recordCallbackTimeout()`，异常时再次清零整块、调用 `recordCallbackException()` 并以 `CallbackException` 硬重置。
6. `audioDeviceError` 与 `audioDeviceListChanged` 只写 lock-free 原子；`serviceNonRealtime()` 在消息线程读取错误、xrun 和 runtime 终止状态。收到列表变化后必须重新 scan；若当前输出名已不存在，立即 `device.stop()`、hard reset 为 `DeviceError`、close/reset 当前 device 和 plan，把格式设为 `available=false` 并递增 generation，不能保留旧计划等待设备同名重现。发生其他 device/runtime fault 时同样必须先 `device.stop()` 等 pending callback 清空，随后才 hard reset/close；若发现更大的 `largestObservedBlockFrames`，把 prepared maximum 更新为该值、递增完整 format generation，并要求重新构建计划。`close()` 固定为 `device.stop()`、runtime hard reset、`device.close()`、释放 plan/device；device type 和 listener 保留到 host 析构，确保关闭输出后仍可重新枚举和打开。
7. `playTestTone()` 在非实时线程构造 440 Hz、200 ms Note On + 30 ms release 的临时计划，仍通过同一 runtime/stop 协议，最大 voice 幅度 0.02；不修改工程或播放头。
8. 运行 `JUCE_TEST`，再运行现有 `trackloom_juce_tests`、完整 CTest 和 `git diff --check`；提交：`feat: host shared WASAPI audio output`。

**可直接粘贴的首个 RED：** `FakeJuceAudioDeviceType` 默认提供一个名为 `Fake Speakers`、48 kHz、256 frames、2 outputs 的设备，并公开最后一次 open 的 input/output channel count；首次应因缺少两个新 header 失败。

```cpp
void juceAudioHostOpensOutputOnlySharedDevice()
{
    trackloom::test::FakeJuceAudioDeviceType* observedType = nullptr;
    trackloom::JuceAudioHost host([&]() -> std::unique_ptr<juce::AudioIODeviceType> {
        auto type = std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
        observedType = type.get();
        return type;
    });
    const auto devices = host.refreshOutputDevices();
    require(devices.size() == 1, "fake output should be enumerated");
    const auto opened = host.openOutput({ "Fake Speakers", 48000.0, 256, 2 });
    require(opened.success, "supported fake format should open");
    require(observedType->lastOpenInputChannelCount() == 0, "audio milestone must request zero inputs");
    require(observedType->lastOpenOutputChannelCount() == 2, "audio milestone should request stereo");
    require(host.deviceFormatSnapshot().generation == 1, "first device instance should advance generation");
}
```

**最小 GREEN 核心：** 正式 factory 只有下面一个分支；fake factory 不进入正式构造路径。

```cpp
std::unique_ptr<juce::AudioIODeviceType> createSharedWasapiType()
{
    return std::unique_ptr<juce::AudioIODeviceType>(
        juce::AudioIODeviceType::createAudioIODeviceType_WASAPI(
            juce::WASAPIDeviceMode::shared));
}
```

##### Task 6：增加工程 generation 和可取消异步播放准备

**文件：**

- 修改 `src/app/AppProjectSession.h`
- 修改 `src/app/AppProjectSession.cpp`
- 修改 `src/app/AppPlaybackActions.h`
- 修改 `src/app/AppPlaybackActions.cpp`
- 修改 `src/app/AppMainMenu.cpp`
- 修改 `src/app/juce/TrackLoomApplication.cpp`
- 修改 `tests/app_support_tests.cpp`
- 修改 `tests/CMakeLists.txt`

**应用接口：**

```cpp
struct AppProjectPlaybackSnapshot {
    Project project;
    std::uint64_t projectEditGeneration = 0;
};

std::uint64_t AppProjectSession::projectEditGeneration() const noexcept;
AppProjectPlaybackSnapshot AppProjectSession::capturePlaybackSnapshot() const;

enum class AppPlaybackState {
    Unavailable, Stopped, Preparing, Playing, Stopping, Faulted
};

enum class AppPlaybackFailureReason {
    None,
    NoAudioDevice,
    PreparationCancelled,
    PreparationFailed,
    StalePreparation,
    HostRejected,
    DeviceFault
};

struct AppPlaybackActionFeedback {
    bool success = false;
    AppPlaybackActionFeedbackKind kind = AppPlaybackActionFeedbackKind::PrepareFailed;
    AppPlaybackFailureReason failureReason = AppPlaybackFailureReason::None;
    std::string message;
};

struct AppPlaybackStatus {
    AppPlaybackState state = AppPlaybackState::Stopped;
    AppPlaybackFailureReason failureReason = AppPlaybackFailureReason::None;
    bool deviceAvailable = false;
    bool canStart = false;
    std::int64_t projectSamplePosition = 0;
    std::uint64_t renderedSampleCount = 0;
    double projectSeconds = 0.0;
    std::string stateLabel;
    std::string summary;
};

struct AppPlaybackPreparationKey {
    std::uint64_t projectEditGeneration = 0;
    std::uint64_t deviceFormatGeneration = 0;
    std::int64_t playbackStartSample = 0;
    std::optional<PlaybackLoopRange> loopRange;
    bool operator==(const AppPlaybackPreparationKey&) const = default;
};

using AppPreparedPlanBuildOperation = std::function<
    PreparedMidiPlaybackPlanBuildResult(
        PreparedMidiPlaybackPlanBuildRequest,
        std::stop_token)>;

class AppPlaybackController final {
public:
    explicit AppPlaybackController(
        RealtimePlaybackHost& host,
        AppPreparedPlanBuildOperation build = buildPreparedMidiPlaybackPlan);
    AppPlaybackActionFeedback start(
        AppProjectPlaybackSnapshot project,
        std::optional<PlaybackLoopRange> loopRange = std::nullopt);
    AppPlaybackActionFeedback stop();
    AppPlaybackActionFeedback rewindToStart();
    void poll(const AppProjectSession& session);
    AppPlaybackStatus status() const;
    bool isPlaying() const noexcept;
    std::int64_t currentSample() const noexcept;
    double currentSeconds() const noexcept;
};

AppPlaybackActionFeedback startAppPlayback(
    AppPlaybackController& playback,
    const AppProjectSession& session,
    std::optional<PlaybackLoopRange> loopRange = std::nullopt);
AppPlaybackActionFeedback stopAppPlayback(AppPlaybackController& playback);
AppPlaybackActionFeedback toggleAppPlayback(
    AppPlaybackController& playback,
    const AppProjectSession& session);
AppPlaybackActionFeedback rewindAppPlaybackToStart(
    AppPlaybackController& playback);
```

**步骤：**

1. 先写 generation RED：初值固定；`editProject()`、成功 command、undo、redo、新建、成功 open 各递增一次；失败 command/undo/redo/open、save/saveAs 不递增。`capturePlaybackSnapshot()` 返回与同一 generation 对应的独立 Project 副本。
2. 再用 `FakeRealtimePlaybackHost`、`std::latch` 控制的 fake builder 写异步 RED：`Unavailable/Stopped → Preparing → Playing → Stopping → Stopped` 与 host fault → `Faulted`；Preparing 期间测试线程可立即查询状态；stop 请求取消；旧工程 generation、旧完整设备 format generation、旧 start sample、旧 loop 均丢弃；过期/失败结果不调用 install；播放中编辑不替换当前计划；停止后重播使用新 generation；无设备或 host 正在播放测试音时 `canStart == false`。每条失败路径断言稳定 `AppPlaybackFailureReason`，不解析 message。
3. 运行 `APP_TEST`，记录缺少 generation、新构造函数或状态 API 的编译 RED。
4. generation 用 `std::uint64_t` 仅在消息线程修改。`editProject()` 在返回可写引用前保守递增；成功 command/undo/redo、新建/open 在实际替换完成后递增；保存不改变工程内容，不递增。
5. controller 用一个 `std::jthread` 执行同步 builder；worker 只持有工程副本、格式值和 key。完成结果写入受 mutex 保护的单槽 mailbox 后标记完成；`poll()` 才 join 已完成线程、复核四项 key、调用 host。Preparing 中 stop 只 `request_stop()` 并进入 `Stopping`；worker 回报或确认退出后才转 `Stopped`，期间拒绝第二个 start。按钮 handler 不得等待仍运行的 worker；controller 析构时才允许 request-stop 后 join，以保证工程副本和 mailbox 生命周期。
6. 把 `app_support_tests.cpp` 中所有旧的默认构造 `AppPlaybackController` 改为显式注入同一个可控 fake host；保留 `AppMainMenu.cpp` 当前编译所需的 `isPlaying()/currentSample()/currentSeconds()` 只读便利方法，旧 `prepared` 断言迁移到新的结构化 `AppPlaybackStatus`。旧播放语义测试必须改为断言状态和 host 交互，不能保留 UI 静音 block 模拟器作为兼容后门。
7. `AppMainMenu` 的 Play enabled 只读取 `status.canStart`；Preparing/Playing 时 Stop enabled，Unavailable/Faulted/Stopping 时按稳定状态禁用。`poll()` 先调用 `host.serviceNonRealtime()`；host 进入 `Stopped/Faulted` 时更新应用状态。播放中 rewind 先进入 `Stopping`，停止完成后把起点设为 0 并重新准备；UI `Timer` 不再调用 `advanceOneUiBlock`。
8. 在本任务内同步做 JUCE 壳的最小编译迁移，不能等 Task 8：先声明 `JuceAudioHost audioHost_`，再用 `AppPlaybackController playback_ { audioHost_ }`；所有 start/toggle 改传 `session_`，stop/rewind 使用新签名，`timerCallback()` 只调用 `playback_.poll(session_)` 和刷新。此步不增加设置窗口，设备尚未打开时 UI 只显示 Unavailable。
9. 同时建立无人值守烟测：`initialise(commandLine)` 识别 `--hidden-smoke-test`，不显示窗口并在 250 ms 后从消息线程调用 `JUCEApplicationBase::quit()`；`tests/CMakeLists.txt` 注册 `trackloom_app_hidden_smoke_tests` 和 `TIMEOUT 10`。Task 8 抽取主组件时必须保留该既有行为，不重新发明第二套入口。
10. 运行 `APP_TEST`、相关 core/JUCE CTest，构建 `trackloom_app` 并运行 `trackloom_app_hidden_smoke_tests`，再运行 `git diff --check`；提交：`app: coordinate asynchronous audio preparation`。

**可直接粘贴的首个 RED：** 首次应因缺少 `projectEditGeneration()` 失败；若失败操作错误递增，第二个断言给出精确原因。

```cpp
void projectPlaybackGenerationTracksOnlyPossibleContentChanges()
{
    trackloom::AppProjectSession session;
    const auto initial = session.projectEditGeneration();
    session.editProject().rename("Changed");
    require(session.projectEditGeneration() == initial + 1,
            "editable project access should conservatively advance generation once");
    const auto afterEdit = session.projectEditGeneration();
    require(!session.undoProjectEdit(), "direct edit should clear command history");
    require(session.projectEditGeneration() == afterEdit,
            "failed undo must not advance project generation");
    const auto snapshot = session.capturePlaybackSnapshot();
    require(snapshot.projectEditGeneration == afterEdit, "snapshot and project copy must share one generation");
    require(snapshot.project.name() == "Changed", "snapshot should copy the matching project state");
}
```

**最小 GREEN 核心：** 所有递增集中到一个消息线程 helper，保存路径不得调用它。

```cpp
void AppProjectSession::advanceProjectEditGeneration() noexcept
{
    ++projectEditGeneration_;
}
```

##### Task 7：增加本机音频设置、菜单和命令分发

**文件：**

- 新建 `src/app/AppAudioSettings.h`
- 新建 `src/app/AppAudioSettings.cpp`
- 修改 `src/app/CMakeLists.txt`
- 修改 `src/app/AppMainMenu.h`
- 修改 `src/app/AppMainMenu.cpp`
- 修改 `src/app/AppCommandDispatcher.h`
- 修改 `src/app/AppCommandDispatcher.cpp`
- 修改 `tests/app_support_tests.cpp`

**设置格式和命令：**

```cpp
struct AppAudioSettings {
    std::string outputDeviceName;
    double requestedSampleRate = 48000.0;
    int requestedBufferFrames = 256;
    int requestedOutputChannels = 2;
};

enum class AppAudioSettingsLoadKind { Loaded, Missing, Invalid };
struct AppAudioSettingsLoadResult {
    AppAudioSettingsLoadKind kind = AppAudioSettingsLoadKind::Missing;
    AppAudioSettings settings;
    std::string warning;
};
AppAudioSettingsLoadResult loadAppAudioSettings(const std::filesystem::path& path);
bool saveAppAudioSettings(
    const AppAudioSettings& settings,
    const std::filesystem::path& path);
using AppAudioSettingsLoadOperation = std::function<
    AppAudioSettingsLoadResult(const std::filesystem::path&)>;
using AppAudioSettingsSaveOperation = std::function<
    bool(const AppAudioSettings&, const std::filesystem::path&)>;
```

`AppMainMenuCommand::OpenAudioSettings = 1303`、`AppCommandKind::OpenAudioSettings` 和 `AppCommandHandlers::openAudioSettings` 使用同一稳定映射；“工具”组显示“音频设置…”。诊断信息放在同一设置窗口，不再增加第二个重复命令。

**步骤：**

1. 先写设置 RED：缺失文件返回默认值与 `Missing`；合法 UTF-8 设备名往返；坏版本、非有限采样率、非正 buffer、非 1/2 声道返回默认值与 `Invalid`；保存失败不破坏内存设置。磁盘格式固定五行：`trackloom_audio_settings 1`、`output_device <std::quoted UTF-8>`、`sample_rate <double>`、`buffer_frames <int>`、`output_channels <int>`。
2. 先写菜单/dispatcher RED：工具菜单存在且 enabled；command id 1303 映射正确；handler 被调用一次；缺 handler 返回 `MissingHandler`；命令面板能搜索“音频设置”。
3. 运行 `APP_TEST`，记录缺少设置类型/enum 的编译 RED。
4. 复用 `AppShortcutSettings` 的本机设置目录和容错风格，但文件固定为用户应用数据目录 `TrackLoom/audio-settings.txt`，不得写入工程或命令历史。
5. 运行 `APP_TEST`、完整 CTest、`git diff --check`；提交：`app: add local audio settings command`。

**可直接粘贴的首个 RED：** 使用现有测试临时目录 helper 生成 `missing-audio-settings.txt`；首次应因 `AppAudioSettings.h` 不存在而编译失败。

```cpp
void missingAudioSettingsUseDocumentedDefaults()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "missing-audio-settings.txt";
    const auto loaded = trackloom::loadAppAudioSettings(path);
    require(loaded.kind == trackloom::AppAudioSettingsLoadKind::Missing,
            "missing settings should have a stable load kind");
    require(loaded.settings.requestedSampleRate == 48000.0,
            "missing settings should request 48 kHz");
    require(loaded.settings.requestedBufferFrames == 256,
            "missing settings should request 256 frames");
    require(loaded.settings.requestedOutputChannels == 2,
            "missing settings should request stereo");
    require(trackloom::appMainMenuCommandId(
                trackloom::AppMainMenuCommand::OpenAudioSettings) == 1303,
            "audio settings command id must stay stable");
}
```

**最小 GREEN 核心：** missing 与 invalid 都返回同一默认 settings，但 `kind` 和中文 warning 必须不同。

```cpp
if (!std::filesystem::exists(path))
    return { AppAudioSettingsLoadKind::Missing, AppAudioSettings {}, {} };
```

##### Task 8：接入可测试的专用 JUCE 设置界面与真实播放控制

**文件：**

- 新建 `src/app/juce/AudioSettingsComponent.h`
- 新建 `src/app/juce/AudioSettingsComponent.cpp`
- 新建 `src/app/juce/TrackLoomMainComponent.h`
- 新建 `src/app/juce/TrackLoomMainComponent.cpp`
- 修改 `src/app/CMakeLists.txt`，新增 `trackloom_app_juce_support` library
- 修改 `src/app/juce/TrackLoomApplication.cpp`
- 新建 `tests/app_juce_audio_tests.cpp`
- 修改 `tests/CMakeLists.txt`

**可测试 JUCE 边界：**

```cpp
inline constexpr auto audioDeviceSelectorComponentId = "trackloom-audio-device";
inline constexpr auto audioSampleRateSelectorComponentId = "trackloom-audio-sample-rate";
inline constexpr auto audioBufferSelectorComponentId = "trackloom-audio-buffer";
inline constexpr auto audioChannelsSelectorComponentId = "trackloom-audio-channels";
inline constexpr auto audioApplyButtonComponentId = "trackloom-audio-apply";
inline constexpr auto audioTestToneButtonComponentId = "trackloom-audio-test-tone";
inline constexpr auto mainNewButtonComponentId = "trackloom-main-new";
inline constexpr auto mainOpenButtonComponentId = "trackloom-main-open";
inline constexpr auto mainSaveButtonComponentId = "trackloom-main-save";
inline constexpr auto mainPlayButtonComponentId = "trackloom-main-play";
inline constexpr auto mainPlaybackStatusComponentId = "trackloom-main-playback-status";

struct AudioSettingsComponentCallbacks {
    std::function<void(const AppAudioSettings&)> settingsApplied;
    std::function<void(std::string)> feedback;
};

class AudioSettingsComponent final : public juce::Component {
public:
    AudioSettingsComponent(
        JuceAudioHost& host,
        AppAudioSettings initialSettings,
        AudioSettingsComponentCallbacks callbacks = {});
    void refreshFromHost();
    AppAudioSettings selectedSettings() const;
    bool applySelectedSettings();
    bool triggerTestTone();
};

struct TrackLoomMainComponentDependencies {
    std::unique_ptr<JuceAudioHost> audioHost;
    AppPreparedPlanBuildOperation buildOperation = buildPreparedMidiPlaybackPlan;
    std::filesystem::path audioSettingsPath;
    AppAudioSettingsLoadOperation loadAudioSettings = loadAppAudioSettings;
    AppAudioSettingsSaveOperation saveAudioSettings = saveAppAudioSettings;
    std::function<void(std::string)> titleChanged;
    std::function<void(std::unique_ptr<AudioSettingsComponent>)>
        presentAudioSettings;
};

class TrackLoomMainComponent final
    : public juce::Component
    , public juce::MenuBarModel
    , private juce::Timer
    , private juce::KeyListener {
public:
    explicit TrackLoomMainComponent(TrackLoomMainComponentDependencies dependencies);
    AppCommandDispatchResult dispatchCommand(int commandId);
    void serviceUiTimer();
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex(int index, const juce::String& name) override;
    void menuItemSelected(int commandId, int topLevelMenuIndex) override;
    bool keyPressed(const juce::KeyPress& key) override;
    bool keyPressed(const juce::KeyPress& key, juce::Component* origin) override;
    void resized() override;

private:
    void timerCallback() override;
};
```

**步骤：**

1. 先建 `trackloom_app_juce_tests` RED。用 `juce::ScopedJuceInitialiser_GUI` 和 Task 5 fake device type 实例化 `AudioSettingsComponent`；通过上述固定 component id 取得真实 ComboBox/Button，断言只显示 `Windows Audio` shared 输出、设备/采样率/buffer/1–2 声道控件，播放中 Apply/测试音禁用，Stopped 时 `triggerTestTone()` 只调用 host 且不改变外部 session generation、dirty、command history 或 playback start。
2. 再用 fake-device-backed `JuceAudioHost`、`std::latch` 阻塞的 `buildOperation`、返回 `Invalid` 的 `loadAudioSettings` 和 `presentAudioSettings` lambda 写 `TrackLoomMainComponent` 装配 RED：`dispatchCommand(1303)` 恰好交付一个设置组件；设置加载失败回退默认并返回中文 warning；通过固定 main component id 验证无设备时 Play disabled 但 New/Open/Save 仍 enabled；阻塞 builder 时状态 label 稳定显示“正在准备音频…”；连续调用 `serviceUiTimer()` 后只 poll，fake device 没收到 callback 渲染或 Transport 推进。
3. 运行新 target，记录缺少 `AudioSettingsComponent.h`/JUCE support library 的编译 RED。
4. `trackloom_app_juce_support` 链接 `trackloom_app_support`、`trackloom_juce`、`juce::juce_gui_basics`；把当前私有主组件及其直接使用的 JUCE-only helper 类、常量和转换函数从 `TrackLoomApplication.cpp` 移到可链接的 `TrackLoomMainComponent.{h,cpp}`，应用文件只保留 Application/MainWindow bootstrap，不能复制两份实现。设置组件使用普通 ComboBox/Label/Button 自行展示 shared WASAPI，不使用 `AudioDeviceSelectorComponent`。应用启动加载设置并打开/回退设备；播放或停止尾音期间禁用设备 Apply 和测试音，只有 `Stopped` 才能改变设备格式。应用关闭先取消 worker、停止 host、关闭设备，再销毁窗口。
5. `TrackLoomMainComponentDependencies::audioHost` 必须非空；所有 operation 为空时构造函数补正式默认值。组件内部按“host 先声明、controller 后声明”的顺序持有，使 `JuceAudioHost` 先于引用它的 `AppPlaybackController` 构造、后于 controller 销毁。New/Open/Save/Play 按钮和播放状态 label 在构造时设置上述固定 ID。Play/Stop/Rewind、Space 和菜单继续走 `AppPlaybackActions/AppCommandDispatcher`，不能从 JUCE handler 直接 start/stop `AudioIODevice`。
6. `timerCallback()` 只调用 controller poll、读取 host snapshot、刷新文字；诊断显示设备名、实际 sample rate、buffer、声道、callback、timeout、oversized block、xrun（`-1` 显示“后端未提供”）、voice stealing 和 stale Note Off。
7. 抽取后保留 Task 6 已建立的 `--hidden-smoke-test`：构造真实主组件但不显示窗口，并继续用 `juce::Timer::callAfterDelay(250, [] { juce::JUCEApplicationBase::quit(); })` 从消息线程自动退出。`trackloom_app_hidden_smoke_tests` 继续使用 `$<TARGET_FILE:trackloom_app> --hidden-smoke-test` 和 `TIMEOUT 10`；普通启动不设置自动退出。
8. 构建 `trackloom_app_juce_tests trackloom_app`，运行 `trackloom_app_juce_tests`、现有 JUCE tests、`trackloom_app_hidden_smoke_tests` 和完整 CTest；执行下面的额外 PowerShell 进程烟测并运行 `git diff --check`。提交：`app: connect realtime audio controls and settings UI`。

   ```powershell
   $process = Start-Process 'build\src\app\trackloom_app.exe' -ArgumentList '--hidden-smoke-test' -WindowStyle Hidden -PassThru
   if (-not $process.WaitForExit(10000) -or $process.ExitCode -ne 0) {
       throw 'trackloom_app hidden smoke test failed'
   }
   ```

**可直接粘贴的首个 RED：** 首次应因缺少 `AudioSettingsComponent.h` 失败；component id 错误时必须显示下面的精确断言。

```cpp
void audioSettingsComponentExposesDedicatedSharedWasapiControls()
{
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    trackloom::JuceAudioHost host([]() -> std::unique_ptr<juce::AudioIODeviceType> {
        return std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
    });
    trackloom::AudioSettingsComponent component(host, trackloom::AppAudioSettings {});
    component.refreshFromHost();
    require(dynamic_cast<juce::ComboBox*>(
                component.findChildWithID(trackloom::audioDeviceSelectorComponentId)) != nullptr,
            "dedicated audio device selector should be test-visible");
    require(dynamic_cast<juce::TextButton*>(
                component.findChildWithID(trackloom::audioTestToneButtonComponentId)) != nullptr,
            "dedicated test tone button should be test-visible");
}
```

**最小 GREEN 核心：** IDs 在控件加入组件时设置，测试不得依赖 child index 或中文 label。

```cpp
deviceSelector_.setComponentID(audioDeviceSelectorComponentId);
testToneButton_.setComponentID(audioTestToneButtonComponentId);
addAndMakeVisible(deviceSelector_);
addAndMakeVisible(testToneButton_);
```

##### Task 9：固定参考工程、硬件烟测与阶段证据

**文件：**

- 新建 `tests/fixtures/audio/minimum-audible-midi/reference.trackloom`
- 新建 `tests/fixtures/audio/minimum-audible-midi/expected-events.tsv`
- 新建 `tests/fixtures/audio/minimum-audible-midi/expected-audio.properties`
- 新建 `tests/fixtures/audio/minimum-audible-midi/SHA256SUMS`
- 新建 `tests/fixtures/audio/minimum-audible-midi/README.md`
- 新建 `tests/verify_audio_fixture_hashes.cmake`
- 新建 `tests/juce_audio_hardware_smoke_tests.cpp`
- 新建 `tests/evidence/audio/README.md`
- 新建 `tests/evidence/audio/run-template.md`
- 修改 `tests/core_tests.cpp`
- 修改 `tests/CMakeLists.txt`

**固定清单：** `reference.trackloom` 使用 120 BPM、4/4 和一条居中 unity-gain 乐器轨；工程格式本身不持久化 loop，所以 `README.md` 另外固定 builder 请求为 `sample_rate=48000`、`maximum_block_frames=256`、`output_channels=2`、`output_channel_mask=3`、`playback_start_sample=0`、`loop_start_tick=0`、`loop_end_tick=3840`。四个音符依次为 C4/E4/G4/C5，起点 0/960/1920/2880 tick、长度各 480 tick、velocity 96、channel 1。48 kHz 预期 Note On sample 为 0/24000/48000/72000，Note Off 为 12000/36000/60000/84000；loop 长度为 96000 samples，允许误差均为 1 sample。

`expected-events.tsv` 首行固定为 `table\tsample_position\ttype\tnote_instance_id\tevent_ordinal\tinstrument_slot\tchannel\tnote\tvelocity`；本 fixture 的 8 行均属于 `events` 表，按 sample 与 Note Off-before-Note On 排序，boundary/initial-chase/loop-start-chase 三表为空。`expected-audio.properties` 固定 `render_frames=96000`、`render_block_frames=256`、`non_silent=true`、`rms_min=0.0001`、`peak_max=0.8`、`frequency_reference_note=60`、`frequency_reference_hz=261.625565`、`frequency_window_start_sample=2400`、`frequency_window_end_sample=9600`、`frequency_tolerance_hz=1.0`；频率只在 C4 已进入 sustain 且尚未 Note Off 的固定窗口内按同方向过零间距测量。

**步骤：**

1. 先加 fixture 驱动 RED：核心测试从磁盘打开 reference，按 README 的精确请求构建 plan，逐字段比较 `expected-events.tsv`；再把同一计划通过 `PreparedMidiPlaybackRuntime` 离线渲染 96000 frames，测量非零样本、RMS、绝对峰值和第一段 C4 稳态过零频率并逐项比较 properties 容差。在文件尚不存在时记录 RED。`tests/CMakeLists.txt` 给该测试设置 `${CMAKE_SOURCE_DIR}` 工作目录，避免从 build 目录解析相对路径。
2. 创建上述固定内容，使用 `Get-FileHash -Algorithm SHA256` 生成签名。`verify_audio_fixture_hashes.cmake` 用 CMake `file(SHA256 ...)` 逐项核对 `SHA256SUMS` 中的 reference、expected-events、expected-audio 和 README；在 `tests/CMakeLists.txt` 注册独立 `trackloom_audio_fixture_hash_tests`，不得为了校验 fixture 在 core 引入新的加密库。再运行 `CORE_TEST` 和该 hash CTest 取得 GREEN。
3. 新建 `trackloom_audio_hardware_smoke_tests`。runner 在构造 `JuceAudioHost` 前于 console 线程调用 `CoInitializeEx(nullptr, COINIT_MULTITHREADED)`；成功初始化时以 RAII 对称 `CoUninitialize()`，`RPC_E_CHANGED_MODE` 则稳定记录已有 apartment、继续构造 host 且不得 `CoUninitialize()`。未设置 `TRACKLOOM_AUDIO_HARDWARE_SMOKE=1` 时返回 77；启用时读取 `TRACKLOOM_AUDIO_OUTPUT_NAME`（空则默认设备）和 `TRACKLOOM_AUDIO_SMOKE_SECONDS`（默认 600），打开 reference、按实际设备格式重建同一 tick loop，并在退出时同时打印默认请求（48 kHz、256 frames、2 channels）和实际格式、完整逐字段偏差、当次 deviceId/mask（不声称跨重插稳定）、10 次计划构建耗时、持续时间和全部诊断计数。实际格式偏离默认请求时，runner 必须在开始 600 秒前打印完整偏差并返回非零，除非用户在看到 actual 后显式设置 `TRACKLOOM_ACCEPT_AUDIO_FORMAT_DEVIATION=1`；无偏差时不得要求该变量。获确认的偏差仍必须使用 actual 格式重建计划并写入证据；不得要求或伪造单一 256-frame buffer。若后端 xrun 值不为 `-1`，xrun 必须为 0；若为 `-1`，callback timeout 必须为 0；两种情况都要求 oversized block、callback exception、voice stealing 和 stale Note Off 为 0，否则测试返回非零。CTest 设置 `${CMAKE_SOURCE_DIR}` 工作目录，标记 `LABELS "manual;hardware"`、`SKIP_RETURN_CODE 77` 和 `TIMEOUT 720`。
4. 自动门槛：fresh 配置后构建全部 target；完整 CTest 必须 0 failure，只有 MIDI/audio hardware smoke 可按 77 跳过；隐藏 app 烟测正常退出；`git diff --check` 通过。用固定 reference 连续构建计划至少 10 次，逐次记录耗时并校验单次不超过 250 ms。
5. 实机门槛需要用户协助选择并监听目标输出设备。先不设置 `TRACKLOOM_ACCEPT_AUDIO_FORMAT_DEVIATION` 运行一次，若 console 报告实际格式偏差则由用户审阅；只有用户确认后，才在第二次运行前设置该变量。无偏差时不设置该变量。执行：

   ```powershell
   $env:TRACKLOOM_AUDIO_HARDWARE_SMOKE = '1'
   $env:TRACKLOOM_AUDIO_OUTPUT_NAME = '<由用户确认的设备名>'
   $env:TRACKLOOM_AUDIO_SMOKE_SECONDS = '600'
   # 仅当上一次输出已显示 request → actual 偏差且用户明确接受时取消注释：
   # $env:TRACKLOOM_ACCEPT_AUDIO_FORMAT_DEVIATION = '1'
   $evidenceCommit = git rev-parse HEAD
   Write-Output "Evidence commit: $evidenceCommit"
   cmd.exe /d /s /c 'call "E:\Android\VS\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 && "E:\Android\VS\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --test-dir build -R "^trackloom_audio_hardware_smoke_tests$" -V'
   ```

6. 把 `run-template.md` 复制为带绝对日期的证据文件，填写 commit、Windows、设备、驱动、请求与实际格式、每项偏差理由、用户是否设置 `TRACKLOOM_ACCEPT_AUDIO_FORMAT_DEVIATION=1`、当次 deviceId/mask（不声称跨重插稳定）、COM apartment 初始化结果、10 次构建耗时、600 秒持续时间、xrun/timeout/oversized/voice-steal/stale-NoteOff 和人工监听结论。没有实机 GREEN 与完整字段时，不得把 2026-10-31 里程碑标成完成。
7. 运行完整构建、完整 CTest、隐藏 app 烟测、硬件烟测和 `git diff --check`；提交 fixture/runner/template：`test: add audible MIDI playback acceptance fixture`。实际机器证据在验证完成后单独提交：`test: record WASAPI playback evidence`。

**可直接粘贴的首个 RED：** 在任何 fixture 文件创建前注册此测试；首次运行必须以 `reference fixture should load` 失败，而不是静默生成 fixture。

```cpp
void audibleReferenceFixtureBuildsExpectedLoop()
{
    const auto loaded = trackloom::loadProjectFromFile(
        std::filesystem::path("tests/fixtures/audio/minimum-audible-midi/reference.trackloom"));
    require(loaded.project.has_value(), "reference fixture should load");
    trackloom::PreparedMidiPlaybackPlanBuildRequest request;
    request.projectSnapshot = *loaded.project;
    request.sampleRate = 48000.0;
    request.maximumBlockFrames = 256;
    request.outputChannelCount = 2;
    request.outputChannelMask = 3;
    request.playbackStartSample = 0;
    request.loopRange = trackloom::PlaybackLoopRange { 0, 3840 };
    const auto built = trackloom::buildPreparedMidiPlaybackPlan(std::move(request));
    require(built.plan != nullptr, "reference fixture should build");
    require(built.plan->events.size() == 8, "four reference notes should create eight events");
    require(built.plan->loop->loopLengthSamples == 96000,
            "reference loop should last exactly 96000 samples at 48 kHz");
}
```

**最小 GREEN 哈希脚本：** `SHA256SUMS` 每行固定为 `<64 lowercase hex><two spaces><relative filename>`；脚本逐行解析并在第一处不一致时 `FATAL_ERROR`。

```cmake
file(SHA256 "${FIXTURE_DIR}/${expected_file}" actual_hash)
if(NOT actual_hash STREQUAL expected_hash)
    message(FATAL_ERROR "Audio fixture hash mismatch: ${expected_file}")
endif()
```

##### Task 10：设备设置候选、故障恢复与实际格式证据

**目标：** 将 Task 8 的专用设置界面和 Task 9 的实机 runner 收紧为“候选选择不产生副作用、显式应用才切换、使用 host 实际格式保存和取证”的单一路径；设备被移除后仍可由用户选择并应用新设备恢复，且不会把通知线程、UI Timer 或默认 Windows 路由放入切换流程。

**全局约束：** 所有新增中文源文本均以 UTF-8 保存并由 MSVC `/utf-8` 编译；`JuceAudioHost` 的 callback、`audioDeviceError` 和 device-list listener 不得执行扫描、格式化、设置持久化或设备开关。`Faulted`/`DeviceError` 只可由一次成功的替换设备 `openOutput` 清除。每个子项都先取得指定 RED，再写最小 GREEN；不得因真实硬件缺失把 manual runner 的 77 跳过记为硬件通过。

###### Task 10A：中文设置 UI、待应用状态与实际格式保存

**文件：**

- 修改 `src/app/juce/AudioSettingsComponent.h`
- 修改 `src/app/juce/AudioSettingsComponent.cpp`
- 修改 `src/app/juce/TrackLoomMainComponent.cpp`
- 修改 `tests/app_juce_audio_tests.cpp`

**RED：** 在 `tests/app_juce_audio_tests.cpp` 新增 `audioSettingsCandidateRequiresApplyAndPersistsActualFormat`。使用 Task 5 fake device 打开 `Fake Speakers` 后，将 UI 选择改为不同设备或不同采样率/buffer/声道；断言 `host.deviceFormatSnapshot()` 未变、状态文字为准确 UTF-8 的“待应用”、`audioTestToneButtonComponentId` 对应按钮禁用，且 `getButtonText().toUTF8()` 分别为“应用”和“测试音”。再令 fake 对 48 kHz/256/stereo 请求协商为另一合法格式，按“应用”，断言 callback 与磁盘保存收到的单一 `AppAudioSettings` 已被 `actualFormat` 覆盖为实际设备显示名、采样率、block 和输出声道数；当次 UI 同时显示 request → actual 偏差，不持久化 deviceId 或 mask。只公布一个 buffer size 的 fake 必须能正常显示与应用。

**GREEN：** 不扩展 `AppAudioSettings` schema：缺失设置继续使用原有 48 kHz/256/2 默认值。`AudioSettingsComponent` 分别保存已应用快照与可编辑候选；ComboBox `onChange` 只更新候选和“待应用”显示，不调用 `openOutput`。只有 `applySelectedSettings()` 调用 `host_.openOutput()`；成功时以 `JuceAudioHostResult::actualFormat` 覆盖这份单一 settings 快照的设备显示名、采样率、buffer 和输出声道数并回调持久化，UI 在当次操作中显示 request → actual 偏差。候选与已打开实际格式不等或 host 不可用时禁用测试音；应用失败保留已应用输出和候选，给出中文失败反馈。`TrackLoomMainComponent` 仅在成功 callback 后保存该实际快照，不改默认 Windows 路由。

**验证：** 先构建并运行 `trackloom_app_juce_tests`，再运行 `trackloom_app_support_tests`、`trackloom_juce_audio_tests`、隐藏应用烟测和 `ctest --test-dir build --output-on-failure`；最后执行 `git diff --check`。建议提交：`app: persist applied WASAPI audio format`。

###### Task 10B：device-list revision、Faulted 诊断与 controller 恢复

**文件：**

- 修改 `src/core/RealtimePlaybackHost.h`
- 修改 `src/platform/juce/JuceAudioHost.h`
- 修改 `src/platform/juce/JuceAudioHost.cpp`
- 修改 `src/app/AppPlaybackActions.h`
- 修改 `src/app/AppPlaybackActions.cpp`
- 修改 `src/app/juce/AudioSettingsComponent.h`
- 修改 `src/app/juce/AudioSettingsComponent.cpp`
- 修改 `src/app/juce/TrackLoomMainComponent.cpp`
- 修改 `tests/support/FakeJuceAudioDeviceType.h`
- 修改 `tests/juce_audio_tests.cpp`
- 修改 `tests/app_juce_audio_tests.cpp`

**RED：** 在 `tests/juce_audio_tests.cpp` 增加连续 device-list notifications 的用例：listener 只使公开单调 `deviceListRevision` 增长，未调用 `scanForDevices`、`stop`、`close` 或 `createDevice`；消息线程 service 后只针对新 revision 枚举一次。在 fake 移除当前设备后，断言 snapshot 仍为 `Faulted`/`DeviceError`、旧计划失效且设置枚举/选择/应用不被禁用；重新提供或选择另一 fake device 后，成功 `openOutput` 才清除 fault 并增长 format generation。于 `tests/app_juce_audio_tests.cpp` 增加 controller 级回归：设备移除使 Play 保持禁用和稳定原因；设置成功应用后，controller poll 确认 host `Stopped`、available 且此前 failureReason 为 `DeviceFault`，恢复 `AppPlaybackState::Stopped` 与 `canStart=true`，不重用旧 plan，也不在恢复时重建计划。

**GREEN：** 在 `RealtimePlaybackHostSnapshot` 中以单调递增的 `uint64_t deviceListRevision` 发布 `JuceAudioHost` 的设备列表变化，按实用运行期不回绕处理；listener 只原子递增。非实时 host service 幂等消费内部工作，多个消息线程 Timer 可以串行调用它，但不得在 30 Hz 每帧扫描设备。删除或不再匹配当前输出时关闭旧实例、硬重置 runtime、保留 `Faulted`/`DeviceError` 诊断与失效 generation；不能用一个会被多个 Timer 清掉的 boolean 作为 UI 通知。`AudioSettingsComponent` 持久记住最后已观察的 revision，即使 `TrackLoomMainComponent` 先 service host，也会在比较到新 revision 后恰好刷新一次并保留尚可匹配的候选，fault 时仍启用选择和 Apply；不要求主界面持有设置窗口指针或重构为单一 Timer。为 `AppPlaybackController` 加入恢复边界：成功 `openOutput` 后，poll 确认 host 为 `Stopped` 且 available、此前 failureReason 为 `DeviceFault` 时恢复 `AppPlaybackState::Stopped` 与 `canStart=true`；清除旧异步构建结果，用户下一次点 Play 才按新 format generation 重建，旧异步结果仍丢弃。

**验证：** 先运行新增 RED 所在的 `trackloom_juce_audio_tests` 与 `trackloom_app_juce_tests`，GREEN 后运行 `CORE_TEST`、`APP_TEST`、`JUCE_TEST`、完整 CTest 和 `git diff --check`；复核 fake 的 scan/create/open/stop/close 记录证明通知线程没有副作用。建议提交：`fix: recover audio controls after device loss`。

###### Task 10C：runner COM 初始化与协商格式偏差证据

**文件：**

- 修改 `tests/juce_audio_hardware_smoke_tests.cpp`
- 修改 `tests/CMakeLists.txt`
- 修改 `tests/evidence/audio/run-template.md`

**RED：** 在 runner 的 `TRACKLOOM_AUDIO_HARDWARE_SMOKE_SELF_TEST=1` 路径增加独立的格式判定测试：请求 48 kHz/256/stereo、fake/注入结果返回另一合法 shared WASAPI 实际格式时，未设置 `TRACKLOOM_ACCEPT_AUDIO_FORMAT_DEVIATION=1` 必须在开始 600 秒前打印完整偏差并拒绝；设置该变量后才可运行且须记录偏差。actual 无效、`openOutput` 失败或诊断门槛失败始终拒绝。新增可注入 COM 初始化结果的 console-thread helper 测试，断言 host 构造发生在 `CoInitializeEx` 之后；成功初始化恰好配对一次 `CoUninitialize`，`RPC_E_CHANGED_MODE` 记录已有 apartment、继续构造 host 且不调用 `CoUninitialize`。

**GREEN：** 在 `main()` 的任何 `JuceAudioHost` 自动对象之前建立 Windows COM MTA RAII guard；将 hardware self-test 与实际运行共用相同 guard 和结果报告。guard 成功初始化时配对 `CoUninitialize()`，`RPC_E_CHANGED_MODE` 记录现有 apartment 后继续且不反初始化。保留请求常量 48 kHz/256/stereo，打开后总是先打印 request、actual 与逐字段偏差；存在偏差而未设置 `TRACKLOOM_ACCEPT_AUDIO_FORMAT_DEVIATION=1` 时，不安装计划或开始 600 秒循环，设置后才用 `open.actualFormat` 构建计划。`tests/CMakeLists.txt` 显式链接 Windows 所需 COM 库并保留 hardware test 的 `SKIP_RETURN_CODE 77`；证据模板将请求、实际、偏差理由、用户确认变量和 COM 初始化结果列为必填字段。

**验证：** 运行 `trackloom_audio_hardware_smoke_completion_tests` 验证无需硬件的 RED/GREEN、COM apartment 和偏差确认路径；未设置 `TRACKLOOM_AUDIO_HARDWARE_SMOKE=1` 时运行 hardware target 必须以 77 跳过。得到用户授权的设备名与监听结果后，先以 `TRACKLOOM_AUDIO_HARDWARE_SMOKE=1` 运行 runner 审阅 actual；仅在实际格式有偏差且用户明确接受后才额外设置 `TRACKLOOM_ACCEPT_AUDIO_FORMAT_DEVIATION=1` 运行 600 秒，并保存完整 console 输出到带绝对日期的 evidence；随后 fresh 全量构建、完整 CTest、隐藏 app 烟测和 `git diff --check`。建议提交：`test: record negotiated WASAPI hardware format`。

**最终复审门槛：** Task 9 与 Task 10 的自动与实机证据完成后，使用独立 reviewer 对照第 16.4.1–16.4.4 节逐项检查；任何 Critical/Important 必须先补可复现 RED 并用独立 follow-up commit 修复。最终只在 fresh 全量构建、自动 CTest、隐藏应用烟测、10 分钟实机播放和 `git diff --check` 全部有当前 commit 证据时，才能更新第 16.2 节的 2026-10-31 里程碑状态。

### 16.5 2026-12-31 可视 MIDI 循环编曲 D1 书面规格

- 规格确认日期：2026-08-25
- 当前状态：交互、数据流、异常规则和验收口径已确认；本节只锁定 D1 设计，不代表代码已经实现。
- 已确认实施顺序：D1 循环区域与真实循环播放 → D2 MIDI 片段拖动/缩放/吸附/撤销 → D3 钢琴卷帘音符编辑。

本节细化第 16.3 节的 2026-12-31 里程碑，先交付一个可见、可保存、可撤销且能进入现有实时音频路径的工程级 MIDI 循环区域。D1 完成不等同于阶段 D 或 2026-12-31 里程碑整体完成；MIDI 片段自由拖动与缩放、钢琴卷帘以及把循环展开到普通时间线仍须在后续增量中完成。

#### 16.5.1 D1 范围与明确排除

D1 必须形成以下用户可见闭环：

1. 主界面采用已确认的布局 A：顶部运输控制，左侧轨道列表，中部时间线，右侧检查器，底部保留可折叠 MIDI 编辑器区域；底部编辑器在 D1 只保留结构位置，不宣称钢琴卷帘已经实现。
2. 时间线显示普通工程轨道、普通 MIDI 片段块、小节标尺以及至多一个工程级循环区域。循环区域引用现有时间线内容，不复制片段，也不创建特殊循环片段。
3. 用户点击 MIDI 片段后，可在右侧检查器执行“设为循环”。应用层使用检查过溢出的加法计算片段终点；成功后以该片段的实际半开范围 `[clip.startTick, clip.startTick + clip.lengthTick)` 创建工程循环范围，并自动启用当前会话的循环播放。加法溢出或结果非法时必须拒绝操作。
4. 循环区域显示在小节标尺下方。用户可拖动左右边界；拖动期间只显示预览，松开鼠标后才提交一次工程命令。若吸附后的范围与拖动前完全相同，则只结束预览，不执行 no-op 命令、不新增历史也不显示命令失败。
5. 边界拖动按工程拍号图生成的小节边界吸附。每个拍号事件所在 tick 视为新的小节边界；候选距离相同时选择较小 tick。左边界只能吸附到右边界之前，右边界只能吸附到左边界之后，提交前仍须再次执行统一范围验证。
6. 顶部“循环”按钮只改变当前会话的循环启用状态。播放按钮与 Space 键必须使用同一个播放切换入口和同一份有效循环范围计算。
7. 用户可清除工程循环范围；设置、修改和清除范围必须进入统一工程命令历史并支持撤销/重做。

D1 明确不包含：

- Pattern、Scene、特殊 `LoopClip` 或第二套循环内容模型；
- MIDI 片段自由拖动、跨轨拖动、边界缩放、批量编辑和重叠策略；这些属于 D2；
- 钢琴卷帘、音符框选、音符拖动/缩放和力度编辑；这些属于 D3；
- 音频片段循环、波形编辑、效果尾音渲染、插件状态循环或录音；
- 播放中无缝替换实时计划；
- 把循环展开到普通时间线。展开功能后续必须使用一次原子批量命令，保留稳定对象身份、撤销/重做和 tempo/拍号语义，不得通过 UI 直接复制工程对象。

#### 16.5.2 核心模型、命令与工程格式

1. 将当前位于 `PlaybackClock.h` 的 `PlaybackLoopRange` 移至独立核心头文件 `LoopRange.h`。范围始终使用音乐 tick 和半开区间 `[startTick, endTick)`；结构合法条件为 `startTick >= 0` 且 `endTick > startTick`，不另设任意长度上限。tick 到 sample 的派生转换仍由计划构建阶段独立检查溢出和数值安全。`PlaybackClock`、`AudioEngine`、`PreparedMidiPlaybackPlan`、工程模型和应用层必须复用同一个公开范围验证函数，不得各自维护不同判断。
2. `Project` 新增一个可选的 `PlaybackLoopRange`。没有范围与范围存在是工程数据；是否启用循环不是工程数据。
3. 新增单一 `SetProjectPlaybackLoopCommand`，以可选新范围表达设置、修改和清除。命令保存旧范围，成功执行后可撤销并可重做；非法范围或与当前值完全相同的 no-op 必须失败，不进入历史、不标脏工程，也不增加工程 generation。
4. 工程格式由 v10 升级到 v11。存在循环范围时，序列化器在拍号记录之后、轨道记录之前恰好写出一行 `playback_loop <startTick> <endTick>`；不存在范围时不写该记录。
5. v1–v10 工程加载后默认没有工程循环范围。现有 `tests/fixtures/audio/minimum-audible-midi/reference.trackloom` 继续保持 v10 和原始字节不变，不能为迁移测试改写固定音频证据。
6. `playback_loop` 只允许出现在 v11 工程中；v1–v10 文件即使该记录字段合法也必须以版本不匹配失败，不得静默接受未来字段。v11 中出现多个循环记录、字段缺失、额外字段、整数解析失败、负起点、零长度或反向范围时，加载必须整体失败。加载过程只构造临时 `Project`，失败时不得替换当前会话工程、路径、dirty 状态或命令历史。

#### 16.5.3 应用状态与界面边界

1. 新增纯应用层时间线快照，例如 `AppTimelineCanvasStatus`。快照至少包含稳定轨道 ID、轨道类型与名称、片段 ID、所属轨道、片段类型、片段 tick 范围、工程循环范围和可见小节边界；快照 builder 显式接收当前可见 tick 范围，并根据工程拍号图生成该范围所需的小节边界，JUCE 组件不得自行推导拍号语义。`Project` 不保存像素坐标、缩放、滚动或颜色。
2. 新增应用动作边界，例如 `AppLoopActions`，负责从当前 MIDI 片段选择创建范围、提交拖动结果和清除范围。动作先完成选择、类型和范围验证，再调用 `AppProjectSession` 执行核心命令，并返回稳定失败分类及中文展示文本。若“设为循环”计算出的范围已与工程范围相同但会话 `enabled == false`，动作只启用会话并返回独立的 session-only 成功分类，不执行 no-op 工程命令、不新增历史、不标脏也不增加 generation；范围相同且已经启用时返回可解释 no-op。边界拖动结果未变化时只取消预览，不把核心 no-op 当成用户错误。
3. 新增会话级 `AppLoopPlaybackState` 保存 `enabled`，并保持不变量：工程没有循环范围时 `enabled` 必须为 `false`。成功新建、成功打开或关闭工程时取消拖动预览并把 `enabled` 重置为 `false`；工程中已保存的范围仍正常显示。被阻止或加载失败的工程切换不得重置循环状态、选择或预览。该状态不得进入工程文件、工程撤销栈或 dirty 判断。
4. 成功执行“设为循环”后把 `enabled` 设为 `true`；成功清除范围后把它设为 `false`。每次工程命令、撤销或重做完成后，上层都要用最新工程范围协调会话状态：无范围时强制关闭，有范围时保持 `enabled` 原值而不自动开启。因此“设为循环 → undo → redo”在 redo 后只恢复可见范围，不自动恢复播放启用；“清除 → undo”同样只恢复范围。只有再次点击顶部循环按钮，或按第 2 项通过 session-only “设为循环”动作，才会启用。工程没有范围时顶部循环按钮禁用。
5. 新增独立 JUCE 时间线组件，例如 `TimelineLoopEditorComponent`。组件只拥有水平缩放、水平滚动、hover 和拖动预览等短期显示状态；它接收上层传入的当前 selected MIDI clip ID，并通过稳定 ID、可见 tick 范围和候选循环范围回调上层。它不得持有第二份选择真源、可写 `Project`、直接执行命令或访问实时音频 host。
6. `TrackLoomMainComponent` 唯一持有当前 selected MIDI clip ID，并负责连接时间线回调、右侧检查器、既有 MIDI 片段动作和会话循环状态。时间线点击、既有目标 MIDI 片段控件及其他选择入口都只能更新这一份 ID，再由上层传回时间线和检查器；删除片段以及成功新建/打开工程后必须清除失效选择。所有可自动测试的组件必须提供稳定 component ID；界面逻辑不得通过中文标签反推轨道或片段类型。
7. 所有会替换当前 `AppProjectSession` 工程的入口，包括新建、文件选择器打开和最近工程，必须复用一个“可安全替换工程”判断。`Preparing`、`Playing` 或 `Stopping` 时直接拒绝并提示用户先停止播放；文件选择器打开时即使已通过检查，异步回调真正加载前也必须再次检查。允许替换的边界包括：控制器与实时 host 都为 `Stopped` 且没有 worker；没有设备时控制器为 `Unavailable`、回调未运行且没有 worker；或控制器为 `Faulted` 但消息线程已先完成非实时 hard reset，确认回调停止、旧计划释放且没有 worker。被拒绝、hard reset 失败或工程加载失败时不得替换工程、清除选择、取消当前循环状态或让旧工程音频与新工程 UI 并存。音频设备不可用或已安全复位的故障不得永久阻断基础工程操作。

#### 16.5.4 播放计划与生效边界

1. 每次请求开始播放时，统一计算 `effectiveLoopRange = loopState.enabled && project.playbackLoopRange().has_value() ? project.playbackLoopRange() : std::nullopt`，并把它传给现有 `startAppPlayback`。播放按钮、Space 键以及停止后自动执行的隐式重播都必须复用这一计算，不允许形成两个播放语义或沿用控制器中缓存的旧循环范围。
2. D1 用户入口采用明确的循环起播策略：当前播放头若位于有效循环半开范围内，则从当前位置起播；若位于循环左侧、右侧或恰好等于右边界，则从循环左边界起播。设置或拖动范围本身不立即移动播放头，只有实际开始播放时应用该策略。应用层必须使用工程 tempo map 和当前设备采样率做检查过溢出的 tick/sample 边界换算，不依赖低层 plan builder 的 floor-mod 产生用户语义；现有低层 floor-mod 继续作为非 GUI 调用方的确定性防御规则。
3. 现有消息线程/worker 继续从工程快照和有效循环范围构建不可变 `PreparedMidiPlaybackPlan`；实时回调只消费已安装计划，不读取 `Project`、应用会话或 JUCE 界面状态，也不增加锁、动态分配和异常路径。
4. 播放准备键必须包含捕获时的有效循环范围。处于 `Preparing` 时，只要顶部开关、工程范围或其他动作使当前有效循环范围与准备键不同，上层就立即请求取消并使该准备键失效；旧 worker 即使稍后完成也不得安装或自动启动。控制器回到 `Stopped` 后显示“循环设置已变化，请重新播放”或等价提示，由用户再次点击播放，不自动替用户发起新播放。工程范围变化继续由 project generation 检查兜底，纯会话开关变化必须显式比较准备键中的循环范围，不能只依赖 generation。
5. 处于 `Playing` 时允许切换会话循环开关，也允许设置、拖动或清除工程循环范围，但已经安装的计划保持不变。界面必须区分当前正在播放的计划与下一次播放状态，并显示“循环设置已修改，停止并重新播放后生效”或等价明确提示；不得暗示当前声音已经切换到新状态或新范围。
6. 停止后再次通过播放按钮或 Space 开始播放时，按最新工程 generation、设备 format generation、有效循环范围和第 2 项起播策略重新准备计划。播放中执行“回到开头”若保留现有的停止后自动重播行为，也必须在真正重播的时刻重新读取最新工程快照和有效循环范围；循环启用时，项目开头若位于范围外便从循环左边界起播，不得复用旧 `loopRange_`。旧异步结果继续按准备键和 generation 规则丢弃。
7. 循环计划继续复用已测试的跨边界窗口切分、Note Off 合成、一次性 chase、活动音符释放和固定 voice stealing 规则。D1 不得在 UI 层重新实现这些实时语义。

#### 16.5.5 失败处理与可见反馈

- 未选择片段、片段 ID 已失效、选择音频片段或片段时间范围非法时，“设为循环”失败且不修改工程。
- UI 预览不得提交零长度、反向或左边界为负的范围；核心命令仍必须独立拒绝同类输入，避免其他调用方绕过 UI。
- 成功新建、成功打开、关闭工程或组件失去有效工程上下文时，未提交拖动预览必须取消，不能留下幽灵范围或在新工程中提交旧 ID；被阻止或加载失败的工程切换仍保留当前工程有效的预览和选择。
- `Preparing`、`Playing` 或 `Stopping` 时，新建、打开和最近工程入口必须拒绝工程替换；异步打开回调必须二次检查。`Unavailable` 可在确认没有回调/worker 后替换，`Faulted` 可在消息线程 hard reset 成功并释放旧计划后替换。拒绝、复位失败或加载失败不得重置工程、选择、循环状态或预览，避免新工程 UI 与旧工程已安装音频计划并存。
- `Preparing` 中改变有效循环范围必须取消或淘汰旧准备结果并要求用户重新播放；`Playing` 中切换开关、清除或修改范围只影响下一次播放，当前计划继续运行时必须保留待生效提示，停止后清除提示。
- 音频设备打开、测试音、异步计划准备或实时 host 故障不得删除或改写工程循环范围。设备故障继续使用现有播放失败分类和恢复入口。
- 工程加载中的非法或重复循环记录按第 16.5.2 节原子拒绝，不进行静默修复；错误信息必须能区分不支持版本、循环记录语法错误和非法范围。

#### 16.5.6 测试与 D1 完成门槛

实现必须遵循“先 RED、最小 GREEN、独立 reviewer、修复回归、fresh 全量验证”。D1 使用独立测试源文件或测试目标，不继续把所有覆盖堆入现有大型 core/app/JUCE 测试文件。

自动化覆盖至少包括：

1. 循环范围合法与非法边界，以及所有现有播放调用方复用统一验证规则。
2. `SetProjectPlaybackLoopCommand` 的设置、修改、清除、no-op、失败不入栈、撤销和重做。
3. v11 有范围/无范围往返，v1–v10 默认无范围，v1–v10 携带 `playback_loop` 时版本门禁失败，v11 重复、损坏和非法记录原子失败，以及固定 v10 音频 fixture 字节与哈希保持不变。
4. 从有效 MIDI 片段创建范围；无选择、缺失片段、音频片段、非法片段以及 `startTick` 接近 `INT64_MAX` 时的终点加法溢出拒绝；相同范围且会话关闭时只启用会话、不执行工程命令，相同范围且已启用时返回 no-op。
5. 4/4 及变拍号工程的小节边界生成、最近边界吸附、等距选择较小 tick、左右边界不能交叉。
6. 会话循环开关不标脏、不进入撤销栈；新建/打开重置为关闭；保存范围仍可见；清除范围后禁用开关；覆盖“设为循环 → undo → redo”和“清除 → undo → redo”矩阵，证明只有范围进入历史且无范围时不存在潜伏 `enabled=true`。
7. 播放按钮与 Space 使用相同有效范围；播放头在循环内时保留当前位置，在循环外或恰好等于右边界时从左边界起播；`Preparing` 中改变范围或纯会话开关都使旧准备结果失效且不安装；`Playing` 中修改继续消费旧计划并显示待生效提示；停止重播使用新范围。
8. 新建、文件打开和最近工程在 `Preparing`/`Playing`/`Stopping` 状态下拒绝且不改变任何工程或 UI 会话状态；异步文件选择期间开始播放后，打开回调二次检查仍会拒绝；`Stopped`、无设备的安全 `Unavailable` 以及 hard reset 成功的 `Faulted` 边界允许切换，hard reset 失败保持原工程；只有成功切换才重置选择和循环启用状态。
9. 播放中请求“回到开头”后，在停止等待期间修改循环开关或范围，自动重播必须使用最新有效范围而不是旧控制器缓存。
10. JUCE 组件稳定 ID、片段点击选择、“设为循环”、循环带绘制、拖动预览、mouse-up 只提交一次、边界未变化时零命令、清除、撤销恢复和隐藏窗口启动。
11. fake device 路径中的跨循环 MIDI、边界 Note Off、停止后活动音符归零、设备失败不修改工程范围。普通自动化测试不得依赖当前机器声卡。

D1 阶段验收环境为 Windows 11 x64。除自动化测试外，还必须形成以下当前 commit 证据：

- 在包含至少 8 条乐器轨和 32 个 MIDI 片段的固定基准工程中，以 deterministic fake device 的 48 kHz、512 samples 配置累计推进 `28,800,000` frames，即 `56,250` 个 block、每个 block `512` frames，模拟 10 分钟音频时间；测试必须同步加速执行，CTest 设置 120 秒防挂死 timeout。在指定 Windows 11 x64 验收机的 Release 构建中，回调推进目标不超过 60 秒；Debug 和未标定 CI 环境仍执行完整功能断言并记录耗时，但不因超过 60 秒单独判定功能失败。所有环境都必须断言无崩溃、死锁、播放头异常、callback timeout、oversized block、xrun 诊断或悬挂音符。该自动化门槛不构成可听硬件证据，也不替代第 16.2 节最终候选版的 48 kHz、256 samples、30 分钟毕业验收。
- 完成“打开工程 → 选中 MIDI 片段 → 设为循环 → 调整两侧边界 → 可听循环播放 → 关闭循环 → 停止并重新播放 → 确认恢复线性播放 → 撤销/重做 → 保存重开”的人工流程，并确认重开后范围恢复而会话开关默认关闭。
- 由一名未参与实现的人按统一操作说明完成“选片段—设循环—调整边界—播放—关闭循环—停止重播并确认线性播放”，目标用时不超过 3 分钟；记录实际用时、失败步骤和提示需求。该 D1 可用性烟测不替代第 16.2 节最终 5 名初学者任务测试。
- 最终运行 D1 定向测试、完整 CTest、隐藏应用冒烟测试以及 `git diff --check`/`git show --check`。真实 WASAPI 监听和硬件连续播放单独记录；未提供可用设备或未执行时必须标记为 skipped，且 D1 的“可听循环播放”验收保持未完成，不得因 fake-device 测试通过而判定整个 D1 完成。

### 16.6 毕业后范围

以下能力仍保留在长期 A–H 路线中，但不作为 2027-05-31 毕业验收的前置条件：

- VST3 扫描辅助进程、独立插件宿主进程、完整插件状态和崩溃恢复；毕业版的可听路径不得依赖它们。
- 完整音乐分离、单音色转录、AI 辅助扒谱、AI 音频生成、多模型调度、完整项目级对话和完全代理模式；毕业版只验收第 16.1 节定义的单一 AI MIDI 工作流。
- 跨设备同步、HTTPS WebDAV、SFTP、同步冲突和删除传播；这些仍必须默认关闭并且不依赖官方服务器。
- macOS、Linux 和其他非 Windows 平台；当前结构应保留可移植性，但毕业发布和测试只承诺 Windows x64。
- DAWproject、AAF/OMF、完整 stems、录音、自动化和更完整的 DAW 编辑能力。主流 DAW 私有原生工程格式同样不属于毕业范围，且即使毕业后也不作为 TrackLoom 的承诺；对外互通继续优先使用标准和开放交换格式。
- 跨设备同步之外的版本化备份包、选择性恢复和完整崩溃恢复。毕业版仍必须满足第 16.1 节的事务性保存与最近一次成功保存恢复边界。

## 17. 长期阶段路线

### 阶段 A：本地核心基础

状态：截至 2026-06-28 已完成主要底座。

已覆盖：

- C++20/CMake/CTest 基础；
- 工程模型；
- 命令栈；
- 文本工程保存；
- 轨道和片段基础；
- tempo、拍号和 marker；
- 音频 block 渲染骨架；
- 基础混音状态；
- MIDI 音符、调度、sample offset、dispatch、路由和活动音符管理；
- 项目播放会话；
- 循环 MIDI 调度；
- 安全播放控制命令。

剩余边界：

- 已有最小 JUCE 桌面壳编译和隐藏启动验证，首屏已显示当前工程状态和轨道列表，并提供新建工程、打开工程、保存、另存为、添加乐器轨、添加/删除空音频轨、添加空文件夹轨以及创建、重命名、复制、中点拆分、跨音频轨移动、一拍左右移动、片头/片尾缩短/延长和删除空音频片段按钮；
- 桌面壳已有应用层工程会话边界，可新建、保存、打开工程并管理当前路径、dirty 状态和首批命令历史；当前已把打开、保存、另存为接到 JUCE 文件选择器，并已提供首屏最近工程选择、打开入口、基础文件/编辑/轨道/片段/播放菜单栏、基础菜单命令分发边界、基础命令面板数据层、基础命令面板会话状态层、可见命令面板浮层、第一批文件快捷键、基础撤销/重做快捷键、轨道动作、轨道状态动作、默认 MIDI 片段创建/删除/重命名/复制/拆分/一拍移动/跨轨移动/片尾缩短/延长/片头缩短/延长命令历史基础、空音频片段外壳创建/删除/重命名/复制/拆分/跨轨移动/一拍移动/片尾缩短/片尾延长/片头缩短/片头延长命令历史基础，以及 MIDI 音符创建/删除/复制/音高/力度/长度/起点微调命令历史基础；基础编辑菜单和快捷键已能触发工程撤销/重做，基础轨道菜单已能触发添加乐器轨、添加音频轨、添加文件夹轨，以及依赖当前目标选择的乐器轨重命名/删除/上移/下移/静音切换/独奏切换/禁用切换/隐藏切换和音频轨删除/上移/下移/静音切换/独奏切换/禁用切换/隐藏切换，基础片段菜单已有选择依赖的 MIDI/音频片段编辑入口，命令面板已支持搜索、Enter 执行、高亮移动、Home/End、PageUp/PageDown、鼠标悬停高亮、鼠标点击和独立可见窗口滚动，但尚未接入完整快捷键、完整编辑命令体系、完整轨道/片段菜单或正式未保存确认弹窗；
- 2026-07-04 起，基础片段菜单已扩展为按当前选择触发重命名所选 MIDI 片段、删除所选 MIDI 片段、复制所选 MIDI 片段、拆分所选 MIDI 片段、移动所选 MIDI 片段到目标乐器轨、左移所选 MIDI 片段、右移所选 MIDI 片段、缩短所选 MIDI 片尾、延长所选 MIDI 片尾、缩短所选 MIDI 片头、延长所选 MIDI 片头、重命名所选音频片段、删除所选音频片段、复制所选音频片段、拆分所选音频片段、移动所选音频片段到目标音频轨、左移所选音频片段、右移所选音频片段、缩短所选音频片尾、延长所选音频片尾、缩短所选音频片头和延长所选音频片头；普通片段命令按片段存在且类型匹配启用，跨轨移动命令还要求目标轨存在、类型正确且不是片段当前所属轨；命令执行继续复用现有 MIDI/音频片段动作和会话命令历史；当前仍不是完整片段菜单，不代表时间线拖拽、批量片段编辑或完整剪贴板已完成；
- 2026-07-06 起，`AppCommandShortcuts` 已增加应用层自定义快捷键活动表基础：从默认绑定生成活动表，接受不冲突覆盖，拒绝并报告冲突覆盖，并允许给无默认快捷键的命令新增绑定；同日新增 `AppShortcutSettings`，可把用户覆盖项保存到本地 UTF-8 文本设置文件并重新加载，缺失文件、坏行和当前不支持的组合键会被忽略，冲突仍由统一合并结果报告；JUCE 桌面壳现在启动时从本机应用数据目录加载 `TrackLoom/shortcuts.txt`，键盘分发和命令面板快捷键标签共用同一份活动绑定表；`AppShortcutStatus` 已能把主菜单命令、活动快捷键、用户覆盖项和冲突合成为设置界面可展示的行快照、冲突快照和摘要；当前仍没有可编辑设置界面、完整编辑快捷键或平台原生菜单快捷键展示；
- 2026-07-08 起，工具菜单和命令面板新增“快捷键状态...”只读入口；`AppMainMenuCommand::OpenShortcutStatus` 通过 `AppCommandDispatcher` 分发到 JUCE 桌面壳，弹窗文本复用 `AppShortcutStatus` 展示当前命令快捷键、用户覆盖标记和冲突详情。该入口只用于查看，不保存快捷键设置，不等同于完整快捷键编辑界面；CTest 覆盖工具菜单快照、命令面板展开、分发器成功路径和缺失 handler 失败路径；
- 2026-07-02 起，当前已有空音频片段外壳动作已接入同一会话命令历史，可撤销/重做并保留稳定 clip id；当前能力仍只是音频片段外壳编辑，不代表音频文件导入、素材偏移、真实修剪、波形或可听播放已经实现；
- 桌面壳现已接入 `AppPlaybackController`、不可变 MIDI 播放计划、固定复音内置合成器和直接 JUCE shared WASAPI 音频回调；播放按钮与 Space 可异步准备并启动真实输出，工具菜单中的音频设置可选择输出设备并把成功应用的实际格式保存到本机设置；
- 当前可听路径只证明内置合成器 MIDI 经 shared WASAPI 输出的基础能力，并已有不依赖声卡的 fake-device 自动测试；它仍不代表音频片段播放、插件播放、录音、ASIO、外接 MIDI 硬件或完整 DAW 音频引擎已经完成，真实设备连续播放证据必须按硬件 smoke 规则单独记录，未运行时按 skipped 报告；
- 没有用户可操作的 MIDI 设备选择界面；已有默认跳过的外接 MIDI 硬件冒烟测试入口，但尚未完成实机验证；
- 没有插件宿主；
- 没有 AI 服务；
- 没有真实音频文件导入导出；
- 没有用户可操作的循环编曲器界面。

### 阶段 B：播放控制与设备接入

目标：把已测试的核心播放会话连接到更接近真实使用的运行边界。

当前进展：

- 2026-06-28：已为播放控制命令和项目播放会话增加稳定失败原因分类，UI、快捷键、设备层和 AI 工具后续可以读取枚举原因，而不是解析自由文本消息。
- 2026-06-28：已增加核心 MIDI 输出设备抽象层，包括平台端口接口、`MidiEventReceiver` 适配器、打开/关闭/发送失败原因和 dispatch 停止语义。
- 2026-06-28：已新增独立 `trackloom_juce` 平台适配库，通过本地 JUCE 8 枚举 MIDI 输出设备、按设备 id 打开和关闭端口，并把核心层 `MidiOutputMessage` 转换为 JUCE 三字节 MIDI 消息。CTest 已覆盖设备信息映射、消息字节转换、无硬件假设的枚举、无效设备打开失败和端口工厂。
- 2026-06-28：已新增 JUCE MIDI 输出设备管理器，通过可替换端口工厂打开设备并持有 `MidiOutputDevice` 适配器，再调用 `ProjectPlaybackSession::rebuildMidiOutputSafely` 接入现有安全释放边界。CTest 已覆盖接入播放会话、打开失败保留旧设备、切换前释放活动音符、清空输出和非法轨道不触发设备打开。
- 2026-06-28：已新增 `trackloom_midi_hardware_smoke_tests` 手动硬件冒烟测试入口。普通 CTest 默认跳过；只有设置 `TRACKLOOM_MIDI_HARDWARE_SMOKE=1` 和 `TRACKLOOM_MIDI_OUTPUT_ID` 时才会向指定 JUCE MIDI 输出设备发送短音符，并在结束时清空路由释放活动音符。
- 2026-06-28：已新增 MIDI 输出设备快照差异识别，按稳定设备 id 比较两次枚举结果，输出新增、移除和保留设备。CTest 覆盖设备重命名、列表新增/移除和输出顺序，后续可供 UI 设备选择、热插拔提示和断开处理复用。
- 2026-06-28：已新增 `JuceMidiOutputDeviceList` 手动刷新入口，持有当前 MIDI 输出设备缓存，并通过注入枚举函数返回刷新差异。CTest 覆盖首次刷新、后续刷新、缓存替换和按缓存查找设备，后续 UI 可复用该入口刷新设备列表。
- 2026-06-28：已新增 MIDI 输出设备移除绑定策略计划，根据当前轨道绑定和最新可见设备列表，输出可继续路由的绑定、不可用但应保留给 UI 和未来重连的绑定，并在有绑定设备消失时要求上层走安全重建。CTest 覆盖设备消失、保留原绑定信息、更新可见设备显示名和全部设备仍可见时不要求重建。
- 2026-06-28：已新增 `JuceMidiOutputDeviceManager::refreshProjectMidiOutputForVisibleDevicesSafely`，把设备刷新计划连接到播放会话安全重建。CTest 覆盖设备消失后收缩路由、活动音符释放失败时保留旧路由和全部设备仍可见时不重新打开端口。
- 2026-06-28：已新增 `JuceMidiOutputRoutingController` 无 UI 设备选择控制层，保存用户当前轨道设备选择，刷新设备缓存，并把选择应用到安全路由重建。CTest 覆盖设备消失时保留用户选择但收缩运行态路由、设备重新出现时恢复运行态路由、同一设备改绑到另一条轨道时重建路由、按缓存设备 id 设置轨道选择、输出路由状态快照、按快照生成可展示状态分类、识别已清除但仍待安全重建移除的旧运行态路由、设备显示名变化时不重开端口、清除选择后关闭运行态设备。
- 2026-06-29：已新增 `JuceMidiOutputRoutingController::applySelectedProjectMidiOutputIfNeeded`，为后续 UI 的“应用更改”按钮提供无 UI 入口。当前选择已生效时不会重开端口；清除选择后会通过安全重建关闭旧运行态路由。CTest 覆盖已应用路由的无操作应用和清除选择后的旧路由移除。
- 2026-06-29：已修正 MIDI 输出刷新边界：只保存了用户选择但尚未应用到运行态时，如果设备离线，刷新只保留离线选择给 UI 提示和未来重连，不要求未准备的播放会话执行安全重建。CTest 覆盖未应用离线选择的刷新 no-op。
- 2026-06-29：已新增 `summarizeMidiOutputRoutingRefreshResult`，把设备新增/移除、离线选择、安全重建尝试和安全重建失败汇总为 UI/诊断面板可直接读取的布尔摘要。CTest 覆盖无提示刷新、离线选择提示和安全重建失败提示。
- 2026-06-29：已新增 `summarizeMidiOutputRoutingApplyResult`，把“应用当前选择”的成功状态、离线选择、安全重建尝试、运行态变化和安全重建失败汇总为 UI/诊断面板可直接读取的布尔摘要。CTest 覆盖无变化应用、离线选择提示、成功应用运行态变化和应用失败提示。
- 2026-06-29：已使用 Windows 可见输出设备 `Microsoft GS Wavetable Synth` 运行手动 MIDI 烟测，验证 JUCE 到 Windows 软件合成器的发送路径和清空路由释放 Note Off 流程。该验证不等同于外接 MIDI 硬件实机验证，仍需后续连接真实外设测试拔插和断线行为。

建议顺序：

1. 已完成：完善播放控制结果和错误分类，便于 UI、快捷键和 AI 工具显示明确失败原因。
2. 已完成：接入核心 MIDI 输出设备抽象层，并覆盖设备未打开、打开失败、发送失败、断开和重连测试。
3. 已完成：实现 JUCE/Windows MIDI 输出端口枚举、打开和关闭适配层。
4. 已完成：把 JUCE MIDI 输出端口接入项目播放会话的设备配置入口。
5. 已完成：为外接 MIDI 设备准备手动硬件冒烟测试，并明确没有硬件或没有指定设备 id 时跳过。
6. 已完成：实现 MIDI 输出设备快照差异识别，为热插拔提示和 UI 设备列表刷新打底。
7. 已完成：在设备列表刷新入口中使用快照差异，提供当前设备缓存和按缓存查找能力。
8. 已完成：明确底层设备移除策略，消失设备从可路由绑定中排除，但保留原绑定信息给 UI 提示和未来重连，并要求上层通过安全重建释放和更新路由。
9. 已完成：实现设备刷新结果到设备管理器安全重建的连接层，明确安全重建失败时保留旧路由，并把不可用绑定返回给 UI 提示和未来重连。
10. 已完成：新增无 UI 的设备选择控制层，保存用户选择，刷新设备缓存，并通过安全重建更新运行态路由；设备重新出现时恢复已保留选择的运行态路由，同一设备改绑到另一条轨道时重建轨道路由；后续 UI 可按当前缓存中的设备 id 设置轨道输出选择，并通过只读状态快照、状态分类和统一 apply 判断区分用户选择、当前可见设备、已应用路由、已打开端口、待应用、设备离线、待移除旧路由，以及“应用更改”按钮是否需要启用。
11. 已完成：新增应用当前 MIDI 输出选择的按需入口；已应用状态不重开端口，存在待应用或待移除旧路由时才走安全重建。
12. 已完成：刷新设备列表时区分用户选择和运行态路由；未应用的离线选择只提示不重建，已应用的离线路由才需要通过安全重建收缩。
13. 已完成：新增设备刷新结果摘要，让后续 UI/诊断面板不用重复拆解设备差异、离线选择和安全重建失败。
14. 已完成：新增应用选择结果摘要，让后续 UI/诊断面板不用重复拆解离线选择、运行态变化和安全重建失败。
15. 已部分完成：已在 `Microsoft GS Wavetable Synth` 上运行手动 MIDI 烟测，验证软件合成器输出路径；有外接 MIDI 设备时仍需记录真实设备 id、可听结果、Note Off 释放和设备断开行为。
16. 后续：补充设备热插拔、断开重连、活动音符释放和真实硬件发送路径的持续验证。
17. 持续保持设备层不影响工程数据和音频渲染结果的隔离规则。

验收：

- 停止、跳转、切换 MIDI 输出目标时不会产生悬挂音符。
- 设备发送失败有可解释结果。
- 设备失败不会破坏工程保存和基础播放状态。
- CTest 覆盖正常路径和失败路径。

### 阶段 C：JUCE 桌面壳与最小可用编辑

目标：做出 Windows 上可启动的最小桌面程序。

当前进展：

- 2026-07-08：已新增只读快捷键状态入口。`AppMainMenu` 的工具菜单新增 `OpenShortcutStatus` 稳定 command id 和“快捷键状态...”菜单项，命令面板会展示同一工具命令，`AppCommandDispatcher` 新增 `OpenShortcutStatus` kind 与 handler；JUCE 桌面壳触发后读取当前主菜单快照、启动时加载的本机自定义快捷键覆盖项和活动绑定表，调用 `AppShortcutStatus` 生成摘要、命令快捷键、自定义标记和冲突详情，并用只读消息弹窗展示。CTest 覆盖菜单项、命令面板项、分发器成功执行和缺失 handler 失败。当前能力只是查看快捷键状态，不代表已经支持在界面中修改、保存或重置快捷键。
- 2026-07-06：已扩展 `AppCommandShortcuts`，新增 `customizeAppShortcutBindings`、`AppShortcutConflict`、按活动绑定表查询的 `appCommandIdForShortcut(chord, bindings)`，以及同时接收快捷键上下文和活动绑定表的 `appCommandIdForShortcut(chord, context, bindings)`；应用层现在可以从默认快捷键表生成活动表，非冲突用户覆盖会替换同一命令旧绑定，冲突覆盖会报告现有命令和请求命令并保留原绑定，也能为无默认快捷键的命令新增绑定。随后新增 `AppShortcutSettings`，只把用户覆盖项保存到本地 UTF-8 文本设置文件，不保存默认表，不写入 `.trackloom` 工程文件；加载缺失文件、坏行和当前不支持的组合键时保持容错，加载后继续复用统一合并规则报告冲突。JUCE 桌面壳现在启动时从本机应用数据目录读取 `TrackLoom/shortcuts.txt`，键盘分发和命令面板快捷键标签共用同一份活动绑定表；命令面板打开时仍抑制全局快捷键。随后新增 `AppShortcutStatus`，可从主菜单快照、快捷键合并结果和用户覆盖项生成设置界面可用的命令行、活动快捷键标签、自定义标记、冲突行和摘要。CTest 覆盖替换默认绑定、冲突拒绝、新增无默认绑定命令、保存加载、缺失文件、冲突加载、坏行忽略、活动表命令面板标签、上下文内自定义快捷键分发、活动快捷键状态行和冲突状态行。当前能力只是自定义快捷键的数据层、本地设置读写、启动加载和设置状态快照基础，不代表已经有可编辑设置界面、完整快捷键体系或平台原生菜单快捷键展示。
- 2026-07-04：已扩展 `AppMainMenu` 与 `AppCommandDispatcher` 的选择依赖片段命令，新增 `DuplicateSelectedMidiClip`、`DuplicateSelectedAudioClip`、`RenameSelectedMidiClip`、`RenameSelectedAudioClip`、`SplitSelectedMidiClip`、`SplitSelectedAudioClip`、`MoveSelectedMidiClipToTargetTrack`、`MoveSelectedAudioClipToTargetTrack`、`MoveSelectedMidiClipLeft`、`MoveSelectedMidiClipRight`、`MoveSelectedAudioClipLeft`、`MoveSelectedAudioClipRight`、`TrimSelectedMidiClipEnd`、`ExtendSelectedMidiClipEnd`、`TrimSelectedAudioClipEnd`、`ExtendSelectedAudioClipEnd`、`TrimSelectedMidiClipStart`、`ExtendSelectedMidiClipStart`、`TrimSelectedAudioClipStart` 和 `ExtendSelectedAudioClipStart` 稳定 command id 与 handler；片段菜单和命令面板现在会展示重命名/删除/复制/拆分/移动到目标轨/左移/右移/缩短片尾/延长片尾/缩短片头/延长片头所选 MIDI 片段，以及重命名/删除/复制/拆分/移动到目标轨/左移/右移/缩短片尾/延长片尾/缩短片头/延长片头所选音频片段。普通片段命令按当前目标片段存在且类型匹配启用；跨轨移动命令还要求目标轨存在、类型正确且不是片段当前所属轨。JUCE 菜单触发这些命令时复用已有片段动作函数，继续进入现有片段命令历史。CTest 覆盖无选择禁用、有效选择启用、同轨目标禁用、类型错配禁用、命令面板展开顺序和分发器 handler 映射。当前仍不是完整片段菜单，不代表时间线拖拽、批量片段编辑或完整剪贴板已完成。
- 2026-06-29：已新增 `trackloom_app_support` 和 `trackloom_app`。`trackloom_app_support` 保存应用名、版本和组织名并由 CTest 覆盖；`trackloom_app` 是可编译、可隐藏启动并主动关闭的 JUCE 桌面壳。当前窗口只显示项目状态文案，还没有工程新建、打开、保存、播放控制或设置存储。
- 2026-06-29：已新增 `AppProjectSession`，作为桌面壳当前工程的应用层会话边界。它复用核心 `ProjectFile` 保存/读取逻辑，管理当前工程、当前文件路径和 dirty 状态；CTest 覆盖新建工程、编辑标脏、另存为、重新打开、打开失败不覆盖当前工程，以及无路径保存失败。当前该能力仍是无 UI 支持库，尚未接到菜单、按钮或快捷键。
- 2026-06-29：已新增 `AppProjectStatus`，把 `AppProjectSession` 派生为窗口标题、工程路径提示、dirty 标记、轨道计数和状态行，供 UI 复用。JUCE 首屏已接入该状态快照，并提供新建工程和添加乐器轨按钮；CTest 覆盖未保存 dirty 工程和已保存 clean 工程的状态描述。当前打开、保存和另存为仍未接入文件选择器、菜单或快捷键。
- 2026-06-29：已新增 `AppProjectFileActions`，为打开、保存、另存为、取消选择、无路径保存和默认 `.trackloom` 扩展名提供可测试反馈。`AppProjectSessionResult` 已增加稳定失败原因，UI 不再需要解析错误字符串。JUCE 首屏已接入打开、保存、另存为文件选择器；当前工程有未保存修改时，会先要求保存或另存为，再允许新建或打开其他工程。当前仍未实现完整菜单、完整快捷键、正式确认弹窗或保存后自动打开所在目录等扩展体验。
- 2026-06-29：已新增 `AppTrackListStatus` 和 `AppTrackActions`，把核心 `Project` 轨道转换为可展示行，并把首屏添加/删除乐器轨收敛到应用层动作。JUCE 首屏可添加默认乐器轨，也可选择目标乐器轨并删除；删除乐器轨会移除该轨拥有的片段和片段内音符。CTest 覆盖空工程、轨道顺序、类型标签、片段计数、轨道状态标签、默认乐器轨创建、连续命名、成功删除、缺失轨道删除拒绝和非乐器轨删除拒绝。当前仍未实现拖拽排序或通用轨道编辑入口。
- 2026-06-30 新增、2026-07-01 扩展：已扩展 `AppTrackActions`，新增默认空音频轨创建和目标音频轨删除入口。JUCE 首屏可点击“添加音频轨”在轨道列表中追加 `Audio N` 音频轨，轨道列表会显示为“音频轨”；也可选择目标音频轨并点击“删除音频轨”，删除该轨及其拥有的空音频片段。该入口只管理空音频轨外壳，不导入音频文件，不自动创建音频片段，也不把音频轨加入“目标乐器轨”下拉框。CTest 覆盖成功创建音频轨、连续音频轨命名、成功删除音频轨及其片段、缺失音频轨删除拒绝、非音频轨删除拒绝和 dirty 状态。当前能力只是音频轨外壳创建/删除，不等同于音频文件导入、波形显示、音频片段播放、完整轨道类型选择器或批量轨道编辑。
- 2026-06-30 新增、2026-07-01 扩展：已新增并扩展 `AppAudioClipActions`，为首屏提供可测试的默认空音频片段创建、重命名、复制、中点拆分、跨音频轨移动、一拍左右移动、片头/片尾缩短/延长和删除入口。JUCE 首屏新增独立“目标音频轨”下拉框、“创建音频片段”按钮、“目标音频片段”下拉框、音频片段名称输入框、“重命名音频片段”按钮、“复制音频片段”按钮、“拆分音频片段”按钮、“移到音频轨”按钮、“左移音频片段”按钮、“右移音频片段”按钮、“缩短音频片尾”按钮、“延长音频片尾”按钮、“缩短音频片头”按钮、“延长音频片头”按钮和“删除音频片段”按钮；可在目标音频轨末尾追加一个一小节长度的空音频片段，连续创建会追加到该轨已有片段之后，也可选择目标音频片段重命名、复制到同轨道源片段结束位置、从长度中点拆分成左右两个空音频片段、移动到当前目标音频轨、一拍左移、一拍右移、缩短片尾、延长片尾、缩短片头、延长片头或删除；缺失轨道、非音频轨、缺失片段、空名称、MIDI 片段、过短片段、同轨移动、目标轨道缺失、目标轨道不是音频轨、左移到时间线起点之前、片尾/片头缩短后长度不足一拍和片头延长到时间线起点之前会被拒绝且不标脏工程。`AppTimelineStatus` 空时间线提示已改为同时指向 MIDI 片段和音频片段。CTest 覆盖成功创建、连续追加、缺失轨道拒绝、非音频轨拒绝、成功重命名、空名称拒绝、缺失片段重命名拒绝、MIDI 片段重命名拒绝、成功复制、缺失片段复制拒绝、MIDI 片段复制拒绝、成功中点拆分、缺失片段拆分拒绝、MIDI 片段拆分拒绝、过短音频片段拆分拒绝、成功移动到另一条音频轨、同轨移动拒绝、缺失源片段跨轨移动拒绝、缺失目标轨道跨轨移动拒绝、乐器目标轨跨轨移动拒绝、MIDI 片段跨轨移动拒绝、成功一拍右移、成功一拍左移、左边界拒绝、缺失片段移动拒绝、MIDI 片段移动拒绝、成功片尾缩短、成功片尾延长、过短片段片尾缩短拒绝、缺失片段片尾编辑拒绝、MIDI 片段片尾编辑拒绝、成功片头缩短、成功片头延长、过短片段片头缩短拒绝、片头延长越过时间线起点拒绝、缺失片段片头编辑拒绝、MIDI 片段片头编辑拒绝、成功删除、缺失片段删除拒绝和 MIDI 片段删除拒绝。当前能力只是音频片段外壳创建、重命名、复制、中点拆分、跨音频轨移动、一拍移动、片头/片尾长度编辑和删除；复制只复制片段外壳，中点拆分只拆时间线外壳，跨音频轨移动只改变片段所属轨道，一拍移动只改变片段起点，片尾编辑只改变片段长度，片头编辑只改变外壳左边界并保持旧终点稳定，不等同于音频文件导入、素材复制、素材引用、素材偏移、素材移动、波形显示、音频片段播放、真实音频切点、交叉淡化、拖拽定位、吸附网格、重叠冲突处理或真实音频修剪。
- 2026-07-02：`AppAudioClipActions` 中当前已有空音频片段外壳动作已迁移到 `AppProjectSession` 命令历史：创建通过 `AddClipCommand`，重命名通过 `RenameClipCommand`，删除通过 `DeleteClipCommand`，复制通过 `DuplicateClipCommand`，拆分通过 `SplitClipCommand`，跨轨移动通过 `MoveClipToTrackCommand`，一拍移动、片尾延长和片头延长通过 `SetClipTimingCommand`，片尾缩短通过 `TrimClipEndCommand`，片头缩短通过 `TrimClipStartCommand`。撤销/重做会保留同一个 clip id，并恢复片段名称、轨道、类型、起点和长度；复制会恢复同一个副本 clip id，拆分会恢复同一个右侧片段 clip id，跨轨移动会在源轨和目标轨之间切换。CTest 新增并通过音频片段创建、重命名、删除、复制、拆分、跨轨移动、一拍移动、片尾缩短、片尾延长、片头缩短和片头延长撤销/重做覆盖。当前能力仍只是空音频片段外壳编辑，不代表音频文件导入、素材复制、素材引用、素材偏移、波形显示、可听播放、真实音频切点、交叉淡化、拖拽定位、吸附网格、重叠处理或真实音频修剪。
- 2026-06-30：已扩展 `AppTrackActions`，新增默认空文件夹轨创建入口。JUCE 首屏可点击“添加文件夹”在轨道列表中追加 `Folder N` 文件夹轨，轨道列表会显示为“文件夹轨”；该入口只创建空轨道，不建立层级、不折叠归组、不移动子轨道，也不把文件夹轨加入“目标乐器轨”下拉框。CTest 覆盖成功创建文件夹轨、连续文件夹轨命名和 dirty 状态。当前能力只是文件夹轨外壳创建，不等同于文件夹层级、折叠显示、轨道归组、批量移动或完整轨道类型选择器。
- 2026-06-30：已扩展 `AppTrackActions`，新增轨道重命名应用层入口。JUCE 首屏可编辑当前选中乐器轨名称并点击“重命名”或按回车应用；输入名称会去掉首尾空白，空名称和缺失轨道会被拒绝且不标脏工程。CTest 覆盖成功重命名标脏、首尾空白清理、空名称拒绝、缺失轨道拒绝和轨道列表显示新名称。当前能力只是目标轨道名称编辑，不等同于内联列表编辑、批量重命名、通用轨道属性面板或撤销重做 UI。
- 2026-06-30：已扩展 `AppTrackActions`，新增目标乐器轨上移和下移入口。JUCE 首屏可对当前选中乐器轨调整顺序，轨道列表和目标轨道下拉框会按新顺序刷新并保持同一 track id 选中；顶部/底部、缺失轨道和非乐器轨会被拒绝且不标脏工程。CTest 覆盖上移、下移、边界拒绝、缺失轨道拒绝和非乐器轨拒绝。当前能力只是按钮式顺序调整，不等同于拖拽排序、跨类型批量排序或完整轨道编组编辑。
- 2026-06-30 新增、2026-07-01 扩展：已新增 `AppTrackStateActions`，为首屏和后续菜单、快捷键、AI 工具提供轨道静音、独奏、禁用和隐藏的统一切换入口。JUCE 首屏可对当前选中乐器轨切换四种状态，轨道列表摘要会同步显示状态标签；状态切换现在通过核心状态命令进入 `AppProjectSession` 命令历史，可通过会话层撤销/重做。CTest 覆盖静音切换标脏、播放状态互不覆盖、隐藏不影响播放状态、缺失轨道不标脏不入历史、列表状态标签，以及静音和隐藏撤销/重做。当前能力只是目标轨道状态按钮和命令历史基础，不等同于混音器、轨道头完整状态区、完整 Undo/Redo UI、批量轨道操作或自动化。
- 2026-06-29 新增、2026-07-02 扩展：已新增 `AppMidiClipActions` 和 `AppTimelineStatus`，为首屏提供可测试的目标乐器轨选择、默认 MIDI 片段创建、目标 MIDI 片段删除、目标 MIDI 片段重命名、目标 MIDI 片段复制、目标 MIDI 片段中点拆分、目标 MIDI 片段一拍左右移动、目标 MIDI 片段移动到目标乐器轨、目标 MIDI 片段片尾缩短与延长、目标 MIDI 片段片头缩短与延长和只读时间线摘要。JUCE 首屏可选择乐器轨并创建一小节默认 MIDI 片段，新片段会追加到该轨已有片段之后；也可选择目标 MIDI 片段删除整个片段、重命名、复制到自身之后、从中点拆分、按一拍左右移动、移动到另一条目标乐器轨、缩短片尾、延长片尾、缩短片头或延长片头。默认 MIDI 片段创建现在通过核心 `AddClipCommand` 进入 `AppProjectSession` 命令历史，删除通过 `DeleteClipCommand` 进入命令历史，重命名通过 `RenameClipCommand` 进入命令历史，复制通过 `DuplicateClipCommand` 进入命令历史，拆分通过 `SplitClipCommand` 进入命令历史，一拍移动通过 `SetClipTimingCommand` 进入命令历史，跨轨移动通过 `MoveClipToTrackCommand` 进入命令历史，片尾缩短通过 `TrimClipEndCommand` 进入命令历史，片尾延长通过 `SetClipTimingCommand` 进入命令历史，片头缩短和片头延长通过 `SetMidiClipStartKeepingNoteTimesCommand` 进入命令历史；撤销/重做会保留稳定 clip id，并恢复被删片段内的 MIDI 音符、旧片段名称、复制得到的新 note id、拆分得到的右侧片段与移动后的 MIDI 音符、移动前后的片段起点、跨轨移动前后的源轨和目标轨、片尾编辑前后的片段长度，或片头编辑前后的片段左边界、长度和音符相对 tick。CTest 覆盖成功创建、追加位置、缺失轨道、非乐器轨拒绝、成功删除、缺失片段删除拒绝、音频片段删除拒绝、成功重命名、空名称拒绝、成功复制、成功拆分、成功一拍移动、成功跨轨移动、成功片尾缩短、成功片尾延长、成功片头缩短、成功片头延长、空时间线、片段行摘要，以及创建/删除/重命名/复制/拆分/一拍移动/跨轨移动/片尾缩短/片尾延长/片头缩短/片头延长撤销重做。当前仍未实现正式时间线拖拽、完整编辑命令体系、钢琴卷帘或播放控制接入。
- 2026-06-30：已扩展 `AppMidiClipActions`，新增目标 MIDI 片段复制入口。JUCE 首屏可复制当前选中 MIDI 片段，副本放在同一轨道、源片段结束位置，并自动选中新片段；复制会为片段内 MIDI 音符分配新 ID，避免源片段和副本共享音符身份。CTest 覆盖成功复制、缺失片段拒绝和音频片段拒绝。当前能力只是同轨、紧接源片段的按钮式复制，不等同于跨轨复制、拖拽定位、重叠冲突处理或完整剪贴板。
- 2026-06-30：已扩展 `AppMidiClipActions`，新增目标 MIDI 片段重命名入口。JUCE 首屏可编辑当前选中 MIDI 片段名称并点击“重命名片段”或按回车应用；输入名称会去掉首尾空白，空名称、缺失片段和音频片段会被拒绝且不标脏工程。CTest 覆盖成功重命名标脏、首尾空白清理、空名称拒绝、缺失片段拒绝、音频片段拒绝和时间线显示新名称。当前能力只是目标 MIDI 片段名称编辑，不等同于时间线内联编辑、批量重命名或完整片段属性面板。
- 2026-06-30 新增、2026-07-01 扩展：已扩展 `AppMidiClipActions`，新增目标 MIDI 片段中点拆分入口。JUCE 首屏可把当前选中 MIDI 片段从长度中点拆成左右两段，成功后自动选中新生成的右侧片段；缺失片段、音频片段、过短片段和存在跨中点音符的片段会被拒绝且不标脏工程。2026-07-01 起，成功拆分通过核心 `SplitClipCommand` 进入 `AppProjectSession` 命令历史；撤销会恢复原始片段长度和原有 MIDI 音符，重做会恢复同一个右侧片段 id 和移动后的 MIDI note id。CTest 覆盖成功拆分、右侧音符移动、缺失片段拒绝、音频片段拒绝、过短片段拒绝、跨中点音符拒绝和拆分撤销/重做。当前能力只是无输入的中点拆分，不等同于任意切点刀片工具、拖拽拆分、跨音符自动切断或延音规则。
- 2026-06-30 新增、2026-07-01 扩展：已扩展 `AppMidiClipActions`，新增目标 MIDI 片段一拍左移和一拍右移入口。JUCE 首屏可把当前选中 MIDI 片段按四分音符 tick 步长左右移动；移动只改变片段起点，保持片段长度和内部 MIDI 音符相对 tick 不变；左移到时间线起点之前、缺失片段和音频片段会被拒绝且不标脏工程。2026-07-01 起，成功一拍移动通过核心 `SetClipTimingCommand` 进入 `AppProjectSession` 命令历史；撤销会恢复旧起点，重做会恢复移动后的新起点。CTest 覆盖右移、左移、左边界拒绝、缺失片段拒绝、音频片段拒绝和一拍移动撤销/重做。当前能力只是按钮式固定步长移动，不等同于拖拽定位、吸附网格、重叠冲突处理、跨轨移动或完整剪贴板。
- 2026-06-30 新增、2026-07-01 扩展：已扩展 `AppMidiClipActions`，新增目标 MIDI 片段移动到目标乐器轨入口。JUCE 首屏可用已有“目标乐器轨”和“目标 MIDI 片段”下拉框，把当前选中 MIDI 片段移动到另一条乐器轨；该动作只改变片段所属 `trackId`，不改变片段起点、长度和内部 MIDI 音符相对 tick；同轨移动、缺失目标轨、音频目标轨和音频片段会被拒绝且不标脏工程。2026-07-01 起，成功跨轨移动通过核心 `MoveClipToTrackCommand` 进入 `AppProjectSession` 命令历史；撤销会恢复源轨，重做会恢复目标轨，并保留片段与音符身份。CTest 覆盖成功跨轨移动、同轨拒绝、缺失目标轨拒绝、音频目标轨拒绝、音频片段拒绝和跨轨移动撤销/重做。当前能力只是按钮式跨乐器轨移动，不等同于拖拽跨轨、重叠冲突处理、跨类型转换或批量片段移动。
- 2026-06-30 新增、2026-07-01 扩展：已扩展 `AppMidiClipActions`，新增目标 MIDI 片段片尾缩短一拍入口。JUCE 首屏可点击“缩短片尾”把当前选中 MIDI 片段右边界向左缩短一个四分音符 tick 步长；修剪只改变片段长度，不改变片段起点和内部 MIDI 音符相对 tick；片段不足一拍、缺失片段、音频片段和缩短后会截掉已有音符的情况会被拒绝且不标脏工程。2026-07-01 起，成功片尾缩短通过核心 `TrimClipEndCommand` 进入 `AppProjectSession` 命令历史；撤销会恢复旧长度，重做会恢复缩短后的长度，并保留片段与音符身份。CTest 覆盖成功缩短、过短拒绝、缺失片段拒绝、音频片段拒绝、音符边界拒绝和片尾缩短撤销/重做。当前能力只是按钮式右边界向内修剪，不等同于拖拽修剪、向外扩展、自动截断音符或完整时间线编辑器。
- 2026-06-30 新增、2026-07-01 扩展：已扩展 `AppMidiClipActions`，新增目标 MIDI 片段片尾延长一拍入口。JUCE 首屏可点击“延长片尾”把当前选中 MIDI 片段右边界向右延长一个四分音符 tick 步长；延长只增加片段外壳长度，不改变片段起点和内部 MIDI 音符相对 tick；缺失片段、音频片段和时间范围溢出会被拒绝且不标脏工程。2026-07-01 起，成功片尾延长通过核心 `SetClipTimingCommand` 进入 `AppProjectSession` 命令历史；撤销会恢复旧长度，重做会恢复延长后的长度，并保留片段与音符身份。CTest 覆盖成功延长、缺失片段拒绝、音频片段拒绝和片尾延长撤销/重做。当前能力只是按钮式右边界向外延长，不等同于拖拽修剪、重叠冲突处理或完整时间线编辑器。
- 2026-06-30 新增、2026-07-02 扩展：已扩展 `AppMidiClipActions`，新增目标 MIDI 片段片头缩短和片头延长一拍入口。JUCE 首屏可点击“缩短片头”把当前选中 MIDI 片段左边界向右移动一拍，或点击“延长片头”把左边界向左移动一拍；应用层会同步重算 MIDI 音符相对 tick，让保留下来的音符绝对播放时间不变。2026-07-02 起，核心 `SetMidiClipStartKeepingNoteTimesCommand` 会把片段左边界与内部音符相对 tick 迁移作为一次原子命令执行，片头缩短和片头延长可通过会话层撤销/重做。片头缩短会截掉已有音符、片头延长越过时间线 0、缺失片段和音频片段会被拒绝且不标脏工程。CTest 覆盖成功缩短片头、成功延长片头、片头缩短音符边界拒绝、核心片头命令撤销/重做，以及应用层片头缩短/延长撤销/重做。当前能力只是按钮式固定步长左边界编辑，不等同于拖拽修剪、自动截断音符、重叠冲突处理、音频素材偏移或完整时间线编辑器。
- 2026-06-29 新增、2026-06-30 扩展 `AppMidiNoteActions` 和 `AppTimelineStatus`，为首屏提供可测试的默认 MIDI 音符添加、删除末尾音符、复制末尾音符、末尾音符半音升降、末尾音符力度增减、末尾音符长度增减、末尾音符起点左右移动和末尾音符摘要入口。JUCE 首屏可选择目标 MIDI 片段并追加一个 C4 四分音符，音符会按片段内已有音符顺序追加；也可删除该片段里时间位置最后的音符，复制末尾音符并把副本放在源音符之后，把末尾音符升高/降低一个 MIDI note number，按 8 调整末尾音符 velocity，按十六分音符 tick 延长/缩短末尾音符，或按十六分音符 tick 左移/右移末尾音符起点；时间线摘要会显示当前末尾音符的起点、长度、音高和力度。复制时会保留源音符的长度、音高、力度和通道，并分配新的 note id。音高调整到 0-127 范围外、力度调整到 1-127 范围外、长度短于十六分音符、长度超出片段右边界、起点早于片段开头、移动后右边界超出片段、复制后右边界超出片段、空片段、缺失片段和音频片段会被拒绝且不标脏工程。CTest 覆盖成功添加、连续追加、复制末尾音符、缺失片段、音频片段拒绝、片段已满拒绝、复制空间不足拒绝、删除末尾音符、升高/降低末尾音符、增强/减弱末尾音符力度、延长/缩短末尾音符长度、左移/右移末尾音符起点、末尾音符摘要、空 MIDI 片段不显示末尾音符摘要、音高上下界拒绝、力度上下界拒绝、长度上下界拒绝、起点移动上下界拒绝、空片段音高/力度/长度/起点/复制调整拒绝、缺失片段力度/长度/起点/复制调整拒绝、音频片段力度/长度/起点/复制调整拒绝和删除失败路径。当前能力只是默认音符追加、末尾音符撤回、末尾音符复制、末尾音符音高微调、末尾音符力度步进微调、末尾音符长度步进微调、末尾音符起点步进微调和只读摘要，不等同于自由音符编辑、钢琴卷帘、量化、力度曲线、任意音符力度编辑、任意音符长度编辑、任意音符位置编辑或任意音符选择删除。
- 2026-07-02：`AppMidiNoteActions` 当前已有 MIDI 音符按钮动作已迁移到 `AppProjectSession` 命令历史：创建和复制通过 `AddMidiNoteCommand`，删除通过 `DeleteMidiNoteCommand`，音高通过 `SetMidiNotePitchCommand`，力度通过 `SetMidiNoteVelocityCommand`，长度和起点通过 `SetMidiNoteTimingCommand`。撤销/重做会保留同一个 note id，并恢复音符起点、长度、音高、力度和通道；复制会恢复同一个副本 note id，删除会恢复被删的末尾音符。CTest 新增并通过 MIDI 音符创建、删除、复制、音高、力度、长度和起点撤销/重做覆盖。当前仍不代表自由音符编辑、钢琴卷帘、任意音符选择、量化、力度曲线或完整编辑体验已经完成。
- 2026-06-29：已新增 `AppPlaybackActions`，为首屏提供可测试的播放/停止运行态入口。JUCE 首屏已有“播放”和“停止”按钮；播放会准备空的项目播放会话并切换内部 `Transport` 到播放状态，停止会复用核心 `StopPlaybackCommand` 的安全停止边界。CTest 覆盖开始播放、停止播放、未开始时停止、播放状态描述和不标脏工程。当前能力只是桌面运行态播放控制入口，不代表真实音频设备回调、声卡输出、插件播放或硬件 MIDI 输出已经完成。
- 2026-06-29：已新增 `advanceAppPlaybackForUiTick` 和首屏播放状态行，JUCE 首屏在播放中会用 `Timer` 推进一个静音 UI block 并显示 sample/秒数位置；停止状态下推进返回 no-op，不准备运行态也不标脏工程。CTest 覆盖播放中推进、停止时不推进、停止后保持位置和状态摘要秒数。当前 UI Timer 只用于可见播放头推进，不等同于真实音频设备回调、实时输出、插件输出或硬件 MIDI 输出。
- 2026-06-29：已新增 `rewindAppPlaybackToStart`，为首屏提供可测试的“回到开头”运输控制。该入口复用核心 `SeekPlaybackCommand`，播放中回到 sample 0 后继续保持播放状态，停止状态下只移动位置，不启动播放；JUCE 首屏新增“回到开头”按钮。CTest 覆盖播放中回到开头、停止后回到开头、初始运行态回到开头和不标脏工程。当前能力只是播放头运行态 seek，不代表时间线选区、循环定位、音频设备定位或真实声卡 seek 已完成。
- 2026-06-29：已新增 `toggleAppPlayback`，为首屏和后续菜单、快捷键、AI 工具提供统一的播放/停止切换入口。JUCE 首屏支持空格键切换播放和停止，内部仍复用已有开始与安全停止边界，并按结果启动或停止 UI Timer。CTest 覆盖停止态切换为播放、播放态切换为停止、推进后切换停止保留当前位置和不标脏工程。当前快捷键只覆盖桌面壳焦点下的基础切换，不代表完整菜单系统、全局快捷键或真实音频设备控制已完成。
- 2026-06-29 新增、2026-07-01 扩展：已新增并扩展 `AppRecentProjects`，为后续菜单、最近文件入口和本地设置存储提供应用层最近工程列表。该列表按最新在前去重、限制最大数量，并用 UTF-8 文本设置文件保存和读取；JUCE 首屏已显示最近工程区域，可选择最近工程并打开，打开、保存和另存为成功后会更新本地最近工程设置。CTest 覆盖去重限长、中文路径保存读取、缺失设置文件空列表、展示快照、设置保存失败时保留内存列表、按编号打开、dirty 阻断、缺失选择和缺失文件失败。当前最近工程已经出现在基础文件菜单中，但仍不会写入 `.trackloom` 工程格式。
- 2026-07-01 新增、2026-07-04 扩展：已新增并扩展 `AppMainMenu`，把文件菜单、编辑菜单、轨道菜单、片段菜单、播放菜单和最近工程动态菜单项整理成可测试的应用层主菜单快照。JUCE 首屏已接入 `MenuBarComponent`，文件菜单可触发新建工程、打开工程、保存、另存为和最近工程打开，编辑菜单可触发工程撤销和重做，轨道菜单可触发添加乐器轨、添加音频轨、添加文件夹轨，以及依赖当前目标选择的重命名所选乐器轨、删除所选乐器轨、上移所选乐器轨、下移所选乐器轨、切换所选乐器轨静音/独奏/禁用/隐藏、删除所选音频轨、上移所选音频轨、下移所选音频轨和切换所选音频轨静音/独奏/禁用/隐藏；片段菜单可触发所选 MIDI/音频片段的重命名、删除、复制、拆分、移动到目标轨、一拍左右移动、片尾缩短/延长和片头缩短/延长；播放菜单可触发播放、停止和回到开头。菜单项启用状态读取现有应用会话、命令历史、播放运行态、最近工程列表和桌面壳传入的目标轨道/片段选择，轨道移动命令会按轨道边界禁用，状态切换命令随对应有效轨道选择启用，普通片段命令随对应有效片段选择启用，片段跨轨移动命令还要求目标轨有效且不同于当前所属轨，实际命令继续复用原有动作。CTest 覆盖文件/编辑/轨道/片段/播放菜单分组、稳定 command id、撤销/重做历史状态、基础轨道创建项、选择依赖轨道命令启用状态、选择依赖片段命令启用状态、空最近工程禁用行、播放中启用状态和最近工程 command id 往返。当前能力只是基础文件/编辑/轨道/片段/播放菜单栏和当前目标轨道/片段的部分菜单入口，不等同于完整菜单体系、全局快捷键、菜单栏里的全部编辑命令、正式未保存确认弹窗或平台原生命令路由。
- 2026-07-01 新增、2026-07-04 扩展：已新增并扩展 `AppCommandDispatcher`，把基础菜单 command id 解析成稳定 `AppCommandKind`，并通过注入的处理函数执行新建、打开、保存、另存为、撤销、重做、添加乐器轨、添加音频轨、添加文件夹轨、重命名所选乐器轨、删除所选乐器轨、上移所选乐器轨、下移所选乐器轨、切换所选乐器轨静音、切换所选乐器轨独奏、切换所选乐器轨禁用、切换所选乐器轨隐藏、删除所选音频轨、上移所选音频轨、下移所选音频轨、切换所选音频轨静音、切换所选音频轨独奏、切换所选音频轨禁用、切换所选音频轨隐藏、所选 MIDI/音频片段重命名、删除、复制、拆分、移动到目标轨、一拍左右移动、片尾缩短/延长、片头缩短/延长、播放、停止、回到开头和最近工程打开。JUCE 菜单点击现在只构造命令处理回调表并调用分发器，不再在 `menuItemSelected` 中复制普通命令、选择依赖轨道/片段命令和最近工程动态命令的映射。CTest 覆盖普通命令分发、撤销/重做分发、轨道创建分发、选择依赖轨道命令分发、选择依赖片段命令分发、最近工程编号传递、未知命令拒绝和缺失处理函数拒绝。当前能力只是基础菜单的命令分发边界，不等同于完整快捷键体系、完整编辑菜单、完整轨道菜单或 AI 工具命令注册表。
- 2026-07-02：已新增并扩展 `AppCommandPalette`，把 `AppMainMenuStatus` 展开为命令面板可用的应用层数据列表，保留菜单组名、标签、启用状态、稳定 command id 和可选快捷键显示文本，跳过分隔线与“暂无最近工程”等无 command id 的提示行，支持按菜单组名、标签或快捷键显示文本进行轻量过滤，并可选择过滤结果中的第一个 enabled 命令但不执行命令。选择结果现在提供稳定分类，可区分已选中可执行命令、没有匹配命令、只有禁用匹配命令；确认执行入口会先选择，再通过 `AppCommandDispatcher` 执行，禁用命令、无匹配命令和分发失败都不会静默成功。快捷键显示文本来自 `AppCommandShortcuts` 的默认绑定表；禁用命令仍可显示快捷键，重做等多快捷键命令会合并显示，命令面板搜索也复用这些显示文本。CTest 覆盖基础菜单展开、禁用命令保留、轨道组搜索、动态最近工程命令、提示行过滤、跳过 disabled 命令、缺失查询无选择、禁用匹配与无匹配分类、首个 enabled 命令选择、命令面板激活执行、禁用或无匹配不执行、分发失败分类、快捷键标签填充、多快捷键标签合并和快捷键文本搜索。该阶段能力只是命令面板数据层、选择规则、激活边界、失败分类、快捷键显示数据和快捷键搜索数据，不等同于可见命令面板弹窗、自定义快捷键、完整编辑菜单或 AI 工具命令注册表。
- 2026-07-03：已新增并扩展 `AppCommandPaletteSession`，作为可见命令面板 UI 前的应用层临时状态容器。它保存打开/关闭状态、查询文本、过滤后的命令列表和当前高亮索引；打开时清空查询并高亮第一个 enabled 命令，查询变化时复用 `filterAppCommandPalette` 刷新结果并重置高亮，上下移动只在 enabled 命令之间循环并跳过 disabled 项，关闭时清空查询、过滤结果和高亮。当前也提供 `activateHighlightedAppCommandPaletteCommand`，按当前高亮项执行命令并复用 `AppCommandDispatcher`，不会根据 query 重新选择第一条结果；关闭状态、无高亮、禁用匹配和分发失败都会返回稳定结果。`describeAppCommandPaletteSession` 可把会话转换成 UI 行快照，保留 command id、group、label、快捷键、enabled、高亮标记和统一空结果提示。CTest 覆盖打开初始状态、禁用项跳过、按快捷键查询后禁用匹配无高亮、查询变化重置高亮、上下移动循环、关闭清空状态、高亮项执行、关闭或禁用状态不执行、缺失 handler 时报告分发失败，以及展示行高亮、禁用行和空结果提示。该阶段能力只是命令面板会话状态层、高亮确认边界和展示快照，不等同于当时尚未完成的可见命令面板弹窗、键盘焦点管理、自定义快捷键或 AI 工具命令注册表。
- 2026-07-03：已接入并扩展可见命令面板浮层。JUCE 桌面壳可通过“工具/命令面板...”或 `Ctrl+K`/`Ctrl+Shift+P` 打开命令面板，显示搜索框、最多 6 行命令结果、disabled 文案、当前范围提示和高亮行；Enter 执行当前高亮命令，点击可见行会按当前过滤结果重新确认并执行，Esc 关闭。Home/End 会跳到第一/最后 enabled 命令，PageUp/PageDown 按可见行数跨页并跳过 disabled 目标。2026-07-03 继续新增 `scrollVisibleRowsByWheelSteps` 和独立的 `firstVisibleRowIndex`，鼠标滚轮现在只滚动可见窗口，不移动当前高亮命令；查询变化和键盘导航会清除独立窗口偏移并回到高亮驱动窗口。随后新增 `highlightCommandById`，鼠标悬停可见行时只高亮当前过滤结果里的 enabled 命令，不执行命令，也不让 disabled、缺失或关闭状态改变当前高亮。CTest 覆盖可见行文本、点击执行、悬停高亮、范围提示、Home/End、PageUp/PageDown、鼠标滚轮高亮步进和独立可见窗口滚动。当前仍不等同于自定义快捷键、命令分类图标、视觉截图验收或触控板惯性累计。
- 2026-07-01 新增、2026-07-06 扩展：已新增 `AppCommandShortcuts`，把第一批平台无关文件快捷键映射到现有菜单 command id：`Ctrl+N` 新建工程、`Ctrl+O` 打开工程、`Ctrl+S` 保存、`Ctrl+Shift+S` 另存为；2026-07-02 起，`Ctrl+Z` 映射工程撤销，`Ctrl+Y` 和 `Ctrl+Shift+Z` 映射工程重做。JUCE `keyPressed` 现在把 `KeyPress` 转成 `AppShortcutChord`，再通过 `AppCommandDispatcher` 执行；Space 播放/停止切换继续走 `toggleAppPlayback`，因为它是切换语义，不等同于播放菜单中的独立播放和停止命令。2026-07-03 新增 `AppShortcutContext`，命令面板打开时，未被命令面板专门处理的全局快捷键不再触发保存、重新打开面板或其他主窗口命令。2026-07-06 新增自定义快捷键活动表基础和 `AppShortcutSettings` 本地设置读写基础，非冲突覆盖可替换同一命令旧绑定，冲突覆盖会报告并保留原绑定，用户覆盖项可保存为本地 UTF-8 文本并重新加载；JUCE 桌面壳启动时加载本机 `TrackLoom/shortcuts.txt`，并让键盘分发和命令面板标签共用同一份活动绑定表；`AppShortcutStatus` 可为后续设置界面生成当前命令快捷键、是否自定义、冲突明细和摘要。CTest 覆盖文件快捷键、撤销/重做快捷键、未注册组合拒绝、命令面板快捷键显示、命令面板打开上下文中的全局快捷键抑制、替换默认绑定、冲突拒绝、无默认命令新增绑定、设置保存加载、缺失设置、冲突设置、坏行忽略、活动表命令面板标签、上下文内自定义快捷键分发和快捷键状态快照。当前能力只是第一批窗口内文件快捷键、基础撤销/重做快捷键、命令面板输入焦点隔离、自定义快捷键数据层、本地设置读写、启动加载和状态快照基础，不等同于全局快捷键、完整编辑快捷键、快捷键设置界面或平台原生菜单快捷键展示。
- 2026-07-01 新增、2026-07-02 扩展：已在 `AppProjectSession` 中新增应用层工程命令历史入口，可执行核心 `Command`，查询、撤销和重做命令历史；成功命令才标脏并进入历史，失败命令不标脏不入栈，新建或打开工程会清空历史。为避免旧直接编辑入口造成错误撤销，`editProject()` 现在会清空旧历史。首批已把 `AppTrackActions` 的轨道创建、删除、重命名和上下移动迁移到核心命令执行；`AppTrackStateActions` 的静音、独奏、禁用和隐藏也已迁移到核心状态命令；`AppMidiClipActions` 的默认 MIDI 片段创建、删除、重命名、复制、中点拆分、一拍左右移动、跨轨移动、片尾缩短/延长和片头缩短/延长已迁移到核心片段命令；`AppAudioClipActions` 当前已有空音频片段外壳动作也已迁移到核心片段命令；`AppMidiNoteActions` 当前已有默认音符创建、末尾音符删除、末尾音符复制、音高、力度、长度和起点微调已迁移到核心音符命令。因此这些轨道动作、轨道状态动作、默认 MIDI 片段创建/删除/重命名/复制/拆分/一拍移动/跨轨移动/片尾缩短/片尾延长/片头缩短/片头延长、空音频片段创建/删除/重命名/复制/拆分/跨轨移动/一拍移动/片尾缩短/片尾延长/片头缩短/片头延长，以及 MIDI 音符创建/删除/复制/音高/力度/长度/起点微调可以通过会话历史撤销/重做。基础编辑菜单已新增“撤销”和“重做”，启用状态来自 `canUndoProjectEdit()` / `canRedoProjectEdit()`，命令 id 通过 `AppMainMenuCommand::UndoProject` / `RedoProject` 暴露，并由 `AppCommandDispatcher` 注入回调执行；基础撤销/重做快捷键也复用同一 command id。JUCE 入口只刷新提示和界面，不把播放、最近工程或文件选择器状态纳入工程撤销栈。CTest 覆盖命令执行、失败命令、历史重置、直接编辑清空历史、轨道创建撤销/重做、静音撤销/重做、隐藏撤销/重做、MIDI 片段创建/删除/重命名/复制/拆分/一拍移动/跨轨移动/片尾缩短/片尾延长/片头缩短/片头延长撤销/重做、音频片段创建/删除/重命名/复制/拆分/跨轨移动/一拍移动/片尾缩短/片尾延长/片头缩短/片头延长撤销/重做、MIDI 音符创建/删除/复制/音高/力度/长度/起点微调撤销/重做，以及编辑菜单和快捷键撤销/重做分发。该阶段能力当时仍不等同于可见命令面板 UI、完整编辑菜单、音频素材编辑、钢琴卷帘或自由音符编辑。
- 2026-07-02：音频片段动作完成当前外壳动作的同一命令历史接入。默认空音频片段创建通过 `AddClipCommand`，音频片段重命名通过 `RenameClipCommand`，音频片段删除通过 `DeleteClipCommand`，音频片段复制通过 `DuplicateClipCommand`，中点拆分通过 `SplitClipCommand`，跨音频轨移动通过 `MoveClipToTrackCommand`，一拍移动、片尾延长和片头延长通过 `SetClipTimingCommand`，片尾缩短通过 `TrimClipEndCommand`，片头缩短通过 `TrimClipStartCommand`；这些当前已有空音频片段外壳动作可通过会话历史撤销/重做，并保留稳定 clip id。CTest 覆盖上述音频片段动作撤销/重做。当前仍不代表音频文件导入、素材引用、素材偏移或真实音频修剪已经完成。

范围：

- 工程新建、打开、保存；
- 轨道列表；
- 基础时间线视图；
- 运输控制；
- 简单 MIDI 片段显示和编辑入口；
- 错误信息显示；
- 本地设置存储。

验收：

- 用户能新建工程、创建轨道、创建 MIDI 片段、播放、停止、保存并重新打开。
- 没有 AI 配置时仍可完成上述流程。
- 崩溃或保存失败不会破坏已有工程文件。

### 阶段 D：循环编曲器首版

目标：提供 TrackLoom 的第一种特色快速编曲体验。

范围：

- 循环片段组织；
- 循环播放区域；
- MIDI 循环编辑；
- 将循环内容展开到普通时间线；
- 与 tempo、拍号和播放控制保持一致。

验收：

- 非乐理用户能在可视界面中完成短循环编曲。
- 循环边界长音符不会悬挂。
- 展开后的普通时间线内容可继续编辑和保存。

### 阶段 E：音频文件、导入导出与 stems

目标：让工程进入真实音乐素材工作流。

范围：

- 音频文件导入；
- 基础波形显示；
- 音频片段播放；
- stems 导出；
- MIDI Type 1/Type 0 导入导出；
- 缺失素材提示。

验收：

- 导入素材不破坏原文件。
- 导出不会默认量化或改写演奏时间。
- 缺失素材、未知 MIDI 信息和不支持内容有明确提示。

### 阶段 F：VST3 插件与采样器

目标：接入真实乐器和效果器。

范围：

- VST3 扫描辅助进程；
- 插件状态保存；
- 插件宿主进程隔离；
- 基础插件路由；
- 插件崩溃恢复；
- 采样器接口预留。

验收：

- 插件扫描失败不影响基础 DAW。
- 插件崩溃不破坏工程文件。
- 插件状态可保存、恢复和迁移。

### 阶段 G：AI 服务与项目级对话

目标：让 AI 通过受控命令参与工程创作。

范围：

- Python 本地 AI 服务；
- 第三方 API 配置；
- API key 安全存储；
- 工程命令工具接口；
- 项目级对话；
- 生成文件生命周期；
- 隐私权限检查；
- AI 结果验证和冲突处理。

验收：

- 没有 AI 配置时基础 DAW 可用。
- AI 不能绕过工程命令和隐私规则。
- 结果冲突时不会覆盖用户新修改。
- API key 不进入仓库、工程文件、日志或备份包。

### 阶段 H：同步、备份与恢复

目标：提供不依赖官方服务器的数据保护能力。

范围：

- 本地同步目录；
- HTTPS WebDAV；
- SFTP；
- 版本化备份包；
- 文件清单和校验；
- 选择性恢复；
- 同步冲突处理；
- 删除标记。

验收：

- 同步默认关闭。
- 凭据存入系统安全凭据。
- 恢复前可预览并选择恢复范围。
- 删除不会被其他设备无意恢复。

## 18. 当前未定决策

以下事项尚未最终选择，开发前需要单独确认或通过小原型验证：

- JUCE UI 的具体布局和交互语言。
- 工程文件扩展名、单文件工程包格式和压缩策略。
- 首批 AI 供应商和模型能力声明格式。
- 音频分离、转录、扒谱的技术路线。
- Windows 音频后端优先级，包括 WASAPI、ASIO 和低延迟目标。
- VST3 独立宿主进程的 IPC 协议。
- 项目级对话和生成文件的本地数据库方案。
- 备份包是否默认加密，以及加密密钥恢复策略。
- 同步冲突的 UI 表达方式。
- `AGPL-3.0-only`（SPDX）之外是否需要额外第三方许可证说明模板。
- 项目标识、图标、商标和发布名称。

未定决策不得阻塞已明确的底层核心开发，但进入相关阶段前必须转化为明确需求和验收标准。

## 19. 后续维护清单

每次需求或规划变化时，按顺序检查：

1. 是否改变产品边界。
2. 是否影响实时线程安全。
3. 是否影响工程格式和迁移。
4. 是否影响隐私授权和数据流向。
5. 是否需要新增或调整测试。
6. 是否改变阶段路线或验收标准。
7. 是否需要同步更新 `AGENTS.md` 中的长期协作原则或经验证实教训。
8. 是否需要同步更新 Obsidian Codex 记忆库的 TrackLoom 项目页。

本文档更新后，旧的分散需求与规划草稿不再作为判断依据。
