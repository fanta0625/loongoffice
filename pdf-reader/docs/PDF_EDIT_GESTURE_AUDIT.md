# 按需编辑与缩略图手势的源码调查

## 范围与结论

用户在按需编辑后看到加号鼠标、缩略图禁止操作，继续点击会复制页面；Esc 可恢复。
这与原生页面拖放仍处于活动状态一致。确认的适配缺陷是：自动转换只检查当前页
停留时间，不检查选择/拖放事务是否结束。静态源码尚不能证明具体哪个鼠标释放
事件或拖放结束回调被延迟/遗漏，不能把这个推断写成已经复现的精确根因。

初步调查未改原生 Draw、Qt 或拖放代码。当时未提交的通用输入检查候选已另存本机
临时补丁并撤出工作树；它不能完整覆盖下述拖放生命周期，不应直接作为已修复版本。

## 原生选择、拖放、收尾

1. `sd/source/ui/slidesorter/controller/SlsSelectionFunction.cxx:321`：
   MouseButtonDown 进入选择状态机；CaptureMouse 调用本来就是注释掉的。
2. 同文件 NormalModeHandler::ProcessButtonDownEvent（约 970 行）：
   点击未选中的缩略图在按钮按下时就 SetCurrentPage；因此当前页变化不意味着点击结束。
3. 同文件 ProcessMotionEvent（约 1084 行）：左键按住且位移超过 3 像素即可 StartDrag。
   StartDrag 检查 `SdModule::pTransferDrag` 防止重复启动。
4. 同文件 DragAndDropModeHandler 构造（1383 行）进入拖放模式；
   `SlsClipboard.cxx::CreateSlideTransferable` 创建原生 SdTransferable，
   设置 pTransferDrag，允许 COPY/MOVE，然后调用 TransferableHelper::StartDrag。
5. `vcl/source/treelist/transfer.cxx:1016`：StartDrag 主动 ReleaseMouse，
   Linux 路径还通过 SolarMutexReleaser 释放执行锁，再调用系统拖放。
   因此“没有鼠标捕获”不等于“没有拖放”。
6. `vcl/qt5/QtDragAndDrop.cxx::QtDragSource::startDrag`：QDrag::exec 开启
   嵌套事件循环。拖放尚未完成时并不意味着普通 UNO 请求不能进入。
7. 结束后 fire_dragEnd → TransferableHelper::dragDropEnd →
   SdTransferable::DragFinished → SlideSorterView::DragFinished → Clipboard::DragFinished。
8. `SlsClipboard.cxx:600`：DragFinished 用 PostUserEvent 异步投递
   ProcessDragFinished；后者调用 NotifyDragFinished → SwitchToNormalMode。
   SdTransferable::ObjectReleased 清除 pTransferDrag，与这个异步选择模式恢复
   不是同一时刻，不能只检查 pTransferDrag 是否为空就认定全部收尾完成。
9. `SlsSelectionFunction.cxx:401`：Esc 调用当前模式 Abort 并 SwitchToNormalMode；
   DragAndDropModeHandler::Abort 调用 Clipboard::Abort 和拖放上下文 Dispose。
   这解释了 Esc 恢复现象，但不单独证明哪一条收尾路径出错。

## 我们的调用链与冲突窗口

`pdf-reader/tools/read_edit.py` 在外部 Python 循环每约 100 毫秒调用 EditMode.tick。
`pdf-reader/uno/python/edit_mode.py:32` 只观察页面编号；同一页停留 0.35 秒后，
就调用 edit_current_page。按住鼠标、停留拖动目标以及等待异步拖放收尾都能满足
这个时间条件。调度器没有接收“鼠标已释放且拖放事务收尾完成”的原生事实。

`reader_session.py::_edit_direct_page` 锁撤销、锁控制器、抑制修改通知，然后同步
调用 PDFPageImportFilter。该入口的 SolarMutexGuard 保护整页转换和对象构建，
解决反复绘制问题；但它是执行互斥，不是“当前没有交互事务”的许可检查。
TransferableHelper 已在等待系统拖放时释放过锁，另一个线程取得锁本身不证明安全。
构建完成后还有 Python 导航检查、提交/回滚及解锁，构成更多可交错的 UNO 调用。

普通 PDF 打开先构建模型再创建可交互视图，不会在缩略图按钮按下与拖放结束之间
自动把数千个对象插入活动页。应修正新增适配的调度边界，不能据此重写原生拖放。

