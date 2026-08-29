# `test_data_transfer`：ERNIC BAR、AdminQ 与 verbs 调用链分析

## 1. 测试范围和结论

本报告按 `DEVELOPMENT.md` 在 `debian-13-dev` 中启动 vfio-user server，并在嵌套 VM 中重新构建、加载驱动和本仓库 rdma-core provider 后运行 `test_data_transfer`。本轮记录对应提交 `77f883d2ccd13d980a63f635e1ed4da58601ccfa`，后端为 `loopback:mode=preserve`。

设备的三个名字分别代表不同层次：QMP 设备 ID 是 `ernic0`，PCI function 是 `0000:00:0a.0`，RDMA device 是 `rocep0s10`；网络设备是 `ens10`。

结果如下：

- 单次传输、5 次连续传输和 64/256/1024/2048/4096 字节边界传输全部通过，合计 11 次 SEND、11 次 RECV、22 个 CQE，双向各 11072 字节，测试打印 `ALL TESTS PASSED`。
- 驱动初始化产生 AdminQ #1～#14；`setup_resources()` 产生 #15～#24 共 10 条；`cleanup_resources()` 产生 #25～#31 共 7 条；驱动卸载产生 #32～#36 共 5 条。server 最终统计 `commands=36`、`interrupts=36`，所有 response 的 `err=0`。
- `ibv_post_recv()`、`ibv_post_send()` 和 `ibv_poll_cq()` 不走 AdminQ。前两者写共享 WQE ring 后敲 BAR2 doorbell，后者直接读取共享 CQ ring。
- BAR2 已正确暴露为 `512 × 4 KiB = 2 MiB`。正常卸载耗时 404 ms，没有 AdminQ timeout、`DESTROY_BIND`、GID 注销失败、MSI 警告、WARN/Oops 或调用栈。

本轮可复核证据统一位于 `docs/traces/test-data-transfer-20260829/`；旧的 `20260828` 目录只保留为问题修复前的历史记录，不再作为本文结论依据。

## 2. 一次运行的命令边界

| 阶段 | AdminQ 序号 | 内容 | 数量 |
| --- | --- | --- | ---: |
| 驱动初始化 | #1～#14 | port/pkey 查询，kernel PD/MR/CQ/GSI QP100 创建及状态切换 | 14 |
| `setup_resources()` | #15～#24 | UC、PD、两个 CQ、两个 MR、QP101 创建，QP101 INIT/RTR/RTS | 10 |
| 数据传输 | 无 | QP101 的 22 次 QP doorbell 和共享 ring/CQE | 0 |
| `cleanup_resources()` | #25～#31 | QP101、两个 MR、两个 CQ、PD、UC 销毁 | 7 |
| 驱动卸载 | #32～#36 | port 查询，kernel QP100/CQ0/MR0/PD0 销毁 | 5 |

这一区分很重要：#1～#14 和 #32～#36 是驱动生命周期流量，不能算到 `setup_resources()`；QP100 是驱动的 GSI QP，QP101 才是测试创建的 RC QP。

## 3. ernic0 的 BAR layout

VM 内 `lspci -Dvv -s 0000:00:0a.0` 和 sysfs `resource` 的实测结果为：

| BAR | Guest PA / 长度 | 内容与作用 | 本轮行为 |
| --- | --- | --- | --- |
| BAR0 | `0xc0200000`, 16 KiB | MSI-X table 在 `+0x0000`，3×16 bytes；PBA 在 `+0x2000` | 由 libvfio-user 的 MSI-X capability 管理；server 自有 BAR0 callback 统计 0 read/0 write |
| BAR1 | `0xc0204000`, 256 bytes | 64 个 32-bit 控制/状态寄存器；建立 DSR、提交 AdminQ、读错误码、控制中断 | 78 reads、44 writes |
| BAR2 | `0xc0000000`, 2 MiB | 512 个 UAR page，每个 user context 占 4 KiB；页内放 QP/CQ/SRQ doorbell | 536 writes；测试 context 使用 page 1，即 global offset `0x1000` |

