# LoongOffice

本分支基于 **LibreOffice 26.2.6**（源码基线为 `libreoffice-26.2.6.3`），添加了 **多格式批量打印**，以及 **OFD 导入阅读、编辑和导出** 功能。

Loongnix 版保留全部上述功能；麒麟 V11 版使用 Qt6 后端，暂不提供批量打印，保留普通打印及 OFD 功能。

## 功能与实现

- **批量打印**：使用 Python + PySide6 构建界面，通过 UNO 调用 LoongOffice 文档处理和打印能力，结合 PyMuPDF 处理 PDF 预览与页面排版，并对接系统 CUPS 打印服务。支持各类文档格式的批量打印，包括 Word、Excel、PowerPoint、PDF、OFD 和图片等。可集中添加文件，预览内容，设置页码范围、份数、单双面和纸张参数后批量打印。通过 Office 菜单和文件管理器右键菜单提供入口。
- **OFD 导入、编辑与导出**：以 Java UNO 扩展集成到 LoongOffice，基于 OFDRW 解析、渲染和转换 OFD。支持导入 OFD 阅读、打印，转入 Draw 编辑，并将 Writer、Calc、Impress、Draw、Math 等模块中的文档导出为 OFD。需要 Java 17 运行环境。

## 准备 CI 包

在联网的目标架构主机上安装对应构建依赖，从已提交、无未提交改动的源码目录执行。

### Loongnix

```sh
debian/scripts/prepare-orig-tarball
```

将生成的 `../loongoffice_26.2.6.3.orig.tar.xz` 和仓库中的 `debian/` 一起提交到 CI。

### 麒麟 V11

```sh
debian/scripts/prepare-orig-tarball --kylin
```

将以下两份匹配的产物一起提交到 CI：

- `../loongoffice_26.2.6.3+kylin.orig.tar.xz`
- `../loongoffice_26.2.6.3+kylin.packaging/debian/`

麒麟版的打包配置已固定为 Qt6-only，CI 无需额外参数或探测脚本。

## 自己编译

以下命令生成 Debian 安装包，适用于 loong64 目标主机。先安装 `debian/control` 中对应配置的构建依赖，需要 GCC 12 或更新版本、GNU Make 4.2 或更新版本及 JDK 17。

在联网的源码目录中执行：

```sh
# Loongnix 完整版；麒麟版将此行改为 export DEB_BUILD_PROFILES=pkg.loongoffice.qt6-only
export DEB_BUILD_PROFILES=

debian/scripts/fetch-vendor-sources
debian/scripts/prepare-libreoffice-tarballs
dpkg-buildpackage -b -us -uc
```

生成的 `.deb` 位于源码目录的上一级。若使用已经准备好的 CI 源码包及其匹配的 `debian/`，解压组装后直接执行 `dpkg-buildpackage -b -us -uc` 即可。

详细说明见 [Debian 构建文档](debian/README.source) 和 [26.2.6 分支说明](README.26.2.6.md)。
