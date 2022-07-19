#include <linux/nodemask.h>
#include <linux/memory_hotplug.h>
#include <linux/memory.h>

#include <dpu_ame.h>
#include <dpu_ame_ioctl.h>
#include <dpu_rank.h>

bool ame_initialized = false;
ame_context_t *ame_context_list[MAX_NUMNODES];
struct dpu_ame_fs ame_fs;

int dpu_ame_dev_uevent(struct device *dev, struct kobj_uevent_env *env)
{
    add_uevent_var(env, "DEVMODE=%#o", 0666);
    return 0;
}

struct class *dpu_ame_class;

static int dpu_ame_open(struct inode *inode, struct file *filp)
{
    struct dpu_ame_fs *fs =
        container_of(inode->i_cdev, struct dpu_ame_fs, cdev);

    filp->private_data = fs;

    ame_fs_lock();
    if (fs->is_opened) {
        ame_fs_unlock();
        return -EINVAL;
    }

    fs->is_opened = true;
    ame_fs_unlock();

    return 0;
}

static int dpu_ame_release(struct inode *inode, struct file *filp)
{
    struct dpu_ame_fs *fs = filp->private_data;

    if (!fs)
        return 0;

    ame_fs_lock();
    fs->is_opened = false;
    ame_fs_unlock();

    return 0;
}

static int dpu_ame_check_need_reclamation(unsigned long ptr)
{
    struct dpu_ame_allocation_context allocation_context;
    int node;
    int nr_free_ranks = 0;
    int nr_ltb_ranks = 0;

    if (copy_from_user(&allocation_context, (void *)ptr, sizeof(allocation_context)))
        return -EFAULT;

    for_each_online_node(node)
        nr_free_ranks += atomic_read(&ame_context_list[node]->nr_free_ranks);

    if (allocation_context.nr_req_ranks <= nr_free_ranks)
        return 0;

    for_each_online_node(node)
        nr_ltb_ranks += atomic_read(&ame_context_list[node]->nr_ltb_ranks);

    if (allocation_context.nr_req_ranks <= nr_free_ranks + nr_ltb_ranks) {
        /* we can get enough ranks after relcaiming (nr_req_ranks - nr_free_ranks) ranks */
    } else
        return -EBUSY;

    return 0;
}

static long dpu_ame_ioctl(struct file *filp, unsigned int cmd,
        unsigned long arg)
{
    struct dpu_ame_fs *fs = filp->private_data;
    int ret = 0;

    if (!fs)
        return 0;

    switch (cmd) {
    case DPU_AME_IOCTL_CHECK_NEED_RECLAMATION:
        ret = dpu_ame_check_need_reclamation(arg);
        break;
    default:
        break;
    }

    return ret;
}

static struct file_operations dpu_ame_fops = {
    .owner = THIS_MODULE,
    .open = dpu_ame_open,
    .release = dpu_ame_release,
    .unlocked_ioctl = dpu_ame_ioctl,
};

extern int (*ame_request_mram_expansion)(int nid);
extern int (*ame_request_mram_reclamation)(int nid);

void ame_lock(int nid)
{
    mutex_lock(&(ame_context_list[nid]->mutex));
}

void ame_unlock(int nid)
{
    mutex_unlock(&(ame_context_list[nid]->mutex));
}

void ame_fs_lock(void)
{
    mutex_lock(&ame_fs.mutex);
}

void ame_fs_unlock(void)
{
    mutex_unlock(&ame_fs.mutex);
}

int dpu_ame_create_device(void)
{
    int ret;

    ret = alloc_chrdev_region(&ame_fs.dev.devt, 0, 1, DPU_AME_NAME);
    if (ret)
        return ret;

    cdev_init(&ame_fs.cdev, &dpu_ame_fops);
    ame_fs.cdev.owner = THIS_MODULE;

    device_initialize(&ame_fs.dev);

    ame_fs.dev.class = dpu_ame_class;

    dev_set_drvdata(&ame_fs.dev, &ame_fs);
    dev_set_name(&ame_fs.dev, DPU_AME_NAME);

    ret = cdev_device_add(&ame_fs.cdev, &ame_fs.dev);
	if (ret)
		goto out;

    mutex_init(&ame_fs.mutex);
    ame_fs.is_opened = false;

    return 0;
out:
    put_device(&ame_fs.dev);
    unregister_chrdev_region(ame_fs.dev.devt, 1);
    return ret;
}

void dpu_ame_release_device(void)
{
    cdev_device_del(&ame_fs.cdev, &ame_fs.dev);
    put_device(&ame_fs.dev);
    unregister_chrdev_region(ame_fs.dev.devt, 1);
}

static void init_ame_api(void)
{
    ame_request_mram_expansion = request_mram_expansion;
    ame_request_mram_reclamation = request_mram_reclamation;
}

int init_ame_context(int nid)
{
    ame_context_list[nid] = kzalloc(sizeof(ame_context_t), GFP_KERNEL);

    if (!ame_context_list[nid])
        return -ENOMEM;
    mutex_init(&ame_context_list[nid]->mutex);

    ame_lock(nid);
    INIT_LIST_HEAD(&ame_context_list[nid]->rank_list);
    INIT_LIST_HEAD(&ame_context_list[nid]->ltb_rank_list);

    ame_context_list[nid]->ltb_index = NULL;
    ame_context_list[nid]->nid = nid;
    atomic_set(&ame_context_list[nid]->nr_free_ranks, 0);
    atomic_set(&ame_context_list[nid]->nr_ltb_ranks, 0);
    ame_unlock(nid);

    return 0;
}

