// MYP Language Server Protocol implementation
// Communicates with editors via JSON-RPC over stdin/stdout.
//
// Supported capabilities:
//   textDocument/didOpen        - diagnostics on open
//   textDocument/didChange      - diagnostics on edit
//   textDocument/didSave        - diagnostics on save
//   textDocument/completion     - code completion (keywords, class names, methods)
//   textDocument/hover          - type information on hover
//   textDocument/definition     - go to definition
//   textDocument/documentSymbol - document outline
//   textDocument/references     - find all references

#include "mylang/LSP.h"
#include "mylang/SourceLocation.h"
#include "mylang/DiagnosticEngine.h"
#include "mylang/Token.h"
#include "mylang/Lexer.h"
#include "mylang/Parser.h"
#include "mylang/Sema.h"
#include "mylang/AST.h"

#include <chrono>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <unordered_set>
#include <utility>
#include <cstdlib>
#include <cstring>

namespace mylang {
namespace {

// Forward declarations
std::string typeToBasicTypeName(BuiltinType bt);

// Definition map: symbol name → [{uri, SourceRange}]
using DefinitionMap = std::unordered_map<std::string, std::vector<std::pair<std::string, SourceRange>>>;

// ---- Active document state ----
struct Document {
    std::string uri;
    std::string text;
    std::vector<std::string> lines;
    std::unique_ptr<TranslationUnit> ast;
    DefinitionMap def_map;
    std::string stdlib_path = "stdlib";

