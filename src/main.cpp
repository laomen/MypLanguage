#include "mylang/CodeGen.h"
#include "mylang/DiagnosticEngine.h"
#include "mylang/Lexer.h"
#include "mylang/Parser.h"
#include "mylang/Sema.h"
#include "mylang/SourceLocation.h"

#include <iostream>
#include <string>
#include <vector>
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
                       const std::string& package_path,
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
        // Try multiple locations:
        // 1. <stdlib_path>/ModuleName.myp
        // 2. source_dir/../stdlib/ModuleName.myp
        // 3. source_dir/stdlib/ModuleName.myp
        // 4. <package_path>/Module/src/Module.myp or <package_path>/Module/Module.myp
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
        else if (!package_path.empty()) {
            // Package format: <package_path>/<module>/src/<module>.myp
            std::string pkg_src = package_path + "/" + module_name + "/src/" + module_name + ".myp";
            std::string pkg_root = package_path + "/" + module_name + "/" + module_name + ".myp";
            if (fileExists(pkg_src))
                path = pkg_src;
            else if (fileExists(pkg_root))
                path = pkg_root;
            else
                path = cwd_path;
        } else
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
                        sub_dir, stdlib_path, package_path, tu, diag, loaded))
            return false;
    }
    return true;
}

[[nodiscard]] static std::string doCompile(mylang::TranslationUnit& ast,
                                            const std::string& output_fn,
                                            int opt_level,
                                            bool emit_llvm,
                                            mylang::DiagnosticEngine& diag) {
    // === Phase 4: Semantic Analysis ===
    mylang::Sema sema(diag);
    sema.analyze(ast);

    if (diag.hasErrors()) {
        std::cout << "Semantic analysis failed (" << diag.errorCount() << " errors)\n";
        return "";
    }

    std::cout << "Sema OK\n";

    // === Phase 5: Code Generation ===
    mylang::CodeGen codegen(diag);
    codegen.setEmitLLVM(emit_llvm);
    std::string obj_path = codegen.generate(ast, output_fn, opt_level);

    if (obj_path.empty()) {
        std::cout << "Code generation failed\n";
        return "";
    }

    std::cout << "CodeGen OK: " << obj_path << "\n";

    // --emit-llvm: save IR text and skip object/link
    if (emit_llvm) {
        std::string ll_path = output_fn + ".ll";
        if (codegen.saveIR(ll_path)) {
            std::cout << "LLVM IR saved: " << ll_path << "\n";
        } else {
            std::cerr << "Error: could not write " << ll_path << "\n";
        }
        return "emit_llvm";
    }

    return obj_path;
}

[[nodiscard]] static std::string compileSingle(const std::string& filename,
                                  const std::string& stdlib_path = "stdlib",
                                  const std::string& package_path = "",
                                  int opt_level = 0,
                                  bool trace_enabled = false,
                                  bool emit_llvm = false) {
    mylang::SourceManager source_mgr;
    if (!source_mgr.loadFile(filename)) {
        std::cerr << "Error: cannot open file '" << filename << "'\n";
        return "";
    }

    mylang::DiagnosticEngine diag(source_mgr);

    // === Phase 2: Lexer ===
    mylang::Lexer lexer(source_mgr, diag);
    auto tokens = lexer.tokenize();

    if (diag.hasErrors()) {
        return "";
    }

    std::cout << "Lexer OK: " << tokens.size() << " tokens\n";

    // === Phase 3: Parser ===
    mylang::Parser parser(tokens, diag);
    auto ast = parser.parse();

    if (diag.hasErrors()) {
        return "";
    }

    std::cout << "Parser OK\n";

    // === Phase 3b: Load imported modules ===
    std::string source_dir = getDir(filename);
    std::unordered_set<std::string> loaded_modules;
    for (auto& imp : ast->imports) {
        if (!loadModule(imp.module_name, imp.file_path, imp.is_path,
                        source_dir, stdlib_path, package_path, *ast, diag, loaded_modules)) {
            if (diag.hasErrors()) return "";
        }
    }

    return doCompile(*ast, filename, opt_level, emit_llvm, diag);
}

