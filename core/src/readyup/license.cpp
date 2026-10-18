#include "readyup/license.h"

#include "readyup/minijson.h"

extern "C" {
#include "optional/monocypher-ed25519.h"
}

#include <cmath>
#include <cstdio>
#include <ctime>

namespace readyup::license {
namespace {

namespace mj = readyup::minijson;

int B64UrlValue(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '-') return 62;
  if (c == '_') return 63;
  return -1;
}

bool IsJsWhitespace(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }

std::string Trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && IsJsWhitespace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && IsJsWhitespace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

bool IsDigits(const std::string& s, size_t pos, size_t n) {
  if (pos + n > s.size()) return false;
  for (size_t i = pos; i < pos + n; ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
  }
  return true;
}

int Num(const std::string& s, size_t pos, size_t n) {
  int v = 0;
  for (size_t i = pos; i < pos + n; ++i) v = v * 10 + (s[i] - '0');
  return v;
}

// Date.parse() accepts far more, but issued_at is always written as ISO 8601 by the website.
// Accepted: YYYY-MM-DD, optionally followed by THH:MM[:SS[.fff]] and Z or ±HH:MM.
bool IsIsoTimestamp(const std::string& s) {
  if (s.size() < 10 || !IsDate(s.substr(0, 10))) return false;
  size_t i = 10;
  if (i == s.size()) return true;
  if (s[i] != 'T') return false;
  ++i;
  if (!IsDigits(s, i, 2) || i + 2 >= s.size() || s[i + 2] != ':' || !IsDigits(s, i + 3, 2)) return false;
  if (Num(s, i, 2) > 23 || Num(s, i + 3, 2) > 59) return false;
  i += 5;
  if (i < s.size() && s[i] == ':') {
    if (!IsDigits(s, i + 1, 2) || Num(s, i + 1, 2) > 59) return false;
    i += 3;
    if (i < s.size() && s[i] == '.') {
      ++i;
      const size_t start = i;
      while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
      if (i == start) return false;
    }
  }
  if (i == s.size()) return true;
  if (s[i] == 'Z') return i + 1 == s.size();
  if (s[i] == '+' || s[i] == '-') {
    return i + 6 == s.size() && IsDigits(s, i + 1, 2) && s[i + 3] == ':' && IsDigits(s, i + 4, 2) &&
           Num(s, i + 1, 2) <= 23 && Num(s, i + 4, 2) <= 59;
  }
  return false;
}

bool OneOf(const mj::Value* v, std::initializer_list<const char*> options) {
  if (!mj::IsString(v)) return false;
  for (const char* o : options) {
    if (v->str == o) return true;
  }
  return false;
}

bool NonEmptyString(const mj::Value* v) { return mj::IsString(v) && !v->str.empty(); }

// scripts/license-verify.mjs payloadProblem(): an error message, or "" for a v1 license.
std::string PayloadProblem(const mj::Value& p) {
  if (p.type != mj::Value::Type::Object) return "payload is not an object";
  const mj::Value* v = p.get("v");
  if (!mj::IsNumber(v) || v->num != 1.0) return "unsupported version";
  if (!NonEmptyString(p.get("kid"))) return "missing kid";
  if (!NonEmptyString(p.get("id"))) return "missing id";
  if (!NonEmptyString(p.get("customer"))) return "missing customer";
  if (!OneOf(p.get("product"), {"servers", "platform"})) return "bad product";
  if (!OneOf(p.get("pack"), {"S", "M", "L"})) return "bad pack";
  const mj::Value* ms = p.get("max_servers");
  // Number.isSafeInteger(max_servers) && max_servers >= 1
  if (!mj::IsNumber(ms) || !std::isfinite(ms->num) || std::floor(ms->num) != ms->num || ms->num < 1 ||
      ms->num > 9007199254740991.0) {
    return "bad max_servers";
  }
  if (!OneOf(p.get("kind"), {"month", "event", "year", "founder"})) return "bad kind";
  const mj::Value* issued = p.get("issued_at");
  if (!mj::IsString(issued) || !IsIsoTimestamp(issued->str)) return "bad issued_at";
  const mj::Value* uu = p.get("updates_until");
  if (!mj::IsString(uu) || !IsDate(uu->str)) return "bad updates_until";
  const mj::Value* vf = p.get("valid_from");
  const mj::Value* vt = p.get("valid_to");
  if ((vf == nullptr) != (vt == nullptr)) return "valid_from and valid_to go together";
  if (vf != nullptr &&
      (!mj::IsString(vf) || !mj::IsString(vt) || !IsDate(vf->str) || !IsDate(vt->str) || vf->str > vt->str)) {
    return "bad valid_from/valid_to";
  }
  const mj::Value* lic = p.get("licensee");
  if (lic != nullptr && !mj::IsString(lic)) return "bad licensee";
  const mj::Value* lease = p.get("lease");
  if (lease != nullptr && lease->type != mj::Value::Type::Bool) return "bad lease";
  return {};
}

Result Invalid(const std::string& code, const std::string& message) {
  Result r;
  r.status = Status::Invalid;
  r.issues.push_back({code, message});
  return r;
}

std::string Str(const mj::Value& p, const char* key) {
  const mj::Value* v = p.get(key);
  return mj::IsString(v) ? v->str : std::string();
}

std::string ProductName(const std::string& product) {
  if (product == "servers") return "Servers";
  if (product == "platform") return "Platform";
  return Printable(product, 16);
}

}  // namespace

