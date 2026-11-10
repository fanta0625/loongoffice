# 按需编辑对象复制性能调查

## 实测与结论

500 页 PDF 的第 1 页包含 4254 个顶层对象。目标机日志：指定页导入
4.251 秒，`copy_seconds` 127.005 秒。后者占这两个计时之和约 96.8%，
平均每个顶层对象约 29.9 毫秒（包含循环外开销，不是原生 clone 单次耗时）。
因此当前首要瓶颈是暂存文档到阅读文档的对象迁移阶段；不是阅读瓦片渲染。

源码可以确认存在逐对象跨 UNO 桥调用和逐对象模型通知，但不能仅凭源码
确定它们各占多少秒。原生克隆、样式迁移、布局和通知处理的实际比例仍需计时。
用户直接打开提取出的单页 PDF，不包含本方案额外的跨文档迁移步骤，因而
与按需编辑总耗时不是同一条调用链。

## 调用链与额外工作

`reader_session.py:edit_current_page()` → `edit_bridge.py:open_for_edit()` →
`draw_pdf_import` / `XmlFilterAdaptor`，带 `FilterOptions=Page=n` →
隐藏的暂存 Draw 文档 → Python 逐对象 `page.add(shape)` →
`SvxDrawPage::add()` → `CloneSdrObject(targetModel)` → 目标页插入。

`copy_seconds` 从暂存导入完成开始，包含完整性检查、源对象枚举、复制循环、
图层归属和提交；不包含最后 `staged.dispose()`，也不包含返回值中随后求值的
`view_state()`。邻页预取只缓存暂存文档，能省导入时间，不能省这段对象复制。

具体热点候选：

- `reader_session.py:current_page()` 每个对象都调用：获取 controller/current
  page，校验页数、名称和身份，然后 `_prepare_view()` 再读取两个图层的六个
  状态属性。用于取消和完整性保护，但把完整校验重复了 4254 次。
- 每个对象另外获取和设置源位置/大小、查询目标数量、插入、重新取得目标
  包装对象、读取并验证几何、设置图层；分组还递归设置子对象图层。
  顶层对象循环就有十万量级的 UNO 调用机会，实际数量依分组和桥实现而异。
- `svx/source/unodraw/unopage.cxx:SvxDrawPage::add()` 跨模型时原生 clone，
  重新绑定 UNO wrapper，并调用 `SvxShape::Create()`。
- `svx/source/unodraw/unoshape.cxx:SvxShape::Create()` 会重新应用缓存的
  `maPosition/maSize`。现有几何写回是为该路径播种缓存，不能为提速直接删除，
  否则会重现位置/尺寸错误。
- 当前转换只 `undo.lock()`，没有 `document.lockControllers()`。
  后者在 `sd/source/ui/unoidl/unomodel.cxx` 映射到模型 `setLock()`；
  `svx/source/svdraw/svdpage.cxx:SdrObjList::InsertObject()` 在模型未锁定时
  对每次插入广播 `ObjectInserted`。因此撤销锁并没有抑制这些视图通知。
  解锁本身也有延迟通知和连接线重排成本，必须计入测量。

启动器使用本机 UNO pipe；Python 与 Office 在同一台 Linux 机器。
SSH 到开发机不意味着每个对象经 SSH 往返，不能归因于远程终端延迟。

## 现有原生批量能力

`sd/source/core/drawdoc3.cxx:SdDrawDocument::InsertBookmarkAsObject()` 已有
跨文档对象插入，但会先 `CreateMarkedObjModel()` 创建中间模型再 `Paste()`。
`svx/source/svdraw/svdxcgv.cxx:SdrExchangeView::Paste()` 使用原生循环克隆，
并通过 `CloneList::CopyConnections()` 修复连接线关系，值得复用其处理经验。

它们不是可以直接替换当前循环的无副作用 UNO 批量接口：Paste 有居中移动、
单位缩放、目标图层、选择和撤销行为；书签入口还增加中间复制。必须适配为
“保留源坐标、指定现有目标页”的语义。单纯合并/替换整页也会破坏稳定页身份，
不能直接用作当前会话的对象替换。

## 建议实施顺序

1. 先增加聚合计时：源枚举、导航/图层检查、几何缓存播种、`page.add`、
   目标枚举/几何验证、图层设置、提交和暂存销毁。每批输出一次，避免每个
   对象打印日志。记录对象数、分组递归数、取消和回滚时间。
