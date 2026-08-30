# 基于 ConnectX-6 的 MLNX backend 可行性分析与设计

> 修订 8（2026-08-30）：修正 libvfio-user quiesce/unregister 合同，冻结 `MREMAP_DONTUNMAP` 非移动alias算法，并补齐固定AdminQ slot、backend数据结构、QUERY、EQE、bind和ARM验证语义。

## 1. 结论

在 `debian-13-dev` 上用 `ens10np0` 对应的 `mlx5_0` 承载嵌套 VM 的 RDMA verbs，方案**可行**。将 `ens10np0` 的物理 MAC、选定 IP/GID、RoCE 类型、MTU和端口状态只读镜像给嵌套 VM 的 ERNIC Ethernet/RDMA function，可以消除原设计中最难处理的一层 GID 虚拟化：guest 自动生成的 MAC 派生 link-local GID和 IPv4 RoCE v2 GID均可与 CX6 对应项一致，server 只需按 `(GID, GID type)` 找到 host GID index，而不再改写 DGID。

这里的“透传”必须定义为**只读身份镜像（identity mirror）**，不是把物理网卡或 host 网络配置权交给嵌套 VM：

- `ens10np0/mlx5_0` 仍由 `debian-13-dev` 唯一持有和配置；
- 嵌套 VM 看见相同的物理 MAC 和 shadow IP/GID，但这些值均为只读镜像；
- guest ERNIC 普通 Ethernet frame 不直接进入物理网络；
- 真正的 RoCE packet 只由 server 创建的 CX6 QP 发送和接收；
- guest 修改 MAC/IP、PFC、GID type 等配置不能反向修改 host，且 identity tuple不匹配时禁止创建硬件 QP路径；
- MVP 一块物理 RDMA port 只服务一个 MLNX backend 实例。

在这个所有权模型下，shadow IP和相同 MAC不会在物理 L2 上形成两个竞争网络栈，因为 ERNIC 的 DHCP/identity frame只在 server 内终止，普通 Ethernet frame不会进入 `ens10np0`；同时 QPN/rkey/PD仍由 CX6 提供本地资源隔离。若未来要让 ERNIC netdev 同时承担通用 IP 网络，则必须另做 L2 proxy 或把专用 VF/SF 完整交给该 guest，不能沿用本方案的隔离假设。

不过，该方案不是“把 AdminQ 字段原样再次调用同名 `ibv_xxx`”这么简单。要达到正确、可隔离、可回收的实现，必须同时完成：

1. guest handle 与 host verbs object 的资源映射；
2. guest VA、server VA 与 HCA IOVA 的 MR 映射和生命周期管理；
3. guest QP handle 与线上真实 QPN 的分离；
4. host identity snapshot、guest 固定地址下发及 GID tuple 校验；
5. WQE opcode、flags、立即数、remote address/rkey 的完整转换；
6. host CQ 异步完成到 guest CQ ring/MSI-X 的回写；
7. reset、DMA unmap、未完成 WR 与资源销毁之间的同步。

当前仓库已经具备大部分框架和一份来自 QEMU PVRDMA 的 legacy verbs 实现，可作为主要代码基础；但 `verbs` registry 目前是 `NULL`，且现有公共路径仍有若干会直接阻断真实硬件工作的语义缺口。因此建议把它作为新的 `MLNX` backend 实现，而不是简单打开现有 `verbs` 开关。

### 1.1 冻结的首期范围

首期支持：

- 单个 server 实例绑定 `mlx5_0` port 1；
- RC QP；
- SEND/RECV；
- polling CQ，以及 one-shot 的 all/next-completion CQ notification；solicited-only notification延后二期；
- 普通 user MR，零拷贝访问 server 映射的嵌套 VM 内存；
- 从 `ens10np0` 选取一个显式配置的 IPv4 地址，以固定 DHCP lease 给 guest 配置为 `/32` shadow address；
- guest MAC默认且在 MVP 中强制镜像物理 MAC，保证自动生成的 link-local GID可与 host一致；
- identity allowlist至少包含物理 MAC派生的 link-local RoCE v2 GID和选定 IPv4-mapped RoCE v2 GID；RC QP数据路径强制使用后者；
- RoCE v2 GID按值和类型匹配到 host，禁止依赖易变化的数字 index；
- 协议 v21；包含 v20 QP handle/QPN 分离语义、无冲突的 identity feature bits和显式版本协商；
- 每个 guest QP拥有私有 host completion CQ，完成结果按 cookie路由到 guest send/recv CQ；
- 手工 verbs 建链，连接参数通过已有 virtio 管理网络交换。

首期 guest ERNIC netdev 是 **RDMA identity-only** 接口，普通 TCP/IP 管理流量继续走嵌套 VM 已有的 virtio NIC。后续再支持 RDMA READ/WRITE、atomic、UD/SRQ、GSI/MAD、透明 `rdma_cm` 和多租户。不要在首期宣称这些能力已经可用。

### 1.2 已冻结的 MVP 决策

| 决策项 | MVP 选择 |
| --- | --- |
| 连接模型 | 手工 `ibv_modify_qp()`；不支持 `rdma_cm`/MAD |
| 地址 | 显式 `ip=`，IPv4 RoCE v2，guest `/32`，无 gateway/DNS |
| 地址下发 | fixed-identity DHCP；部署脚本可作测试兜底，不定义第二套正式协议 |
| MAC | MVP 强制镜像 `ens10np0` 物理 MAC；不提供普通 L2出口 |
| GID | allowlist包含物理 link-local RoCE v2和选定 IPv4 RoCE v2；RC QP强制后者 |
| 协议 | 有效版本 `min(server, guest)`；MLNX要求 v21；保留 mesh bit 0，identity使用 bit 8/9 |
| identity 变化 | 启动快照；关键字段变化即 device fatal/fail-stop，reset/reprobe 后恢复 |
| 资源独占 | 每个 `(PCI BDF, port)` 只允许一个 MLNX backend session，并以进程锁强制 |
| 数据面 | RC SEND/RECV；每 QP私有 host CQ；host WR内部全 signaled，guest CQE仍遵守 guest signaling 语义 |
| WQ 信用 | unsignaled成功WR在host completion后归还；guest-visible WR必须等CQE实际写入guest CQ后才推进consumer |
| MR IOVA | v21 `CREATE_MR`显式携带 `start`和`iova`；不允许把两者隐式视为相同 |
| CQ 通知 | MVP只支持polling和one-shot all/next-completion；host CQ始终arm all，solicited-only延后二期 |
| 中断 | MLNX v21要求恰好取得3个 MSI-X用途向量；不支持单向量降级 |
| 端口就绪 | RDMA port按 `NEGOTIATING -> IDENTITY_READY -> ACTIVE`推进；IPv4 bind前不报告 ACTIVE |
| 通用网络 | ERNIC 不提供普通 L2/L3 出口；管理和参数交换使用 virtio NIC |
| 基础页大小 | MVP 仅接受 4 KiB guest/base page；server 启动及 PDIR 解析时强制校验 |

截至修订 7，主体实现所需的 P0 外部可行性实验均已通过，结论为 **Go，可以开始编码**。这里的“Go”不等于功能已交付；第 13.2 节的双机真实网络端到端和故障注入仍是实现完成后的发布门槛。

## 2. 现状与实机证据

本次实现规格复核起点为仓库提交 `3786625`，AdminQ 和共享 ring 细节见 `docs/test-data-transfer-adminq.md`。

### 2.1 CX6 环境

在 `debian-13-dev` 实测：

| 项目 | 结果 |
| --- | --- |
| netdev | `ens10np0`，UP/LOWER_UP，MTU 1500 |
| RDMA device | `mlx5_0` port 1 |
| PCI device | `0000:00:0a.0`，ConnectX-6 Dx `15b3:101d` |
| driver / firmware | `mlx5_core` / `22.39.3560` |
| nested VM memory backing | libvirt/QEMU `memfd` shared mapping |
| port | ACTIVE / LINK_UP / Ethernet |
| active MTU | 1024 |
| MAC | `b8:e9:24:53:8e:c0` |
| IPv4 | `10.232.167.93/26` |
| IPv6 | `fdbd:dc00:b008:19::93/64` 和 link-local |
| GID | RoCE v1/v2 均存在；IPv4 GID `::ffff:10.232.167.93` 当前为 index 2/3 |
| lossless 配置 | RX/TX pause on；PFC 8 个 priority 当前均 off |
| 核心能力 | RC、remote read/write、atomic、ODP、CQ event channel 均可用 |

当前进程的 `RLIMIT_MEMLOCK` soft/hard 均只有 8192 KiB。正式运行前必须在 systemd unit 或启动环境中提高到与允许注册的 guest MR 总量相匹配，推荐 `LimitMEMLOCK=infinity`，再由 backend 自身的 quota 限制实际用量。

### 2.2 身份镜像为何必须包含 MAC

当前嵌套 VM 使用默认 ERNIC MAC `72:6f:63:6d:2d:6e`，因此其 link-local GID 与 `mlx5_0` 的 MAC 派生 GID `fe80::bae9:24ff:fe53:8ec0` 不同。即使 RC数据面计划只使用 IPv4 RoCE v2，Linux netdev/RDMA core仍会先生成并尝试注册 link-local GID；在 `GID_BIND_REQUIRED` 下，独立 guest MAC产生的 tuple无法在 host精确匹配，会造成启动阶段 bind失败或 guest/host GID table不一致。

因此 MVP 将 ERNIC MAC也镜像为 `b8:e9:24:53:8e:c0`。这样，guest 自动生成的默认 link-local GID和 DHCP 后生成的 IPv4-mapped GID `::ffff:10.232.167.93` 都能在 host找到完全相同的 RoCE v2 entry。identity allowlist允许这两个 tuple完成 `CREATE_BIND`，但 `MODIFY_QP` 进入 RTR时只接受配置选定的 IPv4-mapped RoCE v2 GID，从而消除 link-local地址生成方式对 RC路由的歧义。

GID table 的数字顺序可能因内核、地址和 RoCE type变化，所以绑定必须按 `(16-byte GID, RoCE v1/v2)` 查找，不能假设 guest index 3永远等于 host index 3。server端 `activate_device()` 和 guest driver中已有的 synthetic `fe80::...` 初始 GID都必须移除；所有 MLNX GID只能来自 identity profile以及随后观察到的 guest `CREATE_BIND`。

这与 QEMU PVRDMA 的基本约束一致：Ethernet 地址变化驱动 guest GID table，并把 guest resource 一一映射到 backend ibdevice。区别是原 QEMU 方案由 guest 地址变化触发 host/backend 配置，本设计反向以专用 host port 为权威源，将物理 MAC和选定地址只读镜像给 guest，减少动态改写物理接口和地址冲突。

### 2.3 关键 MR 与 DMA lease 实验

2026-08-30 已在 `mlx5_0` 上执行真实 RC loopback spike：把 3 个独立 `memfd` 页分别 `MAP_FIXED` 到一段连续 alias VA，在 `alias + 0x123` 注册 9472 字节，并以 `iova=0x560000000123` 完成 SEND/RECV：

```text
ibv_reg_mr_iova(pd,
                alias + 0x123,
                9472,
                0x560000000123,
                IBV_ACCESS_LOCAL_WRITE)
=> two create/register/transfer/deregister/remap rounds PASS
```

接收数据逐字节等于发送数据，证明CX6可以把server进程中由多个独立 backing page拼成的共享映射注册为MR，并令HCA使用应用指定的guest IOVA。guest WQE中的 `addr`因而可以保持不变，NIC DMA最终落到server映射的guest pages，实现数据payload零拷贝。

同一程序把 IOVA 改为 `0x560000000124` 后，mlx5按预期以 `EINVAL`拒绝页内偏移不一致的注册。程序正常注销后重新建立alias并再次传输成功；故意不调用destroy而直接退出进程后，`rdma resource show mr/qp`与执行前一致，确认内核/驱动回收该进程的HCA对象。现有代码在 `rdma_rm_alloc_mr()` 中对 `host_virt` 增加 `guest_start & (PAGE_SIZE - 1)`，方向正确；新 backend 必须把“三种地址页内偏移相同”作为显式不变量校验。

同时从本仓库 `libvfio-user` 构建并运行 `test_sgl_get_put.py`、`test_dma_unmap.py`和`test_quiesce.py`，3/3通过。其公开契约明确规定：调用 `vfu_sgl_put()` 后不得再通过旧 iovec VA访问SGL；`vfu_dma_unregister_cb_t`返回类型为`void`，且要求回调返回前已经释放该DMA region的全部引用。能够返回`-1/EBUSY`并异步完成的是通过`vfu_setup_device_quiesce_cb()`注册的device quiesce callback，而不是DMA unregister callback。因此生产实现必须把SGL/iovec lease保持到MR撤销之后，并在unregister callback运行前完成全设备quiesce。

进一步使用vendored libvfio-user实际创建3个独立4 KiB DMA region，对`vfu_sgl_get()`返回的iovec逐页执行`MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP`，验证原VA与连续alias双向写入一致；随后按“拆除alias → `vfu_sgl_put()` → DMA_UNMAP”完成，原映射和libvfio-user bookkeeping均未失效。测试内核为Debian 13 `6.12.105+deb13-amd64`。这冻结了生产alias方案：只复制VMA映射，不得移动libvfio-user拥有的原VMA。

### 2.4 identity、MMIO排序与 guest probe 实验

