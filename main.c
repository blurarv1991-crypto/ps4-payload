#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define PKG_RUTA "/data/pkg/ps4-usb-prueba.pkg"

typedef struct {
  char pad[45];
  char message[3075];
} notify_request_t;

typedef struct {
  const char *uri;
  const char *ex_uri;
  const char *playgo_scenario_id;
  const char *content_id;
  const char *content_name;
  const char *icon_url;
} meta_info_t;

typedef struct {
  char content_id[48];
  int type;
  int platform;
} pkg_info_t;

typedef struct {
  char languages[30][8];
  char playgo_scenario_ids[64][3];
  char content_ids[64][48];
  unsigned char unknown[6480];
} playgo_info_t;

int sceKernelSendNotificationRequest(int dev, notify_request_t *req,
                                     size_t size, int blocking);
int sceKernelLoadStartModule(const char *path, size_t argc, const void *argv,
                             unsigned int flags, const void *opt, int *res);
int sceKernelDlsym(int handle, const char *symbol, void **addr);

static meta_info_t g_meta;
static pkg_info_t g_pkg;
static playgo_info_t g_playgo;

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

  anotar("instalador arranco", 0);

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
  int r2 = sceKernelDlsym(h, "sceAppInstUtilInstallByPackage", &f_inst);
  anotar("dlsym initialize", r1);
  anotar("dlsym install", r2);
  if (r1 != 0 || f_init == NULL || r2 != 0 || f_inst == NULL) {
    snprintf(msg, sizeof(msg), "Instalador: faltan funciones (%d, %d)", r1, r2);
    avisar(msg);
    return 0;
  }

  int ini = ((int (*)(void))f_init)();
  anotar("initialize devolvio", ini);
  if (ini != 0) {
    snprintf(msg, sizeof(msg), "Instalador: initialize fallo (0x%x)",
             (unsigned int)ini);
    avisar(msg);
    return 0;
  }

  memset(&g_meta, 0, sizeof(g_meta));
  memset(&g_pkg, 0, sizeof(g_pkg));
  memset(&g_playgo, 0, sizeof(g_playgo));
  g_meta.uri = PKG_RUTA;
  g_meta.content_id = "";

  int ret = ((int (*)(meta_info_t *, pkg_info_t *, playgo_info_t *))f_inst)(
      &g_meta, &g_pkg, &g_playgo);
  anotar("install devolvio", ret);
  g_pkg.content_id[47] = 0;
  FILE *f = fopen("/data/ps4-payload.log", "a");
  if (f) {
    fprintf(f, "content_id: %s\n", g_pkg.content_id);
    fclose(f);
  }

  snprintf(msg, sizeof(msg), "Instalador: ret=0x%x id=%s", (unsigned int)ret,
           g_pkg.content_id);
  avisar(msg);
  return 0;
}
