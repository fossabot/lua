/*
** $Id: lparser.c,v 1.59 2000/02/14 16:51:08 roberto Exp roberto $
** LL(1) Parser and code generator for Lua
** See Copyright Notice in lua.h
*/


#include <stdio.h>
#include <string.h>

#define LUA_REENTRANT

#include "lcode.h"
#include "ldo.h"
#include "lfunc.h"
#include "llex.h"
#include "lmem.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lparser.h"
#include "lstate.h"
#include "lstring.h"


/*
** Expression List descriptor:
** tells number of expressions in the list,
** and, if last expression is open (a function call),
** where is the call pc index.
*/
typedef struct listdesc {
  int n;
  int pc;  /* 0 if last expression is closed */
} listdesc;


/*
** Constructors descriptor:
** `n' indicates number of elements, and `k' signals whether
** it is a list constructor (k = 0) or a record constructor (k = 1)
** or empty (k = ';' or '}')
*/
typedef struct constdesc {
  int n;
  int k;
} constdesc;




/*
** prototypes for recursive non-terminal functions
*/
static void body (LexState *ls, int needself, int line);
static void chunk (LexState *ls);
static void constructor (LexState *ls);
static void expr (LexState *ls, vardesc *v);
static void exp1 (LexState *ls);


static void next (LexState *ls) {
  ls->token = luaX_lex(ls);
}


static void luaY_error (LexState *ls, const char *msg) {
  luaX_error(ls, msg, ls->token);
}


static void error_expected (LexState *ls, int token) {
  char buff[100], t[TOKEN_LEN];
  luaX_token2str(token, t);
  sprintf(buff, "`%.20s' expected", t);
  luaY_error(ls, buff);
}


static void error_unexpected (LexState *ls) {
  luaY_error(ls, "unexpected token");
}


static void error_unmatched (LexState *ls, int what, int who, int where) {
  if (where == ls->linenumber)
    error_expected(ls, what);
  else {
    char buff[100];
    char t_what[TOKEN_LEN], t_who[TOKEN_LEN];
    luaX_token2str(what, t_what);
    luaX_token2str(who, t_who);
    sprintf(buff, "`%.20s' expected (to close `%.20s' at line %d)",
            t_what, t_who, where);
    luaY_error(ls, buff);
  }
}

static void check (LexState *ls, int c) {
  if (ls->token != c)
    error_expected(ls, c);
  next(ls);
}


static int optional (LexState *ls, int c) {
  if (ls->token == c) {
    next(ls);
    return 1;
  }
  else return 0;
}


static void checklimit (LexState *ls, int val, int limit, const char *msg) {
  if (val > limit) {
    char buff[100];
    sprintf(buff, "too many %.50s (limit=%d)", msg, limit);
    luaY_error(ls, buff);
  }
}



static void deltastack (LexState *ls, int delta) {
  FuncState *fs = ls->fs;
  fs->stacksize += delta;
  if (delta > 0 && fs->stacksize > fs->f->maxstacksize) {
    checklimit(ls, fs->stacksize, MAXSTACK, "temporaries or local variables");
    fs->f->maxstacksize = fs->stacksize;
  }
}


static int aux_code (LexState *ls, OpCode op, Instruction i, int delta) {
  deltastack(ls, delta);
  return luaK_code(ls, SET_OPCODE(i, op));
}


static int code_0 (LexState *ls, OpCode op, int delta) {
  return aux_code(ls, op, 0, delta);
}



static int code_U (LexState *ls, OpCode op, int u, int delta) {
  Instruction i = SETARG_U(0, u);
  return aux_code(ls, op, i, delta);
}


static int code_S (LexState *ls, OpCode op, int s, int delta) {
  Instruction i = SETARG_S(0, s);
  return aux_code(ls, op, i, delta);
}


static int code_AB (LexState *ls, OpCode op, int a, int b, int delta) {
  Instruction i = SETARG_A(0, a);
  i = SETARG_B(i, b);
  return aux_code(ls, op, i, delta);
}


static void check_debugline (LexState *ls) {
  if (ls->L->debug && ls->linenumber != ls->fs->lastsetline) {
    code_U(ls, SETLINE, ls->linenumber, 0);
    ls->fs->lastsetline = ls->linenumber;
  }
}


static void check_match (LexState *ls, int what, int who, int where) {
  if (ls->token != what)
    error_unmatched(ls, what, who, where);
  check_debugline(ls);  /* to `mark' the `what' */
  next(ls);
}


static void code_kstr (LexState *ls, int c) {
  code_U(ls, PUSHSTRING, c, 1);
}


static void assertglobal (LexState *ls, int index) {
  luaS_assertglobal(ls->L, ls->fs->f->kstr[index]);
}


