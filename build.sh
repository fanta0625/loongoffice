#!/bin/sh
set -e
if [ -e /etc/debian_version ]; then
    PACKAGEFORMAT=deb
elif [ -e /etc/loongnix_version ]; then
    PACKAGEFORMAT=deb
elif [ -e /etc/redhat-release ]; then
    PACKAGEFORMAT=rpm
fi

./autogen.sh --with-help --disable-firebird-sdbc --with-system-postgresql --with-lang="zh-CN" --disable-librelogo --with-package-format=${PACKAGEFORMAT} --enable-epm --enable-release-build --enable-odk --with-vendor=Loongson --enable-qt6-multimedia --enable-qt6 --enable-kf6 --enable-gstreamer-1-0 --enable-ext-ofdreader --enable-ext-batchprint && make

if [ -d ./out ]; then
    rm -rf ./out
fi

mkdir out

if [ "$PACKAGEFORMAT" = rpm ]; then
    cp ./workdir/installation/LoongOffice_languagepack/rpm/install/LoongOffice_25.8.7.0.0_Linux_rpm_langpack_zh-CN/RPMS/loongofficebasis-zh-CN-help-25.8.7.0.0-1.loongarch64.rpm \
       ./workdir/installation/LoongOffice_languagepack/rpm/install/LoongOffice_25.8.7.0.0_Linux_rpm_langpack_zh-CN/RPMS/loongoffice-zh-CN-25.8.7.0.0-1.loongarch64.rpm \
       ./workdir/installation/LoongOffice_languagepack/rpm/install/LoongOffice_25.8.7.0.0_Linux_rpm_langpack_zh-CN/RPMS/loongofficebasis-zh-CN-25.8.7.0.0-1.loongarch64.rpm \
       ./workdir/installation/LoongOffice/rpm/install/LoongOffice_25.8.7.0.0_Linux_rpm/RPMS/loongoffice-25.8.7.0.0-1.loongarch64.rpm \
       ./workdir/installation/LoongOffice/rpm/install/LoongOffice_25.8.7.0.0_Linux_rpm/RPMS/loongoffice-freedesktop-menus-25.8.7-0.noarch.rpm \
       ./out
    exit 0
fi

cd out

MACHINE=`uname -m`
if [ "$MACHINE" = mips64 ]; then
    MACHINE=mips64el
fi

cp ../workdir/installation/LoongOffice/deb/install/LoongOffice_25.8.7.0.0_Linux_deb/DEBS/loongoffice_25.8.7.0.0-1_${MACHINE}.deb \
   ../workdir/installation/LoongOffice/deb/install/LoongOffice_25.8.7.0.0_Linux_deb/DEBS/loongoffice-debian-menus_25.8.7-0_all.deb \
   ../workdir/installation/LoongOffice/deb/install/LoongOffice_25.8.7.0.0_Linux_deb/DEBS/loongoffice-extension-pdf-import_25.8.7.0.0-1_${MACHINE}.deb \
   ../workdir/installation/LoongOffice/deb/install/LoongOffice_25.8.7.0.0_Linux_deb/DEBS/loongoffice-xsltfilter_25.8.7.0.0-1_${MACHINE}.deb \
   ../workdir/installation/LoongOffice/deb/install/LoongOffice_25.8.7.0.0_Linux_deb/DEBS/loongofficebasis-kde-integration_25.8.7.0.0-1_${MACHINE}.deb \
   ../workdir/installation/LoongOffice/deb/install/LoongOffice_25.8.7.0.0_Linux_deb/DEBS/loongoffice-pyuno_25.8.7.0.0-1_${MACHINE}.deb \
   ../workdir/installation/LoongOffice/deb/install/LoongOffice_25.8.7.0.0_Linux_deb/DEBS/loongoffice-graphicfilter_25.8.7.0.0-1_${MACHINE}.deb \
   ../workdir/installation/LoongOffice/deb/install/LoongOffice_25.8.7.0.0_Linux_deb/DEBS/loongoffice-ogltrans_25.8.7.0.0-1_${MACHINE}.deb \
   ../workdir/installation/LoongOffice_languagepack/deb/install/LoongOffice_25.8.7.0.0_Linux_deb_langpack_zh-CN/DEBS/loongofficebasis-zh-cn_25.8.7.0.0-1_${MACHINE}.deb \
   ../workdir/installation/LoongOffice_languagepack/deb/install/LoongOffice_25.8.7.0.0_Linux_deb_langpack_zh-CN/DEBS/loongofficebasis-zh-cn-help_25.8.7.0.0-1_${MACHINE}.deb \
   ../workdir/installation/LoongOffice_languagepack/deb/install/LoongOffice_25.8.7.0.0_Linux_deb_langpack_zh-CN/DEBS/loongoffice-zh-cn_25.8.7.0.0-1_${MACHINE}.deb \
   ../workdir/installation/LoongOffice_SDK/deb/install/LoongOffice_25.8.7.0.0_Linux_deb_sdk/DEBS/loongoffice-sdk_25.8.7.0.0-1_${MACHINE}.deb .