void destroy_ame_context(int nid)
{
    if (ame_context_list[nid])
        kfree(ame_context_list[nid]);
}

int ame_init(void)
{
    int node;

    for_each_online_node(node)
        ame_init_node(node);

    for_each_online_node(node)
        ame_manager_run(node);

    for_each_online_node(node)
        ame_reclaimer_run(node);

    init_ame_api();

    return 0;
}

static uint32_t expand_one_section(struct dpu_rank_t *rank, int section_id)
{
    struct page *page = virt_to_page(rank->region->base);
    struct memory_block *mem = container_of(rank->dev.parent, struct memory_block, dev);
    struct zone *zone = page_zone(page);

    expose_mram_pages(page_to_pfn(page) + section_id * PAGES_PER_SECTION, PAGES_PER_SECTION, zone, mem->group);
    return 0;
}

static uint32_t reclaim_one_section(struct dpu_rank_t *rank, int section_id)
{
    struct page *page = virt_to_page(rank->region->base);
    struct memory_block *mem = container_of(rank->dev.parent, struct memory_block, dev);

    reclaim_mram_pages(page_to_pfn(page) + section_id * PAGES_PER_SECTION, PAGES_PER_SECTION, mem->group, &rank->region->dpu_dax_dev.pgmap);
    return 0;
}

uint32_t dpu_ame_rank_alloc(struct dpu_rank_t **rank, int nid)
{
    struct dpu_rank_t *rank_iterator;
    pg_data_t *pgdat = NODE_DATA(nid);

    *rank = NULL;

    list_for_each_entry (rank_iterator, &ame_context_list[nid]->rank_list, list) {
        if (rank_iterator->is_pinned)
            continue;
        if (dpu_rank_get(rank_iterator) == DPU_OK) {
            *rank = rank_iterator;

            /* Move the rank from rank_list to ltb_rank_list */
            list_del(&rank_iterator->list);
            list_add_tail(&rank_iterator->list, &ame_context_list[nid]->ltb_rank_list);

            /* Update counters */
            if (atomic_inc_return(&ame_context_list[nid]->nr_ltb_ranks) == 1)
                wakeup_ame_reclaimer(nid);
            atomic_inc(&pgdat->ame_nr_ranks);

            /* Update ltb allocation index */
            ame_context_list[nid]->ltb_index = rank_iterator;
            return DPU_OK;
        }
    }

    /* We can not find a free rank for the AME allocation */
    return DPU_ERR_DRIVER;
}

uint32_t dpu_ame_rank_free(struct dpu_rank_t **rank, int nid)
{
    struct dpu_rank_t *target_rank;
    pg_data_t *pgdat = NODE_DATA(nid);

    target_rank = *rank;

    dpu_rank_put(target_rank);
    ame_context_list[nid]->ltb_index = list_empty(&ame_context_list[nid]->ltb_rank_list) ?
        NULL : list_entry(target_rank->list.prev, typeof(*target_rank), list);

    list_del(&target_rank->list);
    list_add_tail(&target_rank->list, &ame_context_list[nid]->rank_list);

    if (atomic_dec_return(&ame_context_list[nid]->nr_ltb_ranks) == 0)
        ame_context_list[nid]->ltb_index = NULL;

    atomic_dec(&pgdat->ame_nr_ranks);

    return DPU_OK;
}

int request_mram_expansion(int nid)
{
    struct dpu_rank_t *current_ltb_rank;

    ame_lock(nid);
    current_ltb_rank = ame_context_list[nid]->ltb_index;

    if (current_ltb_rank)
        if (atomic_read(&current_ltb_rank->nr_ltb_sections) != SECTIONS_PER_DPU_RANK)
            goto request_one_section;

    /* try to allocate a new rank for AME */
    if (!atomic_read(&ame_context_list[nid]->nr_free_ranks)) {
        ame_unlock(nid);
        return -EBUSY;
    }

    if (dpu_ame_rank_alloc(&current_ltb_rank, nid) == DPU_OK)
        goto request_one_section;

    ame_unlock(nid);
    return -EBUSY;

request_one_section:
    expand_one_section(current_ltb_rank, atomic_read(&current_ltb_rank->nr_ltb_sections));
    atomic_inc(&current_ltb_rank->nr_ltb_sections);
    ame_unlock(nid);
    return 0;
}

int request_mram_reclamation(int nid)
{
    struct dpu_rank_t *current_ltb_rank;

    ame_lock(nid);
    current_ltb_rank = ame_context_list[nid]->ltb_index;

    if (!atomic_read(&ame_context_list[nid]->nr_ltb_ranks)) {
        ame_unlock(nid);
        return -EBUSY;
    }

    atomic_dec(&current_ltb_rank->nr_ltb_sections);
    reclaim_one_section(current_ltb_rank, atomic_read(&current_ltb_rank->nr_ltb_sections));

    if (!atomic_read(&current_ltb_rank->nr_ltb_sections))
        dpu_ame_rank_free(&current_ltb_rank, nid);

    ame_unlock(nid);
    return 0;
}