static int string_constant (LexState *ls, FuncState *fs, TaggedString *s) {
  TProtoFunc *f = fs->f;
  int c = s->constindex;
  if (c >= f->nkstr || f->kstr[c] != s) {
    luaM_growvector(ls->L, f->kstr, f->nkstr, 1, TaggedString *,
                    constantEM, MAXARG_U);
    c = f->nkstr++;
    f->kstr[c] = s;
    s->constindex = c;  /* hint for next time */
  }
  return c;
}


static void code_string (LexState *ls, TaggedString *s) {
  code_kstr(ls, string_constant(ls, ls->fs, s));
}


#define LIM 20
static int real_constant (LexState *ls, real r) {
  /* check whether `r' has appeared within the last LIM entries */
  TProtoFunc *f = ls->fs->f;
  int c = f->nknum;
  int lim = c < LIM ? 0 : c-LIM;
  while (--c >= lim)
    if (f->knum[c] == r) return c;
  /* not found; create a new entry */
  luaM_growvector(ls->L, f->knum, f->nknum, 1, real, constantEM, MAXARG_U);
  c = f->nknum++;
  f->knum[c] = r;
  return c;
}


static void code_number (LexState *ls, real f) {
  if (f <= (real)MAXARG_S && (int)f == f)
    code_S(ls, PUSHINT, (int)f, 1);  /* f has a short integer value */
  else
    code_U(ls, PUSHNUM, real_constant(ls, f), 1);
}


static int checkname (LexState *ls) {
  int sc;
  if (ls->token != NAME)
    luaY_error(ls, "<name> expected");
  sc = string_constant(ls, ls->fs, ls->seminfo.ts);
  next(ls);
  return sc;
}


static TaggedString *str_checkname (LexState *ls) {
  int i = checkname(ls);  /* this call may realloc `f->consts' */
  return ls->fs->f->kstr[i];
}


static void luaI_registerlocalvar (LexState *ls, TaggedString *varname,
                                   int line) {
  FuncState *fs = ls->fs;
  if (fs->nvars != -1) {  /* debug information? */
    TProtoFunc *f = fs->f;
    luaM_growvector(ls->L, f->locvars, fs->nvars, 1, LocVar, "", MAX_INT);
    f->locvars[fs->nvars].varname = varname;
    f->locvars[fs->nvars].line = line;
    fs->nvars++;
  }
}


static void luaI_unregisterlocalvar (LexState *ls, int line) {
  luaI_registerlocalvar(ls, NULL, line);
}


static void store_localvar (LexState *ls, TaggedString *name, int n) {
  FuncState *fs = ls->fs;
  checklimit(ls, fs->nlocalvar+n+1, MAXLOCALS, "local variables");
  fs->localvar[fs->nlocalvar+n] = name;
}


static void adjustlocalvars (LexState *ls, int nvars, int line) {
  FuncState *fs = ls->fs;
  int i;
  fs->nlocalvar += nvars;
  for (i=fs->nlocalvar-nvars; i<fs->nlocalvar; i++)
    luaI_registerlocalvar(ls, fs->localvar[i], line);
}


static void add_localvar (LexState *ls, TaggedString *name) {
  store_localvar(ls, name, 0);
  adjustlocalvars(ls, 1, 0);
}


static int aux_localname (FuncState *fs, TaggedString *n) {
  int i;
  for (i=fs->nlocalvar-1; i >= 0; i--)
    if (n == fs->localvar[i]) return i;  /* local var index */
  return -1;  /* not found */
}


static void singlevar (LexState *ls, TaggedString *n, vardesc *var, int prev) {
  FuncState *fs = prev ? ls->fs->prev : ls->fs;
  int i = aux_localname(fs, n);
  if (i >= 0) {  /* local value? */
    var->k = VLOCAL;
    var->info = i;
  }
  else {
    FuncState *level = fs;
    while ((level = level->prev) != NULL)  /* check shadowing */
      if (aux_localname(level, n) >= 0)
        luaX_syntaxerror(ls, "cannot access a variable in outer scope", n->str);
    var->k = VGLOBAL;
    var->info = string_constant(ls, fs, n);
  }
}


static int indexupvalue (LexState *ls, TaggedString *n) {
  FuncState *fs = ls->fs;
  vardesc v;
  int i;
  singlevar(ls, n, &v, 1);
  for (i=0; i<fs->nupvalues; i++) {
    if (fs->upvalues[i].k == v.k && fs->upvalues[i].info == v.info)
      return i;
  }
  /* new one */
  ++(fs->nupvalues);
  checklimit(ls, fs->nupvalues, MAXUPVALUES, "upvalues");
  fs->upvalues[i] = v;  /* i = fs->nupvalues - 1 */
  return i;
}


static void pushupvalue (LexState *ls, TaggedString *n) {
  if (ls->fs->prev == NULL)
    luaX_syntaxerror(ls, "cannot access upvalue in main", n->str);
  if (aux_localname(ls->fs, n) >= 0)
    luaX_syntaxerror(ls, "cannot access an upvalue in current scope", n->str);
  code_U(ls, PUSHUPVALUE, indexupvalue(ls, n), 1);
}


