/*
 * iis.h — IIS 内核库 C 接口（iis.dll / libiis.so）
 *
 * 约定
 * - 除特别说明外，函数返回 int 状态码：IIS_OK（0）表示成功，负数为 IIS_E_*。
 *   出错时 iis_last_error() 给出中文说明（线程局部，下一次调用前有效）。
 * - 库分配的输出放在 iis_buffer 里，只能用 iis_buffer_free() 释放。
 * - 像素排列：行与行紧密相接、无行填充；有效位 ≤ 8 时每样本 1 字节，9~16 位时每样本 2 字节
 *   （本机字节序，x86 为小端）；多分量按像素交织（RGBRGB…）。
 * - 全部运算为确定性整数运算；同一输入在任何平台、任何线程数下输出相同。
 * - 所有函数可在多线程中并发调用（各自使用自己的缓冲区与上下文）。
 */
#ifndef IIS_IIS_H
#define IIS_IIS_H

#include <stddef.h>
#include <stdint.h>

#if defined(IIS_STATIC)
#  define IIS_API
#elif defined(_WIN32)
#  if defined(IIS_BUILDING)
#    define IIS_API __declspec(dllexport)
#  else
#    define IIS_API __declspec(dllimport)
#  endif
#else
#  define IIS_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 版本与状态 ---- */

#define IIS_ABI_VERSION 3 /* C 接口不兼容变更时加一（2：iis_compress_options 增加 min_saving；3：增加 raw_auto） */

enum {
    IIS_OK = 0,
    IIS_E_INVALID_ARG = -1,   /* 参数错误：空指针、尺寸或位深不合法、像素值超出位深 */
    IIS_E_NO_MEMORY = -2,     /* 内存不足 */
    IIS_E_CORRUPT = -3,       /* 输入数据损坏或不是预期格式 */
    IIS_E_UNSUPPORTED = -4,   /* 合法但本库不支持（如有损 JPEG-LS 码流） */
    IIS_E_SIZE_MISMATCH = -5, /* 数据长度与声明不一致 */
    IIS_E_CODEC = -6,         /* 编码器内部失败 */
    IIS_E_IO = -7,            /* 文件读写失败 */
    IIS_E_NOT_FOUND = -8,     /* 要找的段或文件不存在 */
    IIS_E_BUSY = -9,          /* 文件正被其他程序占用（写入中、被独占打开），稍后重试 */
    IIS_E_CONFLICT = -10,     /* 目标文件已存在，不覆盖（写入 / 还原 / 提交 .iis 时） */
    IIS_E_NO_SPACE = -11,     /* 磁盘已满 */
    IIS_E_INTERNAL = -99      /* 未预期的内部错误 */
};

/* 库版本，如 "0.1.0" */
IIS_API const char* iis_version(void);
/* 返回 IIS_ABI_VERSION；调用方可据此检查头文件与动态库是否匹配 */
IIS_API int iis_abi_version(void);
/* 构建信息：库版本、编码库版本、CRC32C 实现、编译器，如
 * "iis 0.1.0; charls 2.4.5; zstd 1.5.7; crc32c sse4.2; msvc 1944" */
IIS_API const char* iis_build_info(void);
/* 状态码的简短中文名 */
IIS_API const char* iis_status_str(int status);
/* 本线程最近一次失败的详细说明；没有则为空串 */
IIS_API const char* iis_last_error(void);

/* ---- 缓冲区 ---- */

typedef struct iis_buffer {
    uint8_t* data;
    size_t size;
} iis_buffer;

/* 释放库分配的缓冲区并清零；对空缓冲区安全 */
IIS_API void iis_buffer_free(iis_buffer* buf);

/* ---- 校验 ---- */

typedef struct iis_sha256_ctx {
    uint32_t state[8];
    uint64_t total;
    uint8_t block[64];
    uint32_t used;
} iis_sha256_ctx;

IIS_API void iis_sha256_init(iis_sha256_ctx* ctx);
IIS_API void iis_sha256_update(iis_sha256_ctx* ctx, const void* data, size_t size);
IIS_API void iis_sha256_final(iis_sha256_ctx* ctx, uint8_t digest[32]);
IIS_API void iis_sha256(const void* data, size_t size, uint8_t digest[32]);

/* CRC32C（Castagnoli）。首次传 crc = 0；分段计算时把上一段结果作为 crc 传入。
 * CPU 支持 SSE4.2 时自动使用硬件指令，结果与软件实现相同。 */
IIS_API uint32_t iis_crc32c(uint32_t crc, const void* data, size_t size);

