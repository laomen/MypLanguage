#include "mylang/CodeGen.h"
#include "mylang/DiagnosticEngine.h"
#include "mylang/Lexer.h"
#include "mylang/Parser.h"
#include "mylang/Sema.h"
#include "mylang/SourceLocation.h"

#include <iostream>
#include <string>
#include <unordered_set>

#include <sys/stat.h>

// Check if a file exists
static bool fileExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

// Get the directory part of a file path
static std::string getDir(const std::string& path) {
    auto pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return ".";
    return path.substr(0, pos);
}

// Load and merge a stdlib module or a user file into the given TranslationUnit.
// For stdlib modules: module_name = "Env" → loads "stdlib/Env.myp"
// For user files: file_path = "./other.myp" → resolved relative to source_dir
static bool loadModule(const std::string& module_name,
                       const std::string& file_path,
                       bool is_path,
                       const std::string& source_dir,
                       const std::string& stdlib_path,
                       mylang::TranslationUnit& tu,
                       mylang::DiagnosticEngine& diag,
                       std::unordered_set<std::string>& loaded) {
    // Determine the actual file path
    std::string path;
    if (is_path) {
        // Resolve relative to source file directory
        if (file_path.size() > 0 && file_path[0] == '/') {
            path = file_path; // absolute path
        } else {
            path = source_dir + "/" + file_path;
        }
    } else {
        // Try multiple locations for stdlib modules:
        // 1. <stdlib_path>/ModuleName.myp
        // 2. source_dir/../stdlib/ModuleName.myp
        // 3. source_dir/stdlib/ModuleName.myp
        std::string stdlib_name = module_name + ".myp";
        std::string cwd_path = stdlib_path + "/" + stdlib_name;
        std::string parent_path = source_dir + "/../stdlib/" + stdlib_name;
        std::string sibling_path = source_dir + "/stdlib/" + stdlib_name;

        if (fileExists(cwd_path))
            path = cwd_path;
        else if (fileExists(parent_path))
            path = parent_path;
        else if (fileExists(sibling_path))
            path = sibling_path;
        else
            path = cwd_path;
    }

    // Deduplicate: use canonical path as key
    std::string dedup_key = is_path ? path : module_name;
    if (loaded.count(dedup_key)) return true;
    loaded.insert(dedup_key);

    mylang::SourceManager src_mgr;
    if (!src_mgr.loadFile(path)) {
        if (!is_path) {
            // Silently skip if stdlib file not found
            return false;
        }
        std::cerr << "Error: cannot open import '" << path << "'\n";
        return false;
    }
    mylang::Lexer lex(src_mgr, diag);
    auto toks = lex.tokenize();
    if (diag.hasErrors()) return false;

    mylang::Parser par(toks, diag);
    auto sub_ast = par.parse();
    if (diag.hasErrors()) return false;

    // Merge declarations into the main TU
    for (auto& c : sub_ast->classes)
        tu.classes.push_back(std::move(c));
    for (auto& i : sub_ast->interfaces)
        tu.interfaces.push_back(std::move(i));
    for (auto& m : sub_ast->mappings)
        tu.mappings.push_back(std::move(m));
    for (auto& f : sub_ast->functions)
        tu.functions.push_back(std::move(f));
    // Recursively load sub-imports
    std::string sub_dir = is_path ? getDir(path) : source_dir;
    for (auto& imp : sub_ast->imports) {
        if (!loadModule(imp.module_name, imp.file_path, imp.is_path,
                        sub_dir, stdlib_path, tu, diag, loaded))
            return false;
    }
    return true;
}