if [ -d ./extract ]; then
    rm -rf ./extract
fi

if [ -d ./extract-sdk ]; then
    rm -rf ./extract-sdk
fi

mkdir -p ./extract/DEBIAN
mkdir -p ./extract-sdk/DEBIAN

dpkg-deb -x ./loongoffice-zh-cn_25.8.7.0.0-1_${MACHINE}.deb extract
dpkg-deb -x ./loongofficebasis-zh-cn-help_25.8.7.0.0-1_${MACHINE}.deb extract
dpkg-deb -x ./loongofficebasis-zh-cn_25.8.7.0.0-1_${MACHINE}.deb extract
dpkg-deb -x ./loongoffice-debian-menus_25.8.7-0_all.deb extract
dpkg-deb -x ./loongoffice_25.8.7.0.0-1_${MACHINE}.deb extract
dpkg-deb -x ./loongoffice-extension-pdf-import_25.8.7.0.0-1_${MACHINE}.deb extract
dpkg-deb -x ./loongoffice-xsltfilter_25.8.7.0.0-1_${MACHINE}.deb extract
dpkg-deb -x ./loongofficebasis-kde-integration_25.8.7.0.0-1_${MACHINE}.deb extract
dpkg-deb -x ./loongoffice-pyuno_25.8.7.0.0-1_${MACHINE}.deb extract
dpkg-deb -x ./loongoffice-graphicfilter_25.8.7.0.0-1_${MACHINE}.deb extract
#dpkg-deb -x ./loongoffice-ogltrans_25.8.7.0.0-1_${MACHINE}.deb extract

dpkg-deb -x ../external/tarballs/loongbatchprint_1.0.0-1_loong64.deb extract

dpkg-deb -e ./loongoffice-debian-menus_25.8.7-0_all.deb extract/DEBIAN

# 提取 loongbatchprint 的依赖（排除 loongoffice 自身）
BATCHER_DEPENDS=$(dpkg-deb -f ../external/tarballs/loongbatchprint_1.0.0-1_loong64.deb Depends 2>/dev/null | sed 's/loongoffice, //' | sed 's/loongoffice$//' | sed 's/, *$//')

dpkg-deb -e ./loongoffice_25.8.7.0.0-1_${MACHINE}.deb extract/DEBIAN

dpkg-deb -x ./loongoffice-sdk_25.8.7.0.0-1_${MACHINE}.deb extract-sdk
dpkg-deb -e ./loongoffice-sdk_25.8.7.0.0-1_${MACHINE}.deb extract-sdk/DEBIAN

sed -i "s/25.8.7.0.0-1$/25.8.7.0.0-1.lnd.1.0.0001/g" extract/DEBIAN/control
sed -i "s/.*Installed-Size.*/Installed-Size: `du -sk extract | awk '{print $1}'`/g" extract/DEBIAN/control
sed -i "s/Architecture: loongarch64/Architecture: loong64/g" extract/DEBIAN/control

# 删除 loongbatchprint 已有的 Section 和 Priority（后续会重新添加）
sed -i "/^Section:/d" extract/DEBIAN/control
sed -i "/^Priority:/d" extract/DEBIAN/control

echo "Replaces: loongoffice-zh-cn,loongofficebasis-zh-cn-help,loongofficebasis-zh-cn,loongoffice-debian-menus" >> extract/DEBIAN/control
echo "Conflicts: loongoffice-zh-cn,loongofficebasis-zh-cn-help,loongofficebasis-zh-cn,loongoffice-debian-menus" >> extract/DEBIAN/control


# 追加 loongbatchprint 的依赖（排除 loongoffice）
if [ -n "$BATCHER_DEPENDS" ]; then
    echo "Depends: default-jre, ${BATCHER_DEPENDS}" >> extract/DEBIAN/control
else
    echo "Depends: default-jre" >> extract/DEBIAN/control
fi

echo "Section: editors" >> extract/DEBIAN/control
echo "Priority: optional" >> extract/DEBIAN/control

if [ ! -d ./build ]; then
    mkdir ./build
fi

dpkg-deb --build --root-owner-group extract build && cp -a build/*.deb .

cd ..

exit 0
