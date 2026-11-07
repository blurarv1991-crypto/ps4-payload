/* REPO ps4-payload: v21 (instala desde el celular por USB; servidor local que atiende varias conexiones a la vez + Remote Package Installer) */
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

static long g_t0 = 0;
static long ms_ahora(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec * 1000L + tv.tv_usec / 1000L;
}

static void anotar(const char *txt, long valor) {
  FILE *f = fopen("/data/ps4-payload.log", "a");
  if (f) {
    if (g_t0 == 0) g_t0 = ms_ahora();
    long t = ms_ahora() - g_t0;
    fprintf(f, "[%ld.%03ld] %s %ld\n", t / 1000, t % 1000, txt, valor);
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


/* ---------- Servidor que atiende varias conexiones a la vez ----------
   Un solo hilo, todo sin bloqueos: si la PS4 abre una conexion y no lee,
   las demas siguen avanzando. El celular se lee de 1 MB en 1 MB. */

static void *g_h = NULL;      /* celular abierto por USB */
static uint32_t g_obj = 0;    /* objeto MTP del .pkg elegido */
static long g_tam = 0;        /* tamano del .pkg */
#define PARTE_USB 1048576u
#define MAXC 8

static void cerrar_usb(void) {
  if (!g_h) return;
  mtp_enviar(g_h, 0x1003, 0, 0, 0, 0, 0);
  mtp_respuesta(g_h);
  p_soltar(g_h, 0);
  p_cerrar(g_h);
  g_h = NULL;
}

/* Trae 'len' bytes del archivo desde 'off' leyendolos del celular. 0 = bien */
static int mtp_traer(uint8_t *dest, uint64_t off, uint32_t len) {
  if (mtp_enviar(g_h, 0x95C1, 4, g_obj, (uint32_t)(off & 0xFFFFFFFFULL),
                 (uint32_t)(off >> 32), len) != 0) {
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
  if (parte > len) parte = len;
  memcpy(dest, chunk + 12, parte);
  uint32_t recibido = (uint32_t)(got - 12);
  while (recibido < falta) {
    got = 0;
    r = mtp_leer(g_h, chunk, CH, &got);
    if (r != 0 || got <= 0) {
      anotar("USB: lectura cortada, r", r);
      return -1;
    }
    if (parte < len) {
      uint32_t cp = (uint32_t)got;
      if (parte + cp > len) cp = len - parte;
      memcpy(dest + parte, chunk, cp);
      parte += cp;
    }
    recibido += (uint32_t)got;
  }
  mtp_respuesta(g_h);
  if (parte != len) {
    anotar("USB: llegaron menos bytes de los pedidos", (long)parte);
    return -1;
  }
  return 0;
}

typedef struct {
  int fd;                /* -1 = libre */
  int num;               /* numero de conexion (para el log) */
  int estado;            /* 1 leyendo pedido, 2 enviando cabecera, 3 enviando datos */
  char req[2048];
  int nreq;
  char cab[400];
  int lcab;
  int cab_env;
  int es_head;
  uint64_t pos;          /* proximo byte del archivo por traer del celular */
  uint64_t falta;        /* bytes del archivo que aun no se traen */
  uint8_t *buf;          /* datos ya traidos del celular */
  uint32_t blen;
  uint32_t benv;
  long total_env;        /* bytes enviados en este pedido */
  long ultimo;           /* ultima vez que hubo avance */
} conn_t;

static conn_t cs[MAXC];
static int g_rpi = -1;
static char g_rpi_resp[1500];
static int g_rpi_n = 0;

static void poner_no_bloq(int fd) {
  int fl = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void conn_cerrar(conn_t *c, const char *motivo) {
  if (c->fd < 0) return;
  anotar(motivo, c->num);
  anotar("enviado en este pedido", c->total_env);
  close(c->fd);
  c->fd = -1;
}

static void conn_nueva(int fd, long ahora) {
  int i;
  for (i = 0; i < MAXC; i++) {
    if (cs[i].fd < 0) break;
  }
  if (i == MAXC) {
    anotar("demasiadas conexiones, cierro una", 0);
    close(fd);
    return;
  }
  conn_t *c = &cs[i];
  uint8_t *b = c->buf;
  memset(c, 0, sizeof(*c));
  c->buf = b;
  c->fd = fd;
  c->estado = 1;
  c->ultimo = ahora;
  c->num = ++g_conexiones;
  poner_no_bloq(fd);
  anotar("conexion aceptada, numero", c->num);
  if (c->num == 1) avisar("v21: el instalador se conecto al servidor");
}

/* Lee el pedido ya recibido y prepara la cabecera. 0 = bien */
static int conn_preparar(conn_t *c) {
  char *req = c->req;
  char linea[140];
  int k = 0;
  while (k < 130 && req[k] && req[k] != '\r' && req[k] != '\n') {
    linea[k] = req[k];
    k++;
  }
  linea[k] = 0;
  anotar(linea, c->num);
  c->es_head = (strncmp(req, "HEAD", 4) == 0);
  long total = g_tam;
  long ini = 0, fin = total - 1;
  int parcial = 0;
  const char *rg = buscar_ci(req, "range: bytes=");
  if (rg) {
    k = 0;
    while (k < 130 && rg[k] && rg[k] != '\r' && rg[k] != '\n') {
      linea[k] = rg[k];
      k++;
    }
    linea[k] = 0;
    anotar(linea, c->num);
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
  if (parcial) {
    c->lcab = snprintf(c->cab, sizeof(c->cab),
                       "HTTP/1.1 206 Partial Content\r\n"
                       "Content-Type: application/octet-stream\r\n"
                       "Accept-Ranges: bytes\r\n"
                       "Content-Range: bytes %ld-%ld/%ld\r\n"
                       "Content-Length: %ld\r\n"
                       "Connection: close\r\n\r\n",
                       ini, fin, total, largo);
  } else {
    c->lcab = snprintf(c->cab, sizeof(c->cab),
                       "HTTP/1.1 200 OK\r\n"
                       "Content-Type: application/octet-stream\r\n"
                       "Accept-Ranges: bytes\r\n"
                       "Content-Length: %ld\r\n"
                       "Connection: close\r\n\r\n",
                       largo);
  }
  anotar(c->es_head ? "pedido HEAD, ini" : "pedido GET, ini", ini);
  anotar("pedido fin", fin);
  c->pos = (uint64_t)ini;
  c->falta = (uint64_t)largo;
  c->cab_env = 0;
  c->estado = 2;
  return 0;
}

/* Envia sin bloquear: devuelve los bytes enviados (0 si no cabe ahora) o -1 si fallo */
static long enviar_nb(int fd, const uint8_t *d, long n) {
#ifdef MSG_NOSIGNAL
  long r = send(fd, d, (size_t)n, MSG_NOSIGNAL);
#else
  long r = send(fd, d, (size_t)n, 0);
#endif
  if (r >= 0) return r;
  if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
  anotar("send fallo errno", errno);
  return -1;
}

/* Avanza una conexion un paso. Devuelve 1 si hubo avance. */
static int conn_procesar(conn_t *c, long ahora) {
  int prog = 0;
  if (c->fd < 0) return 0;

  if (c->estado == 1) {
    int r = (int)recv(c->fd, c->req + c->nreq, sizeof(c->req) - 1 - (size_t)c->nreq, 0);
    if (r > 0) {
      c->nreq += r;
      c->req[c->nreq] = 0;
      prog = 1;
      c->ultimo = ahora;
      if (strstr(c->req, "\r\n\r\n")) {
        conn_preparar(c);
      } else if (c->nreq >= (int)sizeof(c->req) - 1) {
        conn_cerrar(c, "pedido demasiado largo, numero");
        return 1;
      }
    } else if (r == 0) {
      conn_cerrar(c, "conexion cerrada sin pedido, numero");
      return 1;
    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
      anotar("recv fallo errno", errno);
      conn_cerrar(c, "conexion con error al leer, numero");
      return 1;
    }
  }

  if (c->fd >= 0 && c->estado == 2) {
    long r = enviar_nb(c->fd, (const uint8_t *)c->cab + c->cab_env, c->lcab - c->cab_env);
    if (r < 0) {
      conn_cerrar(c, "cliente cerro, numero");
      return 1;
    }
    if (r > 0) {
      c->cab_env += (int)r;
      c->ultimo = ahora;
      prog = 1;
      if (c->cab_env >= c->lcab) {
        if (c->es_head) {
          conn_cerrar(c, "HEAD atendido, numero");
          return 1;
        }
        c->estado = 3;
      }
    }
  }

  if (c->fd >= 0 && c->estado == 3) {
    if (c->benv >= c->blen) {
      if (c->falta == 0) {
        conn_cerrar(c, "pedido completo, numero");
        return 1;
      }
      uint32_t pedir = c->falta > PARTE_USB ? PARTE_USB : (uint32_t)c->falta;
      if (!c->buf) c->buf = (uint8_t *)malloc(PARTE_USB);
      if (!c->buf) {
        conn_cerrar(c, "sin memoria, numero");
        return 1;
      }
      if (mtp_traer(c->buf, c->pos, pedir) != 0) {
        conn_cerrar(c, "fallo al leer del celular, numero");
        return 1;
      }
      c->blen = pedir;
      c->benv = 0;
      c->pos += pedir;
      c->falta -= pedir;
      prog = 1;
    }
    long r = enviar_nb(c->fd, c->buf + c->benv, (long)(c->blen - c->benv));
    if (r < 0) {
      conn_cerrar(c, "cliente cerro, numero");
      return 1;
    }
    if (r > 0) {
      c->benv += (uint32_t)r;
      c->total_env += r;
      g_servido += r;
      c->ultimo = ahora;
      prog = 1;
    }
  }

  if (c->fd >= 0 && ahora - c->ultimo > 120000) {
    conn_cerrar(c, "conexion sin avance en 120 s, numero");
    return 1;
  }
  return prog;
}

/* Manda la orden de instalacion a Remote Package Installer y deja la conexion
   abierta (sin bloquear) para ver su respuesta mas tarde. 0 = bien */
static int rpi_enviar(const char *url_pkg) {
  if (g_rpi >= 0) {
    close(g_rpi);
    g_rpi = -1;
  }
  g_rpi_n = 0;
  g_rpi_resp[0] = 0;
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) {
    anotar("rpi socket fallo errno", errno);
    return -1;
  }
  struct timeval tv;
  tv.tv_sec = 3;
  tv.tv_usec = 0;
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
  poner_no_bloq(s);
  g_rpi = s;
  anotar("orden enviada a RPI, bytes", lp);
  return 0;
}

static int rpi_paso(void) {
  if (g_rpi < 0) return 0;
  int r = (int)recv(g_rpi, g_rpi_resp + g_rpi_n, sizeof(g_rpi_resp) - 1 - (size_t)g_rpi_n, 0);
  if (r > 0) {
    g_rpi_n += r;
    g_rpi_resp[g_rpi_n] = 0;
    anotar("RPI respondio, bytes", g_rpi_n);
    anotar(g_rpi_resp, 0);
    return 1;
  }
  if (r == 0) {
    anotar("RPI cerro la conexion, bytes recibidos", g_rpi_n);
    close(g_rpi);
    g_rpi = -1;
    return 1;
  }
  if (errno != EAGAIN && errno != EWOULDBLOCK) {
    anotar("rpi recv fallo errno", errno);
    close(g_rpi);
    g_rpi = -1;
    return 1;
  }
  return 0;
}

/* Un paso del servidor: acepta conexiones nuevas y avanza las que haya. */
static int paso(int srv) {
  int prog = 0;
  long ahora = ms_ahora();
  while (1) {
    struct sockaddr_in ca;
    socklen_t cl = sizeof(ca);
    int fd = accept(srv, (struct sockaddr *)&ca, &cl);
    if (fd < 0) break;
    conn_nueva(fd, ahora);
    prog = 1;
  }
  for (int i = 0; i < MAXC; i++) {
    if (conn_procesar(&cs[i], ahora)) prog = 1;
  }
  if (rpi_paso()) prog = 1;
  if (!prog) usleep(2000);
  return prog;
}

int main(void) {
  char msg[400];
  signal(SIGPIPE, SIG_IGN);
  for (int i = 0; i < MAXC; i++) cs[i].fd = -1;
  anotar("v21 arranco", 0);
  avisar("v21: instalando desde el celular por USB...");

  int res = 0;
  int hu = sceKernelLoadStartModule("/system/common/lib/libSceUsbd.sprx", 0, NULL, 0, NULL, &res);
  anotar("cargar libSceUsbd", hu);
  if (hu < 0) {
    avisar("v21: no pude cargar libSceUsbd (mira el log)");
    return 0;
  }
  void *fn[8];
  const char *nombres[8] = {"sceUsbdInit",       "sceUsbdGetDeviceList", "sceUsbdGetDeviceDescriptor",
                            "sceUsbdOpen",       "sceUsbdClaimInterface", "sceUsbdBulkTransfer",
                            "sceUsbdReleaseInterface", "sceUsbdClose"};
  for (int i = 0; i < 8; i++) {
    if (resolver(hu, nombres[i], NULL, &fn[i]) != 0) {
      avisar("v21: faltan funciones de USB (mira el log)");
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
    avisar("v21: no encontre el celular (cable y modo Transferencia de archivos)");
    return 0;
  }
  void *h = NULL;
  r = p_abrir(cel, &h);
  anotar("abrir celular", r);
  if (r != 0 || !h) {
    avisar("v21: no pude abrir el celular por USB (mira el log)");
    return 0;
  }
  r = p_reclamar(h, 0);
  anotar("reclamar interfaz 0", r);
  if (r != 0) {
    avisar("v21: no pude reclamar la interfaz USB del celular (mira el log)");
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
    avisar("v21: no encontre la carpeta Download en el celular");
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
    avisar("v21: no hay un .pkg grande en la carpeta Download del celular");
    cerrar_usb();
    return 0;
  }
  g_obj = cand;
  g_tam = (long)ctam;
  anotar("elegido, bytes", g_tam);

  int srv = -1;
  for (int i = 0; i < PUERTOS_PROBAR && srv < 0; i++) {
    g_puerto = PUERTO + i;
    srv = abrir_servidor(g_puerto);
  }
  anotar("servidor web (descriptor)", srv);
  anotar("servidor web (puerto)", g_puerto);
  if (srv < 0) {
    avisar("v21: no pude abrir el servidor web interno (mira el log)");
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

  for (int i = 0; i < 2 && g_conexiones == 0; i++) {
    if (urls[i][0] == 0) continue;
    anotar("pidiendo a Remote Package Installer", i + 1);
    anotar(urls[i], 0);
    int ok = -1;
    for (int k = 1; k <= 3 && ok != 0; k++) {
      ok = rpi_enviar(urls[i]);
      if (ok != 0) {
        anotar("reintento de conexion con RPI", k);
        sleep(3);
      }
    }
    if (ok != 0) {
      avisar("v21: no pude hablar con Remote Package Installer (esta abierto en pantalla?)");
      close(srv);
      cerrar_usb();
      return 0;
    }
    snprintf(msg, sizeof(msg), "v21: orden enviada a Remote Package Installer (intento %d)", i + 1);
    avisar(msg);
    long t0 = ms_ahora();
    while (ms_ahora() - t0 < 15000 && g_conexiones == 0) paso(srv);
    anotar("conexiones recibidas tras la orden", g_conexiones);
  }
  if (g_conexiones == 0) {
    avisar("v21: nadie se conecto al servidor (mira el log)");
    close(srv);
    if (g_rpi >= 0) close(g_rpi);
    cerrar_usb();
    return 0;
  }

  long ult = ms_ahora();
  while (1) {
    int p = paso(srv);
    long ahora = ms_ahora();
    if (p) ult = ahora;
    if (ahora - ult > 180000) break;
  }

  anotar("servidor termino, conexiones", g_conexiones);
  anotar("servidor termino, enviado total", g_servido);
  for (int i = 0; i < MAXC; i++) {
    if (cs[i].fd >= 0) conn_cerrar(&cs[i], "cierro al terminar, conexion");
  }
  if (g_rpi >= 0) close(g_rpi);
  close(srv);
  cerrar_usb();
  snprintf(msg, sizeof(msg),
           "v21: servidor termino. conexiones=%d enviado=%ld de %ld bytes",
           g_conexiones, g_servido, g_tam);
  avisar(msg);
  return 0;
}
