#ifndef MYLANG_MYPPASSES_H
#define MYLANG_MYPPASSES_H

#include <llvm/IR/PassManager.h>
#include <string>

namespace llvm {
class PassBuilder;
class OptimizationLevel;
class Module;
}

namespace mylang {

/// Register MYP-specific LLVM passes with a PassBuilder (append after the
/// default -O pipeline when OL != O0, and register the "-passes=..." parsing
/// callback for use with `opt`-style nested pipelines).
void registerMypPasses(llvm::PassBuilder& PB,
                       llvm::ModulePassManager& MPM,
                       llvm::OptimizationLevel OL);

/// Run a MYP pass pipeline specified as a comma-separated list of MYP pass
/// names (e.g. "myp-pass") directly on \p M. This is what `mypc --passes`
/// uses. Returns false if any name is unknown (nothing is run then).
bool runMypPasses(llvm::Module& M, const std::string& passes);

} // namespace mylang

#endif // MYLANG_MYPPASSES_H