static void adjuststack (LexState *ls, int n) {
  if (n > 0)
    code_U(ls, POP, n, -n);
  else if (n < 0)
    code_U(ls, PUSHNIL, (-n)-1, -n);
}


static void close_call (LexState *ls, int pc, int nresults) {
  if (pc > 0) {  /* expression is an open function call? */
    Instruction *i = &ls->fs->f->code[pc];
    *i = SETARG_B(*i, nresults);  /* set nresults */
    if (nresults != MULT_RET)
      deltastack(ls, nresults);  /* push results */
  }
}


static void adjust_mult_assign (LexState *ls, int nvars, listdesc *d) {
  int diff = d->n - nvars;
  if (d->pc == 0) {  /* list is closed */
    /* push or pop eventual difference between list lengths */
    adjuststack(ls, diff);
  }
  else {  /* must correct function call */
    diff--;  /* do not count function call itself */
    if (diff <= 0) {  /* more variables than values? */
      /* function call must provide extra values */
      close_call(ls, d->pc, -diff);
    }
    else {  /* more values than variables */
      close_call(ls, d->pc, 0);  /* call should provide no value */
      adjuststack(ls, diff);  /* pop eventual extra values */
    }
  }
}


static void code_args (LexState *ls, int nparams, int dots) {
  FuncState *fs = ls->fs;
  adjustlocalvars(ls, nparams, 0);
  checklimit(ls, fs->nlocalvar, MAXPARAMS, "parameters");
  nparams = fs->nlocalvar;  /* `self' could be there already */
  fs->f->numparams = nparams;
  fs->f->is_vararg = dots;
  if (!dots)
    deltastack(ls, nparams);
  else {
    deltastack(ls, nparams+1);
    add_localvar(ls, luaS_newfixed(ls->L, "arg"));
  }
}


static int getvarname (LexState *ls, vardesc *var) {
  switch (var->k) {
    case VGLOBAL:
      return var->info;
    case VLOCAL:
      return string_constant(ls, ls->fs, ls->fs->localvar[var->info]);
      break;
    default:
      error_unexpected(ls);  /* there is no `var name' */
      return 0;  /* to avoid warnings */
  }
}


static void close_exp (LexState *ls, vardesc *var) {
  switch (var->k) {
    case VLOCAL:
      code_U(ls, PUSHLOCAL, var->info, 1);
      break;
    case VGLOBAL:
      code_U(ls, GETGLOBAL, var->info, 1);
      assertglobal(ls, var->info);  /* make sure that there is a global */
      break;
    case VINDEXED:
      code_0(ls, GETTABLE, -1);
      break;
    case VEXP:
      close_call(ls, var->info, 1);  /* call must return 1 value */
      break;
  }
  var->k = VEXP;
  var->info = 0;  /* now this is a closed expression */
}


static void storevar (LexState *ls, const vardesc *var) {
  switch (var->k) {
    case VLOCAL:
      code_U(ls, SETLOCAL, var->info, -1);
      break;
    case VGLOBAL:
      code_U(ls, SETGLOBAL, var->info, -1);
      assertglobal(ls, var->info);  /* make sure that there is a global */
      break;
    case VINDEXED:
      code_0(ls, SETTABLEPOP, -3);
      break;
    default:
      LUA_INTERNALERROR(ls->L, "invalid var kind to store");
  }
}


static void func_onstack (LexState *ls, FuncState *func) {
  TProtoFunc *f = ls->fs->f;
  int i;
  for (i=0; i<func->nupvalues; i++)
    close_exp(ls, &func->upvalues[i]);
  luaM_growvector(ls->L, f->kproto, f->nkproto, 1, TProtoFunc *,
                  constantEM, MAXARG_A);
  f->kproto[f->nkproto++] = func->f;
  deltastack(ls, 1);  /* CLOSURE puts one extra element (before popping) */
  code_AB(ls, CLOSURE, f->nkproto-1, func->nupvalues, -func->nupvalues);
}


static void init_state (LexState *ls, FuncState *fs, TaggedString *source) {
  lua_State *L = ls->L;
  TProtoFunc *f = luaF_newproto(ls->L);
  fs->prev = ls->fs;  /* linked list of funcstates */
  ls->fs = fs;
  fs->stacksize = 0;
  fs->nlocalvar = 0;
  fs->nupvalues = 0;
  fs->lastsetline = 0;
  fs->f = f;
  f->source = source;
  fs->pc = 0;
  fs->last_pc = -1;  /* invalid index to signal no last instruction */
  f->code = NULL;
  f->maxstacksize = 0;
  f->numparams = 0;  /* default for main chunk */
  f->is_vararg = 0;  /* default for main chunk */
  fs->nvars = (L->debug) ? 0 : -1;  /* flag no debug information? */
  /* push function (to avoid GC) */
  tfvalue(L->top) = f;
  ttype(L->top) = LUA_T_LPROTO;
  incr_top;
}


