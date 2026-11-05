#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

#define NOMBRE "ps4-usb-prueba.pkg"
#define RUTA_A "/user/data/pkg/" NOMBRE
#define RUTA_B "/data/pkg/" NOMBRE
#define DIR_EXT "/mnt/ext0/pkg"
#define RUTA_EXT DIR_EXT "/" NOMBRE

typedef struct {
  char pad[45];
  char message[3075];
} notify_request_t;

typedef struct {
  void *mem;
  unsigned long size;
} bgft_init_params_t;

typedef struct {
  int user_id;
  int entitlement_type;
  const char *id;
  const char *content_url;
  const char *content_ex_url;
  const char *content_name;
  const char *icon_path;
  const char *sku_id;
  int option;
  const char *playgo_scenario_id;
  const char *release_date;
  const char *package_type;
  const char *package_sub_type;
  unsigned long package_size;
} bgft_download_param_t;

typedef struct {
  bgft_download_param_t param;
  unsigned int slot;
} bgft_download_param_ex_t;

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

static int resolver(int h, const char *a, const char *b, void **fn) {
  int r;
  *fn = NULL;
  r = sceKernelDlsym(h, a, fn);
  if (r == 0 && *fn != NULL) {
    anotar(a, 1);
    return 0;
  }
  if (b != NULL) {
    *fn = NULL;
    r = sceKernelDlsym(h, b, fn);
    if (r == 0 && *fn != NULL) {
      anotar(b, 1);
      return 0;
    }
  }
  anotar("no resolvio", r);
  anotar(a, 0);
  return -1;
}

static int copiar(const char *origen, const char *destino, long total) {
  FILE *i = fopen(origen, "rb");
  if (!i) return -1;
  FILE *o = fopen(destino, "wb");
  if (!o) {
    fclose(i);
    return -2;
  }
  static char buf[1048576];
  long hecho = 0;
  long aviso = 1073741824L;
  size_t n;
  int ok = 1;
  while ((n = fread(buf, 1, sizeof(buf), i)) > 0) {
    if (fwrite(buf, 1, n, o) != n) {
      ok = 0;
      break;
    }
    hecho += (long)n;
    if (hecho >= aviso) {
      char m[120];
      snprintf(m, sizeof(m), "v7: copiando al disco externo... %ld de %ld MB",
               hecho / 1048576, total / 1048576);
      avisar(m);
      aviso += 1073741824L;
    }
  }
  fclose(i);
  if (fclose(o) != 0) ok = 0;
  anotar("copiado bytes", hecho);
  if (!ok || hecho != total) {
    unlink(destino);
    return -3;
  }
  return 0;
}

