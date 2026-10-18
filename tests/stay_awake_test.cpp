// Offline tests for core/src/readyup/stay_awake.h: which command text turns empty-server
// hibernation back on (the core then queues sv_hibernate_when_empty 0 again). ctest `stay_awake`.
#include "readyup/stay_awake.h"

#include <cstdio>
#include <string>

using namespace readyup;

static int g_failures = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

int main() {
  CHECK(TextTurnsHibernateOn("sv_hibernate_when_empty 1"));
  CHECK(TextTurnsHibernateOn("sv_hibernate_when_empty 1\n"));
  CHECK(TextTurnsHibernateOn("sv_hibernate_when_empty \"1\""));
  CHECK(TextTurnsHibernateOn("  sv_hibernate_when_empty   2  "));
  CHECK(TextTurnsHibernateOn("log on; sv_hibernate_when_empty 1; mp_warmup_start"));
  CHECK(TextTurnsHibernateOn("hostname x\r\nsv_hibernate_when_empty 1 // sleep when empty\r\n"));

  CHECK(!TextTurnsHibernateOn(kStayAwakeCommand));
  CHECK(!TextTurnsHibernateOn(std::string(kStayAwakeCommand) + "\n"));
  CHECK(!TextTurnsHibernateOn("sv_hibernate_when_empty \"0\""));
  CHECK(!TextTurnsHibernateOn("sv_hibernate_when_empty"));  // a query
  CHECK(!TextTurnsHibernateOn("sv_hibernate_when_empty_x 1"));
  CHECK(!TextTurnsHibernateOn("// sv_hibernate_when_empty 1"));
  CHECK(!TextTurnsHibernateOn("say \"sv_hibernate_when_empty 1; yes\""));
  CHECK(!TextTurnsHibernateOn(""));
  CHECK(!TextTurnsHibernateOn("exec server.cfg"));

  if (g_failures) {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("stay_awake: ok\n");
  return 0;
}
