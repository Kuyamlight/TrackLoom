#pragma once

#include <optional>
#include <string>
#include <vector>

namespace trackloom {

// TrackType 只描述轨道在工程模型里的基本职责。
// 文件夹轨只负责分类和折叠，不代表音频总线；这个边界以后会影响混音和路由设计。
enum class TrackType {
    Instrument,
    Audio,
    Folder
};

// Track 是工程里最小的轨道数据。
// 第一阶段暂不保存插件、片段或自动化，避免在核心边界稳定前过早扩大模型。
struct Track {
    std::string id;
    std::string name;
    TrackType type;
};

// Project 是 TrackLoom 自有工程格式的最小核心状态。
// 后续 UI、AI 和导入器都应通过命令系统修改它，避免绕过验证、撤销和历史记录。
class Project {
public:
    static constexpr int currentFormatVersion = 1;

    explicit Project(std::string name = "Untitled");

    int formatVersion() const;
    const std::string& name() const;
    void rename(std::string newName);

    const std::vector<Track>& tracks() const;
    std::optional<Track> findTrackById(const std::string& id) const;

    // createTrack 用于创建全新轨道，并分配本工程内稳定的可读 ID。
    Track createTrack(std::string name, TrackType type);

    // insertExistingTrack 用于撤销重做或读取文件时恢复已有 ID 的轨道。
    bool insertExistingTrack(const Track& track);
    bool removeTrackById(const std::string& id);

private:
    int formatVersion_ = currentFormatVersion;
    std::string name_;
    std::vector<Track> tracks_;
    int nextTrackNumber_ = 1;

    // 读取旧轨道 ID 后推进计数器，避免下一次新建轨道撞上已有 ID。
    void observeTrackId(const std::string& id);
};

std::string toString(TrackType type);
std::optional<TrackType> trackTypeFromString(const std::string& value);

}