加载后 PCI 状态为 `Mem+ BusMaster+ DisINTx+ MSI-X Enable+ Count=3`。BAR2 长度已经与 `RDMA_BAR2_UAR_SIZE=0x1000*MAX_UCS` 一致，旧记录中的 8 MiB 是已修复的二次乘 `sizeof(uint32_t)` 问题。

### 3.1 BAR1 寄存器

| offset | 寄存器 | 方向 | 本轮用途/行为 |
| --- | --- | --- | --- |
| `0x00` | `VERSION` | R | server 返回硬件协议版本 17；因此 guest 使用旧版 create-QP response，`qp_handle=qpn` |
| `0x04` | `DSRLOW` | W | DSR DMA 地址低 32 位；本轮写 `0x09af3000` |
| `0x08` | `DSRHIGH` | W | DSR DMA 地址高 32 位；本轮写 `0x1`，写入后 server 执行 `load_dsr()` |
| `0x0c` | `CTL` | W | `ACTIVATE=0`、`UNQUIESCE=1`、`RESET=2`；加载时 activate，卸载时 reset |
| `0x10` | `REQUEST` | W | AdminQ doorbell；每次 `rocm_ernic_cmd_post()` 写 0，值本身不编码命令 |
| `0x14` | `ERR` | R | 当前命令 handler 返回码；本轮 36 次均读到 0 |
| `0x18` | `ICR` | R | legacy interrupt cause；MSI-X 模式主要依靠 vector 区分事件 |
| `0x1c` | `IMR` | R/W | 全局中断 mask；0 为允许，`0xffffffff` 为屏蔽。卸载先完成资源 AdminQ，再屏蔽 |
| `0x20/0x24` | `MACL/MACH` | R/W | MAC 地址低/高部分 |

BAR1 只传控制信息，不承载 AdminQ payload。`REQUEST=0` 的 36 次写入与 36 条命令一一对应。

### 3.2 BAR2 doorbell

server 以 global offset 选择 UAR page，再用 `offset & 0xfff` 解码页内寄存器：

- `+0x0`：QP doorbell，bit 30=`SEND`，bit 31=`RECV`，低位为 QP handle。
- `+0x4`：CQ doorbell，bit 29=`ARM_SOL`，bit 30=`ARM`，bit 31=`POLL`，低位为 CQ handle。
- `+0x8`：SRQ doorbell，bit 30=`RECV`，低位为 SRQ handle。

测试 user context 的 UAR page 为 1，QP101 (`0x65`) 的值固定为：

- RECV：BAR2 `+0x1000` 写 `0x80000065`，11 次；
- SEND：BAR2 `+0x1000` 写 `0x40000065`，11 次。

全部 536 次 BAR2 写可精确分解为：QP100 在 page 0 的 512 次 RECV doorbell、CQ0 在 `+0x4` 的 2 次 ARM，以及 QP101 的 11+11 次。本次用户 CQ 未 arm，所以无 CQ doorbell。

## 4. AdminQ DMA、寄存器与 MSI-X 的协作

AdminQ 是“固定 DMA slot + MMIO doorbell + MSI-X completion”，不是 BAR 中的 payload FIFO。

### 4.1 初始化和实测地址

驱动为 DSR、request slot、response slot 分配 coherent DMA 内存。slot 各分配一页，但 server 只映射 ABI union 的实际大小：

| 对象 | Guest DMA | server 映射长度 | 作用 |
| --- | ---: | ---: | --- |
| DSR | `0x109af3000` | 288 bytes | capabilities、AdminQ slot 地址、async/CQ device ring 描述 |
| request slot | `0x1601e4000` | 200 bytes | `union pvrdma_cmd_req`，guest 写/server 读 |
| response slot | `0x109ae9000` | 192 bytes | `union pvrdma_cmd_resp`，server 写/guest 读 |