static void close_func (LexState *ls) {
  FuncState *fs = ls->fs;
  TProtoFunc *f = fs->f;
  code_0(ls, ENDCODE, 0);
  luaM_reallocvector(ls->L, f->code, fs->pc, Instruction);
  luaM_reallocvector(ls->L, f->kstr, f->nkstr, TaggedString *);
  luaM_reallocvector(ls->L, f->knum, f->nknum, real);
  luaM_reallocvector(ls->L, f->kproto, f->nkproto, TProtoFunc *);
  if (fs->nvars != -1) {  /* debug information? */
    luaI_registerlocalvar(ls, NULL, -1);  /* flag end of vector */
    luaM_reallocvector(ls->L, f->locvars, fs->nvars, LocVar);
  }
  ls->fs = fs->prev;
  ls->L->top--;  /* pop function */
}


TProtoFunc *luaY_parser (lua_State *L, ZIO *z) {
  struct LexState lexstate;
  struct FuncState funcstate;
  luaX_setinput(L, &lexstate, z);
  init_state(&lexstate, &funcstate, luaS_new(L, zname(z)));
  next(&lexstate);  /* read first token */
  chunk(&lexstate);
  if (lexstate.token != EOS)
    luaY_error(&lexstate, "<eof> expected");
  close_func(&lexstate);
  return funcstate.f;
}



/*============================================================*/
/* GRAMAR RULES */
/*============================================================*/


static void explist1 (LexState *ls, listdesc *d) {
  vardesc v;
  expr(ls, &v);
  d->n = 1;
  while (ls->token == ',') {
    d->n++;
    close_exp(ls, &v);
    next(ls);
    expr(ls, &v);
  }
  if (v.k == VEXP)
    d->pc = v.info;
  else {
    close_exp(ls, &v);
    d->pc = 0;
  }
}


static void explist (LexState *ls, listdesc *d) {
  switch (ls->token) {
    case ELSE: case ELSEIF: case END: case UNTIL:
    case EOS: case ';': case ')':
      d->pc = 0;
      d->n = 0;
      break;

    default:
      explist1(ls, d);
  }
}


static int funcparams (LexState *ls, int slf) {
  FuncState *fs = ls->fs;
  int slevel = fs->stacksize - slf - 1;  /* where is func in the stack */
  switch (ls->token) {
    case '(': {  /* funcparams -> '(' explist ')' */
      int line = ls->linenumber;
      listdesc e;
      next(ls);
      explist(ls, &e);
      check_match(ls, ')', '(', line);
      close_call(ls, e.pc, MULT_RET);  /* close 1 for old semantics */
      break;
    }

    case '{':  /* funcparams -> constructor */
      constructor(ls);
      break;

    case STRING:  /* funcparams -> STRING */
      code_string(ls, ls->seminfo.ts);  /* must use 'seminfo' before `next' */
      next(ls);
      break;

    default:
      luaY_error(ls, "function arguments expected");
      break;
  }
  fs->stacksize = slevel;  /* call will remove func and params */
  return code_AB(ls, CALL, slevel, 0, 0);
}


static void var_or_func_tail (LexState *ls, vardesc *v) {
  for (;;) {
    switch (ls->token) {
      case '.':  /* var_or_func_tail -> '.' NAME */
        next(ls);
        close_exp(ls, v);  /* `v' must be on stack */
        code_kstr(ls, checkname(ls));
        v->k = VINDEXED;
        break;

      case '[':  /* var_or_func_tail -> '[' exp1 ']' */
        next(ls);
        close_exp(ls, v);  /* `v' must be on stack */
        exp1(ls);
        check(ls, ']');
        v->k = VINDEXED;
        break;

      case ':': {  /* var_or_func_tail -> ':' NAME funcparams */
        int name;
        next(ls);
        name = checkname(ls);
        close_exp(ls, v);  /* `v' must be on stack */
        code_U(ls, PUSHSELF, name, 1);
        v->k = VEXP;
        v->info = funcparams(ls, 1);
        break;
      }

      case '(': case STRING: case '{':  /* var_or_func_tail -> funcparams */
        close_exp(ls, v);  /* `v' must be on stack */
        v->k = VEXP;
        v->info = funcparams(ls, 0);
        break;

      default: return;  /* should be follow... */
    }
  }
}


static void var_or_func (LexState *ls, vardesc *v) {
  /* var_or_func -> ['%'] NAME var_or_func_tail */
  if (optional(ls, '%')) {  /* upvalue? */
    pushupvalue(ls, str_checkname(ls));
    v->k = VEXP;
    v->info = 0;  /* closed expression */
  }
  else  /* variable name */
    singlevar(ls, str_checkname(ls), v, 0);
  var_or_func_tail(ls, v);
}



