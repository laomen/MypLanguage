// codegen_test.cpp — @test 框架的 codegen（语言内建测试套件）。
// 集中管理：测试运行器（main）生成、per-test 异常隔离、测试 runtime 函数声明与
// __myp_* intrinsic 注册。抽到独立文件，保持 codegen.cpp / codegen_class.cpp
// 聚焦主流程（本文件是纯重构，无行为变化）。

#include "mylang/CodeGen.h"
#include "mylang/AST.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>

#include <functional>
#include <string>
#include <vector>

namespace mylang {

// ---- @test runtime 函数声明（declareRuntimeFunctions 内调用）----
void CodeGen::declareTestRuntimeFunctions() {
    auto* i32 = llvm::Type::getInt32Ty(ctx_);
    auto* i64 = llvm::Type::getInt64Ty(ctx_);
    auto* v = llvm::Type::getVoidTy(ctx_);
    auto* p = llvm::PointerType::get(ctx_, 0);
    auto* d_ty = llvm::Type::getDoubleTy(ctx_);

    runtime_assert_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {i32}, false),
        llvm::Function::ExternalLinkage, "myp_assert", module_.get());
    runtime_assert_msg_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {i32, p}, false),
        llvm::Function::ExternalLinkage, "myp_assert_msg", module_.get());
    runtime_test_set_msg_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {p}, false),
        llvm::Function::ExternalLinkage, "myp_test_set_msg", module_.get());
    runtime_assert_eq_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {i32, i32}, false),
        llvm::Function::ExternalLinkage, "myp_assert_eq", module_.get());
    runtime_assert_str_eq_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {p, p}, false),
        llvm::Function::ExternalLinkage, "myp_assert_str_eq", module_.get());
    runtime_assert_neq_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {i32, i32}, false),
        llvm::Function::ExternalLinkage, "myp_assert_neq", module_.get());
    runtime_assert_long_eq_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {i64, i64}, false),
        llvm::Function::ExternalLinkage, "myp_assert_long_eq", module_.get());
    runtime_assert_str_neq_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {p, p}, false),
        llvm::Function::ExternalLinkage, "myp_assert_str_neq", module_.get());
    runtime_test_report_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {p, i32}, false),
        llvm::Function::ExternalLinkage, "myp_test_report", module_.get());
    runtime_test_fail_msg_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {p}, false),
        llvm::Function::ExternalLinkage, "myp_test_fail_msg", module_.get());
    runtime_test_summary_ = llvm::Function::Create(
        llvm::FunctionType::get(i32, {i32}, false),
        llvm::Function::ExternalLinkage, "myp_test_summary", module_.get());
    runtime_assert_long_neq_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {i64, i64}, false),
        llvm::Function::ExternalLinkage, "myp_assert_long_neq", module_.get());
    runtime_assert_float_neq_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {d_ty, d_ty, d_ty}, false),
        llvm::Function::ExternalLinkage, "myp_assert_float_neq", module_.get());
    runtime_assert_null_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {p}, false),
        llvm::Function::ExternalLinkage, "myp_assert_null", module_.get());
    runtime_assert_not_null_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {p}, false),
        llvm::Function::ExternalLinkage, "myp_assert_not_null", module_.get());
}

// ---- __myp_* 测试 intrinsic 注册（declareRuntimeFunctions 内调用）----
void CodeGen::registerTestIntrinsics() {
    intrinsic_map_["__myp_assert"] = runtime_assert_;
    intrinsic_map_["__myp_assert_msg"] = runtime_assert_msg_;
    intrinsic_map_["__myp_test_set_msg"] = runtime_test_set_msg_;
    intrinsic_map_["__myp_assert_eq"] = runtime_assert_eq_;
    intrinsic_map_["__myp_assert_neq"] = runtime_assert_neq_;
    intrinsic_map_["__myp_assert_long_eq"] = runtime_assert_long_eq_;
    intrinsic_map_["__myp_assert_str_eq"] = runtime_assert_str_eq_;
    intrinsic_map_["__myp_assert_str_neq"] = runtime_assert_str_neq_;
    intrinsic_map_["__myp_test_report"] = runtime_test_report_;
    intrinsic_map_["__myp_test_fail_msg"] = runtime_test_fail_msg_;
    intrinsic_map_["__myp_test_summary"] = runtime_test_summary_;
    intrinsic_map_["__myp_assert_long_neq"] = runtime_assert_long_neq_;
    intrinsic_map_["__myp_assert_float_neq"] = runtime_assert_float_neq_;
    intrinsic_map_["__myp_assert_null"] = runtime_assert_null_;
    intrinsic_map_["__myp_assert_not_null"] = runtime_assert_not_null_;
}

