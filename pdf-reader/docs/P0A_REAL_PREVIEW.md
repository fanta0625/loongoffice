# P0-A 真实预览宿主实验

这是 P0-A0 后的中间实验，不将它标记为 P0-A1 通过。复用 Draw 已有图像对象与锁定图层，验证 LoongOffice 内部 PDFium 生成的页面图像与原生编辑对象共存，暂不修改 Office 源码。

## 运行

使用已集成 PDFium 的目标 LoongOffice，在图形桌面终端运行；无需安装 poppler-utils 或其他渲染引擎：

```bash
(
    export PYTHONPATH=/opt/loongoffice/program
    export LD_LIBRARY_PATH=/opt/loongoffice/program
    export URE_BOOTSTRAP=vnd.sun.star.pathname:/opt/loongoffice/program/fundamentalrc
    python3 pdf-reader/tools/p0a_preview_probe.py \
        --office /usr/bin/loongoffice \
        --report /tmp/p0a-real-preview.json \
        --save /tmp/p0a-real-preview.odg
)
```

输出路径必须不存在。脚本生成三页 PDF，在私有 Office 进程中设置 `LO_IMPORT_USE_PDFIUM=1`，通过正常加载路由进入 `SdPdfFilter`，取得第 1、3 页的 PDF Graphic，由内部 GraphicProvider 导出 PNG，放入锁定的阅读图层，中间放原生 Draw 文本与矩形。可以用 `--pdf /path/three-or-more-pages.pdf` 替换输入；中间仍是预置对象，未实现输入 PDF 第二页的编辑转换。使用独立 Office profile 和 pipe，回车关闭其自建进程，不操作已有用户文档。失败时报错，不回退到外部渲染器。

源码链路：`sfx2/source/doc/objstor.cxx` → `sd/source/filter/pdf/sdpdffilter.cxx` → `vcl::ImportPDFUnloaded`，随后 `GraphicProvider.storeGraphic(image/png)` → `Graphic::GetBitmap` → `VectorGraphicData::ensurePdfReplacement` → `vcl::RenderPDFBitmaps` → 内置 PDFium。GraphicProvider 的普通加载接口没有 PDF 页号参数，因此不能直接给 queryGraphic 传入 PageNumber 取第三页。

手工检查：

- 上下预览显示真实 PDF 内容，宽高比正确。
- 锁定的预览不能通过普通选择、移动或删除操作修改；主动解锁图层不在防护范围内。
- 中间文字可编辑，移动与撤销重做正常。
- 滚动、缩放后相对位置稳定，阅读图像与编辑对象共存。
- 关闭后没有残留的本次私有 Office 进程。

JSON 仅记录结构检查，不能代替上述 GUI 验收。`--headless` 用于结构检查并立即退出；外层建议增加 `timeout 120s`。

## 限制与下一步门槛

- 仍为一个高 `SdPage`，不是三个连续的真实 `SdPage`。不解决跨页编辑、不同页面尺寸与裁切框映射。
- 复用当前 Office PDF 图形加载路径会枚举全文页面及其批注／链接；只有选中的预览页显式导出 PNG。这不是最终的独立阅读核心，也不能以此测得按需加载性能。
- 栅格化分辨率沿用 Office 当前图形链路默认值，尚不支持目标像素尺寸和高清图块。
- 不实现图块、异步刷新、取消、按页导入、修改跟踪和 PDF 保存。ODG 仅供检查原型图形结构。
- 锁定图层是现有 Draw 能力的可行性探测，不等于产品层面的阅读权限控制。

后续仍须实现并验证多页面布局与活动编辑视图的配合。只有这个门槛通过，才能宣布统一连续混合宿主成立；不应因本实验显示了两个 PNG 就直接改写默认 PDF 打开或保存行为。

## 开发环境检查（2026-11-06）

旧版实验曾使用外部渲染器，该验证不作为当前内部引擎链路的证据。当前脚本已移除外部渲染器依赖；内置引擎链路的运行验证单独记录。

内部引擎版本在 WSL / LibreOffice 25.2.3.2 上实际 headless 运行通过。报告确认源文档三页、第 1／3 页预览哈希不同、锁定图层和原生文本存在、`external_renderer_used=false`。解包 ODG 验证两张不同 PNG 实际嵌入及原生文本保留；查看嵌入图片确认页面标识分别为 PAGE 1／PAGE 3。Python 语法与差异格式检查通过。此结果不能代替目标 LoongOffice / loong64 验证。

本地无目标 LoongOffice GUI 实测；滚动缩放、锁定图层选择行为、对象编辑与撤销由目标机验证。多 SdPage、阅读核心和异步调度均未在此实验验证。
