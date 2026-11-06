/*
 * daoCpuStages.c -- the registry of CPU stage types from plugins (daoCpuStages.h),
 * in libdaoTools so that a plugin links it with -ldaoTools.
 */
#include <string.h>
#include "daoCpuStages.h"

#define DAO_CPU_MAX_STAGE_TYPES 64

static struct {
    char type[64];
    daoCpuStageFactory factory;
} registry[DAO_CPU_MAX_STAGE_TYPES];
static int nRegistered = 0;

int daoCpuRegisterStage(const char *type, daoCpuStageFactory factory)
{
    if (!type || !factory || strlen(type) >= sizeof registry[0].type) {
        daoError("daoCpuRegisterStage: a type name (under %zu characters) and a factory\n", sizeof registry[0].type);
        return DAO_ERROR;
    }
    if (daoCpuFindStage(type)) {
        daoError("CPU stage type '%s' registered twice\n", type);
        return DAO_ERROR;
    }
    if (nRegistered == DAO_CPU_MAX_STAGE_TYPES) {
        daoError("too many CPU stage types (%d)\n", DAO_CPU_MAX_STAGE_TYPES);
        return DAO_ERROR;
    }
    strcpy(registry[nRegistered].type, type);
    registry[nRegistered].factory = factory;
    nRegistered++;
    return DAO_SUCCESS;
}

daoCpuStageFactory daoCpuFindStage(const char *type)
{
    for (int k = 0; k < nRegistered; k++)
        if (!strcmp(registry[k].type, type))
            return registry[k].factory;
    return NULL;
}
