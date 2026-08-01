#pragma once
#include "token.h"

#include <map>
#include <memory>
#include <set>
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
    SetLit,     // args = elements
    Lambda,     // params = arg names, a = body expr
    Closure,    // res_idx = function index; capture sources in comp_t* vectors
    Starred,    // *a inside a list/call display; a = inner
    SetComp,    // element a + clauses
    MapComp,    // pairs[0] = (key,val) + clauses
    CCall,      // direct C-ABI call: sval = symbol, csig = signature, args = params
    CallStar,   // call with *args/**kwargs at the call site: a = callee,
                // args = positional (may contain Starred), pairs = keyword
                // entries ((StrLit name, value) or (null, **expr))
    Yield,      // yield [a] / yield from a (op: 0 = yield, 1 = yield from);
                // evaluates to the value passed to send() (None for next())
};

struct Expr;

struct CompClause {
    std::vector<std::string> targets;      // loop variables
    std::unique_ptr<Expr> iter;
    std::vector<std::unique_ptr<Expr>> conds;
    std::vector<int> tkind;                // sema: 1=local 2=global
    std::vector<int64_t> tidx;
};

// Name/Call resolution (filled by sema)
enum class Res { Unresolved, Local, Global, BuiltinFunc, UserFunc, Capture };

// Static types inferred by the type-inference pass (typeinf.cpp). A small,
// sound lattice: TY_BOT (no information yet) < concrete type < TY_ANY.
enum KType : uint8_t {
    TY_BOT = 0,
    TY_INT,
    TY_FLOAT,
    TY_BOOL,
    TY_STR,
    TY_NONE,
    TY_ANY,
};

using ExprPtr = std::unique_ptr<Expr>;

struct Expr {
    ExprKind kind;
    int line = 0;

    int64_t ival = 0;          // IntLit / BoolLit
    double fval = 0.0;         // FloatLit
    std::string sval;          // StrLit / Name / Attr+MethodCall name

    std::string csig;          // CCall: C-ABI signature, e.g. "dd" = double(double)

    int op = 0;                // Binary (KamiBinOp) / Unary (KamiUnOp) / BoolOp (0=and,1=or)
    ExprPtr a, b, c;           // operands
    std::vector<ExprPtr> args; // Call args / ListLit items / Slice parts
    std::vector<std::pair<std::string, ExprPtr>> kwargs; // Call keyword args
    std::vector<std::pair<ExprPtr, ExprPtr>> pairs;      // MapLit
    std::vector<std::string> params; // ListComp target names / Lambda params
    std::vector<CompClause> clauses; // ListComp/SetComp/MapComp
    // Lambda only: *args/**kwargs names ("" if absent), keyword-only start
    // index (-1 = none) and default values (aligned to the params tail).
    std::string vararg, kwarg;
    int kwonly = -1;

    // sema annotations
    Res res = Res::Unresolved;
    int64_t res_idx = 0;       // local slot / global index / builtin id / func index
    std::vector<int64_t> comp_tidx; // ListComp resolved target slots
    std::vector<int> comp_tkind;    // 1=local 2=global
    uint8_t sty = TY_ANY;      // static type (typeinf.cpp)
    bool nonneg = false;       // int expr provably >= 0 (typeinf.cpp)
};

// ---------------- statements ----------------
enum class StmtKind {
    ExprStmt, Assign, IndexAssign, AttrAssign, MultiAssign,
    If, While, For, FuncDef, ClassDef, Return, Break, Continue, Pass,
    Import, FromImport, Global, Try, Raise, Del, With,
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
    // FuncDef: `*args` / `**kwargs` parameter names ("" when absent) and the
    // index in `params` where keyword-only parameters begin (-1 = none).
    // The vararg tuple and kwargs dict occupy local slots params.size() and
    // params.size()+1 (when present), directly after the fixed parameters.
    std::string vararg, kwarg;
    int kwonly = -1;
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
    int raise_mode = 0;
    bool relative = false; // FromImport: leading-dot relative import
    bool star = false;     // FromImport: `from X import *`
    bool pyext = false;    // Import/FromImport resolved to a CPython extension
    std::vector<ExprPtr> decorators;  // FuncDef/ClassDef decorator expressions
    bool is_closure = false;          // FuncDef compiled with a %captures param
    int ncaptures = 0;               // Raise: 0=expr,1=bare,2=typed (name in 'name', arg in e1)
    // Assign `s = s + x` where sema's alias analysis proved `s` has no live
    // aliases: codegen emits an in-place buffer append (kami_str_iadd).
    bool str_iadd = false;
    // Assign overwriting an unaliased local holding a fresh container: the
    // old object is dead — recycle its memory (kami_free_hint).
    bool free_hint = false;
    uint8_t sty = TY_ANY; // For: static type of the loop variable (typeinf.cpp)
    // FuncDef: body contains `yield` — calls build a generator object instead
    // of running the body (parser sets it, codegen/runtime consume it).
    bool is_generator = false;
    // ClassDef: (transitively) derives from a builtin exception. Instances
    // without __init__ accept any message argument and stringify as
    // "Name: message" so raise/str work like CPython's Exception.
    bool is_exception = false;
    // ClassDef: the base class is a bridged CPython class (KT_PYOBJ). The
    // class is created at runtime with type(name, (base,), {...}) and the
    // compiled methods are installed as bound-able callables; target_idx
    // holds the global slot of the base value.
    bool pyobj_base = false;
};

