# PDF Fast：原生 Draw 只读预览与按页编辑

分支 `feat/pdf-fast-26.2.6`，基线 `loongoffice/26.2.6`（`4f67c5b36cea7a6938b6ad5e559b4da8220767a5`）。

当前产品入口为原生 Draw：打开 PDF → 原生只读窗口与 InfoBar → 点击“编辑” → 当前页转换为原生 Draw 对象。编辑状态下访问其他页面才转换该页。只读显示复用 PDFium 后台预览、512 px 高清瓦片、需求复用、有限预取、缩放回退与有预算的缓存。窗口、工具栏、文本编辑、撤销、保存和导出由原生 Office 承担。

按页编辑使用注册的 `draw_pdf_import` 默认过滤器，经 Poppler → XML → Draw 原生导入器转换目标页，PDFium 负责只读瓦片。构建、测试与实际 GUI 观察范围见验证文档；尚无与 Okular 的同机性能结论。

## 启用与启动

`--enable-loongoffice-pdf-fast` 默认关闭，本分支的 Debian 配置脚本已显式启用。依赖 PDFium 和默认 Poppler PDF 导入器；验证机更新后重新生成配置。

```sh
sh debian/scripts/configure-libreoffice
make -j4
make CppunitTest_sd_pdf_import_test CppunitTest_sdext_pdfimport CppunitTest_vcl_graphic_test CppunitTest_vcl_outdev
SAL_LOG=+INFO.pdffast ./instdir/program/soffice "/absolute/path/sample.pdf" 2> native-pdf.log
```

采集前关闭已有 Office 进程，避免转交到未开启日志的旧进程。路径末尾不要多余空格。打开 PDF 应显示原生只读 InfoBar，点击“编辑”使用原生 Draw；通过原生菜单另存 ODG 或导出 PDF，不能假定支持覆盖原 PDF 的增量保存。

`loongoffice-pdf-fast` 独立程序保留为后端与随机文件输入的测量工具，其自定义窗口、F2、Esc 和独立编辑库不是最终产品操作入口。产品功能不运行 Python，`qa/summarize.py` 仅离线汇总日志。

## 当前边界

- 未编辑页面保留原始 PDF 图形，仅屏幕显示替换为瓦片；打印和导出保留原生路径。
- 编辑对象按整页转换，不是瓦片级对象映射。后台首次写入固定源并启动持续 helper；正常情况下后续页面复用它。主线程在临时 Draw 文档回放目标页并克隆迁入，复杂页面仍可能阻塞。
- 已编辑页面由原生 Draw 绘制，返回只读也保留修改结果，不覆盖成旧 PDF 缓存；编辑结果的版本化快照尚未接入。
- 原生导入仍完整读取文件并建立全部页面；批注／链接延迟读取，复制、编辑、保存、打印和导出前有完整性屏障。不能套用独立工具随机读取的首屏收益。
- 未转换页的可见缩略图共用后台预览；编辑页使用原生合成。启用特性时 VCL 进程级 PDFium 锁保护原生调用与后台渲染；页面解析、表单绘制及同步转换仍可能让 UI 等待。
- 无界面运行、LibreOfficeKit 和非 PDF 文件不新增这项桌面路由。

## 文档

- [原生改动收敛](docs/REFACTORING.md)：清理范围、独立 VCL 修复及只读门控的取舍。
- [原生 Draw 接入](docs/NATIVE_DRAW.md)：当前接入点、状态和线程边界。
- [验证与成本测量](docs/VALIDATION.md)：原生 GUI 验收、真实渲染测试和日志口径。
- [需求](docs/REQUIREMENTS.md)、[接口审计](docs/INTERFACE_AUDIT.md)、[初期架构](docs/ARCHITECTURE.md)、[实施计划](docs/DEVELOPMENT_PLAN.md)。
- [独立工具第一版](docs/IMPLEMENTATION.md)、[第一轮优化](docs/OPTIMIZATION.md)、[第二轮优化](docs/SECOND_PASS.md) 保留研发记录，独立窗口入口的约定由当前文档取代。
