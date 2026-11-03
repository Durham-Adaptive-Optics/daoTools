/*
 * cpuPluginExample -- a daoCpuPipeline plugin with one stage type, "scale":
 *
 *     plugins: [build/tests/libcpuPluginExample.so]
 *     stages:
 *       - scale: {in: <float SHM>, k: <float SHM, 1 or n values>, out: <float SHM>,
 *                 gate: <SHM, 1 value>}             # gate optional: 0 = silent
 *
 * out = k * in (k scalar or per value, read in place every frame). With gate 0 the
 * stage is silent: nothing published, the stages reading out are not run. The
 * pattern of a stage written outside daoTools (daoCpuStages.h): a factory reading its
 * arguments, run / destroy functions. Used by tests/test_cpu_plugin.py.
 */
#include <stdio.h>
#include <stdlib.h>
#include "daoCpuStages.h"

typedef struct {
    IMAGE *in, *k, *out, *gate;
    long n, kn;
} Scale;

static int scaleRun(void *self)
{
    Scale *s = (Scale *) self;
    if (s->gate && s->gate->array.UI32[0] == 0)
        return 0;                                    /* silent this frame */
    const float *x = s->in->array.F, *k = s->k->array.F;
    float *y = s->out->array.F;
    for (long i = 0; i < s->n; i++)
        y[i] = x[i] * k[s->kn == 1 ? 0 : i];
    return 1;                                        /* the pipeline publishes out */
}

static void scaleDestroy(void *self) { free(self); }

static int scaleCreate(const daoCpuStageArgs *a, daoCpuStageDesc *d)
{
    if (a->api != DAO_CPU_STAGE_API) {
        daoError("scale: built for daoCpuStages.h API %d, the pipeline has %d\n", DAO_CPU_STAGE_API, a->api);
        return -1;
    }
    Scale *s = (Scale *) calloc(1, sizeof *s);
    s->in = daoCpuArgShm(a, "in");
    s->k = daoCpuArgShm(a, "k");
    s->out = daoCpuArgShm(a, "out");
    s->gate = daoCpuArgShm(a, "gate");               /* optional */
    if (!s->in || !s->k || !s->out) {
        daoError("scale: in, k and out SHMs needed\n");
        free(s);
        return -1;
    }
    s->n = (long) s->in->md[0].nelement;
    s->kn = (long) s->k->md[0].nelement;
    if (s->in->md[0].atype != _DATATYPE_FLOAT || s->k->md[0].atype != _DATATYPE_FLOAT
        || s->out->md[0].atype != _DATATYPE_FLOAT || (long) s->out->md[0].nelement < s->n
        || (s->kn != 1 && s->kn < s->n) || (s->gate && s->gate->md[0].atype != _DATATYPE_UINT32)) {
        daoError("scale: float in, out (>= in's size) and k (1 or n values), uint32 gate\n");
        free(s);
        return -1;
    }
    d->in = s->in;
    d->out = s->out;
    d->cnt2FromIn = 1;
    d->run = scaleRun;
    d->destroy = scaleDestroy;
    d->self = s;
    return 0;
}

void daoCpuPluginRegister(void)
{
    daoCpuRegisterStage("scale", scaleCreate);
}
