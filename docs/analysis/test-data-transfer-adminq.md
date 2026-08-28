# `test_data_transfer`：ERNIC BAR、AdminQ 与 verbs 调用链分析

## 1. 结论与复现范围

- 基线：`3680da7a9bab8e02`；追踪改动：`e263abb debug: add admin queue and verbs tracing`。
- 环境：外层 `debian-13-dev` 启动 vfio-user server，内层 `rocm-ernic-e2e` VM（Debian 13，kernel `6.12.105+deb13-amd64`）加载 `rocm_ernic_eth`/`rocm_ernic_rdma` 与本仓库 provider。
- 后端：`loopback:mode=preserve`。
- 结果：11 组 SEND/RECV、22 个 CQE 全部成功，发送和接收各 11072 bytes，`ALL TESTS PASSED`。
- 本次用户态 `setup_resources()` 产生 AdminQ #15～#24；清理产生 #25～#31。设备初始化产生的 #1～#14 不属于该函数。
- 数据面 `ibv_post_recv()`、`ibv_post_send()` 和 `ibv_poll_cq()` 不使用 AdminQ：前两者写共享 WQE ring 后敲 BAR2 doorbell，后者直接轮询共享 CQ ring。

原始证据位于 `docs/traces/test-data-transfer-20260828/`。

## 2. ernic0 BAR layout

实测 PCI 地址来自 VM 中 `lspci -vv -s 00:0a.0`：

| BAR | Guest PA / 实测长度 | 内容 | 本次行为 |
| --- | --- | --- | --- |
| BAR0 | `0xc0800000`, 16 KiB | MSI-X table：offset `0x0000`，3×16 bytes；PBA：offset `0x2000` | 由 libvfio-user 管理；server 自己的 `bar0_access` 统计为 0 read/0 write |
| BAR1 | `0xc0804000`, 256 bytes | 64 个 32-bit 控制/状态寄存器 | 68 reads、45 writes；承载 DSR 建链、AdminQ doorbell、错误码和全局中断 mask |
| BAR2 | `0xc0000000`, **实测 8 MiB** | UAR；每个 user context 4 KiB page，page 内 QP/CQ/SRQ doorbell 位于 `0/4/8` | 本次用户 context 使用 page 1，即 global offset `0x1000`；QP 101 各 11 次 send/recv doorbell |

驱动加载前 BAR disabled；加载后 `Mem+ BusMaster+ DisINTx+ MSI-X Enable+`。

### 2.1 BAR2 长度实现偏差

`MAX_UCS=512`，且 `RDMA_BAR2_UAR_SIZE=(0x1000 * MAX_UCS)` 已经表示 **2 MiB 字节数**。但 `src/rocm_ernic_server.c:431` 调用 `vfu_setup_region()` 时又乘以 `sizeof(uint32_t)`，因此 PCI 实际暴露 8 MiB；边界检查也用了相同的二次乘法。与此同时：

- `bar2_mem` 只分配 `RDMA_BAR2_UAR_SIZE`（2 MiB）；
- `PVRDMADev::uar_data` 也是 2 MiB；
- 资源表只支持 512 个 UC。

因此设计/逻辑长度是 2 MiB，而 guest 看到 8 MiB。当前测试只访问 `0x1000`，没有触发问题；但 2～8 MiB 范围被错误暴露，应将 server region 长度和 bounds check 中的 `* sizeof(uint32_t)` 去掉。`bar2_mem` 当前不参与 doorbell 转发，所以本次没有直接发生对它的越界读写。

### 2.2 BAR1 寄存器作用

