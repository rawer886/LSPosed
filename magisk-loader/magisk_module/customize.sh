#
# This file is part of LSPosed.
#
# LSPosed is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# LSPosed is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with LSPosed.  If not, see <https://www.gnu.org/licenses/>.
#
# Copyright (C) 2020 EdXposed Contributors
# Copyright (C) 2021 LSPosed Contributors
#

# shellcheck disable=SC2034

# 官方文档: https://topjohnwu.github.io/Magisk/guides.html

#SKIPUNZIP = 1 跳过默认安装步骤(Magisk 的配置)；使用这个脚本的内容自行安装所有内容
SKIPUNZIP=1

FLAVOR=@FLAVOR@ # 打包脚本中会替换成对应的值

enforce_install_from_magisk_app() {
  # BOOTMODE: magisk 定义的字段; true 表示从 Magisk Manager 安装 - true if the module is being installed in the Magisk app
  if $BOOTMODE; then
    ui_print "- Installing from Magisk app"
  else
    ui_print "*********************************************************"
    ui_print "! Install from recovery is NOT supported"
    ui_print "! Some recovery has broken implementations, install with such recovery will finally cause Riru or Riru modules not working"
    ui_print "! Please install from Magisk app"
    abort "*********************************************************"
  fi
}

VERSION=$(grep_prop version "${TMPDIR}/module.prop")
ui_print "- LSPosed version ${VERSION}"

# Extract verify.sh
ui_print "- Extracting verify.sh"

# unzip -o /data/local/tmp/LSPosed-v1.8.6-6712-zygisk-debug.zip verify.sh -d /dev/tmp
# >&2: 将输出重定向到标准错误
unzip -o "$ZIPFILE" 'verify.sh' -d "$TMPDIR" >&2
if [ ! -f "$TMPDIR/verify.sh" ]; then
  ui_print "*********************************************************"
  ui_print "! Unable to extract verify.sh!"
  ui_print "! This zip may be corrupted, please try downloading again"
  abort    "*********************************************************"
fi
# 执行 verify.sh
. "$TMPDIR/verify.sh"

# Base check 提取并检查如下文件是否完整
extract "$ZIPFILE" 'customize.sh' "$TMPDIR"
extract "$ZIPFILE" 'verify.sh' "$TMPDIR"
extract "$ZIPFILE" 'util_functions.sh' "$TMPDIR"
. "$TMPDIR/util_functions.sh"
check_android_version
check_magisk_version
check_incompatible_module

if [ "$FLAVOR" == "riru" ]; then
  # Extract riru.sh
  extract "$ZIPFILE" 'riru.sh' "$TMPDIR"
  . "$TMPDIR/riru.sh"
  # Functions from riru.sh
  check_riru_version
fi

enforce_install_from_magisk_app

# Check architecture
# $ARCH: magisk 定义的字段; 当前设备的架构
if [ "$ARCH" != "arm" ] && [ "$ARCH" != "arm64" ] && [ "$ARCH" != "x86" ] && [ "$ARCH" != "x64" ]; then
  abort "! Unsupported platform: $ARCH"
else
  ui_print "- Device platform: $ARCH"
fi

# Extract libs
ui_print "- Extracting module files -> $MODPATH"

#$MODPATH: /data/adb/modules_update/zygisk_lsposed
extract "$ZIPFILE" 'module.prop'        "$MODPATH"
extract "$ZIPFILE" 'post-fs-data.sh'    "$MODPATH"
extract "$ZIPFILE" 'service.sh'         "$MODPATH"
extract "$ZIPFILE" 'uninstall.sh'       "$MODPATH"
extract "$ZIPFILE" 'sepolicy.rule'      "$MODPATH"
extract "$ZIPFILE" 'framework/lspd.dex' "$MODPATH"
extract "$ZIPFILE" 'daemon.apk'         "$MODPATH"
extract "$ZIPFILE" 'daemon'             "$MODPATH"
rm -f /data/adb/lspd/manager.apk
extract "$ZIPFILE" 'manager.apk'        "$MODPATH"

