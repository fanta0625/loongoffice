# P0-A 真实预览宿主实验

这是 P0-A0 后的中间实验，不将它标记为 P0-A1 通过。复用 Draw 已有图像对象与锁定图层，验证外部 PDF 页面图像与原生编辑对象共存，暂不修改 Office 源码。

## 运行

目标机器安装 `poppler-utils`，使用图形桌面终端：

```bash
sudo apt install poppler-utils
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

输出路径必须不存在。脚本生成三页 PDF，仅将第 1、3 页渲染为 PNG，放入锁定的阅读图层，中间放原生 Draw 文本与矩形。可以用 `--pdf /path/three-or-more-pages.pdf` 替换输入；中间仍是预置对象，未实现输入 PDF 第二页的编辑转换。使用独立 Office profile 和 pipe，回车关闭其自建进程，不操作已有用户文档。

手工检查：

- 上下预览显示真实 PDF 内容，宽高比正确。
- 锁定的预览不能通过普通选择、移动或删除操作修改；主动解锁图层不在防护范围内。
- 中间文字可编辑，移动与撤销重做正常。
- 滚动、缩放后相对位置稳定，阅读图像与编辑对象共存。
- 关闭后没有残留的本次私有 Office 进程。

JSON 仅记录结构检查，不能代替上述 GUI 验收。`--headless` 用于结构检查并立即退出；外层建议增加 `timeout 120s`。

## 限制与下一步门槛

- 仍为一个高 `SdPage`，不是三个连续的真实 `SdPage`。不解决跨页编辑、不同页面尺寸与裁切框映射。
- pdftoppm 只是原型外部渲染工具，不是正式 PDFium 阅读后端；同步生成预览也不是最终调度方案。
- 不实现图块、异步刷新、取消、按页导入、修改跟踪和 PDF 保存。ODG 仅供检查原型图形结构。
- 锁定图层是现有 Draw 能力的可行性探测，不等于产品层面的阅读权限控制。

后续仍须实现并验证多页面布局与活动编辑视图的配合。只有这个门槛通过，才能宣布统一连续混合宿主成立；不应因本实验显示了两个 PNG 就直接改写默认 PDF 打开或保存行为。

## 开发环境检查（2026-11-06）

WSL / LibreOffice 上 headless 实际运行通过：生成 PDF，经 pdftoppm 渲染第 1、3 页，通过 UNO 加载预览并创建锁定图层和原生文本，保存 ODG 后关闭私有进程。报告确认两张预览、图层锁定、可打印和原生文字存在；另行解包 ODG 确认两张图片实际嵌入，原生文本仍保留。Python 语法与差异格式检查通过。

本地无目标 LoongOffice GUI 实测；滚动缩放、锁定图层选择行为、对象编辑与撤销由目标机验证。多 SdPage、阅读核心和异步调度均未在此实验验证。