    void updateLines() {
        lines.clear();
        std::istringstream stream(text);
        std::string line;
        while (std::getline(stream, line)) {
            lines.push_back(line);
        }
    }
};

std::unordered_map<std::string, Document> documents_;
std::string server_stdlib_path_ = "stdlib";
bool diagnostics_running_ = false;
auto last_diag_time_ = std::chrono::steady_clock::now();

// Forward declarations
static void buildDefinitionMap(Document& doc);

// ---- Parse a document into AST (includes import resolution for definitions) ----
static void parseDocument(Document& doc, const std::string& uri) {
    doc.ast = nullptr;
    doc.def_map.clear();
    
    // Extract file path from URI (file:///path → /path)
    std::string filepath = uri;
    if (filepath.find("file://") == 0) filepath = filepath.substr(7);
    
    SourceManager src_mgr;
    if (!src_mgr.loadString(doc.text, filepath)) return;
    
    DiagnosticEngine diag(src_mgr);
    Lexer lex(src_mgr, diag);
    auto toks = lex.tokenize();
    if (diag.hasErrors()) return;
    
    Parser parser(toks, diag);
    auto ast = parser.parse();
    if (diag.hasErrors()) return;

    // Build definition map from current file (uses doc.uri)
    doc.ast = std::move(ast);
    buildDefinitionMap(doc);
    
    // Resolve imports — build separate def maps for each and merge
    std::string source_dir = filepath.substr(0, filepath.find_last_of("/\\"));
    std::unordered_set<std::string> loaded;
    loaded.insert(filepath);
    
    for (auto& imp : doc.ast->imports) {
        if (!imp.is_path) continue;
        std::string imp_path = imp.file_path;
        if (imp_path[0] != '/') imp_path = source_dir + "/" + imp_path;
        if (loaded.count(imp_path)) continue;
        loaded.insert(imp_path);
        
        SourceManager imp_src;
        if (!imp_src.loadFile(imp_path)) continue;
        
        DiagnosticEngine imp_diag(imp_src);
        Lexer imp_lex(imp_src, imp_diag);
        auto imp_toks = imp_lex.tokenize();
        if (imp_diag.hasErrors()) continue;
        
        Parser imp_par(imp_toks, imp_diag);
        auto imp_ast = imp_par.parse();
        if (imp_diag.hasErrors()) continue;
        
        // Build def map for the imported file with its own URI
        Document imp_doc;
        imp_doc.uri = "file://" + imp_path;
        imp_doc.ast = std::move(imp_ast);
        buildDefinitionMap(imp_doc);
        
        // Merge its def map into the main document's def map
        for (auto& [name, locs] : imp_doc.def_map) {
            auto& target = doc.def_map[name];
            target.insert(target.end(), locs.begin(), locs.end());
        }
    }
}

// ---- Build definition map from AST ----
static void buildDefinitionMap(Document& doc) {
    if (!doc.ast) return;
    doc.def_map.clear();
    try {
        auto& map = doc.def_map;
        for (auto& cls : doc.ast->classes) {
            map[cls.name].push_back({doc.uri, cls.range});
            for (auto& a : cls.actions)
                map[a.name].push_back({doc.uri, a.range});
            for (auto& e : cls.events)
                map[e.name].push_back({doc.uri, e.range});
            for (auto& p : cls.properties)
                map[p.name].push_back({doc.uri, p.range});
            for (auto& f : cls.functions)
                map[f.name].push_back({doc.uri, f.range});
        }
        for (auto& fn : doc.ast->functions)
            map[fn.name].push_back({doc.uri, fn.range});
        for (auto& en : doc.ast->enums)
            map[en.name].push_back({doc.uri, en.range});
        for (auto& st : doc.ast->structs)
            map[st.name].push_back({doc.uri, st.range});
    } catch (...) {
        // Silently skip definition map errors
    }
}

// ---- Extract word at position from document ----
static std::string extractWordAt(Document& doc, int line, int col) {
    if (line < 0 || line >= (int)doc.lines.size()) return "";
    const std::string& src_line = doc.lines[line];
    if (col < 0 || col > (int)src_line.size()) return "";
    int start = col;
    while (start > 0 && (isalnum(src_line[start-1]) || src_line[start-1] == '_'))
        start--;
    int end = col;
    while (end < (int)src_line.size() && (isalnum(src_line[end]) || src_line[end] == '_'))
        end++;
    return src_line.substr(start, end - start);
}

// Correctly decode a JSON string literal. `start` points to the char AFTER the
// opening quote. Handles \n \t \r \\ \" \/ escapes (passes others through).
static std::string decodeJSONString(const std::string& raw, size_t start) {
    std::string out;
    for (size_t i = start; i < raw.size(); i++) {
        char c = raw[i];
        if (c == '\\') {
            if (i + 1 >= raw.size()) break;
            char e = raw[++i];
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case '\\': out += '\\'; break;
                case '"': out += '"'; break;
                case '/': out += '/'; break;
                default: out += e; break;
            }
        } else if (c == '"') {
            break;
        } else {
            out += c;
        }
    }
    return out;
}

std::string rangeToJSON(const SourceRange& range) {
    std::string result = "{";
    result += "\"start\":{\"line\":" + std::to_string(range.begin.line) +
              ",\"character\":" + std::to_string(range.begin.column) + "},";
    result += "\"end\":{\"line\":" + std::to_string(range.end.line) +
              ",\"character\":" + std::to_string(range.end.column) + "}";
    result += "}";
    return result;
}

// ---- Diagnostics disabled: full compilation on every save causes freezes.
//      Use mypc on the command line to compile and see errors. ----

// === LSP Handlers ===

struct CompletionItem {
    std::string label;
    std::string detail;
    std::string kind; // "keyword"|"class"|"method"|"function"|"property"|"event"
};

void handleInitialize(const std::string& id, const std::string& /*params*/) {
    std::string result = "{";
    result += "\"capabilities\":{";
    result += "\"textDocumentSync\":1,"; // Full (not Incremental) — didChange sends full text
    result += "\"completionProvider\":{\"triggerCharacters\":[\".\",\":\"]},";
    result += "\"hoverProvider\":true,";
    result += "\"definitionProvider\":true,";
    result += "\"documentSymbolProvider\":true,";
    result += "\"referencesProvider\":true";
    result += "}";
    result += "}";
    sendResponse(id, result);
}

void handleTextDocumentDidOpen(const std::string& params) {
    // Parse JSON manually (minimal)
    // Extract uri and text
    auto uri_start = params.find("\"uri\":\"");
    std::string uri;
    if (uri_start != std::string::npos) {
        uri_start += 7;
        auto uri_end = params.find("\"", uri_start);
        if (uri_end != std::string::npos) {
            uri = params.substr(uri_start, uri_end - uri_start);
        }
    }
    auto text_start = params.find("\"text\":\"");
    std::string text;
    if (text_start != std::string::npos)
        text = decodeJSONString(params, text_start + 8);

    if (!uri.empty()) {
        Document doc;
        doc.uri = uri;
        doc.text = text;
        doc.updateLines();
        // Parse document into AST for definition/completion support
        // NOTE: This parses on open which is fine (one-time cost).
        // didChange (keystrokes) does NOT parse — see handleTextDocumentDidChange.
        parseDocument(doc, uri);
        documents_[uri] = std::move(doc);
    }
}

void handleTextDocumentDidChange(const std::string& params) {
    auto uri_start = params.find("\"uri\":\"");
    std::string uri;
    if (uri_start != std::string::npos) {
        uri_start += 7;
        auto uri_end = params.find("\"", uri_start);
        if (uri_end != std::string::npos) {
            uri = params.substr(uri_start, uri_end - uri_start);
        }
    }

    // Simple approach: replace full text
    auto text_start = params.find("\"text\":\"");
    std::string text;
    if (text_start != std::string::npos)
        text = decodeJSONString(params, text_start + 8);

    auto it = documents_.find(uri);
    if (it != documents_.end() && !text.empty()) {
        it->second.text = text;
        it->second.updateLines();
        // LAZY PARSE: do NOT parse AST on every keystroke.
        // AST will be parsed on demand when definition/hover/documentSymbol is requested.
        // This prevents LSP from freezing on fast typing.
        it->second.ast = nullptr;    // invalidate old AST
        it->second.def_map.clear();  // clear def map
    }
}

void handleTextDocumentDidSave(const std::string& params) {
    // Diagnostics disabled — use mypc to compile
}

void handleCompletion(const std::string& id, const std::string& /*params*/) {
    // Provide completions: keywords, class names, methods
    std::vector<CompletionItem> items;

    // MYP keywords
    const char* keywords[] = {
        "class", "action", "event", "property", "interface", "import",
        "void", "byte", "short", "int", "long", "ubyte", "ushort", "uint", "ulong",
        "char", "float", "double", "bool", "string",
        "mapping", "if", "else", "while", "for", "return", "break", "continue",
        "static", "true", "false", "null", "this", "new", "struct", "function",
        "var"
    };
    for (auto& kw : keywords) {
        items.push_back({kw, "keyword", "14"});
    }

    // Class names from active documents
    for (auto& [uri, doc] : documents_) {
        if (doc.ast) {
            for (auto& cls : doc.ast->classes) {
                items.push_back({cls.name, "class", "5"});
                for (auto& act : cls.actions)
                    items.push_back({act.name, "method (" + cls.name + ")", "2"});
                for (auto& ev : cls.events)
                    items.push_back({ev.name, "event (" + cls.name + ")", "2"});
                for (auto& prop : cls.properties)
                    items.push_back({prop.name, "property (" + cls.name + ")", "5"});
                for (auto& fn : cls.functions)
                    items.push_back({fn.name, "function (" + cls.name + ")", "2"});
            }
            for (auto& en : doc.ast->enums) {
                items.push_back({en.name, "enum", "5"});
                for (auto& v : en.variants)
                    items.push_back({v.name, "variant (" + en.name + ")", "2"});
            }
            for (auto& fn : doc.ast->functions)
                items.push_back({fn.name, "function", "2"});
        }
    }

    // Sort and deduplicate
    std::sort(items.begin(), items.end(),
        [](const CompletionItem& a, const CompletionItem& b) {
            return a.label < b.label;
        });
    items.erase(std::unique(items.begin(), items.end(),
        [](const CompletionItem& a, const CompletionItem& b) {
            return a.label == b.label;
        }), items.end());

    // Build JSON response
    std::string completion_list;
    for (size_t i = 0; i < items.size(); i++) {
        if (i > 0) completion_list += ",";
        completion_list += "{";
        completion_list += "\"label\":" + jsonString(items[i].label) + ",";
        completion_list += "\"kind\":" + items[i].kind + ",";
        if (!items[i].detail.empty())
            completion_list += "\"detail\":" + jsonString(items[i].detail);
        completion_list += "}";
    }

    std::string result = "{\"isIncomplete\":false,\"items\":[" + completion_list + "]}";
    sendResponse(id, result);
}

void handleHover(const std::string& id, const std::string& params) {
    // Extract position
    auto uri_start = params.find("\"uri\":\"");
    std::string uri;
    if (uri_start != std::string::npos) {
        uri_start += 7;
        auto uri_end = params.find("\"", uri_start);
        if (uri_end != std::string::npos)
            uri = params.substr(uri_start, uri_end - uri_start);
    }

    auto line_start = params.find("\"line\":");
    int line = 0, col = 0;
    if (line_start != std::string::npos) {
        line_start += 7;
        auto line_end = params.find_first_of(",}", line_start);
        if (line_end != std::string::npos)
            line = std::stoi(params.substr(line_start, line_end - line_start));
    }
    auto col_start = params.find("\"character\":");
    if (col_start != std::string::npos) {
        col_start += 12;
        auto col_end = params.find_first_of(",}", col_start);
        if (col_end != std::string::npos)
            col = std::stoi(params.substr(col_start, col_end - col_start));
    }

    std::string hover_text;
    auto it = documents_.find(uri);
    if (it != documents_.end() && line >= 0 && line < (int)it->second.lines.size()) {
        // Lazy parse: build AST if needed for hover info
        if (!it->second.ast) parseDocument(it->second, uri);
        if (!it->second.ast) { sendResponse(id, "{\"contents\":[]}"); return; }
        
        const std::string& src_line = it->second.lines[line];
        if (col >= 0 && col < (int)src_line.size()) {
            // Extract word at cursor
            int start = col;
            while (start > 0 && (isalnum(src_line[start-1]) || src_line[start-1] == '_'))
                start--;
            int end = col;
            while (end < (int)src_line.size() && (isalnum(src_line[end]) || src_line[end] == '_'))
                end++;
            std::string word = src_line.substr(start, end - start);

            if (!word.empty()) {
                bool found = false;
                // Search in AST for this symbol
                for (auto& cls : it->second.ast->classes) {
                    if (cls.name == word) {
                        hover_text = "class " + cls.name;
                        hover_text += "\n---\nactions: " + std::to_string(cls.actions.size());
                        hover_text += ", events: " + std::to_string(cls.events.size());
                        hover_text += ", properties: " + std::to_string(cls.properties.size());
                        found = true; break;
                    }
                    for (auto& a : cls.actions) {
                        if (a.name == word) {
                            hover_text = cls.name + "." + a.name + "(";
                            for (size_t pi = 0; pi < a.params.size(); pi++) {
                                if (pi > 0) hover_text += ", ";
                                hover_text += typeToBasicTypeName(a.params[pi].type.basic_type);
                            }
                            hover_text += ") → " + typeToBasicTypeName(a.return_type.basic_type);
                            if (a.has_startup) hover_text += " [@startup]";
                            found = true; break;
                        }
                    }
                    if (found) break;
                    for (auto& ev : cls.events) {
                        if (ev.name == word) {
                            hover_text = cls.name + "." + ev.name + "(";
                            for (size_t pi = 0; pi < ev.params.size(); pi++) {
                                if (pi > 0) hover_text += ", ";
                                hover_text += typeToBasicTypeName(ev.params[pi].type.basic_type);
                            }
                            hover_text += ") → event";
                            found = true; break;
                        }
                    }
                    if (found) break;
                    for (auto& p : cls.properties) {
                        if (p.name == word) {
                            hover_text = cls.name + "." + p.name + " : " + typeToBasicTypeName(p.type.basic_type);
                            found = true; break;
                        }
                    }
                    if (found) break;
                    for (auto& fn : cls.functions) {
                        if (fn.name == word) {
                            hover_text = cls.name + "::" + fn.name + "(";
                            for (size_t pi = 0; pi < fn.params.size(); pi++) {
                                if (pi > 0) hover_text += ", ";
                                hover_text += typeToBasicTypeName(fn.params[pi].type.basic_type);
                            }
                            hover_text += ") → " + typeToBasicTypeName(fn.return_type.basic_type);
                            found = true; break;
                        }
                    }
                    if (found) break;
                }
                // Check enums (if not found in classes)
                if (hover_text.empty()) {
                    for (auto& en : it->second.ast->enums) {
                        if (en.name == word) {
                            hover_text = "enum " + en.name + " { ";
                            for (size_t vi = 0; vi < en.variants.size(); vi++) {
                                if (vi > 0) hover_text += ", ";
                                hover_text += en.variants[vi].name;
                            }
                            hover_text += " }";
                            break;
                        }
                    }
                }
                // Check top-level functions
                if (hover_text.empty()) {
                    for (auto& fn : it->second.ast->functions) {
                        if (fn.name == word) {
                            hover_text = "function " + fn.name + "(";
                            for (size_t pi = 0; pi < fn.params.size(); pi++) {
                                if (pi > 0) hover_text += ", ";
                                hover_text += typeToBasicTypeName(fn.params[pi].type.basic_type);
                            }
                            hover_text += ") → " + typeToBasicTypeName(fn.return_type.basic_type);
                            break;
                        }
                    }
                }
            }
        }
    }

    if (hover_text.empty()) {
        sendResponse(id, "{\"contents\":[]}");
    } else {
        std::string result = "{\"contents\":{\"kind\":\"markdown\",\"value\":" + jsonString(hover_text) + "}}";
        sendResponse(id, result);
    }
}

void handleDefinition(const std::string& id, const std::string& params) {
    // Parse URI and position from params
    auto uri_start = params.find("\"uri\":\"");
    std::string uri;
    if (uri_start != std::string::npos) {
        uri_start += 7;
        auto uri_end = params.find("\"", uri_start);
        if (uri_end != std::string::npos)
            uri = params.substr(uri_start, uri_end - uri_start);
    }

    int line = -1, col = -1;
    auto line_start = params.find("\"line\":");
    if (line_start != std::string::npos) {
        line_start += 7;
        line = std::stoi(params.substr(line_start));
    }
    auto col_start = params.find("\"character\":");
    if (col_start != std::string::npos) {
        col_start += 12;
        col = std::stoi(params.substr(col_start));
    }

    auto it = documents_.find(uri);
    if (it == documents_.end()) { sendResponse(id, "null"); return; }

    // Ensure document is parsed
    if (!it->second.ast) parseDocument(it->second, uri);

    std::string word = extractWordAt(it->second, line, col);
    if (word.empty()) { sendResponse(id, "null"); return; }

    // Look up in definition map
    auto dit = it->second.def_map.find(word);
    if (dit != it->second.def_map.end() && !dit->second.empty()) {
        auto& def = dit->second[0]; // First definition (prefer current file)
        for (auto& d : dit->second) {
            if (d.first == uri) { def = d; break; } // Prefer same file
        }
        std::string result = "{";
        result += "\"uri\":" + jsonString(def.first) + ",";
        result += "\"range\":" + rangeToJSON(def.second);
        result += "}";
        sendResponse(id, result);
    } else {
        sendResponse(id, "null");
    }
}

void handleDocumentSymbol(const std::string& id, const std::string& params) {
    try {
    auto uri_start = params.find("\"uri\":\"");
    std::string uri;
    if (uri_start != std::string::npos) {
        uri_start += 7;
        auto uri_end = params.find("\"", uri_start);
        if (uri_end != std::string::npos)
            uri = params.substr(uri_start, uri_end - uri_start);
    }

    // Ensure document is parsed
    auto it = documents_.find(uri);
    if (it == documents_.end()) { sendResponse(id, "[]"); return; }
    if (!it->second.ast) parseDocument(it->second, uri);

    std::string symbols;
    int count = 0;
    if (it->second.ast) {
        for (auto& cls : it->second.ast->classes) {
            if (count++ > 0) symbols += ",";
            symbols += "{";
            symbols += "\"name\":" + jsonString(cls.name) + ",";
            symbols += "\"kind\":5,"; // Class
            symbols += "\"range\":" + rangeToJSON(cls.range) + ",";
            symbols += "\"selectionRange\":" + rangeToJSON(cls.range);
            symbols += "}";
        }
        for (auto& fn : it->second.ast->functions) {
            if (count++ > 0) symbols += ",";
            symbols += "{";
            symbols += "\"name\":" + jsonString(fn.name) + ",";
            symbols += "\"kind\":12,"; // Function
            symbols += "\"range\":" + rangeToJSON(fn.range) + ",";
            symbols += "\"selectionRange\":" + rangeToJSON(fn.range);
            symbols += "}";
        }
        for (auto& en : it->second.ast->enums) {
            if (count++ > 0) symbols += ",";
            symbols += "{";
            symbols += "\"name\":" + jsonString(en.name) + ",";
            symbols += "\"kind\":10,"; // Enum
            symbols += "\"range\":" + rangeToJSON(en.range) + ",";
            symbols += "\"selectionRange\":" + rangeToJSON(en.range);
            symbols += "}";
        }
    }

    if (count == 0) {
        sendResponse(id, "[]");
    } else {
        sendResponse(id, "[" + symbols + "]");
    }
    } catch (...) { sendResponse(id, "[]"); }
}

void handleReferences(const std::string& id, const std::string& /*params*/) {
    sendResponse(id, "[]");
}

// ---- Type info helper (defined inside anonymous namespace) ----
std::string typeToBasicTypeName(BuiltinType bt) {
    switch (bt) {
        case BuiltinType::Void: return "void";
        case BuiltinType::Int: return "int";
        case BuiltinType::Float: return "float";
        case BuiltinType::Double: return "double";
        case BuiltinType::Bool: return "bool";
        case BuiltinType::Byte: return "byte";
        case BuiltinType::Short: return "short";
        case BuiltinType::Long: return "long";
        case BuiltinType::Char: return "char";
        case BuiltinType::String: return "string";
        case BuiltinType::UByte: return "ubyte";
        case BuiltinType::UShort: return "ushort";
        case BuiltinType::UInt: return "uint";
        case BuiltinType::ULong: return "ulong";
        default: return "?";
    }
}

} // anonymous namespace
std::string jsonString(const std::string& s) {
    std::string result = "\"";
    for (char c : s) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result += c;
        }
    }
    result += "\"";
    return result;
}

