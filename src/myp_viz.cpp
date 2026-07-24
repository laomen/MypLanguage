// myp_viz — MYP mapping visualizer
// Reads a .myp file and outputs Graphviz DOT format of mapping() relationships.

#include "mylang/Lexer.h"
#include "mylang/Parser.h"
#include "mylang/SourceLocation.h"

#include <iostream>
#include <string>
#include <unordered_set>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: myp_viz <file.myp>\n";
        return 1;
    }

    mylang::SourceManager src_mgr;
    if (!src_mgr.loadFile(argv[1])) {
        std::cerr << "Error: cannot open '" << argv[1] << "'\n";
        return 1;
    }

    mylang::DiagnosticEngine diag(src_mgr);
    mylang::Lexer lexer(src_mgr, diag);
    auto tokens = lexer.tokenize();
    if (diag.hasErrors()) return 1;

    mylang::Parser parser(tokens, diag);
    auto ast = parser.parse();
    if (diag.hasErrors()) return 1;

    // Collect unique class and instance names from mappings
    std::unordered_set<std::string> nodes;
    struct Edge { std::string from; std::string to; };
    std::vector<Edge> edges;

    for (auto& m : ast->mappings) {
        for (auto& chain : m.chains) {
            for (size_t i = 0; i + 1 < chain.nodes.size(); i++) {
                auto& src = chain.nodes[i];
                auto& dst = chain.nodes[i + 1];
                std::string src_label = src.source_name + "." + src.member_name;
                std::string dst_label = dst.source_name + "." + dst.member_name;
                nodes.insert(src.source_name);
                nodes.insert(dst.source_name);
                edges.push_back({src_label, dst_label});
            }
        }
    }

    // Output DOT format
    std::cout << "digraph MYP {\n";
    std::cout << "  rankdir=LR;\n";
    std::cout << "  node [shape=box, style=rounded];\n";
    std::cout << "  edge [color=blue, arrowhead=normal];\n\n";

    for (auto& n : nodes) {
        std::cout << "  \"" << n << "\";\n";
    }
    std::cout << "\n";

    for (auto& e : edges) {
        std::cout << "  \"" << e.from << "\" -> \"" << e.to << "\";\n";
    }

    std::cout << "}\n";
    return 0;
}
