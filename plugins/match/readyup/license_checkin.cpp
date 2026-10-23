#include "readyup/license_checkin.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <regex>
#include <string>

#include "readyup/minijson.h"

namespace readyup::license_checkin {
namespace {

constexpr const char* kDefaultUrl = "https://autotournament.gg/api/licenses/checkin";
constexpr auto kEvery = std::chrono::minutes(15);
constexpr auto kFirst = std::chrono::seconds(60);  // server.cfg sets the key after load

#ifndef MATCH_VERSION
#define MATCH_VERSION "0.0.0"
#endif

bool IsUuid(const std::string& s) {
  static const std::regex re("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$");
  return std::regex_match(s, re);
}

std::string NewUuid() {
  std::random_device rd;
  std::uniform_int_distribution<int> d(0, 15);
  const char* hex = "0123456789abcdef";
  std::string out;
  for (int i = 0; i < 32; ++i) {
    if (i == 8 || i == 12 || i == 16 || i == 20) out += '-';
    int v = d(rd);
    if (i == 12) v = 4;                 // version 4
    if (i == 16) v = 8 | (v & 3);       // variant
    out += hex[v];
  }
  return out;
}

// The stop signal for the check-in thread (Stop()).
std::mutex g_mu;
std::condition_variable g_cv;
bool g_stop = false;
std::thread g_thread;
int g_leadFd = -1;

// Waits `d`, or less when Stop() is called: false once stopping.
bool SleepFor(std::chrono::milliseconds d) {
  std::unique_lock<std::mutex> lk(g_mu);
  return !g_cv.wait_for(lk, d, [] { return g_stop; });
}

// The match and fleet plugins both run this; one of them sends (whoever holds the lock file),
// the other takes over when that one unloads. Both send the same server anyway (same UUID file).
bool Lead(const std::string& dir) {
  if (g_leadFd >= 0) return true;
  const int fd = open((dir + "/server_id.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
  if (fd < 0) return false;
  if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
    g_leadFd = fd;
    return true;
  }
  close(fd);
  return false;
}

// This server's own id: read from the shared data folder, or made and saved the first time.
std::string ServerUuid(const std::string& dir) {
  const std::string path = dir + "/server_id";
  std::string id;
  if (std::ifstream in(path); in >> id && IsUuid(id)) return id;
  id = NewUuid();
  std::ofstream(path) << id << "\n";
  return id;
}

std::string ReadCmdline() {
  std::ifstream in("/proc/self/cmdline", std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string Base64UrlDecode(const std::string& in) {
  std::string out;
  int val = 0, bits = -8;
  for (const char c : in) {
    int d;
    if (c >= 'A' && c <= 'Z') d = c - 'A';
    else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
    else if (c >= '0' && c <= '9') d = c - '0' + 52;
    else if (c == '-') d = 62;
    else if (c == '_') d = 63;
    else break;
    val = (val << 6) + d;
    bits += 6;
    if (bits >= 0) {
      out += static_cast<char>((val >> bits) & 0xFF);
      bits -= 8;
    }
  }
  return out;
}

void Loop(const ru_api* api, std::string dir, std::string version, PostFn post) {
  if (!SleepFor(kFirst)) return;
  const char* env = std::getenv("READYUP_LICENSE_CHECKIN_URL");  // tests point it at a local server
  const std::string url = env && *env ? env : kDefaultUrl;
  const std::string uuid = ServerUuid(dir);
  const int port = PortFromCmdline(ReadCmdline());
  do {
    if (!Lead(dir)) continue;
    char buf[4096];
    const int n = api->license_key(api->self, buf, sizeof(buf));
    if (n <= 0 || n >= static_cast<int>(sizeof(buf))) continue;
    const std::string key(buf, static_cast<size_t>(n));
    const std::string id = KeyId(key);
    if (id.empty()) continue;
    // Key, id, UUID and version are [A-Za-z0-9._+-] only (checked by the key format and IsUuid), so no escaping.
    const std::string body = std::string("{\"token\":\"") + key + "\",\"key_id\":\"" + id + "\",\"instance_id\":\"" + uuid +
                             "\",\"server_count\":1,\"platform_version\":\"" + version + "\",\"product\":\"readyup\",\"port\":" +
                             std::to_string(port) + "}";
    post(url, body);  // best effort: the answer changes nothing here
  } while (SleepFor(kEvery));
}

}  // namespace

std::string KeyId(const std::string& token) {
  const size_t a = token.find('.');
  const size_t b = a == std::string::npos ? std::string::npos : token.find('.', a + 1);
  if (b == std::string::npos) return {};
  const auto v = minijson::Parse(Base64UrlDecode(token.substr(a + 1, b - a - 1)), nullptr);
  const minijson::Value* id = v ? v->get("id") : nullptr;
  if (!id || !minijson::IsString(id)) return {};
  static const std::regex re("^[A-Za-z0-9_-]{1,64}$");
  return std::regex_match(id->str, re) ? id->str : std::string();
}

int PortFromCmdline(const std::string& cmdline) {
  std::string prev, arg;
  for (const char c : cmdline + std::string(1, '\0')) {
    if (c != '\0') {
      arg += c;
      continue;
    }
    if (prev == "-port") {
      char* end = nullptr;
      const long p = std::strtol(arg.c_str(), &end, 10);
      if (end && *end == '\0' && p >= 1 && p <= 65535) return static_cast<int>(p);
    }
    prev = arg;
    arg.clear();
  }
  return 27015;
}

void Start(const ru_api* api, const std::string& dataDir, const std::string& version, PostFn post) {
  if (!RU_API_HAS(api, license_key) || !api->license_key || g_thread.joinable()) return;
  // The folder every plugin's data folder sits in: one server id and one lock per server.
  const size_t slash = dataDir.find_last_of('/');
  const std::string shared = slash == std::string::npos ? dataDir : dataDir.substr(0, slash);
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_stop = false;
  }
  g_thread = std::thread([api, shared, version, post] { Loop(api, shared, version, post); });
}

void Stop() {
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_stop = true;
  }
  g_cv.notify_all();
  if (g_thread.joinable()) g_thread.join();
  if (g_leadFd >= 0) {
    close(g_leadFd);  // releases the lock: the other plugin takes over
    g_leadFd = -1;
  }
}

}  // namespace readyup::license_checkin
