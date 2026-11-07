/* REPO ps4-payload: v19 (instala desde el celular por USB: servidor local que lee el .pkg por MTP + Remote Package Installer) */
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

/* ---------- Mini servidor web (solo para la prueba de la etapa 1) ----------
   Sirve el .pkg por http://127.0.0.1:PUERTO con soporte de Range.
   Atiende una conexion a la vez; cada respuesta cierra la conexion. */

static long g_servido = 0;
static int g_conexiones = 0;

static int mandar(int c, const char *d, long n) {
  long hecho = 0;
  while (hecho < n) {
#ifdef MSG_NOSIGNAL
    long r = send(c, d + hecho, (size_t)(n - hecho), MSG_NOSIGNAL);
#else
    long r = send(c, d + hecho, (size_t)(n - hecho), 0);
#endif
    if (r <= 0) {
      anotar("send fallo errno", errno);
      return -1;
    }
    hecho += r;
  }
  return 0;
}

static const char *buscar_ci(const char *txt, const char *clave) {
  size_t lc = strlen(clave);
  for (const char *p = txt; *p; p++) {
    size_t i = 0;
    while (i < lc && p[i]) {
      char a = p[i], b = clave[i];
      if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
      if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
      if (a != b) break;
      i++;
    }
    if (i == lc) return p;
  }
  return NULL;
}


static void *g_h = NULL;      /* celular abierto por USB */
static uint32_t g_obj = 0;    /* objeto MTP del .pkg elegido */
#define PARTE_USB 2097152u    /* pedimos al celular de 2 MB en 2 MB */

/* Manda al cliente el rango [ini, ini+largo) del archivo, leyendolo del celular.
   Si el cliente corta, sigue leyendo (y tirando) lo que quede de ese trozo para no
   desordenar el USB. Devuelve 0 si todo bien, 1 si el cliente corto, -1 si fallo el USB. */
static int servir_rango_usb(int c, uint64_t ini, uint64_t largo) {
  uint64_t hecho = 0;
  int cliente_ok = 1;
  while (hecho < largo) {
    uint64_t resto = largo - hecho;
    uint32_t pedir = resto > PARTE_USB ? PARTE_USB : (uint32_t)resto;
    uint64_t pos = ini + hecho;
    if (mtp_enviar(g_h, 0x95C1, 4, g_obj, (uint32_t)(pos & 0xFFFFFFFFULL),
                   (uint32_t)(pos >> 32), pedir) != 0) {
      return -1;
    }
    int got = 0;
    int r = mtp_leer(g_h, chunk, CH, &got);
    if (r != 0 || got < 12 || rd16(chunk + 4) != 2) {
      anotar("USB: fallo al leer, r", r);
      anotar("USB: codigo", got >= 8 ? rd16(chunk + 6) : 0);
      return -1;
    }
    uint32_t falta = rd32(chunk) - 12;
    uint32_t parte = (uint32_t)(got - 12);
    if (cliente_ok && parte > 0) {
      if (mandar(c, (const char *)(chunk + 12), (long)parte) != 0) cliente_ok = 0;
      else g_servido += (long)parte;
    }
    while (parte < falta) {
      got = 0;
      r = mtp_leer(g_h, chunk, CH, &got);
      if (r != 0 || got <= 0) {
        anotar("USB: lectura cortada, r", r);
        return -1;
      }
      if (cliente_ok) {
        if (mandar(c, (const char *)chunk, (long)got) != 0) cliente_ok = 0;
        else g_servido += (long)got;
      }
      parte += (uint32_t)got;
    }
    mtp_respuesta(g_h);
    if (parte == 0) return -1;
    hecho += parte;
    if (!cliente_ok) return 1;
  }
  return 0;
}

static void cerrar_usb(void) {
  if (!g_h) return;
  mtp_enviar(g_h, 0x1003, 0, 0, 0, 0, 0);
  mtp_respuesta(g_h);
  p_soltar(g_h, 0);
  p_cerrar(g_h);
  g_h = NULL;
}

