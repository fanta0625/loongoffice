# 指定目标页直接导入

## 目标与边界

复杂第一页包含 4254 个顶层对象。旧路径指定页解析约 4 秒，逐对象跨文档复制达
127 秒；两轮 Python 批处理实验回退到约 507/518 秒。新路径消除临时 Draw 文档
和第二次对象克隆，不是操作系统共享内存：

```text
PDF 快照 → xpdfimport 指定页解析 → 原生 SAX → 主文档目标页的新 Draw 对象
                                                  ↓
                                   提交前保留阅读预览，完成后移除
```

PDF 解析树、图片解码和 Draw 对象仍需内存，不能宣称零分配或已解决所有内存峰值。
阅读和瓦片仍使用 Office 内部 PDFium，没有引入外部渲染引擎。

## 原生入口

新增 Draw 服务 `org.loongoffice.comp.Draw.PDFPageContentImporter`，以
`TargetPage`、`EditLayerName`、`ResourcePrefix` 初始化，提供 XImporter 和
XDocumentHandler。现有 SvXMLLegacyToFastDocHandler 在 Office 进程内桥接 SAX，
XML token 不逐个回到 Python。

- 目标必须是当前 Draw 文档中的页，开始时恰有一个对象；Python 验证这是拥有的预览。
- 只接收一页内容；不清空页面、不改名、不替换母版、不应用页面设置/元数据/脚本。
- 用 PDF 发出的页面布局检查宽高与预览页相差不超过 30 个 1/100mm 单位。
  缺少尺寸或不匹配时拒绝导入；没有新增 CropBox/旋转适配。
- 自动样式供本次内容使用。PDF 的命名图形样式和父样式引用采用请求前缀，
  复用原生 SdXMLStylesContext / XMLShapeStyleContext 创建独立样式并绑定父关系。
  原有主文档 standard 不被重置；字体、字偶距、边距使用原生属性池默认值，
  不使用一张手工列举的 PDF 默认参数表。适配位于 pdfpageimportstyles.cxx。
  虚线和填充位图资源以每次请求独立前缀命名并映射引用，避免覆盖已有资源。
  不认识的资源类型拒绝导入，不静默丢失。
- 对象在最终模型中创建，原生 finishShape 设置编辑图层，包括分组和子对象。
  不经过跨文档克隆与几何缓存重放。通用 ODF 导入入口保持原行为。

## 会话事务

read_edit.py 默认直接导入；缺少新服务时明确报错，不静默退回慢路径。
`--legacy-edit-copy` 仅用于对照；原有 probe_page_edit.py 继续覆盖旧路径。

成功路径只有页级调用，没有 Python 逐对象查询、复制和验证。事务期间锁定
主文档控制器与撤销管理器，保留此前撤销历史。成功后移除预览并记录已编辑页。
异常或提交前导航变化时，清除目标页新增对象和本次命名的图形样式、虚线/位图资源，
恢复修改标志，保留预览；只有失败路径会通过 UNO 枚举对象。

这是同步整页原生调用：进行中没有逐对象进度，也不能立即中断解析；导航变化
在调用返回、提交前检查。不能描述成可抢占的后台导入，UI 是否短暂阻塞须实测。
邻页预取暂时关闭，避免继续创建临时 Draw 模型；后续需另行设计解析结果缓存
或可取消的增量原生事务。

`edit.direct.import.begin/end` 与 `edit.direct.summary` 提供耗时。成功事件标记
`edit_route=native-direct-page`、`temporary_draw_documents=0`、`copied_shapes=0`。
`copy_seconds=0` 表示没有二次复制，不代表整页导入零耗时。

## 验证

本地 Python 单测 57 项：56 通过、1 项环境依赖跳过。新增覆盖不逐对象复制、
部分失败回滚、导航取消和旧 Office 服务缺失。旧路径的 WSL UNO 适配回归通过；
它不验证新的原生服务，且其已有像素一致性字段仍为 false。
本地未配置这棵源码的 C++ 构建，不能宣称新原生入口已编译或端到端通过。
样式修复后的本地 WSL 私有 Office 连接尝试未成功，未取得这次修复的原生运行结果；
上面的旧路径回归记录不构成本次样式修复的验证。

新增原生 Cppunit `testPDFDirectPageContent` 验证：非第一页目标、预览保留、
页面名称/尺寸/母版、原有样式及虚线资源、分组图层和尺寸不匹配拒绝。
样式回归现在额外要求主文档 standard 的直接属性保持不变、导入对象绑定私有
standard 样式且采用原生 CharAutoKerning 默认值；源样式填充色仍由原生 mapper 导入。
新增 `probe_direct_edit.py` 用原生指定页导入作测试对照，检查几何、可编辑文字、
已编辑页渲染、历史撤销、失败回滚和提交前导航取消。对照文档仅用于测试。
文字通过 XText.setString 修改；该调用走 QuickInsertText/UpdateData，不能要求它
像界面输入一样生成文档撤销记录。探针用 Draw 原生 .uno:Delete 删除一个测试对象，
创建非空历史，导入另一页后检查历史保留并执行 undo/redo；另外检查撤销锁已释放。
这验证原生删除的撤销/重做，不宣称已经验证 GUI 键盘输入文字的撤销。

目标机执行：

