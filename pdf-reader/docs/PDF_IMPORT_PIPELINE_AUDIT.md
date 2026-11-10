# PDF 编辑导入完整调用链调查

调查日期：2026-11-10。实现基线：d635f6d3015d。
范围是本分支的桌面 Draw PDF 编辑导入；不把 PDFium 阅读光栅化与编辑导入混为一谈。
这次只做源码调查，不修改运行代码。以下源码路径均相对仓库根目录。

## 结论

现有适配复用了 PDF 转换与 XML 对象创建，但绕过了完整文件加载的生命周期和
外层执行保护。正常打开与按需编辑不只是同一个导入函数的两种参数。

普通加载先加载模型，再创建文档控制器/画布，并有 SfxModelGuard 的外层
SolarMutex 保护。当前 Python 直接调用 PDFIRawAdaptor::importer，转换器入口没有
同等的外层保护；各个 Draw UNO 对象操作各自短暂获取 SolarMutex，GUI 主线程
有机会在中间构建状态下执行 Paint。模型锁与修改通知抑制不能替代这一执行边界。

用户完整采样显示 VCL Paint → DrawViewShell::Paint → CompleteRedraw → 图元生成
占约 46.9% 的报告 CPU 样本，支持优先修正该边界。不能把样本比例直接当成
墙钟耗时或预计加速比，也不能仅靠源码确认所有额外 96 秒都来自重绘。

## 一、普通打开 PDF 的外层

```text
Desktop / LoadEnv → SfxFrameLoader_Impl::load
  → 创建 DrawingDocument 模型
  → XLoadable::load
      → SfxBaseModel::load [SfxModelGuard]
          → SfxObjectShell::DoLoad
              → Disable modified-state notifications
              → 选择编辑导入分支
              → SfxObjectShell::ImportFrom
                  → FilterFactory(draw_pdf_import)
                  → XmlFilterAdaptor::filter / importImpl
                      → PDF 转换 + XML 对象创建
              → FinishedLoading
  → impl_createDocumentView
      → createViewController
      → ConnectFrameControllerModel
  → 框架显示文档
```

证据：

- sfx2/source/view/frmload.cxx:618：load 实现；774 调用 xLoadable->load，813 才
  调用 impl_createDocumentView。581 的视图工厂创建控制器并连接 frame/model。
- sfx2/source/doc/sfxbasemodel.cxx:1955：SfxBaseModel::load 创建 E_INITIALIZING
  的 SfxModelGuard；1999 转入 DoLoad。已初始化的模型再次 load 会抛异常，
  不能把它当作向当前页插入内容的直接替代。
- sfx2/source/inc/docundomanager.hxx:62：SfxModelGuard 持有
  SolarMutexClearableGuard。模型布尔锁与此互斥保护不是同一机制。
- sfx2/source/doc/objstor.cxx:664：关闭修改状态更新；735 起区分 PDFium 与
  STARONE 外部过滤器。767 走 ImportFrom；2598 实现创建过滤器、设置目标、
  组装描述符并调用 filter。
- 框架尝试 LockUpdates 属性，但捕获 UnknownPropertyException；Draw 模型没有在
  属性表实现该入口，不能据此假定 Draw 已经获得窗口绘制锁。

注意：普通文件加载在局部交互等代码中仍可能让出执行权，不能把 SolarMutex 描述为
绝不让步的全程冻结；核心事实是它有外层保护且当前文档视图尚未创建。

## 二、编辑导入分支与 PDFium 分支

sdext/source/pdfimport/config/pdf_import_filter.xcu 的 draw_pdf_import 配置：

- DocumentService：com.sun.star.drawing.DrawingDocument。
- FilterService：com.sun.star.comp.Writer.XmlFilterAdaptor；Writer 名字是历史命名。
- UserData：org.libreoffice.comp.documents.DrawPDFImport +
  com.sun.star.comp.Draw.XMLOasisImporter。

objstor.cxx 的 PDFium 分支可由构建特性、LOK 或 LO_IMPORT_USE_PDFIUM 选择，
此时进入 DrawDocShell::ConvertFrom → SdPdfFilter::Import（sd/source/filter/pdf）。
用户 perf 中实际出现 xpdfimport，证明这次普通编辑打开使用了转换器分支。
阅读模式使用内部 PDFium 则是另一条有意保留的路径，不能为编辑性能对照强制打开
LO_IMPORT_USE_PDFIUM 后再声称测的是同一条路径。

