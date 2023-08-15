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
# Copyright (C) 2021 LSPosed Contributors
#

# 程序的入口，这里是一个 shell 脚本, 通过 magisk 来启动

# %/* 表示删除最后一个 / 及其右边的内容. 这里的位置是: /data/adb/modules/zygisk_lsposed
MODDIR=${0%/*}

rm -f "/data/local/tmp/daemon.apk"
rm -f "/data/local/tmp/manager.apk"
cd "$MODDIR"

# 通过 magisk 来启动 daemon 在后台运行.
# 启动时创建一个新的挂载名称空间，并且设置其为slave传播属性，即子进程对文件系统的修改不会反向影响到父进程，但父进程对文件系统的修改会影响到子进程。


# 具体参数含义:
#   unshare: 用于创建一个新的命名空间;
#   --propagation: 用于设置新创建挂载点的传播属性。slave 模式表示挂载事件只能从 original（其他主要命名空间）传播到 slave，但不能反向，也就是从 slave 传播到 original。
#                  也就是说，内部对文件系统的修改（如新的挂载点）不会影响到原有的名字空间，但原有名字空间对文件系统的修改会影响到新的名字空间。
#   -m 或--mount: 是新建一个新的挂载点的名称空间，该参数使子进程拥有自己独立的挂载点的名称空间，该子进程的挂载和卸载不会影响其他进程。
#   sh -c: 在新的命名空间中执行命令
#   $@: 传递给 daemon 的参数 (这里是 "&" 表示在后台运行)
unshare --propagation slave -m sh -c "$MODDIR/daemon $@&"
