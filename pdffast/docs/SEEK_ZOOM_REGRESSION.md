# 跳页、缩略图与编辑后缩放回归修复

日期：2026-11-13。修复基于 `183bf1565850`，验证环境为 WSL / Debian x86_64 release、GTK3 / X11、私有 Xvfb 1280×900。尚未在 Loongnix / LoongArch 上验证，以下数据不能代替该机器的性能验收。

## 已定位的问题

1. 缩略图请求每约 10 ms 重试，`schedule()` 每次重启 16 ms 的交付 Timer。持续重试能让已生成像素长期无法交付。新增集成测试在修复前 30 秒超时，加入“只在未活动时启动 Timer”后约 270 ms 通过。
2. 缩略图请求受原生“空闲”条件限制，仅提交队头；队头未完成便重新占据首位。主画布的所有高清瓦片又排在缩略图前。可见侧栏页现在提前提交，不等待空闲；与画布共享 256×512 范围内的小预览，优先于可被抢占的高清细化。非可见缩略图保留后台优先级。
3. `Paint()` 对每个原生对象调用整页 `findPage()`，导致对象数平方级扫描；快照瓦片生成还在 UI 反复绘制复杂页，并使整页对象 primitive 缓存失效。已删除已转换页的屏幕快照管线。`PreparePaint()` 在已转换页返回 false，不安装 PDF 重定向。原始 PDF 对象使用身份索引，已转换页阅读和编辑均完全使用 Draw。
4. 仅缩略图结果也广播 `ActionChanged()`，干扰原生页面缓存和预览队列。现在这类结果由侧栏自己的轮询消费，不广播模型变更。
5. 编辑模式跳转到新页时缺少明确的按需转换请求。现在只在显式编辑模式下登记新页意图，继续保持 120 ms 静置和取消规则；预览跳页不转换。

元数据空闲任务改为仅处理可见页，避免持续遍历整份 PDF 以及主线程争抢 PDFium 锁。复制、编辑、保存、打印和导出的完整性屏障保留。

## 验证

Linux 增量构建成功。三个定向 CppUnit 目标分别通过 17、5、12 项，共 34 项。新增集成用例覆盖已转换页在缩放／往返切页／只读切换时绕过 PDF 重定向，以及连续请求缩略图时正常交付且页面视觉版本不变。已有像素等价、保存重开、取消、失败回滚测试继续通过。

真实窗口脚本：[seek_zoom.py](../qa/seek_zoom.py)。测试文件为用户的 500 页 PDF，本次输入副本 66,756,017 字节，SHA-256：`58ab727d3944df7506a298eaa1c8757f591cec828e9b1824930f92b448cef87c`。第 400 页转换后有 5221 个对象。

| 操作 | 首次窗口验证 | 增加连续缩放的窗口验证 |
| --- | ---: | ---: |
| 预览跳至第 400 页，侧栏与画布稳定 | 1032 ms | 1030 ms |
| 进入编辑，转换完成 | 1057 ms | 1022 ms |
| 编辑后 100%／150%／200%／75% 画面稳定 | 527／643／880／697 ms | 587／641／1547／640 ms |
| 选中对象后缩放至 125%，画面稳定 | 370 ms | 373 ms |
| 编辑态跳到下一页，画面稳定 | 995 ms | 713 ms |
| 返回已转换页，画面稳定 | 415 ms | 642 ms |
| 连续 13 次编辑态缩放，80%→200% | 未执行 | API 共 556 ms，最后画面稳定共 1192 ms |

截图稳定包括连续五次不变、每次约 50 ms 的观察等待以及图像读取成本，属于脚本代理指标，不是输入到显示器呈现的帧延迟。该脚本也不能保证每个中间缩放帧都已呈现。两次观测有波动，高倍率原生绘制仍有可见耗时，不宣称达到 Okular 或龙芯实机的流畅度目标。

本机详细日志、截图、JSON：`/home/wangxin/pdf-fast-linux-results/seek-zoom-regression/fixed-run2` 和 `fixed-burst`。新路径没有 `native-read-snapshot` 日志；转换后的缩放使用原生对象而非再次生成页面瓦片。

最终版本又确认一遍相同窗口流程，结果在 `seek-zoom-regression/final`：预览跳页稳定 1044 ms，编辑就绪 1013 ms，编辑缩放稳定 589／637／890／667 ms，13 次连续编辑缩放 API 共 618 ms、最后画面稳定共 1202 ms，返回已转换页稳定 413 ms；所有操作完成。最终侧栏还跳过已有最新缩略图的重复预请求，避免后台队列尚未清空时反复缩放已缓存图像。