## 三、两条编辑路径共用的转换和对象创建

```text
XmlFilterAdaptor::importImpl
  → 创建 SdXMLImport，setTargetDocument
  → 创建 PDFIRawAdaptor（DrawPDFImport）
  → model.lockControllers / 异常安全恢复
  → SvXMLLegacyToFastDocHandler
  → PDFIRawAdaptor::importer
      → 从描述符取得 InputStream / FilterOptions / Password / StatusIndicator
      → xpdf_ImportFromStream
          → 将输入流复制到临时文件
          → xpdf_ImportFromFile
              → 启动当前安装目录的 xpdfimport [可传 -o Page=N]
              → 读取转换协议，建立 PDFIProcessor 元素树
      → PDFIProcessor::emit
          → optimizing visitor
          → style collecting visitor
          → SAX 发出样式和内容
              → SdXMLImport / SdXMLDrawPageContext
              → SdXMLShapeContext::AddShape
                  → XShapes::add → SvxDrawPage::add → 创建并插入 SdrObject
                  → 原生样式、几何、文字导入
                  → 原生 XActionLockable 对单个对象的更新保护
              → endDocument / Z-order 等收尾
```

证据：

- filter/source/xmlfilteradaptor/XmlFilterAdaptor.cxx:60、110、114、124、138、193。
- sdext/source/pdfimport/pdfiadaptor.cxx：parse、importer、Draw 服务工厂。
- sdext/source/pdfimport/wrapper/wrapper.cxx:1043、1254；转换器路径通过
  $BRAND_BASE_DIR 下的 xpdfimport 得到，并非任意系统 PATH 命令。
- sdext/source/pdfimport/tree/pdfiprocessor.cxx:694：优化、收集样式、输出三阶段。
- xmloff/source/draw/ximpshap.cxx:403：AddShape，474 添加对象 action lock，
  endFastElement 解除；无需我们重写一套属性批处理。
- svx/source/unodraw/unopage.cxx:150：add 的 SolarMutexGuard 只覆盖该对象操作。

单页输入和 500 页输入都会经过流临时文件复制；它不是 Draw 对象的跨文档复制。
Page=N 是本分支已有选页支持，不应因它是原生代码就认定毫无开销。

## 四、当前按需编辑的调用链及实际缺口

```text
已打开的 Draw 阅读文档 / 已创建且可见的控制器
  → Python ReaderSession._edit_direct_page
      → UndoManager.lock
      → model.lockControllers（SdrModel 布尔锁）
      → XModifiable2.disableSetModified
      → direct_edit.import_into_page
          → 新建 PDFPageContentImporter [目标页、图层、资源前缀]
          → setTargetDocument
          → 直接调用 PDFIRawAdaptor.importer（远程 UNO 调用）
              → 共用解析 / SAX / 对象创建
              → 各对象 UNO 方法短锁之间，主线程仍可绘制
      → 校验页身份、移除预览 / 失败删除新增对象
      → 恢复修改通知、发布一次修改状态、解模型锁和撤销锁
```

保留的新增适配：目标页复用、命名样式隔离、资源名映射、图层分配、尺寸校验。
它们不是原生完整打开所提供的指定页插入接口，仍需逐项保留回归覆盖。

遗漏的是外层执行/显示事务，不是缺少 PDF 优化 visitor，也不是缺少每个文字
对象的 action lock。两者均已复用。

证据：pdf-reader/uno/python/direct_edit.py、reader_session.py、native_import_batch.py；
新增服务工厂在 xmloff/source/draw/sdxmlimp.cxx:255，返回原生 LegacyToFast 桥。
该工厂构造服务时的短生命周期也不能覆盖后续的 converter.importer 调用。

## 五、三类保护必须区分