int main(void) {
  char msg[400];
  int res = 0;

  anotar("instalador v7 (BGFT) arranco", 0);

  long ta = tamano(RUTA_A);
  long tb = tamano(RUTA_B);
  long te = tamano(RUTA_EXT);
  anotar("tamano /user/data/pkg", ta);
  anotar("tamano /data/pkg", tb);
  anotar("tamano disco externo", te);

  const char *origen = NULL;
  long tam = 0;
  if (ta >= 0) {
    origen = RUTA_A;
    tam = ta;
  } else if (tb >= 0) {
    origen = RUTA_B;
    tam = tb;
  }

  const char *ruta = NULL;
  const char *a_borrar = NULL;
  if (origen) {
    mkdir(DIR_EXT, 0777);
    avisar("v7: copiando el .pkg al disco externo...");
    int rc = copiar(origen, RUTA_EXT, tam);
    anotar("copiar al externo devolvio", rc);
    if (rc == 0) {
      ruta = RUTA_EXT;
      a_borrar = origen;
      avisar("v7: copiado al disco externo, instalando");
    } else {
      snprintf(msg, sizeof(msg),
               "v7: no pude copiar al externo (%d), instalo desde el interno", rc);
      avisar(msg);
      ruta = origen;
    }
  } else if (te >= 0) {
    ruta = RUTA_EXT;
    tam = te;
  } else {
    avisar("v7: no existe ps4-usb-prueba.pkg en /data/pkg ni en el disco externo");
    return 0;
  }

  int h = sceKernelLoadStartModule("/system/common/lib/libSceAppInstUtil.sprx",
                                   0, NULL, 0, NULL, &res);
  anotar("cargar AppInstUtil", h);
  if (h < 0) {
    snprintf(msg, sizeof(msg), "v7: no cargo AppInstUtil (0x%x)",
             (unsigned int)h);
    avisar(msg);
    return 0;
  }

  void *f_init = NULL, *f_tid = NULL, *f_slot = NULL, *f_over = NULL;
  if (resolver(h, "sceAppInstUtilInitialize", NULL, &f_init) ||
      resolver(h, "sceAppInstUtilGetTitleIdFromPkg", NULL, &f_tid) ||
      resolver(h, "sceAppInstUtilGetPrimaryAppSlot", NULL, &f_slot) ||
      resolver(h, "sceAppInstUtilAppPrepareOverwritePkg", NULL, &f_over)) {
    avisar("v7: faltan funciones de AppInstUtil (mira el log)");
    return 0;
  }

  int ini = ((int (*)(void))f_init)();
  anotar("initialize devolvio", ini);
  if (ini != 0) {
    snprintf(msg, sizeof(msg), "v7: initialize fallo (0x%x)",
             (unsigned int)ini);
    avisar(msg);
    return 0;
  }

  char title_id[32];
  memset(title_id, 0, sizeof(title_id));
  int is_app = 0;
  int rt = ((int (*)(const char *, char *, int *))f_tid)(ruta, title_id,
                                                         &is_app);
  anotar("GetTitleIdFromPkg devolvio", rt);
  if (rt != 0) {
    snprintf(msg, sizeof(msg), "v7: no pudo leer el pkg (0x%x)",
             (unsigned int)rt);
    avisar(msg);
    return 0;
  }

  unsigned int slot = 0;
  int overwrite = 0;
  int rs = ((int (*)(const char *, unsigned int *))f_slot)(title_id, &slot);
  anotar("GetPrimaryAppSlot devolvio", rs);
  if (rs != 0) {
    if ((unsigned int)rs == 0x80A3000Eu) {
      slot = 0;
    } else {
      snprintf(msg, sizeof(msg), "v7: slot fallo (0x%x)", (unsigned int)rs);
      avisar(msg);
      return 0;
    }
  } else if (is_app) {
    overwrite = 1;
  }
  if (overwrite) {
    int ro = ((int (*)(const char *))f_over)(ruta);
    anotar("PrepareOverwritePkg devolvio", ro);
  }

  int hb = sceKernelLoadStartModule("/system/common/lib/libSceBgft.sprx", 0,
                                    NULL, 0, NULL, &res);
  anotar("cargar Bgft", hb);
  if (hb < 0) {
    snprintf(msg, sizeof(msg), "v7: no cargo Bgft (0x%x)", (unsigned int)hb);
    avisar(msg);
    return 0;
  }

  void *f_binit = NULL, *f_breg = NULL, *f_bstart = NULL;
  if (resolver(hb, "sceBgftInitialize", "sceBgftServiceIntInit", &f_binit) ||
      resolver(hb, "sceBgftDownloadRegisterTaskByStorageEx",
               "sceBgftServiceIntDownloadRegisterTaskByStorageEx", &f_breg) ||
      resolver(hb, "sceBgftDownloadStartTask",
               "sceBgftServiceIntDownloadStartTask", &f_bstart)) {
    avisar("v7: faltan funciones de Bgft (mira el log)");
    return 0;
  }

  bgft_init_params_t ip;
  memset(&ip, 0, sizeof(ip));
  ip.size = 0x100000;
  ip.mem = malloc(ip.size);
  if (!ip.mem) {
    avisar("v7: sin memoria para Bgft");
    return 0;
  }
  memset(ip.mem, 0, ip.size);

  int rbi = ((int (*)(bgft_init_params_t *))f_binit)(&ip);
  anotar("Bgft init devolvio", rbi);
  if (rbi != 0) {
    /* Si el instalador ya corrio antes en este arranque, Bgft puede seguir
       iniciado y devolver error aqui. No paramos: probamos a registrar igual. */
    anotar("Bgft init fallo, sigo igual", rbi);
  }

  const char *nombre = NOMBRE;
  bgft_download_param_ex_t dp;
  memset(&dp, 0, sizeof(dp));
  dp.param.entitlement_type = 5;
  dp.param.id = "";
  dp.param.content_url = ruta;
  dp.param.content_name = nombre;
  dp.param.icon_path = "";
  dp.param.playgo_scenario_id = "0";
  dp.param.option = 0x10000;
  dp.slot = slot;

  int task = -1;
  int rr = ((int (*)(bgft_download_param_ex_t *, int *))f_breg)(&dp, &task);
  anotar("Bgft registrar devolvio", rr);
  anotar("task id", task);
  if (rr != 0) {
    snprintf(msg, sizeof(msg), "v7: registrar fallo (0x%x) init=0x%x id=%s",
             (unsigned int)rr, (unsigned int)rbi, title_id);
    avisar(msg);
    return 0;
  }

  int rst = ((int (*)(int))f_bstart)(task);
  anotar("Bgft start devolvio", rst);
  if (a_borrar && rr == 0 && rst == 0) {
    int ru = unlink(a_borrar);
    anotar("borre el original del disco interno", ru);
  }

  snprintf(msg, sizeof(msg), "v7: id=%s tam=%ld slot=%u reg=0x%x start=0x%x",
           title_id, tam, slot, (unsigned int)rr, (unsigned int)rst);
  avisar(msg);
  return 0;
}
