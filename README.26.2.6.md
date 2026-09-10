# LoongOffice 26.2.6 基线迁移

## 版本与来源

- 分支：`loongoffice/26.2.6`
- 上游标签：`libreoffice-26.2.6.3`
- 上游提交：`8221e31b3ac356a1623c672912a3d2b492f7e3d1`
- Debian 版本：`26.2.6.3-1.lnd.1`
- 定制来源：`debian/single-package-source-build`，提交
  `34883c770a7c`，原基线 `libreoffice-25.8.7.3`。

从原分支迁移了 26 个提交，保留原作者和 cherry-pick 来源记录。
原有中文排版、SmartArt 定位、演示计时、品牌化、中文模块名称、
OFD 扩展、批量打印和单包离线构建功能均保留。

## 迁移处理

- 演示计时使用上游 `canvastools::ElapsedTime` 命名空间，保留新增的总计时器。
- 在 26.2 菜单上移除社区入口，保留上游 `menu:style` 等界面更新。
- 保留 LoongOffice 品牌图片和独立的 `/opt/loongoffice` 安装、命令及用户配置。
- 保留 `481de855fee6` 的 libpng LSX 适配。仅对 LSX 滤镜实现增加 `-mlsx`，
  通用调度代码仍通过 libpng 的 `AT_HWCAP` 检测选择实现。
- 不迁移 `4dfe75d3112f` 的 EPM 架构命名补丁，并移除后续提交中重复的 MIPS
  EPM 设置。单包使用 debhelper，Debian 架构仍为 `loong64`。
- `build.sh` 改为执行 `dpkg-buildpackage -b -us -uc`，不再执行旧的 EPM
  构建、固定 25.8 路径和中间 DEB 合并流程。
- 保留 Loongnix 的 GCC 搜索路径和跳过 `dh_dwz` 的兼容措施。
- dictionaries、helpcontent2 和 translations 锁定到新上游 gitlink；批量打印
  提交和预编译 OFD OXT 不变。
- 中文演示计时补丁重新生成，仅添加 16 条词条，不覆盖上游译文或更新无关行号。
- 声明 GNU Make >= 4.2、GTK >= 3.24、ATK >= 2.36；继续使用系统 Python。
- 保留上游 Cargo 下载规则，但单包配置明确禁用实验性 YRS 和 Rust UNO。
  当前离线归档机制不负责导出 Cargo 缓存。

## 构建

在满足 `debian/control` 依赖的 Loongnix loong64 主机上，从此分支的干净
checkout 开始。编译器需要支持 C++20、GCC 12 或更新版本，以及 libpng
滤镜所需的 `-mlsx`。不要复用 25.8 的 workdir、instdir 或依赖清单。

联网准备主机执行：

```sh
debian/scripts/prepare-orig-tarball
```

该脚本获取锁定的子模块、批量打印源码和外部依赖，生成
`../loongoffice_26.2.6.3.orig.tar.xz`。旧的 vendor/libreoffice-tarballs
目录应先移走，以便生成与新版本匹配的依赖和校验清单。

准备完输入后，使用标准 Debian 源包流程，或在准备好的 checkout 中执行：

```sh
./build.sh
```

`debian/` 携带预编译 OFD OXT。生成和提交源包的详细约定见
[debian/README.source](debian/README.source)。

## 验证状态

迁移时已在 Windows / Debian WSL x86_64 环境完成：

- 修改涉及的 XML、UI、配置 schema 和 SVG 共 54 个文件的解析检查。
- Debian 构建及安装脚本的 Shell 语法检查。
- `aclocal` / `autoconf` 生成、生成脚本语法及配置选项检查。
- `dpkg-source --before-build` 实际应用翻译补丁，`--after-build` 撤销并
  比较原文件；`msgfmt --check` 校验译文。上游 PO 头部的默认项目名称产生警告。
- 预编译 OFD OXT 的摘要、ZIP 完整性和源码排除检查。
- 上游祖先关系、子模块锁定、版本一致性、冲突标记及差异空白检查。

本次未完成 LoongArch 原生编译、完整离线 orig 源包生成或运行回归。
发布前需在目标主机完成：

1. 准备新依赖，在禁网环境完成单包构建；确认 libpng 和 Skia 编译、链接成功。
2. 安装后执行 `debian/tests/installed-layout`，验证与系统 LibreOffice 共存。
3. 验证 OFD 打开和打印、批量打印及 PyUNO。
4. 回归中文断行和混排、SmartArt 定位，以及演示计时的三种模式和六个位置。
5. 在目标 CPU 范围内检查 PNG 加载和图形渲染，确认没有非法指令。

仓库提交钩子已执行；本机缺少所需的 clang-format 5.0.0，因此该格式检查
未能执行。源码迁移完成不代表上述目标平台验证已经通过。