bool IsDate(const std::string& s) {
  if (s.size() != 10 || !IsDigits(s, 0, 4) || s[4] != '-' || !IsDigits(s, 5, 2) || s[7] != '-' || !IsDigits(s, 8, 2)) {
    return false;
  }
  const int y = Num(s, 0, 4), m = Num(s, 5, 2), d = Num(s, 8, 2);
  if (m < 1 || m > 12 || d < 1) return false;
  static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
  const int last = (m == 2 && leap) ? 29 : kDays[m - 1];
  return d <= last;
}

bool Base64UrlDecode(const std::string& in, std::vector<uint8_t>* out) {
  out->clear();
  out->reserve(in.size() * 3 / 4);
  uint32_t acc = 0;
  int bits = 0;
  for (char c : in) {
    const int v = B64UrlValue(c);
    if (v < 0) return false;
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out->push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
    }
  }
  return true;
}

std::string TodayUtc() {
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
  gmtime_r(&now, &tm);
  char buf[16];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
  return buf;
}

std::string Printable(const std::string& s, size_t max_bytes) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7F) continue;
    out.push_back(c);
  }
  if (out.size() > max_bytes) {
    size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
    out.resize(cut);
  }
  return out;
}

Result Verify(const std::string& token, const Options& opts) {
  const std::vector<PublicKey>& keys = opts.keys ? *opts.keys : EmbeddedPublicKeys();

  // decodeLicense()
  const std::string t = Trim(token);
  if (t.size() > kMaxTokenLength) return Invalid("malformed", "This is not a valid license key (too long).");
  const size_t d1 = t.find('.');
  const size_t d2 = d1 == std::string::npos ? std::string::npos : t.find('.', d1 + 1);
  if (d2 == std::string::npos || t.find('.', d2 + 1) != std::string::npos || t.compare(0, d1, kTokenPrefix) != 0 ||
      d1 != std::char_traits<char>::length(kTokenPrefix)) {
    return Invalid("malformed", "This is not a valid license key (not an ATL1 key).");
  }
  const std::string body = t.substr(d1 + 1, d2 - d1 - 1);
  const std::string sig = t.substr(d2 + 1);
  std::vector<uint8_t> bodyBytes, sigBytes;
  if (body.empty() || sig.empty() || !Base64UrlDecode(body, &bodyBytes) || !Base64UrlDecode(sig, &sigBytes)) {
    return Invalid("malformed", "This is not a valid license key (not base64url).");
  }
  mj::ParseError perr;
  const auto parsed = mj::Parse(std::string(bodyBytes.begin(), bodyBytes.end()), &perr);
  if (!parsed) return Invalid("malformed", "This is not a valid license key (the payload is not JSON).");
  if (sigBytes.size() != 64) return Invalid("malformed", "This is not a valid license key (bad signature length).");
  const mj::Value& payload = *parsed;

  // verifyLicense()
  const mj::Value* v = payload.get("v");
  if (mj::IsNumber(v) && v->num != 1.0) {
    char num[32];
    std::snprintf(num, sizeof(num), "%g", v->num);
    return Invalid("unsupported_version", std::string("License key version ") + num + " needs a newer release.");
  }
  const mj::Value* kidV = payload.get("kid");
  const PublicKey* key = nullptr;
  if (mj::IsString(kidV)) {
    for (const auto& k : keys) {
      if (kidV->str == k.kid) {
        key = &k;
        break;
      }
    }
  }
  if (!key) return Invalid("unknown_kid", "This license key was signed with a key this release does not know.");

  std::vector<uint8_t> pub;
  bool ok = Base64UrlDecode(key->x, &pub) && pub.size() == 32;
  if (ok) {
    const std::string signedPart = std::string(kTokenPrefix) + "." + body;
    ok = crypto_ed25519_check(sigBytes.data(), pub.data(), reinterpret_cast<const uint8_t*>(signedPart.data()),
                              signedPart.size()) == 0;
  }
  if (!ok) {
    return Invalid("bad_signature", "The license key signature does not match: the key was changed or mistyped.");
  }

  const std::string problem = PayloadProblem(payload);
  if (!problem.empty()) return Invalid("malformed", "The license key content is not valid (" + problem + ").");

  Result r;
  Payload& p = r.license;
  p.kid = Str(payload, "kid");
  p.id = Str(payload, "id");
  p.customer = Str(payload, "customer");
  p.licensee = Str(payload, "licensee");
  p.product = Str(payload, "product");
  p.pack = Str(payload, "pack");
  p.max_servers = static_cast<int64_t>(payload.get("max_servers")->num);
  p.kind = Str(payload, "kind");
  p.issued_at = Str(payload, "issued_at");
  p.updates_until = Str(payload, "updates_until");
  p.valid_from = Str(payload, "valid_from");
  p.valid_to = Str(payload, "valid_to");
  const mj::Value* lease = payload.get("lease");
  p.lease = lease != nullptr && lease->type == mj::Value::Type::Bool && lease->b;

  // Founder keys carry updates_until 9999-12-31; covered whatever the line date.
  if (p.kind != "founder" && !opts.line_date.empty() && opts.line_date > p.updates_until) {
    r.issues.push_back({"updates_expired", "updates for this version line ended " + p.updates_until +
                                               " (this build's line is from " + opts.line_date + ")"});
  }
  if (!p.valid_to.empty() && !opts.today.empty() && opts.today > p.valid_to) {
    r.issues.push_back({"period_ended", "the event window ended " + p.valid_to});
  }
  if (!p.valid_from.empty() && !opts.today.empty() && opts.today < p.valid_from) {
    r.issues.push_back({"period_not_started", "the event window starts " + p.valid_from});
  }
  r.status = r.issues.empty() ? Status::Ok : Status::Warning;
  return r;
}

