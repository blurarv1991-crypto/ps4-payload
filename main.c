/* REPO ps4-payload: v18 (prueba: el payload lee el .pkg del celular por USB/MTP, sin servidor) */
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define NOMBRE "ps4-usb-prueba.pkg"
#define RUTA_A "/user/data/pkg/" NOMBRE
#define RUTA_B "/data/pkg/" NOMBRE
#define PUERTO 9095
#define PUERTOS_PROBAR 5
static int g_puerto = PUERTO;

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


/* ---------- USB / MTP desde el payload (prueba) ---------- */
#define EP_IN 0x81
#define EP_OUT 0x01
#define CAP 16384
#define CH 262144
#define MAXH 256
#define NOMB 256

static uint8_t datos[CAP];
static uint8_t chunk[CH];
static uint32_t tid = 0;

static void wr16(uint8_t *b, uint16_t v) { b[0] = v & 0xff; b[1] = (v >> 8) & 0xff; }
static void wr32(uint8_t *b, uint32_t v) { for (int i = 0; i < 4; i++) b[i] = (v >> (8 * i)) & 0xff; }
static uint16_t rd16(const uint8_t *b) { return (uint16_t)(b[0] | (b[1] << 8)); }
static uint32_t rd32(const uint8_t *b) { return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24); }

/* funciones de libSceUsbd, resueltas por nombre */
static int (*p_init)(void);
static int (*p_lista)(void ***);
static int (*p_desc)(void *, void *);
static int (*p_abrir)(void *, void **);
static int (*p_reclamar)(void *, int);
static int (*p_bulk)(void *, unsigned char, unsigned char *, int, int *, unsigned int);
static int (*p_soltar)(void *, int);
static int (*p_cerrar)(void *);

static int mtp_enviar(void *h, uint16_t code, int np, uint32_t p1, uint32_t p2, uint32_t p3, uint32_t p4) {
  uint8_t b[28];
  uint32_t len = 12 + 4 * np;
  tid++;
  wr32(b, len);
  wr16(b + 4, 1);
  wr16(b + 6, code);
  wr32(b + 8, tid);
  if (np > 0) wr32(b + 12, p1);
  if (np > 1) wr32(b + 16, p2);
  if (np > 2) wr32(b + 20, p3);
  if (np > 3) wr32(b + 24, p4);
  int tr = 0;
  int r = p_bulk(h, EP_OUT, b, (int)len, &tr, 5000);
  if (r != 0) {
    anotar("MTP envio fallo, codigo", code);
    anotar("MTP envio fallo, r", r);
  }
  return r;
}

static int mtp_leer(void *h, uint8_t *buf, int max, int *got) {
  int r = 0;
  *got = 0;
  for (int i = 0; i < 3; i++) {
    r = p_bulk(h, EP_IN, buf, max, got, 10000);
    if (r != 0 || *got > 0) break;
  }
  return r;
}

static int mtp_respuesta(void *h) {
  uint8_t b[512];
  int got = 0;
  int r = mtp_leer(h, b, 512, &got);
  if (r != 0 || got < 12) {
    anotar("MTP respuesta fallo, r", r);
    return -1;
  }
  return rd16(b + 6);
}

static int mtp_datos(void *h, int *total) {
  int got = 0;
  int r = mtp_leer(h, datos, 512, &got);
  if (r != 0 || got < 12) {
    anotar("MTP datos fallo, r", r);
    return -1;
  }
  if (rd16(datos + 4) == 3) {
    anotar("MTP sin datos, respuesta", rd16(datos + 6));
    return -2;
  }
  uint32_t largo = rd32(datos);
  if (largo > CAP - 512) {
    anotar("MTP datos demasiado grandes", largo);
    return -1;
  }
  while (got < (int)largo) {
    int g = 0;
    r = mtp_leer(h, datos + got, 512, &g);
    if (r != 0 || g <= 0) {
      anotar("MTP datos cortados, r", r);
      return -1;
    }
    got += g;
  }
  *total = got;
  return 0;
}

static int mtp_info(void *h, uint32_t handle, uint16_t *fmt, uint32_t *tam, char *nombre) {
  int total = 0;
  nombre[0] = 0;
  if (mtp_enviar(h, 0x1008, 1, handle, 0, 0, 0) != 0) return -1;
  if (mtp_datos(h, &total) != 0) return -1;
  const uint8_t *p = datos + 12;
  if (total < 12 + 53) return -1;
  *fmt = rd16(p + 4);
  *tam = rd32(p + 8);
  int ln = p[52];
  int k = 0;
  if (12 + 53 + 2 * ln <= total) {
    for (int i = 0; i < ln && k < NOMB - 1; i++) {
      uint8_t lo = p[53 + 2 * i];
      uint8_t hi = p[53 + 2 * i + 1];
      if (lo == 0 && hi == 0) break;
      nombre[k++] = (hi == 0 && lo >= 32 && lo < 127) ? (char)lo : '?';
    }
  }
  nombre[k] = 0;
  mtp_respuesta(h);
  return 0;
}

