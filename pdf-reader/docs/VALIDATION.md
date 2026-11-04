# 初始化验证记录

日期：2026-11-04。

## 工作区基线

`feat/pdf-reader-uno-26.2.6` 的 HEAD 与本地 `loongoffice/26.2.6` 相同：
`af1911d32d3775e6a8becb3b0a9b7b1b52d9c894`。

新增文件全部位于 `pdf-reader/`。未修改或复用旧快读分支的 Office 核心实现。

## 本地验证

- WSL Debian、GNU C++ 14.2.0：独立 CMake 配置成功。
- `session.hxx` 及依赖头文件以 C++17、`-Wall -Wextra -Werror -pedantic` 编译语法检查通过。
- 编辑适配层的 5 个单元测试覆盖：过滤器缺失、配置不匹配、导入失败/异常资源释放、成功后文档所有权转交。
- 真实 UNO 全链探测运行环境：**LibreOffice 25.2.3.2 520(Build:2)**，WSL Debian，非目标 LoongOffice/loong64。

真实探测结果：

```json
{
  "status": "passed",
  "mode": "FilterFactory-direct",
  "office": "LibreOffice 25.2.3.2 520(Build:2)",
  "pages": 1,
  "shapes": [
    {"type": "com.sun.star.drawing.PolyLineShape", "text": ""},
    {"type": "com.sun.star.drawing.TextShape", "text": "UNO_PDF_EDIT_PROBE"}
  ],
  "pdfium_environment_forced": true,
  "fixture_editable_text_verified": true
}
```

这证明独立 UNO 调用路径在本地已有 Office 上实际生成了文本/矢量对象，不只是打开位图。它不代替目标 LoongOffice 26.2.6 安装包的运行验证，也不表示阅读器、调度器或 OXT 已完成。

## 复查命令

```bash
python3 -m unittest discover -s pdf-reader/tests -v
cmake -S pdf-reader -B /tmp/pdf-reader-build
printf '#include <pdf_reader/session.hxx>\n' | \
  g++ -std=c++17 -Wall -Wextra -Werror -pedantic \
      -Ipdf-reader/core/include -x c++ -fsyntax-only -
timeout 120s /usr/bin/python3 pdf-reader/tools/probe_edit.py --office /usr/bin/loongoffice
```

最后一项在目标机器执行；如 UNO 模块不在默认搜索路径，请按 README 在子 shell 配置对应产品的环境。
