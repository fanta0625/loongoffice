# P0-A0 Draw 宿主原型

## 目的

P0-A0 不修改 LoongOffice 源码。它使用 UNO 创建一个高页面的 Draw 文档，在同一画布中放置三段页面：

- 第 1、3 段是阅读核心预览的占位区域；
- 第 2 段包含真实 Draw 文本和矩形对象，模拟按需编辑后的页面。

它只验证 Draw 窗口的滚动、缩放、坐标布局和对象交互体验，**不证明**阅读位图可以作为原生 Draw 页面绘制，也不证明多 `SdPage` 连续混合视图已经实现。

## 运行

在目标 LoongOffice 机器上，确保 Python 使用对应的 UNO 运行时：

```bash
export PYTHONPATH=/opt/loongoffice/program
export LD_LIBRARY_PATH=/opt/loongoffice/program
export URE_BOOTSTRAP=vnd.sun.star.pathname:/opt/loongoffice/program/fundamentalrc

python3 pdf-reader/tools/p0a_draw_host_probe.py \
    --office /usr/bin/loongoffice \
    --save /tmp/p0a-draw-host.odg
```

原型会打开一个 Draw 窗口。手工检查：

1. 滚动能连续到达三段页面；
2. 缩放后页面间距和文本位置保持一致；
3. 中间段的 `EDITABLE` 对象可以选择、移动、修改和撤销；
4. 上下阅读占位区域的坐标与中间编辑区域一致；
5. 关闭时没有残留 Office 进程。

## 结论记录

记录机器、LoongOffice 版本、脚本提交、显示后端、手工结果和截图。若交互满意，再进入 P0-A1：在 `sd` 的绘制入口接入阅读核心图像；若不满意，转向“插件阅读窗口＋Draw 编辑窗口”备用方案。
