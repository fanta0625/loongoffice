# PDF 编辑导入：现有能力复用调查

调查基线：本分支 98bcc7ae87dd；2026-11-10。本文是源码调查，不代表新的实现已通过运行验证。

## 后续修改约束

优先调用 LibreOffice 已有的解析、样式、资源映射、对象创建和事务能力。
新增代码限于现有能力缺失的目标页接入和会话调度；放在独立文件，尽量不改通用流程。
不能因少数样例通过而逐项硬编码字体、字距、边距或对象宽度，也不能放宽几何断言掩盖差异。
确需修改旧代码时，先注明缺失能力、接入点和普通导入的回归范围。

## 已有调用链及当前缺口

普通编辑导入：

```text
FilterFactory(draw_pdf_import) → XmlFilterAdaptor
  → PDFIRawAdaptor / DrawPDFImport → xpdfimport 指定页解析
  → SvXMLLegacyToFastDocHandler → SdXMLImport
  → SdXMLStylesContext → XMLShapeImportHelper / SdXMLShapeContext
  → 最终 Draw 对象
```

`filter/source/xmlfilteradaptor/XmlFilterAdaptor.cxx` 的 importImpl 已负责创建
XML importer、setTargetDocument 和 legacy/fast SAX 桥接。桥接不是需要新写的转换器。
`pdf-reader/uno/python/edit_bridge.py::open_for_edit` 已复用 FilterFactory 的整条路径。

新直接导入入口已经复用了 PDF 解析和形状创建，但 `PDFPageResourcesContext`
跳过了 office:styles 的 named style，破坏了原有父样式解析链。不能称为完整复用。

## 文本宽度差异的源码解释

1. `sdext/source/pdfimport/tree/drawtreevisiting.cxx::DrawXmlFinalizer::visit(FrameElement&)`
   发出 graphic 自动样式，父样式名为 standard，开启 auto-grow-width/height，清零文本 padding。
2. 同文件 TextElement 的处理发出实际字体和字号；没有为每个文字片段显式发出全部默认属性。
3. `sdext/source/pdfimport/tree/style.cxx::StyleContainer::getStandardStyleId`
   生成 named standard style；`emit` 把它放入 office:styles。
4. `xmloff/source/draw/ximpstyl.cxx::SdXMLStylesContext::ImpSetGraphicStyles`
   对同名样式使用已有 property mapper 和 XPropertyState 重置直接属性，再填充导入属性；
   不存在时通过样式族的 XSingleServiceFactory 创建样式。
5. 同文件 `endFastElement` 为自动样式关联已经解析的父样式。
6. `xmloff/source/draw/ximpshap.cxx::SdXMLShapeContext::SetStyle` 优先使用解析好的样式；
   没有该样式时会按父样式名查目标文档的 graphics 样式族。

当前跳过步骤 3–5 的 named standard 处理，使步骤 6 采用阅读文档现有的 standard。
目标机实测 actual CharAutoKerning=true、expected=false，且背景对象的默认字体、字号和
padding 也不相同。这与继承目标默认样式的路径一致。字偶距是文本宽度变化的直接候选，
但仍须修复后用严格几何和渲染对照确认，不能仅根据属性推断宣称修复。

`sd/source/core/stlsheet.cxx::SdStyleSheet::setPropertyToDefault/getPropertyDefault`
已经实现属性池默认值处理。应复用该默认值语义，不硬编码一张 PDF 默认属性表。

## 其他可复用路径及代价

| 现有能力 | 源码 | 可复用部分与限制 |
|---|---|---|
| 标准 XML 样式导入 | xmloff/source/draw/ximpstyl.cxx | 默认值、自动样式、父样式绑定都已有；直接导入应保留这条处理链，补最小命名隔离 |
| 样式冲突复制 | sd/source/core/stlpool.cxx，RenameAndCopyGraphicSheets / CopySheets | 根据 ItemSet 判断同名冲突，重命名、复制并恢复父关系；需要源样式池，不是可直接替换 SAX 样式导入的 API |
| 整页插入 | sd/source/core/drawdoc3.cxx，InsertBookmark / copyStyles / insertAllPages | 已有样式和母版迁移；插入或替换页会涉及当前会话固定页身份，不能未经适配直接调用 |
| 页面合并 | svx/source/svdraw/svdmodel.cxx，SdrModel::Merge | 跨模型明确调用 CloneSdrPage；即使允许修改源模型，也先克隆再删除原页，不是零复制移动 |
| 对象粘贴 | svx/source/svdraw/svdxcgv.cxx，SdrExchangeView::Paste(SdrModel...) | 在原生循环中 CloneSdrObject，处理单位、位置、图层、连接器和撤销；仍分配新对象，位置默认按包围盒中心平移 |
| 插入书签对象 | sd/source/core/drawdoc3.cxx，InsertBookmarkAsObject | 创建 marked-object 临时模型，再 Paste；不能作为消除临时模型/复制的直接答案 |
| UNO 页面容器 | offapi/com/sun/star/drawing/XDrawPages.idl、XDrawPages2.idl | 新建、删除和命名新页；这些接口没有跨文档接管现有页的入口 |

原生批量复制可能大幅减少 Python/URP 往返，但源码无法证明具体速度或内存峰值；
不能把它和先前的 Python 逐对象复制视为同一性能结果，也不能称为共享内存。

## 收敛方向

优先修复直接导入中被绕过的标准样式链：保留 SdXMLStylesContext 的 named-style 创建、
属性默认值处理和自动样式父关系绑定；通过原有 CreateStyleChildContext 等扩展点接入
本次请求的命名隔离，避免重置主文档 standard。新增适配放独立文件。

仅给资源改名或仅在 finishShape 中设置 CharAutoKerning=false 均不完整；当前
PDFPageResourcesContext 的忽略 named-style 行为应移除或由上述适配替代。
具体私有样式如何取得与标准导入一致的默认值，必须通过源代码路径及运行对照确认，
不能假设创建一个带前缀的空样式就天然等价。

若完整保持标准样式语义需要更大侵入，备选应是标准单页导入加现有原生批量迁移，
明确接受一次原生复制，先测量再决定；不再设计 Python 属性重放或自制通用复制器。

本次不改变生产路径，不要求用户再次编译或重复运行未修复的探针。
下一次功能变更应同时给出严格几何对照、文本属性/渲染对照、主文档样式不变、
历史编辑及撤销保留、失败清理和大页耗时的证据。

## 后续实现记录

样式适配已移到 `xmloff/source/draw/pdfpageimportstyles.cxx`。继承已有的
SdXMLStylesContext 和 XMLShapeStyleContext，仅在已有虚函数扩展点改写命名图形样式名
和父样式名；标准创建、属性填充及父样式绑定仍执行原实现。没有覆盖 final 的 SAX 工厂，
也没有手工设置 CharAutoKerning。SdStyleFamily::createInstance 调用
SdStyleSheet::CreateEmptyUserStyle，GetItemSet 创建没有父 ItemSet 的空属性集合，
这是私有样式使用属性池默认值的现有实现依据。

这次修改需要目标机重新编译并通过严格几何探针。源码依据不替代运行验证。