写 `DSRLOW/DSRHIGH` 后，`load_dsr()` 依次 DMA-map 上述三个对象以及 device event/CQ ring。各 CQ/MR/QP request 中的 `pdir_dma` 则是相应资源自己的两级页目录，和 AdminQ slot 是不同的 DMA 对象。

### 4.2 单条命令时序

1. `rocm_ernic_cmd_post()` 取得 semaphore，因此同一设备同时最多一条 outstanding command。
2. 驱动把完整 200-byte request union 复制到固定 request slot，重置 `cmd_done`，向 BAR1 `REQUEST` 写 0。
3. server 的 `pvrdma_regs_write()` 调用 `pvrdma_exec_cmd()`，从 request slot 读 `hdr.cmd`，经 `cmd_handlers[]` 分派到 handler。
4. handler 填充 response payload；命令执行后的公共逻辑写入 header 的 `response`、`ack=0x80000000|cmd`、`err`，同时把 handler 返回码写入 BAR1 `ERR`。
5. server 触发 MSI-X vector 0。guest IRQ handler完成 `cmd_done`，等待线程读取 `ERR`；调用者要求 response 时，再复制 192-byte response union 并校验 `ack`。
6. 销毁命令虽然调用 `cmd_post(..., rsp=NULL, 0)`，仍等待 vector 0 并读取 `ERR`；只是 guest 不复制 NOOP response payload。

本轮 `hdr.response` 始终为 0。关联正确性不依赖该值，而依赖单 outstanding、固定 slot 和 `ack` 的命令类型校验。命令等待上限为 10 秒，本轮未发生超时。

### 4.3 MSI-X 的作用与观测

| vector | 定义用途 | 测试前 → 测试后 | 结论 |
| --- | --- | ---: | --- |
| 0 | AdminQ response | 14 → 31 | 增加 17，正好对应 setup 10 条和 cleanup 7 条 |
| 1 | async event | 0 → 0 | 本轮无异步事件 |
| 2 | CQ completion | 0 → 0 | CQ 未 arm；完成由用户态直接 poll ring |

卸载的 5 条命令继续各触发一次 vector 0，因此 server 最终是 `commands=36`、`interrupts=36`。BAR0 的 MSI-X table/PBA 由 libvfio-user 截获处理，所以 server 的普通 BAR0 callback 计数为 0，并不表示 MSI-X 没有工作。

## 5. `setup_resources()`：ibv 参数与 AdminQ 字段的完整对应

### 5.1 对应关系的三个层次

- **直接字段**：例如 MR 的 `addr/length/access`、QP caps、modify mask，经过 ABI 枚举转换后进入 AdminQ。
- **对象句柄**：用户看到的 `struct ibv_* *` 指针从不发送给 server。provider/uverbs/kernel 将指针解析为之前 AdminQ 返回并保存在内核对象中的 `ctx/pd/cq/mr/qp_handle`。
- **派生字段**：provider 分配 ring buffer；kernel pin pages 并生成 `pdir_dma/nchunks`。这些字段不是应用实参，但由该调用触发并决定 server 可访问哪些 guest pages。

因此，不能把测试日志中的用户虚拟对象地址（如 `ibv_pd *`）与 AdminQ handle 数值直接比较；应沿“公开对象 → provider 对象 → uverbs 内核对象 → AdminQ handle”比较。

### 5.2 不产生 AdminQ 的 setup 调用

| 调用 | 实际行为与输出 |
| --- | --- |
| `ibv_get_device_list(&num_devices)` | libibverbs 枚举 sysfs/uverbs；得到 1 个设备。无设备命令 |
| `ibv_get_device_name(dev_list[0])` | 读取 libibverbs device 对象中的名字 `rocep0s10`；测试在 open 前和打印成功信息时各调用一次。无设备命令 |
| `ibv_free_device_list(dev_list)` | 释放用户态枚举列表；不关闭已经打开的 context。无设备命令 |
| `ibv_query_gid(ctx,1,0,&gid)` | uverbs 调用 kernel `rocm_ernic_query_gid()`，从 `sgid_tbl[0]` 复制 `fe80:...:2d6e`。不访问 server，因而 #22 与 #23 之间没有新 AdminQ 序号 |
| `ibv_wc_status_str(status)` | 仅在 completion 错误分支把枚举转成字符串；本轮没有错误，因此未实际执行，也没有设备命令 |

