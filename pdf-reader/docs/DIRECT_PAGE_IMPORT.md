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

本地 Python 单测 55 项：54 通过、1 项环境依赖跳过。新增覆盖不逐对象复制、
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