/*
** {======================================================================
** Rules for Constructors
** =======================================================================
*/


static void recfield (LexState *ls) {
  /* recfield -> (NAME | '['exp1']') = exp1 */
  switch (ls->token) {
    case NAME:
      code_kstr(ls, checkname(ls));
      break;

    case '[':
      next(ls);
      exp1(ls);
      check(ls, ']');
      break;

    default: luaY_error(ls, "<name> or `[' expected");
  }
  check(ls, '=');
  exp1(ls);
}


static int recfields (LexState *ls) {
  /* recfields -> { ',' recfield } [','] */
  int n = 1;  /* one has been read before */
  int mod_n = 1;  /* mod_n == n%RFIELDS_PER_FLUSH */
  while (ls->token == ',') {
    next(ls);
    if (ls->token == ';' || ls->token == '}')
      break;
    recfield(ls);
    n++;
    if (++mod_n == RFIELDS_PER_FLUSH) {
      code_U(ls, SETMAP, RFIELDS_PER_FLUSH-1, -2*RFIELDS_PER_FLUSH);
      mod_n = 0;
    }
  }
  if (mod_n)
    code_U(ls, SETMAP, mod_n-1, -2*mod_n);
  return n;
}


static int listfields (LexState *ls) {
  /* listfields -> { ',' exp1 } [','] */
  int n = 1;  /* one has been read before */
  int mod_n = 1;  /* mod_n == n%LFIELDS_PER_FLUSH */
  while (ls->token == ',') {
    next(ls);
    if (ls->token == ';' || ls->token == '}')
      break;
    exp1(ls);
    n++;
    checklimit(ls, n, MAXARG_A*LFIELDS_PER_FLUSH,
               "items in a list initializer");
    if (++mod_n == LFIELDS_PER_FLUSH) {
      code_AB(ls, SETLIST, n/LFIELDS_PER_FLUSH - 1, LFIELDS_PER_FLUSH-1,
              -LFIELDS_PER_FLUSH);
      mod_n = 0;
    }
  }
  if (mod_n > 0)
    code_AB(ls, SETLIST, n/LFIELDS_PER_FLUSH, mod_n-1, -mod_n);
  return n;
}



static void constructor_part (LexState *ls, constdesc *cd) {
  switch (ls->token) {
    case ';': case '}':  /* constructor_part -> empty */
      cd->n = 0;
      cd->k = ls->token;
      return;

    case NAME: {
      vardesc v;
      expr(ls, &v);
      if (ls->token == '=') {
      code_kstr(ls, getvarname(ls, &v));
      next(ls);  /* skip '=' */
      exp1(ls);
        cd->n = recfields(ls);
        cd->k = 1;  /* record */
      }
      else {
        close_exp(ls, &v);
        cd->n = listfields(ls);
        cd->k = 0;  /* list */
      }
      break;
    }

    case '[':  /* constructor_part -> recfield recfields */
      recfield(ls);
      cd->n = recfields(ls);
      cd->k = 1;  /* record */
      break;

    default:  /* constructor_part -> exp1 listfields */
      exp1(ls);
      cd->n = listfields(ls);
      cd->k = 0;  /* list */
      break;
  }
}


static void constructor (LexState *ls) {
  /* constructor -> '{' constructor_part [';' constructor_part] '}' */
  int line = ls->linenumber;
  int pc = code_U(ls, CREATETABLE, 0, 1);
  int nelems;
  constdesc cd;
  check(ls, '{');
  constructor_part(ls, &cd);
  nelems = cd.n;
  if (ls->token == ';') {
    constdesc other_cd;
    next(ls);
    constructor_part(ls, &other_cd);
    if (cd.k == other_cd.k)  /* repeated parts? */
      luaY_error(ls, "invalid constructor syntax");
    nelems += other_cd.n;
  }
  check_match(ls, '}', '{', line);
  /* set initial table size */
  ls->fs->f->code[pc] = SETARG_U(ls->fs->f->code[pc], nelems);
}

/* }====================================================================== */




/*
** {======================================================================
** For parsing expressions, we use a classic stack with priorities.
** Each binary operator is represented by an index: EQ=2, NE=3, ... '^'=13.
** The unary NOT is 0 and UNMINUS is 1.
** =======================================================================
*/

#define INDNOT		0
#define INDMINUS	1

/* code of first binary operator */
#define FIRSTBIN	2

/* code for power operator (last operator)
** '^' needs special treatment because it is right associative
*/
#define POW	13


static const int priority [POW+1] =  {5, 5, 1, 1, 1, 1, 1, 1, 2, 3, 3, 4, 4, 6};

static const OpCode opcodes [POW+1] = {NOTOP, MINUSOP, EQOP, NEQOP, GTOP,
  LTOP, LEOP, GEOP, CONCOP, ADDOP, SUBOP, MULTOP, DIVOP, POWOP};

