# 真实渲染验证与成本测量

状态：验证机已完成上一版构建，并通过 pdffast_core、pdffast_render、sd_pdf_import_test、vcl_graphic_test 四项目标。首次大文档 GUI 验证报告冷启动／缩放／首次编辑缓慢，编辑后绘制失真；尚未通过 GUI 验收。本轮编辑后端替换和新增测试尚未在验证机执行。本机仍只进行源码检查，不编译或启动应用。

本轮修正：编辑由 PDFium 图形打散改为默认 Draw Poppler/XML 导入链，给 xpdfimport 增加受编译开关控制的单页选项。新增三页样本，覆盖文本、矢量路径、渐变、Form 变换、图片和空白页；新测试对照默认整份导入与延迟单页导入的对象位置和实际绘制像素，并检查单页过滤、失败保留预览。原有跨页撤销测试也会经过新后端。这些用例已经编写，尚未运行，不能据此认定问题 PDF 已修复。

诊断更新：启用 PDF Fast 时，sd、svxcore 和 pdffastcore 显式保留 INFO 日志，release 构建也能使用 `SAL_LOG=+INFO.pdffast`；无需全局开启 `--enable-sal-log`。已有视口更新后的瓦片重新调度修正仍待 GUI 验证。新日志 `native-edit-import backend=poppler` 明确记录默认导入器调用结果、输出页数和耗时。

## 原生 Draw 验证入口

当前产品入口以 [原生接入设计](NATIVE_DRAW.md) 为准，下面原有的独立阅读器命令仅用于后端对照。更新后先重新生成带编译开关的配置，再在验证机运行：

```sh
sh debian/scripts/configure-libreoffice
make -j4
make CppunitTest_sd_pdf_import_test CppunitTest_sdext_pdfimport
SAL_LOG=+INFO.pdffast ./instdir/program/soffice "/absolute/path/sample.pdf" 2> native-pdf.log
```

上一版已通过的用例包括：内存源 Session 与原生 PDFium 交错绘制／关闭；旧编辑后端的当前页转换、重复准备幂等、跨页撤销；固定源字节在原容器销毁后的生命周期。当前编辑后端已改变，必须重跑 sd_pdf_import_test，并运行 sdext_pdfimport 的真实 Poppler 单页测试；此前通过不能替代本轮验证。交错压力用例不能代替 TSAN 或确定性的竞争点注入，现有样本也不能替代用户报告失真的大文档。

GUI 验收顺序：

1. 打开多页 PDF，确认原生 Draw 窗口和只读 InfoBar；缩放、拖动滚动条、快速换页，观察先预览后高清。高 DPI、缓存不足时不能留下持续白洞。
2. 点 InfoBar“编辑”，确认只准备当前页；修改文本／移动对象，切换另一页首次准备，再返回并撤销／重做，修改与历史必须保留。
3. 切回只读，已编辑页必须显示修改结果；此时使用原生绘制，不承诺编辑快照缓存。另存 ODG 后重开，导出 PDF 后用独立查看器核对编辑页、未访问页、批注／链接、旋转和透明内容。
4. 有后台任务时 Ctrl+O、关窗口、关闭文档、退出；多个视图及多份 PDF 重复。检查无挂起／崩溃，必要时用已有 ASan 构建。真实只读文件仍遵循原生权限流程。
5. 打开 ODG、ODP、SVG 等非 PDF，确认不进入新分支；关闭编译开关构建时确认无额外只读行为。无界面转换与 LibreOfficeKit 单独回归。

`native-import total_us` 包含原生文件读取、全部页面建立及批注导入，不含 Office 启动与后续首屏呈现。`native-ui-image allocation_us/upload_cache_us` 与现有汇总脚本兼容；`native-edit-import backend=poppler total_us` 包含当前页过滤器调用的临时文件写入、helper 启动、PDF 解析和 XML 导入；`native-edit-prepare total_us` 还包含暂存、样式固化和对象迁移。区域渲染继续使用已有 worker 日志。原生入口使用导入的内存源，不产生随机 FileInput 的 input-summary，缺少这条记录不等于文件 I/O 为零。

出现编辑失真时，保留只读／编辑两种状态的对照和问题 PDF（可以是仍能复现的单页裁剪版本）。日志筛选：

```sh
grep -E 'native-(import|attach|viewport|ui-image|edit-)|PDF region' native-pdf.log
```

`native-edit-fonts`、`native-edit-objects`、`native-edit-unsupported` 来自 PDFium 图形打散器，当前产品按页编辑不再经过该路径。新路径应出现 `native-edit-import backend=poppler success=1 pages=1`。`native-edit-begin` 保留 PDF 页号与目标矩形；`native-edit-prepare success` 只表示原图被对象替换，不表示视觉等价。不能仅凭有对象或 CppUnit 通过认定问题文档转换正确。

## 集成目标

启用 PDF Fast 的既有构建环境中运行 `make CppunitTest_pdffast_render`；模块 check 也已登记此目标。它链接真实 VCL／PDFium，编译实际 input.cxx 与 session.cxx，不用模拟渲染器。已有纯逻辑目标 `CppunitTest_pdffast_core` 保留。

