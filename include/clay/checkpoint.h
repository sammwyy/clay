#ifndef CLAY_CHECKPOINT_H
#define CLAY_CHECKPOINT_H

#include "clay/array.h"

/* Workspace snapshots via a real git repo, independent of whether the
   workspace itself is a git repo (Kilo's checkpoint design). `checkpoints_dir`
   is a bare repo used only as storage - write-tree/commit-tree before each
   mutating tool call, read-tree + checkout-index to restore. No libgit2. */

typedef struct {
    char *commit;
    char *label;
    long long created_at;
} ClayCheckpoint;

/* Snapshots the current state of workspace_dir into a new checkpoint commit
   labeled `label`, creating checkpoints_dir as a bare repo on first use.
   A no-op (0, no new commit) if nothing changed since the last checkpoint.
   0 on success, -1 on failure. */
int clay_checkpoint_save(const char *checkpoints_dir, const char *workspace_dir, const char *label);

/* Every checkpoint on the checkpoints branch, newest first. Caller frees
   with clay_checkpoint_list_free. 0 on success (including none yet), -1 on
   failure. */
int clay_checkpoint_list(const char *checkpoints_dir, ClayArray *checkpoints);
void clay_checkpoint_list_free(ClayArray *checkpoints);

/* Overwrites workspace_dir's tracked files with the state recorded in
   `commit`. Files created after that checkpoint (and never checkpointed)
   are left in place - checkout-index only writes what's in the target
   tree, it doesn't delete. 0 on success, -1 on failure. */
int clay_checkpoint_restore(const char *checkpoints_dir, const char *workspace_dir, const char *commit);

#endif /* CLAY_CHECKPOINT_H */
