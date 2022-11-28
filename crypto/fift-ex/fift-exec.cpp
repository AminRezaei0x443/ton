#include "fift/Fift.h"
#include "fift/IntCtx.h"
#include "fift/Dictionary.h"
#include "fift/SourceLookup.h"
#include "td/utils/PathView.h"
#include "td/utils/JsonBuilder.h"
#include "fift/words.h"
#include "vm/cp0.h"

#include "td/utils/logging.h"
#include "td/utils/misc.h"
#include "td/utils/Parser.h"
#include "td/utils/port/path.h"

#include "common.h"
#include "StringLog.h"

#include "func/func.h"
#include "git.h"
#include "td/utils/JsonBuilder.h"
#include "fift/utils.h"
#include "td/utils/base64.h"
#include <sstream>
#include <iomanip>


#if _WIN32
#define DLL_EXPORT __declspec(dllexport)
#else
#define DLL_EXPORT 
#endif


auto memLog = new StringLog();

void parse_include_path_set(std::string include_path_set, std::vector<std::string>& res) {
  td::Parser parser(include_path_set);
  while (!parser.empty()) {
    auto path = parser.read_till_nofail(':');
    if (!path.empty()) {
      res.push_back(path.str());
    }
    parser.skip_nofail(':');
  }
}

extern "C" DLL_EXPORT void* fift_init(char* lib_path) {
    td::log_interface = memLog;
    SET_VERBOSITY_LEVEL(verbosity_DEBUG);
    memLog->clear();

    std::vector<std::string> source_include_path;
    std::string fift_libs(lib_path);
    
    bool interactive = false;
    bool fift_preload = false, no_env = false;
    bool script_mode = false;

    if(fift_libs != "<none>"){
      parse_include_path_set(lib_path, source_include_path);
      fift_preload = true;
    }

    fift::Fift::Config config;

    config.source_lookup = fift::SourceLookup(std::make_unique<fift::OsFileLoader>());
    for (auto& path : source_include_path) {
        config.source_lookup.add_include_path(path);
    }

    fift::init_words_common(config.dictionary);
    fift::init_words_vm(config.dictionary, true);  // enable vm debug
    fift::init_words_ton(config.dictionary);

    fift::Fift* fift = new fift::Fift(std::move(config));

    if (fift_preload) {
        auto status = fift->interpret_file("Fift.fif", "");
        if (status.is_error()) {
        LOG(ERROR) << "Error interpreting standard preamble file `Fift.fif`: " << status.error().message()
                    << "\nCheck that correct include path is set by -I or by FIFTPATH environment variable, or disable "
                        "standard preamble by -n.\n";
        std::exit(2);
        }
    }

    return fift;
}

extern "C" DLL_EXPORT char* fift_eval(void* fift_pointer, char* code, char* current_dir, char* stack_data, int len) {
    fift::Fift* fift = (fift::Fift*) fift_pointer;
    std::stringstream ss(code);
    std::string c_dir(current_dir);
    fift::IntCtx ctx(ss, "<input>", c_dir, true);
    std::string v(stack_data, len);
    auto stack_j_r = td::json_decode(v);
    auto stack_j = stack_j_r.move_as_ok();
    auto &obj = stack_j.get_object();
    auto stack_j2 = td::get_json_object_field(obj, "data", td::JsonValue::Type::Array, false).move_as_ok();
    auto& stack_a = stack_j2.get_array();
    for (auto &x : stack_a) {
      auto &obj = x.get_object();
      auto entry_r = json_to_stack_entry(obj);
      auto entry = entry_r.move_as_ok();
      ctx.stack.push(entry);
    }
    auto i = fift->do_interpret(ctx, false);
    std::string res = R"({ "status":)";

    if(i.is_error()){
      res += R"("error")";
      res += R"(,"message": ")";
      std::ostringstream os;
      ctx.print_error_backtrace(os);
      LOG(ERROR) << os.str();
      res.append(i.error().message().str());
      res += R"(")";
      res += R"(})";
    }else{
      auto resV = stack2json(td::make_ref<vm::Stack>(ctx.stack));
      auto result = resV.move_as_ok();
      res += R"("ok", "stack":)";
      res.append(result);
      res += R"(})";
    }
    return strdup(res.c_str());
}

extern "C" DLL_EXPORT char* vm_exec(int len, char* _data) {
  // Init logging
  td::log_interface = memLog;
  SET_VERBOSITY_LEVEL(verbosity_DEBUG);
  memLog->clear();

  std::string config(_data, len);

  auto res = vm_exec_from_config(config, []() -> std::string { return memLog->get_string(); });

  if (res.is_error()) {
    std::string result;
    result += "{";
    result +=  R"("ok": false,)";
    result +=  R"("error": ")" + res.move_as_error().message().str() + R"(")";
    result += "}";

    return strdup(result.c_str());
  }

  return strdup(res.move_as_ok().c_str());
}

td::Result<std::string> compile_internal(char* config_json, int len) {
  std::string v(config_json, len);
  TRY_RESULT(input_json, td::json_decode(v))
  auto &obj = input_json.get_object();

  TRY_RESULT(opt_level, td::get_json_object_int_field(obj, "optLevel", false));
  TRY_RESULT(sources_obj, td::get_json_object_field(obj, "sources", td::JsonValue::Type::Array, false));

  auto &sources_arr = sources_obj.get_array();

  std::vector<std::string> sources;

  for (auto &item : sources_arr) {
    sources.push_back(item.get_string().str());
  }

  funC::opt_level = std::max(0, opt_level);
  funC::program_envelope = true;
  funC::verbosity = 0;
  funC::indent = 1;

  std::ostringstream outs, errs;
  auto compile_res = funC::func_proceed(sources, outs, errs);

  if (compile_res != 0) {
    return td::Status::Error(std::string("Func compilation error: ") + errs.str());
  }

  td::JsonBuilder result_json;
  auto result_obj = result_json.enter_object();
  result_obj("status", "ok");
  result_obj("fiftCode", escape_json(outs.str()));
  result_obj.leave();

  outs.clear();
  errs.clear();

  return result_json.string_builder().as_cslice().str();
}

extern "C" {

DLL_EXPORT const char* ton_version() {
  auto version_json = td::JsonBuilder();
  auto obj = version_json.enter_object();
  obj("funcVersion", funC::func_version);
  obj("funcFiftLibCommitHash", GitMetadata::CommitSHA1());
  obj("funcFiftLibCommitDate", GitMetadata::CommitDate());
  obj.leave();
  return strdup(version_json.string_builder().as_cslice().c_str());
}

DLL_EXPORT const char *func_compile(char *config_json, int len) {
  auto res = compile_internal(config_json, len);

  if (res.is_error()) {
    auto result = res.move_as_error();
    auto error_res = td::JsonBuilder();
    auto error_o = error_res.enter_object();
    error_o("status", "error");
    error_o("message", result.message().str());
    error_o.leave();
    return strdup(error_res.string_builder().as_cslice().c_str());
  }

  auto res_string = res.move_as_ok();

  return strdup(res_string.c_str());
}
}