2026-08-30 在 `rocm-ernic-e2e` 内用临时 veth + `rdma_rxe`验证identity生成规则：设置MAC `b8:e9:24:53:8e:c0`、`addrgenmode eui64`并添加 `10.232.167.93/32` 后，RDMA core生成：

```text
gid[0]=fe80::bae9:24ff:fe53:8ec0  RoCE v2
gid[1]=::ffff:10.232.167.93       RoCE v2
```

这确认 `/32` 不妨碍 IPv4-mapped GID生成；也确认必须显式把 guest ERNIC 的 IPv6地址生成模式设为EUI-64，不能依赖发行版可能启用的stable-secret默认值。临时 netdev/RXE设备已在实验结束后删除。

使用真实 vfio-user socket发出带`VFIO_USER_F_NO_REPLY`的BAR2 `PVRDMA_UAR_CQ_OFFSET` ARM write，紧接同地址read；server日志确认posted-write handler完整返回后才处理read，readback为`0xffffffff`。因此MVP采用“同BAR同offset的32-bit read，返回值忽略”作为posted-write flush；guest在read返回后再检查shared CQ producer。当前受测组合固定为QEMU 11.0.2和vendored libvfio-user commit `8fa68ba`；若未来transport实现或版本改变，必须重新运行真实guest ARM竞态测试，否则切换到显式ARM_ACK。

最后，临时构建了移除 `ib_device_ops.get_dma_mr` 且 `create_qp`只接受RC的guest模块，将vfio-user设备热插入 `rocm-ernic-e2e`；PCI `1022:8000`、netdev `ens10`和RDMA device `rocep0s10`均成功注册。随后卸载模块、热拔设备，未留下RDMA对象。这确认MVP禁用DMA MR和GSI不会阻塞guest probe。

### 2.5 已有可复用代码

| 能力 | 现有位置 | 可复用程度 |
| --- | --- | --- |
| backend vtable | `rdma_backend_ops.h`、`rdma_backend_core.c` | 高 |
| PD/MR/CQ/QP resource manager | `rdma_rm.c` | 中；需补强 key/handle/lifetime |
| legacy verbs 操作 | `rdma_backend.c` | 高；可拆成 MLNX ops |
| PDIR 到连续 server VA | `pvrdma_cmd.c::pvrdma_map_to_pdir()` | 中；需重做错误和撤销处理 |
| shared SQ/RQ/CQ ring | `pvrdma_cmd.c`、`pvrdma_dev_ring.c` | 高 |
| doorbell 到 WQE 消费 | `pvrdma_qp_ops.c` | 中；当前丢失部分 WQE 语义 |
| host WC 到 guest CQE | `rdma_backend.c::rdma_poll_cq()`、`pvrdma_qp_ops_comp_handler()` | 中 |
| completion thread | `rdma_backend.c::comp_handler_thread()` | 中；需纳入新 backend 生命周期 |

### 2.6 当前 HEAD 的待实现项

1. `rdma_backend_core.c` 中 `RDMA_BACKEND_TYPE_VERBS` 的 registry 项为 `NULL`，CLI 虽接受 `verbs:...`，实际无法初始化。
2. `rdma_backend_init_with_ops()` 不会调用 legacy `rdma_backend_init()`；现有 verbs 代码没有接入新 vtable。
3. server 报告 PVRDMA version 17，guest 将 `qp_handle=qpn`。真实 mlx5 QPN 不适合作为 guest resource table 下标；协议已经定义 v20 response，可分别返回 `qp_handle` 和 `qpn`，但 server 尚未使用。当前 guest会写入 `driver_version`，server却没有计算有效协商版本，不能仅把全局版本常量改成 21。
4. `rdma_rm_modify_qp()` 只向 backend 传少数字段；legacy verbs 又硬编码 MTU、timeout、retry、atomic depth 等值，不能忠实执行 guest 请求。
5. `rdma_backend_post_send()` 固定生成 `IBV_WR_SEND | IBV_SEND_SIGNALED`，忽略 guest opcode、send flags、立即数、invalidate key、RDMA remote address/rkey 和 atomic 参数。
6. `CREATE_MR` 强制给 guest 返回 `rkey=0xffffffff`，远端 RDMA READ/WRITE/atomic 无法工作。
7. guest driver 初始化会创建 `PVRDMA_MR_FLAG_DMA` 的 DMA MR，但该请求没有 PDIR/host VA；当前 RM 又没有把 MR flag 传给 backend。它不能直接调用 `ibv_reg_mr_iova(NULL, ...)`。
8. CQE 回写未完整复制 `imm_data` 等 WC 字段；unsignaled WR 也没有正确处理。
9. `pvrdma_map_to_pdir()` 会用`mremap()`移动libvfio-user拥有的原VMA并破坏其内部mapping bookkeeping，循环还没有检查每次返回值；生产实现必须改用7.2冻结的`MREMAP_DONTUNMAP`非移动alias，DMA mapping也不能继续使用全局固定256项数组。
10. MR 页映射在 DMA region 被 guest 撤销时没有与 HCA MR deregistration 建立强制顺序，存在 stale DMA mapping 风险。
11. standalone server 注释掉了原 QEMU `rdmacm-mux` 集成；GSI/MAD 与透明 `rdma_cm` 目前不成立。
12. GID 仍来自嵌套 VM 的虚拟 netdev，而 host QP 使用 `mlx5_0` 的 GID table；MLNX 模式尚未下发 shadow IP，也没有 fixed-identity DHCP/profile。
13. guest Ethernet driver 给 ERNIC netdev设置了 `IFF_NO_QUEUE`，RDMA driver据此把它误判为 dummy netdev并跳过 `CREATE_BIND`；MLNX 模式必须改用显式 capability决定是否发送 GID bind。
14. 当前 Ethernet TX 在 MLNX 模式处理完 DHCP/ARP/内置 CM逻辑后会丢弃普通 frame，没有通往 `ens10np0` 的 L2 数据面；因此不能把镜像 IP描述成通用网络 passthrough。
15. QP/CQ 销毁与 completion thread 缺少明确的 quiesce/drain 协议，存在 use-after-free 和 completion 丢失风险；若所有 host WR都强制 signaled，现有按 guest CQ深度创建 host CQ的模型还可能发生容量不足。
16. v20 下的 QP资源身份尚未重构：resource table、真实 QPN hash和 guest handle的键值混用；guest `qp_tbl` 写入时取模、删除时却直接索引。
17. 当前 fixed DHCP只在 loopback/TCP模式初始化，响应帧又复用 device MAC；不能直接用于物理 MAC镜像模式。
18. `QUERY_QP` 等路径仍把 packed guest ABI结构直接 cast成 host `libibverbs` 结构；布局、枚举和 mask必须显式转换，不能依赖两套 ABI偶然相同。
19. 当前 `mesh_flags.bit0` 已被 TCP backend用作 mesh enabled，不能再把 bit 0定义为 `IDENTITY_MIRROR`；否则旧 backend会被 guest误识别为 identity mirror。
20. guest driver除动态 `add_gid()` 外还初始化 synthetic GID；只移除 server synthetic GID不足以建立严格 tuple模型。
21. guest已有 async event ring和 `DEVICE_FATAL` handler，但 server缺少完整的 async EQE生产、ring满处理和 MSI-X vector 1通知路径，identity fail-stop无法闭环。
22. `QUERY_PORT`/device caps仍硬编码或放大部分能力，guest driver还无条件增加 `IB_PORT_CM_SUP`；这与 MVP不支持 `rdma_cm`、SRQ、atomic等边界矛盾。
23. guest `reg_user_mr()`分别取得 pinned VA `start`和 HCA IOVA `virt_addr`，但现有 `CREATE_MR`只发送 `start`；非默认 IOVA会被静默注册成错误地址。
24. guest provider以 shared ring consumer作为 WQ credit，而 server在 host post后立即推进 consumer；这会在 WR完成前提前归还 slot，使 guest可突破协商的 `max_send_wr/max_recv_wr`。
25. 当前 completion路径对每个 CQE都写 completion notification ring；普通 ARM和 solicited ARM的状态处理也未形成 one-shot arm-and-check合同，可能丢通知或填满通知 ring。
26. guest driver允许仅分配一个 MSI/MSI-X vector，但 v21 MLNX正确性依赖 vector 0/1/2分别承载 AdminQ response、async和 CQ通知；降级到单 vector时后两条路径不存在。
27. `CREATE_BIND.mtu`当前固定发送1024，语义是RDMA active MTU字节数；若按Ethernet MTU 1500校验，所有合法bind都会失败。
28. AdminQ response已有 `hdr.err`，但guest `rocm_ernic_cmd_post()`先读取 `REG_ERR`并把任意非零值折叠成 `-EFAULT`；destroy路径还会忽略失败并释放guest对象，无法实现精确的 `-EBUSY/-EOPNOTSUPP`合同。
29. `struct ibv_wc`不包含“该receive是否solicited”的标志；在一个host CQ混合SQ/RQ且始终arm all时，server无法从WC恢复solicited-only通知语义。
30. v20起guest handle与真实QPN分离，但userspace provider仍把CQE中的guest handle直接返回为 `wc.qp_num`，并截断到16 bit。

## 3. 目标与非目标

### 3.1 目标

- 嵌套 VM 继续使用现有 `rocm_ernic` kernel driver/provider 和 AdminQ ABI。
- server 通过标准 `libibverbs` 驱动 `mlx5_0`，首期不直接编程 mlx5 WQE/doorbell。
- server 在启动时从 `ens10np0/mlx5_0` 构造不可变的 `MlnxIdentityProfile`，guest 只读消费。
- guest物理 MAC、IPv4 RoCE v2 GID、port state 和 MTU与物理 port一致；link-local GID也必须能完成严格 tuple bind。
- guest 普通 MR 的 payload 数据不经过 server CPU copy。
- guest 不接触 host `ibv_context`、CQ buffer 或 UAR；硬件对象只存在于 server 进程。
- 每类资源均有确定的创建、失败回滚、销毁和 reset 顺序。
- 对不支持的 verb/opcode 明确返回错误，不能静默降级成 SEND。

### 3.2 非目标

- 首期不做 VF/SF PCI passthrough。
- 首期不使用 `mlx5dv` 绕过 libibverbs；这可作为性能优化，不是正确性前提。
- 首期不支持 live migration，因为真实 QP/MR/CQ 状态不能仅靠当前 AdminQ 状态透明恢复。
- 首期不承诺 GSI/MAD、multicast、XRC、MW、ODP、fast-reg、masked atomic。
- 不允许把 `host_virt == NULL` 当作 MLNX MR 创建成功；该容错只适用于软件 loopback。
- 首期不把 ERNIC netdev 的普通 Ethernet frame 转发到 `ens10np0`，也不向 guest 提供修改物理 port 配置的能力。

## 4. 总体架构

```text
嵌套 VM userspace
  ibv_create_qp / ibv_reg_mr / ibv_post_send / ibv_poll_cq
             │
             ▼
rocm_ernic guest driver/provider
  AdminQ control objects + shared SQ/RQ/CQ rings + BAR2 doorbells
             │ vfio-user shared memory/MMIO
             ▼
rocm-ernic server
  ens10np0/netlink ── identity manager ── optional BAR MAC + fixed DHCP + GID map
                              │
  AdminQ decoder ── resource manager ── MLNX backend ── libibverbs
       │                  │                    │
       │           guest handle map           ▼
       │                                  mlx5_0 / CX6
       │                                      │
       └──── guest CQE writer ◀── WC poller ◀─┘
```

backend 分成三层：

- **协议层**：只解析固定slot的AdminQ/PVRDMA ABI和shared WQE，做ABI版本、reserved、枚举、mask及WQE长度校验；AdminQ自身没有动态wire length。
- **资源层**：维护 guest handle、guest key、host object、依赖关系和状态机。
- **身份层**：从 `ens10np0/mlx5_0` 读取 MAC、地址、GID type/index、MTU和 port state，向 guest 提供一致的只读视图。
- **MLNX 层**：执行 `ibv_*`，只接收已经规范化的 backend 参数，不直接依赖 packed guest ABI。

推荐新增 `rdma_backend_mlnx.c`，注册为 `rdma_backend_ops_mlnx`。用户接口接受：

```text
--backend mlnx:device=mlx5_0,ethdev=ens10np0,port=1,roce=v2,ip=10.232.167.93,mirror-mac=on
```

`mlnx:`是唯一规范名称；如保留 `verbs:`，它仅作为采用同一key-value语法的deprecated alias，旧的 positional `verbs:<device>`脚本必须迁移，不能宣称无需修改即可兼容。内部只保留一套实现。MVP要求显式指定 `ip=`，并要求 `mirror-mac=on`；地址不存在、存在重复匹配、MAC不合法或没有对应 RoCE v2 GID时启动失败。`mirror-mac=off` 留作未来独立 identity/translation模式，MVP拒绝该配置。不要把数字 `gid-index` 作为持久配置；它只用于诊断。首期仍使用通用 verbs，名称 `MLNX` 表示经过 CX6 验证的硬件 backend，而不是依赖私有 mlx5 API。

解析规则固定为：`device`、`ethdev`、`port`、`roce`、`ip`、`mirror-mac`均为必填且每项只能出现一次；未知key、重复key、空值和尾随垃圾全部启动失败。`port`仅接受十进制1，`roce`仅接受`v2`，`mirror-mac`仅接受`on`。`verbs:`只作为完全相同语义的deprecated alias，不保留另一套默认值或旧硬件路径；启动日志必须输出规范化配置和选中的identity profile，但不得输出裸指针。

## 5. 资源与标识符模型