#define MAXOPS	20  /* op's stack size (arbitrary limit) */

typedef struct stack_op {
  int ops[MAXOPS];
  int top;
} stack_op;


/*
** returns the index of a binary operator
*/
static int binop (int op) {
  switch (op) {
    case EQ: return FIRSTBIN;
    case  NE: return FIRSTBIN+1;
    case  '>': return FIRSTBIN+2;
    case  '<': return FIRSTBIN+3;
    case  LE: return FIRSTBIN+4;
    case  GE: return FIRSTBIN+5;
    case  CONC: return FIRSTBIN+6;
    case  '+': return FIRSTBIN+7;
    case  '-': return FIRSTBIN+8;
    case  '*': return FIRSTBIN+9;
    case  '/': return FIRSTBIN+10;
    case  '^': return FIRSTBIN+11;
    default: return -1;
  }
}


static void push (LexState *ls, stack_op *s, int op) {
  if (s->top >= MAXOPS)
    luaY_error(ls, "expression too complex");
  s->ops[s->top++] = op;
}


static void pop_to (LexState *ls, stack_op *s, int prio) {
  int op;
  while (s->top > 0 && priority[(op=s->ops[s->top-1])] >= prio) {
    code_0(ls, opcodes[op], op<FIRSTBIN?0:-1);
    s->top--;
  }
}

static void simpleexp (LexState *ls, vardesc *v) {
  check_debugline(ls);
  switch (ls->token) {
    case NUMBER: {  /* simpleexp -> NUMBER */
      real r = ls->seminfo.r;
      next(ls);
      code_number(ls, r);
      break;
    }

    case STRING:  /* simpleexp -> STRING */
      code_string(ls, ls->seminfo.ts);  /* must use 'seminfo' before `next' */
      next(ls);
      break;

    case NIL:  /* simpleexp -> NIL */
      adjuststack(ls, -1);
      next(ls);
      break;

    case '{':  /* simpleexp -> constructor */
      constructor(ls);
      break;

    case FUNCTION:  /* simpleexp -> FUNCTION body */
      next(ls);
      body(ls, 0, ls->linenumber);
      break;

    case '(':  /* simpleexp -> '(' expr ')' */
      next(ls);
      expr(ls, v);
      check(ls, ')');
      return;

    case NAME: case '%':
      var_or_func(ls, v);
      return;

    default:
      luaY_error(ls, "<expression> expected");
      return;
  }
  v->k = VEXP; v->info = 0;
}


static void prefixexp (LexState *ls, vardesc *v, stack_op *s) {
  /* prefixexp -> {NOT | '-'} simpleexp */
  while (ls->token == NOT || ls->token == '-') {
    push(ls, s, (ls->token==NOT)?INDNOT:INDMINUS);
    next(ls);
  }
  simpleexp(ls, v);
}


static void arith_exp (LexState *ls, vardesc *v) {
  stack_op s;
  int op;
  s.top = 0;
  prefixexp(ls, v, &s);
  while ((op = binop(ls->token)) >= 0) {
    close_exp(ls, v);
    /* '^' is right associative, so must 'simulate' a higher priority */
    pop_to(ls, &s, (op == POW)?priority[op]+1:priority[op]);
    push(ls, &s, op);
    next(ls);
    prefixexp(ls, v, &s);
    close_exp(ls, v);
  }
  if (s.top > 0) {
    close_exp(ls, v);
    pop_to(ls, &s, 0);
  }
}


static void exp1 (LexState *ls) {
  vardesc v;
  expr(ls, &v);
  close_exp(ls, &v);
}


static void expr (LexState *ls, vardesc *v) {
  /* expr -> arith_exp {(AND | OR) arith_exp} */
  arith_exp(ls, v);
  while (ls->token == AND || ls->token == OR) {
    OpCode op = (ls->token == AND) ? ONFJMP : ONTJMP;
    int pc;
    close_exp(ls, v);
    next(ls);
    pc = code_S(ls, op, 0, -1);
    arith_exp(ls, v);
    close_exp(ls, v);
    luaK_fixjump(ls, pc, ls->fs->pc);
  }
}


/* }==================================================================== */


/*
** {======================================================================
** Rules for Statements
** =======================================================================
*/


static void block (LexState *ls) {
  /* block -> chunk */
  FuncState *fs = ls->fs;
  int nlocalvar = fs->nlocalvar;
  chunk(ls);
  adjuststack(ls, fs->nlocalvar - nlocalvar);
  for (; fs->nlocalvar > nlocalvar; fs->nlocalvar--)
    luaI_unregisterlocalvar(ls, fs->lastsetline);
}


