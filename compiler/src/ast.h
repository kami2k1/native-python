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
    IfExp,      // a if cond else b:  a=then, b=cond, c=else
    Slice,      // a=base; args[0..2] = start/stop/step (may be null)
    ListComp,   // a=element expr, b=iter, c=cond(opt), params=target names
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
    ExprPtr a, b, c;           // operands
    std::vector<ExprPtr> args; // Call args / ListLit items / Slice parts
    std::vector<std::pair<std::string, ExprPtr>> kwargs; // Call keyword args
    std::vector<std::pair<ExprPtr, ExprPtr>> pairs;      // MapLit
    std::vector<std::string> params; // ListComp target names

    // sema annotations
    Res res = Res::Unresolved;
    int64_t res_idx = 0;       // local slot / global index / builtin id / func index
    std::vector<int64_t> comp_tidx; // ListComp resolved target slots
    std::vector<int> comp_tkind;    // 1=local 2=global
};

// ---------------- statements ----------------
enum class StmtKind {
    ExprStmt, Assign, IndexAssign, AttrAssign, MultiAssign,
    If, While, For, FuncDef, ClassDef, Return, Break, Continue, Pass,
    Import, FromImport, Global, Try, Raise,
};

struct Stmt;
using StmtPtr = std::unique_ptr<Stmt>;

struct ExceptClause {
    std::string as_name;   // "" if absent
    std::vector<StmtPtr> body;
    int as_kind = 0;       // 1=local 2=global (sema)
    int64_t as_idx = 0;
};

struct Stmt {
    StmtKind kind;
    int line = 0;

    ExprPtr e1, e2, e3; // Assign: e1=value; IndexAssign: e1=base,e2=index,e3=value
                        // AttrAssign: e1=base, name=attr, e3=value
                        // If/While: e1=cond; For: e1=iter; Return: e1; Raise: e1(opt)
    std::string name;   // Assign target / FuncDef / ClassDef / For var / Import module
    std::string alias;  // import ... as alias / ClassDef base name
    std::vector<std::string> params;            // FuncDef params / For multi-targets / Global names
    std::vector<ExprPtr> defaults;              // FuncDef default values (aligned to params tail)
    std::vector<std::pair<std::string, std::string>> import_names; // FromImport (name, alias)
    std::vector<StmtPtr> body, orelse, final_body; // blocks; Try: body/orelse(else)/finally
    std::vector<ExceptClause> handlers;            // Try
    std::vector<ExprPtr> targets;                  // MultiAssign target exprs
    std::vector<ExprPtr> values;                   // MultiAssign RHS exprs (1 = unpack)

    // sema annotations
    Res target_res = Res::Unresolved; // Assign/For variable
    int64_t target_idx = 0;
    std::vector<int> multi_tkind;     // For multi-targets: 1=local 2=global
    std::vector<int64_t> multi_tidx;
    int nlocals = 0;                  // FuncDef
    int64_t global_idx = -1;          // FuncDef/ClassDef global slot
    int func_index = -1;              // FuncDef index into module function table
    int raise_mode = 0;               // Raise: 0=expr,1=bare,2=typed (name in 'name', arg in e1)
};

struct Module {
    std::vector<StmtPtr> body;         // module-level statements
    std::vector<Stmt*> functions;      // all FuncDefs incl. methods (borrowed)
    std::vector<Stmt*> classes;        // all ClassDefs (borrowed)
    int64_t nglobals = 0;
};

// S-expression dump for tests/debugging.
std::string dump_expr(const Expr* e);
std::string dump_stmt(const Stmt* s, int indent = 0);
std::string dump_module(const Module& m);

} // namespace kami
