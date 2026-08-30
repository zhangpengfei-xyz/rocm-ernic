/*
 * Mellanox/NVIDIA ConnectX backend.
 *
 * The MLNX backend deliberately uses only the public libibverbs API.  Its
 * distinguishing feature is the strict, read-only identity profile: the
 * configured netdev, RDMA device, IPv4 address and RoCE-v2 GIDs must all
 * describe the same physical port before any guest-visible object exists.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/if.h>
#include <linux/if_arp.h>
#include <linux/memfd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include "rdma_backend.h"
#include "rdma_backend_ops.h"
#include "rdma_rm_defs.h"
#include "rdma_utils.h"

#define MLNX_PROTOCOL_VERSION 21
#define MLNX_ROCE_V2_FLAG     2
#define MLNX_VLAN_UNTAGGED    0xfff

typedef struct MlnxBackend {
    MlnxIdentityProfile profile;
    int lock_fd;
    int guest_gid_map[MAX_PORT_GIDS];
} MlnxBackend;

static int mlnx_get_gid_index(RdmaBackendDev *dev, int guest_idx);

static int read_one_line(const char *path, char *buf, size_t size)
{
    FILE *fp;
    size_t len;

    fp = fopen(path, "re");
    if (!fp) {
        return -errno;
    }
    if (!fgets(buf, (int)size, fp)) {
        int ret = ferror(fp) ? -errno : -EIO;
        fclose(fp);
        return ret;
    }
    fclose(fp);
    len = strlen(buf);
    while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
        buf[--len] = '\0';
    }
    return 0;
}

static int parse_config(const char *config, MlnxIdentityProfile *profile)
{
    char *copy = NULL;
    char *save = NULL;
    char *token;
    bool seen_device = false, seen_ethdev = false, seen_port = false;
    bool seen_roce = false, seen_ip = false, seen_mirror = false;
    int ret = -EINVAL;

    if (!config || !*config) {
        rdma_error_report("MLNX requires device,ethdev,port,roce,ip and mirror-mac");
        return -EINVAL;
    }
    copy = strdup(config);
    if (!copy) {
        return -ENOMEM;
    }

    for (token = strtok_r(copy, ",", &save); token;
         token = strtok_r(NULL, ",", &save)) {
        char *eq = strchr(token, '=');
        const char *key;
        const char *value;
        char *end = NULL;
        unsigned long port;

        if (!eq || eq == token || !eq[1] || strchr(eq + 1, '=')) {
            goto out;
        }
        *eq = '\0';
        key = token;
        value = eq + 1;

        if (!strcmp(key, "device")) {
            if (seen_device || strlen(value) >= sizeof(profile->device)) {
                goto out;
            }
            strcpy(profile->device, value);
            seen_device = true;
        } else if (!strcmp(key, "ethdev")) {
            if (seen_ethdev || strlen(value) >= sizeof(profile->ethdev)) {
                goto out;
            }
            strcpy(profile->ethdev, value);
            seen_ethdev = true;
        } else if (!strcmp(key, "port")) {
            if (seen_port) {
                goto out;
            }
            errno = 0;
            port = strtoul(value, &end, 10);
            if (errno || !end || *end || port != 1) {
                goto out;
            }
            profile->port = (uint8_t)port;
            seen_port = true;
        } else if (!strcmp(key, "roce")) {
            if (seen_roce || strcmp(value, "v2")) {
                goto out;
            }
            seen_roce = true;
        } else if (!strcmp(key, "ip")) {
            if (seen_ip || inet_pton(AF_INET, value, &profile->ipv4_be) != 1) {
                goto out;
            }
            seen_ip = true;
        } else if (!strcmp(key, "mirror-mac")) {
            if (seen_mirror || strcmp(value, "on")) {
                goto out;
            }
            seen_mirror = true;
        } else {
            goto out;
        }
    }

    if (!seen_device || !seen_ethdev || !seen_port || !seen_roce || !seen_ip ||
        !seen_mirror) {
        goto out;
    }
    ret = 0;
out:
    if (ret) {
        rdma_error_report("Invalid MLNX configuration '%s'", config);
    }
    free(copy);
    return ret;
}

static int verify_netdev_identity(MlnxIdentityProfile *profile)
{
    char net_path[PATH_MAX], ib_path[PATH_MAX];
    char net_real[PATH_MAX], ib_real[PATH_MAX];
    struct ifaddrs *ifas = NULL, *ifa;
    struct ifreq ifr = {};
    int fd = -1;
    bool found_ip = false;
    int ret = -EINVAL;

    snprintf(net_path, sizeof(net_path), "/sys/class/net/%s/device",
             profile->ethdev);
    snprintf(ib_path, sizeof(ib_path), "/sys/class/infiniband/%s/device",
             profile->device);
    if (!realpath(net_path, net_real) || !realpath(ib_path, ib_real)) {
        rdma_error_report("Cannot resolve MLNX netdev/RDMA sysfs identity");
        return -errno;
    }
    if (strcmp(net_real, ib_real)) {
        rdma_error_report("MLNX ethdev %s and device %s are on different PCI functions",
                          profile->ethdev, profile->device);
        return -EINVAL;
    }
    snprintf(profile->pci_bdf, sizeof(profile->pci_bdf), "%s",
             strrchr(net_real, '/') ? strrchr(net_real, '/') + 1 : net_real);

    fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -errno;
    }
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", profile->ethdev);
    if (ioctl(fd, SIOCGIFHWADDR, &ifr)) {
        ret = -errno;
        goto out;
    }
    if (ifr.ifr_hwaddr.sa_family != ARPHRD_ETHER) {
        ret = -EOPNOTSUPP;
        goto out;
    }
    memcpy(profile->mac, ifr.ifr_hwaddr.sa_data, sizeof(profile->mac));
    if ((profile->mac[0] & 1) ||
        !memcmp(profile->mac, (const uint8_t[6]){0}, sizeof(profile->mac))) {
        rdma_error_report("MLNX requires a valid unicast Ethernet MAC");
        ret = -EINVAL;
        goto out;
    }
    if (ioctl(fd, SIOCGIFMTU, &ifr)) {
        ret = -errno;
        goto out;
    }
    profile->ethernet_mtu = (uint32_t)ifr.ifr_mtu;

    if (getifaddrs(&ifas)) {
        ret = -errno;
        goto out;
    }
    for (ifa = ifas; ifa; ifa = ifa->ifa_next) {
        struct sockaddr_in *sin;
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET ||
            strcmp(ifa->ifa_name, profile->ethdev)) {
            continue;
        }
        sin = (struct sockaddr_in *)ifa->ifa_addr;
        if (sin->sin_addr.s_addr == profile->ipv4_be) {
            found_ip = true;
            break;
        }
    }
    if (!found_ip) {
        rdma_error_report("Configured MLNX IP is not assigned to %s",
                          profile->ethdev);
        ret = -EADDRNOTAVAIL;
        goto out;
    }
    ret = 0;
out:
    freeifaddrs(ifas);
    close(fd);
    return ret;
}

static bool gid_is_rocev2(const char *device, uint8_t port, int index)
{
    char path[PATH_MAX], type[32];

    snprintf(path, sizeof(path),
             "/sys/class/infiniband/%s/ports/%u/gid_attrs/types/%d", device,
             port, index);
    return read_one_line(path, type, sizeof(type)) == 0 &&
           !strcmp(type, "RoCE v2");
}

static int find_gid(RdmaBackendDev *dev, const union ibv_gid *wanted)
{
    struct ibv_port_attr port_attr;
    int found = -1;
    int i;

    if (ibv_query_port(dev->context, dev->port_num, &port_attr)) {
        return -errno;
    }
    for (i = 0; i < port_attr.gid_tbl_len; i++) {
        union ibv_gid gid;
        if (!ibv_query_gid(dev->context, dev->port_num, i, &gid) &&
            !memcmp(&gid, wanted, sizeof(gid)) &&
            gid_is_rocev2(dev->identity->device, dev->port_num, i)) {
            if (found >= 0) {
                return -ENOTUNIQ;
            }
            found = i;
        }
    }
    return found >= 0 ? found : -ENOENT;
}

static int verify_runtime_prerequisites(void)
{
    struct rlimit limit;
    void *source = MAP_FAILED;
    void *alias = MAP_FAILED;
    int fd = -1;
    int ret = -EOPNOTSUPP;

    if (sysconf(_SC_PAGESIZE) != PAGE_SIZE) {
        rdma_error_report("MLNX requires a 4096-byte host base page");
        return -EOPNOTSUPP;
    }
    if (getrlimit(RLIMIT_MEMLOCK, &limit)) {
        return -errno;
    }
    if (limit.rlim_cur != RLIM_INFINITY && limit.rlim_cur < MAX_MR_SIZE) {
        rdma_error_report("MLNX memlock limit is too small (%llu < %lu)",
                          (unsigned long long)limit.rlim_cur,
                          (unsigned long)MAX_MR_SIZE);
        return -ENOMEM;
    }

    fd = memfd_create("rocm-ernic-mlnx-probe", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, PAGE_SIZE)) {
        ret = -errno;
        goto out;
    }
    source = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    alias = mmap(NULL, PAGE_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1,
                 0);
    if (source == MAP_FAILED || alias == MAP_FAILED) {
        ret = -errno;
        goto out;
    }
    ((uint8_t *)source)[0] = 0x5a;
    if (mremap(source, PAGE_SIZE, PAGE_SIZE,
               MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP,
               alias) != alias || ((uint8_t *)alias)[0] != 0x5a) {
        ret = errno ? -errno : -EIO;
        goto out;
    }
    ((uint8_t *)alias)[1] = 0xa5;
    if (((uint8_t *)source)[1] != 0xa5) {
        ret = -EIO;
        goto out;
    }
    ret = 0;
out:
    if (alias != MAP_FAILED) {
        munmap(alias, PAGE_SIZE);
    }
    if (source != MAP_FAILED) {
        munmap(source, PAGE_SIZE);
    }
    if (fd >= 0) {
        close(fd);
    }
    return ret;
}

static void build_expected_gids(MlnxIdentityProfile *p)
{
    memset(&p->link_local_gid, 0, sizeof(p->link_local_gid));
    p->link_local_gid.raw[0] = 0xfe;
    p->link_local_gid.raw[1] = 0x80;
    p->link_local_gid.raw[8] = p->mac[0] ^ 0x02;
    p->link_local_gid.raw[9] = p->mac[1];
    p->link_local_gid.raw[10] = p->mac[2];
    p->link_local_gid.raw[11] = 0xff;
    p->link_local_gid.raw[12] = 0xfe;
    p->link_local_gid.raw[13] = p->mac[3];
    p->link_local_gid.raw[14] = p->mac[4];
    p->link_local_gid.raw[15] = p->mac[5];

    memset(&p->ipv4_gid, 0, sizeof(p->ipv4_gid));
    p->ipv4_gid.raw[10] = 0xff;
    p->ipv4_gid.raw[11] = 0xff;
    memcpy(&p->ipv4_gid.raw[12], &p->ipv4_be, sizeof(p->ipv4_be));
}

static int open_exclusive_lock(MlnxBackend *priv)
{
    char path[PATH_MAX];

    snprintf(path, sizeof(path), "/run/lock/rocm-ernic-%s-p%u.lock",
             priv->profile.pci_bdf, priv->profile.port);
    priv->lock_fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (priv->lock_fd < 0) {
        return -errno;
    }
    if (flock(priv->lock_fd, LOCK_EX | LOCK_NB)) {
        int ret = errno == EWOULDBLOCK ? -EBUSY : -errno;
        close(priv->lock_fd);
        priv->lock_fd = -1;
        return ret;
    }
    return 0;
}

static int mlnx_init(RdmaBackendDev *dev, const char *config)
{
    MlnxBackend *priv;
    struct ibv_device **list = NULL;
    struct ibv_port_attr port_attr;
    int count = 0, i, ret;

    priv = calloc(1, sizeof(*priv));
    if (!priv) {
        return -ENOMEM;
    }
    priv->lock_fd = -1;
    for (i = 0; i < MAX_PORT_GIDS; i++) {
        priv->guest_gid_map[i] = -1;
    }
    ret = parse_config(config, &priv->profile);
    if (ret) {
        goto fail;
    }
    ret = verify_netdev_identity(&priv->profile);
    if (ret) {
        goto fail;
    }
    ret = verify_runtime_prerequisites();
    if (ret) {
        goto fail;
    }
    ret = open_exclusive_lock(priv);
    if (ret) {
        rdma_error_report("MLNX physical port %s/%u is already in use",
                          priv->profile.pci_bdf, priv->profile.port);
        goto fail;
    }

    list = ibv_get_device_list(&count);
    if (!list) {
        ret = -errno;
        goto fail;
    }
    for (i = 0; i < count; i++) {
        if (!strcmp(ibv_get_device_name(list[i]), priv->profile.device)) {
            dev->ib_dev = list[i];
            break;
        }
    }
    if (!dev->ib_dev) {
        ret = -ENODEV;
        goto fail_list;
    }
    dev->context = ibv_open_device(dev->ib_dev);
    if (!dev->context) {
        ret = -errno;
        goto fail_list;
    }
    dev->port_num = priv->profile.port;
    dev->identity = &priv->profile;
    if (ibv_query_port(dev->context, dev->port_num, &port_attr)) {
        ret = -errno;
        goto fail_context;
    }
    if (port_attr.link_layer != IBV_LINK_LAYER_ETHERNET) {
        ret = -EOPNOTSUPP;
        goto fail_context;
    }
    if (port_attr.state != IBV_PORT_ACTIVE || !port_attr.active_mtu) {
        rdma_error_report("MLNX physical port is not active");
        ret = -ENETDOWN;
        goto fail_context;
    }
    priv->profile.active_mtu = port_attr.active_mtu;
    priv->profile.port_state = port_attr.state;
    priv->profile.generation = 1;
    build_expected_gids(&priv->profile);
    priv->profile.link_local_gid_index =
        find_gid(dev, &priv->profile.link_local_gid);
    priv->profile.ipv4_gid_index = find_gid(dev, &priv->profile.ipv4_gid);
    if (priv->profile.link_local_gid_index < 0 ||
        priv->profile.ipv4_gid_index < 0) {
        rdma_error_report("MLNX required RoCE-v2 identity GID is missing");
        ret = -ENOENT;
        goto fail_context;
    }
    dev->channel = ibv_create_comp_channel(dev->context);
    if (!dev->channel) {
        ret = -errno;
        goto fail_context;
    }
    dev->backend_private = priv;
    dev->feature_flags = RDMA_BACKEND_F_IDENTITY_MIRROR |
                         RDMA_BACKEND_F_GID_BIND_REQUIRED;
    dev->protocol_version = MLNX_PROTOCOL_VERSION;
    dev->comp_thread.run = false;
    dev->comp_thread.is_running = false;
    ibv_free_device_list(list);
    rdma_info_report("MLNX identity: %s/%s port=%u PCI=%s MAC=%02x:%02x:%02x:%02x:%02x:%02x IPv4-GID-index=%d active-mtu=%u",
                     priv->profile.device, priv->profile.ethdev,
                     priv->profile.port, priv->profile.pci_bdf,
                     priv->profile.mac[0], priv->profile.mac[1],
                     priv->profile.mac[2], priv->profile.mac[3],
                     priv->profile.mac[4], priv->profile.mac[5],
                     priv->profile.ipv4_gid_index,
                     128U << priv->profile.active_mtu);
    return 0;

fail_context:
    ibv_close_device(dev->context);
    dev->context = NULL;
fail_list:
    ibv_free_device_list(list);
fail:
    if (priv->lock_fd >= 0) {
        close(priv->lock_fd);
    }
    free(priv);
    dev->identity = NULL;
    return ret;
}

static void mlnx_fini(RdmaBackendDev *dev)
{
    MlnxBackend *priv = dev->backend_private;

    if (dev->comp_thread.run || dev->comp_thread.is_running) {
        rdma_backend_stop(dev);
    }
    if (dev->channel) {
        ibv_destroy_comp_channel(dev->channel);
        dev->channel = NULL;
    }
    if (dev->context) {
        ibv_close_device(dev->context);
        dev->context = NULL;
    }
    if (priv) {
        if (priv->lock_fd >= 0) {
            close(priv->lock_fd);
        }
        free(priv);
    }
    dev->backend_private = NULL;
    dev->identity = NULL;
}

static int mlnx_query_port(RdmaBackendDev *dev, struct ibv_port_attr *attr)
{
    MlnxBackend *priv = dev->backend_private;
    bool ready = false;
    int i;
    int ret = ibv_query_port(dev->context, dev->port_num, attr);

    if (ret) {
        return -errno;
    }
    /* The selected IPv4 binding is the RDMA readiness gate. */
    for (i = 0; i < MAX_PORT_GIDS; i++) {
        if (priv->guest_gid_map[i] == priv->profile.ipv4_gid_index) {
            ready = true;
            break;
        }
    }
    if (!ready) {
        attr->state = IBV_PORT_INIT;
    }
    attr->gid_tbl_len = 2;
    attr->pkey_tbl_len = 1;
    attr->port_cap_flags = 0;
    return 0;
}