/* ---- JPEG-LS（CharLS，仅无损）---- */

typedef struct iis_image_info {
    uint32_t width;
    uint32_t height;
    uint32_t components;      /* 1、3 或 4 */
    uint32_t bits_per_sample; /* 2~16 */
} iis_image_info;

/* 每样本字节数（1 或 2）与紧密排列的像素总字节数 */
IIS_API size_t iis_image_bytes(const iis_image_info* info);

/* 无损编码。pixels 长度必须等于 iis_image_bytes(info)；样本值不得超过 2^bits-1。
 * 3 分量按像素交织（ILV=1 sample）、4 分量按行交织（ILV=line），与阶段 0 的 imagecodecs
 * 输出逐字节一致；解码输出总是按像素交织。 */
IIS_API int iis_jpegls_encode(const void* pixels, size_t size, const iis_image_info* info,
                              iis_buffer* out);

/* 只读码流头，取图像参数 */
IIS_API int iis_jpegls_read_info(const void* data, size_t size, iis_image_info* info);

/* 解码为紧密排列的交织像素；有损码流（NEAR ≠ 0）返回 IIS_E_UNSUPPORTED */
IIS_API int iis_jpegls_decode(const void* data, size_t size, iis_image_info* info, iis_buffer* out);

/* ---- zstd ---- */

#define IIS_SIZE_UNKNOWN UINT64_MAX

/* 单帧压缩，帧头写入原始长度；level 取 1~19（与阶段 0 一致，兜底用 19） */
IIS_API int iis_zstd_compress(const void* data, size_t size, int level, iis_buffer* out);

/* 解压单个 zstd 帧。expected_size 已知时传入，长度不符返回 IIS_E_SIZE_MISMATCH；
 * 未知传 IIS_SIZE_UNKNOWN。帧后有多余字节视为损坏。 */
IIS_API int iis_zstd_decompress(const void* data, size_t size, uint64_t expected_size,
                                iis_buffer* out);

/* ---- 容器 v1（spec/container-v1.md）---- */

enum { IIS_SEG_RECIPE = 1, IIS_SEG_SKELETON = 2, IIS_SEG_PLANE = 3, IIS_SEG_EXTRA = 4, IIS_SEG_META = 5 };
enum { IIS_CODEC_NONE = 0, IIS_CODEC_ZSTD = 1, IIS_CODEC_JPEGLS = 2, IIS_CODEC_HTJ2K = 3, IIS_CODEC_JPEGXL = 4 };
#define IIS_SEG_FLAG_CRITICAL 1u

typedef struct iis_range {
    uint64_t offset;
    uint64_t length;
} iis_range;

typedef struct iis_container_info {
    uint16_t version_major;
    uint16_t version_minor;
    uint32_t incompat_flags;
    uint32_t compat_flags;
    uint32_t segment_count;
    uint64_t orig_size;
    uint8_t orig_sha256[32];
    uint64_t container_size;
    int restorable; /* 0：含本版本不认识的关键段或不能解码的编码；原因见 iis_reader_restore 的错误说明 */
} iis_container_info;

typedef struct iis_segment_info {
    uint64_t offset; /* 段记录在容器中的起点 */
    uint64_t raw_len;
    uint64_t data_len;
    uint32_t meta_len;
    uint32_t data_crc;
    uint16_t type;
    uint16_t codec;
    uint16_t flags;
    uint16_t id;
} iis_segment_info;

/* 写入：按顺序追加段，最后 finish 写段表与文件尾。
 * 文件写入方只新建文件，目标已存在返回 IIS_E_CONFLICT（绝不覆盖）；未 finish 就释放时删除不完整文件。
 * 原子提交（临时文件 → 自检 → 重命名）由调用方负责。 */
typedef struct iis_writer iis_writer;

IIS_API int iis_writer_create_file(const char* path_utf8, uint64_t orig_size, const uint8_t orig_sha256[32],
                                   iis_writer** out);
IIS_API int iis_writer_create_memory(uint64_t orig_size, const uint8_t orig_sha256[32], iis_writer** out);
/* 追加配方 / 像素平面 / 附加字节 / 描述信息段；data 为已编码内容，raw_len 为解码后长度 */
IIS_API int iis_writer_add(iis_writer* w, uint16_t type, uint16_t codec, uint16_t id, uint64_t raw_len,
                           const void* data, size_t data_len);
