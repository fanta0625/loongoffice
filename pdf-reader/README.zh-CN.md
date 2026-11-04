# LoongOffice PDF 阅读扩展

## 基线与工作区

- 基线：本地 `loongoffice/26.2.6` 分支，`af1911d32d3775e6a8becb3b0a9b7b1b52d9c894`。
- 开发分支：`feat/pdf-reader-uno-26.2.6`。
- 工作区：`E:/projects/loongoffice-pdf-reader-26.2.6`。
- 未移植旧 `feat/pdf-fast-import` 代码，也未修改 Office 既有源码和构建配置。

这不是原型功能已经完成的声明。目前提供独立目录、C++ 核心接口契约、UNO 编辑入口及可执行探测工具；阅读后端、调度/缓存实现、阅读窗口和 OXT 菜单打包仍待实现。

## 模块边界

```text
UNO 扩展适配层（uno/）
  菜单、文件选择、窗口生命周期、视口事件、UI 线程呈现、进入编辑
                 │ plain request / immutable image
                 ▼
独立阅读核心（core/）
  ReaderSession：调度、优先级、缓存、视图 generation、取消、关闭
                 │ DocumentBackend
                 ▼
可替换 PDF 后端
  PDFium 或 Poppler：文档解析、页面几何、区域渲染

进入编辑：UNO FilterFactory → 原有 PDF 编辑过滤器 → 新 Draw 文档
```

约束：

1. 核心公开头文件仅使用 C++17 标准库，不引入 UNO、VCL、Draw、Qt 或具体 PDF 引擎头文件。
2. 后端返回拥有自身内存的 RGBA 图像；引擎对象和 Office 对象不跨边界共享。
3. 核心拥有文档与 worker 生命周期；同一文档初期串行渲染。多文档并发必须服从引擎额外线程限制。
4. 视图以 `ViewId + Generation` 替换自己的需求，不能取消其他窗口需求；相同渲染规格可共享工作与缓存。
5. UI 通过取出完成队列更新画面。worker 不调用 UNO，不捕获 UI 裸指针。
6. 取消分为排队移除、请求取消、过时结果不发布。不能把“标记取消”等同于后端能立即中断。
7. 分别限制缓存、在途分配、排队数量；原 PDF/引擎缓存不算在位图预算内，另行测量。
8. 阅读窗口保留原 PDF 来源；编辑成功后才允许关闭阅读会话，失败时不丢失阅读状态。

`core/include/pdf_reader/backend.hxx` 定义引擎边界；`session.hxx` 定义调度/缓存边界。当前 CMake 目标是 INTERFACE 契约目标，尚无可运行阅读器库。可独立配置，不需要编译整个 Office：

```bash
cmake -S pdf-reader -B /tmp/pdf-reader-build
```

## 编辑过滤器核查

详见 [编辑入口核查](docs/EDIT_IMPORT_BASELINE.md)。扩展可用：

```python
from edit_bridge import open_for_edit
document = open_for_edit(office_context, original_pdf_url)
```

该入口经 `FilterFactory.createInstance("draw_pdf_import")` 得到初始化后的 XmlFilterAdaptor，创建新 Draw 目标并显式导入。它不依赖旧分支的 `SID_EDITDOC`、阅读标记或 SFX 路由改动。

新建的是未命名 Draw 文档，不会直接覆盖源 PDF；默认保存策略、标题、密码交互、进度与取消 UI 还需适配层完善。目前面向本地、非加密 PDF 的功能验证。

## 在目标机器验证

先保证 Python 导入的是目标 Office 对应的 UNO 运行时。原生发行版的 LibreOffice 通常可直接使用系统 `/usr/bin/python3`。对于安装在 `/opt/loongoffice` 的产品，在独立子 shell 中运行：

```bash
(
    export PYTHONPATH=/opt/loongoffice/program
    export LD_LIBRARY_PATH=/opt/loongoffice/program
    export URE_BOOTSTRAP=vnd.sun.star.pathname:/opt/loongoffice/program/fundamentalrc
    timeout 120s /usr/bin/python3 pdf-reader/tools/probe_edit.py \
        --office /usr/bin/loongoffice
)
```

探测脚本：

- 使用临时 profile、独有 UNO pipe 和独立 headless 进程，不连接现有用户文档。
- 自动生成一个包含文本与矩形的 PDF，验证导入后存在实际 Draw 可编辑文本。
- 显式设置 `LO_IMPORT_USE_PDFIUM=1`，验证直接调用过滤器不依赖 SFX 的正常打开路径。
- 输出运行产品版本、过滤器配置、页数与对象类型；退出时释放测试文档并终止自建进程。
- 可加 `--pdf /absolute/path/example.pdf` 观察真实文档；此时不检查内置测试文字，不宣称所有内容都可编辑。

探测是非交互式工具，应在外层使用 `timeout`，防止远程 UNO 调用异常阻塞。目标安装必须包含 PDF 编辑导入组件及其辅助程序。完成探测不等于 PDF 阅读器已实现，也不等于用户目标基线已经运行验证。

## 下一批实现

1. 在目标 26.2.6 安装包跑探测，确认组件注册、辅助程序与实际导入结果。
2. 实现一个独立后端和 `ReaderSession`，先测取消、内存预算、过时请求、多视图与关闭竞态。
3. 实现阅读窗口和显式“快速打开 PDF”菜单；不接管默认 PDF 过滤器，不向 Draw 插入阅读位图对象。
4. 当前页/邻页异步阅读稳定后再加入瓦片、高倍缩放与搜索/文本选择。
5. 打包 OXT 与 loong64 原生模块。窗口工具包与进程内/独立进程边界通过原型评估后决定，不能假定 UNO 能直接承载 Qt/KDE widget。
