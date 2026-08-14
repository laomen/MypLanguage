// MYP Language compiler — entry point.
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 MYP Language authors
// See LICENSE for the full MIT license text.
#include "mylang/CodeGen.h"
#include "mylang/DiagnosticEngine.h"
#include "mylang/Eval.h"
#include "mylang/Fmt.h"
#include "mylang/FrontendDump.h"
#include "mylang/Macro.h"
#include "mylang/Lexer.h"
#include "mylang/Parser.h"
#include "mylang/Sema.h"
#include "mylang/SourceLocation.h"

#include <llvm/Support/ErrorHandling.h>

#include <chrono>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <unordered_set>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// Check if a file exists
static bool fileExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

// ---- Phase timing (MYP_TIMING=1 prints per-phase durations to stderr) ----
static bool timingEnabled() {
    static bool on = [] {
        const char* e = getenv("MYP_TIMING");
        return e && e[0] == '1';
    }();
    return on;
}
static long long timingNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
static long long g_timing_last = 0;
static void phaseMark(const char* name) {
    if (!timingEnabled()) return;
    long long t = timingNowMs();
    if (g_timing_last == 0) g_timing_last = t;
    fprintf(stderr, "[timing] %-16s %5lld ms\n", name, t - g_timing_last);
    g_timing_last = t;
}

// Normalize path: remove "./" prefixes and "//" sequences
static std::string normalizePath(const std::string& path) {
    std::string result;
    // Remove "./" at the start
    if (path.size() >= 2 && path[0] == '.' && path[1] == '/')
        result = path.substr(2);
    else
        result = path;
    // Remove "/./" sequences
    for (;;) {
        auto p = result.find("/./");
        if (p == std::string::npos) break;
        result = result.substr(0, p) + result.substr(p + 2);
    }
    // Remove "//" sequences
    for (;;) {
        auto p = result.find("//");
        if (p == std::string::npos) break;
        result = result.substr(0, p) + result.substr(p + 1);
    }
    // Remove trailing "."
    if (result.size() == 1 && result[0] == '.')
        result.clear();
    return result;
}

// Get the directory part of a file path
static std::string getDir(const std::string& path) {
    auto pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return ".";
    return path.substr(0, pos);
}

// 点分模块名 → 目录层级路径：gpu.backend → gpu/backend
static std::string dotToSlash(const std::string& name) {
    std::string s = name;
    for (auto& c : s) if (c == '.') c = '/';
    return s;
}