[[nodiscard]] static bool linkObjects(const std::vector<std::string>& obj_files,
                                       const std::string& output_name,
                                       const std::string& stdlib_path,
                                       bool trace_enabled) {
    if (obj_files.empty()) return false;

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

    // Build object file list
    std::string obj_list;
    for (auto& o : obj_files) obj_list += " " + o;

    std::string trace_def = trace_enabled ? " -DTRACE_ENABLED" : "";
    std::string link_cmd;
    if (trace_enabled) {
        std::string rebuild_cmd = "gcc -I" + inc_path + " -DTRACE_ENABLED -c " + runtime_c
                                + " -o /tmp/myp_runtime_trace.o 2>&1";
        std::system(rebuild_cmd.c_str());
        link_cmd = "gcc -I" + inc_path + obj_list + " /tmp/myp_runtime_trace.o"
                 + " -o " + output_name + " -lpthread -lm 2>&1";
    } else {
        link_cmd = "gcc -I" + inc_path + obj_list + " " + runtime_c
                 + " -o " + output_name + " -lpthread -lm 2>&1";
    }
    int link_result = std::system(link_cmd.c_str());

    if (link_result != 0) {
        std::cerr << "Linking failed (exit: " << link_result << ")\n";
        return false;
    }

    std::cout << "Link OK: " << output_name << "\n";
    return true;
}