| guest 可见对象 | server handle | host 对象/标识 | 设计要求 |
| --- | --- | --- | --- |
| UC | `ctx_handle` | backend session | 不对应 `ibv_context *`；一个设备共用一个 context |
| PD | `pd_handle` | `ibv_pd *` | guest handle 与 host handle 分离 |
| MR | `mr_handle` | `ibv_mr *`、真实 lkey/rkey | 分别保存pinned guest VA、guest IOVA、server VA、长度、access和DMA lease |
| CQ | `cq_handle` | guest CQ ring与逻辑完成目标 | 不直接决定 host CQ容量；可被多个 QP引用 |
| QP | `qp_handle` | `ibv_qp *`、私有 `ibv_cq *`、真实 QPN | v21继承 v20语义，分离 guest handle 与线上 QPN；host完成按 cookie路由 |
| SRQ | `srq_handle` | `ibv_srq *` | 二期支持 |
| GID identity | frontend `(gid,type,index)` | `mlx5_0 (gid,type,index)` | gid/type 必须相同；index 单独映射 |

### 5.1 QP handle 与 QPN

这是必须先解决的 ABI 问题：

- `qp_handle` 只用于 AdminQ、BAR2 doorbell、server hash 和 guest `qp_tbl`；应是 `0..max_qp-1` 的稳定小整数。
- `qpn` 是 CX6 分配并在线路上使用的真实 QPN；应用通过 `ibv_qp->qp_num` 交换给远端。
- MLNX server/guest协商到 v21后使用 v20已定义的 `create_qp_resp_v2`，并填充独立 handle/QPN。
- v21复用现有64-bit CQE `qp`字段：低32 bit写 `qp_handle`，高32 bit写真实 `qpn`。kernel driver用低32 bit并做 `handle < max_qp`边界检查，userspace provider用高32 bit返回 `ibv_wc.qp_num`；禁止现有的 `% max_qp`或 `& 0xffff`截断。host `wc.qp_num`必须与cookie保存的真实QPN一致，否则进入fail-stop。

不得继续用真实QPN作为guest handle。`qp_handle/cq_handle`受BAR doorbell低24 bit限制，发布的上限必须小于 `2^24`。当前driver创建时用 `% max_qp`写表、释放时却直接以handle下标清表，真实QPN会导致错误索引甚至越界；实现MLNX前应一并修复为统一的边界校验和直接handle索引。

该规则对所有handle一致适用：QP/CQ/PD/MR/UC/GID index先检查`handle < advertised_limit`，再直接索引并校验对象类型、LIVE状态和generation；禁止`% max_*`、位掩码截断或用低16位兜底。BAR2的24-bit handle必须先与对应资源表上限比较。

cookie中的object generation用于丢弃迟到host WC；kernel可见QP的正常destroy还必须先发布完该QP已产生的guest CQE，再从 `qp_tbl`移除handle。现有CQ notification entry和async EQE都只有32-bit `info`而没有generation，因此凡是可能出现在CQNE/EQE中的CQ/QP/其他resource handle，destroy后都不能立即复用：资源表把它放入 `RETIRED` quarantine，并分别记录该对象最后发布的notification/async sequence；只有guest相应ring consumer以release越过全部序号、server以acquire确认后，handle才可重新进入free list。没有发布过异步entry的对象只需等待普通引用归零。reset/session close可以整体丢弃quarantine。若以后修改ABI在事件entry中加入generation，方可取消此延迟复用规则。

### 5.2 MR key 策略

首期推荐向 guest 返回 CX6 的真实 `lkey/rkey`：

- guest SGE中的 `addr`是应用注册的guest IOVA；默认 `ibv_reg_mr()`下它等于guest VA，`ibv_reg_mr_iova()`下可以不同。两种情况均无需server改写地址。
- guest SGE 中的 `lkey` 可直接用于 host `ibv_post_*`。
- 应用交换的 `rkey` 和 remote address 可直接供远端真实 QP 使用。

server 仍需维护 `lkey -> MR` 和 `rkey -> MR` 索引，但两者用途不同：本地 SGE必须通过 `lkey -> MR`检查 PD、范围和 local access；本地 MR被公布给 peer时，`rkey -> MR`用于生命周期、撤销和诊断。出站 RDMA WR携带的 remote address/rkey属于远端 HCA，server不得拿它查询本地 `rkey -> MR`，只能按原值提交并由远端 HCA校验。不能只依赖“key 难以猜测”；本地 HCA的 PD/key检查是最后一道防线，不替代本地 SGE协议校验。

若未来需要隐藏真实 key，可加入虚拟 key translation，但这要求发送端 backend 能解析远端虚拟 key，跨 host 时还需要控制面同步，不适合作为首期方案。

MVP没有MAD proxy，也不支持kernel GSI，因此MLNX模式的guest `ib_device_ops`不注册`get_dma_mr`，AdminQ对`PVRDMA_MR_FLAG_DMA`返回`-EOPNOTSUPP`，`create_qp`对GSI返回`-EOPNOTSUPP`，且不创建synthetic DMA MR。要支持guest kernel RDMA client或GSI，必须另行设计按DMA address动态注册/缓存guest pages，不能伪装成覆盖全部guest地址空间的普通host MR；legacy loopback/TCP行为不受此限制。

### 5.3 网络/RDMA 身份的所有权

| 配置 | guest 视图 | 实际生效位置 | guest 能否修改 |
| --- | --- | --- | --- |
| MAC | 与 `ens10np0` 相同 | CX6始终使用物理 MAC | 否 |
| IPv4 | 与选定 host IPv4 相同 | host 保持真实地址；guest 为隔离 shadow address | 不反向同步 |
| GID value/type | link-local与选定 IPv4 tuple均与 `mlx5_0` 相同 | CX6 GID table | 否；CREATE_BIND 只做 allowlist校验/引用 |
| GID index | 可与 host 不同 | server 保存映射 | guest index 可变，host index由匹配得到 |
| link/port state | host状态经v21 readiness状态机门控 | host + selected GID bind | 否 |
| Ethernet MTU | `ens10np0` MTU | host | 首期只读 |
| RDMA active MTU | `mlx5_0` active MTU | host HCA | 否 |
| PFC/pause/ECN/DCB | 可选只读 telemetry | host/交换网络 | 否 |
| QP traffic class/SL | guest QP 属性经校验后转换 | host QP | 在策略范围内可设置 |

PFC、pause、ECN、qdisc、RSS和物理队列数不应“复制成 guest 配置”。它们是 port/link 级策略，CX6 流量天然受 host 设置约束；guest 最多读取归一化状态，不能独立改变它们。

功能正确性不以PFC开启为前提，但无损网络策略会直接影响拥塞下的丢包、重传、尾延迟和吞吐；性能验收必须记录两端及交换机的PFC/ECN/DCB配置，未固定fabric策略的结果只能作为当前实验环境数据，不能外推为backend性能保证。

## 6. 控制面设计

### 6.1 backend 初始化

`mlnx_init()` 执行：

1. 通过 sysfs 验证 `ens10np0` 确实属于 `mlx5_0` port 1，禁止混配不同 port 的 netdev/ibdev。
2. 精确匹配 `mlx5_0`，调用 `ibv_open_device()`；禁止找不到时退化到第一块设备。
3. 用 rtnetlink 读取 `ens10np0` 的 MAC、IPv4/IPv6、prefix、MTU、operstate；用 verbs/sysfs 读取 GID value/type/index。
4. 按配置的 `(roce type, IP)` 选择唯一 IPv4-mapped GID。当前推荐 RoCE v2 + `10.232.167.93`，其 host index 当前是 3，但运行时必须重新按值匹配。
5. 交叉验证选定 IPv4推导出的 GID和物理 MAC派生的 link-local GID均与 HCA GID entry一致，构造只允许这两个 RoCE v2 tuple的不可变 `MlnxIdentityProfile`。
6. 创建一个 completion channel；每个 guest QP创建私有 host CQ并共享该 channel，guest CQ仅作为完成结果的逻辑路由目标。
7. 初始化 completion poller、资源锁、停止事件和 quota。
8. 以 host capability、identity profile 与虚拟 ABI 限制的交集生成 guest capability；不暴露未实现能力。

启动时还应检查 `RLIMIT_MEMLOCK`，若低于配置的 `max-registered-bytes`，直接失败并给出明确错误。MLNX模式必须调用`vfu_setup_device_quiesce_cb()`；没有该回调时不得启用直接SGL映射。启动时还要执行一次不涉及guest数据的4 KiB shared-memfd能力探测，确认当前内核支持对file-backed shared mapping执行`MREMAP_DONTUNMAP`且原/alias VA保持一致；失败即拒绝MLNX backend，不回退到移动原VMA。server固定公布vector 0/1/2三个MSI-X用途向量；guest完成v21 identity协商时必须检查 `nr_vectors == 3`，否则probe失败，不允许继续以单MSI/INTx运行。loopback/TCP仍可保留原有中断兼容策略。

### 6.2 身份下发和 guest 配置

采用现有 Ethernet function 完成配置闭环：

1. server 在 vfio-user device 可见前把 profile物理 MAC写入 BAR1 `ETH_MAC0/1`。该值只用于构造 guest identity，不授予 guest修改物理口的权限。
2. MLNX backend在 `REG_VERSION`报告server version 21，并在DSR caps的 `mesh_flags`发布server feature bits；guest先读取二者，确认版本不低于21且必需bit齐全，再写自己的 `driver_version`，最后写 `PVRDMA_REG_CTL=ACTIVATE`。该ACTIVATE写就是guest对server版本/features的显式接受，无需新增guest feature字段。
3. server只在ACTIVATE handler中读取一次 `driver_version`，计算并保存per-session `effective_version = min(21, driver_version)`，再次验证必需bit；`effective_version < 21`或缺bit时以设备错误拒绝ACTIVATE，保持NEGOTIATING。成功后版本与feature集合在该session内不可变，直到RESET或断连。AdminQ和doorbell在ACTIVATE成功前均拒绝。loopback/TCP继续使用各自现有协议语义。
4. 保留 device caps末尾 16-bit `mesh_flags` 字段的 ABI大小、offset和既有 bit 0含义，不做无版本保护的语义重命名。统一定义 `BACKEND_F_MESH = BIT(0)`、`BACKEND_F_IDENTITY_MIRROR = BIT(8)`、`BACKEND_F_GID_BIND_REQUIRED = BIT(9)`；低位其余 bit保留。guest driver不能再用 `IFF_NO_QUEUE` 判断是否跳过 bind；只有协商出 identity bits的 MLNX模式才必须发送 `CREATE_BIND`。双方不识别 v21或任一必需 bit时必须拒绝 MLNX模式，不能静默按 v20运行。
5. MLNX 模式创建 fixed-identity DHCP responder，只向该 ERNIC MAC提供 profile IPv4。固定返回 `/32`，不下发 default gateway/DNS；用途是触发 guest RDMA core生成与 host一致的 IPv4 GID，而不是承载普通 IP流量。DHCP server固定使用保留测试地址 `192.0.2.1`、独立 synthetic MAC和 `0xffffffff` infinite lease；响应 IP header的 source必须是 server IP，不能复用广播地址或 guest/device MAC。该 responder只处理 DHCP，不提供 ARP、ICMP或通用 IP服务。
6. guest部署配置以systemd-networkd的`IPv6LinkLocalAddressGenerationMode=eui64`（或等价netlink设置）固定ERNIC netdev地址生成模式，并在启用DHCP前生效；DHCP client加地址后，RDMA core调用driver `add_gid()`，driver发 `CREATE_BIND(gid,type,index)`。
7. server 对 `(gid,type)` 做精确匹配，保存 `guest index -> host index`，不添加、删除或改写 `ens10np0` 地址。
8. 后续 `MODIFY_QP` 用该映射填 host `sgid_index`，但 RC QP转 RTR时只接受 profile中选定的 IPv4-mapped RoCE v2 tuple；link-local tuple仅用于保持 Linux GID生命周期一致。DGID、QPN、rkey和 remote address保持对端公布的真实值。

RDMA port使用以下唯一的就绪状态机：

```text
NEGOTIATING --(v21/features/3 MSI-X ready)--> IDENTITY_READY
IDENTITY_READY --(selected IPv4 RoCE v2 CREATE_BIND succeeds)--> ACTIVE
```

`NEGOTIATING/IDENTITY_READY`期间 `QUERY_PORT`最多返回 INIT，不得因物理 port为 ACTIVE而提前返回 ACTIVE；只有选定 IPv4 tuple绑定成功后才返回 ACTIVE并产生一次 `PORT_ACTIVE`。ERNIC Ethernet carrier可以为 DHCP保持 UP，它不代表 RDMA port ACTIVE。选定bind被删除时立即阻止新QP进入RTR和依赖QP的新WR，将依赖QP转ERROR、完成有界host回收后释放mapping引用，令RDMA port回到INIT并上报 `PORT_ERR/GID_CHANGE`；该确定性停用完成后 `DESTROY_BIND`返回成功。物理identity变化仍按9.4执行device-fatal，不做在线恢复。

部署和测试脚本统一使用 `systemd-networkd`在 `ernic0`上启动 DHCP client，并等待选定 IPv4的 `CREATE_BIND`成功后再运行 RDMA测试；静态 `ip addr add`仅作为人工调试兜底，不定义第二套正式 identity协议。不要让 kernel RDMA driver直接管理 IP地址。

