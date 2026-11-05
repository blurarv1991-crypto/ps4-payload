#include <stdio.h>
#include <string.h>

typedef struct {
  char pad[45];
  char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int dev, notify_request_t *req,
                                     size_t size, int blocking);

static void anotar(const char *txt, int valor) {
  FILE *f = fopen("/data/ps4-payload.log", "a");
  if (f) {
    fprintf(f, "%s %d\n", txt, valor);
    fclose(f);
  }
}

int main(void) {
  notify_request_t req;

  anotar("payload arranco", 0);

  memset(&req, 0, sizeof(req));
  strcpy(req.message, "Payload OK desde ps4-payload");

  int r = sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
  anotar("notificacion devolvio", r);

  return 0;
}
