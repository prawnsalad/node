#ifndef V8_DEOPTIMIZER_DEOPT_CRASH_INL_H_
#define V8_DEOPTIMIZER_DEOPT_CRASH_INL_H_

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>

#include "src/base/logging.h"
#include "src/common/globals.h"
#include "src/deoptimizer/deoptimize-reason.h"
#include "src/objects/code.h"
#include "src/objects/deoptimization-data.h"
#include "src/objects/objects-inl.h"
#include "src/objects/script.h"
#include "src/objects/shared-function-info.h"
#include "src/objects/string-inl.h"

namespace v8::internal {

struct DeoptCrashContext {
  bool opted_in = false;
  bool directly_marked = false;
  bool enclosing_marked = false;
  bool script_marked = false;
  bool has_nested_functions = false;
  const char* function_name = "<unknown>";
  const char* script_name = "<unknown>";
  int position = -1;
  int line = -1;
  int column = -1;
  Address code_start = kNullAddress;
};

namespace deopt_crash {

constexpr char kDeoptCrashMarkerRaw[] = "use deopt-crash";
constexpr int kPrefixScanLimit = 2048;
constexpr int kFunctionScanLimit = 2048;

inline bool IsDirectiveWhitespace(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
         c == '\v';
}

inline void SkipDirectiveSpaceAndComments(const char*& p, const char* end) {
  while (p < end) {
    if (IsDirectiveWhitespace(*p)) {
      ++p;
      continue;
    }
    if ((end - p) >= 2 && p[0] == '/' && p[1] == '/') {
      p += 2;
      while (p < end && *p != '\n') ++p;
      continue;
    }
    if ((end - p) >= 2 && p[0] == '/' && p[1] == '*') {
      p += 2;
      while ((end - p) >= 2 && !(p[0] == '*' && p[1] == '/')) ++p;
      if ((end - p) >= 2) p += 2;
      continue;
    }
    break;
  }
}

inline bool HasDirectivePrologueMarker(const char* text, size_t length,
                                       bool allow_hashbang) {
  const char* p = text;
  const char* end = text + length;
  SkipDirectiveSpaceAndComments(p, end);
  if (allow_hashbang && (end - p) >= 2 && p[0] == '#' && p[1] == '!') {
    p += 2;
    while (p < end && *p != '\n') ++p;
    SkipDirectiveSpaceAndComments(p, end);
  }

  while (p < end && (*p == '"' || *p == '\'')) {
    char quote = *p++;
    const char* literal_start = p;
    bool has_escape = false;
    while (p < end && *p != quote) {
      if (*p == '\\') {
        has_escape = true;
        ++p;
        if (p < end) ++p;
        continue;
      }
      if (*p == '\n' || *p == '\r') return false;
      ++p;
    }
    if (p >= end) return false;

    size_t literal_len = static_cast<size_t>(p - literal_start);
    bool marker = !has_escape &&
                  literal_len == (sizeof(kDeoptCrashMarkerRaw) - 1) &&
                  std::memcmp(literal_start, kDeoptCrashMarkerRaw,
                              sizeof(kDeoptCrashMarkerRaw) - 1) == 0;
    ++p;
    SkipDirectiveSpaceAndComments(p, end);
    if (p < end && *p == ';') {
      ++p;
      SkipDirectiveSpaceAndComments(p, end);
    }
    if (marker) return true;
  }
  return false;
}

inline const char* DupCString(Tagged<String> s, int start = 0, int length = -1) {
  uint32_t offset = start > 0 ? static_cast<uint32_t>(start) : 0;
  if (offset > s->length()) offset = s->length();

  uint32_t max_length = s->length() - offset;
  uint32_t slice_length =
      length < 0 ? max_length
                 : std::min<uint32_t>(static_cast<uint32_t>(length), max_length);

  if (slice_length == 0) {
    char* out = new char[1];
    out[0] = '\0';
    return out;
  }

  std::unique_ptr<char[]> cstr(s->ToCString(offset, slice_length, nullptr));
  if (!cstr) return "<oom>";

  size_t len = std::strlen(cstr.get());
  char* out = new char[len + 1];
  std::memcpy(out, cstr.get(), len + 1);
  return out;
}

inline bool TryGetSharedFunctionInfo(Tagged<Code> code,
                                     Tagged<SharedFunctionInfo>* out) {
  DisallowGarbageCollection no_gc;

  if (!code->uses_deoptimization_data()) return false;

  Tagged<DeoptimizationData> data = code->deoptimization_data();
  *out = data->GetSharedFunctionInfo();
  return true;
}

inline bool ScriptHasOptInMarker(Tagged<Script> script) {
  DisallowGarbageCollection no_gc;

  Tagged<Object> source_obj = script->source();
  if (!IsString(source_obj)) return false;

  Tagged<String> source = Cast<String>(source_obj);
  uint32_t scan_len =
      std::min<uint32_t>(source->length(), static_cast<uint32_t>(kPrefixScanLimit));

  std::unique_ptr<char[]> prefix(source->ToCString(0, scan_len, nullptr));
  if (!prefix) return false;

  return HasDirectivePrologueMarker(prefix.get(), scan_len, true);
}

inline bool FunctionHasOptInMarker(Tagged<SharedFunctionInfo> shared,
                                   Tagged<Script> script) {
  DisallowGarbageCollection no_gc;

  Tagged<Object> source_obj = script->source();
  if (!IsString(source_obj)) return false;

  int start = shared->StartPosition();
  int end = shared->EndPosition();
  if (start < 0 || end <= start) return false;

  Tagged<String> source = Cast<String>(source_obj);
  uint32_t source_len = source->length();
  uint32_t start_u = static_cast<uint32_t>(start);
  if (start_u >= source_len) return false;

  uint32_t max_len = source_len - start_u;
  uint32_t func_len = static_cast<uint32_t>(end - start);
  uint32_t scan_len = std::min<uint32_t>(
      std::min<uint32_t>(func_len, max_len), static_cast<uint32_t>(kFunctionScanLimit));
  if (scan_len == 0) return false;

  std::unique_ptr<char[]> text(source->ToCString(start_u, scan_len, nullptr));
  if (!text) return false;

  const char* body = std::strchr(text.get(), '{');
  if (!body) return false;
  ++body;
  size_t body_len = std::strlen(body);
  return HasDirectivePrologueMarker(body, body_len, false);
}

inline bool EnclosingFunctionHasOptInMarker(Tagged<SharedFunctionInfo> shared,
                                            Tagged<Script> script) {
  DisallowGarbageCollection no_gc;

  int inner_start = shared->StartPosition();
  int inner_end = shared->EndPosition();
  if (inner_start < 0 || inner_end <= inner_start) return false;

  Tagged<WeakFixedArray> infos = script->infos();
  for (int i = 0; i < infos->length(); ++i) {
    Tagged<MaybeObject> raw = infos->get(i);
    Tagged<HeapObject> heap_object;
    if (!raw.GetHeapObject(&heap_object) || !IsSharedFunctionInfo(heap_object)) {
      continue;
    }

    Tagged<SharedFunctionInfo> candidate = Cast<SharedFunctionInfo>(heap_object);
    if (candidate == shared) continue;

    int outer_start = candidate->StartPosition();
    int outer_end = candidate->EndPosition();
    if (outer_start < 0 || outer_end <= outer_start) continue;

    if (outer_start <= inner_start && inner_end <= outer_end &&
        FunctionHasOptInMarker(candidate, script)) {
      return true;
    }
  }
  return false;
}

inline bool HasNestedFunction(Tagged<SharedFunctionInfo> shared,
                              Tagged<Script> script) {
  DisallowGarbageCollection no_gc;

  int outer_start = shared->StartPosition();
  int outer_end = shared->EndPosition();
  if (outer_start < 0 || outer_end <= outer_start) return false;

  Tagged<WeakFixedArray> infos = script->infos();
  for (int i = 0; i < infos->length(); ++i) {
    Tagged<MaybeObject> raw = infos->get(i);
    Tagged<HeapObject> heap_object;
    if (!raw.GetHeapObject(&heap_object) || !IsSharedFunctionInfo(heap_object)) {
      continue;
    }

    Tagged<SharedFunctionInfo> candidate = Cast<SharedFunctionInfo>(heap_object);
    if (candidate == shared) continue;

    int inner_start = candidate->StartPosition();
    int inner_end = candidate->EndPosition();
    if (inner_start < 0 || inner_end <= inner_start) continue;

    if (outer_start <= inner_start && inner_end <= outer_end) return true;
  }
  // Fallback: infos() can miss nested functions in some compilation paths.
  // Use a lightweight source scan inside this function body.
  Tagged<Object> source_obj = script->source();
  if (!IsString(source_obj)) return false;
  Tagged<String> source = Cast<String>(source_obj);

  uint32_t source_len = source->length();
  uint32_t start_u = static_cast<uint32_t>(outer_start);
  if (start_u >= source_len) return false;

  uint32_t max_len = source_len - start_u;
  uint32_t func_len = static_cast<uint32_t>(outer_end - outer_start);
  uint32_t scan_len = std::min<uint32_t>(func_len, max_len);
  scan_len = std::min<uint32_t>(scan_len, static_cast<uint32_t>(kFunctionScanLimit));
  if (scan_len == 0) return false;

  std::unique_ptr<char[]> text(source->ToCString(start_u, scan_len, nullptr));
  if (!text) return false;

  const char* body = std::strchr(text.get(), '{');
  if (!body) return false;
  ++body;

  return std::strstr(body, "function ") != nullptr ||
         std::strstr(body, "=>") != nullptr ||
         std::strstr(body, "class ") != nullptr;
}

inline Tagged<SharedFunctionInfo> ChooseDisplaySharedFunctionInfo(
    Tagged<Code> code, Tagged<SharedFunctionInfo> outer_shared) {
  DisallowGarbageCollection no_gc;

  if (!code->uses_deoptimization_data()) return outer_shared;
  Tagged<DeoptimizationData> data = code->deoptimization_data();
  int inlined_count = data->InlinedFunctionCount().value();
  if (inlined_count <= 0) return outer_shared;

  Tagged<Object> outer_script = outer_shared->script();
  int outer_start = outer_shared->StartPosition();
  int outer_end = outer_shared->EndPosition();
  int best_size = (outer_start >= 0 && outer_end > outer_start)
                      ? (outer_end - outer_start)
                      : std::numeric_limits<int>::max();
  Tagged<SharedFunctionInfo> best = outer_shared;

  for (int i = 0; i < inlined_count; ++i) {
    Tagged<SharedFunctionInfo> candidate = data->GetInlinedFunction(i);
    if (candidate->script() != outer_script) continue;

    int start = candidate->StartPosition();
    int end = candidate->EndPosition();
    if (start < 0 || end <= start) continue;

    int size = end - start;
    if (size < best_size) {
      best = candidate;
      best_size = size;
    }
  }

  return best;
}

inline const char* GetFunctionName(Tagged<SharedFunctionInfo> shared) {
  DisallowGarbageCollection no_gc;
  Tagged<Object> maybe_name = shared->Name();
  if (IsString(maybe_name)) {
    Tagged<String> name = Cast<String>(maybe_name);
    if (name->length() != 0) return DupCString(name);
  }

  Tagged<String> inferred = shared->inferred_name();
  if (inferred->length() != 0) return DupCString(inferred);

  return "<anonymous>";
}

inline const char* GetScriptName(Tagged<Script> script) {
  DisallowGarbageCollection no_gc;
  Tagged<Object> maybe_name = script->name();
  if (IsString(maybe_name)) return DupCString(Cast<String>(maybe_name));

  Tagged<Object> maybe_source = script->source();
  if (IsString(maybe_source)) return "<anonymous-script>";

  return "<unknown-script>";
}

inline void AppendSummaryChunk(char* out, size_t cap, size_t* len,
                               const char* format, const char* a,
                               const char* b, const char* cstr, int d,
                               int e) {
  if (*len + 1 >= cap) return;
  int written =
      std::snprintf(out + *len, cap - *len, format, a, b, cstr, d, e);
  if (written <= 0) return;
  size_t written_size = static_cast<size_t>(written);
  if (written_size >= cap - *len) {
    *len = cap - 1;
    out[cap - 1] = '\0';
    return;
  }
  *len += written_size;
}

inline void AppendFunctionSummary(char* out, size_t cap, size_t* len,
                                  const char* prefix,
                                  Tagged<SharedFunctionInfo> shared) {
  const char* function_name = GetFunctionName(shared);
  const char* script_name = "<unknown-script>";
  int line = -1;
  int column = -1;

  Tagged<Object> maybe_script = shared->script();
  if (IsScript(maybe_script)) {
    Tagged<Script> script = Cast<Script>(maybe_script);
    script_name = GetScriptName(script);
    int position = shared->StartPosition();
    if (position >= 0) {
      Script::PositionInfo info;
      if (script->GetPositionInfo(position, &info)) {
        line = info.line + 1;
        column = info.column + 1;
      }
    }
  }

  AppendSummaryChunk(out, cap, len, "%s%s@%s:%d:%d", prefix, function_name,
                     script_name, line, column);
}

inline const char* BuildDeoptFrameSummary(Tagged<Code> code) {
  DisallowGarbageCollection no_gc;
  if (!code->uses_deoptimization_data()) return nullptr;

  Tagged<DeoptimizationData> data = code->deoptimization_data();
  constexpr size_t kSummaryCap = 2048;
  char* out = new char[kSummaryCap];
  size_t len = 0;
  out[0] = '\0';

  Tagged<SharedFunctionInfo> outer = data->GetSharedFunctionInfo();
  AppendFunctionSummary(out, kSummaryCap, &len, "frames=deopt:", outer);

  int inlined_count = data->InlinedFunctionCount().value();
  for (int i = 0; i < inlined_count; ++i) {
    AppendFunctionSummary(out, kSummaryCap, &len, " | inline:",
                          data->GetInlinedFunction(i));
  }
  return out;
}

inline bool GetContext(Isolate* isolate, Tagged<Code> code,
                       DeoptCrashContext* out) {
  *out = DeoptCrashContext{};
  out->code_start = code->instruction_start();

  Tagged<SharedFunctionInfo> shared;
  if (!TryGetSharedFunctionInfo(code, &shared)) return false;

  Tagged<SharedFunctionInfo> display_shared =
      ChooseDisplaySharedFunctionInfo(code, shared);
  out->function_name = GetFunctionName(display_shared);
  out->position = display_shared->StartPosition();

  Tagged<Object> maybe_script = display_shared->script();
  if (!IsScript(maybe_script)) return true;

  Tagged<Script> script = Cast<Script>(maybe_script);
  out->script_name = GetScriptName(script);
  if (out->position >= 0) {
    Script::PositionInfo info;
    if (script->GetPositionInfo(out->position, &info)) {
      out->line = info.line + 1;
      out->column = info.column + 1;
    }
  }
  out->directly_marked = FunctionHasOptInMarker(display_shared, script);
  out->enclosing_marked =
      EnclosingFunctionHasOptInMarker(display_shared, script);
  out->script_marked = ScriptHasOptInMarker(script);
  out->has_nested_functions = HasNestedFunction(display_shared, script);
  out->opted_in =
      out->directly_marked || out->enclosing_marked || out->script_marked;
  return true;
}

inline bool ShouldCrashForCode(Isolate* isolate, Tagged<Code> code) {
  DeoptCrashContext ctx;
  if (!GetContext(isolate, code, &ctx)) return false;
  if (!ctx.opted_in) return false;

  // In ESM/CommonJS execution, V8 may deopt anonymous top-level code objects
  // (start position 0) before reaching user function code. Skip those so that
  // we crash on concrete user functions like `readX`.
  if (ctx.position == 0 && std::strcmp(ctx.function_name, "<anonymous>") == 0) {
    return false;
  }

  // If a directly marked function has nested functions, prefer crashing when a
  // nested function deoptimizes so the report points at the concrete inner
  // function instead of the outer driver function.
  if (ctx.directly_marked && !ctx.enclosing_marked && ctx.has_nested_functions) {
    return false;
  }
  return true;
}

inline bool ShouldCrashForEagerReason(Isolate* isolate, Tagged<Code> code,
                                      DeoptimizeReason reason) {
  // Ignore eager deopts that are commonly emitted during tier-up/OSR feedback
  // collection; they tend to fire on outer driver functions before the inner
  // function deopt we care about.
  switch (reason) {
    case DeoptimizeReason::kInsufficientTypeFeedbackForArrayLiteral:
    case DeoptimizeReason::kInsufficientTypeFeedbackForBinaryOperation:
    case DeoptimizeReason::kInsufficientTypeFeedbackForCall:
    case DeoptimizeReason::kInsufficientTypeFeedbackForCompareOperation:
    case DeoptimizeReason::kInsufficientTypeFeedbackForConstruct:
    case DeoptimizeReason::kInsufficientTypeFeedbackForForIn:
    case DeoptimizeReason::kInsufficientTypeFeedbackForGenericGlobalAccess:
    case DeoptimizeReason::kInsufficientTypeFeedbackForGenericKeyedAccess:
    case DeoptimizeReason::kInsufficientTypeFeedbackForGenericNamedAccess:
    case DeoptimizeReason::kInsufficientTypeFeedbackForInstanceOf:
    case DeoptimizeReason::kInsufficientTypeFeedbackForObjectLiteral:
    case DeoptimizeReason::kInsufficientTypeFeedbackForTypeOf:
    case DeoptimizeReason::kInsufficientTypeFeedbackForUnaryOperation:
      return false;
    default:
      break;
  }
  return ShouldCrashForCode(isolate, code);
}

[[noreturn]] inline void FatalOptedInDeopt(Isolate* isolate, Tagged<Code> code,
                                           const char* mode,
                                           const char* reason_text,
                                           const char* extra = nullptr) {
  DeoptCrashContext ctx;
  GetContext(isolate, code, &ctx);
  const char* frames = BuildDeoptFrameSummary(code);

  if (extra && extra[0] != '\0') {
    FATAL(
        "deopt-crash: mode=%s reason=%s function=%s location=%s:%d:%d position=%d "
        "code_start=%p%s%s\nError: deopt-crash (%s, %s)\n%s",
        mode, reason_text, ctx.function_name, ctx.script_name, ctx.line,
        ctx.column, ctx.position, reinterpret_cast<void*>(ctx.code_start),
        frames ? " " : "", frames ? frames : "", mode, reason_text, extra);
  }

  FATAL(
      "deopt-crash: mode=%s reason=%s function=%s location=%s:%d:%d position=%d "
      "code_start=%p%s%s",
      mode, reason_text, ctx.function_name, ctx.script_name, ctx.line,
      ctx.column, ctx.position, reinterpret_cast<void*>(ctx.code_start),
      frames ? " " : "", frames ? frames : "");
}

[[noreturn]] inline void FatalOptedInDeoptCompat(Isolate* isolate,
                                                 Tagged<Code> code,
                                                 const char* mode) {
  FatalOptedInDeopt(isolate, code, mode,
                    DeoptimizeReasonToString(LazyDeoptimizeReason::kMapDeprecated));
}

}  // namespace deopt_crash

inline bool ShouldCrashOnDeoptForCode(Isolate* isolate, Tagged<Code> code) {
  return deopt_crash::ShouldCrashForCode(isolate, code);
}

[[noreturn]] inline void FatalOptedInDeoptCompat(Isolate* isolate,
                                                 Tagged<Code> code,
                                                 const char* mode) {
  deopt_crash::FatalOptedInDeoptCompat(isolate, code, mode);
}

#if defined(V8_OBJECTS_DEPENDENT_CODE_H_) && \
    !defined(V8_DEOPTIMIZER_DEOPTIMIZER_H_)
#define FatalOptedInDeopt(isolate, code, mode, reason) \
  FatalOptedInDeoptCompat(isolate, code, mode)
#endif

}  // namespace v8::internal

#endif  // V8_DEOPTIMIZER_DEOPT_CRASH_INL_H_