复现示例（先退出旧进程；每次指定新的结果目录）：

```bash
export PYTHONPATH="$PWD/instdir/program"
export LD_LIBRARY_PATH="$PWD/instdir/program"
export URE_BOOTSTRAP="vnd.sun.star.pathname:$PWD/instdir/program/fundamentalrc"
timeout 180 /usr/bin/python3 pdffast/qa/seek_zoom.py \
  --program "$PWD/instdir/program" \
  --pdf /absolute/path/large-test-500p-50mb.pdf \
  --page 400 --output /tmp/pdf-seek-zoom-run1
```

需要 Linux 的 Xvfb、Pillow 和匹配的 UNO Python 模块。日志开启有额外开销，正式性能对照应另做关闭日志的同机测试。

## ae50e6d2a0a2 上的间歇性卡顿调查

本节前半记录取栈前的调查；后续实机栈已定位到图形换出期间的同步 PDF 渲染，见下节。

Loongnix 验证机反馈：快速 seek 后缩放偶尔无响应，预览和编辑模式均可发生。现有日志只记录已完成的图像上传和转换；不能区分卡住时的 PDFium 解析、锁等待、Draw 绘制或窗口后端等待。日志中的图像分配和上传通常为几十到几百微秒，页面 478 的编辑准备为 585974 微秒，没有证据将十几秒无响应归因于这些已完成阶段。Qt 剪贴板信息提示实机存在 Qt 组件，但不能据此认定 Qt 是原因。

此前窗口流程在 seek 后等待截图稳定才缩放，未覆盖任务重叠。`seek_zoom.py --stress` 增加五个目标页（13、498、32、479、400），每次跳页后立即设置六个缩放倍率；在预览和编辑模式分别执行。编辑模式下包含转换尚未完成时的导航与取消，不能声称每个目标页都已转换；原有独立缩放阶段仍在第 400 页确认转换完成后执行。`--cpus 0` 只限制测试进程到一个逻辑 CPU，`--vcl` 指定窗口后端，`--preview-only` 可以单独运行预览部分。每个阶段开始即输出标记并保存 JSON，阻塞时仍保留所在阶段。

不改动 ae50e6d2a0a2 应用代码，在相同 WSL 样本上的单次观测：

| 单 CPU 测试 | GTK3 / X11 | gen / X11 |
| --- | ---: | ---: |
| 五次预览 seek + 六次缩放的 API 时间 | 56–120 ms | 46–93 ms |
| 五次编辑模式 seek + 六次缩放的 API 时间 | 33–189 ms | 9–138 ms |
| 第 400 页转换就绪 | 1203 ms | 1017 ms |

两次均未观察到十几秒 API 阻塞。结果目录为 `seek-zoom-regression/immediate-singlecpu` 和 `immediate-gen-singlecpu`。截图短暂不变不保证后台任务完成；这些结果不构成 Loongnix 故障已修复或所有交互均流畅的证据。当前 WSL 构建没有 Qt 插件，尚未覆盖实机 Qt 路径。没有提交仅延长防抖时间的应用代码调整。

实机复现时，保留无响应的窗口，在另一终端执行：

```bash
pgrep -a -x soffice.bin
# 将下面的 45403 替换为本次 PDF 窗口所属的 PID；不要沿用旧日志中的 PID。
sudo bash pdffast/qa/capture_hang.sh 45403
```

脚本需要已安装的 `gdb`，连续三次采集所有线程栈、线程状态、内存和相关绘图后端库路径。每次 attach 短暂停住进程，取栈后自动 detach，不结束程序；不生成 core 或收集环境变量。默认输出目录为 `/tmp/pdf-hang.XXXXXX`，以脚本实际打印的目录为准。若指定输出目录，必须是新目录。工具已在一个仍运行的 WSL Draw 测试进程上验证三次取栈和 detach；另通过 bash 语法检查。

保留 `stacks-1.txt`、`stacks-2.txt`、`stacks-3.txt`、`threads-*.txt` 和 `environment.txt`。采集前不要覆盖或重编译运行中进程所加载的库，以免 GDB 使用了不同版本的符号。结合这组现场数据再决定修复解析、取消、锁顺序或窗口绘制路径。

## 实机确认：图形缓存换出意外渲染整页 PDF

用户提供的 Loongnix 25 / Loongson-3A5000 / Qt6 三次采样中，主线程 47114 都在执行：

```text
MemoryManager::ReduceMemoryTimerHandler
  -> loopAndReduceMemory -> ImpGraphic::swapOut -> createSwapInfo
  -> getPrefSize -> VectorGraphicData::getRange -> ensureSequenceAndRange
  -> XPdfDecomposer::getDecomposition -> RenderPDFBitmaps
  -> FPDF_RenderPageBitmap -> PDFium AGG 路径栅格化
```