// ---- 测试运行器：生成 main，逐个调用 @test 函数/action，异常隔离 + 汇总 ----
// `mypc --test`：sema 收集 @test 注解（顶层函数 / 类 action / 静态 action），
// codegen 在此生成运行器 main（用户 main 被跳过）。每个 @test 经
// emitTestRunnerProtectedCall 包裹：未捕获异常只 FAIL 该用例，不终止整套。
void CodeGen::generateTestRunner() {
    if (!current_tu_) return;
    auto& tu = *current_tu_;

    auto* i32t = llvm::Type::getInt32Ty(ctx_);
    auto* i8t = llvm::Type::getInt8Ty(ctx_);
    auto* pt = llvm::PointerType::get(ctx_, 0);
    auto* vt = llvm::Type::getVoidTy(ctx_);

    // Collect @test functions and @test actions
    struct TestAction {
        std::string class_name;
        std::string action_name;
        bool is_static;
    };
    std::vector<TestAction> test_actions;
    std::vector<std::string> test_functions;

    for (auto& cls : tu.classes) {
        for (auto& action : cls.actions) {
            if (action.has_test) {
                test_actions.push_back({cls.name, action.name, false});
            }
        }
        for (auto& action : cls.static_actions) {
            if (action.has_test) {
                test_actions.push_back({cls.name, action.name, true});
            }
        }
    }
    for (auto& f : tu.functions) {
        if (f.has_test) {
            test_functions.push_back(f.name);
        }
    }

    // If nothing to test, generate a minimal main
    auto* ft = llvm::FunctionType::get(i32t, {}, false);
    auto* main_func = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "main", module_.get());
    auto* bb = llvm::BasicBlock::Create(ctx_, "entry", main_func);
    builder_.SetInsertPoint(bb);
    current_function_ = main_func;

    // Call init mappings first
    if (init_func_) {
        builder_.CreateCall(init_func_, {});
    }

    llvm::Function* printf_fn = module_->getFunction("printf");
    if (!printf_fn) {
        auto* printf_ft = llvm::FunctionType::get(i32t, {pt}, true);
        printf_fn = llvm::Function::Create(printf_ft, llvm::Function::ExternalLinkage, "printf", module_.get());
    }

    // Print header
    auto* fmt_header = builder_.CreateGlobalStringPtr("=== MYP Test Runner ===\n");
    builder_.CreateCall(printf_fn, {fmt_header});

    // Call each @test function (exception-protected: an uncaught throw in one
    // test FAILs it and continues, rather than terminating the whole runner).
    for (auto& fname : test_functions) {
        auto* func = module_->getFunction(fname);
        if (!func) continue;
        auto* fmt = builder_.CreateGlobalStringPtr(("  RUN: " + fname + "\n").c_str());
        builder_.CreateCall(printf_fn, {fmt});
        emitTestRunnerProtectedCall(printf_fn, fname, [&] {
            builder_.CreateCall(func, {});
        });
    }

    // For each @test action, create instance and call
    for (auto& ta : test_actions) {
        std::string fn_name = ta.class_name + "_" + ta.action_name;
        auto* func = module_->getFunction(fn_name);
        if (!func) continue;

        auto* fmt = builder_.CreateGlobalStringPtr(("  RUN: " + ta.class_name + "." + ta.action_name + "\n").c_str());
        builder_.CreateCall(printf_fn, {fmt});

        emitTestRunnerProtectedCall(printf_fn, ta.class_name + "." + ta.action_name, [&] {
            if (ta.is_static) {
                builder_.CreateCall(func, {llvm::ConstantPointerNull::get(pt)});
            } else {
                // Allocate instance
                auto* cls_struct = class_structs_[ta.class_name];
                if (!cls_struct) return;
                auto* instance = builder_.CreateCall(runtime_alloc_, {
                    llvm::ConstantExpr::getSizeOf(cls_struct)
                });
                // Zero-init
                builder_.CreateMemSet(instance, llvm::ConstantInt::get(i8t, 0),
                    llvm::ConstantExpr::getSizeOf(cls_struct), llvm::MaybeAlign(1));
                // Call init mapping to register event handlers
                auto* alloc_init = module_->getFunction("__myp_alloc_init_" + ta.class_name);
                if (alloc_init) {
                    builder_.CreateCall(alloc_init, {instance});
                }
                builder_.CreateCall(func, {instance});
            }
        });
    }

    // Print summary
    auto* fmt_done = builder_.CreateGlobalStringPtr("=== MYP Tests Complete ===\n");
    builder_.CreateCall(printf_fn, {fmt_done});

    // Print assertion totals and return non-zero exit code on failure
    // (myp_test_summary returns 1 if any assertion failed). Previously the
    // runner always returned 0, so a failing test suite was indistinguishable
    // from a green one in scripts/CI.
    auto* summary = module_->getFunction("myp_test_summary");
    llvm::Value* exit_code = llvm::ConstantInt::get(i32t, 0);
    if (summary) {
        int n_tests = (int)test_functions.size() + (int)test_actions.size();
        exit_code = builder_.CreateCall(summary->getFunctionType(), summary,
            {llvm::ConstantInt::get(i32t, n_tests)});
    }
    builder_.CreateRet(exit_code);
    current_function_ = nullptr;
}