2. 优化现有 Python 路径：把完整页身份和图层修复移到批次边界；批内保留
   轻量失效检查；缓存 LayerManager；短批次锁定控制器并在 finally 解锁。
   初始可试 16～32 个对象并按耗时调整，解锁后允许视图处理输入。不要整页
   长时间锁定，也不要移除几何修复或清空已有撤销历史。
3. 若迁移仍主导，增加范围明确的原生批量复制桥：源页、目标页、批次范围和
   会话 token 为输入，在 C++ 内克隆、保持几何、设置图层并维护对象关系，
   一次返回进度。避免每对象跨桥和源 UNO wrapper 的重新绑定；保留逐批显示。
   分批接口需维护跨批连接关系、已插入对象清单和取消/失败回滚，不能只是
   把 Python 循环机械搬进一个长时间同步调用。

预览只在整页成功提交后移除；取消只回收本次插入对象，不能动其他页的用户
修改。成功后重复编辑复用已有对象。原生桥不改变 PDFium 阅读渲染引擎，
也不替换现有指定页编辑导入过滤器。

## 验证边界

在同一机器、相同源快照、相同构建/缩放条件下分别记录冷页转换和缓存命中，
至少重复三次。复用现有取消、失败回滚、预览保留、几何和撤销保护验证，
再验证复杂分组、文本、图片、透明度及连接线。目标机须确认转换中快速跳页
仍可响应，且取消后无残留对象。当前调查未做目标机分段性能测量，因此不承诺
具体加速倍数，也不把全部 127 秒归因于 UNO 通信。

## 已落地的第一轮优化

当前实现只修改 Python 适配层，不需要重新编译 Office：

- 完整页身份校验和图层状态恢复移到批次边界；批内每个对象仍通过缓存的
  controller 检查当前页，提交前重新完整验证。LayerManager 在会话内复用。
- 每批最多 16 个顶层对象，或在一个对象完成后发现本批已用时 100 毫秒就
  解锁，让 Draw 处理视图更新。100 毫秒不是硬实时上限，单次原生 clone
  不能中断。已有外层控制器锁不会被本批错误释放。
- 保留源几何缓存播种、目标数量/几何验证、递归图层归属、预览最后移除、
  取消/异常回滚与已有撤销历史。没有整页长时间锁定或使用剪贴板。
- `edit.copy.progress` 每秒最多一次，加最后一批进度；
  `edit.copy.summary` 在成功、取消和异常时输出聚合分段计时。

`phase_ms` 和 `phase_calls` 包含 `source_enumeration`、`navigation`、
`geometry_seed`、`native_add`、`target_validation`、`layers`、`view_lock`、
`view_unlock`、`commit`，以及发生时的 `rollback` 和 `staged_dispose`。
图层计时包含对子分组的递归处理；调用计数是阶段执行次数，不是 UNO 往返总数。
汇总总时长包含暂存销毁，原有 `copy_seconds` 仍不包含暂存销毁。
成功事件另带 `copy_profile`（销毁前快照），方便报告保存；完整数据以
`edit.copy.summary` 为准。各阶段只覆盖被包围的操作，不能要求相加严格等于总时长。

Windows Python 单测共 50 项，49 项通过、1 项环境依赖测试跳过；新增覆盖
分批/时间预算解锁、批内跳页取消、插入后抛错、几何错误、图层失败、外层锁
和分组子对象归属。WSL 的未打补丁 LibreOffice 上，
`probe_page_edit.py --adapter-only --cold-import` 实际验证冷包装对象的几何、
复制取消/失败回滚、预览保留、重复编辑复用和历史撤销保留。
该模式使用替代单页导入器，不证明目标构建的原生指定页导入或 GUI 响应。

目标机继续用同一 500 页文件，阅读后点击编辑第一页，收集新的
`import_seconds`、`copy_seconds` 和 `edit.copy.summary`。转换过程中跳到
第 450 页并返回，确认取消后预览完整、无残留；再重试转换并检查文本可编辑。
若 `native_add` 或 `view_unlock` 仍主导，再依计时决定原生批量桥的范围，
本轮不宣称已解决全部迁移开销。

像素回读比较 cross_document_raster_equal 在此 WSL 环境中为 false；
未修改前的基线同样为 false，因此本次不将其计为像素一致性验证通过。
