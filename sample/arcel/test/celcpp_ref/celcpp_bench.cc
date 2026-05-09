// Minimal cel-cpp benchmark CLI for comparing against arcel.
//
// Usage:  celcpp_bench -e '<expr>' [-i '<json>'] -n <iterations>
// Output: "<iters> <elapsed_ns> <ns_per_op>" (same as celgo_ref / arcel
//         so benchmark/run.rb can ingest all three uniformly).
//
// This intentionally mirrors arcel's `bench` subcommand:
//   - parse + plan once outside the loop
//   - bindings parsed once outside the loop
//   - per-iter cost = pure Evaluate() + arena reset, nothing else

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "cel/expr/syntax.pb.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "eval/public/activation.h"
#include "eval/public/builtin_func_registrar.h"
#include "eval/public/cel_expr_builder_factory.h"
#include "eval/public/cel_expression.h"
#include "eval/public/cel_options.h"
#include "eval/public/cel_value.h"
#include "eval/public/containers/container_backed_list_impl.h"
#include "eval/public/containers/container_backed_map_impl.h"
#include "google/protobuf/arena.h"
#include "parser/parser.h"

using ::cel::expr::ParsedExpr;
using ::google::api::expr::parser::Parse;
using ::google::api::expr::runtime::Activation;
using ::google::api::expr::runtime::CelExpressionBuilder;
using ::google::api::expr::runtime::CelValue;
using ::google::api::expr::runtime::ContainerBackedListImpl;
using ::google::api::expr::runtime::CreateCelExpressionBuilder;
using ::google::api::expr::runtime::CreateContainerBackedMap;
using ::google::api::expr::runtime::InterpreterOptions;
using ::google::api::expr::runtime::RegisterBuiltinFunctions;

// ---- minimal JSON parser (just what the bench harness emits) ----------
//
// Supports objects/arrays/strings/numbers/bools/null.  Returns CelValue
// rooted in the supplied arena.  Strings are interned via arena.
// Errors print to stderr and exit().
namespace {

struct JsonParser {
  const char* src;
  size_t pos;
  size_t len;
  google::protobuf::Arena* arena;

  void Skip() {
    while (pos < len) {
      char c = src[pos];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos;
      else break;
    }
  }
  [[noreturn]] void Fail(const char* msg) {
    fprintf(stderr, "celcpp_bench: json parse: %s at %zu\n", msg, pos);
    exit(2);
  }
};

CelValue ParseJsonValue(JsonParser& p);

std::string ParseJsonString(JsonParser& p) {
  if (p.pos >= p.len || p.src[p.pos] != '"') p.Fail("expected '\"'");
  ++p.pos;
  std::string out;
  while (p.pos < p.len && p.src[p.pos] != '"') {
    char c = p.src[p.pos];
    if (c == '\\') {
      if (++p.pos >= p.len) p.Fail("trailing backslash");
      char esc = p.src[p.pos++];
      switch (esc) {
        case '"':  out += '"'; break;
        case '\\': out += '\\'; break;
        case '/':  out += '/'; break;
        case 'n':  out += '\n'; break;
        case 't':  out += '\t'; break;
        case 'r':  out += '\r'; break;
        case 'b':  out += '\b'; break;
        case 'f':  out += '\f'; break;
        case 'u': {
          if (p.pos + 4 > p.len) p.Fail("bad \\u");
          unsigned cp = 0;
          for (int i = 0; i < 4; ++i) {
            char h = p.src[p.pos++];
            cp <<= 4;
            if (h >= '0' && h <= '9') cp |= h - '0';
            else if (h >= 'a' && h <= 'f') cp |= 10 + h - 'a';
            else if (h >= 'A' && h <= 'F') cp |= 10 + h - 'A';
            else p.Fail("bad hex");
          }
          if (cp < 0x80) out += static_cast<char>(cp);
          else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
          } else {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
          }
          break;
        }
        default: out += esc; break;
      }
    } else {
      out += c;
      ++p.pos;
    }
  }
  if (p.pos >= p.len) p.Fail("unterminated string");
  ++p.pos;
  return out;
}

