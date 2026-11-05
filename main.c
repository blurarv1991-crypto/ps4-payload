#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define PKG_RUTA "/data/pkg/ps4-usb-prueba.pkg"

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
  char msg[300];
  int res = 0;

  anotar("instalador v2 arranco", 0);

  int h = sceKernelLoadStartModule("/system/common/lib/libSceAppInstUtil.sprx",
                                   0, NULL, 0, NULL, &res);
  anotar("cargar modulo devolvio", h);
  if (h < 0) {
    snprintf(msg, sizeof(msg), "Instalador: no cargo AppInstUtil (0x%x)",
             (unsigned int)h);
    avisar(msg);
    return 0;
  }

  void *f_init = NULL;
  void *f_inst = NULL;
  int r1 = sceKernelDlsym(h, "sceAppInstUtilInitialize", &f_init);
  int r2 = sceKernelDlsym(h, "sceAppInstUtilAppInstallPkg", &f_inst);
  anotar("dlsym initialize", r1);
  anotar("dlsym AppInstallPkg", r2);
  if (r1 != 0 || f_init == NULL || r2 != 0 || f_inst == NULL) {
    snprintf(msg, sizeof(msg), "Instalador v2: faltan funciones (%d, %d)", r1,
             r2);
    avisar(msg);
    return 0;
  }

  int ini = ((int (*)(void))f_init)();
  anotar("initialize devolvio", ini);
  if (ini != 0) {
    snprintf(msg, sizeof(msg), "Instalador v2: initialize fallo (0x%x)",
             (unsigned int)ini);
    avisar(msg);
    return 0;
  }

  int ret = ((int (*)(const char *, int))f_inst)(PKG_RUTA, 0);
  anotar("AppInstallPkg devolvio", ret);

  snprintf(msg, sizeof(msg), "Instalador v2: ret=0x%x", (unsigned int)ret);
  avisar(msg);
  return 0;
}