if [ "$FLAVOR" == "zygisk" ]; then
  mkdir -p "$MODPATH/zygisk"
  # 提取 arm 和 arm64 的 so 文件
  if [ "$ARCH" = "arm" ] || [ "$ARCH" = "arm64" ]; then
    extract "$ZIPFILE" "lib/armeabi-v7a/liblspd.so" "$MODPATH/zygisk" true
    mv "$MODPATH/zygisk/liblspd.so" "$MODPATH/zygisk/armeabi-v7a.so"

    if [ "$IS64BIT" = true ]; then
      extract "$ZIPFILE" "lib/arm64-v8a/liblspd.so" "$MODPATH/zygisk" true
      mv "$MODPATH/zygisk/liblspd.so" "$MODPATH/zygisk/arm64-v8a.so"
    fi
  fi

  # 提取 x86 和 x64 的 so 文件
  if [ "$ARCH" = "x86" ] || [ "$ARCH" = "x64" ]; then
    extract "$ZIPFILE" "lib/x86/liblspd.so" "$MODPATH/zygisk" true
    mv "$MODPATH/zygisk/liblspd.so" "$MODPATH/zygisk/x86.so"

    if [ "$IS64BIT" = true ]; then
      extract "$ZIPFILE" "lib/x86_64/liblspd.so" "$MODPATH/zygisk" true
      mv "$MODPATH/zygisk/liblspd.so" "$MODPATH/zygisk/x86_64.so"
    fi
  fi
elif [ "$FLAVOR" == "riru" ]; then
  mkdir "$MODPATH/riru"
  mkdir "$MODPATH/riru/lib"
  mkdir "$MODPATH/riru/lib64"
  if [ "$ARCH" = "arm" ] || [ "$ARCH" = "arm64" ]; then
    ui_print "- Extracting arm libraries"
    extract "$ZIPFILE" "lib/armeabi-v7a/lib$RIRU_MODULE_LIB_NAME.so" "$MODPATH/riru/lib" true

    if [ "$IS64BIT" = true ]; then
      ui_print "- Extracting arm64 libraries"
      extract "$ZIPFILE" "lib/arm64-v8a/lib$RIRU_MODULE_LIB_NAME.so" "$MODPATH/riru/lib64" true
    fi
  fi

  if [ "$ARCH" = "x86" ] || [ "$ARCH" = "x64" ]; then
    ui_print "- Extracting x86 libraries"
    extract "$ZIPFILE" "lib/x86/lib$RIRU_MODULE_LIB_NAME.so" "$MODPATH/riru/lib" true

    if [ "$IS64BIT" = true ]; then
      ui_print "- Extracting x64 libraries"
      extract "$ZIPFILE" "lib/x86_64/lib$RIRU_MODULE_LIB_NAME.so" "$MODPATH/riru/lib64" true
    fi
  fi

  if [ "$RIRU_MODULE_DEBUG" = true ]; then
    mv "$MODPATH/riru" "$MODPATH/system"
    mv "$MODPATH/system/lib/lib$RIRU_MODULE_LIB_NAME.so" "$MODPATH/system/lib/libriru_$RIRU_MODULE_LIB_NAME.so"
    mv "$MODPATH/system/lib64/lib$RIRU_MODULE_LIB_NAME.so" "$MODPATH/system/lib64/libriru_$RIRU_MODULE_LIB_NAME.so"
    if [ "$RIRU_API" -ge 26 ]; then
      mkdir -p "$MODPATH/riru/lib"
      mkdir -p "$MODPATH/riru/lib64"
      touch "$MODPATH/riru/lib/libriru_$RIRU_MODULE_LIB_NAME"
      touch "$MODPATH/riru/lib64/libriru_$RIRU_MODULE_LIB_NAME"
    else
      mkdir -p "/data/adb/riru/modules/$RIRU_MODULE_LIB_NAME"
    fi
  fi