std::string ConsoleLine(const Result& r) {
  if (r.status == Status::Invalid) {
    const std::string why = r.issues.empty() ? std::string("unknown reason") : r.issues.front().message;
    return "License warning: the license key is not valid: " + why + " Ready Up runs as normal.";
  }
  const Payload& p = r.license;
  const std::string id = Printable(p.id, 64);
  const std::string licensee = Printable(p.licensee);
  if (r.status == Status::Warning) {
    std::string msg;
    for (size_t i = 0; i < r.issues.size(); ++i) msg += (i ? "; " : "") + r.issues[i].message;
    return "License warning: " + msg + " (license " + id + (licensee.empty() ? "" : ", " + licensee) + ")";
  }
  std::string kind;
  if (p.kind == "event") {
    kind = p.valid_from == p.valid_to ? "event " + p.valid_to : "event " + p.valid_from + " to " + p.valid_to;
  } else if (p.kind == "month") {
    kind = "monthly, paid until " + p.updates_until;
  } else if (p.kind == "year") {
    kind = "yearly, updates until " + p.updates_until;
  } else {
    kind = "founder";
  }
  std::string line = "License: ";
  if (!licensee.empty()) line += licensee + " \xC2\xB7 ";
  line += ProductName(p.product) + " " + p.pack + " (" + std::to_string(p.max_servers) +
          (p.max_servers == 1 ? " server) " : " servers) ");
  line += "\xC2\xB7 " + kind + " \xC2\xB7 valid (license " + id + ")";
  return line;
}

