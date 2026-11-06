# P0-A 真实页面与连续总览切换实验

## 源码结论

当前基线的 Draw 编辑视图只维护一个活动 `SdrPageView`：

- `include/svx/svdpntv.hxx`：`GetSdrPageView()` 返回唯一的 `mpPageView`。
- `svx/source/svdraw/svdpntv.cxx`：`ShowSdrPage()` 在换页时释放旧页面视图，再创建新视图。
- `sd/source/ui/view/drviews1.cxx`：`SwitchPage()` 调用 `HideSdrPage()` / `ShowSdrPage(mpActualPage)`，同步控制器、图层等状态。

因此反复调用 `setCurrentPage()` 只能切页，不能同时激活多个页面。单个高页面实验通过，不代表多个真实页面能直接连续编辑。若坚持连续画布中原位编辑，需要进一步实现宿主与活动编辑视图的坐标、裁切、事件及生命周期适配；当前没有将这个要求降级为单页切换。

另一方面，现有 `com.sun.star.drawing.PageShape` 可引用真实页面：`svx/source/unodraw/unoshape.cxx` 的 `OWN_ATTR_PAGE_NUMBER` 设置 `SdrPageObj::SetReferencedPage()`。`svx/source/sdr/contact/viewobjectcontactofpageobj.cxx` 提取被引用页的绘图 primitive，并传播重绘失效。这提供一个不复制编辑对象的总览验证手段，但页面引用本身不是被引用对象的编辑视图。

还需注意 `sd/source/ui/unoidl/unopage.cxx` 的 `SetWidth()` / `SetHeight()` 会同步修改共用同一母版的页面。原型为合成总览分配独立母版，并检查内容页尺寸，避免总览高度改变真实页面大小。

## 本次实现与范围

`tools/p0a_page_switch_probe.py` 创建三个独立内容页和第四个实验用连续总览页。第 1／3 页放内部 PDFium 生成的预览；第 2 页为原生文本与矩形。总览以锁定的 `PageShape` 引用前三页，切换到第二页使用 Draw 原生编辑，再切回总览查看变化。

这比先前的高页面实验多验证了真实页面引用、活动页切换以及编辑后的引用刷新。总览依旧是一个合成高页面，并非 Draw 原生连续多页视图。第二页仍是测试对象，不是 PDF 指定页编辑导入。没有实现异步渲染、调度、图块、PDF 保存或 OXT 菜单；不修改 Office 的 `sd` / `svx` 源码。

## 目标机运行

更新分支后，在图形桌面终端运行。输出路径必须不存在，可更换文件名再运行。

```bash
(
    export PYTHONPATH=/opt/loongoffice/program
    export LD_LIBRARY_PATH=/opt/loongoffice/program
    export URE_BOOTSTRAP=vnd.sun.star.pathname:/opt/loongoffice/program/fundamentalrc
    python3 pdf-reader/tools/p0a_page_switch_probe.py \
        --office /usr/bin/loongoffice \
        --report /tmp/p0a-page-switch.json \
        --save /tmp/p0a-page-switch.odg
)
```

在脚本终端输入命令并回车：

1. 初始是连续总览：检查滚动缩放、上下 PDF 图像，以及中间黄色编辑页；页面引用不能普通选择、移动或删除。
2. 输入 `e`：同一 Draw 窗口切到真实第 2 页。修改文字、移动对象，检查撤销和重做。
3. 输入 `o`：返回总览，检查刚才的修改实际显示。再次 `e` / `o`，检查内容保留、缩放和滚动位置恢复是否符合预期。
4. 输入 `q` 或直接回车：结束并关闭本次私有 Office。勿在检查中直接关闭窗口后继续输入切页命令。

`--save` 保存的是交互前的 ODG 测试夹具，包含合成总览，不是 PDF 保存，也不会自动保存手动编辑结果。脚本使用私有 profile 和 pipe，不连接已有文档。`--headless` 仅执行自动检查，不代替 GUI 验收；运行自动检查可在外层加 `timeout 150s`。

自动报告中的 `multi_page_layout_verified`、`inline_editing_verified`、`specified_pdf_page_edit_import_verified` 均保持 `false`，不能由页面引用或切换成功推断为通过。

若报告 `status: unsupported-page-geometry`，退出码为 2，说明设置总览高度时真实内容页也被拉高。此时脚本关闭实验文档，不继续展示错误布局，也不保存 ODG；JSON 会记录期望和实际尺寸。这是宿主能力检查失败，不是缺少 Python 依赖，不应靠删除尺寸检查绕过。

## 开发环境验证（2026-11-06）

早期版本在 WSL / LibreOffice 25.2.3.2 headless 验证过引用页号、活动页切换、修改／恢复文字后的总览 PNG 哈希变化，以及 ODG 保存重开后的引用更新；预览来自内部 PDFium。随后查看导出图发现：该环境把三个内容页也同步为总览尺寸，图像缩在大页面中，因此早期结构检查通过不能证明页面布局正确。

当前版本新增严格的内容页尺寸门槛。开发环境旧版的预期结果是 `unsupported-page-geometry`，不将它记为布局通过。26.2.6 基线的 `SetWidth()` / `SetHeight()` 源码按母版身份限制同步范围，与旧版实测表现有区别；目标安装包能否通过仍需实际运行确认。原有单高页预览脚本回归检查通过，并修复了关闭最后一个文档后 Office 已退出时的清理异常。

目标 LoongOffice 的页面尺寸门槛和 GUI 行为尚待验证。此实验不作为最终连续混合宿主通过的结论；下一项源码适配需解决连续视口与单个活动编辑页的衔接，不应把所有编辑对象搬到总览页来绕过页面模型。