| offset | 寄存器 | 方向 | 行为 |
| --- | --- | --- | --- |
| `0x00` | `VERSION` | R | server 初始化为 `PVRDMA_HW_VERSION=17` |
| `0x04/0x08` | `DSRLOW/DSRHIGH` | W | guest 写 DSR DMA 地址；写 HIGH 时 server 执行 `load_dsr()`，DMA-map DSR、request/response slot 与事件/CQ ring |
| `0x0c` | `CTL` | W | `ACTIVATE=0`、`UNQUIESCE=1`、`RESET=2` |
| `0x10` | `REQUEST` | W | AdminQ doorbell；driver 写 0，server 同步读取 req slot 并执行命令 |
| `0x14` | `ERR` | R | server 写 handler 的 errno，driver 收到 vector 0 后读取 |
| `0x18` | `ICR` | R | legacy interrupt cause；MSI-X 模式下 vector 本身区分用途 |
| `0x1c` | `IMR` | R/W | server 的全局 interrupt mask；0 开启，`0xffffffff` 屏蔽 |
| `0x20/0x24` | `MACL/MACH` | R/W | MAC 地址低/高部分；后续 offset 还有 Ethernet 控制寄存器 |

### 2.3 BAR2 doorbell 编码

每个 UC 占一页；server 对 global offset 取 page 内 `offset & 0xfff`：

- QP doorbell `+0x0`：bit 30=`SEND`，bit 31=`RECV`，低位为 QP handle。
- CQ doorbell `+0x4`：bit 29=`ARM_SOL`，bit 30=`ARM`，bit 31=`POLL`，低位为 CQ handle。
- SRQ doorbell `+0x8`：bit 30=`RECV`，低位为 SRQ handle。

本次 provider 将 UAR page 1 mmap 到用户态。QP 101 (`0x65`) 的实际写值为：

- recv：BAR2 offset `0x1000`，value `0x80000065`；
- send：BAR2 offset `0x1000`，value `0x40000065`。

## 3. AdminQ DMA、寄存器、MSI-X 的协作

AdminQ 不是 BAR 内的一组 payload 寄存器，而是 **guest coherent DMA memory + BAR1 doorbell + MSI-X completion**：

1. 驱动分配 coherent DSR、一个 PAGE_SIZE request slot 和一个 PAGE_SIZE response slot，并把 slot DMA 地址写入 DSR。
2. 驱动把 DSR DMA 地址依次写入 `DSRLOW/DSRHIGH`；server 在 HIGH 写入后通过 vfio-user DMA mapping 得到 DSR、req、rsp 和 ring 的 host pointer。
3. `rocm_ernic_cmd_post()` 用 semaphore 保证同时最多一条命令，将完整 request union memcpy 到 req slot，重新初始化 `cmd_done`，然后向 BAR1 `REQUEST` 写 0。
4. `pvrdma_regs_write()` → `pvrdma_exec_cmd()` 从 DMA req slot 取命令，经 `cmd_handlers[]` 分派；handler 填充 DMA rsp slot，server 设置 `rsp.hdr.response/ack/err` 与 BAR1 `ERR`。
5. server 触发 MSI-X vector 0；guest IRQ handler `complete(&cmd_done)`。等待者读取 `ERR`，然后校验 response `ack` 并复制 payload。
6. 超时为 10 秒；`response` 字段本次始终为 0，实际关联依靠“单 outstanding command + 固定 req/rsp slot”，`ack=0x80000000|cmd` 用于校验命令类型。

### 3.1 MSI-X 三个 vector

| vector | 定义用途 | 本次观测 |
| --- | --- | --- |
| 0 | AdminQ response / command ring | test 前累计 14，test 后 31，**恰好 +17**，对应 #15～#31 每条 AdminQ 一次 |
| 1 | async event | 0 → 0 |
| 2 | CQ completion | 0 → 0 |

测试没有 completion channel，也没有 arm CQ；provider 的 `ibv_poll_cq()` 直接读共享 CQ ring，因此数据完成不会触发 vector 2。server 总计 `interrupts=31` 也与卸载前的 AdminQ 数量一致。

## 4. `setup_resources()` 的 ibv 输入/输出与 AdminQ 对应

完整抓包见 `adminq-setup-cleanup.log`。这里的 “driver 补充” 指不来自原始 ibv 参数、而由 provider/kernel 创建的句柄、页目录或 DMA 地址。

### 4.1 无 AdminQ 的调用