fi

if [ "$API" -ge 29 ]; then # -ge: 大于等于
  ui_print "- Extracting dex2oat binaries"
  mkdir "$MODPATH/bin"

  # 提取 dex2oat32 和 dex2oat64
  if [ "$ARCH" = "arm" ] || [ "$ARCH" = "arm64" ]; then
    extract "$ZIPFILE" "bin/armeabi-v7a/dex2oat" "$MODPATH/bin" true
    mv "$MODPATH/bin/dex2oat" "$MODPATH/bin/dex2oat32"

    if [ "$IS64BIT" = true ]; then
      extract "$ZIPFILE" "bin/arm64-v8a/dex2oat" "$MODPATH/bin" true
      mv "$MODPATH/bin/dex2oat" "$MODPATH/bin/dex2oat64"
    fi
  elif [ "$ARCH" == "x86" ] || [ "$ARCH" == "x64" ]; then
    extract "$ZIPFILE" "bin/x86/dex2oat" "$MODPATH/bin" true
    mv "$MODPATH/bin/dex2oat" "$MODPATH/bin/dex2oat32"

    if [ "$IS64BIT" = true ]; then
      extract "$ZIPFILE" "bin/x86_64/dex2oat" "$MODPATH/bin" true
      mv "$MODPATH/bin/dex2oat" "$MODPATH/bin/dex2oat64"
    fi
  fi

  ui_print "- Patching binaries"
  # 生成随机的 32 位字符串(a-f0-9)
  # -dc: 删除所有非指定字符; -d 删除；-c 取‘a-f0-0’的补集；和在一起就是前面的意思
  # head -c 16: 取前 16 个字符; 表示截断,不然会一直生成
  DEV_PATH=$(tr -dc 'a-z0-9' < /dev/urandom | head -c 32)
  # 替换文件中的 5291374ceda0aef7c5d86cd2a4f6a3ac 字符串为随机值; sed -i: 表示直接在源文件上替换字符串
  # 格式 sed -i "s/xxx/yyy/g" : 将文件中的 xxx 替换为 yyy; 其中 g 表示全局替换; . 表示任意字符; 这里表示 16 个任意字符替换
  # 这里的意思是: 将 5291374ceda0aef7c5d86cd2a4f6a3ac 替换为 placeholder_/dev/$DEV_PATH
  sed -i "s/5291374ceda0aef7c5d86cd2a4f6a3ac/$DEV_PATH/g" "$MODPATH/daemon.apk"
  sed -i "s/5291374ceda0aef7c5d86cd2a4f6a3ac/$DEV_PATH/" "$MODPATH/bin/dex2oat32"
  sed -i "s/5291374ceda0aef7c5d86cd2a4f6a3ac/$DEV_PATH/" "$MODPATH/bin/dex2oat64"
else
  extract "$ZIPFILE" 'system.prop' "$MODPATH"
fi

# 设置权限和上下文
# set_perm_recursive <directory> <owner> <group> <dirpermission> <filepermission> [context]
# If [context] is not specified, the default is "u:object_r:system_file:s0"
set_perm_recursive "$MODPATH" 0 0 0755 0644
# 2000: shell 用户; 3000: system 用户
set_perm_recursive "$MODPATH/bin" 0 2000 0755 0755 u:object_r:magisk_file:s0
chmod 0744 "$MODPATH/daemon"

# ro.maple.enable 是用于控制 Android ART（Android Runtime）中新一代的垃圾回收器（Garbage Collector）Maple的开启状态。
# 值为 1 时开启，值为 0 时关闭。默认值为 0
if [ "$(grep_prop ro.maple.enable)" == "1" ] && [ "$FLAVOR" == "zygisk" ]; then
  ui_print "- Add ro.maple.enable=0"
  echo "ro.maple.enable=0" >> "$MODPATH/system.prop"
fi

ui_print "- Welcome to LSPosed!"