| 原生能力 | 已有作用 | 不能据此保证 |
|---|---|---|
| lockControllers → SdrModel::setLock | 抑制/推迟部分模型对象通知 | 可见窗口不会执行 Paint |
| XModifiable2 | 抑制 DrawDocShell 的 DocChanged 广播 | 其他失效请求或 GUI 重绘停止 |
| SfxModelGuard / SolarMutex | 原生模型操作的执行互斥边界 | 自动提供进度、取消或始终响应的 UI |

sd/source/ui/unoidl/unomodel.cxx:2175；svx/source/svdraw/svdpage.cxx:323；
svx/source/svdraw/svdobj.cxx:1011；sd/source/ui/docshell/docshell.cxx:426。

## 六、检查过的现有插入路径与绘制候选

- sd/source/ui/func/fuinsfil.cxx → InsertFileAsPage / InsertBookmarkAsObject。
  sd/source/core/drawdoc3.cxx:1078 的对象插入使用 CreateMarkedObjModel 后 Paste。
  它是可复用的原生跨文档复制，不是零复制；用于替代方案对照时须保留几何/样式/
  页身份检查，不能直接套用后宣称满足指定页直接创建目标。
- sd::View::CompleteRedraw（sd/source/ui/view/sdview.cxx:470）检查 mnLockRedrawSmph，
  但本次搜索没有发现 Draw 主视图提供可调用的计数 RAII；slidesorter 的
  DrawLock 属于另一种视图，不可错用到编辑画布。
- vcl::Window::SetUpdateMode（vcl/source/window/window.cxx:2978）只是设置标志并发送
  StateChanged。通用 Paint 没有检查该标志，Draw Paint 也没有检查它；不能仅靠
  函数名把它当作保证停止 Draw 绘制的通用接口。
- Scheduler::IdlesLockGuard 已是原生 RAII（include/vcl/scheduler.hxx，
  vcl/source/app/scheduler.cxx:279）；非主线程构造会等待主线程回到外层消息循环。
  Linux 调度器对 HIGH_IDLE 及以下任务应用此保护，含 REPAINT；画面 paint idle
  在 vcl/source/window/window.cxx:804 设置 REPAINT。
  这是候选机制，不是已验证的最终方案：进度/密码交互/嵌套循环/异常退出及对全局
  其他文档的影响必须检查，不能未经验证直接给整个 Office 关调度。
- 本分支 DrawViewShell::Paint 有 PdfHybridViewProbe 钩子；默认实验关闭时快速返回。
  当前采样显示主体是 CompleteRedraw/图元生成，不能仅因钩子存在就判定它是根因。

## 七、基于完整流程的下一步

先修正新增入口的原生执行事务，优先复用现有 SolarMutex / 调度 RAII，不去修改
通用 Draw Paint 或加另一套对象属性默认值。事务应该由独立的原生适配入口持有，
覆盖调用 converter.importer 的整个过程及结束清理；仅给 SAX startDocument 加一个
局部 guard、仅给服务工厂加锁均不够。不能持有线程专属锁跨不同 UNO 调用保存。

普通完整加载靠先创建模型再创建视图避开中间显示；活跃阅读文档不能简单重走
XLoadable::load / FinishedLoading，也不应伪装成未初始化的新文档。
适配选择应明确 UI 响应和取消的实际边界，不承诺同步整页导入可立即中止。

实施前检查进度/密码交互、UNO 回调线程，以及外层已有锁的恢复语义；修改只进入
独立适配文件和必要注册点。先保留已有指定页 SAX、样式隔离与图层逻辑。
验证用同一单页 PDF 对照完整原生打开与按需导入；检查几何、文字、图层、旧撤销、
失败恢复、其他窗口和导航，最后再测 500 页跳转。

## 八、现有实验能支持的结论

用户手机秒表：未修改安装版约 29 秒，修改版普通打开约 26 秒。不能据单次测量
声称加速，但普通打开没有出现与按需入口相当的巨大退化。
单页按需约 122 秒，排除 500 页规模是必需条件；样本存在 4254/4255 对象差异，
不可当作输入字节完全相同的严格实验。
完整采样 3954 个样本，无丢失，约 46.9% CPU 样本沿 VCL Paint/CompleteRedraw；
最初短时解析热点不能代表整段转换。无需让用户重复采样来代替本次源码调查。

本文件为源码审查与实验解释，不宣称候选修复已实现或通过原生构建。
