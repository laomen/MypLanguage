#include "mylang/DiagnosticEngine.h"
#include "mylang/Fmt.h"
#include "mylang/SourceLocation.h"

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static const char* MYP_VERSION = "3.9.0";

int main(int argc, char* argv[]) {
    bool check_mode = false;
    bool in_place = true; // default: modify files in place
    std::vector<std::string> filenames;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: myp_fmt [options] <file.myp> ...\n";
            std::cout << "Options:\n";
            std::cout << "  --check        Only check formatting, don't write\n";
            std::cout << "  --stdout       Write formatted output to stdout\n";
            std::cout << "  --version      Show version\n";
            std::cout << "  --help, -h     Show this help\n";
            return 0;
        } else if (arg == "--version") {
            std::cout << "MYP Formatter v" << MYP_VERSION << "\n";
            return 0;
        } else if (arg == "--check") {
            check_mode = true;
            in_place = false;
        } else if (arg == "--stdout") {
            in_place = false;
        } else {
            filenames.push_back(arg);
        }
    }

    if (filenames.empty()) {
        std::cerr << "Error: no input files\n";
        return 1;
    }

    bool all_ok = true;

    for (auto& fname : filenames) {
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
            std::cerr << "Error formatting " << fname << "\n";
            all_ok = false;
            continue;
        }

        if (check_mode) {
            if (formatted != src_mgr.source()) {
                std::cout << fname << " would be reformatted\n";
                all_ok = false;
            }
        } else if (in_place) {
            // Write back to file
            std::ofstream ofs(fname, std::ios::trunc);
            if (!ofs) {
                std::cerr << "Error: cannot write '" << fname << "'\n";
                all_ok = false;
                continue;
            }
            ofs << formatted;
            ofs.close();
            std::cout << "Formatted: " << fname << "\n";
        } else {
            // --stdout
            std::cout << "// " << fname << "\n";
            std::cout << formatted;
        }
    }

    return all_ok ? 0 : 1;
}