static void atender(int c, const char *ruta, long total) {
  char req[2048];
  int n = 0;
  while (n < (int)sizeof(req) - 1) {
    int r = (int)recv(c, req + n, sizeof(req) - 1 - n, 0);
    if (r <= 0) break;
    n += r;
    req[n] = 0;
    if (strstr(req, "\r\n\r\n")) break;
  }
  if (n <= 0) return;
  req[n] = 0;

  int es_head = (strncmp(req, "HEAD", 4) == 0);
  long ini = 0, fin = total - 1;
  int parcial = 0;
  const char *rg = buscar_ci(req, "range: bytes=");
  if (rg) {
    rg += 13;
    if (*rg >= '0' && *rg <= '9') {
      char *fp = NULL;
      ini = strtol(rg, &fp, 10);
      if (fp && *fp == '-') {
        fp++;
        if (*fp >= '0' && *fp <= '9') fin = strtol(fp, NULL, 10);
      }
      if (fin > total - 1) fin = total - 1;
      if (ini < 0 || ini > fin) {
        ini = 0;
        fin = total - 1;
      } else {
        parcial = 1;
      }
    }
  }
  long largo = fin - ini + 1;

  char cab[400];
  int lc;
  if (parcial) {
    lc = snprintf(cab, sizeof(cab),
                  "HTTP/1.1 206 Partial Content\r\n"
                  "Content-Type: application/octet-stream\r\n"
                  "Accept-Ranges: bytes\r\n"
                  "Content-Range: bytes %ld-%ld/%ld\r\n"
                  "Content-Length: %ld\r\n"
                  "Connection: close\r\n\r\n",
                  ini, fin, total, largo);
  } else {
    lc = snprintf(cab, sizeof(cab),
                  "HTTP/1.1 200 OK\r\n"
                  "Content-Type: application/octet-stream\r\n"
                  "Accept-Ranges: bytes\r\n"
                  "Content-Length: %ld\r\n"
                  "Connection: close\r\n\r\n",
                  largo);
  }
  anotar(es_head ? "pedido HEAD, ini" : "pedido GET, ini", ini);
  anotar("pedido fin", fin);
  if (mandar(c, cab, lc) != 0) return;
  if (es_head) return;

  long antes = g_servido;
  servir_rango_usb(c, (uint64_t)ini, (uint64_t)largo);
  anotar("enviado en este pedido", g_servido - antes);
}


static int obtener_ip(char *out, size_t n) {
  int u = socket(AF_INET, SOCK_DGRAM, 0);
  if (u < 0) return -1;
  struct sockaddr_in d;
  memset(&d, 0, sizeof(d));
  d.sin_family = AF_INET;
  d.sin_port = htons(53);
  d.sin_addr.s_addr = inet_addr("8.8.8.8");
  if (connect(u, (struct sockaddr *)&d, sizeof(d)) != 0) {
    close(u);
    return -1;
  }
  struct sockaddr_in l;
  socklen_t ll = sizeof(l);
  memset(&l, 0, sizeof(l));
  if (getsockname(u, (struct sockaddr *)&l, &ll) != 0) {
    close(u);
    return -1;
  }
  close(u);
  snprintf(out, n, "%s", inet_ntoa(l.sin_addr));
  return 0;
}

static int abrir_servidor(int puerto) {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) {
    anotar("socket fallo errno", errno);
    return -1;
  }
  int uno = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &uno, sizeof(uno));
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons(puerto);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
    anotar("bind fallo errno", errno);
    close(s);
    return -1;
  }
  if (listen(s, 16) != 0) {
    anotar("listen fallo errno", errno);
    close(s);
    return -1;
  }
  int fl = fcntl(s, F_GETFL, 0);
  fcntl(s, F_SETFL, fl | O_NONBLOCK);
  return s;
}

static void servir(int s, const char *ruta, long total) {
  long ocioso = 0; /* milisegundos sin actividad */
  while (1) {
    struct sockaddr_in ca;
    socklen_t cl = sizeof(ca);
    int c = accept(s, (struct sockaddr *)&ca, &cl);
    if (c >= 0) {
      int fl = fcntl(c, F_GETFL, 0);
      fcntl(c, F_SETFL, fl & ~O_NONBLOCK);
      struct timeval tv;
      tv.tv_sec = 15;
      tv.tv_usec = 0;
      setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
      g_conexiones++;
      if (g_conexiones == 1) avisar("v19: el instalador se conecto al servidor");
      atender(c, ruta, total);
      close(c);
      ocioso = 0;
    } else {
      usleep(20000);
      ocioso += 20;
    }
    if (g_conexiones == 0 && ocioso > 40000) break;
    if (g_conexiones > 0 && ocioso > 180000) break;
  }
  close(s);
}



