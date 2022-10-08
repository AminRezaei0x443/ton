#include "fift/Fift.h"
#include "fift/IntCtx.h"
#include "fift/Dictionary.h"
#include "fift/SourceLookup.h"
#include "td/utils/PathView.h"
#include "td/utils/JsonBuilder.h"
#include "fift/words.h"

#include "td/utils/logging.h"
#include "td/utils/misc.h"
#include "td/utils/Parser.h"
#include "td/utils/port/path.h"

#include "common.h"

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

extern "C" __declspec(dllexport) void* fift_init(char* lib_path) {  
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

extern "C" __declspec(dllexport) char* fift_eval(void* fift_pointer, char* code, char* current_dir, char* stack_data, int len) {
    fift::Fift* fift = (fift::Fift*) fift_pointer;
    std::stringstream ss(code);
    std::string c_dir(current_dir);
    fift::IntCtx ctx(ss, "<input>", c_dir, false);
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
    auto res = stack2json(td::make_ref<vm::Stack>(ctx.stack));
    auto result = res.move_as_ok();
    return strdup(result.c_str());
}