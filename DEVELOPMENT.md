# 单虚拟机 loopback RDMA 开发与验证指南

本文给出一套可重复执行的单虚拟机调测流程。主机运行 `rocm-ernic`
vfio-user 服务端，独立 Debian 13 VM 运行内核驱动、定制 rdma-core provider
和功能测试。

流程遵守以下边界：

- 主机与 VM 各自 `git clone` 仓库，不复制彼此的构建产物；
- 主机只构建服务端及主机侧测试，所有输出均放在 `/root/Codex`；
- VM 只构建内核模块、rdma-core provider 和 VM 侧测试；
- 不向主机或 `debian-13-dev` 安装本项目的中间产物；
- 使用 `virt-install` 创建和管理 VM，再使用 `virt-xml` 写入 vfio-user
  设备参数，不直接启动或停止 QEMU；
- 文中的命令不依赖仓库外临时脚本。

以下命令均以 root 身份执行。访问互联网前设置代理：

```bash
export http_proxy=http://192.168.100.1:3128
export https_proxy=http://192.168.100.1:3128
```

## 1. 变量和目录

主机使用：

```bash
export REPO_URL=https://github.com/zhangpengfei-xyz/rocm-ernic.git
export ERNIC_REV=main                 # 改为包含待验证修改的分支或提交
export HOST_REPO=/root/ByteDance/rocm-ernic
export HOST_WORK=/root/Codex/rocm-ernic-host
export HOST_DEPS="$HOST_WORK/deps"
export HOST_SYSROOT="$HOST_DEPS/sysroot"
export HOST_PREFIX="$HOST_DEPS/prefix"
export HOST_BUILD="$HOST_WORK/build"
export VM_NAME=rocm-ernic-loopback
export VM_DIR=/root/Codex/rocm-ernic-loopback-vm
export VM_DISK="$VM_DIR/rocm-ernic-loopback.qcow2"
export VFIO_SOCKET="$VM_DIR/vfio-user.sock"
export IMAGE_TGZ=/root/ByteDance/Images/debian-13-generic-amd64-20260803-2559.qcow2.tgz
export BASE_IMAGE="$VM_DIR/debian-13-generic-amd64-20260803-2559.qcow2"
```

VM 内使用相同的仓库地址和版本，但路径属于 VM 自己的文件系统：

```bash
export REPO_URL=https://github.com/zhangpengfei-xyz/rocm-ernic.git
export ERNIC_REV=main                 # 必须与主机检出的版本一致
export VM_REPO=/root/ByteDance/rocm-ernic
export VM_WORK=/root/Codex/rocm-ernic-vm
export RDMA_PREFIX=/opt/rdma-core-ernic
```

如果待验证提交尚未推送，请先把它推送到主机和 VM 都能访问的仓库，再开始
下面的两个独立 clone。不要通过 `rsync` 或 `scp` 复制源码或构建目录。

## 2. 主机：独立 clone 并只构建服务端

当前 `/root/ByteDance/rocm-ernic` 已是主机 clone 时不要重复创建；从空环境执行时，
该目录必须尚不存在：

```bash
test -d "$HOST_REPO/.git" || git clone "$REPO_URL" "$HOST_REPO"
git -C "$HOST_REPO" fetch origin
git -C "$HOST_REPO" checkout "$ERNIC_REV"
mkdir -p "$HOST_DEPS/packages" "$HOST_PREFIX" "$HOST_BUILD"
```

主机需要 `git`、`gcc`、`cmake`、`ninja`、`meson`、`pkg-config`、
`glib-2.0` 和 `json-c` 的开发环境，以及 QEMU/libvirt 工具。不要在主机执行
`cmake --install` 或安装本项目生成的包。

如果主机没有 libibverbs/librdmacm 开发头文件，可下载 Debian 包并只解压到
隔离 sysroot；这不会调用 `dpkg -i`，也不会修改主机系统目录：

```bash
cd "$HOST_DEPS/packages"
apt download libibverbs-dev libibverbs1 librdmacm-dev
for package in ./*.deb; do
  dpkg-deb -x "$package" "$HOST_SYSROOT"
done
```