| ibv 调用 | 原因/输出 |
| --- | --- |
| `ibv_get_device_list(&num_devices)` | 只枚举 sysfs/uverbs；输出 1 个 `rocep0s10`，无设备 AdminQ |
| `ibv_query_gid(ctx, 1, 0, &gid)` | kernel `rocm_ernic_query_gid()` 从本地 `sgid_tbl[0]` 复制；得到 `fe80:0000:...:2d6e`，无 server 调用 |
| `ibv_free_device_list()` | 只释放 libibverbs 用户态列表 |

### 4.2 创建路径实测字段映射

| ibv 调用及实参 | AdminQ request（实测） | AdminQ response → ibv 可见输出 |
| --- | --- | --- |
| `ibv_open_device(rocep0s10)` | #15 `CREATE_UC pfn=0xc0001`。PFN 由 kernel UAR allocator 产生，对应 BAR2 page 1，不是 ibv 显式参数 | `ctx=0` 留在 kernel；kernel udata 另返回 `qp_tab_size=max_qp`，provider 构造 context |
| `ibv_alloc_pd(ctx)` | #16 `CREATE_PD ctx=0` | `pd=1` → kernel `pd_handle/pdn=1` → provider `pd->pdn=1`；公开 API 返回 `ibv_pd *` |
| `ibv_create_cq(ctx,16,NULL,NULL,0)` send | #17 `CREATE_CQ ctx=0 cqe=16 nchunks=2 pdir_dma=0x1069fd000`。`cqe=16` 源自实参；provider mmap 8192-byte CQ buffer，kernel pin 后生成其余 DMA 字段 | `cq=1 cqe=16` → udata `cqn=1,ncqe=16,cqe_size=64`；公开 CQ 的 `cqe=16` |
| 同上 recv | #18 同命令，`pdir_dma=0x1807a0000` | `cq=2 cqe=16` → recv CQN 2 |
| `ibv_reg_mr(pd,0x564c52a7f7c0,4096,LOCAL_WRITE)` send | #19 `CREATE_MR pd=1 start=0x564c52a7f7c0 length=4096 access=1 flags=0 nchunks=2 pdir_dma=0xf67df7000`。地址/长度/access 直传；PD 由对象解引用；2 pages 是非页对齐 4 KiB buffer 被 pin 后的结果 | `mr=1 lkey=2 rkey=0xffffffff` → `ibv_mr.{lkey=2,rkey=0xffffffff}`；`mr=1` 仅 kernel 保存供销毁 |
| `ibv_reg_mr(pd,0x564c52a807d0,4096,LOCAL_WRITE)` recv | #20 相同转换，`pdir_dma=0xf680c0000` | `mr=2 lkey=3 rkey=0xffffffff` |
| `ibv_create_qp(pd,&attr)`，RC，SCQ=1/RCQ=2，WR=16/16，SGE=1/1，`sq_sig_all=0` | #21 `CREATE_QP pd=1 scq=1 rcq=2 srq=0 send_wr=16 recv_wr=16 send_sge=1 recv_sge=1 inline=0 sq_sig_all=0 qp_type=2 is_srq=0 chunks=3/1 pdir_dma=0x1610e1000`。provider 创建 SQ/RQ buffer；kernel pin、建 page directory 并将对象指针换成 server handle | `qpn=101` 和协商后 caps 16/16、1/1、inline 0。kernel 另在 udata 中补 `qp_handle=101`、WQE sizes、UAR mmap offset、QP/CQ doorbell offsets；这些不是 AdminQ response 字段 |
| `ibv_modify_qp(...INIT, mask=0x39)` | #22 `MODIFY_QP qp=101 mask=0x39 state=1 pkey=0 port=1 access=0x6` | 仅 header：`ack=0x8000000a err=0` → API 返回 0 |
| `ibv_modify_qp(...RTR, mask=0x129181)` | #23 `state=2 mtu=3 dest_qpn=101 rq_psn=0 max_dest_atomic=1 min_rnr=12 sgid=0 dgid=fe80:...:2d6e` | 仅 header，成功返回 0 |
| `ibv_modify_qp(...RTS, mask=0x12e01)` | #24 `state=3 sq_psn=0 timeout=14 retry=7 rnr_retry=7 max_rd_atomic=1` | 仅 header，成功返回 0 |

