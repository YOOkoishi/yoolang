%{

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "ast/ast.h"

int yylex();
void yyerror(std::unique_ptr<CompUnit> &ast, const char *s);

using namespace std;

template <typename T>
static std::unique_ptr<T> take_ptr(void *raw) {
  return std::unique_ptr<T>(static_cast<T *>(raw));
}

template <typename T>
static std::vector<std::unique_ptr<T>> adopt_unique_vec(std::vector<void *> *raw) {
  std::vector<std::unique_ptr<T>> result;
  if (raw == nullptr) {
    return result;
  }
  result.reserve(raw->size());
  for (void *ptr : *raw) {
    result.push_back(std::unique_ptr<T>(static_cast<T *>(ptr)));
  }
  delete raw;
  return result;
}

static std::vector<FuncParam> adopt_param_vec(std::vector<void *> *raw) {
  std::vector<FuncParam> result;
  if (raw == nullptr) {
    return result;
  }
  result.reserve(raw->size());
  for (void *param : *raw) {
    result.push_back(std::move(*static_cast<FuncParam *>(param)));
    delete static_cast<FuncParam *>(param);
  }
  delete raw;
  return result;
}

static BuiltinType to_builtin_type(int value) {
  return static_cast<BuiltinType>(value);
}

%}

%parse-param { std::unique_ptr<CompUnit> &ast }

%union {
  std::string *str_val;
  int int_val;
  float float_val;
  void *expr;
  void *stmt;
  void *decl_stmt;
  void *var_decl;
  void *func_def;
  void *comp_unit;
  void *init_val;
  void *param_val;
  std::vector<void *> *expr_vec;
  std::vector<void *> *stmt_vec;
  std::vector<void *> *var_decl_vec;
  std::vector<void *> *init_val_vec;
  std::vector<void *> *param_vec;
}

%token INT FLOAT VOID RETURN CONST IF ELSE WHILE BREAK CONTINUE
%token LE GE EQ NE LOR LAND
%token <str_val> IDENT
%token <int_val> INT_CONST
%token <float_val> FLOAT_CONST

%type <int_val> BType
%type <comp_unit> CompUnit
%type <func_def> FuncDef
%type <stmt> Stmt DeclStmt BlockStmt ExprStmt AssignStmt ReturnStmt WhileStmt BreakStmt ContinueStmt OpenStmt ClosedStmt SimpleStmt
%type <var_decl> VarDeclItem
%type <var_decl_vec> VarDeclList
%type <stmt_vec> StmtList
%type <expr> Expr LOrExp LAndExp EqExp RelExp AddExp MulExp UnaryExp PrimaryExp LVal Number
%type <expr_vec> ExprList DimList ParamDimList LValDimList
%type <init_val> InitVal
%type <init_val_vec> InitValList
%type <param_val> FuncParam
%type <param_vec> FuncParamList

%left LOR
%left LAND
%left EQ NE
%left '<' '>' LE GE
%left '+' '-'
%left '*' '/' '%'
%right UPLUS UMINUS UNOT

%%

CompUnit
  : DeclStmt {
      auto unit = new CompUnit();
      unit->global_decls.push_back(take_ptr<DeclStmt>($1));
      ast.reset(unit);
      $$ = unit;
    }
  | FuncDef {
      auto unit = new CompUnit();
      unit->functions.push_back(take_ptr<FuncDef>($1));
      ast.reset(unit);
      $$ = unit;
    }
  | CompUnit DeclStmt {
      auto unit = static_cast<CompUnit *>($1);
      unit->global_decls.push_back(take_ptr<DeclStmt>($2));
      $$ = unit;
    }
  | CompUnit FuncDef {
      auto unit = static_cast<CompUnit *>($1);
      unit->functions.push_back(take_ptr<FuncDef>($2));
      $$ = unit;
    }
  ;