// A C function the compiled program calls directly (from a C extension mapping
// or from ctypes/cffi). Collected by sema so codegen can `declare` it once.
struct NativeDecl {
    std::string symbol;
    std::string csig; // see cext.h: first char = return type, rest = parameters
};

// Per-function results of the type-inference pass (parallel to
// Module::functions). Filled by infer_types() in typeinf.cpp.
struct FuncTypeInfo {
    std::vector<uint8_t> locals; // static type per local slot (KType)
    std::vector<uint8_t> params; // join of argument types over all call sites
    uint8_t ret = TY_BOT;        // join of all return expression types
    bool escapes = true;         // name used as a value → dynamic calls possible
    bool native_ok = false;      // monomorphized @n_<alias> specialization emitted

    // Speculative monomorphization for functions whose name escapes (e.g.
    // passed to a benchmark harness): the body is typed under an assumed
    // parameter signature (int unless call sites say float); u_<alias> gets a
    // runtime tag guard that dispatches to @n_<alias> when the actual
    // arguments match, falling back to the generic boxed body otherwise.
    bool guarded = false;
    std::vector<uint8_t> spec_locals, spec_params;
    uint8_t spec_ret = TY_BOT;
    // Non-negativity lattice (optimistic true, cleared by any possibly-
    // negative assignment). Lets codegen emit plain sdiv/srem for // and %
    // by positive constants — the exact codegen Go gets for its % operator.
    std::vector<uint8_t> locals_nn, spec_locals_nn;
};

// Effective (native-callable) view of a function's signature.
inline bool eff_native(const FuncTypeInfo& f) { return f.native_ok || f.guarded; }
inline const std::vector<uint8_t>& eff_params(const FuncTypeInfo& f) {
    return f.guarded ? f.spec_params : f.params;
}
inline uint8_t eff_ret(const FuncTypeInfo& f) { return f.guarded ? f.spec_ret : f.ret; }

struct Module {
    std::vector<StmtPtr> body;         // module-level statements
    std::vector<Stmt*> functions;      // all FuncDefs incl. methods (borrowed)
    std::vector<Stmt*> classes;        // all ClassDefs (borrowed)
    std::vector<StmtPtr> synth;        // synthetic functions (lambdas), owned
    int64_t nglobals = 0;
    // Local .py modules whose source was bundled into this module by the
    // driver ("import utils" → utils.py compiled in). Their top-level code is
    // spliced before the main body; sema maps "utils.x" to plain "x".
    std::set<std::string> user_modules;
    // Subset of user_modules that came from the Python installation on this
    // machine rather than the project or the shipped pylib.
    std::set<std::string> system_modules;
    // Symbol prefix applied to a bundled module's top-level names. Library
    // modules (pylib/ or the system Python) are mangled ("re" → std_re_search)
    // so a stdlib helper can never be clobbered by a user global of the same
    // name; project .py files keep the flat namespace.
    std::map<std::string, std::string> module_prefix;
    // Package re-exports: "concurrent.futures.Future" → the mangled global it
    // really refers to ("std_concurrent_futures__base_Future"). Filled by the
    // bundler from top-level (and hoisted lazy) from-imports of packages.
    std::map<std::string, std::string> module_exports;
    // Library module names that were importable — used to print a useful list
    // when an import cannot be resolved.
    std::set<std::string> stdlib_available;
    std::string source_path; // input file path (for __file__)

    // C-ABI bindings discovered while analysing this module.
    std::vector<NativeDecl> natives;   // unique (symbol, signature) pairs
    std::set<std::string> link_libs;   // extra -l<name> flags for the linker

    // Type-inference results (parallel to `functions`); see typeinf.cpp.
    std::vector<FuncTypeInfo> ftypes;
};

// Type inference & monomorphization analysis (typeinf.cpp). Runs after
// analyze(); stamps Expr::sty / Stmt::sty and fills Module::ftypes.
void infer_types(Module& m);

// Re-stamps a single function body's Expr::sty under the speculative (spec =
// true) or normal (spec = false) type tables. Codegen uses this to emit the
// guarded @n_ specialization and then restore the stamps for the boxed body.
void stamp_function(Module& m, size_t func_index, bool spec);

// libm symbol for a builtin math id usable in native (monomorphized) bodies,
// or null. Shared between typeinf.cpp and codegen.cpp.
const char* native_math_symbol(int64_t id, int* arity);

// S-expression dump for tests/debugging.
std::string dump_expr(const Expr* e);
std::string dump_stmt(const Stmt* s, int indent = 0);
std::string dump_module(const Module& m);

} // namespace kami