在主机的隔离前缀构建 libvfio-user：

```bash
git clone https://github.com/nutanix/libvfio-user.git \
  "$HOST_DEPS/libvfio-user"
git -C "$HOST_DEPS/libvfio-user" checkout 20e9803
meson setup "$HOST_DEPS/libvfio-user/build" \
  "$HOST_DEPS/libvfio-user" --prefix="$HOST_PREFIX" -Dwarning_level=0
ninja -C "$HOST_DEPS/libvfio-user/build"
meson install -C "$HOST_DEPS/libvfio-user/build"
```

只构建主机需要的服务端、PCI 配置测试和后端单元测试：

```bash
export PKG_CONFIG_PATH="$HOST_SYSROOT/usr/lib/x86_64-linux-gnu/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export CFLAGS="-I$HOST_SYSROOT/usr/include"
export LDFLAGS="-L$HOST_SYSROOT/usr/lib/x86_64-linux-gnu"

cmake -S "$HOST_REPO" -B "$HOST_BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DERNIC_BUILD_KMOD=OFF \
  -DVFIO_USER_FALLBACK_INC="$HOST_PREFIX/include" \
  -DVFIO_USER_FALLBACK_LIB="$HOST_PREFIX/lib/x86_64-linux-gnu/libvfio-user.so"
cmake --build "$HOST_BUILD" --target \
  rocm-ernic test_pci_client test_rdma_backend_query_port -j"$(nproc)"
```

先执行不需要 VM 的主机侧检查：

```bash
export LD_LIBRARY_PATH="$HOST_PREFIX/lib/x86_64-linux-gnu:$HOST_SYSROOT/usr/lib/x86_64-linux-gnu"
ctest --test-dir "$HOST_BUILD" --output-on-failure \
  -R 'pci-config-test|rdma-backend-query-port-unit'
SERVER_BIN="$HOST_BUILD/rocm-ernic" \
  "$HOST_REPO/tests/test_loopback_ci.sh"
```

最后一条命令检查 `loopback`、`preserve`、`zeros`、`random` 和 `md5`
配置能否启动；它不代替后面的 VM 数据通路测试。

## 3. 主机：用 virt-install 创建独立 VM

先从只读基础镜像创建 overlay。基础镜像和 `debian-13-dev` 均不会被修改：

```bash
mkdir -p "$VM_DIR"
tar -xzf "$IMAGE_TGZ" -C "$VM_DIR"
qemu-img create -f qcow2 -F qcow2 -b "$BASE_IMAGE" "$VM_DISK" 64G
```

首次创建时不添加 ERNIC 设备。共享 `memfd` 是 vfio-user DMA 映射的必要条件：

```bash
virt-install \
  --name "$VM_NAME" \
  --memory 16384 \
  --memorybacking source.type=memfd,access.mode=shared \
  --vcpus vcpus=8,sockets=1,cores=8,threads=1 \
  --cpu host-passthrough,migratable=off \
  --machine q35 \
  --clock hpet_present=yes \
  --disk path="$VM_DISK",format=qcow2,bus=virtio,cache=none,address.type=pci,address.bus=0,address.slot=4,address.function=0 \
  --network network=default,model=virtio-transitional \
  --rng /dev/urandom \
  --graphics none \
  --security type=none \
  --osinfo debian13 \
  --import \
  --noautoconsole
```

查询 libvirt DHCP 地址并登录。基础镜像的初始账户为 `root/debian13`；首次可用
`virsh console "$VM_NAME"` 登录并配置 SSH 公钥。

```bash
virsh domifaddr "$VM_NAME" --source lease
export VM_IP=<上一步显示的IPv4地址>
ssh root@"$VM_IP"
```

## 4. VM：独立 clone 并只构建 VM 产物

本节全部在新 VM 内执行。允许把依赖安装到该 VM：

