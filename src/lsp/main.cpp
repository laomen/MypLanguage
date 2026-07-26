// MYP Language Server — main entry point
// Communicates with editors via LSP protocol over stdin/stdout.
//
// Usage: myp_lsp [--stdlib <path>]

#include "mylang/LSP.h"

int main(int argc, char** argv) {
    return mylang::runLSPServer(argc, argv);
}
