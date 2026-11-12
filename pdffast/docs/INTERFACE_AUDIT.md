# 启动、显示、编辑接口审计

> 本文调查对象为 26.2.6 基线。后续新增的 PDFium 随机输入与渐进渲染封装不属于基线能力，详见 [第二轮接口决策](SECOND_PASS.md#公共封装变更)。

基线：`loongoffice/26.2.6`，提交 `4f67c5b36cea7a6938b6ad5e559b4da8220767a5`。结论来自本地源码静态阅读，不代表运行验证或性能承诺。

标记口径：**已具备**表示有明确源码入口；**部分具备**表示只满足子能力；**需补齐**表示在下列已审查接口中未找到完整契约，并非声称整个代码库绝无相关实现；**待验证**表示需要宿主集成或运行证据。

## 1. 总览

| 边界 | 需求 | 基线能力 | 结论 |
| --- | --- | --- | --- |
| 启动 | 独立原生窗口和事件循环 | `minvcl` 示例、VCL | 已具备基础设施；完整产品启动待设计 |
| 启动 | 延迟创建 Draw 文档 | DrawingDocument 服务工厂 | 已具备；创建时仍有模块和模型初始化成本 |
| 启动 | 自有宿主承载完整 Draw 控制器 | `XModel2`、`XFrame`、SFX 加载接口 | 部分具备；轻量启动下的组合待验证 |
| 启动 | 现有 Office 在重流程前分流 PDF | 原有 Desktop／过滤器入口 | 需明确集成点；过滤器本身无法消除此前成本 |
| 显示 | 持有 PDF 文档、按页打开 | 导出的 PDFium 封装 | 已具备；输入内存生命周期由调用方保证 |
| 显示 | 直接渲染区域 | `PDFiumBitmap::renderPageBitmap` | 已具备；可用目标小位图与偏移裁剪 |
| 显示 | 优先级、缓存、取消过时结果 | 无完整阅读会话契约 | 新模块需补齐；底层阻塞渲染取消另论 |
| 显示 | 连续 PDF 与编辑页混合画布 | VCL／drawinglayer／SdrView 基础设施 | 部分具备；统一视口和输入契约需补齐 |
| 编辑 | PDF 指定页转 Draw 对象 | `SdrEditView::DoImportMarkedMtf` 的 PDF 分支 | 已具备入口；字体预处理可能扫全文件 |
| 编辑 | Poppler 原导入器仅处理指定页 | 原始 wrapper 遍历全部页 | 基线缺少该语义，不能假定 `FilterOptions` 已支持 |
| 编辑 | 向任意目标页原子提交并回滚 | `XImporter` 仅设置目标文档 | 需补齐事务和目标页资源隔离契约 |
| 编辑 | 私有模型批量交接 | `SdrModel::Merge`、`SdrExchangeView::Paste` | 部分具备；涉及克隆、样式和撤销语义 |
| 编辑 | 混合状态保存 | PDFium 保存与普通 Draw 导出等基础设施 | 需定义协调策略；不是现成完整接口 |

## 2. 启动边界

### 2.1 原生轻量入口

[`vcl/workben/minvcl.cxx`](../../vcl/workben/minvcl.cxx) 的 `SAL_IMPLEMENT_MAIN`、`TheApplication::Main` 展示了：创建组件上下文、设置进程服务工厂、初始化语言和 VCL、创建 `WorkWindow`、进入事件循环并清理。

[`vcl/Executable_minvcl.mk`](../../vcl/Executable_minvcl.mk) 直接链接 VCL、基础 UNO 和框架等库，没有直接链接 `sd`。因此可以探索自有原生入口，不必先走完整 `desktop::Desktop`。但示例仍使用进程内 UNO，且没有证明间接依赖开销、完整编辑环境或冷启动时间。

[`desktop/source/app/app.cxx`](../../desktop/source/app/app.cxx) 中的应用初始化、用户配置处理等工作发生在投递 `OpenClients` 事件之前。仅在 PDF 过滤器中分流，不能消除已发生的启动成本。

**待解决：**独立入口与现有 Office 入口如何选择，文件关联、现存进程转发、最近文档与显式 Draw 导入如何保持一致。

### 2.2 延迟初始化 Draw

[`sd/source/ui/unoidl/unodoc.cxx`](../../sd/source/ui/unoidl/unodoc.cxx) 的 `sd_DrawingDocument_get_implementation` 在 SolarMutex 下调用 `SdDLL::Init()` 并创建 `sd::GraphicDocShell`。服务见 [`sd/util/sd.component`](../../sd/util/sd.component)。

[`sd/source/ui/app/sddll.cxx`](../../sd/source/ui/app/sddll.cxx) 的 `SdDLL::Init` 注册模块、视图、接口和控制器等；[`sd/source/ui/docshell/docshell.cxx`](../../sd/source/ui/docshell/docshell.cxx) 的 `DrawDocShell::Construct` 创建绘图文档、基础模型、池和撤销管理。

可以把创建推迟到首次编辑，但“通过工厂延迟创建”不等于“首次编辑没有初始化开销”。必须分别度量阅读首屏和首次编辑成本。

### 2.3 窗口、模型和控制器连接

| 接口 | 现成能力 | 限制 |
| --- | --- | --- |
| [`XModel2`](../../offapi/com/sun/star/frame/XModel2.idl) | 枚举、创建默认或指定视图控制器 | 调用方负责连接模型、Frame、控制器和当前控制器 |
| [`XFrame`](../../offapi/com/sun/star/frame/XFrame.idl) | 初始化容器窗口、设置组件 | 接管窗口生命周期；不保证支持重新设父窗口；组件窗必须属于容器 |
| [`XController2`](../../offapi/com/sun/star/frame/XController2.idl) | 暴露 `ComponentWindow` 等信息 | 不是任意矩形内的轻量编辑器插槽 |
| [`VCLUnoHelper`](../../include/toolkit/helper/vclunohelper.hxx) | VCL 与 UNO 窗口桥接 | 不替调用方管理完整编辑视图生命周期 |
| [`SfxViewFrame`](../../include/sfx2/viewfrm.hxx) | `LoadDocumentIntoFrame`、隐藏文档加载 | 仍进入 SFX 文档和视图机制 |

[`sfx2/source/doc/sfxbasemodel.cxx`](../../sfx2/source/doc/sfxbasemodel.cxx) 的 `createViewController` 创建常规 SFX view shell。它是完整文档视图入口，不是直接放入多页阅读画布的页内对象编辑控件。

**结论：**原有 UNO 对模型、Frame 和服务生命周期有可用价值；连续混合画布的布局、裁剪、输入和焦点边界没有由这些接口自动解决。必须验证完整控制器嵌入或采用原生 `SdrView` 适配的可行性。

### 2.4 原有 PDF 打开路径的前置工作

[`sfx2/source/doc/objstor.cxx`](../../sfx2/source/doc/objstor.cxx) 的加载路径先执行文档初始化，再决定 `LO_IMPORT_USE_PDFIUM`／LOK 等 PDFium 导入分支。此时已经不是纯阅读器的最早入口。

[`sd/source/filter/pdf/sdpdffilter.cxx`](../../sd/source/filter/pdf/sdpdffilter.cxx) 调用 `ImportPDFUnloaded`，随后创建、复制文档页并加入图形与批注。注意函数名中的 `Unloaded` 不意味着首屏前没有全量处理：[`vcl/source/filter/ipdf/pdfread.cxx`](../../vcl/source/filter/ipdf/pdfread.cxx) 中该函数读取完整数据、打开文档，并循环所有页获取页信息、批注和链接。

**缺口：**若需要首屏前只付首屏相关成本，不能直接把这条完整导入路径作为新阅读器的打开契约。

## 3. 显示边界

### 3.1 PDFium 文档和区域渲染

[`include/vcl/filter/PDFiumLibrary.hxx`](../../include/vcl/filter/PDFiumLibrary.hxx) 已导出 `PDFium`、`PDFiumDocument`、`PDFiumPage`、`PDFiumBitmap` 等接口：

- `PDFiumLibrary::get()`：取得共享封装入口。
- `openDocument(data, size, password)`：打开内存 PDF；长度参数为 `int`，不能承诺任意大文件。
- `getPageCount`、`getPageSize`、`openPage(index)`：页计数、尺寸与按页访问。
- `createBitmap(width, height, alpha)`：分配目标图，宽高按引用传入，可能被缩小。
- `renderPageBitmap(document, page, startX, startY, sizeX, sizeY)`：按完整页输出尺度与偏移渲染到目标图。
- buffer／stride／format 接口与 `createBitmapFromBuffer`：读取或转换渲染结果。

[`vcl/source/pdf/PDFiumLibrary.cxx`](../../vcl/source/pdf/PDFiumLibrary.cxx) 使用 `FPDF_LoadMemDocument`，调用方必须让输入内存在文档存活期间保持有效。`renderPageBitmap` 内部调用 `FPDF_RenderPageBitmap` 和表单绘制；当前封装的 rotation、flags 为固定值。

**区域渲染已可表达：**为视口分块分配小目标位图，将整页渲染尺寸设为目标缩放后的页尺寸，用负偏移把所需区域移入小目标图。无须先增加 UNO “分块渲染”接口，也无须先画完整大图再裁切。边缘接缝、实际分配尺寸、步幅、像素格式、透明度、页旋转和表单／批注策略仍需处理。

**封装尚未提供的能力：**随机访问文件回调式输入、渐进渲染的中断接口、完整阅读请求调度。底层引擎可能另有能力，不能把它们当作当前封装已公开的接口。

### 3.2 线程和所有权

当前 PDFium 封装没有为所有 Office 调用方提供统一的全局串行锁。只在新模块内部加锁，不能保护与原有 PDF 导入、导出等调用的并发关系。不能假设不同文档即可任意并行。

需要决定进程内协调、统一串行执行或隔离进程等策略，并核对所用 PDFium 版本和现有调用者约束。串行工作线程也不意味着同步渲染可随时中断；请求取消至少应先实现排队取消和过时结果丢弃，关闭时仍需安全等待正在使用的资源。

源字节、文档、页和图像缓冲必须按依赖顺序销毁。跨线程交接优先使用明确拥有数据的缓冲，不能把借用的 PDFium 指针或短生命周期渲染对象交给 UI。

### 3.3 原生绘制与连续视口

[`include/vcl/window.hxx`](../../include/vcl/window.hxx) 提供 `Paint`、`Invalidate`，[`include/vcl/outdev.hxx`](../../include/vcl/outdev.hxx) 提供 `DrawBitmap`，[`include/vcl/svapp.hxx`](../../include/vcl/svapp.hxx) 提供 `PostUserEvent`、`RemoveUserEvent` 等 UI 调度基础设施。新模块可以直接绘制缓存图像；布局、优先级、预取、预算、事件代次与关闭清理仍由模块负责。

[`include/svx/svdpntv.hxx`](../../include/svx/svdpntv.hxx) 的 `SdrPaintView` 有 `ShowSdrPage`、`HideSdrPage`、重绘和覆盖层接口，但其活动页视图围绕单个 `mpPageView`。临时 `SdrPaintWindow` 不能在 `EndCompleteRedraw` 后继续假定有效。

[`OverlayManager`](../../include/svx/sdr/overlay/overlaymanager.hxx) 与 [`OverlayObject`](../../include/svx/sdr/overlay/overlayobject.hxx) 可作为覆盖层基础，不是完整 PDF 阅读画布。绘制覆盖层并不会自动提供编辑焦点、文本输入、鼠标捕获、剪贴板和撤销上下文。

[`XDrawView`](../../offapi/com/sun/star/drawing/XDrawView.idl) 仅提供当前页切换；[`sd/source/ui/view/drviews1.cxx`](../../sd/source/ui/view/drviews1.cxx) 的切页逻辑会隐藏旧页、显示新页。未发现可直接作为任意多页 PDF 与编辑页连续混排宿主的公共接口。

### 3.4 LibreOfficeKit 是另一候选，不是免费嵌入层

[`include/LibreOfficeKit/LibreOfficeKit.hxx`](../../include/LibreOfficeKit/LibreOfficeKit.hxx) 已有 `paintTile`、`setPart`、输入事件和回调接口。Draw 实现在 [`sd/source/ui/unoidl/unomodel.cxx`](../../sd/source/ui/unoidl/unomodel.cxx) 的 `paintTile`、`initializeForTiledRendering`、`postMouseEvent` 等方法；使用既有视图、坐标转换和 tiled painting 状态。LOK 初始化见 [`desktop/source/lib/init.cxx`](../../desktop/source/lib/init.cxx)。

它可作为已加载编辑文档的显示与输入候选。但不会自动提供 PDF 延迟转换、轻量首屏，也不能未经验证直接与普通原生 GUI 模式混用。是否采用需单独决定进程、模式和生命周期边界。

## 4. 编辑边界

### 4.1 基线已有 PDFium 单页转绘图对象入口

[`include/svx/svdedtv.hxx`](../../include/svx/svdedtv.hxx) 的导出类 `SdrEditView` 公开 `DoImportMarkedMtf`。实现见 [`svx/source/svdraw/svdedtv2.cxx`](../../svx/source/svdraw/svdedtv2.cxx)：对选中的 PDF 类型 `SdrGrafObj`，构造 `ImpSdrPdfImport`，使用 `Graphic::getPageNumber()` 向对象列表导入，然后处理原图形、选择和撤销。

这证明现有原生接口可以表达指定 PDF 页转换，不必先扩展远程 UNO。它也有明确前提：必须正确准备图形的源数据、页号、矩形、所属模型和选择状态；调用会修改目标对象列表与撤销栈，不是纯解析函数。

[`svx/source/svdraw/svdpdf.cxx`](../../svx/source/svdraw/svdpdf.cxx) 中的细节需要特别注意：

- 构造转换器时重新打开 PDF；嵌入字体映射可以缓存在共享 `GfxLink` 中。
- 字体缓存尚未建立时，`CollectFonts` 会遍历全部页。指定页对象转换不等于完全没有全文件前置扫描。
- `DoObjects` 打开选定页处理对象；经 `SvdProgressInfo::ReportActions` 有进度及部分中断检查。
- 上述进度支持不是完整的异步转换、原子提交和取消回滚契约；不能笼统声称“完全不可取消”，也不能把中断当成事务成功回滚。
- `ImpSdrPdfImport` 的声明位于内部 [`svx/source/inc/svdpdf.hxx`](../../svx/source/inc/svdpdf.hxx)，不能假定新库可以直接依赖这个私有类。

该路线与 sdext 的 Poppler 导入路线不同。字体、文本可编辑性、裁剪、透明度、图片、路径、表单和复杂布局的保真及耗时均需分别验证。

### 4.2 既有 Poppler／SAX 导入路线

注册入口见 [`sdext/source/pdfimport/pdfimport.component`](../../sdext/source/pdfimport/pdfimport.component)。[`pdfiadaptor.cxx`](../../sdext/source/pdfimport/pdfiadaptor.cxx) 的 `PDFIRawAdaptor::parse` 创建 `PDFIProcessor`、调用 xpdf wrapper，再生成 SAX；其 `importer` 接口需要 `InputStream`，不能只提供 URL 就假定成立。

[`wrapper/wrapper.cxx`](../../sdext/source/pdfimport/wrapper/wrapper.cxx) 的 `xpdf_ImportFromStream` 把流完整复制到临时文件，再通过文件路径启动子进程。基线 [`xpdfwrapper/wrapper_gpl.cxx`](../../sdext/source/pdfimport/xpdfwrapper/wrapper_gpl.cxx) 识别 `SkipImages` 并循环 `1..nPages`。接受字符串 `FilterOptions` 不表示已实现 `Page=n` 或选页协议。

[`tree/pdfiprocessor.cxx`](../../sdext/source/pdfimport/tree/pdfiprocessor.cxx) 完成优化、样式收集和 XML 输出；[`XmlFilterAdaptor.cxx`](../../filter/source/xmlfilteradaptor/XmlFilterAdaptor.cxx) 展示目标文档与 `SvXMLLegacyToFastDocHandler` 的连接。

**结论：**可以复用原有转换技术，但它不是基线现成的持久 PDF 会话或按页低延迟导入服务。若选此路线，需要另行设计选页、进程复用、流与临时文件开销，以及导入事务。

### 4.3 任意目标页与资源隔离

[`XImporter`](../../offapi/com/sun/star/document/XImporter.idl) 只有 `setTargetDocument(XComponent)`，没有任意目标页、资源命名空间或原子提交参数。正常 Draw XML 导入的页定位见 [`xmloff/source/draw/ximpbody.cxx`](../../xmloff/source/draw/ximpbody.cxx)：按内部页计数复用已有页或追加，并处理页属性等内容。

不能把整个活动阅读模型交给通用导入器，就假定它只改所需页而不会触碰其他页、样式或默认设置。暂存模型、资源冲突检查与受控提交是候选方案，尚非现成契约。

### 4.4 编辑视图和模型转移

| 接口 | 可复用部分 | 不能直接推导的能力 |
| --- | --- | --- |
| [`SdrView`](../../include/svx/svdview.hxx) | 原生绘图视图、鼠标键盘与对象编辑基础 | 完整 Draw 菜单、工具策略、Frame 和多页宿主 |
| [`SdrModel`](../../include/svx/svdmodel.hxx) | 撤销管理、分组、模型合并 | 页面和共享资源无成本迁移 |
| [`SvxDrawPage`](../../include/svx/unopage.hxx) | 原生页桥接和内部 tunnel | 跨版本稳定的外部 SDK 契约 |
| [`SdrExchangeView`](../../include/svx/svdxcgv.hxx) | `Paste` 批量粘贴对象 | 任意文档无损、零复制的页接管 |

[`svx/source/svdraw/svdmodel.cxx`](../../svx/source/svdraw/svdmodel.cxx) 的 `Merge` 实际克隆页面，再按参数处理源页，不能只据头文件中的“move”文字当作零复制移动。实现见 [`svdxcgv.cxx`](../../svx/source/svdraw/svdxcgv.cxx) 的 `Paste` 同样涉及对象克隆、位置、层、连接关系和撤销；样式等资源需要额外核对。

[`sd/inc/drawdoc.hxx`](../../sd/inc/drawdoc.hxx) 虽有部分页面与样式辅助函数，`InsertBookmark`／`InsertBookmarkAsObject` 标记为 `SAL_DLLPRIVATE`，不能默认新模块可跨库调用。未找到 Draw 对 `XTransferableSupplier` 的可用整页转移实现，不能把该接口当作既有解决方案。[`sd/source/ui/view/sdview2.cxx`](../../sd/source/ui/view/sdview2.cxx) 的 `CreateClipboardDataObject` 会涉及模型克隆与系统剪贴板，不宜假定为私有事务交接通道。

### 4.5 尚需定义的编辑契约

- 稳定源页身份与实际 Draw 页的映射，不以创建顺序或当前页号作为源页标识。
- 准备、校验、UI 线程提交和当前请求独占资源的回滚；过时请求不得清理后来请求或用户产生的对象。
- 样式、字体、图层和共享资源的所有权及冲突策略，不能只移动对象列表。
- “已转换”“已激活编辑”“用户已修改”分别管理；转换本身不应污染文档修改状态。
- 缓存预算仅回收可重建阅读数据，不能丢弃编辑状态、撤销记录或未保存结果。
- 源页与编辑页混合保存及导出语义；`PDFiumDocument::saveWithVersion` 本身不是此协调器。

## 5. 导出、构建和开关边界

头文件存在不等于整个类都能从新库调用。[`DrawDocShell.hxx`](../../sd/source/ui/inc/DrawDocShell.hxx) 和 [`DrawViewShell.hxx`](../../sd/source/ui/inc/DrawViewShell.hxx) 使用 RTTI 导出及部分方法导出，不能假定所有内部方法是公共跨库 API。应优先通过明确导出的 PDFium、`SdrView`／`SdrEditView` 或进程内服务边界集成。

`XRenderable` 是已加载文档到输出设备的渲染接口，不能代替原始 PDF 会话、异步区域调度或按需导入。

已有构建入口可参考：

- [`configure.ac`](../../configure.ac)、[`config_host.mk.in`](../../config_host.mk.in)：PDFium／PDF import 配置检测和生成变量。
- [`config_host/config_features.h.in`](../../config_host/config_features.h.in)：特性宏模式；若生成 0／1 宏，应使用 `#if` 判断值。
- [`RepositoryModule_host.mk`](../../RepositoryModule_host.mk)：宿主模块清单。
- [`svgio/Module_svgio.mk`](../../svgio/Module_svgio.mk)、[`svgio/Library_svgio.mk`](../../svgio/Library_svgio.mk)、[`svgio/svgio.component`](../../svgio/svgio.component)：解析服务模块组织参考。
- [`filter/Library_pdffilter.mk`](../../filter/Library_pdffilter.mk)：PDF 过滤器构建参考，不是独立阅读应用入口。

拟议选项 `--enable-loongoffice-pdf-fast` **尚未实现**。开关应覆盖编译目标、链接、组件注册、UI、资源和安装。启用构建也不能让普通文档无条件加载完整阅读与编辑库；需要延迟加载边界。依赖关系应按实际选定路线检查，PDFium 阅读与 sdext Poppler 导入不能混为同一必选依赖。

## 6. Okular 源码对照

参考为本地 `E:\projects\okular-reference`，远端标识 `https://invent.kde.org/graphics/okular.git`，调查提交 `bb02e97fbc969b11a7b7cc876a94682b242234b8`。该目录不属于本仓库，也不是本方案构建依赖；以下为该版本源码观察，未做运行对比。

| 文件／符号 | 观察 | 对新模块的启示 |
| --- | --- | --- |
| `generators/poppler/generator_pdf.cpp`：`PDFGenerator`、`image` | 持有文档；声明线程、分块、取消能力；区域 `renderToImage` 携带中断回调 | 会话与区域任务分离；封装真实取消能力，不只丢弃输出 |
| 同文件：`loadPages` | 仍遍历页并装载部分批注、表单和动作信息 | 不能声称 Okular 完全不扫描全文件；需要拆解各阶段实际成本 |
| `core/generator.cpp`：`generatePixmap` | 异步生成图像 | 渲染不能阻塞 UI 事件循环 |
| `part/pageview.cpp`：可见请求与预取 | 可见区域异步请求、归一化区域、不同内存策略；Greedy 可预取全文件 | 学习调度机制，不照搬“永远只处理附近页”的不实假设 |
| `core/document.cpp`：请求与缓存管理 | 请求排序、缓存清理、非可见分块处理 | 明确任务优先级、内存预算及淘汰条件 |
| `gui/pagepainter.cpp`、`part/pageview.cpp` | 页面视图绘制已有缓存内容 | 阅读显示独立于 Draw 对象转换 |

可借鉴的是持久文档、区域渲染、异步请求、优先级和缓存预算。无需因为参考项目使用 Qt／Poppler 就预先引入相同依赖；达到相同阅读体验仍需实测。冷启动还受入口、配置、动态库和字体初始化影响。

## 7. 调查结论

单独模块有现实基础：底层启动、绘制和部分单页转换能力已存在。放弃外部 Python／远程 UNO 主路径可减少控制层约束，但不会自动补上生命周期、混合画布和事务边界。

下一步应先验证轻量首屏与原生编辑宿主能否通过现有导出接口组合，再选择转换路线。零原有业务源码改动、完整 Draw 编辑体验与 Okular 级冷启动能否同时满足，目前没有运行证据，不能提前承诺。