注意：loopback backend 内部 MR rkey 是 `handle+0x10000`，但 `rdma_rm_alloc_mr()` 当前固定把 guest response rkey 设成 `-1`，所以 ibv 看到 `0xffffffff`；这是资源管理层的显式行为，不是 trace 丢失。

### 4.3 cleanup 对应

| ibv 调用 | AdminQ | server response |
| --- | --- | --- |
| `ibv_destroy_qp(qpn=101)` | #25 `DESTROY_QP qp=101` | ack `0x8000000c`, err 0 |
| `ibv_dereg_mr(send)` | #26 `DESTROY_MR mr=1` | ack `0x80000005`, err 0 |
| `ibv_dereg_mr(recv)` | #27 `DESTROY_MR mr=2` | 同上 |
| `ibv_destroy_cq(send)` | #28 `DESTROY_CQ cq=1` | ack `0x80000008`, err 0 |
| `ibv_destroy_cq(recv)` | #29 `DESTROY_CQ cq=2` | 同上 |
| `ibv_dealloc_pd()` | #30 `DESTROY_PD pd=1` | ack `0x80000003`, err 0 |
| `ibv_close_device()` | #31 `DESTROY_UC ctx=0` | ack `0x8000000e`, err 0 |

## 5. server 后端处理调用链与行为

所有有 AdminQ 的调用共享前后半段：

`ibv_*` → rocm_ernic provider `ibv_cmd_*` → uverbs → kernel `rocm_ernic_*` → `rocm_ernic_cmd_post()` → BAR1 `REQUEST` → `pvrdma_regs_write()` → `pvrdma_exec_cmd()` → `cmd_handlers[cmd].exec()` → response slot/ERR → MSI-X vector 0 → guest `cmd_done`。

各 handler 的后端链如下：

- `ibv_open_device`：`create_uc()` → `rdma_rm_alloc_uc()`；只分配 UC resource-table handle，不调用 loopback vtable。close 反向 `destroy_uc()` → `rdma_rm_dealloc_uc()`。
- `ibv_alloc_pd`：`create_pd()` → `rdma_rm_alloc_pd()` → `backend_ops->create_pd()` → `loopback_create_pd()`；loopback 分配内部 PD handle，resource manager 返回 guest PD handle 1。销毁时 `rdma_rm_dealloc_pd()` → `loopback_destroy_pd()` → 释放 resource-table slot。
- `ibv_create_cq`：`create_cq()` → `create_cq_ring()`，按 `pdir_dma` DMA-map page directory、page table、ring-state 与 CQE pages，调用 `pvrdma_ring_init()`；随后 `rdma_rm_alloc_cq()` → `loopback_create_cq()`，创建 loopback completion queue。销毁先 `destroy_cq_ring()` unmap DMA，再 `rdma_rm_dealloc_cq()` → `loopback_destroy_cq()`。
- `ibv_reg_mr`：`create_mr()` → `pvrdma_map_to_pdir()` 映射 guest pinned pages → `rdma_rm_alloc_mr()` → `loopback_create_mr()`，记录 guest VA、host mapping、长度、access 和 key；销毁为 `rdma_rm_dealloc_mr()` → `loopback_destroy_mr()` → munmap DMA mapping。
- `ibv_create_qp`：`create_qp()` → `create_qp_rings()` DMA-map统一 page directory；`tbl[0]` 是 SQ/RQ ring state，随后 pages 构成 SQ、RQ WQE ring；再经 `rdma_rm_alloc_qp()` 校验 PD/CQ handle → `loopback_create_qp()`，创建 QPN 101、初态 RESET 和 send/recv queues。销毁先 `destroy_qp_rings()`，再 `rdma_rm_dealloc_qp()` → `loopback_destroy_qp()`，清队列、解除配对并删除 QP。
- INIT：`modify_qp()` → `rdma_rm_modify_qp()` → `loopback_qp_state_init()`；保存 state=INIT/qkey。
- RTR：同链 → `loopback_qp_state_rtr()`；保存 remote QPN=101、DGID、RQ PSN，并为 QP 101 生成 local address `0x1065000` / local rkey `0xffffffff`。
- RTS：同链 → `loopback_qp_state_rts()`；保存 SQ PSN，尝试自动配对。本次单 QP self-loopback 没有另一配对对象，日志为 “waiting for pairing partner”；数据发送时仍按目标 QPN 101 找到自身。
- `ibv_query_gid`：到 kernel `rocm_ernic_query_gid()` 截止，只读 `sgid_tbl`，没有上述 server 链。