static int assignment (LexState *ls, vardesc *v, int nvars) {
  int left = 0;
  checklimit(ls, nvars, MAXVARSLH, "variables in a multiple assignment");
  if (ls->token == ',') {  /* assignment -> ',' NAME assignment */
    vardesc nv;
    next(ls);
    var_or_func(ls, &nv);
    if (nv.k == VEXP)
      luaY_error(ls, "syntax error");
    left = assignment(ls, &nv, nvars+1);
  }
  else {  /* assignment -> '=' explist1 */
    listdesc d;
    if (ls->token != '=')
      error_unexpected(ls);
    next(ls);
    explist1(ls, &d);
    adjust_mult_assign(ls, nvars, &d);
  }
  if (v->k != VINDEXED || left+(nvars-1) == 0) {
    /* global/local var or indexed var without values in between */
    storevar(ls, v);
  }
  else {  /* indexed var with values in between*/
    code_U(ls, SETTABLE, left+(nvars-1), -1);
    left += 2;  /* table&index are not popped, because they aren't on top */
  }
  return left;
}


/* maximum size of a while condition */
#ifndef MAX_WHILE_EXP
#define MAX_WHILE_EXP 200	/* arbitrary limit */
#endif

static void whilestat (LexState *ls, int line) {
  /* whilestat -> WHILE exp1 DO block END */
  Instruction buffer[MAX_WHILE_EXP];
  FuncState *fs = ls->fs;
  int while_init = fs->pc;
  int cond_size;
  int i;
  next(ls);  /* skip WHILE */
  exp1(ls);  /* read condition */
  cond_size = fs->pc - while_init;
  /* save condition (to move it to after body) */
  if (cond_size > MAX_WHILE_EXP)
    luaY_error(ls, "while condition too complex");
  for (i=0; i<cond_size; i++) buffer[i] = fs->f->code[while_init+i];
  /* go back to state prior condition */
  fs->pc = while_init;
  deltastack(ls, -1);
  code_S(ls, JMP, 0, 0);  /* initial jump to condition */
  check(ls, DO);
  block(ls);
  check_match(ls, END, WHILE, line);
  luaK_fixjump(ls, while_init, fs->pc);
  /* copy condition to new position, and correct stack */
  for (i=0; i<cond_size; i++) luaK_primitivecode(ls, buffer[i]);
  deltastack(ls, 1);
  luaK_fixjump(ls, code_S(ls, IFTJMP, 0, -1), while_init+1);
}


static void repeatstat (LexState *ls, int line) {
  /* repeatstat -> REPEAT block UNTIL exp1 */
  FuncState *fs = ls->fs;
  int repeat_init = fs->pc;
  next(ls);
  block(ls);
  check_match(ls, UNTIL, REPEAT, line);
  exp1(ls);
  luaK_fixjump(ls, code_S(ls, IFFJMP, 0, -1), repeat_init);
}


static int localnamelist (LexState *ls) {
  /* localnamelist -> NAME {',' NAME} */
  int i = 1;
  store_localvar(ls, str_checkname(ls), 0);
  while (ls->token == ',') {
    next(ls);
    store_localvar(ls, str_checkname(ls), i++);
  }
  return i;
}


static void decinit (LexState *ls, listdesc *d) {
  /* decinit -> ['=' explist1] */
  if (ls->token == '=') {
    next(ls);
    explist1(ls, d);
  }
  else {
    d->n = 0;
    d->pc = 0;
  }
}


static void localstat (LexState *ls) {
  /* stat -> LOCAL localnamelist decinit */
  FuncState *fs = ls->fs;
  listdesc d;
  int nvars;
  check_debugline(ls);
  next(ls);
  nvars = localnamelist(ls);
  decinit(ls, &d);
  adjustlocalvars(ls, nvars, fs->lastsetline);
  adjust_mult_assign(ls, nvars, &d);
}


static int funcname (LexState *ls, vardesc *v) {
  /* funcname -> NAME [':' NAME | '.' NAME] */
  int needself = 0;
  singlevar(ls, str_checkname(ls), v, 0);
  if (ls->token == ':' || ls->token == '.') {
    needself = (ls->token == ':');
    next(ls);
    close_exp(ls, v);
    code_kstr(ls, checkname(ls));
    v->k = VINDEXED;
  }
  return needself;
}


static int funcstat (LexState *ls, int line) {
  /* funcstat -> FUNCTION funcname body */
  int needself;
  vardesc v;
  if (ls->fs->prev)  /* inside other function? */
    return 0;
  check_debugline(ls);
  next(ls);
  needself = funcname(ls, &v);
  body(ls, needself, line);
  storevar(ls, &v);
  return 1;
}


static void namestat (LexState *ls) {
  /* stat -> func | ['%'] NAME assignment */
  vardesc v;
  check_debugline(ls);
  var_or_func(ls, &v);
  if (v.k == VEXP) {  /* stat -> func */
    if (v.info == 0)  /* is just an upper value? */
      luaY_error(ls, "syntax error");
    close_call(ls, v.info, 0);  /* call statement uses no results */
  }
  else {  /* stat -> ['%'] NAME assignment */
    int left = assignment(ls, &v, 1);
    adjuststack(ls, left);  /* remove eventual garbage left on stack */
  }
}