`malloc()`/`memset()` 只是测试的普通内存操作，不属于 verbs 调用；它们随后通过 `ibv_reg_mr()` 才被 pin 和登记。

### 5.3 创建调用：输入、request、response 和公开输出

| 调用（本轮实参） | AdminQ request：直接/句柄/派生字段 | AdminQ response → kernel/provider → ibv 输出 |
| --- | --- | --- |
| `ibv_open_device(rocep0s10)` | #15 `CREATE_UC pfn=0xc0001`。设备名只用于选中 uverbs device，不进入 request；kernel UAR allocator 选择 BAR2 page 1，物理 PFN 才进入 request | `ctx_handle=0` 只保存在 kernel ucontext；`qp_tab_size` 由 kernel 从 DSR caps 另经 udata 返回。provider 分配并返回 `ibv_context *=0x562afa9370f0`，该指针不来自 AdminQ |
| `ibv_alloc_pd(ctx)` | #16 `CREATE_PD ctx=0`；公开 context 指针转换为 kernel 保存的 `ctx_handle` | `pd_handle=1` → kernel `pd_handle/pdn=1` → provider `pdn=1`；公开返回 `ibv_pd *=0x562afa92d9d0` |
| `ibv_create_cq(ctx,16,NULL,NULL,0)`（send） | #17 `CREATE_CQ ctx=0 cqe=16 nchunks=2 pdir_dma=0x140aea000`。`cqe` 经 power-of-two round（16 不变）；provider 创建 8192-byte buffer（4 KiB ring header + 16×64-byte CQE 向页对齐），kernel pin 后派生 2 pages 和页目录。`cq_context=NULL` 仅是用户态回调数据；`channel=NULL`、`comp_vector=0` 不进入 AdminQ | `cq_handle=1,cqe=16` → udata `cqn=1,ncqe=16`；`cqe_size=64` 是 kernel 本地常量，不是 AdminQ response。公开返回 `ibv_cq *=0x562afa937500,cqe=16` |
| 同上（recv） | #18 字段相同，仅派生 `pdir_dma=0x105553000` | `cq_handle=2,cqe=16`；公开返回 `ibv_cq *=0x562afa937660,cqe=16` |
| `ibv_reg_mr(pd,0x562afa9377c0,4096,LOCAL_WRITE)`（send） | #19 `CREATE_MR pd=1 start=0x562afa9377c0 length=4096 pdir_dma=0x161207000 access=0x1 flags=0 nchunks=2`。`pd/start/length/access` 对应实参；`flags=0` 表示普通 user MR；地址未页对齐，所以覆盖两个 4 KiB pages，kernel 派生 `nchunks/pdir_dma` | `mr_handle=1,lkey=2,rkey=0xffffffff`；handle 留在 kernel 供销毁，标准 uverbs response 形成公开 `ibv_mr *=0x562afa9397e0,lkey=2,rkey=0xffffffff` |
| `ibv_reg_mr(pd,0x562afa9387d0,4096,LOCAL_WRITE)`（recv） | #20 同样映射，`pdir_dma=0x10557b000,nchunks=2` | `mr_handle=2,lkey=3,rkey=0xffffffff`；公开 `ibv_mr *=0x562afa939820` |
| `ibv_create_qp(pd,&attr)` | #21 `CREATE_QP pd=1 scq=1 rcq=2 srq=0 send_wr=16 recv_wr=16 send_sge=1 recv_sge=1 inline=0 sq_sig_all=0 qp_type=2 is_srq=0 total_chunks=3 send_chunks=1 pdir_dma=0x10851d000`，另有 `lkey=0,access_flags=LOCAL_WRITE`。PD/CQ 指针转换为 handle；RC 枚举转换为 2；NULL SRQ 转成 0/false。provider 派生 SQ/RQ WQE size 和两个 mmap buffer，kernel pin 后形成 3 pages，其中 `tbl[0]` 是共享 ring-state page、1 个 SQ data page、1 个 RQ data page | 协议 v17 response 为 `qpn=101` 和协商 caps `16/16,1/1,inline=0`；kernel 令 `qp_handle=qpn=101`。kernel 经 udata 另返回 WQE sizes、depth、UAR mmap offset 和 QP/CQ doorbell offset，这些不是 AdminQ response 字段。公开返回 `ibv_qp *=0x562afa939860,qp_num=0x65` |

