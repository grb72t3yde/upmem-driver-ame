#ifndef DPU_AME_H
#define DPU_AME_H

#include <linux/list.h>
#include <dpu_region.h>
#include <dpu_rank.h>

#define PAGES_PER_DPU_RANK (DPU_RANK_SIZE / PAGE_SIZE)
#define SECTIONS_PER_DPU_RANK (PAGES_PER_DPU_RANK / PAGES_PER_SECTION)

typedef struct ame_context {
    int nid;
    struct mutex mutex;
    struct list_head rank_list;
    struct list_head ltb_rank_list;
    struct dpu_rank_t *ltb_index;
    atomic_t nr_free_ranks;
    atomic_t nr_ltb_ranks;
} ame_context_t;

void ame_lock(int nid);
void ame_unlock(int nid);

uint32_t dpu_ame_rank_alloc(struct dpu_rank_t **rank, int nid);
uint32_t dpu_ame_rank_free(struct dpu_rank_t **rank, int nid);

int request_mram_expansion(int nid);
int request_mram_reclamation(int nid);

#endif