```bash
export http_proxy=http://192.168.100.1:3128
export https_proxy=http://192.168.100.1:3128

apt update
apt install -y build-essential cmake ninja-build pkg-config git curl \
  pciutils kmod linux-image-amd64 linux-headers-amd64 rdma-core \
  libibverbs-dev librdmacm-dev libcmocka-dev libnl-3-dev \
  libnl-route-3-dev libudev-dev libsystemd-dev python3-pyelftools

git clone "$REPO_URL" "$VM_REPO"
git -C "$VM_REPO" checkout "$ERNIC_REV"
mkdir -p "$VM_WORK/tests"
```

只构建 VM 使用的两个内核模块：

```bash
make -C "/lib/modules/$(uname -r)/build" \
  M="$VM_REPO/driver" -j"$(nproc)" modules
```

构建并安装带 `rocm_ernic` provider 的 rdma-core。安装前缀位于独立 VM 内，
不会影响主机：

```bash
RDMA_CORE_VERSION=64.0 \
  "$VM_REPO/scripts/build-rdma-core.sh" \
  "$VM_WORK/rdma-core" "$RDMA_PREFIX" "$(nproc)"
```

直接从 VM 的独立 clone 构建 VM 侧测试，不在 VM 内构建 vfio-user 服务端：

```bash
gcc -D_GNU_SOURCE -O2 -Wall -Wextra \
  -I"$RDMA_PREFIX/include" \
  "$VM_REPO/tests/test_data_transfer.c" \
  -L"$RDMA_PREFIX/lib" -Wl,-rpath,"$RDMA_PREFIX/lib" -libverbs \
  -o "$VM_WORK/tests/test_data_transfer"

gcc -O2 -Wall -Wextra \
  -I"$RDMA_PREFIX/include" \
  "$VM_REPO/tests/test_rdma_cm.c" \
  -L"$RDMA_PREFIX/lib" -Wl,-rpath,"$RDMA_PREFIX/lib" -libverbs \
  -o "$VM_WORK/tests/test_rdma_cm"

gcc -D_POSIX_C_SOURCE=200809L -O2 -Wall -Wextra \
  -I"$RDMA_PREFIX/include" \
  -I"$VM_REPO/rdma-core/providers/rocm_ernic" \
  "$VM_REPO/tests/test_dc_loopback.c" \
  -L"$RDMA_PREFIX/lib" -Wl,-rpath,"$RDMA_PREFIX/lib" \
  -libverbs -lrocm_ernic \
  -o "$VM_WORK/tests/test_dc_loopback"

gcc -O2 -Wall -Wextra -I"$VM_REPO/driver" \
  "$VM_REPO/tests/test_ernic_dc_uapi.c" -lcmocka \
  -o "$VM_WORK/tests/test_ernic_dc_uapi"
```

完成后在 VM 内关机：

```bash
systemctl poweroff
```

## 5. 主机：用 virt-xml 插入 ERNIC 设备

必须在 VM 关机状态下修改持久 XML：

```bash
virsh domstate "$VM_NAME"
```

Debian 13 的 virtinst 5.0.0 会把 `--qemu-commandline` 中 JSON 引号重复转义，
导致 QEMU 收到字面量 `&quot;`。仓库内的
`scripts/virt-xml-qemu-json` 保留标准 virt-xml 参数和处理流程，只修正这一处
转义；它不是运行时生成的临时脚本。

```bash
"$HOST_REPO/scripts/virt-xml-qemu-json" "$VM_NAME" --edit \
  --qemu-commandline='-device "{\"driver\":\"vfio-user-pci\",\"socket\":{\"type\":\"unix\",\"path\":\"/root/Codex/rocm-ernic-loopback-vm/vfio-user.sock\"}}"'
```

确认设备参数已写入 libvirt 配置，而且原生 QEMU 参数中保留了 JSON 引号：

```bash
virsh dumpxml "$VM_NAME" | grep -A3 qemu:commandline
virsh dumpxml "$VM_NAME" | \
  virsh domxml-to-native qemu-argv /dev/stdin | grep vfio-user-pci
```

不要直接运行 `qemu-system-x86_64`，也不要使用仓库外的 VM 启停脚本。

## 6. 启动服务端和 VM

在主机终端 A 启动服务端。服务端必须先创建 socket：

