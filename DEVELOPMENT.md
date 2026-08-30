# debian-13-dev 内的嵌套 VM 端到端调测

第 1～9 节只使用 `debian-13-dev`：`rocm-ernic` 服务端和 libvirt/QEMU 均在
其中运行，驱动、定制 rdma-core provider 和数据测试只在独立的嵌套 VM 内
运行。第 10 节的 MLNX 实机测试另需一台同 L2/VLAN 的 CX6 对端，但对端只运行
发行版 `ibv_rc_pingpong`。不在物理主机或对端构建、安装本项目。

除最开始的登录命令外，本文所有“外层”命令均在 `debian-13-dev` 中以 root
身份执行：

```bash
ssh root@192.168.100.30
```

项目构建、安装前缀、镜像和日志统一放在 `/root/Codex`。
不要在 `debian-13-dev` 执行本项目的系统级安装或把自行构建的库复制到 `/usr` 等；
只有嵌套 VM 可以安装内核和 provider。

## 1. 变量和依赖

在 `debian-13-dev` 的终端中设置：

```bash
export {http,https}_proxy=http://192.168.100.1:3128
VM_IMAGE=debian-13-generic-amd64-20260803-2559.qcow2
VM_NAME=rocm-ernic-e2e
VM_IP=172.16.100.30
REPO_DIR=/root/ByteDance/rocm-ernic
LIBVFIO_DIR=$REPO_DIR/libvfio-user
E2E_DIR=/root/Codex/$VM_NAME
LIBVFIO_BUILD=$E2E_DIR/libvfio-user/build
LIBVFIO_PREFIX=$E2E_DIR/libvfio-user/usr
SERVER_BUILD=$E2E_DIR/build
SERVER_BIN=$SERVER_BUILD/rocm-ernic
VFIO_SOCK=$E2E_DIR/vfio-user.sock
JOB_NUM=$(nproc)
```

确认 KVM 可用并安装发行版依赖。这些是系统依赖，不是项目构建产物：

```bash
apt-get update
apt-get install -y build-essential cmake meson ninja-build pkg-config \
  libglib2.0-dev libjson-c-dev libcmocka-dev libibverbs-dev librdmacm-dev \
  libvirt-daemon-system libvirt-clients virtinst \
  sshpass git tar
mkdir -p $E2E_DIR
```