static int mtp_listar(void *h, uint32_t parent, uint32_t *hs, uint32_t *nh, uint32_t *cuenta) {
  int total = 0;
  *nh = 0;
  *cuenta = 0;
  if (mtp_enviar(h, 0x1007, 3, 0x00010001, 0, parent, 0) != 0) return -1;
  if (mtp_datos(h, &total) != 0) return -1;
  *cuenta = rd32(datos + 12);
  for (uint32_t i = 0; i < *cuenta && i < MAXH && (int)(16 + 4 * i + 4) <= total; i++) {
    hs[(*nh)++] = rd32(datos + 16 + 4 * i);
  }
  mtp_respuesta(h);
  return 0;
}

static int es_pkg(const char *n) {
  int len = (int)strlen(n);
  if (len <= 4) return 0;
  const char *e = n + len - 4;
  return e[0] == '.' && (e[1] == 'p' || e[1] == 'P') && (e[2] == 'k' || e[2] == 'K') &&
         (e[3] == 'g' || e[3] == 'G');
}

static int mtp_tam64(void *h, uint32_t handle, uint64_t *out) {
  int total = 0;
  if (mtp_enviar(h, 0x9803, 2, handle, 0xDC04, 0, 0) != 0) return -1;
  if (mtp_datos(h, &total) != 0) return -1;
  int ok = total >= 12 + 8;
  if (ok) *out = (uint64_t)rd32(datos + 12) | ((uint64_t)rd32(datos + 16) << 32);
  mtp_respuesta(h);
  return ok ? 0 : -1;
}

/* Lee un rango del archivo (GetPartialObject64). No guarda nada: cuenta los bytes y
   deja en 'primeros' los 16 primeros. Devuelve 0 si salio bien. */
static int mtp_rango(void *h, uint32_t handle, uint64_t off, uint32_t len,
                     uint8_t *primeros, uint64_t *contados) {
  *contados = 0;
  memset(primeros, 0, 16);
  if (mtp_enviar(h, 0x95C1, 4, handle, (uint32_t)(off & 0xFFFFFFFFULL),
                 (uint32_t)(off >> 32), len) != 0) {
    return -1;
  }
  int got = 0;
  int r = mtp_leer(h, chunk, CH, &got);
  if (r != 0 || got < 12 || rd16(chunk + 4) != 2) {
    anotar("rango: fallo, r", r);
    anotar("rango: codigo", got >= 8 ? rd16(chunk + 6) : 0);
    return -1;
  }
  uint32_t falta = rd32(chunk) - 12;
  uint32_t parte = (uint32_t)(got - 12);
  int n0 = parte < 16 ? (int)parte : 16;
  memcpy(primeros, chunk + 12, (size_t)n0);
  while (parte < falta) {
    got = 0;
    r = mtp_leer(h, chunk, CH, &got);
    if (r != 0 || got <= 0) {
      anotar("rango cortado, r", r);
      return -1;
    }
    parte += (uint32_t)got;
  }
  *contados = parte;
  mtp_respuesta(h);
  return 0;
}

static long ahora_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec * 1000L + tv.tv_usec / 1000L;
}

static void probar_rango(void *h, uint32_t handle, uint64_t off, uint32_t len, const char *nombre) {
  uint8_t pr[16];
  uint64_t cont = 0;
  long t0 = ahora_ms();
  int r = mtp_rango(h, handle, off, len, pr, &cont);
  long ms = ahora_ms() - t0;
  if (ms < 1) ms = 1;
  anotar(nombre, r);
  anotar("  pedido, desde", (long)off);
  anotar("  pedido, largo", (long)len);
  anotar("  recibido", (long)cont);
  anotar("  milisegundos", ms);
  anotar("  KB/s", (long)(cont * 1000 / 1024 / (uint64_t)ms));
  char hex[64];
  snprintf(hex, sizeof(hex), "  primeros bytes: %02x %02x %02x %02x %02x %02x %02x %02x", pr[0], pr[1],
           pr[2], pr[3], pr[4], pr[5], pr[6], pr[7]);
  anotar(hex, 0);
}