double ParseJsonNumber(JsonParser& p, bool& is_float) {
  size_t start = p.pos;
  is_float = false;
  if (p.src[p.pos] == '-') ++p.pos;
  while (p.pos < p.len && p.src[p.pos] >= '0' && p.src[p.pos] <= '9') ++p.pos;
  if (p.pos < p.len && p.src[p.pos] == '.') {
    is_float = true;
    ++p.pos;
    while (p.pos < p.len && p.src[p.pos] >= '0' && p.src[p.pos] <= '9') ++p.pos;
  }
  if (p.pos < p.len && (p.src[p.pos] == 'e' || p.src[p.pos] == 'E')) {
    is_float = true;
    ++p.pos;
    if (p.pos < p.len && (p.src[p.pos] == '-' || p.src[p.pos] == '+')) ++p.pos;
    while (p.pos < p.len && p.src[p.pos] >= '0' && p.src[p.pos] <= '9') ++p.pos;
  }
  std::string s(p.src + start, p.pos - start);
  return is_float ? std::stod(s) : static_cast<double>(std::stoll(s));
}

CelValue ParseJsonValue(JsonParser& p) {
  p.Skip();
  if (p.pos >= p.len) p.Fail("unexpected EOF");
  char c = p.src[p.pos];
  if (c == '"') {
    auto* s = google::protobuf::Arena::Create<std::string>(p.arena, ParseJsonString(p));
    return CelValue::CreateString(s);
  }
  if (c == 't') {
    if (p.pos + 4 > p.len || std::strncmp(p.src + p.pos, "true", 4) != 0) p.Fail("expected true");
    p.pos += 4; return CelValue::CreateBool(true);
  }
  if (c == 'f') {
    if (p.pos + 5 > p.len || std::strncmp(p.src + p.pos, "false", 5) != 0) p.Fail("expected false");
    p.pos += 5; return CelValue::CreateBool(false);
  }
  if (c == 'n') {
    if (p.pos + 4 > p.len || std::strncmp(p.src + p.pos, "null", 4) != 0) p.Fail("expected null");
    p.pos += 4; return CelValue::CreateNull();
  }
  if (c == '[') {
    ++p.pos;
    p.Skip();
    std::vector<CelValue> items;
    if (p.pos < p.len && p.src[p.pos] != ']') {
      while (true) {
        items.push_back(ParseJsonValue(p));
        p.Skip();
        if (p.pos < p.len && p.src[p.pos] == ',') { ++p.pos; p.Skip(); continue; }
        break;
      }
    }
    if (p.pos >= p.len || p.src[p.pos] != ']') p.Fail("expected ']'");
    ++p.pos;
    auto* list = google::protobuf::Arena::Create<ContainerBackedListImpl>(p.arena, std::move(items));
    return CelValue::CreateList(list);
  }
  if (c == '{') {
    ++p.pos;
    p.Skip();
    std::vector<std::pair<CelValue, CelValue>> entries;
    if (p.pos < p.len && p.src[p.pos] != '}') {
      while (true) {
        auto* k = google::protobuf::Arena::Create<std::string>(p.arena, ParseJsonString(p));
        p.Skip();
        if (p.pos >= p.len || p.src[p.pos] != ':') p.Fail("expected ':'");
        ++p.pos;
        CelValue v = ParseJsonValue(p);
        entries.emplace_back(CelValue::CreateString(k), v);
        p.Skip();
        if (p.pos < p.len && p.src[p.pos] == ',') { ++p.pos; p.Skip(); continue; }
        break;
      }
    }
    if (p.pos >= p.len || p.src[p.pos] != '}') p.Fail("expected '}'");
    ++p.pos;
    auto map_or = CreateContainerBackedMap(absl::MakeConstSpan(entries));
    if (!map_or.ok()) {
      fprintf(stderr, "celcpp_bench: map create: %s\n", std::string(map_or.status().message()).c_str());
      exit(2);
    }
    auto map_ptr = std::move(map_or).value().release();
    p.arena->Own(map_ptr);
    return CelValue::CreateMap(map_ptr);
  }
  if (c == '-' || (c >= '0' && c <= '9')) {
    bool is_float;
    double d = ParseJsonNumber(p, is_float);
    if (is_float) return CelValue::CreateDouble(d);
    return CelValue::CreateInt64(static_cast<int64_t>(d));
  }
  p.Fail("expected value");
}

}  // namespace