identity profile 至少包含：版本/generation、physical/guest MAC、选定 IPv4、guest prefix policy、RoCE type、允许的 GID tuple集合、QP选定 GID、物理 MTU、RDMA active MTU和 port state。physical node GUID、system image GUID、vendor/part ID可在 server内部用于诊断，但 guest继续暴露稳定的虚拟设备 identity；不能把整套物理 capability透传给 guest。gateway/DNS不是 RDMA identity 的组成部分，MVP不下发。

### 6.3 AdminQ 到 verbs 的转换

| AdminQ | MLNX 操作 | 关键处理 |
| --- | --- | --- |
| `QUERY_PORT` | `ibv_query_port()` + readiness gate | 返回物理状态/MTU的受限视图；selected IPv4 bind前不得为ACTIVE |
| `CREATE_UC` | 分配 session handle | 不为每个 UC 重开 HCA context |
| `CREATE_PD` | `ibv_alloc_pd()` | 建立 guest PD → host PD 映射 |
| `CREATE_MR` | 普通 MR：PDIR 映射 + `ibv_reg_mr_iova()`；MVP拒绝DMA MR | v21请求显式传 `start`和`iova`；失败整体回滚并返回真实 lkey/rkey；MR flag传到 backend并校验 |
| `CREATE_CQ` | 映射 guest CQ ring，创建逻辑完成目标 | 不在此处创建共享 host CQ |
| `CREATE_QP` | 创建私有 host CQ + 映射 guest SQ/RQ + `ibv_create_qp()` | host CQ深度覆盖该 QP最大 send/recv WR及余量；返回小整数 handle + 真实 QPN |
| `MODIFY_QP` | `ibv_modify_qp()` | 按 guest mask 显式转换全部首期支持字段 |
| destroy | 对应 `ibv_destroy_*()` / `ibv_dereg_mr()` | 普通销毁仅在依赖/inflight清零后执行；强制清理按逆序回收 |
| `CREATE_BIND` | identity tuple lookup | 精确校验gid/type/RDMA active MTU/VLAN，记录profile generation，只建立guest GID index → host GID index |
| `DESTROY_BIND` | 删除 frontend mapping/ref | selected bind触发确定性QP停用和端口降级；不删除 host 原有 IP/GID |

MVP命令集合固定如下，避免“未列出即默认兼容”：

| 命令 | MVP行为 |
| --- | --- |
| `QUERY_PORT` | 支持，返回readiness gate后的受限能力 |
| `QUERY_PKEY` | 仅支持port 1/index 0，返回`0xffff`；其他index返回`EINVAL` |
| `CREATE/DESTROY_UC`、`CREATE/DESTROY_PD` | 支持 |
| `CREATE/DESTROY_MR` | 仅普通user MR；DMA MR、FRMR和未列出的access返回`EOPNOTSUPP` |
| `CREATE/DESTROY_CQ` | 支持；`RESIZE_CQ`返回`EOPNOTSUPP` |
| `CREATE/MODIFY/QUERY/DESTROY_QP` | 仅RC及6.4明确的状态/mask；其他类型或mask返回`EOPNOTSUPP` |
| `CREATE/DESTROY_BIND` | 支持严格identity tuple映射 |
| `CREATE/MODIFY/QUERY/DESTROY_SRQ` | 返回`EOPNOTSUPP` |
| 未知命令 | 可解析但不支持时返回`EOPNOTSUPP`；ABI版本、枚举、handle或保留字段非法时返回`EINVAL` |

`activate_device()` 目前无条件插入 synthetic `fe80::...` GID，guest driver也预置了 synthetic GID。MLNX 模式必须同时移除两处行为，GID只能来自 identity profile驱动的 guest网络配置与随后观察到的 `CREATE_BIND`。同时扩展 backend GID API携带 `gid_type`，resource manager保存 value、type、guest index、host index和 profile generation。

v21定义 `create_mr_v2`：保持v20所有字段的offset不变，把独立的64-bit `iova`追加在现有 `nchunks`之后；不得插入旧字段中间。server和guest镜像头文件必须用static assertion固定v1/v2结构大小及全部关键offset。新guest连接v20 legacy backend时仍发送旧字段布局，尾部 `iova`被忽略并令legacy语义 `iova=start`；只有effective version 21的MLNX读取尾部字段。`start`表示被pin的guest userspace VA/PDIR页内偏移来源，`iova`是应用放入SGE的地址基准；server调用：

```c
ibv_reg_mr_iova(host_pd, host_alias, length, iova, access);
```

server同时校验 `offset(start) == offset(iova) == offset(host_alias)`，以及 `start + length`、`iova + length`不溢出；三者页内偏移不同统一返回 `-EINVAL`。旧版本请求没有 `iova`字段时不得进入MLNX backend。v21共享ABI首期明确只支持x86_64 little-endian；所有新增字段仍使用显式little-endian读写，不能直接依赖packed host struct cast。

### 6.4 QP 属性

不要沿用现有 hard-code。MVP定义 backend-neutral的 `RdmaBackendQpAttr`，字段全集固定为：

- state、current state、pkey index、port、access flags；
- path MTU、dest QPN、RQ/SQ PSN；
- AH/GRH：DGID、SGID index、hop limit、traffic class、flow label、SL；
- timeout、retry、RNR retry、min RNR timer；
- max destination/read atomic；
- QKey（UD 二期）。

协议层按 `attr_mask` 拷贝，MLNX 层再映射成 `struct ibv_qp_attr`。对于 guest 请求但 backend 不支持的 mask，返回 `-EOPNOTSUPP`，不能忽略后返回成功。

MR/QP/send access flags 也应逐位转换并验证依赖关系，例如 REMOTE_WRITE/REMOTE_ATOMIC 必须同时具备 LOCAL_WRITE；不要依赖 guest kernel 枚举值与 userspace `IBV_ACCESS_*` 恰好相同。

MVP只接受以下 RC状态迁移和字段集合；额外 mask或跳跃迁移返回 `-EOPNOTSUPP`/`-EINVAL`：

| 迁移 | 必需字段与约束 |
| --- | --- |
| RESET → INIT | state、pkey_index=0、port_num=1、qp_access_flags；只接受已发布的 access bit |
| INIT → RTR | state、AH/GRH、path_mtu、dest_qpn、rq_psn、max_dest_rd_atomic、min_rnr_timer；必须使用已绑定的 selected IPv4 SGID，DGID为IPv4 RoCE v2，QPN/PSN为24 bit，MTU不超过host active MTU |
| RTR → RTS | state、sq_psn、timeout、retry_cnt、rnr_retry、max_rd_atomic；PSN为24 bit，atomic depth不超过协商上限 |
| 任意有效态 → ERR/RESET | 停止新提交；ERR产生flush completion，RESET须完成quiesce后才成功 |

`CREATE_QP` response返回实际分配的SQ/RQ depth、SGE和inline上限；实际值不得超过已映射guest ring容量。MVP guest driver必须拒绝用户和kernel client创建UD/GSI/SRQ/XRC等对象，并不注册`get_dma_mr` op；同时不发布CM/MAD能力。支持这些kernel verbs属于后续独立阶段。

阶段1进一步固定AH/GRH策略：`is_global=1`、`port_num=1`、`pkey_index=0`、`sl=0`、`traffic_class=0`、`flow_label=0`、`hop_limit=1`、`dlid=0`、`src_path_bits=0`，SGID必须是selected IPv4 tuple；请求其他值返回 `-EOPNOTSUPP`，不静默改写。阶段1 MR access白名单精确为 `0`或`LOCAL_WRITE`，两者之外任何bit均返回`-EOPNOTSUPP`；QP `qp_access_flags=0`、`max_rd_atomic=0`、`max_dest_rd_atomic=0`。`REMOTE_READ/REMOTE_WRITE/REMOTE_ATOMIC`与相应QP access从阶段2/3按opcode开放，避免“本端不能发起但远端已能访问”的范围歧义。

### 6.5 创建失败与回滚

每个 create handler 按“映射 guest ring/pages → 创建 host object → 发布 guest handle”的顺序执行。handle 只有在所有步骤成功后才对 guest 可见。失败路径严格逆序撤销，response 不返回半初始化 key/QPN。

v21同时冻结AdminQ错误合同，避免现有 `REG_ERR` 折叠真实errno：

- `REG_ERR`只表示AdminQ传输、设备或response DMA失败，不承载某个合法命令的业务errno；
- 每个已解析命令都必须返回并由guest读取公共response header，包括destroy和原本的NOOP response；`resp.hdr.err`保存正的Linux errno值，0表示成功，guest统一返回 `-resp.hdr.err`；
- 现有response header没有独立sequence/length字段。v21把现有64-bit `hdr.response`冻结为guest生成的单调递增transaction cookie：guest发请求前写入request header同名字段，server原样回显；guest必须校验`hdr.response == expected_cookie`和`hdr.ack == expected_response_opcode`。AdminQ request/response slot始终分别映射并复制完整的`union pvrdma_cmd_req/resp`，wire上不存在可校验的动态length；两端必须先将发送union整体清零，再填充按`(opcode,effective_version)`选定的结构，并用`static_assert/BUILD_BUG_ON`固定union大小、成员大小及关键offset。接收端只按该静态表访问对应成员并校验其reserved字段为0，不能声称从wire读取或校验不存在的length；cookie/ack、ABI断言或reserved校验失败属于协议/传输错误；
- transaction cookie在一个session内从1开始、每次请求递增，0保留；64-bit回绕前必须reset session。AdminQ仍是单in-flight，但cookie可识别迟到IRQ/旧response；
- 未知或超出8-bit `hdr.err`可表达范围的内部错误对guest归一为 `EIO`，完整errno保留在server日志；
- frontend只有在response成功后才从本地handle table释放对象；destroy失败必须保留对象和依赖关系，不能忽略错误后继续free；
- server handler返回的普通负errno必须精确写入 `hdr.err`，不得把 `-EBUSY`、`-EINVAL`、`-EOPNOTSUPP`转换成通用成功或 `-EFAULT`。

### 6.6 capability 裁剪

MLNX 模式的 device/port capability必须取“CX6实测能力、guest ABI可表达能力、当前 backend已实现能力”三者交集：

- MVP只发布 RC、SEND/RECV、受支持的 CQ notification、实际 MR access和真实 MTU/port state；
- v21 MLNX guest不得无条件增加 `IB_PORT_CM_SUP`，因为首期没有 CM/MAD代理；
- `max_msg_sz`、`gid_tbl_len`、QP/CQ/MR数量和 SGE上限必须来自实际约束，不能沿用硬编码或人为放大值；
- SRQ、UD、atomic、inline、ODP、MW等在相应阶段完成前均不发布；
- host支持但 backend未实现的 capability同样必须隐藏，避免应用选择一条必然失败的路径。

MVP的`QUERY_PORT`返回规则固定如下，response先整体清零，禁止把`struct ibv_port_attr`直接cast到guest ABI：

| guest字段 | 唯一来源/取值 |
| --- | --- |
| `state` | 6.2 readiness gate：selected bind前最多INIT，成功后才ACTIVE |
| `max_mtu/active_mtu` | host port对应值，经显式枚举转换；无法表达时启动失败 |
| `gid_tbl_len` | 2，分别容纳profile允许的link-local和selected IPv4 tuple |
| `port_cap_flags` | 0；MVP不发布CM、MAD或其他port capability |
| `max_msg_sz` | `min(host.max_msg_sz, backend_max_transfer_bytes, UINT32_MAX)` |
| `pkey_tbl_len` | 1；仅index 0和`0xffff` |
| `bad_pkey_cntr/qkey_viol_cntr/lid/sm_lid/lmc/max_vl_num/sm_sl/subnet_timeout/init_type_reply` | 0 |
| `active_width/active_speed/phys_state` | 显式转换host值；guest ABI不能表达时返回0/UNKNOWN，不用相近值冒充 |

device caps同样先清零再逐项填写：`page_size_cap=4096`、`phys_port_cnt=1`、`gid_tbl_len=2`、`max_pkeys=1`、`mode=ROCE`、`gid_types=ROCE_V2`、`atomic_ops=bmme_flags=0`；`max_qp/max_cq/max_mr/max_pd/max_uar/max_mr_size/max_qp_wr/max_sge/max_cqe`分别取host能力、server配置quota、ABI/handle上限和已实现路径上限的最小值。`device_cap_flags`只保留已实现的port-active event和RC RNR NAK语义，其余未实现对象数量和扩展capability保持0，QP create result中的`max_inline_data`固定为0。所有quota必须是命名配置或命名常量，初始化日志输出最终值；不得在handler中使用另一套隐含默认值。

`QUERY_QP`只接受6.4状态机中出现的字段mask以及CAP；response整体清零后，从resource manager保存的规范化QP状态填写请求字段，从创建结果填写实际CAP。host `ibv_query_qp()`只用于一致性检查，不能把其结构直接cast为guest response；请求任何未支持mask返回`-EOPNOTSUPP`，非法handle或状态返回`-EINVAL`。返回值必须与最近一次成功的`MODIFY_QP`和`CREATE_QP`结果一致。

### 6.7 backend 接口与所有权合同

现有 vtable不能直接承载上述模型，必须按以下合同调整：