loopback backend 自己为 MR 生成 `rkey=handle+0x10000`，但 `rdma_rm_alloc_mr()` 明确把 guest response `rkey` 设成 `-1`，所以应用看到 `0xffffffff`；这不是日志遗漏。类似地，公开对象指针只用于当前进程，重跑会变化；句柄、序号和字段关系才是协议语义。

### 5.4 三次 `ibv_modify_qp()`

三次调用均将公开 QP 指针解析为 `qp_handle=101`。kernel 先校验状态迁移，再将 `ib_qp_attr` 和 `attr_mask` 转成 PVRDMA ABI。因为测试每次先清零 `qp_attr`，mask 未选中的字段在 trace 中为 0。

| 转换 | ibv 输入 → AdminQ request | server 实际消费 | response → ibv 输出 |
| --- | --- | --- | --- |
| INIT | #22：mask `0x39`，`state=1,pkey=0,port=1,access=0x6` | `modify_qp()` → `rdma_rm_modify_qp()`；loopback `qp_state_init(qp_type,qkey=0)` 将状态置 INIT。当前 RM/backend 不使用 pkey、port、access | 只有 header：`ack=0x8000000a,err=0`；`ibv_modify_qp()` 返回 0 |
| RTR | #23：mask `0x129181`，`state=2,mtu=3,dest_qpn=101,rq_psn=0,max_dest_atomic=1,min_rnr=12,sgid_index=0,dgid=fe80:...:2d6e`；AV 中的 port/global/hop-limit 等也在完整结构内 | RM 传递 `sgid_index,dgid,dest_qpn,rq_psn,qkey`；loopback 保存远端 QPN/GID/RQ PSN并生成 local address/rkey。当前 backend 不使用 MTU、max_dest_atomic、min_rnr 等重试/流控属性 | header `ack=0x8000000a,err=0`；返回 0 |
| RTS | #24：mask `0x12e01`，`state=3,sq_psn=0,timeout=14,retry=7,rnr_retry=7,max_rd_atomic=1` | RM 传递 `sq_psn,qkey`；loopback 置 RTS 并尝试配对。本例无独立 peer，日志显示等待 partner；SEND 时仍按 `remote_qpn=101` 找到自身。当前 backend 不使用 timeout/retry/rnr_retry/max_rd_atomic | header `ack=0x8000000a,err=0`；返回 0 |

上述“server 实际消费”列避免把“字段已通过 ABI 发送”误写成“后端已经实现该语义”。

### 5.5 清理调用及句柄闭环

| 调用 | AdminQ request | response/公开结果 |
| --- | --- | --- |
| `ibv_destroy_qp(qpn=101)` | #25 `DESTROY_QP qp=101` | `ack=0x8000000c,err=0`；返回 0，provider 再 unmap/free SQ、RQ、UAR 映射 |
| `ibv_dereg_mr(send)` | #26 `DESTROY_MR mr=1` | `ack=0x80000005,err=0`；返回 0 |
| `ibv_dereg_mr(recv)` | #27 `DESTROY_MR mr=2` | 同上 |
| `ibv_destroy_cq(send)` | #28 `DESTROY_CQ cq=1` | `ack=0x80000008,err=0`；返回 0，provider unmap/free CQ buffer |
| `ibv_destroy_cq(recv)` | #29 `DESTROY_CQ cq=2` | 同上 |
| `ibv_dealloc_pd(pd)` | #30 `DESTROY_PD pd=1` | `ack=0x80000003,err=0`；返回 0 |
| `ibv_close_device(ctx)` | #31 `DESTROY_UC ctx=0` | `ack=0x8000000e,err=0`；关闭 uverbs context，kernel 释放 UAR page |