static int mlnx_query_device(RdmaBackendDev *dev, struct ibv_device_attr *attr)
{
    struct ibv_device_attr host;

    if (ibv_query_device(dev->context, &host)) {
        return -errno;
    }
#define CAP_MIN(member) do { if (attr->member > host.member) attr->member = host.member; } while (0)
    CAP_MIN(max_mr_size);
    CAP_MIN(max_qp);
    CAP_MIN(max_cq);
    CAP_MIN(max_mr);
    CAP_MIN(max_pd);
    CAP_MIN(max_sge);
    CAP_MIN(max_qp_wr);
    CAP_MIN(max_cqe);
#undef CAP_MIN
    attr->page_size_cap = PAGE_SIZE;
    attr->max_qp_rd_atom = 0;
    attr->max_qp_init_rd_atom = 0;
    attr->max_ah = 0;
    attr->max_srq = 0;
    attr->max_srq_wr = 0;
    attr->max_srq_sge = 0;
    attr->atomic_cap = IBV_ATOMIC_NONE;
    return 0;
}

static int mlnx_create_mr(RdmaBackendMR *mr, RdmaBackendPD *pd, void *addr,
                          size_t length, uint64_t iova, int access)
{
    if (!addr || !length || (access & ~IBV_ACCESS_LOCAL_WRITE) ||
        (((uintptr_t)addr ^ iova) & (PAGE_SIZE - 1))) {
        return (access & ~IBV_ACCESS_LOCAL_WRITE) ? -EOPNOTSUPP : -EINVAL;
    }
    mr->ibmr = ibv_reg_mr_iova(pd->ibpd, addr, length, iova, access);
    if (!mr->ibmr) {
        return -errno;
    }
    mr->ibpd = pd->ibpd;
    return 0;
}