- `RdmaBackendCQ`只表示 logical guest CQ，保存guest ring、arm状态、关联QP集合和引用计数；`create_cq()`不创建host CQ。
- `RdmaBackendQP`独占一个physical host CQ和一个host QP；创建顺序为host CQ→host QP→加入logical CQ关联表，销毁严格逆序。
- `post_send/post_recv`接收规范化WR并返回 `POSTED`、`RETRY`或`FATAL`。只有 `POSTED`转移cookie所有权；`RETRY`保留WQE和cookie并等待completion释放credit后重试；`FATAL`触发QP ERROR和错误/flush completion。
- 每个posted WR持有QP、目标logical CQ及所有local MR引用，直到内部host completion完成；destroy不得绕过这些引用。
- `add_gid()`必须携带gid value、gid type、guest index及profile generation，不再依赖ifname或隐含index。
- completion poller是唯一允许调用 `ibv_get_cq_event()/ibv_poll_cq()/ibv_req_notify_cq()`的线程；CQ `POLL` doorbell只向该线程投递wake/drain请求，不能从MMIO线程并发poll同一个host CQ。logical CQ的 `publish_lock`只负责串行化guest CQE producer和通知状态，不能替代physical host CQ的单owner规则。

接口返回普通负errno的控制面操作不得转化为成功；数据面返回值使用上述三态，避免把暂时的host SQ/RQ满误报为guest WR永久失败。

实现时以如下v2形态替换现有含糊的`void destroy/post`接口；具体结构体可拆文件，但参数和所有权语义不得变化：

```c
enum RdmaPostResult { RDMA_POSTED, RDMA_RETRY, RDMA_FATAL };

struct RdmaBackendOpsV2 {
    int  (*init)(RdmaBackendDev *, const RdmaBackendConfig *);
    void (*fini)(RdmaBackendDev *); /* 幂等，内部先stop/join poller */
    int  (*query_port)(RdmaBackendDev *, uint8_t, RdmaBackendPortAttr *);
    int  (*create_pd)(RdmaBackendDev *, RdmaBackendPD *);
    int  (*destroy_pd)(RdmaBackendPD *);
    int  (*create_mr)(RdmaBackendPD *, const RdmaBackendMrAttr *,
                      GuestMemoryLease *, RdmaBackendMR *);
    int  (*destroy_mr)(RdmaBackendMR *);
    int  (*create_cq)(RdmaBackendDev *, const RdmaBackendCqAttr *,
                      RdmaBackendCQ *); /* logical CQ only */
    int  (*destroy_cq)(RdmaBackendCQ *);
    int  (*create_qp)(RdmaBackendPD *, const RdmaBackendQpInitAttr *,
                      RdmaBackendQP *, RdmaBackendQpInitResult *);
    int  (*modify_qp)(RdmaBackendQP *, const RdmaBackendQpAttr *, uint64_t mask);
    int  (*query_qp)(RdmaBackendQP *, RdmaBackendQpAttr *, uint64_t mask);
    int  (*destroy_qp)(RdmaBackendQP *);
    int  (*add_gid)(RdmaBackendDev *, const RdmaBackendGidAttr *,
                    RdmaBackendGidBinding *);
    int  (*del_gid)(RdmaBackendGidBinding *);
    enum RdmaPostResult (*post_send)(RdmaBackendQP *,
                    const RdmaBackendSendWr *, RdmaWrCookie *, int *fatal_errno);
    enum RdmaPostResult (*post_recv)(RdmaBackendQP *,
                    const RdmaBackendRecvWr *, RdmaWrCookie *, int *fatal_errno);
};
```

`RdmaBackendMrAttr`必须包含`start/iova/length/access/mr_flags`，`RdmaBackendQpInitAttr/Result`包含请求与实际SQ/RQ depth、SGE、inline、logical send/recv CQ，`RdmaBackendGidAttr`包含value/type/guest_index/profile_generation。所有create由调用者预分配空对象，成功才转为LIVE；`create_mr`成功后backend拥有lease直到`destroy_mr/fini`，失败时lease仍归调用者；destroy成功才清空private data。`POSTED`后backend拥有cookie，`RETRY/FATAL`时仍由调用者拥有；`fatal_errno`只在FATAL有效。

为避免协议层、资源层和MLNX层并行实现时自行补字段，v2结构的最小完整合同冻结为：

| 结构 | 必须字段与方向 |
| --- | --- |
| `RdmaBackendPortAttr` | 输出：state、max/active MTU、gid/pkey table长度、port flags、max message、物理状态/width/speed；值严格按6.6表生成 |
| `RdmaBackendMrAttr` | 输入：start、iova、length、access、mr_flags；均为host-endian规范化值 |
| `RdmaBackendCqAttr` | 输入：guest CQ handle、ring depth、ring lease；输出对象保存arm state、关联QP集合和发布sequence |
| `RdmaBackendQpInitAttr` | 输入：QP type、PD、logical send/recv CQ、请求send/recv WR和SGE depth、inline、`sq_sig_all`、guest ring lease |
| `RdmaBackendQpInitResult` | 输出：实际send/recv WR和SGE depth、inline、真实QPN；不得大于guest已映射ring或公布capability |
| `RdmaBackendQpAttr` | state/current-state、pkey/port/access、path MTU、dest QPN、RQ/SQ PSN、AH/GRH、timeout/retry/RNR、atomic depth、CAP及保留给UD二期的QKey；每个字段只在对应内部mask置位时有效 |
| `RdmaBackendSendWr` | opcode、guest send flags、SGE数组/数量、immediate/invalidate、remote address/rkey、atomic参数；阶段1只允许8.2白名单 |
| `RdmaBackendRecvWr` | SGE数组和数量；不含opcode、remote或send flag字段 |
| `RdmaWrCookie` | guest wr_id、QP/CQ指针及generation、真实QPN、SQ/RQ方向、单调slot sequence、guest-visible/signaled标志和持有的MR引用集合 |
| `RdmaBackendGidAttr/Binding` | 输入value/type/guest index/profile generation；输出host index和对不可变identity profile的引用 |

所有数组均由`pointer + count`表示，count在解引用前按公布上限校验；callee不得保留调用者临时数组。mask使用独立的`RdmaBackendQpAttrMask`位定义，协议枚举与`IBV_QP_*`均通过显式switch转换。查询输出、create result和`fatal_errno`由callee写入；其他输入为只读。以上结构定义应集中在单一公共header，guest packed ABI不得出现在MLNX backend header中。

## 7. 内存与零拷贝设计

### 7.1 地址关系

设：

- `U`：guest被 pin的 userspace起始 VA，即 v21 `CREATE_MR.start`；
- `G`：应用可见的 HCA IOVA，即 v21 `CREATE_MR.iova`，可与 `U`不同；
- `H`：server将 PDIR中各 guest page拼接后的页对齐连续 VA；
- `O = U & (PAGE_SIZE - 1)`。

注册调用为：

```c
ibv_reg_mr_iova(host_pd, H + O, guest_length, G, access);
```

于是 HCA 访问 guest SGE 地址 `A` 时，实际 DMA 到：

```text
(H + O) + (A - G)
```

该地址正是相应 guest page 在 server 中的共享映射。`H + O`与`U`必须具有相同页内偏移；`G`只参与HCA IOVA换算，不用于定位PDIR页。SEND/RECV/RDMA payload不需要 server copy；server只读取WQE并写CQE。

### 7.2 映射实现要求

当前 `pvrdma_map_to_pdir()` 可以作为原型，但生产实现应改为一个显式 `GuestMemoryLease`：

- PDIR和PTE的SGL只在解析期间持有，复制并校验PTE值后立即成对`vfu_sgl_put()`；每个data page保存guest DMA address、原始SGL/iovec和长度，直到MR销毁。每次成功的`vfu_sgl_get()`必须恰好对应一次最终`vfu_sgl_put()`；
- 校验page count、算术溢出、4 KiB页对齐、重复页、DMA region权限和最大MR大小；每个data-page请求必须得到页对齐、长度恰为4096的iovec，并确认其所属`vfu_dma_info.page_size==4096`，否则返回`-EOPNOTSUPP`；
- 用`mmap(NULL, rounded_length, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)`预留连续区间`H`。对第`i`个data-page原始iovec执行`mremap(src, 4096, 4096, MREMAP_MAYMOVE|MREMAP_FIXED|MREMAP_DONTUNMAP, H+i*4096)`；返回地址必须严格等于目标。该操作只增加alias，严禁使用旧的`mremap(..., old_size=0, ...)`或任何会移动/替换libvfio-user原VMA的方式；
- 任一步失败时先`munmap()`已建立的alias/预留区，再对已取得的原始SGL逐项`vfu_sgl_put()`；不得对原始iovec调用`munmap()`。成功销毁顺序固定为`ibv_dereg_mr()`→`munmap(H, rounded_length)`→全部`vfu_sgl_put()`；
- MVP固定guest/base page为4096字节；server启动时要求`sysconf(_SC_PAGESIZE)==4096`并通过6.1的`MREMAP_DONTUNMAP`能力探测，PDIR page shift/entry必须为4 KiB对齐，否则拒绝启动或请求；hugetlb/mixed base-page支持留待后续；
- MR存活期间保持SGL和原始iovec有效；HCA MR仍存活时严禁拆alias或调用`vfu_sgl_put()`，put之后严禁访问alias或旧iovec VA；
- 禁止固定大小全局 mapping 表，改为 per-device、带锁、按区间索引的动态结构；
- reset 时先停 doorbell/WQE 消费，再销毁 hardware objects，最后释放 guest mappings。

DMA map/unmap与device quiesce的唯一合同为：

1. 初始化时注册`vfu_device_quiesce_cb_t`。libvfio-user在DMA_MAP、DMA_UNMAP、reset等需要静止设备的命令前调用它；该回调不携带待处理region，因此必须先全局冻结新AdminQ、doorbell和SGL访问，不能按“可能只是MAP”猜测并继续硬件DMA。
2. MVP不支持存在LIVE HCA对象时动态改变VFIO DMA map。若尚未ACTIVATE且没有QP/MR/inflight lease，quiesce callback可同步返回0；否则统一进入session fail-stop，设置`errno=EBUSY`并返回-1，由清理线程按10.1强制顺序令QP ERROR、drain/destroy QP、dereg全部MR、拆alias、执行全部`vfu_sgl_put()`，直到不再持有任何DMA region引用。该保守策略同时覆盖无限期posted RECV，避免假设硬件QP能够暂停后原样恢复。
3. 强制静止完成后，清理线程调用`vfu_device_quiesced(vfu_ctx, 0)`。libvfio-user在该调用返回前处理原pending命令并调用相应DMA register/unregister/reset callback；回调链结束后该session仍保持fail-stop，完成pending命令响应后关闭，不能因libvfio-user内部unquiesce而重新接受WR。
4. `dma_unregister_cb()`返回类型为`void`，绝不能返回`EBUSY`。它按`info->iova`区间确认mapping tree已无lease；若仍发现引用，视为清理不变量破坏，在回调内同步完成剩余dereg/alias teardown/put并标记device fatal，返回前保证不再持有该region引用。
5. ACTIVATE前的DMA_MAP由register callback加入region后可以继续初始化；ACTIVATE后的任何DMA_MAP/UNMAP、callback失败或5秒内无法quiesce均关闭session。MVP部署因此禁止运行期RAM hotplug、vIOMMU remap和live migration；这些能力必须先设计可暂停/恢复的hardware object协议。

P0已完成：多backing page、非页对齐注册、真实RC DMA、注销/VA重用、非法offset和进程异常退出均通过；libvfio-user的SGL/unmap/quiesce测试以及实际SGL iovec的`MREMAP_DONTUNMAP` alias/unmap生命周期均通过。实现阶段仍须增加端到端故障注入，验证上述`quiesce EBUSY -> drain/dereg/alias teardown/put -> device_quiesced -> void unregister确认零引用 -> session close`顺序；这是实现验收项，不再是架构可行性的前置阻塞。

### 7.3 一致性与屏障

- guest 写 WQE 后以 release 语义更新 producer；server 以 acquire 语义读 producer 后再读 WQE。
- server完整写CQE后以release语义更新CQ producer，guest以acquire语义观察；guest推进CQ consumer时使用release，server判断空间时使用acquire。SQ/RQ的server consumer与guest producer同样双向配对。
- 共享ABI保留既有宽度的环计数；两端各自维护不回绕的64-bit私有sequence，并按相邻值把共享计数展开到对应generation。任意producer相对consumer的前进距离超过ring depth、倒退或跨越多于一个可判定generation都视为损坏并fail-stop，不能仅取模后继续。
- host WC 到达即表示对应 DMA 的 verbs 可见性条件满足，但仍要保证 server CPU 写 guest CQE 的顺序。
- 首期不启用 `IBV_ACCESS_RELAXED_ORDERING`。

## 8. 数据面设计

### 8.1 RECV

BAR2 RECV doorbell 后：

1. 按 batch 从 guest RQ ring 读取 WQE；
2. 校验 `num_sge`、每个 SGE 的 lkey、地址范围、长度和所属 PD；
3. 分配内部 `wr_cookie`，保存 guest `wr_id`、guest QP handle、目标 guest CQ、资源 generation；
4. 构造 `ibv_recv_wr`，SGE 地址/lkey保持 guest 所见的 IOVA/真实 lkey；
5. `ibv_post_recv()`返回 `POSTED`后只推进server私有 `rq_submit_seq`，不推进shared consumer；内部host completion到达并完成CQE处理后，才按序推进shared RQ consumer归还guest credit；`RETRY`保留当前WQE，`FATAL`令QP进入ERROR。

### 8.2 SEND

现有 backend API 参数不足。MVP把完整规范化 WR传给 MLNX backend：

```c
struct RdmaBackendSendWr {
    enum ibv_wr_opcode opcode;
    uint32_t guest_send_flags;
    uint32_t imm_data;
    uint32_t invalidate_rkey;
    struct ibv_sge *sge;
    uint32_t num_sge;
    uint64_t remote_addr;
    uint32_t rkey;
    uint64_t compare_add;
    uint64_t swap;
    /* UD fields omitted in phase 1 */
};
```

