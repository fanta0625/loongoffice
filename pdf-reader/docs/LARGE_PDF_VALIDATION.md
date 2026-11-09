# 大文件导入回归样本

样本：`E:/projects/test-files/large-test-500p-50mb.pdf`。

- 字节数：66756017（约 63.7 MiB，并非文件名标注的 50 MB）。
- SHA-256：`58ab727d3944df7506a298eaa1c8757f591cec828e9b1824930f92b448cef87c`。
- 阅读会话实际返回页数：500。
- 用户报告原始 LoongOffice 打开会崩溃，尚未取得该崩溃的目标机日志。不得将下面的超时视作复现。
- PDF 不放入 Git；在目标机准备同一份文件，用摘要确认一致。

## 独立压测工具

`tools/stress_pdf.py` 在 Linux／WSL 上运行，两条路线分别启动独立 Office 进程和用户配置：

- baseline：通过现有 FilterFactory 编辑过滤器导入全部页面。它是明确的全量编辑路径，不等于所有版本双击打开 PDF 时采用的默认路径。
- reader：通过内部 PDFium 路线创建阅读会话，检查全部页的初始化，不调用编辑导入器。

工具不保存文档，前后校验源文件摘要。保留 Office 日志、worker 异常、阶段、退出码、超时状态和每 0.2 秒采样的进程组 RSS 合计峰值。共享页可能重复计数，短时峰值可能漏采；这不是精确 PSS 或整机内存峰值。

默认每种模式最多 90 秒，每个进程虚拟地址空间上限 4096 MiB。此上限不是整个进程树的物理内存上限，会改变运行行为，也可能导致 Java 预留内存失败或原生分配失败。因此失败不自动归因为系统 OOM、硬件故障或 Office 缺陷。不要用此受限测试替代目标机实际崩溃日志。

结束／超时会清理该测试自己的进程组，不终止用户其他 Office 会话。输出目录必须是新的目录。

目标机示例，先设置与 Office 构建匹配的 PyUNO 环境：

```bash
(
    export PYTHONPATH="$PWD/instdir/program"
    export LD_LIBRARY_PATH="$PWD/instdir/program"
    export URE_BOOTSTRAP="vnd.sun.star.pathname:$PWD/instdir/program/fundamentalrc"

    python3 pdf-reader/tools/stress_pdf.py \
        --office "$PWD/instdir/program/soffice" \
        --pdf /absolute/path/large-test-500p-50mb.pdf \
        --out "pdf-reader/results/large-$(date +%Y%m%d-%H%M%S)" \
        --timeout 120 --memory-mib 4096
)
```

`--mode reader` 可以只测阅读，`--mode baseline` 只测整份编辑导入。任一失败／超时返回非零；判断具体结果看 `summary.json`，不要只看 shell 退出码。测试范围是无界面初始化，不包括首屏绘制、滚动帧率、瓦片、相邻页预取、真实文件按需编辑和保存正确性。

## 2026-11-09 开发机初测

环境是 WSL x86-64、LibreOffice 25.2.3.2，非目标 Loongnix／本分支 LoongOffice；限时每模式 60 秒，单进程地址空间 4096 MiB。

| 路线 | 结果 | 阶段耗时 | 采样进程组 RSS 合计峰值 |
|---|---|---|---|
| 全量编辑导入 | 总计 60 秒超时，主动终止，非已确认崩溃 | 进入编辑导入时约已用 21.7 秒；尚未导入完成 | 3311832 KiB，约 3.16 GiB |
| 阅读初始化 | 500 页成功，编辑导入次数为零 | 初始化阶段约 18.8 秒，含启动清理总计约 42.4 秒 | 410008 KiB，约 400 MiB |

源文件摘要未改变。启动阶段 Java 因地址空间限制产生内存预留失败报告，证据保存在忽略目录 `results/large-500p-check-1/`，数据受该限制影响，不能直接推算正常启动耗时或认定原始崩溃根因。

单页控制样本在相同限制下，两条路线均成功；控制样本证据 `results/stress-smoke-1/`。测试工具的成功和超时路径均实际执行过。

后续首先在目标构建复测并收集日志；然后测真实 GUI 的首次可见页时间、2→10→2 切页、已编辑内容保留和长会话内存增长。本样本即使阅读初始化成功，仍不能证明已经实现“500 页仅初始化可见页”：当前适配层还会遍历并建立所有页面预览对象。
