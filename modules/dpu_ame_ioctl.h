#ifndef DPU_AME_IOCTL_INCLUDE_H
#define DPU_AME_IOCTL_INCLUDE_H

#include <dpu_ame.h>

#define DPU_AME_IOCTL_MAGIC 'd'

#define DPU_AME_IOCTL_CHECK_NEED_RECLAMATION _IOWR(DPU_AME_IOCTL_MAGIC, 0, struct dpu_ame_allocation_context *)
#define DPU_AME_IOCTL_TRIGGER_ASYNC_RECLAMATION _IOWR(DPU_AME_IOCTL_MAGIC, 1, int)

#endif