[[nodiscard]] static int compile(const std::string& filename,
                                  const std::string& stdlib_path = "stdlib",
                                  const std::string& output_name_override = "",
                                  int opt_level = 0,
                                  bool trace_enabled = false) {
    mylang::SourceManager source_mgr;
    if (!source_mgr.loadFile(filename)) {
        std::cerr << "Error: cannot open file '" << filename << "'\n";
        return 1;
    }

    mylang::DiagnosticEngine diag(source_mgr);

    // === Phase 2: Lexer ===
    mylang::Lexer lexer(source_mgr, diag);
    auto tokens = lexer.tokenize();

    if (diag.hasErrors()) {
        return 1;
    }

    std::cout << "Lexer OK: " << tokens.size() << " tokens\n";

    // === Phase 3: Parser ===
    mylang::Parser parser(tokens, diag);
    auto ast = parser.parse();

    if (diag.hasErrors()) {
        return 1;
    }

    std::cout << "Parser OK\n";

    // === Phase 3b: Load imported modules ===
    std::string source_dir = getDir(filename);
    std::unordered_set<std::string> loaded_modules;
    for (auto& imp : ast->imports) {
        if (!loadModule(imp.module_name, imp.file_path, imp.is_path,
                        source_dir, stdlib_path, *ast, diag, loaded_modules)) {
            if (diag.hasErrors()) return 1;
        }
    }

    // === Phase 4: Semantic Analysis ===
    mylang::Sema sema(diag);
    sema.analyze(*ast);

    if (diag.hasErrors()) {
        std::cout << "Semantic analysis failed (" << diag.errorCount() << " errors)\n";
        return 1;
    }

    std::cout << "Sema OK\n";

    // === Phase 5: Code Generation ===
    mylang::CodeGen codegen(diag);
    std::string obj_path = codegen.generate(*ast, filename, opt_level);

    if (obj_path.empty()) {
        std::cout << "Code generation failed\n";
        return 1;
    }

    std::cout << "CodeGen OK: " << obj_path << "\n";

    // === Link with runtime ===
    std::string output_name = output_name_override.empty()
        ? filename.substr(0, filename.find_last_of('.')) + ".out"
        : output_name_override;
    std::string runtime_dir = stdlib_path.substr(0, stdlib_path.find_last_of('/'));
    if (runtime_dir.empty() || runtime_dir == stdlib_path) runtime_dir = ".";
    std::string runtime_c;
    if (fileExists("src/runtime/runtime.c"))
        runtime_c = "src/runtime/runtime.c";
    else if (fileExists(runtime_dir + "/src/runtime/runtime.c"))
        runtime_c = runtime_dir + "/src/runtime/runtime.c";
    else
        runtime_c = "src/runtime/runtime.c";

    std::string inc_path = ".";
    if (fileExists("include/mylang/runtime.h")) inc_path = "include";
    else if (fileExists(runtime_dir + "/include/mylang/runtime.h")) inc_path = runtime_dir + "/include";

    std::string trace_def = trace_enabled ? " -DTRACE_ENABLED" : "";
    std::string link_cmd;
    if (trace_enabled) {
        // Rebuild runtime.c with TRACE_ENABLED
        std::string rebuild_cmd = "gcc -I" + inc_path + " -DTRACE_ENABLED -c " + runtime_c
                                + " -o /tmp/myp_runtime_trace.o 2>&1";
        std::system(rebuild_cmd.c_str());
        link_cmd = "gcc -I" + inc_path + " " + obj_path + " /tmp/myp_runtime_trace.o"
                 + " -o " + output_name + " -lpthread -lm 2>&1";
    } else {
        link_cmd = "gcc -I" + inc_path + " " + obj_path + " " + runtime_c
                 + " -o " + output_name + " -lpthread -lm 2>&1";
    }
    int link_result = std::system(link_cmd.c_str());

    if (link_result != 0) {
        std::cerr << "Linking failed (exit: " << link_result << ")\n";
        return 1;
    }

    std::cout << "Link OK: " << output_name << "\n";
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " [options] <file.myp>\n";
        std::cerr << "Options:\n";
        std::cerr << "  -o <file>       Set output filename\n";
        std::cerr << "  -O[0123]        Set optimization level (default: -O0)\n";
        std::cerr << "  --stdlib <path> Set stdlib directory\n";
        std::cerr << "  --trace         Enable runtime event tracing\n";
        return 1;
    }

    std::string stdlib_path = "stdlib";
    std::string output_name_v;
    int opt_level = 0;
    bool trace_enabled = false;
    std::string filename;
    int i = 1;
    while (i < argc) {
        std::string arg = argv[i];
        if (arg == "--stdlib" && i + 1 < argc) {
            stdlib_path = argv[++i];
        } else if (arg == "-o" && i + 1 < argc) {
            output_name_v = argv[++i];
        } else if (arg.size() >= 2 && arg[0] == '-' && arg[1] == 'O') {
            opt_level = arg.size() > 2 ? (arg[2] - '0') : 1;
            if (opt_level < 0 || opt_level > 3) opt_level = 0;
        } else if (arg == "--trace") {
            trace_enabled = true;
        } else {
            filename = arg;
        }
        i++;
    }

    if (filename.empty()) {
        std::cerr << "Error: no input file specified\n";
        return 1;
    }

    // Auto-detect stdlib relative to executable
    if (stdlib_path == "stdlib" && argv[0][0] == '/') {
        std::string exe_dir = getDir(argv[0]);
        std::string exe_stdlib = exe_dir + "/../stdlib";
        if (fileExists(exe_stdlib + "/env.myp"))
            stdlib_path = exe_stdlib;
    }

    if (trace_enabled) {
        std::cout << "Event tracing enabled (--trace)\n";
    }

    return compile(filename, stdlib_path, output_name_v, opt_level, trace_enabled);
}