std::string jsonPair(const std::string& key, const std::string& val) {
    return jsonString(key) + ":" + val;
}

std::string jsonInt(const std::string& key, int val) {
    return jsonString(key) + ":" + std::to_string(val);
}

std::string jsonArray(const std::string& items) {
    return "[" + items + "]";
}

std::string jsonObject(const std::string& members) {
    return "{" + members + "}";
}

// ---- Message I/O ----
bool readMessage(LSPMessage& msg) {
    msg = LSPMessage{};
    std::string line;

    // Read headers
    while (std::getline(std::cin, line)) {
        if (line.empty() || line == "\r") break;
        if (line.size() >= 2 && line[line.size()-1] == '\r')
            line.resize(line.size() - 1);

        // Parse header
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;

        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);
        // Trim leading spaces
        while (!val.empty() && val[0] == ' ') val = val.substr(1);

        if (key == "Content-Length") {
            msg.content_length = std::stoi(val);
        } else if (key == "Content-Type") {
            msg.content_type = val;
        }
    }

    if (msg.content_length <= 0) return false;

    // Read body
    msg.body.resize(msg.content_length);
    std::cin.read(&msg.body[0], msg.content_length);
    if (std::cin.gcount() != msg.content_length) return false;

    return true;
}

void sendMessage(const std::string& body) {
    std::cout << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    std::cout.flush();
}

