# 宣传素材（三溪缩影）

| 文件 | 尺寸 | 用途 |
|---|---|---|
| `out/1-主海报.png` | 2160 × 2880（3:4） | 朋友圈、小红书、展会易拉宝缩样：产品名、一句话、主截图、三个卖点 |
| `out/2-横幅.png` | 3840 × 2160（16:9） | 官网首屏、公众号头图、PPT 封面：深色底，两张截图 |
| `out/3-功能.png` | 2160 × 2880 | 四步看懂：右键压缩、包里直接看、缩略图、解压一字节不差 |
| `out/4-数据安全.png` | 2160 × 2880 | 写入即自检的流程与三个承诺，给关心数据安全的客户 |
| `out/5-Hero-EN.png` | 2160 × 2880 | 英文版主海报（英文界面截图） |
| `out/6-Banner-EN.png` | 3840 × 2160 | 英文版横幅 |
| `out/7-Features-EN.png` | 2160 × 2880 | 英文版功能四步 |
| `out/8-Safety-EN.png` | 2160 × 2880 | 英文版数据安全 |
| `assets/shots-en/*.png` | 2880 × 1800 等 | 英文界面截图（演示文件也用英文文件名）：view-light / view-dark / package / package-dark / empty-light / compress / extract-done |
| `assets/shots/*.png` | 2880 × 1800 等 | 软件真实截图（浅色 / 深色、包内浏览、压缩窗口、解压完成、空白页），可单独用 |

英文海报标题用 Bricolage Grotesque，产品名 Threebrook Pack 在前、“三溪缩影”在下。
外观照三溪外观规范：米白 / 墨绿底、溪青强调、柿橙产品色；标题站酷小薇，英文 Bricolage Grotesque，正文微软雅黑。
三溪组合标志来自三溪寻影仓库 `tools/branding/brand/`。

## 截图里的图像

全是 `tools/make_demo_images.py` 程序合成的“产线图像”（PCB、晶圆、轴承、布纹、电池极片），**不含任何客户样本**。
截图里出现的压缩倍数（约 1.9～2.1 倍）是这些合成图的实测，不代表客户图像；海报正文没有写压缩比，
等有客户实测数据再加（CLAUDE.md：压缩比以客户实测为准）。

注意：站酷小薇的“回”字字形有问题（内外框被填成实心），标题里不要用这个字；标题里的英文用 `<span class="lat">`（站酷小薇的大写 I 像小写 l）。

## 重新生成

```powershell
powershell -ExecutionPolicy Bypass -File marketing\shots.ps1    # 演示图像 → 中文与英文界面截图（需要已构建与 .venv）
powershell -ExecutionPolicy Bypass -File marketing\render.ps1   # 海报 html → out\*.png（本机 Edge 无界面渲染，不联网）
```

改文案直接改 `posters/*.html`，用浏览器打开就能预览。