static int mlnx_create_qp(RdmaBackendQP *qp, uint8_t qp_type,
                          RdmaBackendPD *pd, RdmaBackendCQ *scq,
                          RdmaBackendCQ *rcq, RdmaBackendSRQ *srq,
                          uint32_t max_send_wr, uint32_t max_recv_wr,
                          uint32_t max_send_sge, uint32_t max_recv_sge)
{
    if (qp_type != IBV_QPT_RC || srq) {
        return -EOPNOTSUPP;
    }
    return rdma_backend_create_qp(qp, qp_type, pd, scq, rcq, NULL,
                                  max_send_wr, max_recv_wr, max_send_sge,
                                  max_recv_sge);
}

static bool ipv4_mapped_gid(const union ibv_gid *gid)
{
    static const uint8_t prefix[12] = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff,
    };

    return !memcmp(gid->raw, prefix, sizeof(prefix));
}

static int mlnx_modify_qp(RdmaBackendDev *dev, RdmaBackendQP *qp,
                          uint8_t qp_type, uint32_t mask,
                          const RdmaBackendQpAttr *a)
{
    MlnxBackend *priv = dev->backend_private;
    struct ibv_qp_attr current = {}, out = {};
    struct ibv_qp_init_attr init = {};
    uint32_t required, allowed;
    int host_gid_idx;

    if (qp_type != IBV_QPT_RC || !qp->ibqp || !(mask & IBV_QP_STATE)) {
        return -EOPNOTSUPP;
    }
    if (ibv_query_qp(qp->ibqp, &current, IBV_QP_STATE, &init)) {
        return -errno;
    }

    switch (a->state) {
    case IBV_QPS_INIT:
        required = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                   IBV_QP_ACCESS_FLAGS;
        allowed = required | IBV_QP_CUR_STATE;
        if ((mask & required) != required || (mask & ~allowed) ||
            current.qp_state != IBV_QPS_RESET || a->pkey_index != 0 ||
            a->port_num != dev->port_num || a->access_flags != 0) {
            return -EINVAL;
        }
        out.qp_state = IBV_QPS_INIT;
        out.pkey_index = 0;
        out.port_num = dev->port_num;
        out.qp_access_flags = 0;
        break;

    case IBV_QPS_RTR:
        required = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                   IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                   IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
        allowed = required | IBV_QP_CUR_STATE;
        host_gid_idx = mlnx_get_gid_index(dev, a->sgid_index);
        rdma_info_report("MLNX RTR validate: current=%u guest_sgid=%u host_sgid=%d mtu=%u/%u qpn=%u psn=%u max_dest=%u hop=%u tc=%u flow=%u sl=%u dlid=%u pathbits=%u flags=%u ah_port=%u",
                         current.qp_state, a->sgid_index, host_gid_idx,
                         a->path_mtu, priv->profile.active_mtu, a->dest_qpn,
                         a->rq_psn, a->max_dest_rd_atomic, a->hop_limit,
                         a->traffic_class, a->flow_label, a->sl, a->dlid,
                         a->src_path_bits, a->ah_flags, a->ah_port_num);
        if ((mask & required) != required || (mask & ~allowed) ||
            current.qp_state != IBV_QPS_INIT || host_gid_idx < 0 ||
            host_gid_idx != priv->profile.ipv4_gid_index ||
            a->path_mtu > priv->profile.active_mtu || !a->path_mtu ||
            a->dest_qpn >= (1U << 24) || a->rq_psn >= (1U << 24) ||
            a->max_dest_rd_atomic != 0 || !ipv4_mapped_gid(&a->dgid) ||
            (a->hop_limit != 1 && a->hop_limit != 64) ||
            a->traffic_class != 0 ||
            a->flow_label != 0 || a->sl != 0 || a->dlid != 0 ||
            a->src_path_bits != 0 || a->ah_flags != 1 ||
            a->ah_port_num != dev->port_num) {
            return -EINVAL;
        }
        out.qp_state = IBV_QPS_RTR;
        out.path_mtu = a->path_mtu;
        out.dest_qp_num = a->dest_qpn;
        out.rq_psn = a->rq_psn;
        out.max_dest_rd_atomic = 0;
        out.min_rnr_timer = a->min_rnr_timer;
        out.ah_attr.is_global = 1;
        out.ah_attr.port_num = dev->port_num;
        out.ah_attr.sl = 0;
        out.ah_attr.src_path_bits = 0;
        out.ah_attr.dlid = 0;
        out.ah_attr.grh.dgid = a->dgid;
        out.ah_attr.grh.sgid_index = host_gid_idx;
        out.ah_attr.grh.hop_limit = 1;
        break;

    case IBV_QPS_RTS:
        required = IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT |
                   IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
                   IBV_QP_MAX_QP_RD_ATOMIC;
        allowed = required | IBV_QP_CUR_STATE;
        if ((mask & required) != required || (mask & ~allowed) ||
            current.qp_state != IBV_QPS_RTR || a->sq_psn >= (1U << 24) ||
            a->max_rd_atomic != 0 || a->retry_cnt > 7 ||
            a->rnr_retry > 7 || a->timeout > 31) {
            return -EINVAL;
        }
        out.qp_state = IBV_QPS_RTS;
        out.sq_psn = a->sq_psn;
        out.timeout = a->timeout;
        out.retry_cnt = a->retry_cnt;
        out.rnr_retry = a->rnr_retry;
        out.max_rd_atomic = 0;
        break;

    case IBV_QPS_ERR:
    case IBV_QPS_RESET:
        allowed = IBV_QP_STATE | IBV_QP_CUR_STATE;
        required = IBV_QP_STATE;
        if ((mask & required) != required || (mask & ~allowed)) {
            return -EOPNOTSUPP;
        }
        out.qp_state = a->state;
        break;

    default:
        return -EOPNOTSUPP;
    }

    if (ibv_modify_qp(qp->ibqp, &out, mask & ~IBV_QP_CUR_STATE)) {
        return -errno;
    }
    return 0;
}

