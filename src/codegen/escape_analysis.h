// src/codegen/escape_analysis.h — 逃逸分析（第一版，2026-08-28）
//
// 判定函数内哪些 `let v = new T(...)` 的局部变量可**栈上分配**：
// v 在函数内只被"读字段/写字段"使用（MemberAccess 的 object 位置），从不出现在
// 任何"可能转移/共享引用"的位置（赋给其他变量、传参、存容器、返回、lambda、
// 方法调用接收者、并发/Gpu/Match/Try 等）→ 不逃逸 → 可栈上分配。
//
// 保守策略：宁漏勿错——任何不确定的用法都判逃逸。方法调用 v.method() 判逃逸
// （方法可能持有 v）。复杂语句（Gpu/Mapping/Match/Try/Throw/Await/Destructure/
// Nonlocal）里只要提及 v 即逃逸。
//
// 输出：可栈上分配的变量名集合。调用方（codegen）再按类型过滤（须为 class、
// 非 struct/interface、对象大小阈值内）。
#pragma once
#include <set>
#include <string>

namespace mylang {

struct BlockStmt;
struct Stmt;
struct Expr;

// 分析函数体 body，返回可栈上分配的局部变量名集合。
std::set<std::string> analyzeEscapeStackVars(const BlockStmt* body);

}  // namespace mylang
