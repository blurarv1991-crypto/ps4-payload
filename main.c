#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <errno.h>

#define NOMBRE "ps4-usb-prueba.pkg"
#define RUTA_A "/user/data/pkg/" NOMBRE
#define RUTA_B "/data/pkg/" NOMBRE

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

static long tamano(const char *ruta) {
  FILE *f = fopen(ruta, "rb");
  if (!f) return -1;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fclose(f);
  return n;
}

static void listar(const char *dir) {
  char msg[300];
  DIR *d = opendir(dir);
  if (!d) {
    snprintf(msg, sizeof(msg), "Lista %s: no se pudo abrir (errno %d)", dir,
             errno);
    avisar(msg);
    return;
  }
  int n = 0;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    char ruta[600];
    snprintf(ruta, sizeof(ruta), "%s/%s", dir, e->d_name);
    long t = tamano(ruta);
    anotar(ruta, t);
    n++;
    if (n <= 5) {
      snprintf(msg, sizeof(msg), "Lista %s: %s (%ld bytes)", dir, e->d_name, t);
      avisar(msg);
    }
  }
  closedir(d);
  snprintf(msg, sizeof(msg), "Lista %s: %d archivos en total", dir, n);
  avisar(msg);
}

int main(void) {
  char msg[300];
  int res = 0;

  anotar("instalador v4 arranco", 0);

  listar("/data/pkg");
  listar("/user/data/pkg");

  long tb = tamano(RUTA_B);
  long ta = tamano(RUTA_A);
  anotar("tamano /data/pkg", tb);
  anotar("tamano /user/data/pkg", ta);
  if (tb < 0 && ta < 0) {
    avisar("Instalador v4: no existe ps4-usb-prueba.pkg, no se instala");
    return 0;
  }

  int h = sceKernelLoadStartModule("/system/common/lib/libSceAppInstUtil.sprx",
                                   0, NULL, 0, NULL, &res);
  anotar("cargar modulo devolvio", h);
  if (h < 0) {
    snprintf(msg, sizeof(msg), "Instalador v4: no cargo AppInstUtil (0x%x)",
             (unsigned int)h);
    avisar(msg);
    return 0;
  }

  void *f_init = NULL;
  void *f_inst = NULL;
  int r1 = sceKernelDlsym(h, "sceAppInstUtilInitialize", &f_init);
  int r2 = sceKernelDlsym(h, "sceAppInstUtilAppInstallPkg", &f_inst);
  if (r1 != 0 || f_init == NULL || r2 != 0 || f_inst == NULL) {
    snprintf(msg, sizeof(msg), "Instalador v4: faltan funciones (%d, %d)", r1,
             r2);
    avisar(msg);
    return 0;
  }

  int ini = ((int (*)(void))f_init)();
  anotar("initialize devolvio", ini);
  if (ini != 0) {
    snprintf(msg, sizeof(msg), "Instalador v4: initialize fallo (0x%x)",
             (unsigned int)ini);
    avisar(msg);
    return 0;
  }

  int ra = ((int (*)(const char *, int))f_inst)(RUTA_A, 0);
  anotar("AppInstallPkg /user/data devolvio", ra);
  int rb = 0;
  if ((unsigned int)ra == 0x80020002u) {
    rb = ((int (*)(const char *, int))f_inst)(RUTA_B, 0);
    anotar("AppInstallPkg /data devolvio", rb);
  }

  snprintf(msg, sizeof(msg), "Instalador v4: tam=%ld user=0x%x data=0x%x",
           tb >= 0 ? tb : ta, (unsigned int)ra, (unsigned int)rb);
  avisar(msg);
  return 0;
}