// Absolute directory of the running executable — robust to a RELATIVE argv[0]
// (e.g. `../../build/mypc run x.myp`), which previously made stdlib lookup fail.
// Uses /proc/self/exe (Linux); falls back to argv[0] on failure (non-Linux).
static std::string selfExeDir(const char* argv0) {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        return getDir(std::string(buf));
    }
    return getDir(argv0);
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
        // 点分模块名（gpu.backend）优先映射为子目录（stdlib/gpu/backend.myp），
        // 扁平名（env / gpu）保持原样；再按多个位置尝试：
        //   1. <stdlib_path>/<rel>
        //   2. source_dir/../stdlib/<rel>
        //   3. source_dir/stdlib/<rel>
        //   4. <package_path>/<module>/src/<module>.myp 或 <package_path>/<module>/<module>.myp
        std::string slash_name = dotToSlash(module_name);
        std::vector<std::string> rel_candidates;
        rel_candidates.push_back(slash_name + ".myp");          // gpu.backend → gpu/backend.myp
        if (slash_name != module_name)
            rel_candidates.push_back(module_name + ".myp");     // 扁平兜底（旧行为）

        path.clear();
        for (auto& rel : rel_candidates) {
            std::string cwd_path = stdlib_path + "/" + rel;
            std::string parent_path = source_dir + "/../stdlib/" + rel;
            std::string sibling_path = source_dir + "/stdlib/" + rel;
            if (fileExists(cwd_path))      { path = cwd_path; break; }
            if (fileExists(parent_path))   { path = parent_path; break; }
            if (fileExists(sibling_path))  { path = sibling_path; break; }
            if (!package_path.empty()) {
                std::string pkg_src = package_path + "/" + slash_name + "/src/" + module_name + ".myp";
                std::string pkg_root = package_path + "/" + slash_name + "/" + module_name + ".myp";
                if (fileExists(pkg_src))      { path = pkg_src; break; }
                if (fileExists(pkg_root))     { path = pkg_root; break; }
            }
        }
        if (path.empty())
            path = stdlib_path + "/" + rel_candidates[0];
    }

    // Normalize path for consistent dedup keys
    if (is_path) path = normalizePath(path);

    // Deduplicate: use canonical path as key
    std::string dedup_key = is_path ? path : module_name;
    if (loaded.count(dedup_key)) return true;
    loaded.insert(dedup_key);

    mylang::SourceManager src_mgr;
    if (!src_mgr.loadFile(path)) {
        if (!is_path) {
            // Report error for stdlib module not found
            diag.error(mylang::SourceRange(), "cannot find import '" + module_name + "'");
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
    for (auto& f : sub_ast->ffis)
        tu.ffis.push_back(std::move(f));
    for (auto& e : sub_ast->enums)
        tu.enums.push_back(std::move(e));
    for (auto& s : sub_ast->structs)
        tu.structs.push_back(std::move(s));
    // Recursively load sub-imports.
    // 相对路径 import（is_path）一律以"被导入文件所在目录"为基准解析。
    // 这使得 stdlib 子目录模块（如 stdlib/gpu/backend.myp）能被
    // `import "./gpu/backend.myp";` 正确加载，不受用户源码目录影响。
    // 注：旧实现是 is_path ? getDir(path) : source_dir，导致"名字导入的 stdlib
    // 模块内部"再出现相对路径 import 时，被错误地解析到用户源码目录。
    std::string sub_dir = getDir(path);
    for (auto& imp : sub_ast->imports) {
        if (!loadModule(imp.module_name, imp.file_path, imp.is_path,
                        sub_dir, stdlib_path, package_path, tu, diag, loaded))
            return false;
    }
    return true;
}

// --frontend-dump <tokens|ast|sema> — deterministic front-end dump used as the
// acceptance oracle for the MYP self-hosted compiler (tools/selfhost, F0).
// Prints the frozen format (see include/mylang/FrontendDump.h) to stdout and
// returns 0 on success, 1 if diagnostics were produced.
static int runFrontendDump(const std::string& mode, const std::string& filename,
                           const std::string& stdlib_path,
                           const std::string& package_path) {
    mylang::SourceManager source_mgr;
    if (!source_mgr.loadFile(filename)) {
        std::cerr << "Error: cannot open file '" << filename << "'\n";
        return 1;
    }
    mylang::DiagnosticEngine diag(source_mgr);

    std::cout << "MYP_FRONTEND_DUMP v1\n";
    std::cout << "(Mode " << mode << ")\n";

    mylang::Lexer lexer(source_mgr, diag);
    auto tokens = lexer.tokenize();

    if (mode == "tokens") {
        mylang::dumpTokens(std::cout, tokens);
    } else if (mode == "ast" || mode == "sema") {
        mylang::Parser parser(tokens, diag);
        auto ast = parser.parse();
        if (!diag.hasErrors()) {
            std::string source_dir = getDir(filename);
            std::unordered_set<std::string> loaded;
            for (auto& imp : ast->imports) {
                if (!loadModule(imp.module_name, imp.file_path, imp.is_path,
                                source_dir, stdlib_path, package_path, *ast, diag, loaded))
                    break;
            }
        }
        if (mode == "ast") {
            mylang::dumpAST(std::cout, *ast, false);
        } else {
            mylang::Sema sema(diag);
            sema.analyze(*ast);
            mylang::dumpSema(std::cout, *ast);
        }
    } else {
        std::cerr << "Error: unknown --frontend-dump mode '" << mode << "'\n";
        return 1;
    }

    // Diagnostics (sorted by line, col, message for determinism) + result.
    auto diags = diag.getDiagnostics();
    std::sort(diags.begin(), diags.end(),
              [](const mylang::Diagnostic& a, const mylang::Diagnostic& b) {
                  if (a.range.begin.line != b.range.begin.line)
                      return a.range.begin.line < b.range.begin.line;
                  if (a.range.begin.column != b.range.begin.column)
                      return a.range.begin.column < b.range.begin.column;
                  return a.message < b.message;
              });
    std::cout << "(Diagnostics)\n";
    for (auto& d : diags) {
        const char* sev = d.severity == 1 ? "error"
                        : d.severity == 2 ? "warning" : "info";
        std::cout << "  (Diag line=" << d.range.begin.line
                  << " col=" << d.range.begin.column
                  << " sev=" << sev
                  << " msg=\"" << mylang::escapeDumpString(d.message) << "\")\n";
    }
    std::cout << "(Result ok=" << (diag.hasErrors() ? 0 : 1)
              << " errors=" << diag.errorCount() << ")\n";
    return diag.hasErrors() ? 1 : 0;
}

[[nodiscard]] static std::string doCompile(mylang::TranslationUnit& ast,
                                            const std::string& output_fn,
                                            int opt_level,
                                            bool emit_llvm,
                                            bool library_mode,
                                            bool test_mode,
                                            bool debug,
                                            const std::string& passes,
                                            bool macro_expand,
                                            mylang::DiagnosticEngine& diag,
                                            bool auto_main = false) {
    // === Phase 3b: Macro expansion (declarative `macro` + @macro proc-macro) ===
    bool has_any_macro = !ast.macros.empty();
    if (!has_any_macro) {
        for (auto& f : ast.functions)
            if (f.has_proc_macro) { has_any_macro = true; break; }
    }
    if (has_any_macro) {
        mylang::expandMacros(ast, diag);
        phaseMark("macro");
        if (diag.hasErrors()) {
            std::cout << "Macro expansion failed (" << diag.errorCount() << " errors)\n";
            return "";
        }
        std::cout << "Macro expand OK\n";
        if (macro_expand) mylang::dumpMacroExpandedAST(ast);
    }

    // === Phase 4: Semantic Analysis ===
    mylang::Sema sema(diag);
    sema.setAutoMain(auto_main);   // `mypc run`: 单类文件自动 main
    sema.analyze(ast);
    phaseMark("sema");

    if (diag.hasErrors()) {
        std::cout << "Semantic analysis failed (" << diag.errorCount() << " errors)\n";
        return "";
    }

    std::cout << "Sema OK\n";

    // === Phase 4b: Compile-time evaluation (@eval / const folding) ===
    mylang::evaluateCompileTimeConstants(ast, diag);
    phaseMark("eval");
    if (diag.hasErrors()) {
        std::cout << "Compile-time evaluation failed (" << diag.errorCount() << " errors)\n";
        return "";
    }

    // === Phase 5: Code Generation ===
    mylang::CodeGen codegen(diag);
    codegen.setEmitLLVM(emit_llvm);
    codegen.setLibraryMode(library_mode);
    codegen.setTestMode(test_mode);
    codegen.setDebugMode(debug);
    codegen.setMypPasses(passes);
    std::string obj_path = codegen.generate(ast, output_fn, opt_level);
    phaseMark("codegen");

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
                                  bool emit_llvm = false,
                                  bool library_mode = false,
                                  bool test_mode = false,
                                  bool debug = false,
                                  const std::string& passes = "",
                                  bool macro_expand = false,
                                  bool auto_main = false) {
    mylang::SourceManager source_mgr;
    if (!source_mgr.loadFile(filename)) {
        std::cerr << "Error: cannot open file '" << filename << "'\n";
        return "";
    }
    phaseMark("load");

    mylang::DiagnosticEngine diag(source_mgr);

    // === Phase 2: Lexer ===
    mylang::Lexer lexer(source_mgr, diag);
    auto tokens = lexer.tokenize();
    phaseMark("lexer");

    if (diag.hasErrors()) {
        return "";
    }

    std::cout << "Lexer OK: " << tokens.size() << " tokens\n";

    // === Phase 3: Parser ===
    mylang::Parser parser(tokens, diag);
    auto ast = parser.parse();
    phaseMark("parser");

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
    phaseMark("imports");

    return doCompile(*ast, filename, opt_level, emit_llvm, library_mode, test_mode, debug, passes, macro_expand, diag, auto_main);
}

[[nodiscard]] static bool linkObjects(const std::vector<std::string>& obj_files,
                                       const std::string& output_name,
                                       const std::string& stdlib_path,
                                       bool trace_enabled,
                                       bool shared_lib = false,
                                       bool static_lib = false) {
    if (obj_files.empty()) return false;

    // When MYP_SANITIZE=1, also instrument the generated program so the test
    // suite can run entirely under ASan/UBSan (catches runtime memory bugs).
    // When MYP_SANITIZE_TSAN=1, use ThreadSanitizer instead (codegen adds the
    // TSan pass; here runtime + link get -fsanitize=thread).
    std::string san_flags;
    if (const char* env = getenv("MYP_SANITIZE"); env && env[0] == '1')
        san_flags = " -fsanitize=address,undefined -fno-omit-frame-pointer ";
    else if (const char* env = getenv("MYP_SANITIZE_TSAN"); env && env[0] == '1')
        san_flags = " -fsanitize=thread -fno-omit-frame-pointer ";

    // -ffunction-sections/-fdata-sections on every runtime C object plus
    // -Wl,--gc-sections at link time: only the runtime functions the program
    // actually references get linked in. Without this the WHOLE 6000-line
    // runtime.c is pulled into every binary (flat ~162KB floor). With it a
    // console-only bench (e.g. fannkuch) drops to ~35KB, and per-program bss
    // (GC pools etc.) shrinks from ~78KB to ~8KB.
    const std::string gc_compile = " -ffunction-sections -fdata-sections ";
    const std::string gc_link = " -Wl,--gc-sections";

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

    // ---- Cache runtime C objects: runtime.c / sdl_bridge.c / runtime_gpu.c
    // are compiled fresh via gcc on EVERY link (~90ms each). Hash the source +
    // flags and reuse a cached .o when unchanged (biggest single compile win).
    auto cacheObj = [&](const std::string& src_path,
                        const std::string& extra_flags) -> std::string {
        std::ifstream ifs(src_path, std::ios::binary);
        if (!ifs) return "";  // fall back to no-cache (caller compiles directly)
        uint64_t h = 1469598103934665603ULL;  // FNV-1a 64
        char buf[4096];
        while (ifs) {
            ifs.read(buf, sizeof(buf));
            std::streamsize n = ifs.gcount();
            for (std::streamsize i = 0; i < n; ++i) {
                h ^= (unsigned char)buf[i];
                h *= 1099511628211ULL;
            }
        }
        // Fold flags into the hash (different sanitizer/trace => different object)
        for (char c : extra_flags) { h ^= (unsigned char)c; h *= 1099511628211ULL; }
        for (char c : san_flags)    { h ^= (unsigned char)c; h *= 1099511628211ULL; }
        char hex[17];
        snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)h);
        std::string dir = "/tmp/myp_rt_cache";
        ::mkdir(dir.c_str(), 0755);
        return dir + "/myp_rt_" + hex + ".o";
    };

    // Unique per-process temp path (parallel-safe). The old unseeded
    // rand()-only names collided when multiple mypc ran concurrently
    // (make -j8 / xargs -P): every process got the same temp path, so
    // parallel gcc writes clobbered each other. PID + rand + kind is
    // unique across concurrent compilers.
    auto tmpObj = [](const std::string& kind) {
        return "/tmp/myp_" + kind + "_" + std::to_string(::getpid())
             + "_" + std::to_string(std::rand()) + ".o";
    };

    // Build runtime object. IMPORTANT: the C runtime is compiled fresh for
    // EVERY generated program. It MUST be optimized — gcc's default is -O0,
    // which leaves every runtime function unoptimized (no inlining, all
    // locals spilled to the stack), dominating hot loops (channel sync
    // handoff, coroutine yield/resume, string/ARC ops). -O2 is folded into
    // the cache hash so old -O0 cached objects are never reused.
    std::string rt_obj;
    const char* rt_opt = "-O2";
    std::string trace_def = trace_enabled ? " -DTRACE_ENABLED" : "";
    // gc_compile is part of the cache hash: an older runtime.o built WITHOUT
    // -ffunction-sections cannot be trimmed by --gc-sections, so it must never
    // be reused from cache.
    std::string rt_flags = std::string(rt_opt) + trace_def + gc_compile;
    if (fileExists(runtime_c)) {
        std::string cached = cacheObj(runtime_c, rt_flags);
        if (!cached.empty() && fileExists(cached)) {
            rt_obj = cached;  // cache hit — skip gcc
        } else {
            rt_obj = tmpObj("runtime");
            std::string compile_rt = "gcc -I" + inc_path + " -fPIC " + rt_flags + san_flags + gc_compile + " -c " + runtime_c + " -o " + rt_obj + " 2>&1";
            if (std::system(compile_rt.c_str()) != 0) {
                std::cerr << "Failed to compile runtime\n";
                return false;
            }
            // Atomically install into the shared cache (parallel-safe):
            // rename() is atomic on the same filesystem, so concurrent mypc
            // processes never observe a partially-written cache object.
            if (!cached.empty()) {
                if (::rename(rt_obj.c_str(), cached.c_str()) == 0)
                    rt_obj = cached;  // link from the installed cache path
            }
        }
    } else {
        rt_obj = tmpObj("runtime");
        std::string compile_rt = "gcc -I" + inc_path + " -fPIC" + trace_def + san_flags + gc_compile + " -c " + runtime_c + " -o " + rt_obj + " 2>&1";
        if (std::system(compile_rt.c_str()) != 0) {
            std::cerr << "Failed to compile runtime\n";
            return false;
        }
    }
    phaseMark("rt_compile");

    // Build coroutine context-switch assembly object (x86-64 fast path).
    // Always compiled fresh (tiny, ~10ms); provides myp_ctx_switch referenced
    // by runtime.c's coroutine yield/resume. On non-x86-64 mypc this stays
    // empty and runtime.c uses its ucontext fallback (no myp_ctx_switch ref).
    std::string ctx_obj;
