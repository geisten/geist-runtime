/* test_service.c — the socket client (service_chat in tools/geistr/service.c)
 * against a fake service that answers with a stats line it chose: counts
 * that are negative or past UINT_MAX are clamped, not cast (#203). */
#include "service.h"
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int failures;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
            failures++;                                                  \
        }                                                                \
    } while (0)

static const char *reply;
static char        request[4096]; /* the client's request line */

static void *fake_service(void *arg) { /* one client: reads its request line, sends reply */
    int    fd = accept(*(int *) arg, nullptr, nullptr);
    char   c;
    size_t n = 0;
    while (fd >= 0 && read(fd, &c, 1) == 1 && c != '\n')
        if (n + 1 < sizeof request)
            request[n++] = c;
    request[n] = 0;
    if (fd >= 0 && write(fd, reply, strlen(reply)) < 0)
        perror("write");
    if (fd >= 0)
        close(fd);
    return nullptr;
}

static geistr_status ask(const char *line, struct svc_stats *stats, int think) {
    char dir[] = "/tmp/geistr-svc-XXXXXX", path[64], error[256];
    if (!mkdtemp(dir))
        return GEISTR_IO;
    snprintf(path, sizeof path, "%s/s.sock", dir);
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0 || bind(listener, (struct sockaddr *) &a, sizeof a) != 0 || listen(listener, 1) != 0)
        return GEISTR_IO;
    reply = line;
    pthread_t t;
    pthread_create(&t, nullptr, fake_service, &listener);
    geistr_message m = {.role = "user", .content = "Hi"};
    geistr_status  s = service_chat(path, 1, &m, 0, 0, think, nullptr, nullptr, nullptr, stats, error, sizeof error);
    pthread_join(t, nullptr);
    close(listener);
    unlink(path);
    rmdir(dir);
    return s;
}

int main(void) {
    struct svc_stats st;
    CHECK(ask("{\"done\":true,\"finish\":\"stop\",\"input_tokens\":14,\"context_tokens\":52,\"output_tokens\":7}\n", &st, -1) ==
          GEISTR_OK);
    CHECK(!strstr(request, "\"think\"")); /* the service's default (#266) */
    CHECK(st.input_tokens == 14 && st.context_tokens == 52 && st.output_tokens == 7 && !strcmp(st.finish, "stop"));
    CHECK(ask("{\"done\":true,\"input_tokens\":-1,\"context_tokens\":1e300,\"output_tokens\":-4e300}\n", &st, 0) == GEISTR_OK);
    CHECK(st.input_tokens == 0 && st.context_tokens == UINT_MAX && st.output_tokens == 0);
    CHECK(strstr(request, "\"think\":false,"));
    CHECK(ask("{\"done\":true}\n", &st, 1) == GEISTR_OK && strstr(request, "\"think\":true,"));

    /* #266: the levels clients send (OpenAI reasoning_effort, Ollama think). */
    int           think  = -1;
    geistr_effort effort = GEISTR_EFFORT_DEFAULT;
    CHECK(svc_think_level("none", &think, &effort) && think == 0);
    CHECK(svc_think_level("minimal", &think, &effort) && think == 1 && effort == GEISTR_EFFORT_LOW);
    CHECK(svc_think_level("low", &think, &effort) && think == 1 && effort == GEISTR_EFFORT_LOW);
    CHECK(svc_think_level("medium", &think, &effort) && think == 1 && effort == GEISTR_EFFORT_MEDIUM);
    CHECK(svc_think_level("high", &think, &effort) && think == 1 && effort == GEISTR_EFFORT_HIGH);
    CHECK(svc_think_level("xhigh", &think, &effort) && think == 1 && effort == GEISTR_EFFORT_HIGH);
    CHECK(!svc_think_level("maximum", &think, &effort) && !svc_think_level(nullptr, &think, &effort));
    if (failures)
        return 1;
    puts("service client: stats line, out-of-range counts clamped, think sent, think levels passed");
    return 0;
}