static int mlnx_add_gid(RdmaBackendDev *dev, const char *ifname,
                        union ibv_gid *gid, int gid_idx, uint8_t gid_type,
                        uint32_t vlan, uint32_t mtu)
{
    MlnxBackend *priv = dev->backend_private;
    int host_idx;
    uint32_t active_mtu_bytes = 128U << priv->profile.active_mtu;

    if (!ifname || strcmp(ifname, priv->profile.ethdev) || gid_idx < 0 ||
        gid_idx >= MAX_PORT_GIDS || gid_type != MLNX_ROCE_V2_FLAG ||
        vlan != MLNX_VLAN_UNTAGGED || mtu != active_mtu_bytes) {
        return -EINVAL;
    }
    if (!memcmp(gid, &priv->profile.ipv4_gid, sizeof(*gid))) {
        host_idx = priv->profile.ipv4_gid_index;
    } else if (!memcmp(gid, &priv->profile.link_local_gid, sizeof(*gid))) {
        host_idx = priv->profile.link_local_gid_index;
    } else {
        return -EACCES;
    }
    if (priv->guest_gid_map[gid_idx] >= 0 &&
        priv->guest_gid_map[gid_idx] != host_idx) {
        return -EEXIST;
    }
    priv->guest_gid_map[gid_idx] = host_idx;
    return 0;
}