FuncDef
  : BType IDENT '(' ')' BlockStmt {
      auto func = new FuncDef(to_builtin_type($1), *$2);
      delete $2;
      func->body = take_ptr<BlockStmt>($5);
      $$ = func;
    }
  | BType IDENT '(' FuncParamList ')' BlockStmt {
      auto func = new FuncDef(to_builtin_type($1), *$2);
      delete $2;
      func->params = adopt_param_vec($4);
      func->body = take_ptr<BlockStmt>($6);
      $$ = func;
    }
  ;

FuncParamList
  : FuncParam {
      auto params = new std::vector<void *>();
      params->push_back($1);
      $$ = params;
    }
  | FuncParamList ',' FuncParam {
      $1->push_back($3);
      $$ = $1;
    }
  ;

FuncParam
  : BType IDENT ParamDimList {
      auto param = new FuncParam();
      param->type = to_builtin_type($1);
      param->name = *$2;
      param->dimensions = adopt_unique_vec<Expr>($3);
      delete $2;
      $$ = param;
    }
  ;

ParamDimList
  : {
      $$ = new std::vector<void *>();
    }
  | ParamDimList '[' ']' {
      $1->push_back(nullptr);
      $$ = $1;
    }
  | ParamDimList '[' Expr ']' {
      $1->push_back($3);
      $$ = $1;
    }
  ;

BType
  : INT {
      $$ = static_cast<int>(BuiltinType::Int);
    }
  | FLOAT {
      $$ = static_cast<int>(BuiltinType::Float);
    }
  | VOID {
      $$ = static_cast<int>(BuiltinType::Void);
    }
  ;

BlockStmt
  : '{' '}' {
      $$ = new BlockStmt();
    }
  | '{' StmtList '}' {
      auto block = new BlockStmt();
      block->stmts = adopt_unique_vec<Stmt>($2);
      $$ = block;
    }
  ;

StmtList
  : Stmt {
      auto stmts = new std::vector<void *>();
      stmts->push_back($1);
      $$ = stmts;
    }
  | StmtList Stmt {
      $1->push_back($2);
      $$ = $1;
    }
  ;

Stmt
  : OpenStmt { $$ = $1; }
  | ClosedStmt { $$ = $1; }
  ;

ClosedStmt
  : SimpleStmt { $$ = $1; }
  | IF '(' Expr ')' ClosedStmt ELSE ClosedStmt {
      $$ = new IfStmt(take_ptr<Expr>($3), take_ptr<Stmt>($5), take_ptr<Stmt>($7));
    }
  ;

OpenStmt
  : IF '(' Expr ')' Stmt {
      $$ = new IfStmt(take_ptr<Expr>($3), take_ptr<Stmt>($5));
    }
  | IF '(' Expr ')' ClosedStmt ELSE OpenStmt {
      $$ = new IfStmt(take_ptr<Expr>($3), take_ptr<Stmt>($5), take_ptr<Stmt>($7));
    }
  ;

SimpleStmt
  : DeclStmt { $$ = $1; }
  | AssignStmt { $$ = $1; }
  | ExprStmt { $$ = $1; }
  | BlockStmt { $$ = $1; }
  | ReturnStmt { $$ = $1; }
  | WhileStmt { $$ = $1; }
  | BreakStmt { $$ = $1; }
  | ContinueStmt { $$ = $1; }
  ;

DeclStmt
  : CONST BType VarDeclList ';' {
      auto decl = new DeclStmt(true, to_builtin_type($2));
      decl->decls = adopt_unique_vec<VarDecl>($3);
      for (auto &item : decl->decls) {
        item->is_const = true;
        item->base_type = to_builtin_type($2);
      }
      $$ = decl;
    }
  | BType VarDeclList ';' {
      auto decl = new DeclStmt(false, to_builtin_type($1));
      decl->decls = adopt_unique_vec<VarDecl>($2);
      for (auto &item : decl->decls) {
        item->is_const = false;
        item->base_type = to_builtin_type($1);
      }
      $$ = decl;
    }
  ;

