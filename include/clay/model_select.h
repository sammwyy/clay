#ifndef CLAY_MODEL_SELECT_H
#define CLAY_MODEL_SELECT_H

#include "clay/array.h"

typedef struct {
    const char *id;
    const char *desc; /* optional, NULL to omit */
} ClayModelItem;

/* Appends every available model into `out` (a ClayArray of ClayModelItem).
   Model item strings are borrowed for the duration of clay_model_select.
   Returns 0 on success. */
typedef int (*ClayModelFetch)(void *ctx, ClayArray *out);

typedef struct {
    const char *id;    /* stable provider id returned in ClayModelSelection */
    const char *label; /* provider tab name */
    ClayModelFetch fetch;
    void *ctx;
} ClayModelProvider;

typedef struct {
    char *provider; /* provider id, malloc'd, NULL unless ok */
    char *model;    /* malloc'd, NULL unless ok */
    int ok;         /* 0 if the user cancelled */
} ClayModelSelection;

/* Left/right switch the active provider, shown beside a typed search
   input. The models below are a fixed six-row scrollable area bracketed
   by blank-or-count rows for models above and below; Enter confirms.
   Each provider is fetched lazily and retained while the selector is
   open. Clears itself from the screen before returning. */
ClayModelSelection clay_model_select(const ClayModelProvider *providers, int provider_count, int default_provider);

/* Frees provider/model and resets the struct to a cancelled state. */
void clay_model_selection_free(ClayModelSelection *sel);

#endif /* CLAY_MODEL_SELECT_H */