#if defined(__x86_64__)
    std::string ctx_asm;
    if (fileExists("src/runtime/coro_ctx.S"))
        ctx_asm = "src/runtime/coro_ctx.S";
    else if (fileExists(runtime_dir + "/src/runtime/coro_ctx.S"))
        ctx_asm = runtime_dir + "/src/runtime/coro_ctx.S";
    if (!ctx_asm.empty()) {
        ctx_obj = tmpObj("rt_ctx");
        std::string compile_ctx = "gcc -c " + ctx_asm + " -o " + ctx_obj + " 2>&1";
        if (std::system(compile_ctx.c_str()) != 0) {
            std::cerr << "Failed to compile coro_ctx.S\n";
            return false;
        }
    }
#endif

    // Build SDL bridge object — ONLY when the program actually references
    // myp_sdl_* symbols (i.e. it imports stdlib/sdl.myp and calls into it).
    // sdl_bridge.o calls SDL2 functions, so linking it unconditionally forced
    // -lSDL2 into DT_NEEDED (plus transitive libpulse/libasound/libsamplerate)
    // on EVERY program, even console-only binaries like the bench suite. We
    // detect the need by scanning undefined symbols in the object files.
    std::string sdl_obj;
    std::string sdl_libs;
    std::string sdl_c;
    bool need_sdl = false;
    {
        std::string nm_cmd = "nm -u";
        for (const auto& o : obj_files) nm_cmd += " " + o;
        nm_cmd += " 2>/dev/null | grep -q 'myp_sdl_'";
        need_sdl = (std::system(nm_cmd.c_str()) == 0);
    }
    if (need_sdl && fileExists("src/runtime/sdl_bridge.c"))
        sdl_c = "src/runtime/sdl_bridge.c";
    else if (need_sdl && fileExists(runtime_dir + "/src/runtime/sdl_bridge.c"))
        sdl_c = runtime_dir + "/src/runtime/sdl_bridge.c";
    if (!sdl_c.empty()) {
        sdl_obj = "";
        std::string sdl_cflags = "-O2 -I/usr/include/SDL2 -D_REENTRANT" + gc_compile;
        sdl_libs = "-lSDL2";
        std::string cached = cacheObj(sdl_c, sdl_cflags);
        if (!cached.empty() && fileExists(cached)) {
            sdl_obj = cached;
        } else {
            sdl_obj = tmpObj("sdl");
            std::string compile_sdl = "gcc -I" + inc_path + " -fPIC " + sdl_cflags + san_flags + gc_compile + " -c " + sdl_c + " -o " + sdl_obj + " 2>&1";
            if (std::system(compile_sdl.c_str()) != 0) {
                std::cerr << "Failed to compile SDL bridge\n";
                return false;
            }
            if (!cached.empty()) {
                if (::rename(sdl_obj.c_str(), cached.c_str()) == 0)
                    sdl_obj = cached;
            }
        }
    }

    // Build GPU runtime object（runtime_gpu.c 是 dlopen 胶水，任何 Linux 可编译；
    // 保留以提供 stdlib cuda/gpu 的设备信息符号。GPU offload 的剔除在编译器 NVPTX 层）
    std::string gpu_c;
    if (fileExists("src/runtime/runtime_gpu.c"))
        gpu_c = "src/runtime/runtime_gpu.c";
    else if (fileExists(runtime_dir + "/src/runtime/runtime_gpu.c"))
        gpu_c = runtime_dir + "/src/runtime/runtime_gpu.c";
    std::string gpu_obj;
    if (!gpu_c.empty()) {
        gpu_obj = "";
        std::string cached = cacheObj(gpu_c, "-O2" + gc_compile);
        if (!cached.empty() && fileExists(cached)) {
            gpu_obj = cached;
        } else {
            gpu_obj = tmpObj("gpu");
            std::string compile_gpu = "gcc -I" + inc_path + " -fPIC -O2" + san_flags + gc_compile + " -c " + gpu_c + " -o " + gpu_obj + " 2>&1";
            if (std::system(compile_gpu.c_str()) != 0) {
                std::cerr << "Failed to compile GPU runtime\n";
                return false;
            }
            if (!cached.empty()) {
                if (::rename(gpu_obj.c_str(), cached.c_str()) == 0)
                    gpu_obj = cached;
            }
        }
    }

    // Build vendor-lib hook object（runtime_lib.c：cuBLAS 等厂商库，dlopen 惰性加载）。
    // 与 runtime_gpu.c 同款缓存编译；无依赖，任何 Linux 可编译。
    std::string lib_c;
    if (fileExists("src/runtime/runtime_lib.c"))
        lib_c = "src/runtime/runtime_lib.c";
    else if (fileExists(runtime_dir + "/src/runtime/runtime_lib.c"))
        lib_c = runtime_dir + "/src/runtime/runtime_lib.c";
    std::string lib_obj;
    if (!lib_c.empty()) {
        lib_obj = "";
        std::string cached = cacheObj(lib_c, "-O2" + gc_compile);
        if (!cached.empty() && fileExists(cached)) {
            lib_obj = cached;
        } else {
            lib_obj = tmpObj("lib");
            std::string compile_lib = "gcc -I" + inc_path + " -fPIC -O2" + san_flags + gc_compile + " -c " + lib_c + " -o " + lib_obj + " 2>&1";
            if (std::system(compile_lib.c_str()) != 0) {
                std::cerr << "Failed to compile vendor-lib runtime\n";
                return false;
            }
            if (!cached.empty()) {
                if (::rename(lib_obj.c_str(), cached.c_str()) == 0)
                    lib_obj = cached;
            }
        }
    }

    std::string link_cmd;
    if (shared_lib) {
        link_cmd = "gcc -shared -fPIC -I" + inc_path + san_flags + obj_list + " " + rt_obj + " " + ctx_obj + " " + sdl_obj + " " + gpu_obj + " " + lib_obj
                 + " -o " + output_name + " -lpthread -lm -ldl " + sdl_libs + gc_link + " 2>&1";
    } else if (static_lib) {
        std::string ar_cmd = "ar rcs " + output_name + obj_list + " " + rt_obj + " " + ctx_obj + " " + sdl_obj + " " + gpu_obj + " " + lib_obj + " 2>&1";
        int ar_result = std::system(ar_cmd.c_str());
        if (ar_result != 0) {
            std::cerr << "Static library creation failed\n";
            return false;
        }
        std::cout << "Static lib OK: " << output_name << "\n";
        return true;
    } else {
        link_cmd = "gcc -I" + inc_path + san_flags + obj_list + " " + rt_obj + " " + ctx_obj + " " + sdl_obj + " " + gpu_obj + " " + lib_obj
                 + " -o " + output_name + " -lpthread -lm -ldl " + sdl_libs + gc_link + " 2>&1";
    }
    int link_result = std::system(link_cmd.c_str());
    if (link_result != 0) {
        std::cerr << "Linking failed (exit: " << link_result << ")\n";
        return false;
    }
    phaseMark("link");

    std::cout << "Link OK: " << output_name << "\n";
    return true;
}

