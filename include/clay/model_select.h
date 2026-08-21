#ifndef CLAY_MODEL_SELECT_H
#define CLAY_MODEL_SELECT_H

typedef struct {
    const char *id;
    const char *desc; /* optional, NULL to omit */
} ClayModelItem;

/* Fills up to `max` items into `out`, returns how many were written. */
typedef int (*ClayModelFetch)(void *ctx, ClayModelItem *out, int max);

typedef struct {
    const char *label; /* provider tab name */
    ClayModelFetch fetch;
    void *ctx;
} ClayModelProvider;

typedef struct {
    char *provider; /* malloc'd, NULL unless ok */
    char *model;    /* malloc'd, NULL unless ok */
    int ok;         /* 0 if the user cancelled */
} ClayModelSelection;

/* Left/right switch the provider tab, a typed filter narrows a
   scrollable list (up/down, max 6 visible with "N more above/below"
   hints) of that provider's models, Enter confirms. Clears itself from
   the screen before returning either way. */
ClayModelSelection clay_model_select(const ClayModelProvider *providers, int provider_count, int default_provider);

/* Frees provider/model and resets the struct to a cancelled state. */
void clay_model_selection_free(ClayModelSelection *sel);

#endif /* CLAY_MODEL_SELECT_H */
