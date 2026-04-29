#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/list.h>
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/percpu.h>

#define BUF_SIZE 256

// Standard bandwidth controller registers
#define BC_CAPABILITIES_OFFSET 0x000
#define BC_MON_CTL_OFFSET      0x008
#define BC_MON_CTR_OFFSET      0x010
#define BC_ALLOC_CTL_OFFSET    0x018
#define BC_BW_ALLOC_OFFSET     0x020
// Below two are custom additions
#define GLOBAL_EN_OFFSET       0x100
#define PERIOD_LEN_OFFSET      0x108

// NOTE: Gaps are intentional, those bits are reserved/for custom use
// Read only capabilities
#define BC_CAPABILITIES_VER     GENMASK_ULL(7, 0)
#define BC_CAPABILITIES_NBWBLKS GENMASK_ULL(23, 8)
#define BC_CAPABILITIES_RPFX    BIT_ULL(24)
#define BC_CAPABILITIES_P       GENMASK_ULL(28, 25)
#define BC_CAPABILITIES_MRBWB   GENMASK_ULL(47,32)

// Monitoring config
#define BC_MON_CTL_OP_lo     GENMASK(4, 0)
#define BC_MON_CTL_AT_lo     GENMASK(7, 5)
#define BC_MON_CTL_MCID_lo   GENMASK(19, 8)
#define BC_MON_CTL_EVT_ID_lo GENMASK(27, 20)
#define BC_MPN_CTL_ATV_lo    BIT(28)
#define BC_MON_CTL_STATUS_hi GENMASK(6, 0)
#define BC_MON_CTL_BUSY_hi   BIT(7)

// Monitoring counter offsets
#define BC_MON_CTR_VAL GENMASK_ULL(61, 0)
#define BC_MON_CTR_INV BIT_ULL(62)
#define BC_MON_CTR_OVF BIT_ULL(63)

// BW allocation control offsets
#define BC_ALLOC_CTL_OP_lo     GENMASK(4, 0)
#define BC_ALLOC_CTL_AT_lo     GENMASK(7, 5)
#define BC_ALLOC_CTL_RCID_lo   GENMASK(19, 8)
#define BC_ALLOC_CTL_STATUS_hi GENMASK(6, 0)
#define BC_ALLOC_CTL_BUSY_hi   BIT(7)

// BW allocation config offsets
#define BC_BW_ALLOC_RBWB      GENMASK_ULL(15, 0)
#define BC_BW_ALLOC_MWEIGHT   GENMASK_ULL(27, 20)
#define BC_BW_ALLOC_SHAREDAT  GENMASK_ULL(30, 28)
#define BC_BW_ALLOC_USESHARED BIT_ULL(31)

DEFINE_PER_CPU(u64, srmcfg_val);

#define read_csr(reg) ({ unsigned long __tmp; \
  asm volatile ("csrr %0, " #reg : "=r"(__tmp)); \
  __tmp; })