int main(int argc, char** argv) {
  const char* expr = nullptr;
  const char* input = nullptr;
  long iters = 1000000;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "-e") == 0 && i + 1 < argc) expr = argv[++i];
    else if (std::strcmp(argv[i], "-i") == 0 && i + 1 < argc) input = argv[++i];
    else if (std::strcmp(argv[i], "-n") == 0 && i + 1 < argc) iters = std::strtol(argv[++i], nullptr, 10);
    else { fprintf(stderr, "celcpp_bench: unknown arg '%s'\n", argv[i]); return 2; }
  }
  if (!expr) { fprintf(stderr, "celcpp_bench: -e <expr> required\n"); return 2; }

  // ---- parse + plan, once ----
  google::protobuf::Arena arena;
  auto parsed = Parse(expr);
  if (!parsed.ok()) {
    fprintf(stderr, "celcpp_bench: parse: %s\n", std::string(parsed.status().message()).c_str());
    return 1;
  }
  ParsedExpr parsed_expr = *std::move(parsed);

  InterpreterOptions options;
  options.constant_arena = &arena;
  options.constant_folding = true;          // fairness: arcel folds at AOT, give cel-cpp the equivalent
  auto builder = CreateCelExpressionBuilder(options);
  auto reg = RegisterBuiltinFunctions(builder->GetRegistry(), options);
  if (!reg.ok()) {
    fprintf(stderr, "celcpp_bench: register: %s\n", std::string(reg.message()).c_str());
    return 1;
  }
  auto cel_expr_or = builder->CreateExpression(&parsed_expr.expr(), &parsed_expr.source_info());
  if (!cel_expr_or.ok()) {
    fprintf(stderr, "celcpp_bench: plan: %s\n", std::string(cel_expr_or.status().message()).c_str());
    return 1;
  }
  auto cel_expr = std::move(cel_expr_or).value();

  // ---- bindings, once ----
  Activation activation;
  google::protobuf::Arena bind_arena;
  if (input && input[0]) {
    JsonParser jp{input, 0, std::strlen(input), &bind_arena};
    CelValue root = ParseJsonValue(jp);
    if (root.IsMap()) {
      const auto* map = root.MapOrDie();
      auto keys_or = map->ListKeys();
      if (!keys_or.ok()) {
        fprintf(stderr, "celcpp_bench: map keys: %s\n", std::string(keys_or.status().message()).c_str());
        return 1;
      }
      const auto* keys = keys_or.value();
      for (int i = 0; i < keys->size(); ++i) {
        CelValue k = (*keys)[i];
        if (!k.IsString()) continue;
        std::string key(k.StringOrDie().value());
        auto v_or = (*map)[k];
        if (!v_or.has_value()) continue;
        // Arena-allocate the value name so it outlives this scope.
        auto* name = google::protobuf::Arena::Create<std::string>(&bind_arena, key);
        activation.InsertValue(*name, *v_or);
      }
    } else {
      // Non-object input wrapped as `input` (matches arcel / celgo_ref convention).
      auto* name = google::protobuf::Arena::Create<std::string>(&bind_arena, "input");
      activation.InsertValue(*name, root);
    }
  }

  // ---- warm + measure ----
  for (int i = 0; i < 1000; ++i) {
    google::protobuf::Arena per_iter;
    (void)cel_expr->Evaluate(activation, &per_iter);
  }
  auto t0 = std::chrono::steady_clock::now();
  for (long i = 0; i < iters; ++i) {
    google::protobuf::Arena per_iter;
    (void)cel_expr->Evaluate(activation, &per_iter);
  }
  auto t1 = std::chrono::steady_clock::now();
  long long ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
  double per = static_cast<double>(ns) / static_cast<double>(iters);
  std::printf("%ld %lld %.3f\n", iters, ns, per);
  return 0;
}
