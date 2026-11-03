// 界面语言：中文 / English（三溪外观规范 §7：中英文界面都要有，中文先写好，英文按同样意思写，不逐字直译）。
//
// 默认跟随 Windows 的显示语言（中文系统用中文，其余用英文）；查看器“视图 → 语言”或命令行 --lang zh|en 可改。
// 用法：tr("中文", "English")；带参数的用 trf("共 %d 个", "%d in total", n)。
#pragma once

#include <string>

namespace iis::i18n {

enum class Lang { Zh, En };

Lang lang();
void set_lang(Lang l);
Lang system_lang();  // Windows 显示语言是中文时为 Zh，否则 En

inline const char* tr(const char* zh, const char* en) { return lang() == Lang::En ? en : zh; }
// printf 格式（两种语言各一个格式串，参数相同）
std::string trf(const char* zh, const char* en, ...);

}  // namespace iis::i18n