#define write_csr(reg, val) ({ \
  if (__builtin_constant_p(val) && (unsigned long)(val) < 32) \
    asm volatile ("csrw " #reg ", %0" :: "i"(val)); \
  else \
    asm volatile ("csrw " #reg ", %0" :: "r"(val)); })

enum controller_type {
    CACHE_BANDWIDTH,
    MEM_BANDWIDTH,
    CACHE_CAPACITY,

    UNKOWN
};

struct controller {
    u32 nrcid;
    u32 nmcid;
    unsigned long base_addr;
    unsigned long size;
    struct list_head list;
    enum controller_type type;

    void __iomem *mapped_base;
};

static struct list_head controllers;

static struct dentry *top_dir;
static struct dentry *cache_bw_dir;
static struct dentry *mem_bw_dir;

// Use debugfs to configure, links below are useful and so is the memguard code
// https://www.kernel.org/doc/html/latest/filesystems/debugfs.html
// https://linux-kernel-labs.github.io/refs/heads/master/labs/device_drivers.html
// https://www.kernel.org/doc/html/v6.14-rc7/filesystems/seq_file.html
// https://linux-kernel-labs.github.io/refs/heads/master/labs/device_drivers.html

static int global_enable_show(struct seq_file *m, void *p) {
    struct controller *info = m->private;

    // this should use the offset in info mapped io
    seq_printf(m, "enabled: %s\n", (readq(info->mapped_base + GLOBAL_EN_OFFSET) ? "on" : "off"));

    return 0;
}

static int global_enable_open(struct inode *inode, struct file *file) {
    // passing the i_private (data field) into data field here gives us access to it in the private field of the seq_file in show
    return single_open(file, global_enable_show, inode->i_private);
}

static ssize_t global_enable_write(struct file *file, const char __user *user_buff, size_t size, loff_t *offset) {
    char buf[BUF_SIZE];
    char *p = buf;
    u64 field;

    struct seq_file *m = file->private_data;
    struct controller *info = m->private;

    if (copy_from_user(&buf, user_buff, (size > BUF_SIZE ? BUF_SIZE : size))) {
        return 0;
    }

    sscanf(p, "%llu", &field);
    writeq(field, info->mapped_base + GLOBAL_EN_OFFSET);

    return size;
}

const struct file_operations global_enable_fops = {
    .open = global_enable_open,
    .write = global_enable_write,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

static int period_len_show(struct seq_file *m, void *p) {
    struct controller *info = m->private;

    seq_printf(m, "period (cycles): %lld\n", readq(info->mapped_base + PERIOD_LEN_OFFSET));

    return 0;
}

static int period_len_open(struct inode *inode, struct file *file) {
    return single_open(file, period_len_show, inode->i_private);
}

static ssize_t period_len_write(struct file *file, const char __user *user_buff, size_t size, loff_t *offset) {
    char buf[BUF_SIZE];
    char *p = buf;
    u64 field;

    struct seq_file *m = file->private_data;
    struct controller *info = m->private;

    if (copy_from_user(&buf, user_buff, (size > BUF_SIZE ? BUF_SIZE : size))) {
        return 0;
    }

    sscanf(p, "%llu", &field);
    writeq(field, info->mapped_base + PERIOD_LEN_OFFSET);

    return size;
}

const struct file_operations period_len_fops = {
    .open = period_len_open,
    .write = period_len_write,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

static int bc_capabilities_show(struct seq_file *m, void *p) {
    struct controller *info = m->private;
    u64 reg = readq(info->mapped_base + BC_CAPABILITIES_OFFSET);

    seq_printf(m, "version: %llu\n", (unsigned long long)FIELD_GET(BC_CAPABILITIES_VER, reg));
    seq_printf(m, "nbwblks: %llu\n", (unsigned long long)FIELD_GET(BC_CAPABILITIES_NBWBLKS, reg));
    seq_printf(m, "rpfx: %llu\n", (unsigned long long)FIELD_GET(BC_CAPABILITIES_RPFX, reg));
    seq_printf(m, "p: %llu\n", (unsigned long long)FIELD_GET(BC_CAPABILITIES_P, reg));
    seq_printf(m, "mrbwb: %llu\n", (unsigned long long)FIELD_GET(BC_CAPABILITIES_MRBWB, reg));

    return 0;
}

static int bc_capabilities_open(struct inode *inode, struct file *file) {
    return single_open(file, bc_capabilities_show, inode->i_private);
}

static ssize_t bc_capabilities_write(struct file *file, const char __user *user_buff, size_t size, loff_t *offset) {
    pr_info("Writes not supported to capabilities register");
    return -EINVAL;
}

const struct file_operations bc_capabilities_fops = {
    .open = bc_capabilities_open,
    .write = bc_capabilities_write,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

static int bc_mon_ctl_show(struct seq_file *m, void *p) {
    struct controller *info = m->private;
    u32 reg = readl(info->mapped_base + BC_MON_CTL_OFFSET + 4);

    seq_printf(m, "busy: %s\n", FIELD_GET(BC_MON_CTL_BUSY_hi, reg) ? "yes" : "no");
    seq_printf(m, "status: %lu\n", (unsigned long)FIELD_GET(BC_MON_CTL_STATUS_hi, reg));

    return 0;
}

static int bc_mon_ctl_open(struct inode *inode, struct file *file) {
    return single_open(file, bc_mon_ctl_show, inode->i_private);
}

static ssize_t bc_mon_ctl_write(struct file *file, const char __user *user_buff, size_t size, loff_t *offset) {
    struct seq_file *m = file->private_data;
    struct controller *info = m->private;
    u32 op;
    u32 mcid;
    u32 evt_id;
    char buf[BUF_SIZE];
    size_t end = size > BUF_SIZE ? BUF_SIZE - 1 : size;

    if (copy_from_user(&buf, user_buff, end)) {
        return -EFAULT;
    }
    buf[end] = '\0';

    if (3 != sscanf(buf, "%u %u %u", &op, &mcid, &evt_id)) {
        return -EINVAL;
    }

    u32 reg = FIELD_PREP(BC_MON_CTL_OP_lo, op) |
              FIELD_PREP(BC_MON_CTL_MCID_lo, mcid) |
              FIELD_PREP(BC_MON_CTL_EVT_ID_lo, evt_id);
    writel(reg, info->mapped_base + BC_MON_CTL_OFFSET);

    return size;
}

const struct file_operations bc_mon_ctl_fops = {
    .open = bc_mon_ctl_open,
    .write = bc_mon_ctl_write,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

static int bc_mon_ctr_show(struct seq_file *m, void *p) {
    struct controller *info = m->private;
    u64 reg = readq(info->mapped_base + BC_MON_CTR_OFFSET);

    seq_printf(m, "value: %llu\n", (unsigned long long)FIELD_GET(BC_MON_CTR_VAL, reg));
    seq_printf(m, "invalid: %lu\n", (unsigned long)FIELD_GET(BC_MON_CTR_INV, reg));
    seq_printf(m, "overflow: %lu\n", (unsigned long)FIELD_GET(BC_MON_CTR_OVF, reg));

    return 0;
}

static int bc_mon_ctr_open(struct inode *inode, struct file *file) {
    return single_open(file, bc_mon_ctr_show, inode->i_private);
}

static ssize_t bc_mon_ctr_write(struct file *file, const char __user *user_buff, size_t size, loff_t *offset) {
    pr_info("Write operations not supported for bc_mon_ctr");
    return -EINVAL;
}

const struct file_operations bc_mon_ctr_fops = {
    .open = bc_mon_ctr_open,
    .write = bc_mon_ctr_write,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

static int bc_alloc_ctl_show(struct seq_file *m, void *p) {
    struct controller *info = m->private;
    u32 reg = readl(info->mapped_base + BC_ALLOC_CTL_OFFSET + 4);

    seq_printf(m, "busy: %s\n", FIELD_GET(BC_ALLOC_CTL_BUSY_hi, reg) ? "yes" : "no");
    seq_printf(m, "status: %lu\n", (unsigned long)FIELD_GET(BC_ALLOC_CTL_STATUS_hi, reg));

    return 0;
}

static int bc_alloc_ctl_open(struct inode *inode, struct file *file) {
    return single_open(file, bc_alloc_ctl_show, inode->i_private);
}

static ssize_t bc_alloc_ctl_write(struct file *file, const char __user *user_buff, size_t size, loff_t *offset) {
    struct seq_file *m = file->private_data;
    struct controller *info = m->private;
    u32 op;
    u32 rcid;
    char buf[BUF_SIZE];
    size_t end = size > BUF_SIZE ? BUF_SIZE - 1 : size;

    if (copy_from_user(&buf, user_buff, end)) {
        return -EFAULT;
    }
    buf[end] = '\0';

    if (2 != sscanf(buf, "%u %u", &op, &rcid)) {
        return -EINVAL;
    }

    u32 reg = FIELD_PREP(BC_ALLOC_CTL_OP_lo, op) |
              FIELD_PREP(BC_ALLOC_CTL_RCID_lo, rcid);
    writel(reg, info->mapped_base + BC_ALLOC_CTL_OFFSET);

    return size;
}

const struct file_operations bc_alloc_ctl_fops = {
    .open = bc_alloc_ctl_open,
    .write = bc_alloc_ctl_write,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

static int bc_bw_alloc_show(struct seq_file *m, void *p) {
    struct controller *info = m->private;
    u32 reg = readl(info->mapped_base + BC_BW_ALLOC_OFFSET);

    seq_printf(m, "rbwb: %lu\n", (unsigned long)FIELD_GET(BC_BW_ALLOC_RBWB, reg));

    return 0;
}

static int bc_bw_alloc_open(struct inode *inode, struct file *file) {
    return single_open(file, bc_bw_alloc_show, inode->i_private);
}

static ssize_t bc_bw_alloc_write(struct file *file, const char __user *user_buff, size_t size, loff_t *offset) {
    struct seq_file *m = file->private_data;
    struct controller *info = m->private;
    u32 rbwb;
    char buf[BUF_SIZE];
    size_t end = size > BUF_SIZE ? BUF_SIZE - 1 : size;

    if (copy_from_user(&buf, user_buff, end)) {
        return -EFAULT;
    }
    buf[end] = '\0';

    if (1 != sscanf(buf, "%u", &rbwb)) {
        return -EINVAL;
    }

    u32 reg = FIELD_PREP(BC_BW_ALLOC_RBWB, rbwb);
    writel(reg, info->mapped_base + BC_BW_ALLOC_OFFSET);

    return size;
}

const struct file_operations bc_bw_alloc_fops = {
    .open = bc_bw_alloc_open,
    .write = bc_bw_alloc_write,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

static void read_from_srmcfg(void *info) {
    this_cpu_write(srmcfg_val, read_csr(0x181));
}

static int srmcfg_show(struct seq_file *m, void *p) {
    int cpu;

    on_each_cpu(read_from_srmcfg, NULL, 1);

    for_each_online_cpu(cpu) {
        seq_printf(m, "cpu%d: 0x%llx\n", cpu, (unsigned long long)per_cpu(srmcfg_val, cpu));
    }
        
    return 0;
}

static int srmcfg_open(struct inode *inode, struct file *file) {
    return single_open(file, srmcfg_show, NULL);
}

static void write_to_srmcfg(void *info) {
    u32 config = *(u32 *)info;
    write_csr(0x181, config);
}

static ssize_t srmcfg_write(struct file *file, const char __user *user_buff, size_t size, loff_t *offset) {
    int cpu;
    u32 config;
    char buf[BUF_SIZE];
    size_t end = size > BUF_SIZE ? BUF_SIZE - 1 : size;

    if (copy_from_user(&buf, user_buff, end)) {
        return -EFAULT;
    }
    buf[end] = '\0';

    if (2 != sscanf(buf, "%d %x", &cpu, &config)) {
        return -EINVAL;
    }

    if (!cpu_possible(cpu) || !cpu_online(cpu)) {
        return -EINVAL;
    }

    smp_call_function_single(cpu, write_to_srmcfg, &config, 1);

    return size;
}

const struct file_operations srmcfg_fops = {
    .open = srmcfg_open,
    .write = srmcfg_write,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

static int map_device_io(struct controller* info, const char* name) {
    if (!request_mem_region(info->base_addr, info->size, name)) {
        pr_err("Failed to request cbqri device io %d", -EBUSY);
        return -EBUSY;
    }

    info->mapped_base = ioremap(info->base_addr, info->size);
    if (!info->mapped_base) {
        pr_err("Failed to map cbqri device io %d", -ENOMEM);
        release_mem_region(info->base_addr, info->size);
        return -ENOMEM;
    }

    return 0;
}

static int init_cbqri_debugfs(void) {
    int ret = 0;
    struct controller* info;

    top_dir = debugfs_create_dir("cbqri", NULL);
    if (!top_dir) {
        pr_err("Failed to create cbqri debugfs top directory");
        ret = -1;
    } else {
        debugfs_create_file("per_cpu_srmcfg", 0644, top_dir, NULL, &srmcfg_fops);
        list_for_each_entry(info, &controllers, list) {
            if (info->type == MEM_BANDWIDTH) {
                if (0 == map_device_io(info, "cbqri,mem-bandwidth")) {
                    mem_bw_dir = debugfs_create_dir("mem_bandwidth", top_dir);
                    // passing info into the data field puts it in i_private in inode
                    debugfs_create_file("bc_capabilities", 0644, mem_bw_dir, info, &bc_capabilities_fops);
                    debugfs_create_file("bc_mon_ctl", 0644, mem_bw_dir, info, &bc_mon_ctl_fops);
                    debugfs_create_file("bc_mon_ctr_val", 0644, mem_bw_dir, info, &bc_mon_ctr_fops);
                    debugfs_create_file("bc_alloc_ctl", 0644, mem_bw_dir, info, &bc_alloc_ctl_fops);
                    debugfs_create_file("bc_bw_alloc", 0644, mem_bw_dir, info, &bc_bw_alloc_fops);
                    debugfs_create_file("global_enable", 0644, mem_bw_dir, info, &global_enable_fops);
                    debugfs_create_file("period_len", 0644, mem_bw_dir, info, &period_len_fops);
                }
            } else if (info->type == CACHE_BANDWIDTH) {
                if (0 == map_device_io(info, "cbqri,cache-bandwidth")) {
                    cache_bw_dir = debugfs_create_dir("cache_bandwidth", top_dir);
                    debugfs_create_file("bc_capabilities", 0644, cache_bw_dir, info, &bc_capabilities_fops);
                    debugfs_create_file("bc_mon_ctl", 0644, cache_bw_dir, info, &bc_mon_ctl_fops);
                    debugfs_create_file("bc_mon_ctr_val", 0644, cache_bw_dir, info, &bc_mon_ctr_fops);
                    debugfs_create_file("bc_alloc_ctl", 0644, cache_bw_dir, info, &bc_alloc_ctl_fops);
                    debugfs_create_file("bc_bw_alloc", 0644, cache_bw_dir, info, &bc_bw_alloc_fops);
                    debugfs_create_file("global_enable", 0644, cache_bw_dir, info, &global_enable_fops);
                    debugfs_create_file("period_len", 0644, cache_bw_dir, info, &period_len_fops);
                }
            } else {
                continue;
            }
        }

    }

    return ret;
}

static const struct of_device_id controller_ids[] = {
    { .compatible = "riscv,cbqri-bandwidth-memory",
      .data = (void *)MEM_BANDWIDTH,
    },
    { .compatible = "riscv,cbqri-bandwidth-cache",
      .data = (void *)CACHE_BANDWIDTH,
    },
    {
        /* Drew's RFC v1 DT only labels memory bandwidth nodes with the bare
         * "riscv,cbqri-bandwidth" — treat that as MEM_BANDWIDTH so the QEMU
         * setup probes through the same path as the explicit subtype. */
        .compatible = "riscv,cbqri-bandwidth",
        .data = (void *)MEM_BANDWIDTH,
    },
    { }
};

static int parse_controllers(void) {
    struct controller* info;
    struct device_node* np;
    int error;

    u32 vals[4] = {0};
    u32 val;

    for_each_matching_node(np, controller_ids) {
        const struct of_device_id* match;

        match = of_match_node(controller_ids, np);
        if (!match) {
            of_node_put(np);
            continue;
        }

        info = kzalloc(sizeof(struct controller), GFP_KERNEL);
        if (!info) {
            goto mem_fail;
        }
        info->type = (enum controller_type)match->data;

        error = of_property_read_u32_array(np, "reg", vals, 4);
        if (error) {
            pr_err("Failed to read reg property %d", error);
            goto prop_read_fail;
        }
        info->base_addr = vals[1];
        info->size = vals[3];

        error = of_property_read_u32_index(np, "riscv,cbqri-rcid", 0, &val);
        if (error) {
            pr_err("Failed to read rcid property %d", error);
            goto prop_read_fail;
        }
        info->nrcid = val;

        error = of_property_read_u32_index(np, "riscv,cbqri-mcid", 0, &val);
        if (error) {
            pr_err("Failed to read mcid property %d", error);
            goto prop_read_fail;
        }
        info->nmcid = val;

        of_node_put(np);

        pr_info("base = 0x%lx | nrcid = %u | nmcid = %u", info->base_addr, info->nrcid, info->nmcid);

        INIT_LIST_HEAD(&info->list);
        list_add(&info->list, &controllers);
    }

    return 0;

prop_read_fail:
    kfree(info);

mem_fail:
    of_node_put(np);

    return error;
}

static int __init cbqri_bandwidth_init(void) {
    int error;

    INIT_LIST_HEAD(&controllers);
    error = parse_controllers();
    if (error) {
        pr_err("Failed to parse controllers %d", error);
        return error;
    }

    init_cbqri_debugfs();

    pr_info("CBQRI MMIO driver initialized\n");
    return 0;
}

static void __exit cbqri_bandwidth_cleanup(void) {
    struct controller *info;
    struct controller *next_info;

    debugfs_remove_recursive(top_dir);
    list_for_each_entry_safe(info, next_info, &controllers, list) {
        iounmap(info->mapped_base);
        release_mem_region(info->base_addr, info->size);
        list_del(&info->list);
        kfree(info);
    }

    pr_info("CBQRI MMIO driver clean up\n");
}

module_init(cbqri_bandwidth_init);
module_exit(cbqri_bandwidth_cleanup);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Connor Sullivan");
MODULE_DESCRIPTION("CBQRI Bandwidth");