```bash
export LD_LIBRARY_PATH="$HOST_PREFIX/lib/x86_64-linux-gnu:$HOST_SYSROOT/usr/lib/x86_64-linux-gnu"
"$HOST_BUILD/rocm-ernic" \
  --socket "$VFIO_SOCKET" \
  --backend loopback:mode=preserve \
  --stats-file "$VM_DIR/stats.txt" \
  --verbose 2>&1 | tee "$VM_DIR/server.log"
```

在主机终端 B 等待 socket，随后只通过 libvirt 启动 VM：

```bash
until test -S "$VFIO_SOCKET"; do sleep 0.1; done
virsh start "$VM_NAME"
virsh domifaddr "$VM_NAME" --source lease
```

## 7. VM：加载驱动并运行功能测试

登录 VM，加载刚刚在 VM 内构建的模块：

```bash
ssh root@"$VM_IP"
modprobe ib_uverbs
insmod "$VM_REPO/driver/rocm_ernic_eth.ko"
insmod "$VM_REPO/driver/rocm_ernic_rdma.ko"

export LD_LIBRARY_PATH="$RDMA_PREFIX/lib"
export IBV_DRIVERS_PATH="$RDMA_PREFIX/lib/libibverbs"
export IBV_DRIVERS=rocm_ernic
```

先确认 PCI、内核驱动和 verbs provider：

```bash
lspci -nnk -d 1022:8000
rdma dev show
"$RDMA_PREFIX/bin/ibv_devinfo" -d "$(basename /sys/class/infiniband/rocep*)"
```

运行默认 `preserve` 模式的数据面、QP 配对、DC/SRQ 和 UAPI 测试：

```bash
ERNIC_LOOPBACK_MODE=preserve "$VM_WORK/tests/test_data_transfer"
"$VM_WORK/tests/test_rdma_cm"
"$VM_WORK/tests/test_dc_loopback"
"$VM_WORK/tests/test_ernic_dc_uapi"
```

`test_data_transfer` 会验证以下内容，而不只是等待完成事件：

- PD、CQ、QP、MR 的创建、状态迁移和释放；
- 单次及连续 SEND/RECV；
- 64、256、1024、2048、4096 字节传输；
- RDMA WRITE 和 RDMA READ；
- 接收缓冲区和远端缓冲区的实际字节内容。

`test_rdma_cm` 验证两个 RC QP 的自动配对及 `dest_qp_num` 查询；
`test_dc_loopback` 验证 SRQ 创建、查询、修改、接收队列，DCT/DCI 状态迁移，
以及 SEND_DC 的收发完成和实际负载内容。

验证 sysfs loopback 开关：

```bash
SYSFS_LOOPBACK="/sys/class/infiniband/$(basename /sys/class/infiniband/rocep*)/loopback"
cat "$SYSFS_LOOPBACK"
echo 1 > "$SYSFS_LOOPBACK"
test "$(cat "$SYSFS_LOOPBACK")" = 1
echo 0 > "$SYSFS_LOOPBACK"
test "$(cat "$SYSFS_LOOPBACK")" = 0
```

主机侧统计应同时出现 SEND、RDMA_READ、RDMA_WRITE 和 SEND_DC：

```bash
grep -E 'total_bytes_|SEND_DC|RDMA_READ|RDMA_WRITE|SEND' "$VM_DIR/stats.txt"
```

## 8. 验证全部数据模式和 MD5

数据模式属于服务端启动参数，切换模式必须冷关机后重启服务端和 VM。每一轮按
以下顺序执行：

1. 在 VM 内执行 `systemctl poweroff`，用 `virsh domstate "$VM_NAME"`
   确认状态为 `shut off`；
2. 在主机终端 A 用 `Ctrl-C` 停止旧服务端；
3. 设置 `MODE`，用新模式重新启动服务端；
4. 用 `virsh start "$VM_NAME"` 启动 VM；
5. 加载驱动，并用相同的 `ERNIC_LOOPBACK_MODE` 运行数据测试。

依次将 `MODE` 设置为下面七个值：

```bash
MODE=preserve   # 随后分别改为 zeros、ones、increment、decrement、alternate、random
```

