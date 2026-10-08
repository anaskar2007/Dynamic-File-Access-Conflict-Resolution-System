#ifndef SHADOW_COPY_H
#define SHADOW_COPY_H

#include "models.h"

ManagerResult shadow_copy_create(const char *original_path,
                                 const Application *owner,
                                 unsigned long creation_version,
                                 int shadow_copy_id,
                                 ShadowCopy *out_shadow_copy);

ManagerResult shadow_copy_delete(const ShadowCopy *shadow_copy);

void shadow_copy_set_delete_failure_for_testing(int enabled);
ManagerResult shadow_copy_delete_path(const char *path);

ManagerResult shadow_copy_create_diff_copy(const char *shadow_path,
                                           int diff_id,
                                           char *diff_path,
                                           size_t diff_path_size);

ManagerResult shadow_copy_read_metadata(const char *path,
                                        ShadowMetadata *metadata);

int shadow_copy_metadata_equal(const ShadowMetadata *left,
                               const ShadowMetadata *right);

#endif