/* 追加原样字节段：ranges 升序、互不重叠；data 为这些区间原字节连接后（按 codec）编码的结果 */
IIS_API int iis_writer_add_skeleton(iis_writer* w, uint16_t id, uint16_t codec, const iis_range* ranges,
                                    size_t count, const void* data, size_t data_len);
/* 写段表与文件尾。sync 非 0 时落盘（FlushFileBuffers / fsync）。内存写入方把整个容器放入 out（文件写入方传 NULL） */
IIS_API int iis_writer_finish(iis_writer* w, int sync, iis_buffer* out);
IIS_API void iis_writer_free(iis_writer* w);

/* 读取。open 只做结构校验（文件头、文件尾、段表）；段内容在读取时校验 CRC32C。
 * 同一 reader 不要在多个线程中同时使用。open_memory 不复制数据，关闭前 data 必须保持有效。 */
typedef struct iis_reader iis_reader;

IIS_API int iis_reader_open_file(const char* path_utf8, iis_reader** out);
IIS_API int iis_reader_open_memory(const void* data, size_t size, iis_reader** out);
IIS_API void iis_reader_close(iis_reader* r);
IIS_API int iis_reader_info(const iis_reader* r, iis_container_info* info);
IIS_API int iis_reader_segment(const iis_reader* r, uint32_t index, iis_segment_info* info);
/* 按类型与 id 找段；不存在返回 IIS_E_NOT_FOUND */
IIS_API int iis_reader_find(const iis_reader* r, uint16_t type, uint16_t id, uint32_t* index);
/* 校验后取出段的原样内容（meta、data 可为 NULL 表示不要） */
IIS_API int iis_reader_read(iis_reader* r, uint32_t index, iis_buffer* meta, iis_buffer* data);
/* 校验并解码：none / zstd 得到原字节，jpegls 得到像素（布局同 iis_jpegls_decode） */
IIS_API int iis_reader_decode(iis_reader* r, uint32_t index, iis_buffer* out);
/* 原样字节段的区间表；out->data 为 iis_range 数组，out->size 为字节数 */
IIS_API int iis_reader_ranges(iis_reader* r, uint32_t index, iis_buffer* out);
/* 配方 JSON（UTF-8，未加结尾 0） */
IIS_API int iis_reader_recipe(iis_reader* r, iis_buffer* out);
/* 完整校验：每段 CRC、区间表、配方外层。deep 非 0 时再解码并还原、比对 SHA-256 */
IIS_API int iis_reader_verify(iis_reader* r, int deep);
/* 还原原文件（结果已与头部 SHA-256 比对一致） */
IIS_API int iis_reader_restore(iis_reader* r, iis_buffer* out);
/* 还原到新文件（已存在则返回 IIS_E_CONFLICT，不覆盖）；失败时不留下文件 */
IIS_API int iis_reader_restore_file(iis_reader* r, const char* out_path_utf8, int sync);

/* 整文件兜底容器：配方 format=bytes + 一个覆盖全文件的原样字节段。zstd_level 为 0 时不压缩，1~19 为 zstd 级别 */
IIS_API int iis_pack_bytes(const void* data, size_t size, int zstd_level, iis_buffer* out);

/* ---- 格式识别与单文件压缩（T3）---- */

/* RAW 参数规则（JSON）：{"raw": [{"match": "station3_*.raw", "width": 2448, "height": 2048,
 *   "pixel_format": "Mono12p", "stride": 可选, "offset": 可选}, …]}。match 为相对监控根目录、"/" 分隔的通配符（* 可跨目录），
 *   规则按顺序取第一条匹配；Windows 下不区分大小写。无文件头的 RAW 只有命中规则才按图像压缩。 */
typedef struct iis_raw_rules iis_raw_rules;
IIS_API int iis_raw_rules_parse(const char* json_utf8, size_t len, iis_raw_rules** out);
IIS_API void iis_raw_rules_free(iis_raw_rules* rules);


enum {
    IIS_METHOD_IMAGE = 1, /* 按图像拆分压缩（像素为标准 JPEG-LS 码流） */
    IIS_METHOD_ZSTD = 2,  /* 整文件 zstd 兜底 */
    IIS_METHOD_STORE = 3  /* 压了更大：不生成容器，保留原文件 */
};

