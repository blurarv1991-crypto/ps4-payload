#include <stdio.h>
#include <string.h>
#include <stdint.h>

typedef struct {
  char pad[45];
  char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int dev, notify_request_t *req,
                                     size_t size, int blocking);
int sceKernelLoadStartModule(const char *path, size_t argc, const void *argv,
                             unsigned int flags, const void *opt, int *res);
int sceKernelDlsym(int handle, const char *symbol, void **addr);

static void anotar(const char *txt, long valor) {
  FILE *f = fopen("/data/ps4-payload.log", "a");
  if (f) {
    fprintf(f, "%s %ld\n", txt, valor);
    fclose(f);
  }
}

static void avisar(const char *texto) {
  notify_request_t req;
  memset(&req, 0, sizeof(req));
  strncpy(req.message, texto, sizeof(req.message) - 1);
  int r = sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
  anotar("notificacion devolvio", r);
}

int main(void) {
  char msg[200];
  int res = 0;

  anotar("sonda arranco", 0);

  int h = sceKernelLoadStartModule("/system/common/lib/libSceAppInstUtil.sprx",
                                   0, NULL, 0, NULL, &res);
  anotar("cargar modulo devolvio", h);
  if (h < 0) {
    snprintf(msg, sizeof(msg), "Sonda: no cargo AppInstUtil (0x%x)",
             (unsigned int)h);
    avisar(msg);
    return 0;
  }

  void *fn = NULL;
  int r = sceKernelDlsym(h, "sceAppInstUtilInitialize", &fn);
  anotar("dlsym devolvio", r);
  if (r != 0 || fn == NULL) {
    snprintf(msg, sizeof(msg), "Sonda: no encontro Initialize (0x%x)",
             (unsigned int)r);
    avisar(msg);
    return 0;
  }

  int ini = ((int (*)(void))fn)();
  anotar("initialize devolvio", ini);

  snprintf(msg, sizeof(msg), "Sonda: modulo=%d init=0x%x", h,
           (unsigned int)ini);
  avisar(msg);
  return 0;
}