首期 opcode 矩阵：

| guest opcode | 首期 | host opcode |
| --- | --- | --- |
| SEND | 是 | `IBV_WR_SEND` |
| SEND_WITH_IMM | 二阶段 | `IBV_WR_SEND_WITH_IMM` |
| RDMA_WRITE | 二阶段 | `IBV_WR_RDMA_WRITE` |
| RDMA_WRITE_WITH_IMM | 二阶段 | `IBV_WR_RDMA_WRITE_WITH_IMM` |
| RDMA_READ | 二阶段 | `IBV_WR_RDMA_READ` |
| atomic | 三阶段 | 对应 atomic opcode |
| LOCAL_INV / SEND_WITH_INV / REG_MR | 暂不支持 | `-EOPNOTSUPP` |

MVP把所有 host WR都置 `IBV_SEND_SIGNALED`，便于释放 `wr_cookie`并归还WQ credit；但只有 `sq_sig_all`或guest WQE带`SIGNALED`时才向guest CQ ring发布成功CQE。MVP guest send flag白名单精确为`0`或`SIGNALED`；`SOLICITED`随solicited notification一起延后，`FENCE`、`INLINE`、`IP_CSUM`及未知bit均返回`-EOPNOTSUPP`。host错误和flush无论guest是否请求SIGNALED都必须报告。guest provider尽量同步拒绝，server仍做防御性校验，禁止静默忽略或改写成SEND。

为避免 guest CQ深度与内部 completion数量不一致，每个 guest QP创建私有 host CQ，精确请求`max_send_wr + max_recv_wr`个CQE，并检查provider实际返回容量足以覆盖两条队列全部outstanding WR；flush completion替代原WR completion，不另加未定义的`flush_slack`。如果总和超过host `max_cqe`或backend quota，则按比例/配置下调并把实际depth返回guest，无法至少提供各1个slot时创建失败。host CQ不与 guest CQ一一对应：cookie记录目标 guest send/recv CQ，completion poller据此路由。后续可优化为周期性插入 signaled WR并批量回收此前的 unsignaled cookie，但不属于 MVP。

`IBV_SEND_INLINE` 首期不透传：guest capability 将 `max_inline_data` 报为 0。后续若支持，应由 server 从共享 MR 读取 payload并构造 host inline WQE，这会产生 CPU copy，不能称为零拷贝。

### 8.3 host completion 到 guest CQE

completion thread 对每个 QP私有 host CQ执行：

1. `ibv_get_cq_event()`；
2. 立即重新 arm host CQ；该动作不读取也不依赖guest logical CQ的arm状态；
3. 循环 `ibv_poll_cq()` 直到空；
4. 用 host `wc.wr_id` 查找 `wr_cookie`；
5. 按下表转换status、opcode、byte_len、imm_data、src_qp、wc_flags、vendor_err；
6. 对guest应见的completion尝试写guest CQE；ring满则转入pending且冻结该QP新提交；unsignaled成功completion只做内部回收；
7. guest应见的slot只有在CQE真正发布后才标记完成并按连续顺序推进shared SQ/RQ consumer；unsignaled成功slot在内部WC完成后即可推进；
8. 实际发布CQE且logical CQ处于one-shot `ARM_ALL`时，写一次CQ notification ring、清除arm并触发MSI-X vector 2；
9. WC到达即释放local MR引用；cookie、QP和logical CQ引用在CQE发布或unsignaled内部回收完成后释放。

MVP WC转换表固定为：

| host WC | guest CQE |
| --- | --- |
| `IBV_WC_SUCCESS` + SEND | `SUCCESS`、`WC_SEND`，`byte_len=0`、flags=0 |
| `IBV_WC_SUCCESS` + RECV | `SUCCESS`、`WC_RECV`，复制`byte_len/src_qp`，flags=0 |
| `IBV_WC_LOC_LEN_ERR/LOC_QP_OP_ERR/LOC_PROT_ERR/WR_FLUSH_ERR/LOC_ACCESS_ERR/REM_INV_REQ_ERR/REM_ACCESS_ERR/REM_OP_ERR/RETRY_EXC_ERR/RNR_RETRY_EXC_ERR/FATAL_ERR` | 映射到同名guest status，opcode仍按cookie中的SEND/RECV方向产生 |
| 其他status | `WC_GENERAL_ERR`，保留host `vendor_err` |

阶段1没有immediate/invalidate/GRH metadata，故`imm_data=0`、guest `wc_flags=0`；任何意外host opcode/flags均视为backend不变量破坏并进入QP fatal，不能伪装成普通RECV。

MVP logical guest CQ只保存 `CLEAR/ARM_ALL`。guest `REQ_NOTIFY_CQ`先对BAR2 `PVRDMA_UAR_CQ_OFFSET`写32-bit ARM doorbell，执行`mmiowb()`，再从同一BAR同一offset做32-bit read并忽略返回值；不得使用未定义的“状态寄存器”。vfio-user posted-write实测按write handler完成、随后read handler的顺序执行，server在`publish_lock`下设置ARM_ALL后才结束write handler，因此readback完成时与CQE发布已排序。随后frontend按 `IB_CQ_REPORT_MISSED_EVENTS`语义，以acquire重新读取shared CQ producer并与本地consumer比较：已有CQE则由guest本地返回missed，空环则保证后续首个CQE触发通知。普通server启动无法独立生成guest MMIO序列，因此不得声称存在本地“启动自检”；MVP支持矩阵固定为已验证的QEMU 11.0.2和vendored libvfio-user commit `8fa68ba`，并在任一transport版本变更时运行13.1的真实guest ARM竞态集成测试。该测试失败时必须拒绝该组合或先增加显式arm-ack寄存器/AdminQ命令，不能靠延时猜测。

发布CQE时仅在ARM_ALL首次满足时写一个CQ notification ring entry，将状态原子恢复为CLEAR，再触发vector 2；未arm时只写CQE。MVP frontend拒绝solicited-only arm并不发布相应capability，因为标准 `struct ibv_wc`没有solicited标志，单个混合SQ/RQ的private host CQ无法可靠恢复该属性。二期若要支持，必须把每QP的host send/recv completion拆到独立physical CQ（recv CQ按solicited-only arm）或采用能够提供该元数据且经验证的mlx5专用接口；不得从opcode或guest send flag猜测。

guest主动写CQ `POLL` doorbell时，MMIO线程只向唯一completion poller投递该logical CQ的wake/drain请求；poller以QP关联表快照drain全部关联private host CQ，不得只poll创建CQ时的单个host CQ，也不得由MMIO线程直接poll。一个host CQ event可能对应多条WC，必须批量处理并正确 `ibv_ack_cq_events()`。notification ring满或vector 2不可用属于设备级通知失效，按fail-stop路径处理，不能丢entry后继续运行。

### 8.4 backpressure

- 每个SQ/RQ分别维护64-bit单调递增的 `submit_seq`、`complete_seq`和按slot排列的inflight ledger。按7.3把共享producer展开后，doorbell只扫描 `[submit_seq, guest_producer_seq)`；`POSTED`推进`submit_seq`，host completion只标记对应slot完成，shared consumer仅跨过从`complete_seq`开始连续完成的slot。这样一个posted但未完成的WR始终占用协商WQ credit，server也不会重复提交同一WQE。
- host `ibv_post_*()`因容量暂满返回 `ENOMEM/EAGAIN`时映射为 `RETRY`：不消费WQE、不生成guest CQE，在任一内部completion释放host credit后继续。参数、状态或设备错误才是 `FATAL`，由QP ERROR/flush路径收敛。
- guest一次提交链表时，frontend在首个失败WR处设置 `bad_wr`，但只要此前已有WQE成功进入shared ring，就必须先release producer并doorbell该成功前缀，再把错误返回调用者；不得因链尾失败把已排队前缀永久留在无doorbell状态。server仍逐项维持 `POSTED/RETRY/FATAL`合同。
- guest CQ ring满时不能丢WC。已poll且guest应见的WC进入per-QP pending-CQE ledger；容量固定为该QP的`max_send_wr + max_recv_wr`，因为任一unsignaled WR也可能以error/flush形式变成guest-visible completion。一个QP存在pending CQE后立即停止扫描/提交其新guest WQE，但completion poller仍持续drain host CQ。对应shared SQ/RQ consumer只有在该CQE真正写入guest CQ后才推进；“仅安全进入pending”不足以归还WQ credit。若ledger仍耗尽即内部不变量破坏，报告CQ/device fatal并走允许丢弃guest CQE的强制清理；不得继续接收WQE或等待更多flush CQE进入满队列。
- guest unsignaled成功WC不占guest CQ ring，完成内部资源释放后可直接归还WQ credit；任何错误/flush WC仍需要guest CQE和上述backpressure。
- 每个doorbell的WQE batch有上限；使用event loop continuation，避免一个QP饿死其他QP。

## 9. GID、RoCE 路由与连接建立

### 9.1 身份镜像后的 RC 路径

身份镜像把连接参数简化为真实物理值：

```text
local GID  = ens10np0/mlx5_0 selected GID
local QPN  = ibv_create_qp() returned QPN
local rkey = ibv_reg_mr_iova() returned rkey
local addr = guest API IOVA registered as HCA IOVA
```

两端应用交换这四类值后，guest `MODIFY_QP` 中的 DGID已经是远端真实 GID，dest QPN已经是远端真实 CX6 QPN，RDMA WQE中的 remote address/rkey也是真实 HCA 参数。server 不需要维护 peer GID translation service。

本地 SGID 的 guest index 与 host index 仍可能不同。`CREATE_BIND` 建立的 tuple mapping负责把 guest index解析成 host index；若找不到完全相同的 GID/type，QP 转 RTR 必须失败，不能回退到 index 0。

MVP实机验收固定为两块处于同一 L2/VLAN的真实 CX6 port，GRH `hop_limit=1`，不承诺跨三层路由。单机 HCA loopback只用于早期调试，不能作为网络路径验收结果。连接参数通过已有 virtio管理网交换，控制消息采用显式版本和网络字节序，至少包含 `{gid, gid_type, qpn, psn, path_mtu}`；MR准备后另交换 `{addr, rkey, length}`。backend不推断、自动配对或重写 peer参数。

### 9.2 shadow IP与 MAC镜像为何不会冲突

MVP 中二者不是两个物理网络参与者：

- host `ens10np0` 是物理 L2/L3 和 CX6 GID table 的唯一 owner；
- guest ERNIC netdev 是隔离的 shadow interface；shadow IP只用于 Linux RDMA core 的 GID生成和应用 bind，MAC镜像用于确保默认 link-local GID也能严格匹配；
- DHCP/identity frame 在 server 内终止；普通 guest Ethernet frame 不发到 `ens10np0`；
- 线上 RoCE frame 全部由 `mlx5_0` 生成，使用物理 MAC/IP/GID和各 QP 的真实 QPN。

因此网络上只有一个物理 IP/MAC owner，guest镜像身份不会出现在物理 L2。风险出现在未来把 guest frame raw-forward 到物理口时：host 与 guest可能同时回应 ARP/NDP、使用相同 TCP/UDP端口或产生不一致 conntrack。若启用通用 Ethernet，必须切换到下列模型之一：

- **独占模式**：给 backend 专用 VF/SF/port，host 不在该 function 上运行普通 IP stack；或
- **代理模式**：guest 使用独立 MAC/IP，server 做 L2/L3/CM proxy，同时恢复 GID translation。

不能在 identity mirror 模式下直接增加 AF_PACKET/TAP raw forwarding。

现有 ARP responder 使用 `dev->mac_addr` 作为 server source MAC。物理 MAC镜像后，这会与 guest client MAC相同；MLNX identity-only模式应关闭该通用 ARP responder，仅保留使用独立 synthetic身份的 DHCP channel。

### 9.3 地址下发策略

MVP 固定使用 IPv4 RoCE v2：

- MAC通过现有 BAR寄存器下发并固定为物理 MAC；guest写 MAC不改变 host且不更新当前 profile；
- IP通过固定 DHCP lease配置；测试环境可用静态命令兜底；
- 默认用 `/32`、不下发 gateway/DNS，避免 guest 把普通 IP流量误路由到 identity-only netdev；
- DSR 只公布 RoCE v2，guest 与 host 的 GID type必须相同；
- host GID index按 `(gid,type)` 动态解析，当前的 index 3只是观测值；identity allowlist中的 link-local tuple可完成 bind，但 RC QP只选择 IPv4 tuple。

如业务必须让 guest 的 `rdma_cm` 按物理 `/26` 路由工作，可下发真实 prefix，但必须同时实现 CM proxy和必要的 ARP/NDP代理，不能只修改地址和路由。

MAC镜像使 guest默认 EUI-64 link-local GID与 host默认 GID一致；若 guest启用 stable-secret/privacy 地址并额外生成不在 allowlist内的 GID，server可以拒绝该额外 bind，但不得因此误删已成功绑定的合法 tuple。生产数据路径仍显式选择 IPv4 RoCE v2，避免地址生成方式和 GID type歧义。

`CREATE_BIND`还必须校验MVP固定的untagged `vlan=0xfff`和 `mtu=profile RDMA active MTU bytes`，并把当前profile generation记入mapping；这里的MTU来自guest `QUERY_PORT.active_mtu`转换后的字节值（当前driver固定1024只是待修复实现），不是 `ens10np0`的Ethernet MTU 1500。字段不匹配返回错误，不自动修正。每个mapping保存QP引用计数：RC QP进入RTR时取得selected IPv4 mapping引用，QP销毁/退回RESET后释放。