static void ifpart (LexState *ls, int line) {
  /* ifpart -> cond THEN block [ELSE block | ELSEIF ifpart] */
  FuncState *fs = ls->fs;
  int c;  /* address of the conditional jump */
  int je;  /* address of the unconditional jump (to skip `else' part) */
  int elseinit;
  next(ls);  /* skip IF or ELSEIF */
  exp1(ls);  /* cond */
  c = code_S(ls, IFFJMP, 0, -1);  /* jump `then' if `cond' is false */
  check(ls, THEN);
  block(ls);  /* `then' part */
  je = code_S(ls, JMP, 0, 0);  /* jump `else' part after `then' */
  elseinit = fs->pc;
  if (ls->token == ELSEIF)
    ifpart(ls, line);
  else {
    if (optional(ls, ELSE))
      block(ls);  /* `else' part */
    check_match(ls, END, IF, line);
  }
  if (fs->pc > elseinit)  /* is there an `else' part? */
    luaK_fixjump(ls, je, fs->pc);  /* last jump jumps over it */
  else {
    fs->pc--;  /* remove last jump */
    elseinit--;  /* first jump will be smaller */
    LUA_ASSERT(L, fs->pc == je, "jump out of place");
  }
  luaK_fixjump(ls, c, elseinit);  /* fix first jump to `else' part */
}


static int stat (LexState *ls) {
  int line = ls->linenumber;  /* may be needed for error messages */
  switch (ls->token) {
    case IF:  /* stat -> IF ifpart END */
      ifpart(ls, line);
      return 1;

    case WHILE:  /* stat -> whilestat */
      whilestat(ls, line);
      return 1;

    case DO: {  /* stat -> DO block END */
      next(ls);
      block(ls);
      check_match(ls, END, DO, line);
      return 1;
    }

    case REPEAT:  /* stat -> repeatstat */
      repeatstat(ls, line);
      return 1;

    case FUNCTION:  /* stat -> funcstat */
      return funcstat(ls, line);

    case LOCAL:  /* stat -> localstat */
      localstat(ls);
      return 1;

    case NAME: case '%':  /* stat -> namestat */
      namestat(ls);
      return 1;

    case RETURN: case ';': case ELSE: case ELSEIF:
    case END: case UNTIL: case EOS:  /* 'stat' follow */
      return 0;

    default:
      error_unexpected(ls);
      return 0;  /* to avoid warnings */
  }
}


static void parlist (LexState *ls) {
  int nparams = 0;
  int dots = 0;
  switch (ls->token) {
    case DOTS:  /* parlist -> DOTS */
      next(ls);
      dots = 1;
      break;

    case NAME:  /* parlist, tailparlist -> NAME [',' tailparlist] */
      init:
      store_localvar(ls, str_checkname(ls), nparams++);
      if (ls->token == ',') {
        next(ls);
        switch (ls->token) {
          case DOTS:  /* tailparlist -> DOTS */
            next(ls);
            dots = 1;
            break;

          case NAME:  /* tailparlist -> NAME [',' tailparlist] */
            goto init;

          default: luaY_error(ls, "<name> or `...' expected");
        }
      }
      break;

    case ')': break;  /* parlist -> empty */

    default: luaY_error(ls, "<name> or `...' expected");
  }
  code_args(ls, nparams, dots);
}


static void body (LexState *ls, int needself, int line) {
  /* body ->  '(' parlist ')' chunk END */
  FuncState new_fs;
  init_state(ls, &new_fs, ls->fs->f->source);
  new_fs.f->lineDefined = line;
  check(ls, '(');
  if (needself)
    add_localvar(ls, luaS_newfixed(ls->L, "self"));
  parlist(ls);
  check(ls, ')');
  chunk(ls);
  check_match(ls, END, FUNCTION, line);
  close_func(ls);
  func_onstack(ls, &new_fs);
}


static void ret (LexState *ls) {
  /* ret -> [RETURN explist sc] */
  if (ls->token == RETURN) {
    listdesc e;
    check_debugline(ls);
    next(ls);
    explist(ls, &e); 
    close_call(ls, e.pc, MULT_RET);
    code_U(ls, RETCODE, ls->fs->nlocalvar, 0);
    ls->fs->stacksize = ls->fs->nlocalvar;  /* removes all temp values */
    optional(ls, ';');
  }
}

/* }====================================================================== */


static void chunk (LexState *ls) {
  /* chunk -> { stat [;] } ret */
  while (stat(ls)) {
    LUA_ASSERT(ls->L, ls->fs->stacksize == ls->fs->nlocalvar,
               "stack size != # local vars");
    optional(ls, ';');
  }
  ret(ls);  /* optional return */
}

