/**
 * @file    daoCpuStages.h
 * @brief   CPU stages written outside daoTools, loaded by daoCpuPipeline as plugins.
 *
 * A plugin is a shared library listed in the daoCpuPipeline configuration:
 *
 *     plugins: [/path/to/libmyStages.so]
 *     stages:
 *       - myStage: {in: ..., out: ..., <its own keys>}
 *
 * It exports
 *
 *     void daoCpuPluginRegister(void);
 *
 * which registers its stage types (daoCpuRegisterStage). For each stage of such a
 * type, daoCpuPipeline calls the type's factory with the stage's arguments; the
 * factory fills a daoCpuStageDesc: the stage's data input and output SHMs and its
 * functions. The stage is then one of the pipeline's stages: run on the loop thread
 * in its place in the list, its output published by the pipeline (cnt0, timestamp,
 * semaphores), silent like the built-in stages, its time in daoCpuPipeline -p.
 *
 * The registry is in libdaoTools: a plugin links with -ldaoTools -ldao.
 */
#ifndef DAO_CPU_STAGES_H
#define DAO_CPU_STAGES_H

#include <stdlib.h>
#include "dao.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DAO_CPU_STAGE_API 1

/* A stage's arguments, valid during the factory call only (copy what it keeps). */
typedef struct daoCpuStageArgs {
    int api;                                          /* DAO_CPU_STAGE_API */
    const char *type;                                 /* the stage's YAML key */
    void *ctx;                                        /* the pipeline's */
    const char *(*value)(void *ctx, const char *key); /* the stage's YAML value as text (a list or a
                                                         map: as YAML); NULL: no such key */
    IMAGE *(*shm)(void *ctx, const char *key);        /* the SHM named by that key, opened by the
                                                         pipeline: the same IMAGE * as the other stages
                                                         get for that name, kept open by the pipeline
                                                         until it exits; NULL: absent or failed (said
                                                         with daoError) */
} daoCpuStageArgs;

/* What a factory gives the pipeline. */
typedef struct daoCpuStageDesc {
    IMAGE *in;               /* its data input (from args->shm): the stage writing it before this
                                one is this one's producer (silent producer: not run) */
    IMAGE *out;              /* its output (from args->shm): published by the pipeline when run()
                                returns 1 */
    int cnt2FromIn;          /* published with out's cnt2 = in's cnt2, as the built-in stages */
    int  (*run)(void *self);       /* one frame, on the loop thread: 1 publish out; 0 silent (out
                                      not published, and the stages reading it are not run, as for
                                      a disabled integrator); < 0 error: the rest of the frame is
                                      skipped (say why with daoError) */
    void (*post)(void *self);      /* optional (NULL): after the frame, once every stage ran and
                                      published, if this one published (run() returned 1) */
    void (*destroy)(void *self);   /* optional (NULL): at exit */
    void *self;                    /* given to the functions */
} daoCpuStageDesc;

/* 0: desc filled; anything else: error, said with daoError (the pipeline exits). The
 * pipeline gives desc zeroed. */
typedef int (*daoCpuStageFactory)(const daoCpuStageArgs *args, daoCpuStageDesc *desc);

/* Register a stage type (from daoCpuPluginRegister). DAO_SUCCESS, or DAO_ERROR when the
 * type is already registered. The built-in stage types keep priority over registered
 * ones of the same name. */
int daoCpuRegisterStage(const char *type, daoCpuStageFactory factory);
/* The type's factory, or NULL */
daoCpuStageFactory daoCpuFindStage(const char *type);

static inline const char *daoCpuArgValue(const daoCpuStageArgs *a, const char *key)
{
    return a->value(a->ctx, key);
}
static inline IMAGE *daoCpuArgShm(const daoCpuStageArgs *a, const char *key) { return a->shm(a->ctx, key); }
static inline double daoCpuArgDouble(const daoCpuStageArgs *a, const char *key, double dflt)
{
    const char *v = a->value(a->ctx, key);
    return v ? strtod(v, NULL) : dflt;
}
static inline long daoCpuArgInt(const daoCpuStageArgs *a, const char *key, long dflt)
{
    const char *v = a->value(a->ctx, key);
    return v ? strtol(v, NULL, 10) : dflt;
}

#ifdef __cplusplus
}
#endif

#endif /* DAO_CPU_STAGES_H */
