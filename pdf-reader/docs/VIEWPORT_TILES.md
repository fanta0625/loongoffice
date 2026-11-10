# 阅读窗口自动高清瓦片

`tools/read_edit.py` 默认启用 `uno/python/viewport_tiles.py`。它读取 Draw 当前页、`ZoomValue`、页相对 `ViewOffset` 和 `VisibleArea` 的尺寸；缩放或平移停留约 180 毫秒后，为可见区域请求新的高清瓦片。不是放大原来的低清 PNG。页面像素密度根据缩放、设备 DPI 和 1.5 倍采样确定，像素仍由 Office 内置 PDFium 生成。

## 调度与资源

- 每次主循环最多处理一块，大小最多 512×512；优先视口中心，最多保留 64 块可见瓦片。
- 快速翻页或缩放会使旧计划失效，不继续旧队列。同步原生调用返回后再次检查视口，过时结果只进入缓存，不贴到窗口。
- PNG 文件使用 LRU，按 `宽×高×4` 的解码像素成本计算，预算 64 MiB；这不是 Office 总进程内存上限。PDF 解析、基础预览和 Draw 图形缓存另计。
- 不替换原 PDF XGraphic，始终保留整页预览作底图。旧版本没有瓦片入口或渲染失败时，提示降级并保留底图，不重复请求失败入口。
- 瓦片在锁定的预览图层，更新不产生用户撤销记录，也不把文档标为已修改。点击编辑前清除全部会话瓦片，随后由 Draw 绘制可编辑对象；编辑对象不参与阅读瓦片缓存淘汰。

所有 UNO 操作在同一脚本线程串行执行。当前只能取消未开始的请求、拒绝过时结果，不能中断正在执行的 PDFium 同步调用。原生入口仍会逐次打开 PDF，没有文档句柄复用。本轮没有实现连续多页画布、编辑态瓦片、后台并行渲染或 PDF 保存。

## 目标机验证

已有原生瓦片桥接且 `probe_tiles.py` 通过的构建，只需更新 Python，无需重新编译。在源码根目录、图形桌面运行：

```bash
(
    export PYTHONPATH="$PWD/instdir/program"
    export LD_LIBRARY_PATH="$PWD/instdir/program"
    export URE_BOOTSTRAP="vnd.sun.star.pathname:$PWD/instdir/program/fundamentalrc"
    python3 pdf-reader/tools/read_edit.py \
        --office "$PWD/instdir/program/soffice" \
        --pdf "/absolute/path/example.pdf" \
        --report "/tmp/pdf-viewport-$(date +%Y%m%d-%H%M%S).json"
)
```

1. 保持阅读模式，用 Draw 的缩放控件放大到 200%／400%，停止操作。应陆续出现 `reading-tile` 日志，放大后 `page_pixels` 增大，视口文字逐块变清晰。渲染速度取决于页面复杂度。
2. 在放大状态平移到其他区域，应补充该区域瓦片。快速切换不同页，再回到原页，底图始终保留，最终停留页更新高清瓦片；检查无串页、错位和明显接缝。
3. 点击编辑，确认高清叠图移除，停留页转换为可选择／修改的 Draw 对象。切页返回后修改和撤销历史保留，不出现预览归属错误。
4. 关闭后检查报告 `tile_reading`：包含渲染、实际贴图、缓存命中、过时丢弃及峰值计数。`error` 应为空。`--no-tiles` 可关闭瓦片，作整页预览对照。

`probe_tiles.py` 仍是固定两块的独立像素／贴图检查，不监听缩放。验证本功能请运行 `read_edit.py`。

## 验证边界

Python 回归覆盖视口裁剪、缩放增加分辨率、边缘瓦片、数量／尺寸限制、LRU、快速翻页去抖、同步调用后的过时结果丢弃、旧桥接降级与贴图失败处理。开发机旧版 Office 不含新原生桥接，不能以这些测试宣称新构建的 GUI 高清效果已通过；需按上述步骤在目标机确认。