## 6. 数据面行为

每个 transfer 的实际链路为：

`ibv_post_recv()` → provider 写 RQ WQE/ring producer → BAR2 `0x80000065` → `bar2_access()` → `pvrdma_uar_write_impl()` → `pvrdma_qp_recv(101)` → `backend_ops->post_recv()` → `loopback_post_recv()` 将 receive WR 排队。

`ibv_post_send()` → provider 写 SQ WQE/ring producer → BAR2 `0x40000065` → `pvrdma_qp_send(101)` 消费 WQE → `loopback_post_send()` 按 QPN 找到目标 QP/RQ，DMA-copy payload → `pvrdma_qp_ops_comp_handler()` 分别向 recv CQ 2、send CQ 1 写 CQE。

`ibv_poll_cq()` → provider 直接从 mmap CQ buffer 的 ring consumer 读取 CQE；本次没有 CQ doorbell，也没有 vector 2。

QP 101 实测统计：

- send doorbell 11，recv doorbell 11；
- 处理 11 个 SEND WQE，发布 22 个 CQE；
- send/recv 都是 11072 bytes：`1024 + 5×512 + 64 + 256 + 1024 + 2048 + 4096`。

server 总 `uar_writes=536`，其中其余 514 次主要来自驱动初始化的 kernel GSI QP 100（512 次 recv doorbell 等），不能归因于 `test_data_transfer` 的 QP 101。

## 7. 卸载阶段发现的独立问题

干净测试结束后执行 `rmmod rocm_ernic_rdma rocm_ernic_eth` 耗时约 61.6 秒。server 实际成功处理 #32～#37（QUERY_PORT、销毁 kernel QP100/CQ0/MR0/PD0/BIND），但 guest 每条等待 10 秒超时。

根因是 `rocm_ernic_detach_from_eth_dev()` 在 `ib_unregister_device()` **之前**调用 `rocm_ernic_disable_intrs()`，向 IMR 写 `0xffffffff`；而 unregister 正是在随后触发这些 AdminQ 清理。server 写好了 response/ERR，但 `post_interrupt()` 看到全局 mask 后不触发 vector 0，因此 `cmd_done` 永远不完成。

这不影响本次 `setup_resources()`/用户资源 cleanup 的正确性；它是卸载顺序缺陷。建议把全局关中断移动到 `ib_unregister_device()` 完成之后，或在清理 AdminQ 期间保留 vector 0。干净卸载及随后 QMP detach 未出现 WARN/Oops；曾出现的 MSI 警告来自一次人为中断 rmmod 后又并发 hot-unplug，不作为正常路径结论。

## 8. 追踪文件索引

- `guest-test.log`：完整 TRACE_IBV、测试输出和 cleanup。
- `adminq-setup-cleanup.log`：精简的 #15～#31 request/response。
- `qp101-data-path.log`：QP 101 doorbell、SEND 与 CQE。
- `device-layout.txt`：驱动加载前后 lspci、resource、MSI-X IRQ 与 RDMA device。
- `server-stats.txt`：server/QP 最终统计。
- `unload-adminq-timeout.log`：卸载阶段 #32～#37。

追踪点位于 `src/from-qemu/hw/rdma/vmw/pvrdma_cmd.c`（集中式 AdminQ 解码）和 `tests/test_data_transfer.c`（ibv 调用前后、参数/返回值）。