VarDeclList
  : VarDeclItem {
      auto decls = new std::vector<void *>();
      decls->push_back($1);
      $$ = decls;
    }
  | VarDeclList ',' VarDeclItem {
      $1->push_back($3);
      $$ = $1;
    }
  ;

VarDeclItem
  : IDENT {
      auto decl = new VarDecl(false, BuiltinType::Int, *$1);
      delete $1;
      $$ = decl;
    }
  | IDENT DimList {
      auto decl = new VarDecl(false, BuiltinType::Int, *$1);
      decl->dimensions = adopt_unique_vec<Expr>($2);
      delete $1;
      $$ = decl;
    }
  | IDENT '=' InitVal {
      auto decl = new VarDecl(false, BuiltinType::Int, *$1);
      decl->init = take_ptr<InitVal>($3);
      delete $1;
      $$ = decl;
    }
  | IDENT DimList '=' InitVal {
      auto decl = new VarDecl(false, BuiltinType::Int, *$1);
      decl->dimensions = adopt_unique_vec<Expr>($2);
      decl->init = take_ptr<InitVal>($4);
      delete $1;
      $$ = decl;
    }
  ;

DimList
  : '[' Expr ']' {
      auto dims = new std::vector<void *>();
      dims->push_back($2);
      $$ = dims;
    }
  | DimList '[' Expr ']' {
      $1->push_back($3);
      $$ = $1;
    }
  ;

InitVal
  : Expr {
      auto init = new InitVal();
      init->expr = take_ptr<Expr>($1);
      $$ = init;
    }
  | '{' '}' {
      $$ = new InitVal();
    }
  | '{' InitValList '}' {
      auto init = new InitVal();
      init->elems = adopt_unique_vec<InitVal>($2);
      $$ = init;
    }
  ;

InitValList
  : InitVal {
      auto values = new std::vector<void *>();
      values->push_back($1);
      $$ = values;
    }
  | InitValList ',' InitVal {
      $1->push_back($3);
      $$ = $1;
    }
  ;

AssignStmt
  : LVal '=' Expr ';' {
      $$ = new AssignStmt(take_ptr<LValExpr>($1), take_ptr<Expr>($3));
    }
  ;

ExprStmt
  : Expr ';' {
      $$ = new ExprStmt(take_ptr<Expr>($1));
    }
  | ';' {
      $$ = new ExprStmt();
    }
  ;

ReturnStmt
  : RETURN ';' {
      $$ = new ReturnStmt();
    }
  | RETURN Expr ';' {
      $$ = new ReturnStmt(take_ptr<Expr>($2));
    }
  ;

WhileStmt
  : WHILE '(' Expr ')' Stmt {
      $$ = new WhileStmt(take_ptr<Expr>($3), take_ptr<Stmt>($5));
    }
  ;

BreakStmt
  : BREAK ';' {
      $$ = new BreakStmt();
    }
  ;

ContinueStmt
  : CONTINUE ';' {
      $$ = new ContinueStmt();
    }
  ;

Expr
  : LOrExp { $$ = $1; }
  ;

LOrExp
  : LAndExp { $$ = $1; }
  | LOrExp LOR LAndExp {
      $$ = new BinaryExpr(BinaryOp::Or, take_ptr<Expr>($1), take_ptr<Expr>($3));
    }
  ;

LAndExp
  : EqExp { $$ = $1; }
  | LAndExp LAND EqExp {
      $$ = new BinaryExpr(BinaryOp::And, take_ptr<Expr>($1), take_ptr<Expr>($3));
    }
  ;

EqExp
  : RelExp { $$ = $1; }
  | EqExp EQ RelExp {
      $$ = new BinaryExpr(BinaryOp::Eq, take_ptr<Expr>($1), take_ptr<Expr>($3));
    }
  | EqExp NE RelExp {
      $$ = new BinaryExpr(BinaryOp::Ne, take_ptr<Expr>($1), take_ptr<Expr>($3));
    }
  ;

