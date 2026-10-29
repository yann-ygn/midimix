/* A stand-in for MPC's plugin host, for tests/test_plugin.py: the plugin (wrapper + engine) linked in,
 * driven from stdin, one command per line. What the plugin tells the host is printed as it happens:
 * "automate <index> <value>" and "update". Each command ends with "ok" (or "end" after a listing).
 *   open | close                  a plugin instance more / the last one less (one is opened at start)
 *   params                        "param <index> <key> <name>" for each
 *   get <index>                   "value <index> <normalized> <display text>"
 *   set <index> <normalized>      setParameter
 *   programs                      "program <index> <name>" for each, then "current <index>"
 *   program <index>               effSetProgram
 *   chunk                         effGetChunk: "chunk <length> <text>"
 *   setchunk <text>               effSetChunk (the rest of the line)
 *   run <blocks>                  processReplacing that many 128-frame blocks, checking they are silent
 *   sleep <ms> | quit
 * The control socket's path comes from MIDIMIXD_SOCKET (a fake midimixd). */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct AEffect AEffect;
typedef intptr_t (*host_cb)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
struct AEffect {
    int32_t magic;
    intptr_t (*dispatcher)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
    void (*process)(AEffect *, float **, float **, int32_t);
    void (*setParameter)(AEffect *, int32_t, float);
    float (*getParameter)(AEffect *, int32_t);
    int32_t numPrograms, numParams, numInputs, numOutputs, flags;
    intptr_t resvd1, resvd2;
    int32_t initialDelay, realQualities, offQualities;
    float ioRatio;
    void *object, *user;
    int32_t uniqueID, version;
    void (*processReplacing)(AEffect *, float **, float **, int32_t);
    void (*processDoubleReplacing)(AEffect *, double **, double **, int32_t);
    char future[56];
};
enum { effClose = 1, effSetProgram = 2, effGetProgram = 3, effGetParamDisplay = 7, effGetParamName = 8,
       effGetChunk = 23, effSetChunk = 24, effGetProgramNameIndexed = 29 };
AEffect *VSTPluginMain(host_cb master);

static intptr_t host(AEffect *e, int32_t op, int32_t i, intptr_t v, void *p, float o)
{
    (void)e, (void)v, (void)p;
    if (op == 0)
        printf("automate %d %g\n", i, o);
    else if (op == 42)
        printf("update\n");
    return 0;
}

int main(void)
{
    AEffect *fx[8];
    int n = 0;
    char line[256], buf[256];
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!(fx[n++] = VSTPluginMain(host))) {
        puts("error no plugin");
        return 1;
    }
    while (fgets(line, sizeof line, stdin)) {
        char cmd[32] = "";
        int i = 0;
        float f = 0;
        sscanf(line, "%31s %d %f", cmd, &i, &f);
        AEffect *e = fx[n - 1];
        if (!strcmp(cmd, "quit"))
            break;
        else if (!strcmp(cmd, "open") && n < 8) {
            if (!(fx[n] = VSTPluginMain(host))) {
                puts("error no plugin");
                continue;
            }
            n++;
        } else if (!strcmp(cmd, "close") && n > 1) {
            n--;
            fx[n]->dispatcher(fx[n], effClose, 0, 0, NULL, 0);
        }
        else if (!strcmp(cmd, "params")) {
            for (int k = 0; k < e->numParams; k++) {
                buf[0] = 0;
                e->dispatcher(e, effGetParamName, k, 0, buf, 0);
                printf("param %d %s\n", k, buf);
            }
            puts("end");
            continue;
        } else if (!strcmp(cmd, "get")) {
            buf[0] = 0;
            float v = e->getParameter(e, i);
            e->dispatcher(e, effGetParamDisplay, i, 0, buf, 0);
            printf("value %d %g %s\n", i, v, buf);
        } else if (!strcmp(cmd, "set"))
            e->setParameter(e, i, f);
        else if (!strcmp(cmd, "programs")) {
            for (int k = 0; k < e->numPrograms; k++) {
                buf[0] = 0;
                e->dispatcher(e, effGetProgramNameIndexed, k, 0, buf, 0);
                printf("program %d %s\n", k, buf);
            }
            printf("current %d\n", (int)e->dispatcher(e, effGetProgram, 0, 0, NULL, 0));
            puts("end");
            continue;
        } else if (!strcmp(cmd, "program"))
            e->dispatcher(e, effSetProgram, 0, i, NULL, 0);
        else if (!strcmp(cmd, "chunk")) {
            void *data = NULL;
            int len = (int)e->dispatcher(e, effGetChunk, 0, 0, &data, 0);
            printf("chunk %d %s\n", len, len > 0 && data ? (char *)data : "");
        } else if (!strcmp(cmd, "setchunk")) {
            char *text = line + strlen("setchunk");
            text += strspn(text, " ");
            text[strcspn(text, "\n")] = 0;
            e->dispatcher(e, effSetChunk, 0, (intptr_t)strlen(text) + 1, text, 0);
        } else if (!strcmp(cmd, "run")) {
            float l[128], r[128], *out[2] = {l, r};
            for (int k = 0; k < i; k++) {
                e->processReplacing(e, NULL, out, 128);
                for (int j = 0; j < 128; j++)
                    if (l[j] != 0 || r[j] != 0) {
                        puts("error not silent");
                        return 1;
                    }
            }
        } else if (!strcmp(cmd, "sleep"))
            usleep(i * 1000);
        else {
            printf("error unknown command %s\n", cmd);
            continue;
        }
        puts("ok");
    }
    while (n-- > 0)
        fx[n]->dispatcher(fx[n], effClose, 0, 0, NULL, 0);
    return 0;
}
