#include <linux/nodemask.h>
#include <linux/memory_hotplug.h>
#include <linux/memory.h>

#include <dpu_ame.h>
#include <dpu_rank.h>

ame_context_t *ame_context_list[MAX_NUMNODES];

extern int (*ame_request_mram_expansion)(int nid);
extern int (*ame_request_mram_reclamation)(int nid);

static DEFINE_MUTEX(ame_mutex);
void ame_lock(int nid)
{
    mutex_lock(&(ame_context_list[nid]->mutex));
}

void ame_unlock(int nid)
{
    mutex_unlock(&(ame_context_list[nid]->mutex));
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

    INIT_LIST_HEAD(&ame_context_list[nid]->rank_list);
    INIT_LIST_HEAD(&ame_context_list[nid]->ltb_rank_list);

    ame_context_list[nid]->ltb_index = NULL;
    ame_context_list[nid]->nid = nid;
    atomic_set(&ame_context_list[nid]->nr_free_ranks, 0);
    atomic_set(&ame_context_list[nid]->nr_ltb_ranks, 0);

    return 0;
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

out_no_mem:
    for_each_online_node(node)
        if (ame_context_list[node])
            kfree(ame_context_list[node]);
    return -ENOMEM;
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
        if (dpu_rank_get(rank_iterator) == DPU_OK) {
            *rank = rank_iterator;

            /* Move the rank from rank_list to ltb_rank_list */
            list_del(&rank_iterator->list);
            list_add_tail(&rank_iterator->list, &ame_context_list[nid]->ltb_rank_list);

            /* Update counters */
            atomic_dec(&ame_context_list[nid]->nr_free_ranks);
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
    ame_context_list[nid]->ltb_index = atomic_read(&ame_context_list[nid]->nr_ltb_ranks) == 1 ?
        NULL : list_entry(target_rank->list.prev, typeof(*target_rank), list);

    list_del(&target_rank->list);
    list_add_tail(&target_rank->list, &ame_context_list[nid]->rank_list);

    if (atomic_inc_return(&ame_context_list[nid]->nr_free_ranks) == 1)
        atomic_set(&pgdat->ame_disabled, 0);

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