/* Espera hasta 'ms' milisegundos a que alguien se conecte; si llega, la atiende */
static int esperar_conexion(int s, const char *ruta, long total, int ms) {
  long t = 0;
  while (t < ms) {
    struct sockaddr_in ca;
    socklen_t cl = sizeof(ca);
    int c = accept(s, (struct sockaddr *)&ca, &cl);
    if (c >= 0) {
      int fl = fcntl(c, F_GETFL, 0);
      fcntl(c, F_SETFL, fl & ~O_NONBLOCK);
      struct timeval tv;
      tv.tv_sec = 15;
      tv.tv_usec = 0;
      setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
      g_conexiones++;
      if (g_conexiones == 1) avisar("v19: el instalador se conecto al servidor");
      atender(c, ruta, total);
      close(c);
      return 1;
    }
    usleep(20000);
    t += 20;
  }
  return 0;
}

/* Manda POST /api/install a Remote Package Installer (127.0.0.1:12800).
   Devuelve los bytes de respuesta (en resp) o -1 si no pudo conectar. */
static int pedir_instalacion(const char *url_pkg, char *resp, size_t n) {
  resp[0] = 0;
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) {
    anotar("rpi socket fallo errno", errno);
    return -1;
  }
  struct timeval tv;
  tv.tv_sec = 8;
  tv.tv_usec = 0;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons(12800);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
    anotar("rpi connect fallo errno", errno);
    close(s);
    return -1;
  }
  char cuerpo[300];
  snprintf(cuerpo, sizeof(cuerpo),
           "{\"type\":\"direct\",\"packages\":[\"%s\"]}", url_pkg);
  char peticion[700];
  int lp = snprintf(peticion, sizeof(peticion),
                    "POST /api/install HTTP/1.1\r\n"
                    "Host: 127.0.0.1:12800\r\n"
                    "Content-Type: application/x-www-form-urlencoded\r\n"
                    "Content-Length: %d\r\n"
                    "Connection: close\r\n\r\n%s",
                    (int)strlen(cuerpo), cuerpo);
  if (mandar(s, peticion, lp) != 0) {
    anotar("rpi envio fallo errno", errno);
    close(s);
    return -1;
  }
  int total = 0;
  while (total < (int)n - 1) {
    int r = (int)recv(s, resp + total, n - 1 - total, 0);
    if (r <= 0) break;
    total += r;
  }
  resp[total] = 0;
  close(s);
  return total;
}

