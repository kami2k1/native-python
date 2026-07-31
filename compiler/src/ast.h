#pragma once
#include "token.h"

#include <memory>
#include <string>
#include <vector>

namespace kami {

// ---------------- expressions ----------------
enum class ExprKind {
    IntLit, FloatLit, StrLit, BoolLit, NoneLit,
    Name, Binary, Unary, BoolOp,            // BoolOp: and/or (short-circuit)
    Call, MethodCall, Index, Attr, ListLit, MapLit,
};

// Name/Call resolution (filled by sema)
enum class Res { Unresolved, Local, Global, BuiltinFunc, UserFunc };

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Expr {
    ExprKind kind;
    int line = 0;

    int64_t ival = 0;          // IntLit / BoolLit
    double fval = 0.0;         // FloatLit
    std::string sval;          // StrLit / Name / Attr+MethodCall name

    int op = 0;                // Binary (KamiBinOp) / Unary (KamiUnOp) / BoolOp (0=and,1=or)
    ExprPtr a, b;              // operands / call base / index base+index
    std::vector<ExprPtr> args; // Call args / ListLit items
    std::vector<std::pair<ExprPtr, ExprPtr>> pairs; // MapLit

    // sema annotations
    Res res = Res::Unresolved;
    int64_t res_idx = 0;       // local slot / global index / builtin id / func index
};

// ---------------- statements ----------------
enum class StmtKind {
    ExprStmt, Assign, IndexAssign, If, While, For,
    FuncDef, Return, Break, Continue, Pass, Import,
};

struct Stmt;
using StmtPtr = std::unique_ptr<Stmt>;

struct Stmt {
    StmtKind kind;
    int line = 0;

    ExprPtr e1, e2, e3; // Assign: e1=value; IndexAssign: e1=base,e2=index,e3=value
                        // If/While: e1=cond; For: e1=iter; Return: e1=value(opt); ExprStmt: e1
    std::string name;   // Assign target name / FuncDef name / For var / Import module
    std::vector<std::string> params;   // FuncDef
    std::vector<StmtPtr> body, orelse; // If/While/For/FuncDef

    // sema annotations
    Res target_res = Res::Unresolved; // Assign/For variable
    int64_t target_idx = 0;
    int nlocals = 0;                  // FuncDef
    int64_t global_idx = -1;          // FuncDef global slot
    int func_index = -1;              // FuncDef index into module function table
};

struct Module {
    std::vector<StmtPtr> body;         // module-level statements (FuncDefs included in place)
    std::vector<Stmt*> functions;      // all FuncDefs (borrowed pointers)
    int64_t nglobals = 0;
};

// S-expression dump for tests/debugging.
std::string dump_expr(const Expr* e);
std::string dump_stmt(const Stmt* s, int indent = 0);
std::string dump_module(const Module& m);

} // namespace kami