std::string PlayerLine(const Result& r) {
  if (!r.valid()) return {};
  const std::string licensee = Printable(r.license.licensee);
  if (licensee.empty()) return {};
  return "Licensed to " + licensee;
}

std::string AddDays(const std::string& day, int days) {
  if (!IsDate(day)) return {};
  std::tm t{};
  t.tm_year = std::stoi(day.substr(0, 4)) - 1900;
  t.tm_mon = std::stoi(day.substr(5, 2)) - 1;
  t.tm_mday = std::stoi(day.substr(8, 2)) + days;
  t.tm_hour = 12;
  const time_t at = timegm(&t);
  std::tm out{};
  gmtime_r(&at, &out);
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", out.tm_year + 1900, out.tm_mon + 1, out.tm_mday);
  return buf;
}

Standing StandingFor(const std::string& key, const std::string& lease, const std::string& state,
                     const std::string& state_stops_on, const std::string& today,
                     const std::vector<PublicKey>* keys) {
  Standing st;
  if (key.empty()) return st;
  Options o;
  o.today = today;
  o.keys = keys;
  const Result k = Verify(key, o);
  if (!k.valid() || k.license.lease) {
    st.status = "invalid";
    return st;
  }
  Payload p = k.license;
  if (!lease.empty()) {
    const Result l = Verify(lease, o);
    if (l.valid() && l.license.lease && l.license.id == p.id && l.license.kind == p.kind && l.license.issued_at >= p.issued_at) {
      p = l.license;
    }
  }
  st.paid = true;
  st.status = "active";
  st.max_servers = p.max_servers;

  if (state == "in_use_elsewhere") {
    st.status = "expired";
    st.reason = "in_use_elsewhere";
    st.stops_on = state_stops_on;
    return st;
  }
  if (state == "replaced") {
    st.reason = "replaced";
    st.stops_on = state_stops_on;
    st.status = (state_stops_on.empty() || today >= state_stops_on) ? "expired" : "past_due";
    return st;
  }
  if (state == "revoked") {
    return Standing{false, "invalid", "", "", 0};
  }
  if (p.kind == "month") {
    // Works through the 14th day after the last paid day; stops on the 15th.
    const std::string stops = AddDays(p.updates_until, kGraceDays + 1);
    if (!stops.empty() && today >= stops) {
      st.status = "expired";
      st.stops_on = stops;
    } else if (today > p.updates_until) {
      st.status = "past_due";
      st.stops_on = stops;
    }
  }
  // The license server can only make it stricter (a renewal arrives as a lease).
  auto rank = [](const std::string& s) { return s == "expired" ? 2 : s == "past_due" ? 1 : 0; };
  if ((state == "past_due" || state == "expired") && rank(state) > rank(st.status)) {
    st.status = state;
    if (!state_stops_on.empty()) st.stops_on = state_stops_on;
  }
  if (st.status != "active") st.reason = "unpaid";
  return st;
}

}  // namespace readyup::license
