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

TMPDIR_FOR_VERIFY="$TMPDIR/.vunzip"
# /dev/tmp/.vunzip
mkdir "$TMPDIR_FOR_VERIFY"

abort_verify() {
  ui_print "*********************************************************"
  ui_print "! $1"
  ui_print "! This zip may be corrupted, please try downloading again"
  abort    "*********************************************************"
}

# extract <zip> <file> <target dir> <junk paths>
extract() {
  zip=$1
  file=$2
  dir=$3
  junk_paths=$4
  [ -z "$junk_paths" ] && junk_paths=false
  opts="-o"
  [ $junk_paths = true ] && opts="-oj"

  file_path=""
  hash_path=""
  if [ $junk_paths = true ]; then
    file_path="$dir/$(basename "$file")"
    hash_path="$TMPDIR_FOR_VERIFY/$(basename "$file").sha256"
  else
    file_path="$dir/$file"
    hash_path="$TMPDIR_FOR_VERIFY/$file.sha256"
  fi

  unzip $opts "$zip" "$file" -d "$dir" >&2
  [ -f "$file_path" ] || abort_verify "$file not exists"

  unzip $opts "$zip" "$file.sha256" -d "$TMPDIR_FOR_VERIFY" >&2
  [ -f "$hash_path" ] || abort_verify "$file.sha256 not exists"

  (echo "$(cat "$hash_path")  $file_path" | sha256sum -c -s -) || abort_verify "Failed to verify $file"
  ui_print "- Verified $file" >&1
}

file="META-INF/com/google/android/update-binary"
file_path="$TMPDIR_FOR_VERIFY/$file"
hash_path="$file_path.sha256"
unzip -o "$ZIPFILE" "META-INF/com/google/android/*" -d "$TMPDIR_FOR_VERIFY" >&2
[ -f "$file_path" ] || abort_verify "$file not exists"
if [ -f "$hash_path" ]; then
  # -c 选项表示将哈希值与文件中包含的哈希值进行比较，-s 选项表示不输出任何错误消息，- 表示从标准输入中读取哈希值,也就是从管道符中读取传递过来的哈希值和文件路径
  # 一般比较文件 hash 的命令格式如下: echo "sha256哈希值 文件名" | sha256sum -c
  (echo "$(cat "$hash_path")  $file_path" | sha256sum -c -s -) || abort_verify "Failed to verify $file"
  ui_print "- Verified $file" >&1
else
  ui_print "- Download from Magisk app"
fi


# >&1: 将输出重定向到标准输出; 将会在终端显示
# >&2: 将输出重定向到标准错误输出; 也会在终端显示