```bash
set -o pipefail
make PARALLELISM=4 build 2>&1 | tee ../pdf-direct-page-build.log
make PARALLELISM=4 CppunitTest_xmloff_draw \
  CPPUNIT_TEST_NAME=testPDFDirectPageContent 2>&1 | tee ../pdf-direct-page-unit.log

(
  export PYTHONPATH="$PWD/instdir/program"
  export LD_LIBRARY_PATH="$PWD/instdir/program"
  export URE_BOOTSTRAP="vnd.sun.star.pathname:$PWD/instdir/program/fundamentalrc"
  timeout 180s python3 pdf-reader/tools/probe_direct_edit.py \
    --office "$PWD/instdir/program/soffice"
)
```

小文件回归通过后，沿用原 500 页 read_edit.py 命令。编辑第一页，检查文字/路径
可编辑、其他页预览完整、返回已编辑页修改仍在。比较 edit.direct.summary 总时长
与旧 copy_seconds+import_seconds。目标机耗时尚待验证，不承诺具体倍数。
保存仍未实现，不允许把混合预览页当成完整编辑结果覆盖原 PDF。

## 现有文档导入的原生通知批处理（2026-11-10）

500 页样本直接导入第一页实测约 200 秒：其中 direct_import 199.85 秒，
commit 0.09 秒，view_unlock 0.66 秒。几何最终正确，中间构建状态可见。
这说明去掉二次复制并不足以解决向已打开文档插入对象的成本。

源码确认模型锁与修改通知是两个通道：

- sd/source/ui/unoidl/unomodel.cxx：lockControllers 调用 SdrModel::setLock。
- sd/source/core/drawdoc.cxx：加载完成且允许修改时，SetChanged 转入文档壳。
- sd/source/ui/docshell/docshell.cxx：DrawDocShell::SetModified 每次都会在允许修改时
  广播 DocChanged，即使修改标记已经为 true；控制器锁不检查这个条件。
- sd/source/ui/slidesorter/controller/SlsListener.cxx：DocChanged 触发母版和切换效果检查；
  具体扫描还有页数等条件，不能把每次广播都等同于扫描全部 500 页。
- offapi/com/sun/star/util/XModifiable2.idl 明确提供导入期间抑制修改更新的原生接口。

新增独立 Python 事务适配 native_import_batch.py，使用 isSetModifiedEnabled、
disableSetModified、enableSetModified；不修改旧 C++、不重写通知系统，也不创建
临时 Draw 文档。构建、提交预览替换、异常清理都在抑制期间执行，之后统一发布一次
成功/回滚修改状态。外部已经关闭修改通知时保持关闭，不擅自恢复。
使用显式状态查询，不依赖该基线 IDL 文档与实现存在歧义的返回值描述。

新增 summary 阶段 modification_suspend、modification_restore、modified_publish。
这项修复去掉一条确定存在的重复通知路径，但尚未在 Loongnix 的 500 页样本测量
收益，不能宣称已消除 200 秒耗时或保证中间画面完全不刷新。
模型/文字对象布局等其他成本仍可能存在。

本轮本地 Python 60 项测试：59 通过、1 项 PyUNO 环境依赖跳过；新增真实 UNO
探针检查导入中修改通知关闭，完成和失败后恢复，继续检查原生几何、图层和撤销。
该轮只修改 Python 和文档，无需重新编译；下述页级入口更新需要重新编译。
先运行 probe_direct_edit.py，再运行原有 500 页 read_edit.py 命令，比较
edit.direct.import 和 edit.direct.summary。真实原生探针与大文件耗时须在验证机确认。

## 原生页级执行边界（2026-11-10）

完整 perf 采样显示大量 CPU 样本落在 VCL Paint → Draw CompleteRedraw →
primitive 构造和 Cairo/Pixman 绘制。此前 Python 直接调用裸 PDF 转换器，
其逐对象 UNO 操作各自取得 SolarMutex，不能提供普通文档 load 所拥有的外层
SfxModelGuard 执行边界。样本比例不是墙钟时间，也不能据此承诺加速倍数。

新增独立文件 `xmloff/source/draw/pdfpageimportfilter.cxx`，注册
`org.loongoffice.comp.Draw.PDFPageImportFilter`。Python 现在只调用一次
XFilter.filter，参数为 URL 和从 1 开始的 SourcePage。入口在栈上持有
SolarMutexGuard，复用 SimpleFileAccess、DrawPDFImport 的 Page=N 转换和
已有 PDFPageContentImporter，不改通用绘制、解析、对象创建及样式映射逻辑。
已有 SAX 服务继续负责目标页验证、样式隔离和图层设置。

入口负责流关闭、自己取得的控制器锁恢复，以及转换失败或取消后的新增对象
回滚；保留调用者原有控制器锁。Python 继续负责撤销历史、修改通知抑制、
导航检查、预览提交和失败资源清理。成功路径不再跨 UNO 逐对象复制。

这是同步整页操作，导入期间窗口可能暂时不响应。cancel 只请求转换返回后
回滚，不会立即打断解析；不宣称后台可抢占导入。SolarMutex 也不代表禁止
同线程显式重入，实际是否消除中间绘制及耗时须在目标机验证。

成功报告新增 `native_entry=PDFPageImportFilter`，用于识别新入口。
本地 Python 测试 62 项：61 通过、1 项环境依赖跳过；新增桥接调用及失败检查。
新增 Cppunit `testPDFPageImportFilterFailureRestoresModel` 检查无效源文件时
预览和调用者锁状态保留，以及预先取消。该 C++ 测试和实际 PDF 探针尚未在
本地构建运行。

这次必须先 `make PARALLELISM=4 build`，再运行完整
`make PARALLELISM=4 CppunitTest_xmloff_draw` 和上面的 probe_direct_edit.py。
探针通过后，用同一单页 PDF 测量 edit.direct.import，再测 500 页文件。
