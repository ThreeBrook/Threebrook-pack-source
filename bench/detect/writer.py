"""推测写图程序。只依据可观察的证据，给不出依据就是“未知”。

已知（经实测）：
- OpenCV BMP：40 字节 DIB 头，biSizeImage=0，分辨率=0，8 位时 biClrUsed=0。
- OpenCV TIFF（libtiff）：无 Software、无分辨率标签，显式写 PlanarConfiguration 与 SampleFormat。
- tifffile：Software="tifffile.py"（默认）。
Halcon / VisionPro / 相机 SDK 目前只按 Software 等文本标签匹配，真实指纹待客户样本校准。
"""
from __future__ import annotations

from ..formats.bmp import BmpInfo
from ..formats.tiff import TiffInfo

LABELS = {"halcon": "Halcon", "opencv": "OpenCV", "visionpro": "VisionPro", "camera_sdk": "相机 SDK",
          "tifffile": "tifffile", "unknown": "未知"}

SOFTWARE_RULES = [
    ("halcon", ("halcon", "mvtec")),
    ("visionpro", ("visionpro", "cognex")),
    ("camera_sdk", ("pylon", "basler", "hikrobot", "hikvision", "mvs", "spinnaker", "flir", "vimba",
                    "allied vision", "daheng", "galaxy", "ic capture", "the imaging source", "ezcam",
                    "dalsa", "sapera", "baumer", "jai ", "teledyne", "irayple", "huaray", "dahua")),
    ("opencv", ("opencv",)),
    ("tifffile", ("tifffile",)),
]


def _by_text(text: str | None):
    if not text:
        return None
    t = text.lower()
    for writer, keys in SOFTWARE_RULES:
        for k in keys:
            if k in t:
                return writer, k
    return None


def from_bmp(info: BmpInfo) -> tuple[str, str]:
    if (info.dib_size == 40 and info.size_image == 0 and info.ppm == (0, 0)
            and (info.bits != 8 or info.clr_used == 0)):
        return "opencv", "BMP 头特征：40 字节 DIB 头、biSizeImage=0、分辨率=0、biClrUsed=0（OpenCV imwrite 实测指纹）"
    return "unknown", "BMP 头无可识别指纹"


def from_tiff(info: TiffInfo) -> tuple[str, str]:
    for text, name in ((info.software, "Software"), (info.description, "ImageDescription")):
        hit = _by_text(text)
        if hit:
            return hit[0], f"{name} 标签含“{hit[1]}”：{text[:60]}"
    codes = info.tag_codes
    if info.software is None and 282 not in codes and 284 in codes and 339 in codes:
        return "opencv", "无 Software / 分辨率标签，显式写 PlanarConfiguration 与 SampleFormat（OpenCV+libtiff 实测指纹，中等置信）"
    return "unknown", "TIFF 标签无可识别指纹" + (f"（Software={info.software[:40]}）" if info.software else "")