// 每个 @test 的异常隔离：push handler + setjmp；非零 setjmp = @test body 抛出的
// 未捕获异常 → 打 FAIL、计入失败、pop handler、继续下一用例。
void CodeGen::emitTestRunnerProtectedCall(llvm::Function* printf_fn,
                                          const std::string& label,
                                          const std::function<void()>& call_body) {
    auto* func = builder_.GetInsertBlock()->getParent();
    auto* i32t = llvm::Type::getInt32Ty(ctx_);
    auto* ptr_ty = llvm::PointerType::get(ctx_, 0);

    // Per-test jmp_buf in the entry block so it stays live across the longjmp.
    auto* jb = createEntryBlockAlloca(func, jmp_buf_type_, "test_jmpbuf");
    auto* jb_ptr = builder_.CreateBitCast(jb, ptr_ty);

    // Register this test's handler, then setjmp. A non-zero setjmp result means
    // an exception propagated out of the @test body (uncaught) — mark it FAIL
    // and continue to the next test instead of terminating the whole suite.
    builder_.CreateCall(runtime_exception_push_->getFunctionType(),
                        runtime_exception_push_, {jb_ptr});
    auto* result = builder_.CreateCall(runtime_setjmp_->getFunctionType(),
                                       runtime_setjmp_, {jb_ptr}, "setjmp_res");
    auto* is_err = builder_.CreateICmpNE(result, llvm::ConstantInt::get(i32t, 0));
    auto* ok_bb = llvm::BasicBlock::Create(ctx_, "test_call", func);
    auto* exc_bb = llvm::BasicBlock::Create(ctx_, "test_exc", func);
    auto* next_bb = llvm::BasicBlock::Create(ctx_, "test_next", func);
    builder_.CreateCondBr(is_err, exc_bb, ok_bb);

    // Normal path: run the test body, then pop the handler.
    builder_.SetInsertPoint(ok_bb);
    call_body();
    if (runtime_exception_pop_)
        builder_.CreateCall(runtime_exception_pop_->getFunctionType(),
                            runtime_exception_pop_, {});
    builder_.CreateBr(next_bb);

    // Exception path: report FAIL (uncaught exception), bump the fail count,
    // pop the handler, continue.
    builder_.SetInsertPoint(exc_bb);
    auto* fmt_fail = builder_.CreateGlobalStringPtr(
        ("  FAIL: " + label + " (uncaught exception)\n").c_str());
    builder_.CreateCall(printf_fn, {fmt_fail});
    if (runtime_test_fail_msg_) {
        auto* msg = builder_.CreateGlobalStringPtr(
            ("uncaught exception in " + label).c_str());
        builder_.CreateCall(runtime_test_fail_msg_->getFunctionType(),
                            runtime_test_fail_msg_, {msg});
    }
    if (runtime_exception_pop_)
        builder_.CreateCall(runtime_exception_pop_->getFunctionType(),
                            runtime_exception_pop_, {});
    builder_.CreateBr(next_bb);

    builder_.SetInsertPoint(next_bb);
}

} // namespace mylang
