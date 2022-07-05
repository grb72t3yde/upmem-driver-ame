#include <linux/nodemask.h>

#include "dpu_ame.h"

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
