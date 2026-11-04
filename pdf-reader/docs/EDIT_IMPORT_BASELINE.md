# 干净 LoongOffice 26.2.6 的 PDF 编辑入口

核查提交：`af1911d32d3775e6a8becb3b0a9b7b1b52d9c894`，本地 `loongoffice/26.2.6`。

## 源码结论

**不需要旧快读分支的导入路由修改。扩展可以直接实例化原有 PDF 编辑过滤器并指定目标 Draw 文档。** 这取决于发行包实际包含组件和后端辅助程序；不能单凭源码声明目标安装已验证。

调用链：

```text
com.sun.star.document.FilterFactory
  createInstance("draw_pdf_import")
    → 按注册配置创建并初始化 com.sun.star.comp.Writer.XmlFilterAdaptor

desktop.loadComponentFromURL("private:factory/sdraw", ...)
  → 新 Draw 目标文档

filter.setTargetDocument(draw_document)
filter.filter({URL, InputStream, FilterName})
  → XmlFilterAdaptor::importImpl
  → com.sun.star.comp.Draw.XMLOasisImporter.setTargetDocument
  → org.libreoffice.comp.documents.DrawPDFImport.importer
  → PDF 解析 → SAX/XML → 可编辑 Draw 对象
```

| 位置 | 证据 |
| --- | --- |
| `sdext/source/pdfimport/config/pdf_import_filter.xcu:21` | `draw_pdf_import` 对应 DrawingDocument，FilterService 为 XmlFilterAdaptor，UserData 连接 DrawPDFImport 与 XMLOasisImporter |
| `filter/source/config/cache/filterfactory.cxx:73` | createInstance 调用带参数实例化；从配置获取 FilterService 并以完整配置初始化过滤器 |
| `filter/source/xmlfilteradaptor/XmlFilterAdaptor.cxx:62` | importImpl 从 UserData 读取转换器/目标导入服务，创建服务并设置目标文档 |
| `filter/source/xmlfilteradaptor/XmlFilterAdaptor.cxx:184` | 调用转换器 importer；必要时使用内部 SAX 适配包装 |
| `sdext/source/pdfimport/pdfiadaptor.cxx` | PDFIRawAdaptor::importer 要求 InputStream；解析后向文档处理器输出对象描述 |
| `sdext/source/pdfimport/pdfimport.component` | 注册 DrawPDFImport 等实现 |
| `sdext/Module_sdext.mk` | 组件受 ENABLE_PDFIMPORT、辅助程序受 ENABLE_POPPLER 等构建条件控制 |

## 为什么不只指定 loadComponentFromURL 的 FilterName

通常可以使用 `FilterName="draw_pdf_import"` 加载 PDF。但 `sfx2/source/doc/objstor.cxx:739` 起存在额外 SFX 路由：LibreOfficeKit、无 Poppler 构建或 `LO_IMPORT_USE_PDFIUM` 环境可能将正常加载引向 SdPdfFilter。明确指定过滤器名不等于一定绕过这些条件。

新入口直接调用已初始化的过滤器对象，绕过上述路由。不要手动直接调用 raw `DrawPDFImport.importer()` 配上 XML 导入器：基线 XmlFilterAdaptor 还负责 SAX/fast parser 的兼容适配，应沿用它。

## 验证边界

1. 本基线源码路径已核查。
2. `tools/probe_edit.py` 用真实 UNO 运行时验证“配置检查、实例化、目标文档、PDF 输入、可编辑文本结果”全链；结果应记录实际安装版本。
3. 若测试运行在其他版本，只能作为接口可行性证据，不能代替 LoongOffice 26.2.6/loong64 的实测。
4. 需要另外验证：加密 PDF 交互、用户取消、大文档进度、多窗口、失败恢复、保存/导出工作流。
5. 入口是完整编辑导入，不提供快读性能；阅读模块不应在后台自动调用它。