Debian 的 QEMU 11.0.2 在 QMP `device_del` vfio-user 设备时会触发
`memory_region_del_subregion` 断言。安装包含上游
[`vfio-user: Do not delete the subregion`](https://lore.kernel.org/qemu-devel/20251010-vfio-v1-1-d7a6056539b7@rsg.ci.i.u-tokyo.ac.jp/)
补丁的本地重编包：

```bash
apt-get install -y --reinstall /root/qemu-system-x86_11.0.2+ds-2~bpo13+1_amd64.deb
```

## 2. 构建服务端

libvfio-user 和服务端产物均写入 `$E2E_DIR`：

```bash
git -C $REPO_DIR submodule update --init

meson setup $LIBVFIO_BUILD $LIBVFIO_DIR \
  --prefix=$LIBVFIO_PREFIX --libdir=lib
meson compile -C $LIBVFIO_BUILD -j $JOB_NUM
meson install -C $LIBVFIO_BUILD

cmake -S $REPO_DIR -B $SERVER_BUILD -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DERNIC_BUILD_KMOD=OFF \
  -DCMAKE_PREFIX_PATH=$LIBVFIO_PREFIX
cmake --build $SERVER_BUILD -j $JOB_NUM
ctest --test-dir $SERVER_BUILD --output-on-failure
```

确认服务端通过 RUNPATH 加载隔离前缀，不依赖 `LD_LIBRARY_PATH`：

```bash
readelf -d $SERVER_BIN | grep -E 'RPATH|RUNPATH'
env -u LD_LIBRARY_PATH $SERVER_BIN --help >/dev/null
```

## 3. 创建并初始化嵌套 VM

解压独立磁盘并注册 VM：

```bash
cd $E2E_DIR
tar -xzf /root/ByteDance/Images/$VM_IMAGE.tgz
mv $VM_IMAGE $VM_NAME.qcow2

virt-install --osinfo debian13 --machine pc \
  --name $VM_NAME --import --noreboot \
  --vcpus 32 --cpu host-passthrough,topology.sockets=1 \
  --memory 65536 --memorybacking source.type=memfd,access.mode=shared \
  --disk path=$VM_NAME.qcow2,bus=virtio \
  --network network=default,model=virtio \
  --graphics spice --video virtio
```

`--noreboot` 使导入后的 VM 保持关机。首次启动后通过串口用
`root/debian13` 登录，并配置固定地址：

```bash
virsh start $VM_NAME
virsh console $VM_NAME
```

在 VM 控制台内执行：

```bash
cat >/etc/systemd/network/ens3.network <<'EOF'
[Match]
Name=ens3

[Network]
Address=172.16.100.30/24
Gateway=172.16.100.1
DNS=172.16.100.1
EOF
systemctl restart systemd-networkd
```

按 `Ctrl+]` 退出控制台。保持 VM 运行，在 `debian-13-dev` 的另一个终端设置
第 1 节变量，再准备 VM。源码直接取自当前提交，无需先推送：

```bash
export SSHPASS=debian13
vm_ssh() {
  sshpass -e ssh -o StrictHostKeyChecking=no -o ConnectTimeout=3 \
    root@$VM_IP "$@"
}

vm_ssh 'export {http,https}_proxy=http://192.168.100.1:3128
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y build-essential cmake ninja-build pkg-config curl \
  pciutils kmod linux-image-amd64 linux-headers-amd64 rdma-core \
  libibverbs-dev librdmacm-dev libcmocka-dev libnl-3-dev \
  libnl-route-3-dev libudev-dev libsystemd-dev python3-pyelftools'

git -C $REPO_DIR archive HEAD | vm_ssh \
  'mkdir -p /root/ByteDance/rocm-ernic && \
   tar -xf - -C /root/ByteDance/rocm-ernic'

vm_ssh systemctl poweroff
```

等待 `virsh domstate $VM_NAME` 显示 `shut off`。
后续启动将使用新安装的内核和对应 headers。

## 4. 启动服务端和带 ERNIC 的 VM

两个终端都先设置第 1 节变量；终端 B 继续使用第 3 节定义的 `vm_ssh`。
终端 A 前台启动服务端：

```bash
start_server() {
  RUN_NAME=$1
  local backend=${2:-loopback:mode=$RUN_NAME}
  rm -f $VFIO_SOCK
  $SERVER_BIN \
    --socket $VFIO_SOCK \
    --backend $backend \
    --stats-file $E2E_DIR/stats-$RUN_NAME.txt \
    --log-file $E2E_DIR/server-$RUN_NAME.log
}

start_server preserve
```

终端 B 定义 QMP 热插拔函数，然后启动 VM 并插入 ERNIC：

```bash
qmp() {
  virsh qemu-monitor-command $VM_NAME --pretty "$1"
}

device_present() {
  vm_ssh lspci -n -d 1022:8000 | grep -q 1022:8000
}

attach_ernic() {
  until test -S $VFIO_SOCK; do sleep 1; done
  qmp '{"execute":"device_add","arguments":{"driver":"vfio-user-pci","socket":{"type":"unix","path":"'$VFIO_SOCK'"},"id":"ernic0"}}'
  until device_present; do sleep 1; done
}

detach_ernic() {
  qmp '{"execute":"device_del","arguments":{"id":"ernic0"}}'
  until ! device_present; do sleep 1; done
}

virsh start $VM_NAME
until vm_ssh true 2>/dev/null; do sleep 1; done
attach_ernic
```

QMP 应返回 `"return": {}`。`ernic0` 仅存在于当前 VM 进程，不会写入 libvirt XML；
VM 每次冷启动后都要重新执行 `device_add`。
切换测试配置时则通过 `device_del` 和 `device_add` 重新连接设备，无需重启 VM。

## 5. 在 VM 内构建驱动和 provider

登录 VM：

```bash
vm_ssh
```

在 VM 内执行：

```bash
export http_proxy=http://192.168.100.1:3128
export https_proxy=$http_proxy
REPO_DIR=/root/ByteDance/rocm-ernic
WORK_DIR=/root/Codex/rocm-ernic-vm
RDMA_PREFIX=$WORK_DIR/rdma-prefix
JOB_NUM=$(nproc)

make -C $REPO_DIR/driver -j $JOB_NUM

$REPO_DIR/scripts/build-rdma-core.sh \
  $WORK_DIR/rdma-core $RDMA_PREFIX $JOB_NUM

gcc -D_GNU_SOURCE -O2 -Wall -Wextra \
  -I$RDMA_PREFIX/include $REPO_DIR/tests/test_data_transfer.c \
  -L$RDMA_PREFIX/lib -Wl,-rpath,$RDMA_PREFIX/lib -libverbs \
  -o $WORK_DIR/test_data_transfer

gcc -D_GNU_SOURCE -O2 -Wall -Wextra \
  -I$RDMA_PREFIX/include $REPO_DIR/tests/test_rdma_cm.c \
  -L$RDMA_PREFIX/lib -Wl,-rpath,$RDMA_PREFIX/lib -libverbs \
  -o $WORK_DIR/test_rdma_cm
```

以上安装和构建产物全部位于嵌套 VM。

## 6. 加载驱动并运行端到端测试

仍在 VM 内执行：

```bash
export LD_LIBRARY_PATH=$RDMA_PREFIX/lib
export IBV_DRIVERS_PATH=$RDMA_PREFIX/lib/libibverbs
export IBV_DRIVERS=rocm_ernic
LOAD_TIME=$(date --iso-8601=seconds)

modprobe ib_uverbs
insmod $REPO_DIR/driver/rocm_ernic_eth.ko
insmod $REPO_DIR/driver/rocm_ernic_rdma.ko

until ls /sys/class/infiniband/* >/dev/null 2>&1; do sleep 1; done

lspci -nnk -d 1022:8000
rdma dev show
$RDMA_PREFIX/bin/ibv_devinfo
$WORK_DIR/test_data_transfer
$WORK_DIR/test_rdma_cm
```

`test_data_transfer` 应完成 PD、CQ、QP、MR 创建，以及单次、5 次连续和
64/256/1024/2048/4096 字节传输，最终显示 `ALL TESTS PASSED`、11 个发送完成和
11 个接收完成。`test_rdma_cm` 应显示两个 RC QP 均为 RTS，
并能查询到彼此的 `dest_qp_num`。

检查 sysfs 开关和本轮内核告警：

```bash
RDMA_DEV=$(basename /sys/class/infiniband/*)
LOOPBACK_SWITCH=/sys/class/infiniband/$RDMA_DEV/loopback

grep -qx 0 $LOOPBACK_SWITCH
echo 1 >$LOOPBACK_SWITCH
grep -qx 1 $LOOPBACK_SWITCH
echo 0 >$LOOPBACK_SWITCH
grep -qx 0 $LOOPBACK_SWITCH

! journalctl -k --since $LOAD_TIME --no-pager | \
  grep -E 'WARNING:|BUG:|Oops:|general protection fault'
```

## 7. 数据模式和 MD5 回归

需要验证以下七种模式：

```text
preserve zeros ones increment decrement alternate random
```

切换到其余模式时保持 VM 运行：

1. 在终端 B 卸载驱动并拔出设备：

```bash
vm_ssh rmmod rocm_ernic_rdma rocm_ernic_eth
detach_ernic
```

2. 在终端 A 按 `Ctrl-C`，再启动下一种模式：

```bash
start_server zeros
```

3. 在终端 B 重新插入设备：

```bash
attach_ernic
```

4. 在 VM 内重新加载第 6 节的两个驱动模块并运行 `test_data_transfer`。

每轮都应显示 `ALL TESTS PASSED`。服务端日志应包含对应模式且无崩溃：

```bash
case $RUN_NAME in
  increment) LOG_PATTERN=incrementing ;;
  decrement) LOG_PATTERN=decrementing ;;
  alternate) LOG_PATTERN=alternating ;;
  *) LOG_PATTERN=$RUN_NAME ;;
esac
grep -F "Data pattern='$LOG_PATTERN'" $E2E_DIR/server-$RUN_NAME.log
! grep -Eq 'Assertion|Aborted|Segmentation fault' \
  $E2E_DIR/server-$RUN_NAME.log
```

MD5 按上述热切换步骤另起一轮，终端 A 改为执行：

```bash
start_server preserve-md5 loopback:mode=preserve,md5
```

运行 `test_data_transfer` 后检查；摘要应覆盖非零字节并随输入变化，空数据摘要表示失败：

```bash
grep 'Data MD5:' $E2E_DIR/server-$RUN_NAME.log
! grep -q 'd41d8cd98f00b204e9800998ecf8427e (0 bytes)' \
  $E2E_DIR/server-$RUN_NAME.log
```

## 8. 修复问题的专项回归

完成基本测试后，对 BAR 长度、AdminQ 卸载和 MSI-X
热插拔修复执行以下专项回归。每个卸载或热插拔场景都应在新一轮
server 日志中单独验证，以免混入前一轮的告警。

加载驱动后验证 BAR2 是 `512 × 4 KiB = 2 MiB`，并且三个 MSI-X vector 已启用：

```bash
ERNIC_BDF=$(lspci -Dnn -d 1022:8000 | awk 'NR == 1 { print $1 }')
lspci -vv -s $ERNIC_BDF | tee $WORK_DIR/ernic-lspci.log
grep -Eq 'Region 2:.*\[size=2M\]' $WORK_DIR/ernic-lspci.log
grep -Eq 'MSI-X: Enable\+ Count=3' $WORK_DIR/ernic-lspci.log
```

正常卸载场景从嵌套 VM 内执行。测试前记录时间，卸载必须在 5 秒内完成，
且 AdminQ 清理期间不得出现 command timeout 或内核严重告警：

```bash
UNLOAD_TIME=$(date --iso-8601=seconds)
SECONDS=0
rmmod rocm_ernic_rdma rocm_ernic_eth
test $SECONDS -lt 5

! journalctl -k --since $UNLOAD_TIME --no-pager | \
  grep -E 'command timeout|WARNING:|BUG:|Oops:|general protection fault'
```

在外层确认 server 已处理 QP、CQ、MR 和 PD 的 AdminQ 清理请求，
并为响应触发了 MSI-X vector 0：

```bash
for CMD in DESTROY_QP DESTROY_CQ DESTROY_MR DESTROY_PD; do
  grep -Eq "ADMINQ\[[0-9]+\] RSP cmd=[0-9]+\($CMD\).*err=0" \
    $E2E_DIR/server-$RUN_NAME.log
done
grep -F 'Successfully triggered interrupt vector 0' \
  $E2E_DIR/server-$RUN_NAME.log
```

卸载完成后，在外层执行 `device_del` 并重新启动 server。
为验证 ADD notifier，先在设备不存在时加载模块，再热插入设备：

```bash
vm_ssh "modprobe ib_uverbs
insmod $REPO_DIR/driver/rocm_ernic_eth.ko
insmod $REPO_DIR/driver/rocm_ernic_rdma.ko"
attach_ernic
until vm_ssh 'rdma dev show | grep -q rocep'; do sleep 1; done
```

验证模块保持加载时的 PCI 热拔出：在外层直接删除设备，不要先卸载驱动：

```bash
HOTUNPLUG_TIME=$(vm_ssh date --iso-8601=seconds)
detach_ernic

vm_ssh '! lspci -n -d 1022:8000 | grep -q 1022:8000 &&
! rdma dev show | grep -q rocep'
vm_ssh "! journalctl -k --since '$HOTUNPLUG_TIME' --no-pager | \
  grep -E 'irq_domain_remove|msi_device_data_release|pci_disable_msix|WARNING:|BUG:|Oops:'"
```

最后重新启动 server、插入设备并加载两个模块，在外层同时发起 `rmmod` 和 QMP `device_del`：

```bash
RACE_TIME=$(vm_ssh date --iso-8601=seconds)
vm_ssh 'rmmod rocm_ernic_rdma rocm_ernic_eth' &
RMMOD_PID=$!
qmp '{"execute":"device_del","arguments":{"id":"ernic0"}}' &
QMP_PID=$!
wait $RMMOD_PID
wait $QMP_PID
until ! device_present; do sleep 1; done

vm_ssh "! journalctl -k --since '$RACE_TIME' --no-pager | \
  grep -E 'irq_domain_remove|msi_device_data_release|pci_disable_msix|WARNING:|BUG:|Oops:'"
```

两种热插拔场景中，QMP 都应返回 `"return": {}`，QEMU 和 VM 保持运行，
guest 中不再存在 ERNIC PCI/RDMA 设备，server 可正常退出。

## 9. 停止顺序和通过标准

注意如果已执行过并发卸载热拔测试，则驱动或设备可能已不存在。

测试结束后先卸载驱动并通过 QMP 拔出设备，再在服务端终端按 `Ctrl-C`：

```bash
vm_ssh rmmod rocm_ernic_rdma rocm_ernic_eth
detach_ernic
```

QMP 应返回 `"return": {}`；服务端记录客户端断开后，在终端 A 按 `Ctrl-C`。
日志应包含 `Shutdown complete`，进程以状态 0 退出。
VM 可以保持运行；不再使用时执行 `vm_ssh systemctl poweroff`。

通过标准：

- 服务端单元测试和两个 VM 测试通过；
- 七种数据模式和 MD5 回归通过；
- sysfs loopback 开关可恢复为 0，内核没有新增严重告警；
- BAR2 长度、MSI-X、AdminQ 卸载及热拔插专项回归通过；
- `device_del` 后 QEMU 和 VM 保持运行，服务端正常退出且没有崩溃。

## 10. MLNX identity-mirror backend 实机调测

本节在前述 loopback 流程之外，验证 `docs/mlnx-backend-design.md` 冻结的
首期 RC SEND/RECV 路径。MLNX backend 不为 guest 提供普通 Ethernet 出口：
ERNIC netdev 只持有与物理端口相同的 MAC 和 `/32` shadow IPv4，测试程序的
TCP 控制连接仍通过 guest 的 `ens3` 管理网发送。

以下是已验证拓扑。更换机器时必须同时替换设备名、地址、GID index 和远端
MAC，不能只修改其中一项：

```bash
# debian-13-dev 上由 backend 独占的端口
MLNX_DEV=mlx5_0
MLNX_ETH=ens10np0
MLNX_IP=10.232.167.93

# 对端机器的管理地址，以及用于 RoCE 的独立端口
PEER_SSH=root@10.232.167.41
PEER_DEV=mlx5_2
PEER_ETH=ens9f0np0
PEER_IP=10.232.167.92
PEER_MAC=b8:e9:24:53:8e:9c
PEER_GID_INDEX=3

MLNX_UNIT=rocm-ernic-mlnx-e2e
MLNX_LOG=/root/Codex/mlnx-e2e.log
VFIO_SOCK=/tmp/vfio-user-rocm-ernic-mlnx.sock
```

### 10.1 物理端口预检查

backend 要求 RDMA device 与 Ethernet netdev 属于同一 PCI function、port 1
处于 ACTIVE、显式 IPv4 已配置，并且该 IPv4 存在唯一的 RoCE v2 GID：

```bash
test "$(readlink -f /sys/class/infiniband/$MLNX_DEV/device)" = \
     "$(readlink -f /sys/class/net/$MLNX_ETH/device)"
test "$(cat /sys/class/infiniband/$MLNX_DEV/ports/1/state | awk '{print $1}')" = 4
ip -4 -o address show dev $MLNX_ETH | grep -F " $MLNX_IP/"
show_gids | grep -E "^$MLNX_DEV[[:space:]]+1.*$MLNX_IP.*v2"
ibv_devinfo -d $MLNX_DEV
```

实测环境中 `mlx5_0/ens10np0` 的 BDF 是 `0000:00:0a.0`，IPv4 RoCE v2
GID index 是 3。服务端只接受完整的六字段配置，字段不能缺失、重复或扩展：

```text
mlnx:device=mlx5_0,ethdev=ens10np0,port=1,roce=v2,ip=10.232.167.93,mirror-mac=on
```

`verbs:` 仅作为相同严格语法的兼容别名。MLNX 初始化还要求 4 KiB 基础页和
至少 128 MiB memlock；普通登录 shell 的 memlock 通常不足，因此不要直接从
默认 shell 启动正式实例。

### 10.2 启动 MLNX server 并插入设备

先按第 2 节构建服务端，按第 3 节启动 VM，并在当前 shell 定义第 3、4 节的
`vm_ssh`、`qmp`、`attach_ernic` 和 `detach_ernic` 函数。使用 transient
systemd unit 提供无限 memlock：

```bash
MLNX_BACKEND="mlnx:device=$MLNX_DEV,ethdev=$MLNX_ETH,port=1,roce=v2,ip=$MLNX_IP,mirror-mac=on"

rm -f $VFIO_SOCK $MLNX_LOG
systemd-run --unit=$MLNX_UNIT --collect --service-type=exec \
  --property=LimitMEMLOCK=infinity \
  $SERVER_BIN --socket $VFIO_SOCK --backend "$MLNX_BACKEND" \
  --log-file $MLNX_LOG

until test -S $VFIO_SOCK; do sleep 1; done
systemctl is-active --quiet $MLNX_UNIT
systemctl show $MLNX_UNIT -p LimitMEMLOCK | grep -q infinity
grep -F "MLNX identity: $MLNX_DEV/$MLNX_ETH port=1" $MLNX_LOG

attach_ernic
```

同一个 `(PCI BDF, port)` 同时只能运行一个 MLNX backend。不要在测试期间
启动 host 上的 `ibv_rc_pingpong -d mlx5_0` 或另一个绑定该端口的 server。

### 10.3 guest 驱动、provider 和 identity 检查

按第 5 节在 guest 构建驱动及 rdma-core；源码必须来自当前待测提交。然后加载
驱动，并让 systemd-networkd 从 server 的 fixed-identity DHCP 获得 `/32`
地址：

```bash
vm_ssh "modprobe ib_uverbs
insmod /root/ByteDance/rocm-ernic/driver/rocm_ernic_eth.ko
insmod /root/ByteDance/rocm-ernic/driver/rocm_ernic_rdma.ko"

vm_ssh 'GUEST_RDMA_DEV=$(basename $(find /sys/class/infiniband \
  -mindepth 1 -maxdepth 1 -type l | head -n1))
GUEST_ETH=$(basename /sys/class/infiniband/$GUEST_RDMA_DEV/device/net/*)
cat >/etc/systemd/network/80-ernic-identity.network <<EOF
[Match]
Name=$GUEST_ETH

[Network]
DHCP=ipv4
LinkLocalAddressing=no

[DHCPv4]
UseRoutes=no
UseDNS=no
EOF
networkctl reload
networkctl reconfigure $GUEST_ETH'
```

等待 DHCP 和 RDMA bind 完成后执行检查：

```bash
vm_ssh 'GUEST_RDMA_DEV=$(basename /sys/class/infiniband/*)
GUEST_ETH=$(basename /sys/class/infiniband/$GUEST_RDMA_DEV/device/net/*)
until ip -4 -o address show dev $GUEST_ETH | grep -q "10.232.167.93/32"; do
  sleep 1
done

lspci -vv -d 1022:8000
ip -br link show dev $GUEST_ETH
ip -4 -o address show dev $GUEST_ETH
rdma link show
show_gids | grep $GUEST_RDMA_DEV

grep -q '^4:' /sys/class/infiniband/$GUEST_RDMA_DEV/ports/1/state
test "$(cat /sys/class/infiniband/$GUEST_RDMA_DEV/ports/1/gid_attrs/types/0)" = "RoCE v2"
test "$(cat /sys/class/infiniband/$GUEST_RDMA_DEV/ports/1/gid_attrs/types/1)" = "RoCE v2"'
```

预期 guest PCI ID 为 `1022:8000`、MSI-X 为 `Enable+ Count=3`，MAC 与
`ens10np0` 相同，IPv4 为 `10.232.167.93/32`。GID table 只包含物理 MAC
派生的 link-local GID 和 `::ffff:10.232.167.93`，后者在实测 guest 中的
index 为 1。`ibv_devinfo` 不应声明 CM、SRQ、atomic 等首期未实现能力。

### 10.4 配置到远端 CX6 的 RoCE 路径

先从 `debian-13-dev` 检查远端。控制连接使用 `10.232.167.41`，但
`mlx5_2/ens9f0np0` 的 RoCE identity 是 `10.232.167.92`：

```bash
ssh $PEER_SSH "test \$(cat /sys/class/infiniband/$PEER_DEV/ports/1/state | awk '{print \$1}') = 4
ip -4 -o address show dev $PEER_ETH | grep -F ' $PEER_IP/'
show_gids | grep -E '^$PEER_DEV[[:space:]]+1.*$PEER_IP.*v2'"
```

guest identity 地址是 `/32` 且没有普通 L2 出口。手工 verbs 建链仍需要内核
构造远端 AH，因此为测试目标安装精确 host route 和静态邻居项；不要添加默认
路由或 DNS：

```bash
vm_ssh 'GUEST_RDMA_DEV=$(basename /sys/class/infiniband/*)
GUEST_ETH=$(basename /sys/class/infiniband/$GUEST_RDMA_DEV/device/net/*)
ip route replace 10.232.167.92/32 dev $GUEST_ETH
ip neigh replace 10.232.167.92 lladdr b8:e9:24:53:8e:9c \
  nud permanent dev $GUEST_ETH
ip route show dev $GUEST_ETH
ip neigh show dev $GUEST_ETH'
```

### 10.5 双 CX6 SEND/RECV 测试矩阵

guest 使用第 5 节生成的 `$RDMA_PREFIX/bin/ibv_rc_pingpong`；对端使用发行版
`/usr/bin/ibv_rc_pingpong`。以下脚本依次验证 polling 和 CQ event 两种模式、
64 B 到 1 MiB 五种长度，每项传输 20 次并启用数据校验：

```bash
RDMA_PREFIX=/root/Codex/rocm-ernic-vm/rdma-prefix
GUEST_RDMA_DEV=rocep0s10
GUEST_GID_INDEX=1
PINGPONG=$RDMA_PREFIX/bin/ibv_rc_pingpong

set -euo pipefail
for mode in poll event; do
  EVENT_OPT=
  test "$mode" = event && EVENT_OPT=-e

  for size in 64 1024 4096 65536 1048576; do
    PEER_LOG=/root/Codex/mlnx-peer-$mode-$size.log
    GUEST_LOG=/root/Codex/mlnx-guest-$mode-$size.log

    ssh $PEER_SSH \
      "timeout 120 ibv_rc_pingpong -d $PEER_DEV -i 1 \
       -g $PEER_GID_INDEX -m 1024 -s $size -n 20 -c $EVENT_OPT" \
      >$PEER_LOG 2>&1 &
    PEER_PID=$!
    sleep 1

    vm_ssh "export LD_LIBRARY_PATH=$RDMA_PREFIX/lib
export IBV_DRIVERS_PATH=$RDMA_PREFIX/lib/libibverbs
export IBV_DRIVERS=rocm_ernic
timeout 120 $PINGPONG -d $GUEST_RDMA_DEV -i 1 \
  -g $GUEST_GID_INDEX -m 1024 -s $size -n 20 -c $EVENT_OPT \
  10.232.167.41" >$GUEST_LOG 2>&1
    wait $PEER_PID

    grep -q '20 iters in' $PEER_LOG
    grep -q '20 iters in' $GUEST_LOG
  done
done
```

任何一侧非零退出、timeout、数据校验错误或缺少 `20 iters in` 都算失败。
两侧输出的 GID 应分别为 `::ffff:10.232.167.93` 和
`::ffff:10.232.167.92`。event 轮次还必须确认 CQ MSI-X vector 2 被实际触发：

```bash
grep -F 'Successfully triggered interrupt vector 2' $MLNX_LOG
! grep -Eq 'Assertion|Aborted|Segmentation fault|DEVICE_FATAL' $MLNX_LOG
```

可额外执行一轮 100 次 4096 B event 测试作为稳定性检查，只需将矩阵命令改为
`-e -s 4096 -n 100`；两端都应显示 `100 iters in`。

### 10.6 启动拒绝和独占检查

以下负向测试都要求进程在 5 秒内以状态 1 退出，不能进入等待 QEMU 连接状态。
将 stderr 分别保存到 `/root/Codex/mlnx-invalid-*.log` 并核对原因：

- 删除 `ip=` 等任意必需字段，必须报告需要完整六字段配置；
- 增加未知键、重复键或使用 `port!=1`/`roce!=v2`，必须拒绝；
- 使用 `--mac` 指定非物理 MAC，必须报告 MAC 不匹配；
- 正式实例运行时再启动相同 `(BDF, port)` 的实例，必须因独占锁失败；
- 正式实例停止后，在 `ulimit -l 8192` 的子 shell 中用合法配置启动，必须报告
  memlock 小于 134217728 字节。

例如检查缺少字段和低 memlock：

```bash
set +e
timeout 5 $SERVER_BIN --socket /tmp/mlnx-invalid.sock \
  --backend "mlnx:device=$MLNX_DEV,ethdev=$MLNX_ETH,port=1,roce=v2,mirror-mac=on" \
  >/root/Codex/mlnx-invalid-missing.log 2>&1
test $? -eq 1 || exit 1
set -e
grep -F 'requires exactly device,ethdev,port=1,roce=v2,ip,mirror-mac=on' \
  /root/Codex/mlnx-invalid-missing.log

# 仅在停止正式实例后执行。
set +e
bash -c "ulimit -l 8192; exec $SERVER_BIN \
  --socket /tmp/mlnx-low-memlock.sock --backend '$MLNX_BACKEND'" \
  >/root/Codex/mlnx-low-memlock.log 2>&1
test $? -eq 1 || exit 1
set -e
grep -Ei 'memlock.*134217728' /root/Codex/mlnx-low-memlock.log
```

### 10.7 停止和清理

先释放 guest RDMA 对象并卸载模块，再拔出设备，最后停止 server。顺序反过来
可能把正常的强制清理与实现缺陷混在一起：

```bash
vm_ssh 'rmmod rocm_ernic_rdma rocm_ernic_eth'
detach_ernic
systemctl stop $MLNX_UNIT
rm -f $VFIO_SOCK

! systemctl is-active --quiet $MLNX_UNIT
! vm_ssh 'lspci -n -d 1022:8000 | grep -q 1022:8000'
! vm_ssh 'lsmod | grep -q rocm_ernic'
```

MLNX 首期通过标准：服务端和 provider/driver 构建成功；guest 获得 3 个 MSI-X、
镜像 MAC、固定 `/32` 和两个预期 RoCE v2 GID；双 CX6 的 polling/event 全长度
矩阵均通过数据校验；所有负向启动测试按预期拒绝；清理后无 ERNIC PCI 设备、
驱动模块或运行中的 transient unit。
