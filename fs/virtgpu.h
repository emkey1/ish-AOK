#ifndef FS_VIRTGPU_H
#define FS_VIRTGPU_H

#include <stdbool.h>

// Whether /dev/dri/renderD128 exists: the renderer was built in and
// ISH_VIRTGPU=0 has not turned it off. Its sysfs identity follows this.
bool virtgpu_available(void);

#ifdef ISH_VIRTGPU
extern struct dev_ops virtgpu_dev;
#endif

#endif