static const char* MYP_VERSION = "2.0.0";

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " [options] <file1.myp> [file2.myp ...]\n";
        std::cerr << "Options:\n";
        std::cerr << "  -o <file>       Set output filename\n";
        std::cerr << "  -O[0123]        Set optimization level (default: -O0)\n";
        std::cerr << "  --stdlib <path> Set stdlib directory\n";
        std::cerr << "  --trace         Enable runtime event tracing\n";
        std::cerr << "  --emit-llvm     Save LLVM IR to .ll file (skip linking)\n";
        std::cerr << "  --version       Show version number\n";
        std::cerr << "  --help, -h      Show this help message\n";
        return 1;
    }

    std::string stdlib_path = "stdlib";
    std::string package_path = "";
    std::string output_name_v;
    int opt_level = 0;
    bool trace_enabled = false;
    bool emit_llvm = false;
    std::vector<std::string> filenames;
    int i = 1;
    while (i < argc) {
        std::string arg = argv[i];
        if (arg == "--version") {
            std::cout << "MYP Compiler v" << MYP_VERSION << "\n";
            return 0;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [options] <file1.myp> [file2.myp ...]\n";
            std::cout << "Options:\n";
            std::cout << "  -o <file>       Set output filename\n";
            std::cout << "  -O[0123]        Set optimization level (default: -O0)\n";
            std::cout << "  --stdlib <path> Set stdlib directory\n";
            std::cout << "  --package-path <path> Set local package directory\n";
            std::cout << "  --trace         Enable runtime event tracing\n";
            std::cout << "  --emit-llvm     Save LLVM IR to .ll file (skip linking)\n";
            std::cout << "  --version       Show version number\n";
            std::cout << "  --help, -h      Show this help message\n";
            return 0;
        } else if (arg == "--stdlib" && i + 1 < argc) {
            stdlib_path = argv[++i];
        } else if (arg == "-o" && i + 1 < argc) {
            output_name_v = argv[++i];
        } else if (arg.size() >= 2 && arg[0] == '-' && arg[1] == 'O') {
            opt_level = arg.size() > 2 ? (arg[2] - '0') : 1;
            if (opt_level < 0 || opt_level > 3) opt_level = 0;
        } else if (arg == "--package-path") {
            if (i + 1 >= argc) { std::cerr << "Error: --package-path requires an argument\n"; return 1; }
            package_path = argv[++i];
        } else if (arg == "--trace") {
            trace_enabled = true;
        } else if (arg == "--emit-llvm") {
            emit_llvm = true;
        } else {
            filenames.push_back(arg);
        }
        i++;
    }

    if (filenames.empty()) {
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

    // Determine output name
    if (output_name_v.empty()) {
        if (filenames.size() == 1) {
            output_name_v = filenames[0].substr(0, filenames[0].find_last_of('.')) + ".out";
        } else {
            output_name_v = "a.out";
        }
    }

    // --emit-llvm only works with single file
    if (emit_llvm) {
        if (filenames.size() > 1) {
            std::cerr << "Warning: --emit-llvm only supported for single file\n";
        }
        auto obj = compileSingle(filenames[0], stdlib_path, package_path, opt_level, trace_enabled, true);
        return obj.empty() ? 1 : 0;
    }

    if (filenames.size() == 1) {
        // Single file: use simple compile + link
        auto obj = compileSingle(filenames[0], stdlib_path, package_path, opt_level, trace_enabled, false);
        if (obj.empty()) return 1;
        if (!linkObjects({obj}, output_name_v, stdlib_path, trace_enabled))
            return 1;
        return 0;
    }

    // ---- Multi-file: merge all ASTs, one Sema + CodeGen pass ----
    mylang::SourceManager source_mgr;
    if (!source_mgr.loadFile(filenames[0])) {
        std::cerr << "Error: cannot open file '" << filenames[0] << "'\n";
        return 1;
    }
    mylang::DiagnosticEngine diag(source_mgr);
    std::unordered_set<std::string> loaded_modules;

    // Parse each file into separate ASTs (no import loading yet)
    std::vector<std::unique_ptr<mylang::TranslationUnit>> units;
    std::vector<std::string> dirs;
    for (auto& fn : filenames) {
        std::cout << "--- Parsing: " << fn << " ---\n";
        mylang::SourceManager fmgr;
        if (!fmgr.loadFile(fn)) {
            std::cerr << "Error: cannot open file '" << fn << "'\n";
            return 1;
        }
        mylang::Lexer lx(fmgr, diag);
        auto toks = lx.tokenize();
        if (diag.hasErrors()) return 1;
        std::cout << "  Tokens: " << toks.size() << "\n";

        mylang::Parser pr(toks, diag);
        auto unit = pr.parse();
        if (diag.hasErrors()) return 1;
        dirs.push_back(getDir(fn));
        units.push_back(std::move(unit));
    }

    // Merge all parsed units into one
    auto merged = std::move(units[0]);
    for (size_t i = 1; i < units.size(); i++) {
        for (auto& c : units[i]->classes)   merged->classes.push_back(std::move(c));
        for (auto& i2 : units[i]->interfaces) merged->interfaces.push_back(std::move(i2));
        for (auto& m : units[i]->mappings)   merged->mappings.push_back(std::move(m));
        for (auto& f : units[i]->functions)  merged->functions.push_back(std::move(f));
    }

    // Pre-populate loaded_modules with command-line files so imports skip them
    for (auto& fn : filenames) loaded_modules.insert(fn);

    // Load imports into the merged AST (deduplication prevents double-loading)
    for (auto& imp : merged->imports) {
        // Try each source directory to resolve relative imports
        bool loaded = false;
        for (auto& d : dirs) {
            if (loadModule(imp.module_name, imp.file_path, imp.is_path,
                            d, stdlib_path, package_path, *merged, diag, loaded_modules)) {
                loaded = true;
                break;
            }
        }
        if (!loaded && diag.hasErrors()) return 1;
    }

    // Single sema + codegen pass on merged AST
    std::string obj_path = doCompile(*merged, filenames[0], opt_level, false, diag);
    if (obj_path.empty()) return 1;

    // Link
    if (!linkObjects({obj_path}, output_name_v, stdlib_path, trace_enabled))
        return 1;

    return 0;
}
