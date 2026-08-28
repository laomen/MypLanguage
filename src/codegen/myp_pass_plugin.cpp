#include "mylang/MypPasses.h"

#include <llvm/Config/llvm-config.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/PassPlugin.h>

extern "C" LLVM_ATTRIBUTE_WEAK llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
    return {
        LLVM_PLUGIN_API_VERSION,
        "MYP passes",
        LLVM_VERSION_STRING,
        [](llvm::PassBuilder& builder) {
            llvm::ModulePassManager unused;
            mylang::registerMypPasses(
                builder, unused, llvm::OptimizationLevel::O0);
        }
    };
}