创建与销毁完全对称：`UC 0`、`PD 1`、`CQ 1/2`、`MR 1/2`、`QP 101` 均由同一 handle 闭环；没有遗留用户资源。

## 6. 每个 ibv 调用在 server 后端的处理链

有 AdminQ 的公共前缀和返回路径为：

`ibv_*` → rocm_ernic provider → `ibv_cmd_*` → uverbs → kernel `rocm_ernic_*` → `rocm_ernic_cmd_post()` → BAR1 `REQUEST` → `pvrdma_regs_write()` → `pvrdma_exec_cmd()` → handler → response slot/`ERR` → MSI-X vector 0 → `cmd_done` → uverbs/provider。

各调用进入 server 后的差异如下：

| ibv 调用 | server/resource-manager/backend 调用链 | 代码行为 |
| --- | --- | --- |
| `ibv_open_device` | `create_uc()` → `rdma_rm_alloc_uc()` | 只在 UC resource table 分配 `ctx_handle`；当前 server 不保存/校验 PFN，也不调用 loopback vtable |
| `ibv_alloc_pd` | `create_pd()` → `rdma_rm_alloc_pd()` → `backend_ops->create_pd()` → `loopback_create_pd()` | 分配 guest PD slot和 loopback PD；两层 handle 属于不同命名空间，数值相同不能作为通用假设 |
| `ibv_create_cq` | `create_cq()` → `create_cq_ring()` → `rdma_rm_alloc_cq()` → `loopback_create_cq()` | 先按 pdir 映射 directory/table、ring-state 和 CQE pages并初始化 `PvrdmaRing`，再分配 guest CQ slot和 loopback completion queue |
| `ibv_reg_mr` | `create_mr()` → `pvrdma_map_to_pdir()` → `rdma_rm_alloc_mr()` → `loopback_create_mr()` | 映射 pinned guest pages，记录 guest VA/length/access及 host mapping，创建 lkey；普通 user MR 映射失败时当前代码仍允许 loopback 用 NULL mapping 继续，这是容错路径，本轮未触发 |
| `ibv_create_qp` | `create_qp()` → `create_qp_rings()` → `rdma_rm_alloc_qp()` → `loopback_create_qp()` | 先映射共享 ring-state、SQ/RQ WQE pages，再校验 PD/CQ/SRQ handle，分配 QP resource，创建初态 RESET 的 loopback QP和 send/recv queues，返回 backend QPN 101 |
| `ibv_modify_qp` | `modify_qp()` → `rdma_rm_modify_qp()` → `loopback_qp_state_init/rtr/rts()` | 只在 mask 包含 STATE 时执行状态处理；实际消费字段见 5.4，未实现的属性只被传输、不改变 loopback 行为 |
| `ibv_query_gid` | 不到 server：kernel `rocm_ernic_query_gid()` | 从本地 `sgid_tbl` 返回 GID |
| `ibv_destroy_qp` | `destroy_qp()` → `destroy_qp_rings()` → `rdma_rm_dealloc_qp()` → `loopback_destroy_qp()` | 先解除 SQ/RQ DMA ring 映射，再清空 backend queues/解除 peer/删除 QP和 resource slot |
| `ibv_dereg_mr` | `destroy_mr()` → `rdma_rm_dealloc_mr()` → `loopback_destroy_mr()` | 从 backend MR table 删除，再 unmap host mapping并释放 resource slot |
| `ibv_destroy_cq` | `destroy_cq()` → `destroy_cq_ring()` → `rdma_rm_dealloc_cq()` → `loopback_destroy_cq()` | 先释放 CQ DMA ring；loopback destroy 当前只记录日志，backend CQ table 项留到 backend finalization；RM 立即释放 guest resource slot |
| `ibv_dealloc_pd` | `destroy_pd()` → `rdma_rm_dealloc_pd()` → `loopback_destroy_pd()` | loopback destroy 当前只记录日志，backend PD table 项留到 backend finalization；RM 立即释放 guest resource slot |
| `ibv_close_device` | `destroy_uc()` → `rdma_rm_dealloc_uc()` | 释放 UC resource slot；guest 随后释放 UAR page |