三次采样分别落在扫描线、路径矩形判断和描边顶点生成，说明采样期间主线程正在计算。`pdffast::Session` 与转换线程在条件变量上等待；`BufferedDecompositionFlusher` 在等待 SolarMutex。就这次现场而言，直接阻塞来自缓存回收触发的同步整页绘制，不能归因于异步瓦片队列或 Qt 事件线程等待。机器可用内存约 7.2 GiB，进程 VmSwap 为 0；这里的 `swapOut` 是应用内部图形换出，不代表操作系统正在交换该进程的内存。

原先 `ImportPDFUnloaded` 已读取每页物理尺寸，但只把尺寸放在 `PDFGraphicResult::maSize`，没有存入延迟加载的 `Graphic`。图形变为可用后，其首选尺寸仍为空；缓存回收保存换出信息时调用 `getPrefSize()`，后者为计算范围而同步渲染 PDF。回收定时器和图形闲置时间条件解释了间歇性以及预览、编辑模式均可受影响；CPU 较慢可以放大这条错误路径的成本。

修复内容：

- `PDFGraphicResult` 在构造时把已知尺寸和 Map100thMM 单位写入图形。未加载的原生 PDF 可以保存这两个元数据而不触发换入，随后沿用已有的 `restoreFromSwapInfo()` 恢复。
- `ImpGraphic` 的复制、移动和赋值保留 `maExPrefSize`。否则图形分离复制后仍可能丢失首选尺寸，再次进入范围计算。
- 保留原有缓存换出机制和 Draw 编辑渲染，没有关闭缓存回收或增大用户缓存设置。

新增两个 VCL 回归用例。`testUnloadedPdfSwapDoesNotRasterize` 同时覆盖普通／延迟元数据导入、各页三轮换入换出、尺寸／单位／页码保持，并持有旧向量对象检查换出期间没有生成绘制序列。旧代码在“Cache eviction must not rasterize a PDF page on the UI”断言失败，修复后通过。`testPdfPreferredSizeSurvivesCopyOnWrite` 在旧代码上将 1234×5678 的首选尺寸丢失为 PDF 固有的 21590×27940，修复后保留自定义尺寸且不分解 PDF。

Linux 构建和测试：`make -j6 sd` 成功；`CppunitTest_vcl_graphic_test` 89 项、`CppunitTest_sd_pdf_import_test` 12 项、`CppunitTest_pdffast_render` 5 项通过，共 106 项。日志位于 `seek-zoom-regression/swap-{baseline,copy-baseline,fixed-vcl,fixed-build-sd,fixed-sd,fixed-render}.log`。

窗口脚本新增 `--cache-pressure`：仅在本次新建的独立配置中设定 1 MB 图形缓存、0 秒允许闲置时间并开启换出，且通过 UNO 读取并核对生效值。观察截图时还执行 UNO 响应探测并记录 `max_ui_roundtrip_ms`，避免将无响应窗口的静止截图误判为完成。截图仍是代理指标，不能保证每个中间帧和状态栏刷新均已呈现。

最终 500 页实窗验证位于 `seek-zoom-regression/swap-pressure-final`：WSL / GTK3 / 单 CPU，缓存配置已读取确认；全部流程完成。五组预览 seek 后连续缩放 API 为 69–129 ms，期间响应探测单次最长 7.6 ms；第 400 页转换为 5221 个 Draw 对象，就绪约 1168 ms。编辑阶段响应探测最高 1465 ms，在这个限制 CPU 和缓存的条件下仍有明显绘制耗时，不能宣称已达到整体流畅度目标或替代 Loongnix 验收。此前 `swap-pressure-baseline` 的窗口流程也完成了，因此这里不以窗口前后时间宣称复现了实机十几秒阻塞；根因复现证据来自上述旧代码失败的回归测试与用户的三次现场栈。

实机复测需要先退出 root shell 回到普通用户，并关闭旧 Draw 进程，再执行：

```bash
git pull --ff-only pdf-fast-review feat/pdf-fast-26.2.6
make -j4 vcl && make -j4 sd
```

本次修复涉及 VCL，不能只替换 `libsdlo.so`。不需要 clean 或全量重建。先正常复测快速跳页、缩放、进入编辑和再次跳页；也可以将上面的窗口脚本示例加上 `--stress --cache-pressure --cpus 0`，使用新结果目录运行。修复切断了所提供调用栈中的同步渲染路径；Loongnix 实机效果仍需验收。
