/* REPO ps4-payload: v17 (prueba: pide la instalacion a Remote Package Installer, puerto 12800, con un mini servidor local) */
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

  FILE *f = fopen(ruta, "rb");
  if (!f) return;
  fseek(f, ini, SEEK_SET);
  static char buf[262144];
  long falta = largo;
  while (falta > 0) {
    size_t quiero = falta > (long)sizeof(buf) ? sizeof(buf) : (size_t)falta;
    size_t got = fread(buf, 1, quiero, f);
    if (got == 0) break;
    if (mandar(c, buf, (long)got) != 0) break;
    falta -= (long)got;
    g_servido += (long)got;
  }
  fclose(f);
  anotar("enviado en este pedido", largo - falta);
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
      if (g_conexiones == 1) avisar("v17: el instalador se conecto al servidor");
      atender(c, ruta, total);
      close(c);
      ocioso = 0;
    } else {
      usleep(20000);
      ocioso += 20;
    }
    if (g_conexiones == 0 && ocioso > 40000) break;
    if (g_conexiones > 0 && ocioso > 120000) break;
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
      if (g_conexiones == 1) avisar("v17: el instalador se conecto al servidor");
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
  anotar("v17 arranco", 0);

  long ta = tamano(RUTA_A);
  long tb = tamano(RUTA_B);
  anotar("tamano /user/data/pkg", ta);
  anotar("tamano /data/pkg", tb);
  if (ta < 0 && tb < 0) {
    avisar("v17: no existe ps4-usb-prueba.pkg en /data/pkg");
    return 0;
  }
  const char *ruta = (ta >= 0) ? RUTA_A : RUTA_B;
  long tam = (ta >= 0) ? ta : tb;

  int srv = -1;
  for (int i = 0; i < PUERTOS_PROBAR && srv < 0; i++) {
    g_puerto = PUERTO + i;
    srv = abrir_servidor(g_puerto);
  }
  anotar("servidor web (descriptor)", srv);
  anotar("servidor web (puerto)", g_puerto);
  if (srv < 0) {
    avisar("v17: no pude abrir el servidor web interno (mira el log)");
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
    int n = -1;
    for (int k = 1; k <= 3; k++) {
      n = pedir_instalacion(urls[i], resp, sizeof(resp));
      if (n >= 0) break;
      anotar("reintento de conexion con RPI", k);
      sleep(3);
    }
    anotar("respuesta de RPI (bytes)", n);
    if (n > 0) anotar(resp, 0);
    if (n < 0) {
      avisar("v17: no pude hablar con Remote Package Installer (esta abierto en pantalla?)");
      close(srv);
      return 0;
    }
    snprintf(msg, sizeof(msg), "v17: RPI respondio %d bytes (intento %d)", n, i + 1);
    avisar(msg);
    conectado = esperar_conexion(srv, ruta, tam, 12000);
    anotar("conexion recibida (0=no)", conectado);
  }
  if (!conectado) {
    avisar("v17: nadie se conecto al servidor (mira el log)");
    close(srv);
    return 0;
  }

  servir(srv, ruta, tam);
  anotar("servidor termino, conexiones", g_conexiones);
  anotar("servidor termino, enviado total", g_servido);
  snprintf(msg, sizeof(msg),
           "v17: servidor termino. conexiones=%d enviado=%ld de %ld bytes",
           g_conexiones, g_servido, tam);
  avisar(msg);
  return 0;
}