[render.cxx](../qa/render.cxx) 使用仓库的 Pangram.pdf，覆盖：

- 同步整图与渐进整图逐像素比较；带偏移的渐进裁剪与整图对应区域比较。
- 取消返回 false 后，在同一 PDFium 页面上重新完成渲染，验证未留下破坏后续绘制的状态。
- 随机输入打开、首次编辑源数据冻结、越界读取拒绝，以及文档持有输入回调直至关闭。
- 多次建立会话、替换高分辨率需求、排除过时结果，实际 worker 输出 BGR 与独立同步整图对应像素比较，再带未完成请求销毁；另覆盖立即关闭。

这些测试不能证明全部生命周期：小样本可能在渐进检查点前完成，取消可能发生在调用返回时；关闭前的最后任务也未必已经开始。需要复杂 PDF 和受控暂停点才能确定性覆盖正在解码／渲染时的关闭。测试等待结果有 15 秒期限，但析构 join 自身没有强制超时；持续集成应设置进程级超时，并在已有 ASan 构建下执行同一目标。

目前尚缺扫描／透明／旋转／表单等样本矩阵、合并后多个子块的全链路逐像素对照、文件改写／截断故障注入、UI 销毁与 Ctrl+O 的事件级自动化。不能用此目标替代完整 GUI 回归或证明无释放后使用。

## 采集与汇总

使用支持 INFO 日志的构建，在启动阅读器前设置 `SAL_LOG=+INFO.pdffast`，将标准错误重定向到独立 UTF-8 日志。Windows PowerShell 示例：

```powershell
$env:SAL_LOG = '+INFO.pdffast'
& '安装目录/program/loongoffice-pdf-fast.exe' '样本.pdf' 2> 'reading.log'
python pdffast/qa/summarize.py reading.log > reading-summary.json
```

必须关闭阅读器使输入资源释放，才有 input-summary。若启用日志后仍没有记录，先确认构建没有裁剪 INFO；不能把空汇总当作耗时为零。日志本身、计时器和断言会增加开销；主观流畅度和正式性能对照也需另做关闭日志的采集。

每个输入对象汇总 reads、checks、misses、bytes、read_us 与 check_us：读取请求次数、校验调用次数、64 KiB 缓存块未命中、请求字节数、读取回调总耗时、校验总耗时。bytes 包含失败或重复请求，不是实际磁盘字节数。checks 包含已冻结输入的快速返回，不能直接等同文件系统调用次数。初始化构造时的查询不包含在 check_us 中。

read_us 内包含校验时间；check_us 还包含渲染边界和冻结时独立校验，不能相加或直接取比值计算占比。每个回调的微秒舍入可能低估很短的调用；系统调用数量、磁盘服务时间和锁等待要用平台 profiler 补充。现有代码只在销毁时汇总，不为每次文件读取生成日志。

UI ui-image 的 allocation_us 包含 Bitmap 创建和写访问取得；upload_cache_us 包含 ReadRawDIB、缓存入库及失败处理，不声称是纯拷贝耗时。脚本按最近秩给出 P50／P95／P99、最大值与样本数，并汇总已有结果批次、渲染区域耗时。批次包含布局等后处理；这些时间都不是屏幕实际呈现的帧耗时。

## 对照流程与决策

固定机器、构建类型、PDF、DPI、窗口、缩放与缓存预算；分别采集文字 PDF、大扫描 PDF、复杂矢量 PDF。每类至少重复五次，冷启动与热缓存分开存放日志。先不进入编辑，完成连续滚动、反向滚动、快速跳页和缩放；首次编辑与导出另开一次会话记录，避免其完整源读取混入纯阅读统计。

另用平台 profiler／屏幕呈现追踪记录首屏内容、整屏清晰、输入到呈现延迟、滚动 P95／P99、峰值内存及磁盘活动。与 Okular 对照时使用相同条件。源代码中存在异步线程或动态瓦片不等于性能领先。

决策前置条件：渲染像素及生命周期验证通过、有足够样本数、能够重复观察到瓶颈。文件校验昂贵时，先设计有明确一致性约束的校验批次或快照，再做 A/B；不可直接关闭检查。UI 单图上传昂贵时，先优化图像交接和单项工作量。区域调用开销占主导时再比较瓦片尺寸与合并策略；确有 CPU 吞吐瓶颈时，再调查 PDFium 的安全并行边界、独立文档／进程及内存成本。

本轮保持现有 512 px 瓦片、自适应合并及单 worker；没有实测数据支持现在调整它们，也没有“已达到 Okular”的结论。


## 验证机器首次链接反馈

龙芯 GCC 14 构建已到达 pdffastedit 链接阶段，报告 Graphic(shared_ptr<GfxLink>, page) 为 hidden symbol。该现有构造函数在 graph.hxx 中被 SAL_DLLPRIVATE 覆盖类导出属性，不能被独立编辑库调用。本次去除该成员的隐藏标记，导出现有实现，保持共享源、页号和延迟加载行为；不修改链接器参数或构造实现。验证机器需更新后重建 VCL 与编辑库。此修复仅静态核对，尚未复编或完成 GUI 验证。公共边界除 PDFium 扩展外还包括这一现有 Graphic 构造函数的可见性调整。
