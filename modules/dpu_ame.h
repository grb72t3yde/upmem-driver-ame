#ifndef DPU_AME_H
#define DPU_AME_H

#include <linux/list.h>
#include <dpu_region.h>

typedef struct ame_context {
    int nid;
    struct list_head rank_list;
    struct list_head ltb_rank_list;
    struct dpu_rank_t *ltb_index;
    atomic_t nr_free_ranks;
    atomic_t nr_ltb_ranks;
} ame_context_t;

#endif