bind命令的重试语义固定为幂等：`CREATE_BIND`在同一guest index上重放完全相同的`(gid,type,vlan,mtu)`，且已存mapping的profile generation仍等于当前不可变session generation时，返回成功且不增加引用；若该index已绑定不同tuple则返回`-EEXIST`。现有`DESTROY_BIND` wire ABI只有`index + dest_gid`，不携带gid type或generation：server先按index查mapping，mapping不存在时返回成功；存在时要求`dest_gid`等于已存GID，并要求已存generation等于当前session generation，否则返回`-EINVAL`，type取已存mapping而非请求中不存在的字段。selected mapping的删除仍执行下述确定性停用，重复destroy不得再次上报事件。

删除额外link-local/stable/privacy mapping只移除该mapping并返回成功。删除当前selected IPv4 mapping是网络栈驱动的权威事件，不能因引用长期返回 `-EBUSY`：按6.2冻结新操作、令所有依赖QP进入ERROR，利用已预留pending ledger保存flush CQE并回收host WR，释放引用和mapping、把port降到INIT并上报事件，之后返回成功。该收敛受与强制清理相同的5秒server期限约束；超时升级为device-fatal并关闭session，绝不等待guest主动poll出CQ空间。额外GID的bind失败只影响该请求，不得令已建立的合法selected mapping或device进入fatal。

### 9.4 host identity 变化

server 以启动时的 identity snapshot为整个 session的不可变配置，同时订阅 rtnetlink、RDMA netlink和 device async event检测漂移：

- link down或 selected IP/GID、MTU、MAC、device/port变化：立即停止接收 doorbell和新 AdminQ create/modify；
- 将所有 host QP迁移到 ERROR并 drain completion；
- 通过 server新增的 async EQE producer向 guest报告 `DEVICE_FATAL`，提交到 async ring并触发 MSI-X vector 1；若 ring满或通知失败，立即关闭 vfio-user session作为兜底；
- 撤销 MR后终止 session；只有 server restart与 guest reset/reprobe才能生成新 profile并恢复；
- reset/reprobe必须注销并重建 ERNIC netdev，确保旧 `/32` 地址和旧 GID被清除；部署脚本重新启动 DHCP client并等待新 generation的 `CREATE_BIND`。

profile带 generation。所有 GID mapping和 QP都记录创建时 generation，防止配置变化后误用旧 index。MVP不做在线 identity更新、QP迁移或自动重连。async EQE只由唯一event-dispatch线程写入；其他线程把归一化事件投递到MPSC队列。dispatch线程先写完整EQE，再以release更新producer并触发vector 1；guest以acquire读取producer，以release推进consumer，server以acquire回收空间和解除handle quarantine。async ring满、非法consumer或IRQ失败立即进入session fail-stop，不覆盖旧entry；RESET/断连在停止并join dispatch线程后清空私有sequence和ring状态。

MVP只允许发布下列EQE，`type/info`语义固定，其他host async event仅记录日志并按严重性转QP或device fatal，不得直接cast枚举：

| EQE type | `info` | 产生条件 |
| --- | --- | --- |
| `PORT_ACTIVE` | port 1 | selected IPv4 bind令readiness首次进入ACTIVE |
| `PORT_ERR` | port 1 | selected bind被删除并完成确定性停用 |
| `GID_CHANGE` | port 1 | selected bind删除后紧随`PORT_ERR`发布；同一状态转换只发布一次 |
| `QP_FATAL/QP_REQ_ERR/QP_ACCESS_ERR` | guest QP handle | 可归属单个QP的host async或数据面fatal；发布前对象进入ERROR |
| `CQ_ERR` | guest logical CQ handle | CQ/pending ledger不变量破坏 |
| `DEVICE_FATAL` | 0，guest忽略该字段并按port 1派发 | identity漂移、DMA revoke命中活跃MR、async/CQ通知失效或不可恢复backend错误 |

EQE中的QP/CQ值永远是guest handle，不是host QPN/CQN。`DEVICE_FATAL`之后不得再发布普通对象事件；若event ring没有空间或vector 1注入失败，直接关闭session。

### 9.5 GSI/MAD 与 rdma_cm

legacy QEMU 路径依赖 `rdmacm-mux` 处理 GSI/MAD。当前 standalone 代码只保留了 stub 类型，没有可用的 mux channel。因此：

- 手工交换 GID/QPN 并调用 `ibv_modify_qp()` 的 RC 测试可先完成；
- `rdma_cm`、SA/CM MAD、UD GSI 不能作为首期验收项；
- 后续要么恢复并隔离 `rdmacm-mux`，要么设计新的 CM proxy/control plane。

即使 IP/GID完全一致，这一限制仍然存在：身份一致解决的是寻址参数，不会自动实现 guest CM message 到 host CM/MAD 的事件代理。

## 10. 生命周期、并发与错误处理

### 10.1 对象状态

每个对象保存 `LIVE -> QUIESCING -> RETIRED/DEAD` 状态、generation和refcount。数据面取得引用后才能提交WR。正常AdminQ销毁不隐式级联：有child/inflight引用的MR、CQ或PD返回 `-EBUSY`；QP只有在inflight WR、pending CQE以及已经发布但guest尚未消费的该QP CQE都为0时才允许destroy，否则立即返回 `-EBUSY`并保持LIVE。server按每条CQE的logical sequence跟踪归属，guest以release推进CQ consumer后才能扣减该QP未消费计数。同步destroy不能转ERROR、制造flush CQE再等待guest腾出CQ；应用需要flush时先显式 `MODIFY_QP(ERR)`并poll完completion，再重试destroy。GID bind遵循9.3的网络生命周期特例。

强制清理的host资源逆序为：

```text
stop new doorbells
  -> QP to ERROR
  -> drain/flush host CQ
  -> destroy QP/SRQ
  -> destroy per-QP host CQ
  -> release logical guest CQ
  -> dereg MR
  -> dealloc PD
  -> release guest memory leases
  -> close channel/context
```

设备reset、DMA revoke、identity fatal和client disconnect走同一条幂等强制清理路径，不受普通 `-EBUSY`规则限制：先冻结doorbell，令host QP进入ERROR，并在不超过5秒（小于guest AdminQ当前10秒超时）的server内部期限内poll/reclaim host WR，然后销毁host对象、撤销MR和mapping。该路径可以丢弃尚未发布的guest CQE、清空guest/pending ring并关闭或reset session，绝不能等待guest poll、CQ空间或notification ACK；超时直接关闭session并依靠HCA teardown回收，而不是无限阻塞AdminQ。

server只负责关闭/重建vfio-user session和虚拟设备状态，不能宣称自己直接执行guest PCI reprobe。guest侧的remove/reprobe由QEMU/libvirt hot-unplug/hot-plug或VM重启触发；部署控制器必须等待旧server完全退出、旧socket消失和guest设备remove完成后，才启动新server并重新hot-plug。原地PCI reset仅在实现并验证完整frontend remove/reinit后才可作为快捷路径。

每个posted WR同时持有QP、目标logical CQ及其全部local MR引用。正常路径中，内部WC到达后可释放local MR引用；QP/CQ引用保持到guest CQE已发布，若CQE先进入pending ledger，则引用所有权随pending记录转移。强制清理在停止一切并发访问后集中作废cookie/generation并释放这些引用，不要求构造guest flush CQE。

### 10.2 锁划分

- `resource_lock`：handle table、key index、依赖和 object state；
- 每个 QP 的 `submit_lock`：SQ/RQ 消费与 host post 顺序；
- 每个 CQ 的 `publish_lock`：唯一completion poller发布CQE，并与MMIO线程的ARM状态变更串行化；CQ `POLL`只投递请求；
- DMA map tree 自有锁；
- 唯一允许的嵌套顺序为`resource_lock -> submit_lock -> publish_lock`；DMA map tree锁不得与后三者嵌套。正常实现优先采用“锁内查找并增ref，解锁后调用verbs”的模式，任何`ibv_*`调用、等待eventfd/condition或guest IRQ注入期间均不得持有上述锁；
- completion poller用专用stop eventfd唤醒，收到停止后继续drain或按强制清理合同作废cookie，再`pthread_join()`；join完成之前不得destroy任何private host CQ、completion channel或`ibv_context`；
- 调用可能阻塞的 `ibv_destroy_*()` 时不持有全局锁。

所有 completion cookie 都携带generation，避免handle被复用后旧WC写入新对象；CQ handle还必须遵守5.1的notification quarantine。

### 10.3 错误映射

- 所有AdminQ命令（包括destroy）的同步失败按6.5写公共response header并精确保留errno；`REG_ERR`仅报告传输/设备错误。
- `ibv_post_*` 的同步失败先分类：`ENOMEM/EAGAIN`为可重试backpressure，不消费WQE；确定的参数/状态/设备错误为FATAL，令QP进入ERROR并产生相应错误/flush WC。
- host WC status严格按8.3表映射；未知值映射GENERAL_ERR并保留`vendor_err`，不得依赖两套枚举数值相同。
- CQ overflow、DMA revoke、device fatal 通过 async event ring报告，并令相关 QP/设备进入 ERROR。
- server 日志同时打印 guest handle、host QPN/key、AdminQ sequence 和 errno，禁止只打印裸指针。

## 11. 安全与隔离

- MVP威胁模型明确为可信的实验环境 guest `rocm-ernic-e2e`。guest获得真实 GID/QPN/rkey后，能够以 host物理 identity向可达 RDMA peer发起连接；PD只能隔离本地对象，不能限制网络目标。
- MVP 要求 `mlx5_0/ens10np0` 由一个 MLNX backend session独占，以 `(PCI BDF, port)` 命名的 `flock` 文件（例如 `/run/lock/rocm-ernic-0000:00:0a.0-p1.lock`）在启动时强制；host 可保留该 IP/GID以维持 HCA identity，但不在该 port上运行竞争的 RDMA/CM workload。
- 不同 guest 至少应使用独立 VF/SF和 PD；仅用不同 PD共享 PF不能解决相同 service ID、CM demux、带宽和故障域问题。
- 若目标升级为不可信 tenant或生产多租户，必须改用专用 VF/SF，或至少增加 DGID/service allowlist、速率限制和设备 namespace/cgroup隔离；该安全扩展不属于 MVP。
- 限制 PD/MR/CQ/QP 数、总注册字节数、单 MR 大小、单 WQE SGE 数和 pending CQE 数。
- 检查所有 guest 提供的长度、乘法、页数、ring index 和 handle。
- WQE SGE 必须属于 QP 的 PD，范围完全落在 LIVE MR 内，access 与 opcode 匹配。
- 不允许 guest 任意选择或修改 host netdev/device/MAC/IP/GID/PFC；这些只能来自 server 启动配置和只读 identity profile。
- DMA UNMAP、client disconnect 和 process signal 都必须先撤销 HCA DMA 能力。
- 不建议依赖 root 常驻运行；完成原型后使用所需的最小 capability、设备 cgroup 和 systemd sandbox。

## 12. 实施步骤

### 阶段 0：修复公共基础

- 新增 `MlnxIdentityProfile`，自动校验 `ens10np0 -> mlx5_0/1`，以 `(gid,type)` 解析 index。
- MLNX 模式在设备创建前强制镜像物理 MAC，DSR切换 version 21 + RoCE v2，并同时移除 server和 guest synthetic default GID。
- 保留 `mesh_flags.bit0` 的 mesh语义，在 bit 8/9定义 identity feature；实现 `effective_version=min(server,guest)`，修复 guest以 `IFF_NO_QUEUE` 跳过 `CREATE_BIND` 的逻辑。
- MLNX guest强制申请3个MSI-X用途向量；单向量/MSI/INTx降级只保留给legacy backend。
- 增加 fixed-identity DHCP路径，以 `/32`、无默认路由/DNS方式配置 guest shadow IP；server使用独立 synthetic MAC/IP。
- 扩展 `CREATE_BIND`路径保存guest index → host index及引用；字段MTU固定解释为 `QUERY_PORT`返回的RDMA active MTU字节值；实现 `NEGOTIATING -> IDENTITY_READY -> ACTIVE`及selected bind删除的确定性停用，禁止绑定前报告ACTIVE。
- 扩展v21 `CREATE_MR`：在v20请求尾部追加HCA `iova`，保留全部旧字段offset，并同时传递pinned `start`；两端头文件用static assertion固定ABI布局及页内offset合同。
- 冻结AdminQ response合同：`REG_ERR`只报传输错误，所有命令读取 `hdr.err`并精确返回errno，destroy只有成功后才释放frontend对象。
- 启用 v21继承的 v20 create-QP response，分离 handle/QPN；CQE `qp`低32位放handle、高32位放QPN，统一 resource table、QPN hash、kernel `qp_tbl`和userspace `wc.qp_num`语义，禁止取模或16-bit截断。
- 增加 `(PCI BDF, port)` 独占锁；关键 identity变化执行 device-fatal fail-stop。
- 实现 server async EQE producer、MSI-X vector 1通知、ring full和 session-close兜底，并确保 reset/reprobe清除旧 DHCP地址/GID。
- 为 backend 对象增加 private data、state、generation/refcount，并为不带generation的CQ notification实现handle retirement/quarantine。
- 依据已通过的CX6和实际libvfio-user SGL实验，重构DMA mapping为7.2的`MREMAP_DONTUNMAP`可撤销lease，安装device quiesce callback，消除移动原VMA、256项全局表和错误的unregister-EBUSY假设；实现后补真实guest PDIR/DMA_UNMAP端到端故障注入。
- 扩展QP attr和send WR backend-neutral接口；post返回 `POSTED/RETRY/FATAL`并定义cookie所有权。
- 裁剪 QUERY_PORT/device caps，MLNX模式清除 `IB_PORT_CM_SUP`及所有未实现 capability。
- 为每个未支持 opcode/mask 加显式错误。