用户日志第 451 页转换约 2.58 秒，随后导航取消、回滚约 15.89 秒，说明现有
“导入后检查导航”确实会被触发。它不是手势完成检查，也不能直接证明该次取消
导致了加号鼠标。回滚慢是另一个性能问题，不应混为根因。

## 排除不充分的修复

- lockControllers 是 SdrModel 布尔模型锁，非缩略图手势锁。
- XModifiable2 抑制修改广播，不清除拖放上下文。
- Application::IsUICaptured 只检查捕获、tracking、浮窗和系统对话框；未检查
  SdModule::pTransferDrag 或 SlideSorter 的待处理拖放结束事件。
- GetPointerState 仅描述当前物理按钮；系统拖放和异步收尾不能等价为按钮状态。
- 延长固定 0.35 秒仍不能证明手势结束。
- 自动发 Esc 或每次切换选择工具会取消用户操作，只遮住状态冲突。
- 移除整页 SolarMutexGuard 会重新引入已经测得的反复绘制退化。

## 还需要验证的最小运行证据

下一步应在新增入口与原生选择/拖放边界做一次可关闭的时序诊断，记录单调时间、
线程、页身份、mouse-down/up、拖放开始/结束、ProcessDragFinished、导入开始/结束
及提交/取消。具体需要区分：

1. 导入是否在按住按钮或 QDrag::exec 期间开始；
2. 是否插在 ObjectReleased 与 ProcessDragFinished 之间；
3. 鼠标释放与 dragDropEnd 是未送达、被延迟，还是已完成后状态又被重入改写；
4. 导航回滚时是否还有旧页 transferable 或待处理完成回调。

已有普通 CPU perf 不能给出这些事件的顺序，无需用重复热点采样替代时序验证。
确认后，在新增适配侧使用完整的原生交互状态/收尾边界延后转换；不更改原生
复制、移动、鼠标释放、Esc 或 Qt 拖放语义，也不让 xmloff 直接依赖 sd 私有状态。

## 已实施的验证版

后续实现复用 Draw 的活动拖放对象、缩略图选择模式和待处理拖放结束事件。
Draw 控制器通过只读 PdfReaderEditBusyReason 暴露状态，原生点击、拖放和 Esc
流程保持原样，只加了诊断点。Python 在交互结束、页码重新稳定后才请求转换；
PDFPageImportFilter 在 SolarMutex 内复查交互状态和目标页，防止检查后切页。
暂缓不会进入失败列表，也不会执行对象回滚或发布修改状态。同步转换开始后，
仍可能在提交前被导航取消，保留原有回滚逻辑。

在龙芯图形桌面，重新编译后执行：

```bash
make PARALLELISM=4 build &&
make PARALLELISM=4 CppunitTest_xmloff_draw

export PYTHONPATH="$PWD/instdir/program"
export LD_LIBRARY_PATH="$PWD/instdir/program"
export URE_BOOTSTRAP="vnd.sun.star.pathname:$PWD/instdir/program/fundamentalrc"

python3 pdf-reader/tools/probe_direct_edit.py \
  --office "$PWD/instdir/program/soffice" --ui-text-undo \
  --report /tmp/pdf-direct-gesture-probe.json

stamp=$(date +%Y%m%d-%H%M%S)
python3 pdf-reader/tools/read_edit.py \
  --office "$PWD/instdir/program/soffice" \
  --pdf "/home/loongson/projects/loongoffice/github/pdf-fast-reader/large-test-500p-50mb.pdf" \
  --trace-gestures "/tmp/pdf-native-gesture-$stamp.log" \
  --report "/tmp/pdf-gesture-$stamp.json" 2>&1 | tee "/tmp/pdf-python-gesture-$stamp.log"
```

探针会打开测试文档，请按终端提示进入指定文字框、键入、退出文字框并撤销重做；
每一步核对文字内容，成功才会标记 ui_text_typing_undo_verified。省略该选项则
可无人值守运行，其输出不声明真实键入通过。两种方式都检查请求的页已过期时
原生入口延后，以及导入后提交前切页时保留原预览、锁恢复、撤销历史保留和重试。

