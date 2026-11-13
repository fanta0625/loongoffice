# 原生 Draw 接入

状态：2026-11-13，基于 `f78aaa0f613f` 的工作区实现。已完成 WSL2 / Debian 13 的 Linux release 集成构建，五项 CppUnit 共 134 个测试及 3 个 Python 测试通过；GUI 性能采集条件与覆盖范围见验证记录。完整范围与性能边界见 [OPTIMIZATION_IMPLEMENTATION.md](OPTIMIZATION_IMPLEMENTATION.md)。产品入口是原生 Draw，独立阅读器用于后端对照。

## 打开与只读预览

启用 PDF Fast 的桌面构建在 `sfx2/source/doc/objstor.cxx` 将 Draw PDF 导入路由到既有 PDFium 分支。非 PDF、无界面运行及 LibreOfficeKit 保留原有路由。

`SdPdfFilter::Import()` 读取固定源、页面几何和原生整页 SdrGrafObj，附加 `sd::PdfFastDocument`，DrawDocShell 设置 UI 只读。桌面快速入口通过 `vcl::PDFMetadataReader` 保留源及已打开的解析文档，延后批注／链接；其他调用仍使用原有立即导入接口。首屏返回后在低优先级空闲任务中每次补一个页面，优先可见页。复制、编辑、保存、打印和导出有显式完整性屏障，读取失败不静默丢内容。

`sd::View::CompleteRedraw()` 仅为窗口输出配置 ViewObjectContactRedirector。原始 PDF 页使用共享 Session 的后台 PDFium 预览及可见瓦片；已编辑只读页可以使用绑定原生视觉版本的快照。打印、导出和离屏原生绘制不使用替代器。原生窗口、坐标和缩放继续由 Draw 控制。

原始 PDF 图像预算 128 MiB，可见预览优先、高清工作集受保护，超预算降级预览。高清瓦片 512 px，缩放连续变化静置 80 ms 才请求新高清；同尺度重绘不重新计时，旧分辨率只选最近层级补缺口。侧栏未编辑整页 PDF 通过共享会话异步取缩略图；已编辑／自定义页走原生合成。

已编辑页面仅在只读、标准页、默认绘制模式、非高对比度及对象不越出页面等条件下进入快照路径。快照缓存 64 MiB，区域输出最多 1024×1024 像素，原生绘制每侧增加 32 px 裁剪余量，内部缓冲最多 1088×1088，以页面／母版版本、模型通知、图层、背景色和界面样式失效。先保留原生绘制，预览就绪后再替代，失败或不适用时回退原生。快照在主线程调用原生 PreviewRenderer，不向后台传递 Draw 模型。

## 原生编辑与命令门控

用户点击原生 SID_EDITDOC。对于仅 UI 只读的可写文档，原生 ReadOnlyUIGuard 清除只读并广播 ModeChanged，不重新打开 PDF。真实介质只读仍由原生 Sfx 权限流程处理。

DrawViewShell 接收 ModeChanged 与 SwitchPage，向控制器登记当前标准页。120 ms 静置避免快速换页转换途经页面。未准备页进入 Preparing；SdrPageView 和 SfxDispatcher 的内容只读状态阻止准备期间修改。成功进入 Ready；失败进入 Failed 并显示状态。导航／返回只读撤回旧请求，重访可重试；不提供命令排队重放或百分比进度。窗口销毁时清除回调、请求和调度器门控。

后台 NativeImport 首次把固定源写入临时文件，启动持续会话 xpdfimport，并保持 PDF 打开。每页发出私有 R 请求，使用新的 PDFOutDev 重置字体／图片状态；资源 flush 完成才返回完成标记。命令／资源通过租约交给 UI，回放结束前不复用输出文件。取消会终止 helper；异常和超时可以在后续请求重建。传输期限默认两分钟，主线程回放不在此期限内。

UI 从 FilterFactory 读取默认 `draw_pdf_import` 服务定义，借助私有录制命令／资源描述符回放原有 PDFIProcessor、XML 生成及 Draw XMLOasisImporter。它与默认 XmlFilterAdaptor → DrawPDFImport → Poppler xpdfimport 使用同一对象转换链。普通单页 FilterOptions `LoongOfficePdfPage=N` 仍保留；无私有选项时默认整份导入协议不变，所有扩展受编译开关约束。

目标页先导入无窗口临时 Draw 数据模型，沿用导入初始化而不安装新建文档默认样式，避免改变文字字距，要求只有一个标准页；固化样式后克隆对象、映射图层并解除临时主页面及页面样式引用。提交保留当前目标页尺寸、边距、背景与批注，避免覆盖等待期间已有页面设置。提交前验证源、页、视图及原始对象身份，失败保留预览并回滚。转换不成为用户撤销项、不清除已有历史，并分别恢复模型及文档的修改状态。已准备页不重复转换。

## 元数据与持久化

延迟元数据读完后仍调用原有批注／链接导入逻辑。失败回滚新增批注和链接，可显式重试；后台补齐跳过失败页以免阻塞其他页。

`SdPage::CloneSdrPage()` 在源页克隆前补齐元数据；原生打印／导出入口在生成渲染器前补齐全部页面；编辑在替换原始图形前补齐目标页。原生 Save／SaveAs 先补元数据，再转换剩余 PDF 页，产生普通 ODG；失败终止保存。尚未定义可保留延迟状态的混合 ODG 格式，首次保存大文档可能耗时。

## 线程和生命周期

UI 固定 BinaryDataContainer 的共享不可变字节，所有后台会话使用这份源，不重开可能变化的原路径。后台仅做文件、进程及像素工作，不调用 UNO／VCL 模型；模型提交和元数据应用都在 UI。

VCL PDFium 过程使用进程级递归锁覆盖原生及后台调用，包含构造和析构，守卫不取得 SolarMutex。PDFium 仍串行执行，不能据此宣称多核吞吐提升。页面解析和操作系统 I/O 不能保证即时取消。

文档拥有控制器，析构停止 Timer／Idle、撤回请求并关闭进程和 worker；晚到结果不得提交。原生对象替代失败会退回原生绘制。UI 像素交接的 4 ms／64 项预算是项间检查，不是单项或端到端帧耗时上限。

## 性能边界

完整源读取、全部页面几何扫描及页面骨架仍在打开路径。原生 XML／对象构建和原生快照生成仍有主线程成本；复杂页面可能继续停顿。普通 ODG 保存还需转换未访问页。当前没有首屏优先的虚拟页面模型、后台 Draw 模型渲染或可中断的对象分批构建，也没有与 Okular 同机对照的成绩。测量与验收入口见 [VALIDATION.md](VALIDATION.md)。

编辑结果通知：`NativeImport` 在传输线程发布结果后通知消费者；Draw 通过合并的 `PostUserEvent` 主动唤醒 UI 领取结果。回调只投递事件，不接触文档模型。文档销毁先停止并 join 生产者，再移除排队的 UI 事件，避免关闭后的悬空回调；现有定时器保留防抖和预览职责。
