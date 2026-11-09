# 内部 PDFium 区域渲染最小链路

本轮是独立验证入口，不改变 `read_edit.py` 的阅读方式。已有阅读窗口仍使用整页预览；没有自动缩放监听、可见区域调度、LRU 图块缓存、编辑态图块资源接管。

## 接口及隔离

VCL 增加 `RenderPDFTile`，输入 PDF 字节、零基页索引、虚拟整页像素尺寸、图块像素原点及尺寸，输出白底 Bitmap。调用内置 PDFium，以负原点将整页投影到小目标位图，直接裁剪渲染；不先分配整页高清 Bitmap 再裁图。

`GraphicProvider.storeGraphic` 的实验性 `FilterData` 同时包含：

- `PdfTilePageSize`：UNO `com.sun.star.awt.Size`，整页在所需缩放下的像素尺寸。
- `PdfTileRect`：UNO `com.sun.star.awt.Rectangle`，左上角原点的区域，单位为同一像素网格。

输入是阅读预览持有的 PDF XGraphic，自带源文件字节和页索引；输出仅支持 PNG。没有这些参数的原有导出不变。源 XGraphic 不被替换或修改。错误参数、非 PDF、非 PNG 及渲染失败明确抛异常。

当前限制：单块最多 2048×2048，虚拟页每边最多 131072；区域必须位于页内。上限只约束目标位图，不限制 PDF 内容解析本身的资源。每次调用会重新打开 PDF 文档，尚无文档句柄缓存、异步执行或原生取消。

Python `tile_bridge.py` 负责参数验证和贴图坐标。共用边界采用同一个取整公式，避免相邻图块因独立取整产生坐标缝隙。原预览始终保留。该试验使用未旋转、未裁剪的原始页面；用户旋转／裁剪或编辑对象不能直接套用此坐标规则。

旧 Office 会忽略未知 FilterData，所以适配层先发送无效零宽请求，要求返回带 `PDF tile:` 标识的参数异常；否则明确报告不支持，不拿旧低清输出冒充高清图块。

## 新构建验证

本轮修改了 C++，需要增量编译；不能只更新 Python 就验证新入口。

```bash
set -o pipefail
make PARALLELISM=4 2>&1 | tee ../pdf-tile-build.log
make PARALLELISM=4 CppunitTest_vcl_pdfium_library_test

(
    export PYTHONPATH="$PWD/instdir/program"
    export LD_LIBRARY_PATH="$PWD/instdir/program"
    export URE_BOOTSTRAP="vnd.sun.star.pathname:$PWD/instdir/program/fundamentalrc"
    python3 pdf-reader/tools/probe_tiles.py \
        --office "$PWD/instdir/program/soffice" \
        --out "pdf-reader/results/tiles-$(date +%Y%m%d-%H%M%S)" --wait
)
```

集成图像比较使用 Pillow（Debian/Loongnix 的 `python3-pil`），只解码检查 PNG，不参与 PDF 渲染。输出目录必须是新的目录。

验证先由同一内部引擎输出 2048×1024 参考图和相邻两个 512×512 图块，比较区域内容，再将图块映射到 Draw 原页面。检查缩放时高清区域、位置、接缝及低清预览保留。按 Enter 关闭试验，不提供编辑功能，也不代表完整瓦片阅读已完成。

PDFium 在不同裁剪目标上可能产生少量抗锯齿差异，因此比较采用平均 RGB 通道绝对误差 < 0.25（满量程 255），同时通道误差 >16 的比例 < 0.1%；不是宣称逐像素完全相等。原生测试同样覆盖位置一致性和越界／尺寸限制。

## 已完成验证与边界

- 28 项 Python 单元测试通过，覆盖坐标共边、末端图块、非法请求、旧版拒绝和能力应答，以及原有会话检查。
- 本机 WSL LibreOffice 25.2 的 `--expect-unsupported` 检查通过：旧入口明确被拒绝。
- 直接调用本机 Office 自带的 `/usr/lib/libreoffice/program/libpdfiumlo.so` 验证负原点坐标计算（没有安装其他 PDF 渲染器）：小样本和 500 页大 PDF 的第 2 页各生成两个 512×512 图块，均通过上述误差检查。大文件两个图块的平均 RGB 误差分别约 0.0186 和 0.0047，较大误差通道占比分别约 0.0334% 和 0。
- 这项直接库验证不覆盖新 C++ 包装、UNO 桥接、表单叠加和 Draw 贴图。新原生代码在开发机尚未编译，原生测试及正向 UNO／GUI 验证均待可构建环境执行，不能记录为已通过。

本地证据在忽略目录 `results/direct-internal-tiles-final/` 和 `results/tile-unsupported-1/`；不提交源 PDF 或生成图片。