int main(void) {
  char msg[400];
  signal(SIGPIPE, SIG_IGN);
  anotar("v19 arranco", 0);
  avisar("v19: instalando desde el celular por USB...");

  int res = 0;
  int hu = sceKernelLoadStartModule("/system/common/lib/libSceUsbd.sprx", 0, NULL, 0, NULL, &res);
  anotar("cargar libSceUsbd", hu);
  if (hu < 0) {
    avisar("v19: no pude cargar libSceUsbd (mira el log)");
    return 0;
  }
  void *fn[8];
  const char *nombres[8] = {"sceUsbdInit",       "sceUsbdGetDeviceList", "sceUsbdGetDeviceDescriptor",
                            "sceUsbdOpen",       "sceUsbdClaimInterface", "sceUsbdBulkTransfer",
                            "sceUsbdReleaseInterface", "sceUsbdClose"};
  for (int i = 0; i < 8; i++) {
    if (resolver(hu, nombres[i], NULL, &fn[i]) != 0) {
      avisar("v19: faltan funciones de USB (mira el log)");
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
    if (vid == 0x2717) {
      cel = lista[j];
      break;
    }
  }
  if (!cel) {
    avisar("v19: no encontre el celular (cable y modo Transferencia de archivos)");
    return 0;
  }
  void *h = NULL;
  r = p_abrir(cel, &h);
  anotar("abrir celular", r);
  if (r != 0 || !h) {
    avisar("v19: no pude abrir el celular por USB (mira el log)");
    return 0;
  }
  r = p_reclamar(h, 0);
  anotar("reclamar interfaz 0", r);
  if (r != 0) {
    avisar("v19: no pude reclamar la interfaz USB del celular (mira el log)");
    p_cerrar(h);
    return 0;
  }
  g_h = h;

  mtp_enviar(h, 0x1002, 1, 1, 0, 0, 0);
  anotar("OpenSession, respuesta", mtp_respuesta(h));

  uint32_t hs[MAXH];
  uint32_t nh = 0, cuenta = 0, dl = 0;
  uint16_t fmt = 0;
  uint32_t tam32 = 0;
  char nombre[NOMB];
  if (mtp_listar(h, 0xFFFFFFFF, hs, &nh, &cuenta) == 0) {
    for (uint32_t i = 0; i < nh; i++) {
      if (mtp_info(h, hs[i], &fmt, &tam32, nombre) != 0) continue;
      if (fmt == 0x3001 && strcmp(nombre, "Download") == 0) {
        dl = hs[i];
        break;
      }
    }
  }
  if (!dl) {
    avisar("v19: no encontre la carpeta Download en el celular");
    cerrar_usb();
    return 0;
  }
  uint32_t cand = 0;
  uint64_t ctam = 0;
  if (mtp_listar(h, dl, hs, &nh, &cuenta) == 0) {
    anotar("Download: objetos", cuenta);
    for (uint32_t i = 0; i < nh; i++) {
      if (mtp_info(h, hs[i], &fmt, &tam32, nombre) != 0) continue;
      if (fmt == 0x3001 || !es_pkg(nombre)) continue;
      uint64_t t = tam32;
      if (tam32 == 0xFFFFFFFFu) {
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
    avisar("v19: no hay un .pkg grande en la carpeta Download del celular");
    cerrar_usb();
    return 0;
  }
  g_obj = cand;
  long tam = (long)ctam;
  anotar("elegido, bytes", tam);

  int srv = -1;
  for (int i = 0; i < PUERTOS_PROBAR && srv < 0; i++) {
    g_puerto = PUERTO + i;
    srv = abrir_servidor(g_puerto);
  }
  anotar("servidor web (descriptor)", srv);
  anotar("servidor web (puerto)", g_puerto);
  if (srv < 0) {
    avisar("v19: no pude abrir el servidor web interno (mira el log)");
    cerrar_usb();
    return 0;
  }

  char ipps4[64];
  memset(ipps4, 0, sizeof(ipps4));
  int ip_ok = obtener_ip(ipps4, sizeof(ipps4));
  anotar("ip de la PS4 (0=ok)", ip_ok);
  if (ip_ok == 0) anotar(ipps4, 0);

  char urls[2][160];
  memset(urls, 0, sizeof(urls));
  if (ip_ok == 0) {
    snprintf(urls[0], sizeof(urls[0]), "http://%s:%d/%s", ipps4, g_puerto, NOMBRE);
  }
  snprintf(urls[1], sizeof(urls[1]), "http://127.0.0.1:%d/%s", g_puerto, NOMBRE);

  static char resp[1500];
  int conectado = 0;
  for (int i = 0; i < 2 && !conectado; i++) {
    if (urls[i][0] == 0) continue;
    anotar("pidiendo a Remote Package Installer", i + 1);
    anotar(urls[i], 0);
    int nr = -1;
    for (int k = 1; k <= 3; k++) {
      nr = pedir_instalacion(urls[i], resp, sizeof(resp));
      if (nr >= 0) break;
      anotar("reintento de conexion con RPI", k);
      sleep(3);
    }
    anotar("respuesta de RPI (bytes)", nr);
    if (nr > 0) anotar(resp, 0);
    if (nr < 0) {
      avisar("v19: no pude hablar con Remote Package Installer (esta abierto en pantalla?)");
      close(srv);
      cerrar_usb();
      return 0;
    }
    snprintf(msg, sizeof(msg), "v19: RPI respondio %d bytes (intento %d)", nr, i + 1);
    avisar(msg);
    conectado = esperar_conexion(srv, "", tam, 12000);
    anotar("conexion recibida (0=no)", conectado);
  }
  if (!conectado) {
    avisar("v19: nadie se conecto al servidor (mira el log)");
    close(srv);
    cerrar_usb();
    return 0;
  }

  servir(srv, "", tam);
  anotar("servidor termino, conexiones", g_conexiones);
  anotar("servidor termino, enviado total", g_servido);
  cerrar_usb();
  snprintf(msg, sizeof(msg),
           "v19: servidor termino. conexiones=%d enviado=%ld de %ld bytes",
           g_conexiones, g_servido, tam);
  avisar(msg);
  return 0;
}