static int mlnx_del_gid(RdmaBackendDev *dev, const char *ifname, int gid_idx)
{
    MlnxBackend *priv = dev->backend_private;
    if (!ifname || strcmp(ifname, priv->profile.ethdev) || gid_idx < 0 ||
        gid_idx >= MAX_PORT_GIDS) {
        return -EINVAL;
    }
    priv->guest_gid_map[gid_idx] = -1;
    return 0;
}

static int mlnx_get_gid_index(RdmaBackendDev *dev, int guest_idx)
{
    MlnxBackend *priv = dev->backend_private;
    if (guest_idx < 0 || guest_idx >= MAX_PORT_GIDS) {
        return -EINVAL;
    }
    return priv->guest_gid_map[guest_idx];
}

const RdmaBackendOps rdma_backend_ops_mlnx = {
    .name = "mlnx",
    .type = RDMA_BACKEND_TYPE_VERBS,
    .init = mlnx_init,
    .fini = mlnx_fini,
    .query_port = mlnx_query_port,
    .query_device = mlnx_query_device,
    .create_pd = rdma_backend_create_pd,
    .destroy_pd = rdma_backend_destroy_pd,
    .create_mr = mlnx_create_mr,
    .destroy_mr = rdma_backend_destroy_mr,
    .mr_lkey = rdma_backend_mr_lkey,
    .mr_rkey = rdma_backend_mr_rkey,
    .create_cq = rdma_backend_create_cq,
    .destroy_cq = rdma_backend_destroy_cq,
    .poll_cq = NULL,
    .create_qp = mlnx_create_qp,
    .destroy_qp = rdma_backend_destroy_qp,
    .qpn = rdma_backend_qpn,
    .modify_qp = mlnx_modify_qp,
    .qp_state_init = rdma_backend_qp_state_init,
    .qp_state_rtr = rdma_backend_qp_state_rtr,
    .qp_state_rts = rdma_backend_qp_state_rts,
    .query_qp = rdma_backend_query_qp,
    .query_remote_conn_info = NULL,
    .post_send = NULL,
    .post_recv = NULL,
    .add_gid = mlnx_add_gid,
    .del_gid = mlnx_del_gid,
    .get_backend_gid_index = mlnx_get_gid_index,
};