主机终端 A：

```bash
"$HOST_BUILD/rocm-ernic" \
  --socket "$VFIO_SOCKET" \
  --backend "loopback:mode=$MODE" \
  --stats-file "$VM_DIR/stats-$MODE.txt" \
  --verbose 2>&1 | tee "$VM_DIR/server-$MODE.log"
```

VM 内：

```bash
ERNIC_LOOPBACK_MODE="$MODE" "$VM_WORK/tests/test_data_transfer"
```

MD5 是附加选项，不改变数据模式。以 preserve 数据再运行一轮：

```bash
# 主机终端 A
"$HOST_BUILD/rocm-ernic" \
  --socket "$VFIO_SOCKET" \
  --backend loopback:mode=preserve,md5 \
  --stats-file "$VM_DIR/stats-preserve-md5.txt" \
  --verbose 2>&1 | tee "$VM_DIR/server-preserve-md5.log"

# VM 内
ERNIC_LOOPBACK_MODE=preserve "$VM_WORK/tests/test_data_transfer"

# 主机：摘要必须显示非零字节数，且不同输入应产生不同摘要
grep 'Data MD5:' "$VM_DIR/server-preserve-md5.log"
```

## 9. 停止、重启和移除环境

正常停止时先关闭 VM，再停止服务端：

```bash
# VM 内
systemctl poweroff

# 主机
virsh domstate "$VM_NAME"
# 确认为 shut off 后，在服务端终端按 Ctrl-C
```

再次运行时先启动服务端，确认 socket 已创建，再执行：

```bash
virsh start "$VM_NAME"
```

若不再需要该环境，先确认 VM 已关机，再移除 libvirt 定义；磁盘文件仍保留，
可重新导入：

```bash
virsh undefine "$VM_NAME"
```

## 10. 验证范围与通过标准

一次完整回归应满足：

| 功能 | 通过标准 |
| --- | --- |
| vfio-user 设备 | `lspci` 显示 `1022:8000`，BAR0/1/2 均成功分配 |
| verbs 基础资源 | device、PD、CQ、QP、MR 创建与释放成功 |
| 双边数据面 | SEND/RECV 完成，64–4096 字节内容校验通过 |
| 单边数据面 | RDMA WRITE、RDMA READ 完成且内容校验通过 |
| 连接信息 | 两个 RC QP 自动配对，彼此的 `dest_qp_num` 正确 |
| DC/SRQ | SRQ 创建/查询/修改/接收、DCT/DCI 状态迁移、SEND_DC 内容校验均通过 |
| 数据模式 | preserve、zeros、ones、increment、decrement、alternate、random 全部通过 |
| MD5 | 日志显示真实非零长度及对应摘要 |
| 控制面 | sysfs loopback 开关可读、可写、可恢复 |
| 统计 | SEND、RECV、RDMA_READ、RDMA_WRITE、SEND_DC 字节或 WQE 计数增加 |

上述范围覆盖 loopback 后端当前公开的数据模式、主要 verbs 资源、RC
双边/单边传输、QP 自动配对和 DC/SRQ 路径。性能压测、故障注入、TCP/verbs
后端以及多 VM 互联不属于本单 VM loopback 验证范围。

## 11. 常见问题

- QEMU 报 DMA 映射失败：确认 domain XML 中存在共享 `memfd`，并且服务端在
  `virsh start` 之前已经创建 socket。
- QEMU 报 `JSON parse error` 或参数中出现 `&quot;`：确认使用了仓库内
  `scripts/virt-xml-qemu-json`，并检查 `domxml-to-native` 输出。
- `lspci` 有设备但没有 RDMA 设备：确认按 Ethernet、RDMA 的顺序加载模块，
  再查看 `dmesg`。
- provider 未发现：确认 `LD_LIBRARY_PATH`、`IBV_DRIVERS_PATH` 和
  `IBV_DRIVERS` 均指向 VM 内的 `$RDMA_PREFIX`。
- 更换服务端二进制或数据模式后必须冷重启 VM，不能复用旧 vfio-user 连接。