### 阶段 1：MLNX RC SEND/RECV MVP

- 从 legacy `rdma_backend.c` 提取 init/fini/query/PD/MR/CQ/QP/completion 实现到 `rdma_backend_mlnx.c`。
- 注册 `mlnx`，同时保留 `verbs` alias。
- 实现真实lkey/rkey、QP handle/QPN、每QP私有host CQ和按cookie路由的guest completion回写。
- 实现SQ/RQ shadow submit cursor、completion后归还credit，以及host容量暂满时的RETRY路径。
- 实现logical CQ one-shot ARM_ALL、frontend acquire missed-event检查、唯一poller负责的多私有CQ drain和notification ring fail-stop；solicited-only请求在MVP明确拒绝。
- 使用镜像 identity和 tuple GID mapping，先完成同一 host自连接调试，再以同 L2/VLAN的两端 CX6作为正式功能验收。

### 阶段 2：RDMA READ/WRITE

- 增加SEND_WITH_IMM、RDMA WRITE/WRITE_WITH_IMM和RDMA READ，透传remote address、真实rkey和immediate data。
- 完整校验access flags并补充recv-with-imm CQE；remote rkey只由远端HCA校验，不查询本地MR表。
- 若业务需要solicited-only通知，先拆分每QP的host send/recv CQ并以recv CQ的solicited arm实现；完成竞态和混合send/recv验证后才发布该capability。
- 验证远端写入直接出现在嵌套 VM buffer，且 server CPU 不复制 payload。

### 阶段 3：完整连接与高级 verbs

- CM proxy/`rdma_cm` 与必要的 ARP/NDP代理；
- 在线 identity profile更新、QP重建与自动重连；
- UD/SRQ；
- atomic；
- 更完整的非fatal async events和大规模CQ/QP调优；
- 性能调优，必要时再评估 `mlx5dv`。

## 13. 验收方案

### 13.1 单元与故障注入

- netdev/ibdev 不匹配、无地址、多地址歧义、GID type不匹配；
- guest GID index与 host index不同但 tuple相同；伪造 CREATE_BIND 必须失败；
- `CREATE_BIND.mtu`使用RDMA active MTU字节值，误传Ethernet MTU必须失败；selected bind删除必须完成QP停用、port INIT和事件上报，不能遗留永久 `-EBUSY`；
- mesh bit与 identity bits分别组合，legacy TCP/loopback不能被误识别为 MLNX；guest/server version低于 21时 MLNX必须拒绝启动；
- selected IP/GID/port 在运行中消失，profile generation必须阻止旧映射复用；
- guest pinned VA/server VA/HCA IOVA转换，覆盖 `start != iova`、非页对齐、多页物理不连续MR和重叠VA重用；
- PDIR 中断、重复页、超大 nchunks、算术溢出；
- 启动时`MREMAP_DONTUNMAP`能力探测成功/失败；原iovec、alias和libvfio-user DMA_UNMAP生命周期保持一致，任何路径均不得移动或munmap原iovec；
- create 各步骤失败的逆序回滚；
- 每种AdminQ业务错误精确返回 `-EINVAL/-EBUSY/-EOPNOTSUPP`，`REG_ERR`保持传输语义；destroy失败后frontend对象仍可继续使用或重试；
- forged lkey/rkey、跨 PD SGE、越界 SGE；
- 私有host CQ满、guest CQ满、pending ledger容量边界、重复destroy、普通QP destroy有在途WR、reset时在途WR、DMA UNMAP时活跃MR；验证device quiesce callback返回`EBUSY`、drain/dereg/alias teardown/put、`vfu_device_quiesced()`、void unregister callback确认零引用及session close的严格顺序；普通destroy必须立即 `-EBUSY`，强制清理不得等待guest CQ空间且须在5秒内收敛或关闭session；
- SQ/RQ连续投递超过depth时，在内部WC到达前不得提前归还credit；host post返回RETRY时不得丢WQE、重复post或生成伪错误CQE；
- shared环覆盖计数回绕、跨generation、producer超depth和release/acquire；链式post在中途失败时已排队前缀仍必须doorbell且 `bad_wr`指向首个失败项；
- CQ覆盖polling、ARM_ALL、arm与completion竞态、frontend missed-event检查和多个QP共享一个logical CQ；每次arm最多产生一个notification entry，MVP的ARM_SOLICITED必须明确失败；测试必须由真实guest驱动产生MMIO write/read序列，并记录QEMU/libvfio-user版本，不能用server本地自检替代；
- MLNX只有1/2个vector时probe必须失败，3个vector时response/async/CQ路径分别可用；
- async ring满、MSI-X失败和 identity变化时必须进入 fail-stop，不能继续接收 WR；
- CQE低32位handle与高32位真实QPN分别被kernel/provider正确消费；QP handle复用时迟到WC不得污染新QP，CQ notification consumer越过最后序号前handle不得复用。

### 13.2 实机功能

1. `mlx5_0/1` port ACTIVE、GID index/type符合配置；guest在IPv4 bind前只见INIT，bind后转ACTIVE；
2. guest MAC与 `ens10np0` 一致，通过systemd-networkd取得固定 `/32` IPv4；`show_gids` 同时存在与host相同的link-local和IPv4-mapped RoCE v2 GID；
3. guest发出的合法 `CREATE_BIND(gid,type)` 精确映射到 host entry，即使两端 index不同；额外 GID被拒绝，RC QP强制选用 IPv4 tuple；
4. 嵌套 VM 创建/销毁 UC、PD、MR、CQ、RC QP；
5. 64 B到1 MiB SEND/RECV，覆盖polling、all/next-completion notification和unsignaled send；MVP的solicited-only请求确认返回不支持；
6. 非页对齐 buffer、物理不连续 PDIR、多个 SGE、多个并发 QP及多个 QP共享一个 guest CQ；
7. `ibv_reg_mr_iova()`使用与pinned VA不同的IOVA后，SEND/RECV仍正确访问对应buffer；
8. 证明 guest 普通 Ethernet frame没有从 `ens10np0` 发出，物理网络无重复 ARP/NDP source；
9. identity变化触发 `DEVICE_FATAL`；重复加载/卸载 driver和 server crash/restart后旧 `/32`、GID、MR和 QP均被清理；
10. `ibv_devinfo` 不显示 CM/SRQ/atomic等未实现能力，`rdma resource show`和 server resource counters前后闭环；
11. 两个真实 CX6 endpoint在同一 L2/VLAN完成端到端 SEND/RECV；单机 loopback结果不替代此项。

阶段2另行验收：两端只交换guest API返回值即可使用真实GID/QPN/rkey/address完成SEND_WITH_IMM、RDMA WRITE/WRITE_WITH_IMM和RDMA READ，并验证immediate CQE字段与remote access错误；若实现solicited-only通知，还须覆盖分离host send/recv CQ的one-shot和missed-event竞态。

### 13.3 性能与零拷贝证据

- 对 payload 路径做 `perf`/uprobes，确认没有与传输长度成正比的 `memcpy`；
- 记录doorbell、host post、host WC、guest CQE和WQ credit；验证每个成功host post最终恰有一个内部WC，而guest成功CQE数只等于guest请求signaled的完成数，错误/flush另计；
- 比较 1/4/64 QP 的吞吐、p50/p99 latency 和 CPU；
- 监控 pinned memory、MR cache、CQ overflow 与 retry/error counters。

## 14. Go/No-Go 条件

### 14.1 实现准入

以下是开始主体实现前必须确认的外部约束和P0实验，不把尚未编写的功能误列为准入条件：

- 接受 `ens10np0/mlx5_0`由单个可信guest session专用，并采用只读identity mirror；
- 接受MVP镜像物理MAC、ERNIC netdev仅承载identity，普通网络和控制连接走已有virtio NIC；
- guest与host的selected `(GID,type)`能精确一致；link-local/IPv4 RoCE v2 tuple可bind，但RC QP强制使用selected IPv4 tuple，暂不承诺透明 `rdma_cm`；
- version 21、identity feature bits、AdminQ errno、CREATE_MR尾部扩展、CQE handle/QPN编码及all-only通知合同已完成协议评审并冻结；
- server运行环境可提高memlock limit、设置注册内存quota并提供3个MSI-X用途向量；
- 多页物理不连续且非页对齐的PDIR等价alias通过`ibv_reg_mr_iova()`实机spike，三种地址页内offset一致，释放后无stale HCA pin；libvfio-user quiesce合同能够承载DMA revoke。

编码准入结果（2026-08-30）：

| P0项 | 结果 | 结论 |
| --- | --- | --- |
| CX6非连续页/非对齐VA与IOVA/RC DMA | 两轮PASS；错误offset=`EINVAL` | Go |
| dereg、VA重用、进程异常退出 | PASS；前后无残留MR/QP | Go |
| libvfio-user SGL/unmap/quiesce | 上游3组测试PASS | Go；采用长期lease，禁止put后访问 |
| 实际vfu iovec非移动alias生命周期 | 3个独立DMA region双向一致；alias teardown、put、UNMAP PASS | Go；冻结`MREMAP_DONTUNMAP`，禁止移动原VMA |
| mirror MAC + `/32` IPv4的GID生成 | rxe生成预期link-local和IPv4-mapped RoCE v2 GID | Go；强制`addrgenmode=eui64` |
| BAR2 ARM write/readback顺序 | 真实vfio-user socket PASS | Go；同BAR同offset同步read作为flush |
| guest禁用DMA MR/GSI后的probe | PCI/netdev/RDMA device注册PASS | Go |
| memlock启动环境 | 普通shell为8192 KiB；`systemd-run -p LimitMEMLOCK=infinity`实测unlimited | Go；正式unit必须配置并启动自检 |

真实guest PDIR解析、DMA revoke期间活跃MR、DHCP报文、三vector IRQ、真实guest ARM竞态和双机CX6数据面依赖尚未实现的v21/MLNX代码，无法靠现有legacy backend得到有效结论，已明确归入13节实现验收；它们不是未决架构选择。非移动alias及libvfio-user回调合同已经冻结，至此没有阻止编码的外部可行性问题。

### 14.2 MVP 发布门槛

以下条件全部通过才允许把MLNX RC SEND/RECV标记为可用：

- v21协商、ABI static assertion及feature拒绝路径通过；guest取得3个MSI-X用途向量，async/CQ任一路不可用都拒绝启动；
- AdminQ精确errno和无错误释放、QP/CQ handle generation/quarantine、普通/强制销毁合同通过故障注入；
- DMA revoke与MR deregistration纳入强制生命周期，异常退出后无残留HCA pin或host object；
- SQ/RQ 64-bit shadow sequence、completion归还credit、链表部分成功和host RETRY通过depth、回绕及并发压力测试；
- 每QP private host CQ覆盖内部全signaled WR，且由唯一poller按cookie正确路由；polling和one-shot ARM_ALL/missed-event通知共享guest CQ正确，MVP明确拒绝solicited-only；
- RDMA port在selected IPv4 bind前不报告ACTIVE；bind删除和identity变化分别按停用/fail-stop状态机处理；
- `DEVICE_FATAL` async EQE/MSI-X、notification/async ring full兜底、guest reset/reprobe清除旧identity均已验证；
- QUERY/capability只声明RC SEND/RECV、本阶段local access和实际MTU，GSI/DMA MR及其他未实现opcode/mask精确报错；
- 两个同L2/VLAN的真实CX6 endpoint通过13.2全部MVP项目，且性能报告记录fabric的PFC/ECN/DCB前提。

若必须在第一版同时支持“ERNIC 通用 Ethernet、透明 `rdma_cm`、共享同一 PF 的多 guest、全部 verbs”，则当前 identity mirror 方案不应直接扩张，需要改为专用 VF/SF 或先设计完整 L2/L3/CM proxy和安全模型。

## 15. 参考

- 本仓库：`docs/test-data-transfer-adminq.md`
- 本仓库：`src/from-qemu/hw/rdma/rdma_backend.c`
- 本仓库：`src/from-qemu/hw/rdma/rdma_rm.c`
- 本仓库：`src/from-qemu/hw/rdma/vmw/pvrdma_cmd.c`
- 本仓库：`src/from-qemu/hw/rdma/vmw/pvrdma_qp_ops.c`
- [rdma-core `ibv_reg_mr(3)`](https://github.com/linux-rdma/rdma-core/blob/master/libibverbs/man/ibv_reg_mr.3)
- [rdma-core `ibv_post_send(3)`](https://github.com/linux-rdma/rdma-core/blob/master/libibverbs/man/ibv_post_send.3)
- [QEMU 8.2 PVRDMA verbs backend](https://github.com/qemu/qemu/blob/v8.2.0/hw/rdma/rdma_backend.c)
- [QEMU 8.2 PVRDMA resource manager](https://github.com/qemu/qemu/blob/v8.2.0/hw/rdma/rdma_rm.c)
- [QEMU 8.2 PVRDMA design and setup](https://github.com/qemu/qemu/blob/v8.2.0/docs/pvrdma.txt)
