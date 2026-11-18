/* REPO ps4-payload: v34 (instala desde el celular por USB; elige el .pkg que pida la app (puerto 9096), o instalar.pkg, y si no existe, el mas grande; servidor local con varias conexiones + Remote Package Installer) */
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

static int g_app_fd = -1;   /* conexion abierta con la app (puerto 9096) para mandarle avisos y progreso */

/* Manda una linea (ya con salto de linea al final) a la app. Si falla, se olvida de la app. */
static void enviar_app(const char *linea, int n) {
  if (g_app_fd < 0 || n <= 0) return;
#ifdef MSG_NOSIGNAL
  long w = send(g_app_fd, linea, (size_t)n, MSG_NOSIGNAL);
#else
  long w = send(g_app_fd, linea, (size_t)n, 0);
#endif
  if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
    close(g_app_fd);
    g_app_fd = -1;
  }
}

static void avisar(const char *texto) {
  notify_request_t req;
  memset(&req, 0, sizeof(req));
  strncpy(req.message, texto, sizeof(req.message) - 1);
  int r = sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
  anotar("notificacion devolvio", r);
  if (g_app_fd >= 0) {
    char b[600];
    int n = snprintf(b, sizeof(b), "%s", texto);
    if (n >= (int)sizeof(b)) n = (int)sizeof(b) - 2;
    for (int k = 0; k < n; k++) {
      if (b[k] == '\n' || b[k] == '\r') b[k] = ' ';
    }
    b[n++] = '\n';
    enviar_app(b, n);
  }
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
#define NOMB 1024

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
    for (int i = 0; i < ln && k < NOMB - 5; i++) {
      uint32_t cp = (uint32_t)p[53 + 2 * i] | ((uint32_t)p[53 + 2 * i + 1] << 8);
      if (cp == 0) break;
      if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < ln) {
        uint32_t c2 = (uint32_t)p[53 + 2 * (i + 1)] | ((uint32_t)p[53 + 2 * (i + 1) + 1] << 8);
        if (c2 >= 0xDC00 && c2 <= 0xDFFF) {
          cp = 0x10000 + ((cp - 0xD800) << 10) + (c2 - 0xDC00);
          i++;
        }
      }
      /* UTF-16 del celular -> UTF-8 (para que coincida con el nombre que manda la app) */
      if (cp < 32) {
        nombre[k++] = '?';
      } else if (cp < 0x80) {
        nombre[k++] = (char)cp;
      } else if (cp < 0x800) {
        nombre[k++] = (char)(0xC0 | (cp >> 6));
        nombre[k++] = (char)(0x80 | (cp & 0x3F));
      } else if (cp < 0x10000) {
        nombre[k++] = (char)(0xE0 | (cp >> 12));
        nombre[k++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        nombre[k++] = (char)(0x80 | (cp & 0x3F));
      } else {
        nombre[k++] = (char)(0xF0 | (cp >> 18));
        nombre[k++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        nombre[k++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        nombre[k++] = (char)(0x80 | (cp & 0x3F));
      }
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


#define NOMBRE_ELEGIDO "instalar.pkg"
static char g_pedido[1024];   /* nombre del .pkg que pidio la app (vacio = ninguno) */
/* 1 si el nombre es el que pidio la app o, si no pidio ninguno, instalar.pkg (sin importar mayusculas) */
static int es_elegido(const char *n) {
  const char *obj = g_pedido[0] ? g_pedido : NOMBRE_ELEGIDO;
  if (strlen(n) != strlen(obj)) return 0;
  return buscar_ci(n, obj) == n;
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
#define PARTE_USB 4194304u
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
/* Lectura del celular por pasos: se pide un trozo y luego se va leyendo de a 256 KB,
   para poder ir mandando a la PS4 mientras llegan los datos. Solo un trozo a la vez. */
static struct {
  int activo;
  uint32_t len, parte, falta, recibido;
  int cab;
} g_rd;

static int mtp_ini(uint64_t off, uint32_t len) {
  if (mtp_enviar(g_h, 0x95C1, 4, g_obj, (uint32_t)(off & 0xFFFFFFFFULL),
                 (uint32_t)(off >> 32), len) != 0) {
    return -1;
  }
  g_rd.activo = 1;
  g_rd.len = len;
  g_rd.parte = 0;
  g_rd.falta = 0;
  g_rd.recibido = 0;
  g_rd.cab = 0;
  return 0;
}

/* Lee un pedazo. dest puede ser NULL (se descarta). Devuelve 0 = sigue, 1 = termino, -1 = error */
static int mtp_paso(uint8_t *dest) {
  int got = 0;
  int r = mtp_leer(g_h, chunk, CH, &got);
  if (!g_rd.cab) {
    if (r != 0 || got < 12 || rd16(chunk + 4) != 2) {
      anotar("USB: fallo al leer, r", r);
      anotar("USB: codigo", got >= 8 ? rd16(chunk + 6) : 0);
      g_rd.activo = 0;
      return -1;
    }
    g_rd.falta = rd32(chunk) - 12;
    uint32_t cp = (uint32_t)(got - 12);
    if (cp > g_rd.len) cp = g_rd.len;
    if (dest) memcpy(dest, chunk + 12, cp);
    g_rd.parte = cp;
    g_rd.recibido = (uint32_t)(got - 12);
    g_rd.cab = 1;
  } else {
    if (r != 0 || got <= 0) {
      anotar("USB: lectura cortada, r", r);
      g_rd.activo = 0;
      return -1;
    }
    if (g_rd.parte < g_rd.len) {
      uint32_t cp = (uint32_t)got;
      if (g_rd.parte + cp > g_rd.len) cp = g_rd.len - g_rd.parte;
      if (dest) memcpy(dest + g_rd.parte, chunk, cp);
      g_rd.parte += cp;
    }
    g_rd.recibido += (uint32_t)got;
  }
  if (g_rd.recibido >= g_rd.falta) {
    mtp_respuesta(g_h);
    g_rd.activo = 0;
    if (g_rd.parte != g_rd.len) {
      anotar("USB: llegaron menos bytes de los pedidos", (long)g_rd.parte);
      return -1;
    }
    return 1;
  }
  return 0;
}

/* Si una conexion se cierra a medio trozo, hay que terminar de leerlo para no dejar el USB a medias */
static void mtp_drenar(void) {
  while (g_rd.activo) {
    if (mtp_paso(NULL) != 0) break;
  }
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
  int lee;               /* 1 = esta conexion esta leyendo un trozo del celular */
  uint64_t ini_pedido;   /* primer byte que pidio este pedido */
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

/* Rangos del archivo ya entregados por completo (para saber cuando termino la descarga) */
#define MAXCOV 64
static uint64_t cov_a[MAXCOV], cov_b[MAXCOV];
static int cov_n = 0;
static int g_listo = 0;       /* 1 = la PS4 ya recibio todo el archivo */

static void cov_agregar(uint64_t a, uint64_t b) {
  if (b <= a) return;
  int i = 0;
  while (i < cov_n) {
    if (a <= cov_b[i] && b >= cov_a[i]) { /* se toca o se solapa: unir */
      if (cov_a[i] < a) a = cov_a[i];
      if (cov_b[i] > b) b = cov_b[i];
      for (int k = i; k < cov_n - 1; k++) {
        cov_a[k] = cov_a[k + 1];
        cov_b[k] = cov_b[k + 1];
      }
      cov_n--;
    } else {
      i++;
    }
  }
  if (cov_n < MAXCOV) {
    cov_a[cov_n] = a;
    cov_b[cov_n] = b;
    cov_n++;
  }
  if (cov_n == 1 && cov_a[0] == 0 && cov_b[0] >= (uint64_t)g_tam) g_listo = 1;
}

static void conn_cerrar(conn_t *c, const char *motivo) {
  if (c->fd < 0) return;
  if (c->lee) {
    mtp_drenar();
    c->lee = 0;
  }
  /* si la PS4 cerro a medias (lo hace normal con los primeros bytes), lo enviado cuenta como recibido */
  if (c->estado == 3 && !c->es_head && c->total_env > 0 && strncmp(motivo, "cliente cerro", 13) == 0) {
    cov_agregar(c->ini_pedido, c->ini_pedido + (uint64_t)c->total_env);
  }
  anotar(motivo, c->num);
  anotar("enviado en este pedido", c->total_env);
  close(c->fd);
  c->fd = -1;
  if (c->buf) {
    free(c->buf);
    c->buf = NULL;
  }
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
  /* buffer de envio grande: el kernel sigue mandando por Wi-Fi mientras se lee el siguiente trozo del celular */
  int sb = 4194304;
  setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sb, sizeof(sb));
  if (c->num <= 2) {
    int real = 0;
    socklen_t rl = sizeof(real);
    getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &real, &rl);
    anotar("buffer de envio real", real);
  }
  anotar("conexion aceptada, numero", c->num);
  if (c->num == 1) avisar("v34: el instalador se conecto al servidor");
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
  c->ini_pedido = (uint64_t)ini;
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
    /* 1) si ya se envio todo lo traido, empezar el siguiente trozo (si el USB esta libre) */
    if (!c->lee && c->benv >= c->blen) {
      if (c->falta == 0) {
        if (!c->es_head) cov_agregar(c->ini_pedido, c->pos);
        conn_cerrar(c, "pedido completo, numero");
        return 1;
      }
      if (!g_rd.activo) {
        uint32_t pedir = c->falta > PARTE_USB ? PARTE_USB : (uint32_t)c->falta;
        if (!c->buf) c->buf = (uint8_t *)malloc(pedir);
        if (!c->buf) {
          avisar("v34: sin memoria en el payload. Reinicia la PS4 y prueba otra vez.");
          conn_cerrar(c, "sin memoria, numero");
          return 1;
        }
        if (mtp_ini(c->pos, pedir) != 0) {
          conn_cerrar(c, "fallo al leer del celular, numero");
          return 1;
        }
        c->lee = 1;
        c->blen = 0;
        c->benv = 0;
        c->pos += pedir;
        c->falta -= pedir;
        prog = 1;
      }
    }
    /* 2) mandar lo que ya llego */
    if (c->benv < c->blen) {
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
    /* 3) leer un pedazo mas del celular mientras el kernel manda lo anterior por Wi-Fi */
    if (c->lee) {
      int rr = mtp_paso(c->buf + 0);
      if (rr < 0) {
        c->lee = 0;
        conn_cerrar(c, "fallo al leer del celular, numero");
        return 1;
      }
      c->blen = g_rd.parte;
      if (rr == 1) c->lee = 0;
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

/* La app puede mandar por el puerto 9096 el nombre del .pkg a instalar (una linea).
   Espera unos segundos; si no llega nada, sigue como siempre. */
static void recibir_nombre(void) {
  g_pedido[0] = 0;
  int s = abrir_servidor(9096);
  if (s < 0) return;
  long t0 = ms_ahora();
  int fd = -1;
  while (ms_ahora() - t0 < 6000) {
    struct sockaddr_in ca;
    socklen_t cl = sizeof(ca);
    fd = accept(s, (struct sockaddr *)&ca, &cl);
    if (fd >= 0) break;
    usleep(50000);
  }
  if (fd >= 0) {
    int n = 0;
    int cerrado = 0;
    while (n < 1023 && ms_ahora() - t0 < 9000) {
      char ch;
      long r = recv(fd, &ch, 1, 0);
      if (r == 1) {
        if (ch == '\n' || ch == '\r') break;
        g_pedido[n++] = ch;
      } else if (r == 0) {
        cerrado = 1;
        break;
      } else {
        usleep(20000);
      }
    }
    g_pedido[n] = 0;
    if (cerrado) {
      close(fd);
    } else {
      poner_no_bloq(fd);
      g_app_fd = fd;   /* se queda abierta: los avisos tambien llegan a la app */
    }
  }
  close(s);
  if (g_pedido[0]) {
    anotar("la app pidio el archivo", 0);
    anotar(g_pedido, 0);
  } else {
    anotar("la app no pidio ningun archivo", 0);
  }
}

/* Espacio libre de la PS4 (bytes) en /user. Se busca statfs en la libreria del sistema; si no esta, devuelve -1. */
static int libre_ps4(long long *libre) {
  int hs[2];
  int nh = 0;
  hs[nh++] = 0x2001;
  int res = 0;
  int hk = sceKernelLoadStartModule("/system/common/lib/libkernel.sprx", 0, NULL, 0, NULL, &res);
  if (hk > 0) hs[nh++] = hk;
  void *f = NULL;
  for (int i = 0; i < nh && !f; i++) {
    void *t = NULL;
    if (resolver(hs[i], "statfs", NULL, &t) == 0 && t) f = t;
  }
  if (!f) return -1;
  uint8_t buf[1024];
  memset(buf, 0, sizeof(buf));
  if (((int (*)(const char *, void *))f)("/user", buf) != 0) return -1;
  uint64_t bsize = 0, bavail = 0;
  memcpy(&bsize, buf + 16, 8);    /* f_bsize  (disposicion de FreeBSD 9) */
  memcpy(&bavail, buf + 48, 8);   /* f_bavail */
  if (bsize < 512 || bsize > (1u << 22)) return -1;
  if (bavail > ((uint64_t)1 << 50) / bsize) return -1;
  *libre = (long long)(bavail * bsize);
  return 0;
}

static int principal(void) {
  char msg[400];
  signal(SIGPIPE, SIG_IGN);
  for (int i = 0; i < MAXC; i++) cs[i].fd = -1;
  anotar("v34 arranco", 0);
  avisar("v34: instalando desde el celular por USB...");
  recibir_nombre();

  int res = 0;
  int hu = sceKernelLoadStartModule("/system/common/lib/libSceUsbd.sprx", 0, NULL, 0, NULL, &res);
  anotar("cargar libSceUsbd", hu);
  if (hu < 0) {
    avisar("v34: no pude cargar libSceUsbd (mira el log)");
    return 0;
  }
  void *fn[8];
  const char *nombres[8] = {"sceUsbdInit",       "sceUsbdGetDeviceList", "sceUsbdGetDeviceDescriptor",
                            "sceUsbdOpen",       "sceUsbdClaimInterface", "sceUsbdBulkTransfer",
                            "sceUsbdReleaseInterface", "sceUsbdClose"};
  for (int i = 0; i < 8; i++) {
    if (resolver(hu, nombres[i], NULL, &fn[i]) != 0) {
      avisar("v34: faltan funciones de USB (mira el log)");
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
  unsigned int bcd = 0;
  for (int j = 0; j < n; j++) {
    unsigned char d[18];
    memset(d, 0, sizeof(d));
    if (p_desc(lista[j], d) != 0) continue;
    unsigned int vid = d[8] | (d[9] << 8);
    if (vid == 0x2717) {
      cel = lista[j];
      bcd = d[2] | (d[3] << 8);
      break;
    }
  }
  if (cel) {
    anotar("el celular dice ser USB (bcd, 0x200=USB2, 0x300=USB3)", (long)bcd);
    void *fvel = NULL;
    if (resolver(hu, "sceUsbdGetDeviceSpeed", NULL, &fvel) == 0 && fvel) {
      int sp = ((int (*)(void *))fvel)(cel);
      anotar("velocidad negociada del enlace USB (3=alta 480 Mbps, 4=super 5 Gbps, 5=super+)", sp);
      const char *nom = sp == 3 ? "USB 2.0 (480 Mbps)" : sp == 4 ? "USB 3 (5 Gbps)" : sp == 5 ? "USB 3.1 (10 Gbps)" : sp == 2 ? "USB lento (12 Mbps)" : "desconocida";
      snprintf(msg, sizeof(msg), "v34: enlace con el celular: %s", nom);
      avisar(msg);
    } else {
      anotar("no pude leer la velocidad del enlace USB", 0);
    }
  }
  if (!cel) {
    avisar("v34: no encontre el celular (cable y modo Transferencia de archivos)");
    return 0;
  }
  void *h = NULL;
  r = p_abrir(cel, &h);
  anotar("abrir celular", r);
  if (r != 0 || !h) {
    avisar("v34: no pude abrir el celular por USB (mira el log)");
    return 0;
  }
  r = p_reclamar(h, 0);
  anotar("reclamar interfaz 0", r);
  if (r != 0) {
    avisar("v34: no pude reclamar la interfaz USB del celular (mira el log)");
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
    avisar("v34: no encontre la carpeta Download en el celular");
    cerrar_usb();
    return 0;
  }
  uint32_t cand = 0;
  uint64_t ctam = 0;
  int por_nombre = 0;
  int visto_vacio = 0;
  /* Si la app pidio un archivo y todavia no aparece (o aparece con tamano 0), el celular puede estar
     actualizando su lista: se vuelve a mirar unas veces antes de rendirse. */
  for (int intento = 0; intento < 5; intento++) {
    cand = 0;
    ctam = 0;
    por_nombre = 0;
    visto_vacio = 0;
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
        if (g_pedido[0] && es_elegido(nombre) && t == 0) visto_vacio = 1;
        if (t > 0 && es_elegido(nombre) && (g_pedido[0] || t > 1048576ULL)) {
          cand = hs[i];
          ctam = t;
          por_nombre = 1;
        } else if (!g_pedido[0] && !por_nombre && t > 1048576ULL && t > ctam) {
          cand = hs[i];
          ctam = t;
        }
      }
    }
    if (!g_pedido[0] || por_nombre || intento == 4) break;
    if (intento == 0) avisar("v34: el celular aun no muestra ese archivo; espero unos segundos...");
    anotar("no aparece todavia, reintento", intento + 1);
    sleep(3);
  }
  if (g_pedido[0] && !por_nombre) {
    if (visto_vacio) {
      snprintf(msg, sizeof(msg),
               "v34: el celular muestra %s con tamano 0. Desconecta y conecta el cable (modo Transferencia de archivos) y prueba otra vez",
               g_pedido);
    } else {
      snprintf(msg, sizeof(msg),
               "v34: no encontre %s en Download. Revisa que este en esa carpeta y reconecta el cable para que el celular actualice su lista",
               g_pedido);
    }
    avisar(msg);
    cerrar_usb();
    return 0;
  }
  if (!cand) {
    avisar("v34: no hay un .pkg grande en la carpeta Download del celular");
    cerrar_usb();
    return 0;
  }
  g_obj = cand;
  g_tam = (long)ctam;
  anotar(por_nombre ? "elegido por nombre, bytes" : "elegido el mas grande, bytes", g_tam);
  {
    long long libre = 0;
    if (libre_ps4(&libre) == 0) {
      anotar("espacio libre en la PS4, bytes", (long)libre);
      char lb[96];
      int nl = snprintf(lb, sizeof(lb), "L|%lld|%ld\n", libre, g_tam);
      enviar_app(lb, nl);
      if (libre < (long long)g_tam + (long long)(g_tam / 20)) {
        snprintf(msg, sizeof(msg), "v34: poco espacio libre: %lld MB libres y el juego pesa %ld MB", libre / 1000000LL, g_tam / 1000000L);
        avisar(msg);
      }
    } else {
      anotar("no pude leer el espacio libre de la PS4", 0);
    }
  }

  int srv = -1;
  for (int i = 0; i < PUERTOS_PROBAR && srv < 0; i++) {
    g_puerto = PUERTO + i;
    srv = abrir_servidor(g_puerto);
  }
  anotar("servidor web (descriptor)", srv);
  anotar("servidor web (puerto)", g_puerto);
  if (srv < 0) {
    avisar("v34: no pude abrir el servidor web interno (mira el log)");
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
      avisar("v34: no pude hablar con Remote Package Installer (esta abierto en pantalla?)");
      close(srv);
      cerrar_usb();
      return 0;
    }
    snprintf(msg, sizeof(msg), "v34: orden enviada a Remote Package Installer (intento %d)", i + 1);
    avisar(msg);
    long t0 = ms_ahora();
    while (ms_ahora() - t0 < 15000 && g_conexiones == 0) paso(srv);
    anotar("conexiones recibidas tras la orden", g_conexiones);
  }
  if (g_conexiones == 0) {
    avisar("v34: nadie se conecto al servidor (mira el log)");
    close(srv);
    if (g_rpi >= 0) close(g_rpi);
    cerrar_usb();
    return 0;
  }

  long ult = ms_ahora();
  long t_ini = ult;
  int ult_pct = 0;
  long t_listo = 0;
  long t_prog = ult;
  long b_prog = 0;
  while (1) {
    int p = paso(srv);
    long ahora = ms_ahora();
    if (p) ult = ahora;
    if (g_listo && !t_listo) {
      t_listo = ahora;
      anotar("descarga completa en segundos", (t_listo - t_ini) / 1000);
      char fin[96];
      int nf = snprintf(fin, sizeof(fin), "P|%ld|%ld|0\n", g_tam, g_tam);
      enviar_app(fin, nf);
    }
    /* progreso para la barra de la app: una linea por segundo */
    if (g_app_fd >= 0 && !g_listo && g_tam > 0 && ahora - t_prog >= 1000) {
      long hecho = g_servido < g_tam ? g_servido : g_tam;
      long dt = ahora - t_prog;
      long mbs = (hecho - b_prog) / (dt * 1000);
      if (mbs < 0) mbs = 0;
      char pl[96];
      int np = snprintf(pl, sizeof(pl), "P|%ld|%ld|%ld\n", hecho, g_tam, mbs);
      enviar_app(pl, np);
      t_prog = ahora;
      b_prog = hecho;
    }
    if (ahora - ult > 180000) break;
    if (g_listo && ahora - ult > 15000) {
      anotar("descarga completa, cierro el servidor", 0);
      break;
    }
    if (!g_listo && g_servido >= g_tam && ahora - ult > 30000) {
      anotar("enviado todo el archivo y sin actividad, cierro el servidor", g_servido);
      g_listo = 1;
      break;
    }
    if (g_tam >= 200000000L && !g_listo) {
      long hecho = g_servido < g_tam ? g_servido : g_tam;
      int pct = (int)(hecho * 100 / g_tam);
      if (pct >= 99) pct = 99;
      pct = pct / 10 * 10;
      if (pct > ult_pct) {
        ult_pct = pct;
        long seg = (ahora - t_ini) / 1000;
        if (seg < 1) seg = 1;
        snprintf(msg, sizeof(msg), "v34: %d%% (%ld de %ld MB, %ld MB/s)", pct,
                 hecho / 1000000L, g_tam / 1000000L, hecho / 1000000L / seg);
        avisar(msg);
      }
    }
  }

  anotar("servidor termino, conexiones", g_conexiones);
  anotar("servidor termino, enviado total", g_servido);
  for (int i = 0; i < MAXC; i++) {
    if (cs[i].fd >= 0) conn_cerrar(&cs[i], "cierro al terminar, conexion");
  }
  if (g_rpi >= 0) close(g_rpi);
  close(srv);
  cerrar_usb();
  for (int i = 0; i < MAXC; i++) {
    if (cs[i].buf) {
      free(cs[i].buf);
      cs[i].buf = NULL;
    }
  }
  if (g_listo) {
    long seg = (t_listo - t_ini) / 1000;
    if (seg < 1) seg = 1;
    snprintf(msg, sizeof(msg), "v34: descarga completa (%ld MB en %ld s, %ld MB/s). Servidor cerrado.",
             g_tam / 1000000L, seg, g_tam / 1000000L / seg);
  } else {
    snprintf(msg, sizeof(msg),
             "v34: servidor termino sin completar. conexiones=%d enviado=%ld de %ld bytes",
             g_conexiones, g_servido, g_tam);
  }
  avisar(msg);
  return 0;
}

int main(void) {
  int r = principal();
  if (g_app_fd >= 0) {
    close(g_app_fd);
    g_app_fd = -1;
  }
  return r;
}