typedef struct iis_compress_options {
    int fallback_zstd_level;     /* 兜底 zstd 级别 1~19；0 表示默认 19 */
    const iis_raw_rules* raw_rules;
    const char* rel_path_utf8;   /* 相对监控根目录的路径，用于匹配 RAW 规则；可为 NULL */
    const char* raw_sidecar_json; /* 同名参数文件（xxx.json）的内容，规则未命中时使用；可为 NULL */
    double min_saving;           /* 收益阈值 0~0.9：压缩后须不大于原文件 ×（1 − min_saving），否则不压。默认 0 */
    int raw_auto;                /* 非 0：既没有规则也没有参数文件的无文件头数据，按内容自动识别 RAW 的宽、高、像素格式。
                                    猜错只影响压缩比（照常自检、逐字节无损）；用猜出的参数时同时试 zstd，取小的。默认 0 */
} iis_compress_options;

/* 按内容识别格式，结果为 JSON（class、container、width、height、bits_stored、shift、bayer_candidates 等）。
 * opt 中只用到 raw_rules、rel_path_utf8、raw_sidecar_json；可为 NULL */
IIS_API int iis_detect(const void* data, size_t size, const iis_compress_options* opt, iis_buffer* json_out);

typedef struct iis_compress_result {
    int method;          /* IIS_METHOD_* */
    uint64_t orig_size;
    uint64_t stored_size;
    char detail[512];    /* 识别类别或兜底原因（UTF-8） */
    double seconds_analyze; /* 识别 + 版面拆分 */
    double seconds_encode;  /* 像素编码 + 写容器（兜底时为 zstd） */
    double seconds_verify;  /* 自检：独立读回、解码、重组、SHA-256 */
} iis_compress_result;

/* 压缩一个文件的内容。成功返回的容器已经过自检：独立读回、解码、按版面重组，SHA-256 与原文件一致。
 * method 为 IIS_METHOD_STORE 时 container 为空。opt 为 NULL 时用默认值。 */
IIS_API int iis_compress(const void* data, size_t size, const iis_compress_options* opt, iis_buffer* container,
                         iis_compress_result* result);

/* ---- 文件压缩与原子提交（T4）----
 *
 * iis_compress_file 处理一个文件 X，结果为 X.iis（同目录）：
 *   读原文件（不妨碍其他程序读写）→ 内存内压缩与自检（同 iis_compress）→ 写临时文件 X.iis.<8 位十六进制>.iistmp
 *   → 落盘（FlushFileBuffers / fsync）→ 读回与已自检的容器逐字节比较 → 原子重命名为 X.iis（Windows 写穿；
 *   POSIX 再同步目录）→ 按配置释放原文件：独占打开（拒绝他人写和删）、重新计算 SHA-256 与压缩时一致才删除。
 * 任何一步失败或进程在任意时刻终止，原文件都不会被改动或删除；临时文件由失败路径或 iis_recover_dir 清理。
 * 原文件只在 X.iis 完整落盘之后才可能被删除，所以任何时刻原文件与完整的 .iis 至少存在其一。
 *
 * X.iis 已存在时：内容与 X 一致（深度校验通过）→ 不重复压缩，只按配置释放原文件；
 * 是 IIS 容器但内容是旧的（X 后来被同名覆盖）→ 原文件优先，用新结果原子替换（keep_stale 时报 IIS_E_CONFLICT）；
 * 不是 IIS 容器或无法读取 → IIS_E_CONFLICT，不碰任何文件。 */

enum { IIS_RELEASE_KEEP = 0, IIS_RELEASE_DELETE = 1 };

typedef struct iis_file_options {
    iis_compress_options compress; /* rel_path_utf8 为空时用文件名；raw_sidecar_json 为空时自动读同名 .json（见 no_sidecar） */
    int release;    /* IIS_RELEASE_KEEP（默认）：保留原文件；IIS_RELEASE_DELETE：提交后释放原文件 */
    int keep_stale; /* 非 0：已有的旧 .iis 与原文件内容不同时不替换，返回 IIS_E_CONFLICT */
    int no_sidecar; /* 非 0：不自动读取同名参数文件 */
    int no_sync;    /* 非 0：不落盘（仅供测试；产品必须为 0） */
} iis_file_options;

enum {
    IIS_FILE_COMPRESSED = 1, /* 已提交新的 .iis */
    IIS_FILE_STORED = 2,     /* 压了更大或收益不足：不生成 .iis，原文件保持原样 */
    IIS_FILE_ALREADY = 3,    /* .iis 已存在且与原文件一致，未重复压缩 */
    IIS_FILE_SKIPPED = 4     /* 本身是 .iis / 临时文件 / IIS 容器，不处理 */
};