void sendResponse(const std::string& id, const std::string& result) {
    std::string body = "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":" + result + "}";
    sendMessage(body);
}

void sendNotification(const std::string& method, const std::string& params) {
    std::string body = "{\"jsonrpc\":\"2.0\",\"method\":\"";
    body += method;
    body += "\",\"params\":" + params + "}";
    sendMessage(body);
}

// ---- Main LSP loop ----
int runLSPServer(int argc, char** argv) {
    // Parse --stdlib from argv
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "--stdlib") == 0) {
            server_stdlib_path_ = argv[i + 1];
            break;
        }
    }

    // Initialize handshake
    std::string init_notification = "{\"jsonrpc\":\"2.0\",\"method\":\"window/logMessage\",\"params\":{\"type\":3,\"message\":\"MYP Language Server started\"}}";
    sendMessage(init_notification);

    // Main loop
    LSPMessage msg;
    while (readMessage(msg)) {
        try {
        // Extract method (handle optional spaces)
        auto method_start = msg.body.find("\"method\":");
        std::string method;
        if (method_start != std::string::npos) {
            method_start += 9; // skip past "method":
            // Skip any spaces/whitespace
            while (method_start < msg.body.size() &&
                   (msg.body[method_start] == ' ' || msg.body[method_start] == '\t'))
                method_start++;
            // Skip the opening quote
            if (method_start < msg.body.size() && msg.body[method_start] == '"')
                method_start++;
            auto method_end = msg.body.find("\"", method_start);
            if (method_end != std::string::npos)
                method = msg.body.substr(method_start, method_end - method_start);
        }

        // Extract id if present (handle optional spaces)
        std::string id;
        auto id_start = msg.body.find("\"id\":");
        if (id_start != std::string::npos) {
            id_start += 5; // skip past "id":
            // Skip any spaces
            while (id_start < msg.body.size() &&
                   (msg.body[id_start] == ' ' || msg.body[id_start] == '\t'))
                id_start++;
            auto id_end = msg.body.find_first_of(",}", id_start);
            if (id_end != std::string::npos)
                id = msg.body.substr(id_start, id_end - id_start);
        }

        // Extract params (handle optional spaces)
        auto params_start = msg.body.find("\"params\":");
        std::string params;
        if (params_start != std::string::npos) {
            params_start += 9; // skip past "params":
            // The rest of the body after "params": is the params object
            // Find the matching closing brace
            int brace_depth = 0;
            bool in_string = false;
            size_t params_end = params_start;
            for (; params_end < msg.body.size(); params_end++) {
                char c = msg.body[params_end];
                if (in_string) {
                    if (c == '\\') { params_end++; continue; }
                    if (c == '"') in_string = false;
                    continue;
                }
                if (c == '"') { in_string = true; continue; }
                if (c == '{') brace_depth++;
                if (c == '}') {
                    brace_depth--;
                    if (brace_depth == 0) {
                        params_end++;
                        break;
                    }
                }
            }
            params = msg.body.substr(params_start, params_end - params_start);
        }

        if (method == "initialize") {
            handleInitialize(id, params);
        } else if (method == "textDocument/didOpen") {
            handleTextDocumentDidOpen(params);
        } else if (method == "textDocument/didChange") {
            handleTextDocumentDidChange(params);
        } else if (method == "textDocument/didSave") {
            handleTextDocumentDidSave(params);
        } else if (method == "textDocument/completion") {
            handleCompletion(id, params);
        } else if (method == "textDocument/hover") {
            handleHover(id, params);
        } else if (method == "textDocument/definition") {
            handleDefinition(id, params);
        } else if (method == "textDocument/documentSymbol") {
            handleDocumentSymbol(id, params);
        } else if (method == "textDocument/references") {
            handleReferences(id, params);
        } else if (method == "textDocument/didClose") {
            // Cleanup
            auto uri_start = params.find("\"uri\":\"");
            if (uri_start != std::string::npos) {
                uri_start += 7;
                auto uri_end = params.find("\"", uri_start);
                if (uri_end != std::string::npos) {
                    std::string uri = params.substr(uri_start, uri_end - uri_start);
                    documents_.erase(uri);
                }
            }
        } else if (method == "shutdown") {
            sendResponse(id, "null");
            break;
        } else if (method == "exit") {
            break;
        } else if (method.size() > 2 && method[0] == '$' && method[1] == '/') {
            // Ignore all $/... notifications (cancelRequest, etc.)
        } else {
            // Unknown method - return method not found
            if (!id.empty()) {
                std::string err = "{\"code\":-32601,\"message\":\"Method not found: " + method + "\"}";
                std::string body = "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"error\":" + err + "}";
                sendMessage(body);
            }
        }

        } catch (const std::exception& e) {
            std::string err = "{\"code\":-32603,\"message\":\"Internal error: " + std::string(e.what()) + "\"}";
            std::string body = "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":" + err + "}";
            sendMessage(body);
        } catch (...) {
            // Ignore unknown exceptions
        }
    }

    return 0;
}

} // namespace mylang
