# 单虚拟机 loopback RDMA 开发指南

本文说明如何在独立的 Debian 13 虚拟机中运行
`loopback:mode=preserve` 后端，并验证 SEND/RECV、RDMA READ/WRITE、
`rdma_cm` 和 DC 路径。构建依赖和内核模块只安装到独立虚拟机；主机只保留
QEMU 镜像、Unix socket，以及从虚拟机复制出的便携运行目录。

下文使用以下示例路径：

```bash
REPO=/root/ByteDance/rocm-ernic
VM_DIR=/root/Codex/rocm-ernic-loopback-vm
IMAGE=/root/ByteDance/Images/debian-13-generic-amd64-20260803-2559.qcow2.tgz
```

## 准备独立磁盘

以下操作只需执行一次。先解压基础镜像，再创建写时复制 overlay，避免修改
原始镜像：

```bash
mkdir -p "$VM_DIR"
tar -xzf "$IMAGE" -C "$VM_DIR"
qemu-img create -f qcow2 -F qcow2 \
  -b "$VM_DIR/debian-13-generic-amd64-20260803-2559.qcow2" \
  "$VM_DIR/rocm-ernic-loopback.qcow2" 64G
```

首次启动时可暂不添加 ERNIC 设备，使用 QEMU user networking 和
`hostfwd=tcp:127.0.0.1:22230-:22` 完成 SSH、公钥及虚拟机网络配置。以下命令
均在独立虚拟机内执行。访问网络前先设置代理：

```bash
export http_proxy=http://192.168.100.1:3128
export https_proxy=http://192.168.100.1:3128

apt update
apt install -y build-essential cmake meson ninja-build pkg-config git \
  curl wget rsync pciutils kmod linux-image-amd64 linux-headers-amd64 \
  libibverbs-dev librdmacm-dev rdma-core ibverbs-providers \
  libglib2.0-dev libjson-c-dev libcmocka-dev libnl-3-dev \
  libnl-route-3-dev libudev-dev python3 python3-pyelftools
```

## 构建虚拟机依赖和项目

将仓库同步到虚拟机内的相同路径。下述安装前缀全部位于虚拟机中，不会修改
主机系统。

构建 libvfio-user：

```bash
git clone https://github.com/nutanix/libvfio-user.git /root/src/libvfio-user
git -C /root/src/libvfio-user checkout 20e9803
meson setup /root/src/libvfio-user/build /root/src/libvfio-user \
  --prefix=/opt/rocm-ernic-deps -Dwarning_level=0
ninja -C /root/src/libvfio-user/build
meson install -C /root/src/libvfio-user/build
```

构建 rocm-ernic：

```bash
cmake -S /root/ByteDance/rocm-ernic \
  -B /root/build/rocm-ernic -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
  -DERNIC_BUILD_KMOD=OFF \
  -DVFIO_USER_FALLBACK_INC=/opt/rocm-ernic-deps/include \
  -DVFIO_USER_FALLBACK_LIB=/opt/rocm-ernic-deps/lib/x86_64-linux-gnu/libvfio-user.so
cmake --build /root/build/rocm-ernic -j"$(nproc)"
```

构建内核模块：

```bash
make -C "/lib/modules/$(uname -r)/build" \
  M=/root/ByteDance/rocm-ernic/driver -j"$(nproc)" modules
```

构建带有 `rocm_ernic` provider 的 rdma-core v64.0：

```bash
RDMA_CORE_VERSION=64.0 \
  /root/ByteDance/rocm-ernic/scripts/build-rdma-core.sh \
  /root/build/rdma-core /opt/rdma-core-ernic "$(nproc)"
```

## 启动服务端和虚拟机

设备服务端运行在主机上，但不安装到主机系统。将虚拟机内构建的程序和运行库
复制到临时目录：

```bash
mkdir -p "$VM_DIR/runtime/lib"
rsync -a -e 'ssh -p 22230' \
  root@127.0.0.1:/root/build/rocm-ernic/rocm-ernic \
  "$VM_DIR/runtime/"
rsync -a -e 'ssh -p 22230' \
  root@127.0.0.1:/opt/rocm-ernic-deps/lib/x86_64-linux-gnu/libvfio-user.so.0.0.1 \
  "$VM_DIR/runtime/lib/"
ln -sfn libvfio-user.so.0.0.1 "$VM_DIR/runtime/lib/libvfio-user.so.0"
```

先在主机终端 A 中启动服务端：

```bash
LD_LIBRARY_PATH="$VM_DIR/runtime/lib" \
  "$VM_DIR/runtime/rocm-ernic" \
  --socket "$VM_DIR/vfio-user.sock" \
  --backend loopback:mode=preserve \
  --stats-file "$VM_DIR/stats.json" --verbose
```

然后在主机终端 B 中启动 QEMU。必须使用共享的 `memfd` 内存后端，否则
vfio-user 无法映射虚拟机内存：

```bash
qemu-system-x86_64 \
  -machine q35,accel=kvm,memory-backend=mem0 \
  -object memory-backend-memfd,id=mem0,share=on,size=16G \
  -m 16G -cpu host -smp 8 \
  -drive file="$VM_DIR/rocm-ernic-loopback.qcow2",if=virtio,format=qcow2 \
  -netdev user,id=net0,hostfwd=tcp:127.0.0.1:22230-:22 \
  -device virtio-net-pci,netdev=net0 \
  -device virtio-rng-pci \
  -device "{\"driver\":\"vfio-user-pci\",\
    \"socket\":{\"type\":\"unix\",\
    \"path\":\"$VM_DIR/vfio-user.sock\"}}" \
  -display none -serial mon:stdio
```

当前调测环境已经生成辅助脚本，可以直接使用：

```bash
/root/Codex/rocm-ernic-loopback-vm/start-vm.sh
ssh -p 22230 root@127.0.0.1
/root/Codex/rocm-ernic-loopback-vm/stop-vm.sh
```

## 加载驱动并验证 RDMA

在虚拟机内执行：

```bash
modprobe ib_uverbs
insmod /root/ByteDance/rocm-ernic/driver/rocm_ernic_eth.ko
insmod /root/ByteDance/rocm-ernic/driver/rocm_ernic_rdma.ko

export LD_LIBRARY_PATH=/opt/rdma-core-ernic/lib
export IBV_DRIVERS_PATH=/opt/rdma-core-ernic/lib/libibverbs
export IBV_DRIVERS=rocm_ernic

rdma dev show
/opt/rdma-core-ernic/bin/ibv_devinfo -d rocep0s5
ctest --test-dir /root/build/rocm-ernic --output-on-failure
```

`test_data_transfer` 会检查 SEND/RECV、RDMA WRITE 和 RDMA READ 的完成事件
及缓冲区内容。使用 `ERNIC_RDMA_CORE_BUILD=ON` 配置构建时，CTest 也会运行
DC 冒烟测试；也可以将 `tests/test_dc_loopback.c` 链接到
`/opt/rdma-core-ernic` 后单独执行。

## 问题诊断

查看主机侧统计和日志：

```bash
cat "$VM_DIR/stats.json"
tail -f "$VM_DIR/server.log"
tail -f "$VM_DIR/serial.log"
```

查看虚拟机设备和内核日志：

```bash
lspci -nnk -d 1022:8000
rdma dev show
dmesg --level=err,crit,alert,emerg
```

如果 QEMU 报告 vfio-user DMA 映射错误，首先确认已经使用
`memory-backend-memfd,share=on`，并确认服务端先于 QEMU 启动。更换服务端
二进制后应冷重启虚拟机，避免复用旧的 vfio-user 连接或设备状态。
