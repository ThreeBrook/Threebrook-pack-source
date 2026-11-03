"""阶段 1 T8：iis-cli 与开源解码器 iis-decode 的端到端测试（未构建时跳过）。

合成小数据集复制到临时目录 → iis-cli compress（释放原文件）→ verify --deep → restore -o 另一目录 → 与原文件逐字节相同；
iis-decode 对每个 .iis 单独还原，SHA-256 与原文件一致；export 还原 / 复制；不覆盖已有文件。
"""
from __future__ import annotations

import hashlib
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
EXE = ".exe" if sys.platform == "win32" else ""
CLI = ROOT / ("build/win-x64/src/cli/Release/iis-cli" + EXE if sys.platform == "win32" else "build/linux-x64/src/cli/iis-cli")
DEC = ROOT / ("build/win-x64/decoder/Release/iis-decode" + EXE if sys.platform == "win32" else "build/linux-x64/decoder/iis-decode")
LIC = ROOT / ("build/win-x64/src/license/Release/iis-license" + EXE if sys.platform == "win32" else "build/linux-x64/src/license/iis-license")
DEV_KEY = ROOT / ".cache/license-dev/dev_private.key"  # 开发用私钥（不入库）；没有时跳过需要授权的压缩部分

pytestmark = pytest.mark.skipif(not (CLI.exists() and DEC.exists()), reason="iis-cli / iis-decode 未构建")


def run(*args):
    return subprocess.run([str(a) for a in args], capture_output=True)


@pytest.fixture()
def samples(small):
    return small[0]  # conftest：会话共用的合成小数据集（只读）


def files(d: Path):
    return {p.relative_to(d).as_posix(): p.read_bytes() for p in d.rglob("*") if p.is_file()}


def dev_license(tmp_path) -> Path:
    if not (LIC.exists() and DEV_KEY.exists()):
        pytest.skip("没有开发用私钥，无法签发测试授权")
    code = run(LIC, "machine-code").stdout.decode().strip()
    out = tmp_path / "license.lic"
    r = run(LIC, "issue", "--key", DEV_KEY, "--machine", code, "--customer", "测试", "--device", "测试", "--days", "1",
            "--out", out)
    assert r.returncode == 0, r.stdout.decode("utf-8", "replace")
    return out


def test_compress_requires_license(samples, tmp_path):
    work = tmp_path / "w"
    shutil.copytree(samples, work)
    r = run(CLI, "compress", work, "-r", "--license", tmp_path / "没有.lic")
    assert r.returncode == 1 and "不能压缩" in r.stdout.decode("utf-8") and "IIS1-" in r.stdout.decode("utf-8")
    assert not list(work.rglob("*.iis"))  # 没有授权就不压缩
    bad = tmp_path / "bad.lic"
    bad.write_text("{}", encoding="utf-8")
    assert run(CLI, "compress", work, "-r", "--license", bad).returncode == 1


def test_compress_verify_restore(samples, tmp_path):
    orig = files(samples)
    work = tmp_path / "work"
    shutil.copytree(samples, work)
    r = run(CLI, "compress", work, "-r", "--release", "--license", dev_license(tmp_path))
    assert r.returncode == 0, r.stdout.decode("utf-8", "replace")
    r = run(CLI, "verify", work, "-r", "--deep")
    assert r.returncode == 0, r.stdout.decode("utf-8", "replace")
    out = tmp_path / "restored"
    r = run(CLI, "restore", work, "-r", "-o", out)
    assert r.returncode == 0, r.stdout.decode("utf-8", "replace")
    restored = files(out)
    left = {k: v for k, v in files(work).items() if not k.endswith(".iis")}  # 压不小、保留原文件的
    assert len(restored) + len(left) == len(orig)
    for k, v in {**restored, **left}.items():
        assert v == orig[k], k
    # 再还原一次：已存在不覆盖
    r = run(CLI, "restore", work, "-r", "-o", out)
    assert r.returncode == 0 and "跳过 " in r.stdout.decode("utf-8")
    # 开源解码器：逐个还原，SHA-256 与原文件一致
    iis = sorted(work.rglob("*.iis"))
    assert iis
    r = run(DEC, "--check", *iis)
    text = r.stdout.decode("utf-8", "replace")
    assert r.returncode == 0, text
    for p in iis:
        rel = p.relative_to(work).as_posix()[:-4]
        assert hashlib.sha256(orig[rel]).hexdigest() in text, rel
    one = iis[0]
    dst = tmp_path / "one.out"
    assert run(DEC, one, "-o", dst).returncode == 0
    assert dst.read_bytes() == orig[one.relative_to(work).as_posix()[:-4]]
    assert run(DEC, one, "-o", dst).returncode == 1  # 不覆盖
    # 损坏的 .iis：报错、不留文件
    bad = tmp_path / "bad.iis"
    data = bytearray(one.read_bytes())
    data[len(data) // 2] ^= 0xFF
    bad.write_bytes(bytes(data))
    assert run(DEC, bad).returncode == 1 and not (tmp_path / "bad").exists()
    # export：还原与原样复制
    ex = tmp_path / "export"
    assert run(CLI, "export", work, ex).returncode == 0
    assert files(ex) == {**restored, **left}
    ex2 = tmp_path / "export2"
    assert run(CLI, "export", work, ex2, "--copy").returncode == 0
    assert files(ex2) == files(work)
