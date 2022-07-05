#include <linux/nodemask.h>

#include <dpu_ame.h>
#include <dpu_rank.h>

ame_context_t *ame_context_list[MAX_NUMNODES];

static int init_ame_context(int nid)
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
        if (init_ame_context(node))
            goto out_no_mem;

    return 0;

out_no_mem:
    for_each_online_node(node)
        if (ame_context_list[node])
            kfree(ame_context_list[node]);
    return -ENOMEM;
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
