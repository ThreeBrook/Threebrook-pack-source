// 查看器的图像读取与显示准备（T8，与界面无关，可单独测试）。
//
// 读取：.iis 先在内存还原（SHA-256 核对）为原文件；原文件按内容识别（与压缩用同一套识别），取出像素：
//   BMP（8 位调色板 / 24 / 32 位）、TIFF（多页、条带 / 图块、8 / 16 位、未压缩 / LZW / ZIP / PackBits）、RAW（需参数）；
//   其他图像（PNG、JPEG、GIF 等）用 Windows 自带的解码器（WIC）预览。
// 包（.iisz）内的文件用“包路径|条目序号”表示，读取时从包里取出并核对 SHA-256。
// 显示：16 位窗宽窗位映射到 8 位；Bayer 可简单（双线性）去马赛克预览。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace iis::pkg {
class Reader;
}

namespace iis::view {

// 无文件头 RAW 的参数（来自界面输入）
struct RawParams {
    uint32_t width = 0, height = 0;
    std::string pixel_format = "Mono8";
    uint64_t offset = 0, stride = 0;  // stride 为 0 表示紧密排列
};

struct Image {
    uint32_t width = 0, height = 0;
    int channels = 1;            // 1 灰度 / 3 RGB / 4 RGBA
    int bits = 8;                // 样本取值范围的位数（窗宽上限 2^bits - 1）
    std::vector<uint16_t> px;    // 按像素交织
    std::string bayer;           // 单通道 Bayer 图的左上 2×2 排列（RGGB / GRBG / GBRG / BGGR），否则为空
};

struct Loaded {
    bool ok = false;
    std::string error;
    bool need_raw_params = false;  // 无文件头 RAW：需要用户给出宽、高、像素格式
    bool raw_auto = false;         // 无文件头 RAW 的宽、高、像素格式是自动识别的（不对时可手动输入）
    std::vector<Image> pages;
    std::string kind;               // 如“.iis 压缩图像（原为 TIFF）”
    std::string desc;               // 识别概要：格式、尺寸、位深、压缩、Bayer
    uint64_t file_size = 0, orig_size = 0;
    std::string sha256;             // 原文件 SHA-256（.iis 为容器记录并已核对的值）
    bool from_iis = false;
    bool in_package = false;        // 来自 .iisz 包（已取出并核对 SHA-256）
    std::string name;               // 文件名（包内文件为条目名）
    std::vector<uint8_t> original;  // 原文件字节（.iis 为还原结果），供“另存为原文件”
};

Loaded load_file(const std::string& path_utf8, const RawParams* raw = nullptr);
Loaded load_bytes(std::vector<uint8_t> data, const std::string& name, const RawParams* raw = nullptr);
// 普通路径同 load_file；“包路径|条目序号”从包中取出
Loaded load_any(const std::string& path_utf8, const RawParams* raw = nullptr);

// 打开包（按路径缓存，可在多个线程中共用）；失败返回空并给出原因
std::shared_ptr<const pkg::Reader> open_package(const std::string& path_utf8, std::string* why = nullptr);
// 包内条目的虚拟路径
std::string package_item(const std::string& package_utf8, size_t index);
// 包的预览图（资源管理器与查看器的缩略图）：按“层次浅的在前、同层按名称”取前 4 张能显示的图像
// （跳过子包与超过 256 MB 的文件，最多试 12 个），只读这几张图的条目，不读整个包。
// 4 张拼成 2×2，3 张为左一右二，2 张左右并排（各自居中裁满格子），1 张按原比例缩放；
// 拼图为 size × size，格子之间的缝透明（alpha 0）。没有能显示的图像时返回 false
bool package_mosaic(const pkg::Reader& pk, int size, std::vector<uint8_t>& rgba, int& w, int& h);

struct Window {
    double lo = 0, hi = 255;
};
// 自动窗：取 0.1% 与 99.9% 分位
Window auto_window(const Image& img);

// 转为 RGBA8（width × height × 4）。demosaic 为真且 bayer 非空时做双线性去马赛克；
// white_balance 为真时再按灰度世界假设平衡 R / B（只影响预览，原始数据不变）
void to_rgba(const Image& img, Window w, bool demosaic, const std::string& bayer, std::vector<uint8_t>& rgba,
             bool white_balance = false);

// 缩略图：等比缩到最长边 max_side 以内（区域平均），自动窗
void thumbnail(const Image& img, int max_side, std::vector<uint8_t>& rgba, int& tw, int& th);

// 是否像查看器能打开的文件或包（按扩展名粗筛目录列表；真正的格式仍按内容识别）
bool listable(const std::string& name);

// 批量还原（.iis → 原文件）：src 为目录（recursive 时含子目录）或单个 .iis 文件，还原到 out 下并保持相对层次。
// 每个文件都核对原文件的 SHA-256（不一致不留下文件）；目标已存在的不覆盖；.iis 保留不动。
// progress（可空）在每个文件处理后调用，可用于显示进度
struct RestoreResult {
    int total = 0, ok = 0, skipped = 0, failed = 0;
    std::vector<std::string> errors;  // 最多 100 条：相对路径：原因
};
RestoreResult restore_tree(const std::string& src_utf8, const std::string& out_utf8, bool recursive,
                           const std::function<void(const RestoreResult&)>& progress = {});

// dir 若在本机 IIS 服务的某个监控目录内，返回那个监控目录（还原到那里的原文件会被服务重新压缩）；否则返回空。
// 读服务配置：HKLM\SOFTWARE\IIS\Install 记下的数据目录，或 %ProgramData%\IIS 下的 config.json；没装服务时返回空
std::string watched_root(const std::string& dir_utf8, const std::string& config_utf8 = "");

}  // namespace iis::view