static const char* MYP_VERSION = "3.11.20";
// Language specification version (frozen grammar, see docs/grammar.md).
// Bump ONLY on breaking syntax/semantics changes (see docs/CHANGELOG.md).
static const char* MYP_SPEC_VERSION = "1.0";

// ---- Crash guard -----------------------------------------------------------
// LLVM reports internal errors ("LLVM ERROR: ...", e.g. "Cannot select") via
// report_fatal_error(), which aborts the process by default. For a compiler
// that must never crash on any input, convert these into a clean diagnostic
// by throwing an exception that the outer main() catches.
static void mypFatalErrorHandler(void* /*user_data*/,
                                 const char* reason,
                                 bool /*gen_crash_diag*/) {
    throw std::runtime_error(std::string("LLVM fatal error: ") + reason);
}

static int runFmt(int argc, char* argv[]) {
    bool check_mode = false;
    std::vector<std::string> fmt_files;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--check") check_mode = true;
        else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: mypc fmt [options] <file.myp> ...\n";
            std::cout << "Options:\n";
            std::cout << "  --check    Only check formatting, don't write\n";
            std::cout << "  --help, -h Show this help\n";
            return 0;
        } else fmt_files.push_back(arg);
    }
    if (fmt_files.empty()) {
        std::cerr << "Error: no input files for fmt\n";
        return 1;
    }
    bool all_ok = true;
    for (auto& fname : fmt_files) {
        mylang::SourceManager src_mgr;
        if (!src_mgr.loadFile(fname)) {
            std::cerr << "Error: cannot open '" << fname << "'\n";
            all_ok = false;
            continue;
        }
        mylang::DiagnosticEngine diag(src_mgr);
        mylang::Formatter fmt(src_mgr, diag);
        std::string formatted = fmt.format();
        if (diag.hasErrors()) {
            all_ok = false;
            continue;
        }
        if (check_mode) {
            if (formatted != src_mgr.source()) {
                std::cout << fname << " would be reformatted\n";
                all_ok = false;
            }
        } else {
            std::ofstream ofs(fname, std::ios::trunc);
            if (!ofs) {
                std::cerr << "Error: cannot write '" << fname << "'\n";
                all_ok = false;
                continue;
            }
            ofs << formatted;
            ofs.close();
            std::cout << "Formatted: " << fname << "\n";
        }
    }
    return all_ok ? 0 : 1;
}