500 页文件进入编辑后，点击未转换页的缩略图，按住超过 0.35 秒再释放；
再试短距离拖动并按 Esc 取消，不要刻意投放以改变页序。随后快速切换多个页面，
最后停在一页，确认只转换停住的页，之前编辑的内容仍在。两个日志使用 unix_us
对齐；原生日志包含鼠标按下、释放、拖放开始、结束投递及回调处理完成，Python
日志包含忙碌状态、原生暂缓和导入时序。若再复现，保留同一次运行的两个日志。
本机仅能运行 Python 回归；C++ 构建和上述桌面操作仍待目标机验证。

## 真实文字探针的初始化修正

目标机 UI 探针在输入内容核对阶段失败，尚未到达撤销验证。该探针此前没有执行
正式阅读器 enable_editing 中的原生 SelectObject 初始化，而且可见文档之后又
加载过隐藏对照文档。现在在提示键入前激活测试 Frame、将焦点返回 ComponentWindow，
设置我们自己的未锁定编辑图层、调用原生 SelectObject 并选中待测文字对象。
仅在测试准备时执行一次，不每次轮询抢焦点、不替用户发 Esc，不改产品拖放代码。

失败诊断增加 actual_text、expected_text、view、controllers_locked 和 undo_locked。
这次只修正探针准备和诊断，不宣称产品加号鼠标根因已经确定或真实键入验证通过。
本地 Python 66 项测试中 65 通过、1 项跳过，目标机需要重跑 UI 探针。

## 已转换页交还原生视图控制

目标机最新探针显示文档非只读、编辑图层未锁、控制器和撤销管理器均未锁，
但文字仍未改变。因此真实文字输入及撤销重做尚未通过，不能把它解释成撤销失败，
也不能据此确定偶发禁止光标的来源。

ReaderSession.current_page 现在对已转换页只检查页身份并返回页号，不再准备预览、
修正图层可见/锁定状态或设置 ActiveLayer；反复轮询及离页再返回均保持用户的
Draw 图层选择。未转换页仍需准备受保护的 PDF 预览，自动转换仍等待原生手势结束。
不增加鼠标样式覆盖、自动 Esc 或每次切页 SelectObject。

本次仅修改 Python，不需要重编译 C++。本地 67 项 Python 回归中 66 通过、1 项跳过，
新增回归覆盖已转换页轮询和重访不写入视图状态。龙芯桌面的偶发光标及真实键入
仍需验证；若复现，应保留同次运行的原生手势与 Python 日志，不宣称已修复。

## 500 页交互复现后的检查补充

真实文字输入、撤销重做探针已由目标机验证通过，但 500 页会话仍报告禁止光标及
点击画布复制页面。该次日志没有启用 --trace-gestures，只能确认导入完成，不能
确认拖放收尾或工具状态。减少已转换页图层写入尚未解决此问题。

源码确认 IsInGesture 先前只检查非 NormalMode，而按下到越过拖动阈值之间仍是
NormalMode。既有按键兜底查询键盘焦点窗口，焦点属于另一个 Frame 时不等价于
缩略图输入窗口。因此补充查询缩略图自身窗口的 GetPointerState；复用原生状态，
不改变选择或拖放流程。不能直接把 NormalModeHandler 的 maButtonDownLocation
当作“仍按着鼠标”，它在普通释放后可以保留。

Python 先前只在视图身份改变时检查忙碌，现在未转换页每次准备前均检查，防止
同页手势中修正图层。已转换页仍直接返回，不恢复图层或工具。

画布 MouseButtonDown/Up 增加环境变量控制的只读诊断：
canvas.mouse-down.state / canvas.mouse-up.state 的 detail 为位掩码：
transferable=1、Draw view action=2、create object=4、input locked=8、
固定三页 hybrid 测试钩子存在=16；对应 *.tool 记录原生 FuPoor::GetSlotID。
结合现有 thumbnail.* 事件、Python edit.input 与导入时间，区分页面拖放残留、
创建工具和输入锁。诊断不更改鼠标、焦点、Esc、投放或复制语义。

本地 71 项 Python 测试中 70 通过、1 跳过，新增覆盖同页手势不修改图层、
新页等待释放后才准备预览。Windows 工作区没有配置可用的 LibreOffice C++ 构建，
原生编译、CppunitTest_xmloff_draw 和真实交互待目标机运行。此改动修补已知检查
缺口并完善证据，不代表已证明此次复制页面的完整根因。