int main(void) {
  char msg[300];
  signal(SIGPIPE, SIG_IGN);
  anotar("v18 arranco", 0);
  avisar("v18: probando leer el celular por USB desde el payload");

  int res = 0;
  int hu = sceKernelLoadStartModule("/system/common/lib/libSceUsbd.sprx", 0, NULL, 0, NULL, &res);
  anotar("cargar libSceUsbd", hu);
  if (hu < 0) {
    avisar("v18: no pude cargar libSceUsbd (mira el log)");
    return 0;
  }
  void *fn[8];
  const char *nombres[8] = {"sceUsbdInit",       "sceUsbdGetDeviceList", "sceUsbdGetDeviceDescriptor",
                            "sceUsbdOpen",       "sceUsbdClaimInterface", "sceUsbdBulkTransfer",
                            "sceUsbdReleaseInterface", "sceUsbdClose"};
  for (int i = 0; i < 8; i++) {
    if (resolver(hu, nombres[i], NULL, &fn[i]) != 0) {
      avisar("v18: faltan funciones de USB (mira el log)");
      return 0;
    }
  }
  p_init = fn[0];
  p_lista = fn[1];
  p_desc = fn[2];
  p_abrir = fn[3];
  p_reclamar = fn[4];
  p_bulk = fn[5];
  p_soltar = fn[6];
  p_cerrar = fn[7];

  int r = p_init();
  anotar("sceUsbdInit devolvio", r);

  void **lista = NULL;
  int n = p_lista(&lista);
  anotar("dispositivos USB", n);
  void *cel = NULL;
  for (int j = 0; j < n; j++) {
    unsigned char d[18];
    memset(d, 0, sizeof(d));
    if (p_desc(lista[j], d) != 0) continue;
    unsigned int vid = d[8] | (d[9] << 8);
    unsigned int pid = d[10] | (d[11] << 8);
    anotar("  dispositivo vid", (long)vid);
    anotar("  dispositivo pid", (long)pid);
    if (vid == 0x2717) {
      cel = lista[j];
      break;
    }
  }
  if (!cel) {
    avisar("v18: no encontre el celular (cable y modo Transferencia de archivos)");
    return 0;
  }

  void *h = NULL;
  r = p_abrir(cel, &h);
  anotar("abrir celular", r);
  if (r != 0 || !h) {
    avisar("v18: no pude abrir el celular por USB (mira el log)");
    return 0;
  }
  r = p_reclamar(h, 0);
  anotar("reclamar interfaz 0", r);
  if (r != 0) {
    avisar("v18: no pude reclamar la interfaz USB del celular (mira el log)");
    p_cerrar(h);
    return 0;
  }

  mtp_enviar(h, 0x1002, 1, 1, 0, 0, 0);
  anotar("OpenSession, respuesta", mtp_respuesta(h));

  uint32_t hs[MAXH];
  uint32_t nh = 0, cuenta = 0, dl = 0;
  uint16_t fmt = 0;
  uint32_t tam = 0;
  char nombre[NOMB];

  if (mtp_listar(h, 0xFFFFFFFF, hs, &nh, &cuenta) == 0) {
    for (uint32_t i = 0; i < nh; i++) {
      if (mtp_info(h, hs[i], &fmt, &tam, nombre) != 0) continue;
      if (fmt == 0x3001 && strcmp(nombre, "Download") == 0) {
        dl = hs[i];
        break;
      }
    }
  }
  if (!dl) {
    avisar("v18: no encontre la carpeta Download en el celular");
  } else {
    uint32_t cand = 0;
    uint64_t ctam = 0;
    if (mtp_listar(h, dl, hs, &nh, &cuenta) == 0) {
      anotar("Download: objetos", cuenta);
      for (uint32_t i = 0; i < nh; i++) {
        if (mtp_info(h, hs[i], &fmt, &tam, nombre) != 0) continue;
        if (fmt == 0x3001 || !es_pkg(nombre)) continue;
        uint64_t t = tam;
        if (tam == 0xFFFFFFFFu) {
          t = 0;
          uint64_t t64 = 0;
          if (mtp_tam64(h, hs[i], &t64) == 0) t = t64;
        }
        anotar(nombre, (long)t);
        if (t > 1048576ULL && t > ctam) {
          cand = hs[i];
          ctam = t;
        }
      }
    }
    if (!cand) {
      avisar("v18: no hay un .pkg grande en la carpeta Download del celular");
    } else {
      anotar("elegido, bytes", (long)ctam);
      /* tres pedidos como los que hizo la PS4: cabecera, trozo medio y trozo grande */
      probar_rango(h, cand, 0, 4096, "rango 1 (cabecera)");
      uint32_t l2 = 418223;
      if ((uint64_t)24000 + l2 > ctam) l2 = (uint32_t)(ctam > 24000 ? ctam - 24000 : 0);
      probar_rango(h, cand, 24000, l2, "rango 2 (trozo medio)");
      uint64_t off3 = 524288;
      uint32_t l3 = 8257536;
      if (off3 >= ctam) {
        off3 = 0;
      }
      if (off3 + l3 > ctam) l3 = (uint32_t)(ctam - off3);
      probar_rango(h, cand, off3, l3, "rango 3 (trozo grande)");
      snprintf(msg, sizeof(msg), "v18: lei del celular por USB (archivo de %lu bytes). Mira el log",
               (unsigned long)ctam);
      avisar(msg);
    }
  }

  mtp_enviar(h, 0x1003, 0, 0, 0, 0, 0);
  mtp_respuesta(h);
  p_soltar(h, 0);
  p_cerrar(h);
  anotar("v18 termino", 0);
  return 0;
}