无 AdminQ 的枚举/名称/列表释放只在 libibverbs/sysfs 中完成，见 5.2。数据面三类 ibv 调用见下一节。

## 7. 数据面 ibv 调用、doorbell 和 CQE

### 7.1 `ibv_post_recv()`

provider 将 `wr_id`、`num_sge` 及每个 SGE 的 `addr/length/lkey` 写入共享 RQ WQE，推进 producer，然后向 UAR page 1 的 QP doorbell 写 `0x80000065`：

`rocm_ernic_post_recv_v()` → RQ ring → BAR2 → `bar2_access()` → `pvrdma_uar_write_impl()` → `pvrdma_qp_recv(101)` → `rdma_backend_post_recv()` → `loopback_post_recv()`。

loopback 将 receive WR 排入 QP101 的 recv queue。该路径不写 BAR1、不使用 AdminQ DMA，也不触发 vector 0。

### 7.2 `ibv_post_send()`

provider 把 SEND opcode、`wr_id`、SGE 列表和信号标志写入共享 SQ WQE，推进 producer，再写 `0x40000065`：

`rocm_ernic_post_send_v()` → SQ ring → BAR2 → `bar2_access()` → `pvrdma_uar_write_impl()` → `pvrdma_qp_send(101)` → `rdma_backend_post_send()` → `loopback_post_send()`。

loopback 根据 `remote_qpn=101` 取到自身已排队的 receive WR，用 MR 的 guest VA/host mapping复制 payload，然后通过 `pvrdma_qp_ops_comp_handler()` 向 CQ2 写 RECV CQE、向 CQ1 写 SEND CQE。11 次 payload 长度之和为：

`1024 + 5×512 + 64 + 256 + 1024 + 2048 + 4096 = 11072`。

### 7.3 `ibv_poll_cq()`

`rocm_ernic_poll_cq_v()` 直接读取 provider mmap 的 CQ ring，转换 CQE 到 `struct ibv_wc` 并推进 consumer。测试创建 CQ 时 `channel=NULL` 且未调用 `ibv_req_notify_cq()`，因此 server 只发布 CQE，不需要 CQ doorbell和 MSI-X vector 2。本轮 send/recv completion 的 `wr_id`、长度、状态均与 post 输入对应。

最终 QP101 统计为 send doorbell=11、recv doorbell=11、WQE=11、CQE=22、sent=received=11072；五项相互约束，能排除“只敲 doorbell 未处理 WQE”或“只复制数据未回 completion”等不完整路径。

## 8. 驱动初始化、卸载及已修复问题的验证

### 8.1 初始化 #1～#14

- #1～#4：RDMA core 触发 `QUERY_PORT/QUERY_PKEY`。
- #5～#8：创建 kernel PD0、DMA MR0、CQ0 和 GSI QP100。
- #9～#12：查询 PKey并把 QP100 切到 INIT/RTR/RTS。
- #13～#14：注册设备过程中再次查询 port。

这些命令解释了为什么测试开始前 vector 0 已经计数 14，也解释了用户资源 handle 从 PD/MR/CQ 的 1 开始、loopback QPN 从 101 开始。

### 8.2 正常卸载 #32～#36

卸载顺序为：#32 `QUERY_PORT`，#33 `DESTROY_QP qp=100`，#34 `DESTROY_CQ cq=0`，#35 `DESTROY_MR mr=0`，#36 `DESTROY_PD pd=0`。每条均收到对应 ack、`err=0` 和 vector 0，之后才写 `IMR=0xffffffff` 与 `CTL=RESET`。

