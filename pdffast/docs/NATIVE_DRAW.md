# 原生 Draw 接入

状态：2026-11-12，上一版已在验证机完成构建和四项测试，但 GUI 发现编辑失真；本轮纠正导入后端并增加对照测试，尚待验证机构建和执行。本机不编译、启动应用或运行测试。本文件取代早期文档中将独立程序作为产品入口的约定。

## 打开与只读预览

启用编译开关的桌面构建，`sfx2/source/doc/objstor.cxx` 把 Draw PDF 导入选择到既有 PDFium 分支。非 PDF 文件、无界面运行与 LibreOfficeKit 不增加这项桌面路由。

`SdPdfFilter::Import()` 保留原生导入：原始 PDF、页面大小、原生 SdrGrafObj、批注与链接进入文档。成功后附加 `sd::PdfFastDocument`，DrawDocShell 设置 UI 只读。不创建顶层窗口、菜单或独立编辑模型。

`sd::View::CompleteRedraw()` 为屏幕绘制配置已有的 ViewObjectContactRedirector。仅未转换的原始 PDF 对象可被替换；嵌套页面、打印和导出不替换。后台 Session 保持独立 PDFium 文档，复用需求调度、渐进取消、区域合并与像素交接。UI 用 BitmapPrimitive2D 显示预览和可见瓦片，原生坐标、缩放、滚动及窗口保持不变。

缓存为 128 MiB：可见预览优先保留，可见高清工作集受保护；预取仅占剩余预算，先被淘汰。高清超预算时降级到预览，结果到达不反复重排互相淘汰的请求。缩放时仅在当前分辨率有缺口时显示旧分辨率图像。视口外一行可预取；独立工具的全部滚动预测策略尚未逐项迁移。

输出设备地址仅用作视口身份，不存储可调用的窗口指针；多个窗口的需求合并，关闭视图的旧需求在后续调度中过期。UI 按 4 ms／64 项预算交接，空闲停止轮询，新需求重新启动。4 ms 是项间检查，不是单项或端到端帧耗时保证。

## InfoBar 与按页编辑

使用原生 SID_EDITDOC。对于仅 UI 只读且介质可写的文档，原生 ReadOnlyUIGuard 清除只读状态并广播 ModeChanged，不重新打开 PDF。真实只读介质的权限及重新加载逻辑仍由原生 Sfx 处理。

DrawViewShell::Notify 接收状态变化，SwitchPage 接收原生换页；编辑状态下准备当前标准页。控制器从 FilterFactory 读取 `draw_pdf_import` 的服务和完整定义，直接调用默认 XmlFilterAdaptor → DrawPDFImport → Poppler xpdfimport → Draw XMLOasisImporter。这是默认 Draw 打开 PDF 的导入链；上一版的 DoImportMarkedMtf → ImpSdrPdfImport 属于 PDFium 图形打散路径，不能替代默认导入器。

通过私有 FilterOptions `LoongOfficePdfPage=N`（从 1 开始）限制 Poppler 的 displayPage/processLinks 循环，只向 XML 导入器输出目标页。无该选项或关闭编译开关时保持默认全部页面导入，原有 SkipImages 行为保留。选项无效则失败；同时检查返回文档只有一个标准页，拒绝旧 helper 忽略选项后导入整份文档。

目标页先导入无窗口的临时 Draw 文档。用原生 BurnInStyleSheetAttributes 固化样式，再 CloneSdrPage 保留对象、组、连接关系和坐标；按名称映射图层，解除临时主页面和页面样式引用，迁移页面回调、尺寸、边距与填充属性。避免普通粘贴导致重新居中，以及同名目标样式覆盖外观。

准备前取消当前页尚未完成的瓦片需求。暂存完成前不删除原 PDF 图形；导入失败保留原预览，插入失败回滚对象替换。转换不记录为用户编辑，不清空已有撤销栈，并恢复先前的撤销启用及修改标记。已准备页不重复转换，其他页保留原始图形，使原生保存和导出仍能处理未访问页。

不在绘制回调中修改模型，不实现瓦片级对象导入。已编辑页恢复原生绘制，不使用过时 PDF 缓存；后续若实现编辑快照，必须绑定对象版本与撤销／重做。

## 线程和生命周期

Session 使用导入图形的同一源数据，不重新打开可能已变化的路径。BinaryDataContainer::getDataShared 在 UI 线程固定不可变字节的所有权；后台读取固定字节，不访问原容器，也不额外复制整份文件。

VCL PDFium 封装在引擎调用、构造和析构处使用进程级递归锁。开启此特性时覆盖原生绘制、对象转换、搜索、签名与后台渲染；关闭特性时守卫不取锁。锁不取得 SolarMutex，后台仅发布持有像素的结果，不回调 UI。PDFium 仍串行执行，这个改动解决线程安全边界，不宣称提高多核吞吐。

文档拥有控制器；SdDrawDocument 析构在广播 ModelCleared 前先停 Timer、取消并 join worker，再释放原始对象引用。已转换页的晚到结果被忽略。后台不可用时失效原生对象显示缓存，退回原生绘制。

## 性能边界

原生预览导入的完整源读取和全部页面建立尚未替换。编辑虽然只转换目标页，默认过滤器仍会将完整固定源写入临时文件、启动 Poppler helper 并解析 PDF；每页首次转换仍在 UI 线程执行，不承诺消除几十秒停顿。原生缩略图的同步调用可能与 PDFium 后台任务竞争进程锁。页面解析及表单绘制不能保证即时取消。

后续依据原生导入、首张预览、高清区域、UI 图像交接、首次编辑、保存导出的分别测量，再决定是否收敛导入、延迟高清请求或调整瓦片策略。不能把独立工具的懒加载结果套用到原生入口，当前没有与 Okular 同机对照的成绩。