enum {
    IIS_ORIG_KEPT = 0,     /* 未要求释放，或没有 .iis（原文件保留） */
    IIS_ORIG_RELEASED = 1, /* 已删除原文件 */
    IIS_ORIG_BUSY = 2,     /* 原文件被占用，暂未释放（下次处理或恢复时再释放） */
    IIS_ORIG_CHANGED = 3,  /* 原文件在压缩后被修改，未释放；.iis 已过期，需重新压缩 */
    IIS_ORIG_GONE = 4,     /* 原文件已被别人删除或改名 */
    IIS_ORIG_FAILED = 5    /* 释放失败（只读、权限等），原文件保留 */
};

typedef struct iis_file_result {
    int outcome;  /* IIS_FILE_* */
    int original; /* IIS_ORIG_* */
    iis_compress_result compress; /* outcome 为 COMPRESSED / STORED 时有效 */
    uint64_t container_size;      /* 提交或已存在的 .iis 大小 */
    double seconds_read;    /* 读原文件 + SHA-256 */
    double seconds_write;   /* 写临时文件 + 落盘 + 读回比较 + 重命名 */
    double seconds_release; /* 独占打开 + 重新计算 SHA-256 + 删除 */
    char detail[512];       /* 释放结果或跳过原因（UTF-8） */
} iis_file_result;

/* 返回 IIS_OK 表示已安全完成（含 STORED、ALREADY、SKIPPED 与未能释放原文件）；失败返回错误码，原文件不变、不留临时文件。
 * 常见错误：IIS_E_NOT_FOUND（原文件不存在）、IIS_E_BUSY、IIS_E_NO_SPACE、IIS_E_CONFLICT、IIS_E_IO。 */
IIS_API int iis_compress_file(const char* path_utf8, const iis_file_options* opt, iis_file_result* result);

/* 启动恢复：扫描目录中的残留，按“原文件优先”处理：
 *   临时文件 *.iistmp：原文件在 → 删除；原文件不在且 .iis 不在 → 深度校验通过则改名为 .iis，否则保留并报告；
 *     原文件不在但 .iis 已在 → 保留并报告（不删除无法证明多余的数据）；
 *   X 与 X.iis 同时存在且 release 为 DELETE：.iis 深度校验通过且与 X 内容一致才释放 X；不一致计入 pending（待重新压缩）。
 * 不认识的文件一律不动。符号链接 / 重解析点目录不进入。 */
typedef struct iis_recover_report {
    uint64_t files_scanned;
    uint64_t temps_removed;
    uint64_t temps_promoted;
    uint64_t temps_kept;
    uint64_t originals_released;
    uint64_t pending;  /* 原文件与 .iis 不一致、.iis 损坏或原文件被占用：原文件保留，等待重新压缩或稍后释放 */
    uint64_t errors;   /* 目录无法读取、删除失败等 */
} iis_recover_report;

/* 每处理一项回调一次（可为 NULL）：path 为涉及的文件，action 为中文说明 */
typedef void (*iis_event_callback)(void* user, const char* path_utf8, const char* action_utf8);

IIS_API int iis_recover_dir(const char* dir_utf8, int recursive, const iis_file_options* opt, iis_event_callback cb,
                            void* user, iis_recover_report* report);

/* 线程池：后台多线程压缩。opt 被复制（其中 raw_rules 指针须在线程池销毁前保持有效）。
 * 回调在工作线程中调用：status 为 iis_compress_file 的返回值，error 为失败说明（成功时为空串）。 */
typedef struct iis_pool iis_pool;
typedef void (*iis_pool_callback)(void* user, const char* path_utf8, int status, const iis_file_result* result,
                                  const char* error_utf8);

IIS_API int iis_pool_create(int threads, const iis_file_options* opt, iis_pool_callback cb, void* user,
                            iis_pool** out);
/* 排队一个文件；rel_path 用于匹配 RAW 规则（可为 NULL）。同一路径已在排队或处理中时忽略 */
IIS_API int iis_pool_submit(iis_pool* pool, const char* path_utf8, const char* rel_path_utf8);
/* 暂停 / 继续取新文件（正在处理的文件会做完） */
IIS_API void iis_pool_pause(iis_pool* pool, int paused);
/* 排队中 + 处理中的文件数 */
IIS_API size_t iis_pool_pending(iis_pool* pool);
/* 等待队列清空且没有正在处理的文件（暂停中且队列非空时会一直等） */
IIS_API void iis_pool_wait(iis_pool* pool);
/* 丢弃排队中的文件，等正在处理的完成后释放 */
IIS_API void iis_pool_destroy(iis_pool* pool);

#ifdef __cplusplus
}
#endif

#endif /* IIS_IIS_H */