本轮 `rmmod rocm_ernic_rdma rocm_ernic_eth` 为 404 ms，证明 AdminQ IRQ 在资源注销期间仍可用，已修复旧记录中的约 60 秒五/六次超时问题。GID 日志为：

`del_gid ... netdev=none local=1` → `removed local-only GID from table`

因此 local-only GID 不发送错误的 `DESTROY_BIND`；server 在 vfio-user 断连后的 backend cleanup 又正确调用 `loopback_del_gid()`，日志为 `Loopback: Deleted GID index 0`，未再错误落入 legacy `rdma_umadmux`。

QMP `device_del ernic0` 返回成功；guest kernel 与 server 的禁止模式扫描结果均为 `none`。

## 9. 完整性与一致性审查

本轮按以下闭环检查文档和数据：

- **命令数闭环**：14（初始化）+10（setup）+7（cleanup）+5（卸载）=36，与 server `commands=36` 一致。
- **中断闭环**：测试阶段 vector 0 增加 17，与 #15～#31 一致；全生命周期 36 条命令与 `interrupts=36` 一致；vector 1/2 均为 0，符合无 async event、CQ polling 模式。
- **对象闭环**：每个创建 response 的 handle 都用于后续依赖 request并最终由同一 handle 销毁；公开对象指针没有误当作协议 handle。
- **字段闭环**：表中明确区分 ibv 直接输入、对象句柄转换、provider/kernel 派生字段、AdminQ response 字段和 kernel 本地补充的 udata 字段。
- **语义闭环**：modify-QP 区分“已传输字段”和“当前 backend 实际使用字段”，避免把 ABI 可见误认为后端功能已实现。
- **数据闭环**：11 SEND + 11 RECV doorbell、11 WQE、22 CQE、双向各 11072 bytes一致，且 `ALL TESTS PASSED`。
- **资源生命周期闭环**：用户资源 #15～#31 与 kernel 资源 #5～#8/#33～#36 均成功回收；无 timeout、错误 response、GID残留或卸载警告。

在本测试覆盖范围内，`setup_resources()` 的全部 ibv 调用和字段对应已经列全且前后一致。未覆盖的 verbs（SRQ、RDMA READ/WRITE、atomic、CQ notification、async event、多个 QP/跨节点配对）不应由本轮数据外推。

## 10. 追踪文件索引

- `revision.txt`：仓库提交和内外层 kernel 版本。
- `device-layout.txt`：PCI BAR、resource 长度、MSI-X和 RDMA device。
- `bar2-doorbell-counts.txt`：全部 BAR2 写按 offset/value 汇总后的精确计数。
- `adminq-dma-init.log`：DSR/request/response slot 的 DMA 地址与映射长度。
- `adminq-init.log`：#1～#14 初始化命令。
- `adminq-setup-cleanup.log`：#15～#31 的逐条 request/response。
- `adminq-unload.log`：#32～#36 的逐条 request/response。
- `guest-test.log`：全部 `TRACE_IBV`、测试输出和 cleanup。
- `qp101-data-path.log`：QP101 doorbell、SEND复制和 CQE 记录。
- `interrupts-before-test.txt` / `interrupts-after-test.txt`：三个 MSI-X vector 的测试前后计数。
- `phase-counts.txt`：三个阶段的累计 AdminQ 数量。
- `server-stats.txt`：server 和 per-QP 最终统计。
- `unload-validation.log`：卸载耗时、guest teardown、server cleanup和禁止模式扫描。
- `qmp-device-add.txt` / `qmp-device-del.txt`：热插入和热拔出的 QMP 成功响应。

追踪点位于 `src/from-qemu/hw/rdma/vmw/pvrdma_cmd.c`（集中解码 AdminQ request/response）和 `tests/test_data_transfer.c`（记录 ibv 调用实参/结果）。
