#ifndef KERNEL_PROCFS_H
#define KERNEL_PROCFS_H

#include "vfs.h"

// Creates the /proc filesystem and returns its root directory
// (one reference held, meant to be handed to vfs_mount).
struct vfs_node *procfs_create(void);

#endif