RelExp
  : AddExp { $$ = $1; }
  | RelExp '<' AddExp {
      $$ = new BinaryExpr(BinaryOp::Lt, take_ptr<Expr>($1), take_ptr<Expr>($3));
    }
  | RelExp '>' AddExp {
      $$ = new BinaryExpr(BinaryOp::Gt, take_ptr<Expr>($1), take_ptr<Expr>($3));
    }
  | RelExp LE AddExp {
      $$ = new BinaryExpr(BinaryOp::Le, take_ptr<Expr>($1), take_ptr<Expr>($3));
    }
  | RelExp GE AddExp {
      $$ = new BinaryExpr(BinaryOp::Ge, take_ptr<Expr>($1), take_ptr<Expr>($3));
    }
  ;

AddExp
  : MulExp { $$ = $1; }
  | AddExp '+' MulExp {
      $$ = new BinaryExpr(BinaryOp::Add, take_ptr<Expr>($1), take_ptr<Expr>($3));
    }
  | AddExp '-' MulExp {
      $$ = new BinaryExpr(BinaryOp::Sub, take_ptr<Expr>($1), take_ptr<Expr>($3));
    }
  ;

MulExp
  : UnaryExp { $$ = $1; }
  | MulExp '*' UnaryExp {
      $$ = new BinaryExpr(BinaryOp::Mul, take_ptr<Expr>($1), take_ptr<Expr>($3));
    }
  | MulExp '/' UnaryExp {
      $$ = new BinaryExpr(BinaryOp::Div, take_ptr<Expr>($1), take_ptr<Expr>($3));
    }
  | MulExp '%' UnaryExp {
      $$ = new BinaryExpr(BinaryOp::Mod, take_ptr<Expr>($1), take_ptr<Expr>($3));
    }
  ;

UnaryExp
  : PrimaryExp { $$ = $1; }
  | IDENT '(' ')' {
      auto call = new CallExpr(*$1);
      delete $1;
      $$ = call;
    }
  | IDENT '(' ExprList ')' {
      auto call = new CallExpr(*$1);
      call->args = adopt_unique_vec<Expr>($3);
      delete $1;
      $$ = call;
    }
  | '+' UnaryExp %prec UPLUS {
      $$ = new UnaryExpr(UnaryOp::Pos, take_ptr<Expr>($2));
    }
  | '-' UnaryExp %prec UMINUS {
      $$ = new UnaryExpr(UnaryOp::Neg, take_ptr<Expr>($2));
    }
  | '!' UnaryExp %prec UNOT {
      $$ = new UnaryExpr(UnaryOp::Not, take_ptr<Expr>($2));
    }
  ;

PrimaryExp
  : '(' Expr ')' {
      $$ = $2;
    }
  | LVal { $$ = $1; }
  | Number { $$ = $1; }
  ;

ExprList
  : Expr {
      auto args = new std::vector<void *>();
      args->push_back($1);
      $$ = args;
    }
  | ExprList ',' Expr {
      $1->push_back($3);
      $$ = $1;
    }
  ;

Number
  : INT_CONST {
      $$ = new IntLiteral($1);
    }
  | FLOAT_CONST {
      $$ = new FloatLiteral($1);
    }
  ;

LVal
  : IDENT {
      auto lval = new LValExpr(*$1);
      delete $1;
      $$ = lval;
    }
  | IDENT LValDimList {
      auto lval = new LValExpr(*$1);
      lval->indices = adopt_unique_vec<Expr>($2);
      delete $1;
      $$ = lval;
    }
  ;

LValDimList
  : '[' Expr ']' {
      auto dims = new std::vector<void *>();
      dims->push_back($2);
      $$ = dims;
    }
  | LValDimList '[' Expr ']' {
      $1->push_back($3);
      $$ = $1;
    }
  ;

%%

void yyerror(std::unique_ptr<CompUnit> &ast, const char *s) {
  (void)ast;
  cerr << "error: " << s << endl;
}