// mypc run <file.myp> [args...] — 仿 `go run`：
//   编译（单类文件无 main 时自动注入合成 main 实例化 @startup 类）→ 链接到临时
//   二进制 → 执行（透传 args）→ 清理临时产物。退出码 = 程序退出码。
// Detect a subcommand (run) that may follow leading flags, e.g.
// "mypc -O2 run file.myp [args...]". Returns the argv index of the
// subcommand token, or 0 if the leading tokens are not flags leading to one.
static int findSubcommand(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "run") return i;
        if (a == "fmt") return i;
        // Flags that consume a following value → skip the value.
        if (a == "--stdlib" || a == "--package-path" || a == "--passes" || a == "-o" || a == "--frontend-dump") {
            i++;
            continue;
        }
        // Flags without a value argument.
        if (a.size() >= 2 && a[0] == '-' &&
            (a[1] == 'O' || a[1] == 'g')) continue;
        if (a == "--debug" || a == "--trace" || a == "--emit-llvm" ||
            a == "--shared" || a == "--static" || a == "--test" ||
            a == "--macro-expand" || a == "--version" || a == "--help" || a == "-h")
            continue;
        // First non-flag token that isn't a subcommand → no subcommand.
        return 0;
    }
    return 0;
}

static int runFile(int argc, char* argv[], int sub_idx, int opt_level) {
    if (argc < sub_idx + 2) {
        std::cerr << "Usage: " << argv[0] << " run <file.myp> [args...]\n";
        return 1;
    }
    std::string file = argv[sub_idx + 1];
    std::vector<std::string> prog_args;
    for (int i = sub_idx + 2; i < argc; i++) prog_args.push_back(argv[i]);

    // 相对可执行文件自动检测 stdlib（与主流程一致）
    std::string stdlib_path = "stdlib";
    {
        std::string exe_dir = selfExeDir(argv[0]);
        std::string exe_stdlib = exe_dir + "/../stdlib";
        if (fileExists(exe_stdlib + "/env.myp")) stdlib_path = exe_stdlib;
    }

    // 编译（auto_main=true → 单类文件自动 main）；产物 <file>.myp.o
    auto obj = compileSingle(file, stdlib_path, "", opt_level, false, false, false, false, false, "", false, true);
    if (obj.empty()) return 1;

    // 链接到临时二进制
    std::string base = "/tmp/myp_run_" + std::to_string((long)::getpid()) + "_" +
                       std::to_string(std::rand());
    if (!linkObjects({obj}, base, stdlib_path, false)) {
        std::remove(obj.c_str());
        return 1;
    }
    std::remove(obj.c_str());   // 清理 .o

    // 执行
    std::vector<char*> exec_argv;
    exec_argv.push_back(const_cast<char*>(base.c_str()));
    for (auto& a : prog_args) exec_argv.push_back(const_cast<char*>(a.c_str()));
    exec_argv.push_back(nullptr);

    pid_t pid = ::fork();
    if (pid == 0) {
        ::execv(base.c_str(), exec_argv.data());
        std::cerr << "Error: failed to execute '" << base << "'\n";
        ::_exit(127);
    } else if (pid < 0) {
        std::cerr << "Error: fork failed\n";
        std::remove(base.c_str());
        return 1;
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    std::remove(base.c_str());  // 清理临时二进制
    return exit_code;
}

// Forward declaration: real work of main() (defined after the wrapper).
static int realMain(int argc, char* argv[]);

int main(int argc, char* argv[]) {
    llvm::install_fatal_error_handler(mypFatalErrorHandler);
    try {
        return realMain(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "Internal compiler error: " << e.what() << "\n";
        std::cerr << "This is a compiler bug, not an error in your program.\n"
                  << "Please report the input that triggered it.\n";
        return 1;
    }
}

static int realMain(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " [options] <file1.myp> [file2.myp ...]\n";
        std::cerr << "       " << argv[0] << " fmt [options] <file.myp> ...\n";
        std::cerr << "       " << argv[0] << " run <file.myp> [args...]  (仿 go run: 编译+运行+清理; 单类文件无 main 自动补, 须类带 @startup)\n";
        std::cerr << "Options:\n";
        std::cerr << "  -o <file>       Set output filename\n";
        std::cerr << "  -O[0123]        Set optimization level (default: -O0)\n";
        std::cerr << "  --stdlib <path> Set stdlib directory\n";
        std::cerr << "  --trace         Enable runtime event tracing\n";
        std::cerr << "  --emit-llvm     Save LLVM IR to .ll file (skip linking)\n";
        std::cerr << "  --test          Build and run tests (generate test runner)\n";
        std::cerr << "  -g, --debug     Emit DWARF debug info (line/var/type)\n";
        std::cerr << "  --version       Show version number\n";
        std::cerr << "  --help, -h      Show this help message\n";
        return 1;
    }

    // ---- Subcommand: fmt ----
    if (strcmp(argv[1], "fmt") == 0) {
        return runFmt(argc, argv);
    }

    // ---- Subcommand: run (仿 go run) ----
    // Flags may precede the subcommand (e.g. "mypc -O2 run file.myp" — the
    // run_tests_O2.sh wrapper sets MYPCC="./build/mypc -O2"). Extract the
    // leading -O level and pass it through to the compile.
    {
        int sub_idx = findSubcommand(argc, argv);
        if (sub_idx > 0 && strcmp(argv[sub_idx], "run") == 0) {
            int ropt = 0;
            for (int j = 1; j < sub_idx; j++) {
                std::string a = argv[j];
                if (a.size() >= 2 && a[0] == '-' && a[1] == 'O') {
                    ropt = a.size() > 2 ? (a[2] - '0') : 1;
                    if (ropt < 0 || ropt > 3) ropt = 0;
                }
            }
            return runFile(argc, argv, sub_idx, ropt);
        }
    }

    std::string stdlib_path = "stdlib";
    std::string package_path = "";
    std::string output_name_v;
    std::string frontend_dump_mode;
    int opt_level = 0;
    bool trace_enabled = false;
    bool emit_llvm = false;
    bool shared_lib = false;
    bool static_lib = false;
    bool test_mode = false;
    bool debug_mode = false;
    bool macro_expand = false;
    std::string passes;
    std::vector<std::string> filenames;
    int i = 1;
    while (i < argc) {
        std::string arg = argv[i];
        if (arg == "--version") {
            std::cout << "MYP Compiler v" << MYP_VERSION
                      << " (Language Spec " << MYP_SPEC_VERSION << ")\n";
            return 0;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [options] <file1.myp> [file2.myp ...]\n";
            std::cout << "       " << argv[0] << " fmt [options] <file.myp> ...\n";
            std::cout << "       " << argv[0] << " run <file.myp> [args...]  (仿 go run)\n";
            std::cout << "Options:\n";
            std::cout << "  -o <file>       Set output filename\n";
            std::cout << "  -O[0123]        Set optimization level (default: -O0)\n";
            std::cout << "  --stdlib <path> Set stdlib directory\n";
            std::cout << "  --package-path <path> Set local package directory\n";
            std::cout << "  --trace         Enable runtime event tracing\n";
            std::cout << "  --shared        Build shared library (.so)\n";
            std::cout << "  --static        Build static library (.a)\n";
            std::cout << "  --emit-llvm     Save LLVM IR to .ll file (skip linking)\n";
            std::cout << "  --test          Build and run tests (generate test runner)\n";
            std::cout << "  -g, --debug     Emit DWARF debug info (line/var/type)\n";
            std::cout << "  --passes <p>    Run custom MYP pass pipeline (e.g. myp-pass)\n";
            std::cout << "  --macro-expand  Dump expanded AST after macro expansion\n";
            std::cout << "  --frontend-dump <tokens|ast|sema>  Deterministic front-end dump\n";
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
        } else if (arg == "--shared") {
            shared_lib = true;
        } else if (arg == "--static") {
            static_lib = true;
        } else if (arg == "--test") {
            test_mode = true;
        } else if (arg == "-g" || arg == "--debug") {
            debug_mode = true;
        } else if (arg == "--passes") {
            if (i + 1 >= argc) { std::cerr << "Error: --passes requires an argument\n"; return 1; }
            passes = argv[++i];
        } else if (arg == "--macro-expand") {
            macro_expand = true;
        } else if (arg == "--emit-llvm") {
            emit_llvm = true;
        } else if (arg == "--frontend-dump") {
            if (i + 1 >= argc) { std::cerr << "Error: --frontend-dump requires an argument\n"; return 1; }
            frontend_dump_mode = argv[++i];
        } else {
            filenames.push_back(arg);
        }
        i++;
    }

    if (filenames.empty()) {
        std::cerr << "Error: no input file specified\n";
        return 1;
    }

    // Auto-detect stdlib relative to executable (works for relative argv[0] too)
    if (stdlib_path == "stdlib") {
        std::string exe_dir = selfExeDir(argv[0]);
        std::string exe_stdlib = exe_dir + "/../stdlib";
        if (fileExists(exe_stdlib + "/env.myp"))
            stdlib_path = exe_stdlib;
    }

    if (trace_enabled) {
        std::cout << "Event tracing enabled (--trace)\n";
    }

    // --frontend-dump: deterministic front-end oracle (self-hosting F0).
    if (!frontend_dump_mode.empty()) {
        if (filenames.size() != 1) {
            std::cerr << "Error: --frontend-dump requires exactly one input file\n";
            return 1;
        }
        return runFrontendDump(frontend_dump_mode, filenames[0], stdlib_path, package_path);
    }

    // Determine output name
    if (output_name_v.empty()) {
        if (filenames.size() == 1) {
            output_name_v = filenames[0].substr(0, filenames[0].find_last_of('.')) + ".out";
        } else {
            output_name_v = "a.out";
        }
    }

    bool library_mode = shared_lib || static_lib;

    // Determine default output name for libraries
    if (output_name_v.empty() && library_mode && filenames.size() == 1) {
        std::string base = filenames[0].substr(0, filenames[0].find_last_of('.'));
        output_name_v = shared_lib ? base + ".so" : base + ".a";
    }

    // --emit-llvm only works with single file
    if (emit_llvm) {
        if (filenames.size() > 1) {
            std::cerr << "Warning: --emit-llvm only supported for single file\n";
        }
        auto obj = compileSingle(filenames[0], stdlib_path, package_path, opt_level, trace_enabled, true, library_mode, test_mode, debug_mode, passes, macro_expand);
        return obj.empty() ? 1 : 0;
    }

    if (filenames.size() == 1) {
        // Single file: use simple compile + link
        auto obj = compileSingle(filenames[0], stdlib_path, package_path, opt_level, trace_enabled, false, library_mode, test_mode, debug_mode, passes, macro_expand);
        if (obj.empty()) return 1;
        if (!linkObjects({obj}, output_name_v, stdlib_path, trace_enabled, shared_lib, static_lib))
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
    std::string obj_path = doCompile(*merged, filenames[0], opt_level, false, library_mode, test_mode, debug_mode, passes, macro_expand, diag);
    if (obj_path.empty()) return 1;

    // Link
    if (!linkObjects({obj_path}, output_name_v, stdlib_path, trace_enabled, shared_lib, static_lib))
        return 1;

    return 0;
}
