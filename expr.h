// expr.h(caspeer): a tiny expression compiler for f(x).
// compiles an expression string into postfix (rpn) ops and evaluates
// it cheaply for every sample of the plot.
//
// grammar:
//   expression = term (('+'|'-') term)*
//   term       = unary (('*'|'/'|'%') unary | <implicit-multiply>)*
//   unary      = ('-'|'+') unary | power
//   power      = primary ('^' unary)?
//   primary    = number | 'x' | const | fn '(' expression ')' | '(' expression ')'
//
// implicit multiply is supported for things like 2x, xsin(x), pi*e ...

#ifndef EXPR_H
#define EXPR_H

#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPR_TEXT_MAX 128
#define EXPR_MAX_ITEMS 256
#define EXPR_STACK_MAX 32

typedef enum {
	EXPR_OK = 0,
	EXPR_ERR_EMPTY,
	EXPR_ERR_TOO_LONG,
	EXPR_ERR_SYNTAX,
	EXPR_ERR_UNKNOWN_NAME,
} ExprResult;

typedef enum {
	EXPR_KIND_VALUE = 1, // push constant
	EXPR_KIND_VARX,      // push x
	EXPR_KIND_OP,        // id = operator or builtin function
} ExprKind;

typedef enum {
	EXPR_OP_ADD = 1,
	EXPR_OP_SUB,
	EXPR_OP_MUL,
	EXPR_OP_DIV,
	EXPR_OP_MOD,
	EXPR_OP_POW,
	EXPR_OP_NEG,
	EXPR_FN_SIN,
	EXPR_FN_COS,
	EXPR_FN_TAN,
	EXPR_FN_ASIN,
	EXPR_FN_ACOS,
	EXPR_FN_ATAN,
	EXPR_FN_SINH,
	EXPR_FN_COSH,
	EXPR_FN_TANH,
	EXPR_FN_SQRT,
	EXPR_FN_ABS,
	EXPR_FN_EXP,
	EXPR_FN_LN,
	EXPR_FN_LOG,  // base 10
	EXPR_FN_LOG2,
	EXPR_FN_FLOOR,
	EXPR_FN_CEIL,
	EXPR_FN_MIN,  // 2 args
	EXPR_FN_MAX,  // 2 args
	EXPR_FN_POW,  // 2 args
	EXPR_FN_MOD,  // 2 args
	EXPR_FN_ATAN2,// 2 args
	EXPR_FN_HYPOT,// 2 args
} ExprOp;

typedef struct {
	unsigned char kind;
	unsigned char op;
	double value;
} ExprItem;

typedef struct {
	int count;
	ExprItem items[EXPR_MAX_ITEMS];
} Expr;

static const char* expr_error_string(ExprResult r)
{
	switch (r){
	case EXPR_ERR_EMPTY: return "empty expression";
	case EXPR_ERR_TOO_LONG: return "expression too long";
	case EXPR_ERR_SYNTAX: return "syntax error";
	case EXPR_ERR_UNKNOWN_NAME: return "unknown name";
	default: return "ok";
	}
}

static ExprResult expr_compile(const char* text, Expr* out);
static double expr_eval(const Expr* e, double x);

#ifdef EXPR_IMPLEMENTATION

typedef struct {
	const char* src;
	int pos;
	Expr* out;
	ExprResult err;
} Parser;

typedef struct {
	const char* name;
	unsigned char op;
	unsigned char arity;
} ExprFnInfo;

//NOTE(caspeer): longest names of a shared prefix should come first
//(sinh before sin, cosh before cos ...)
static const ExprFnInfo expr_fns[] = {
	{"sinh",   EXPR_FN_SINH,   1},
	{"cosh",   EXPR_FN_COSH,   1},
	{"tanh",   EXPR_FN_TANH,   1},
	{"asin",   EXPR_FN_ASIN,   1},
	{"acos",   EXPR_FN_ACOS,   1},
	{"atan2",  EXPR_FN_ATAN2,  2},
	{"atan",   EXPR_FN_ATAN,   1},
	{"sin",    EXPR_FN_SIN,    1},
	{"cos",    EXPR_FN_COS,    1},
	{"tan",    EXPR_FN_TAN,    1},
	{"sqrt",   EXPR_FN_SQRT,   1},
	{"abs",    EXPR_FN_ABS,    1},
	{"exp",    EXPR_FN_EXP,    1},
	{"log2",   EXPR_FN_LOG2,   1},
	{"log",    EXPR_FN_LOG,    1},
	{"ln",     EXPR_FN_LN,     1},
	{"floor",  EXPR_FN_FLOOR,  1},
	{"ceil",   EXPR_FN_CEIL,   1},
	{"min",    EXPR_FN_MIN,    2},
	{"max",    EXPR_FN_MAX,    2},
	{"pow",    EXPR_FN_POW,    2},
	{"mod",    EXPR_FN_MOD,    2},
	{"hypot",  EXPR_FN_HYPOT,  2},
};

static const struct { const char* name; double value; } expr_consts[] = {
	{"pi", M_PI},
	{"tau", M_PI * 2.0},
	{"e", M_E},
};

static void p_emit(Parser* p, unsigned char kind, unsigned char op, double value)
{
	if (p->err) return;
	if (p->out->count >= EXPR_MAX_ITEMS){
		p->err = EXPR_ERR_TOO_LONG;
		return;
	}
	ExprItem* it = &p->out->items[p->out->count++];
	it->kind = kind;
	it->op = op;
	it->value = value;
}

static int expr_fn_find(const char* s, int len, int* fn_len)
{
	for (size_t i = 0; i < sizeof(expr_fns) / sizeof(expr_fns[0]); i++){
		const ExprFnInfo* f = &expr_fns[i];
		size_t nl = strlen(f->name);
		if (len >= (int)nl && strncmp(s, f->name, nl) == 0){
			*fn_len = (int)nl;
			return (int)f->op;
		}
	}
	return -1;
}

static int expr_const_find(const char* s, int len, int* const_len)
{
	for (size_t i = 0; i < sizeof(expr_consts) / sizeof(expr_consts[0]); i++){
		size_t nl = strlen(expr_consts[i].name);
		if (len >= (int)nl && strncmp(s, expr_consts[i].name, nl) == 0){
			*const_len = (int)nl;
			return (int)i;
		}
	}
	return -1;
}

static void p_skip_ws(Parser* p)
{
	while (isspace((unsigned char) p->src[p->pos])) p->pos++;
}

static double expr_parse_expression(Parser* p);

// primary(caspeer): handles identifiers by splitting them into atoms.
// "xsin(x)" => x * sin(x), "pie" => pi * e ..., functions must be
// followed by a parenthesised argument.
static bool expr_parse_primary(Parser* p)
{
	if (p->err) return false;
	p_skip_ws(p);
	char c = p->src[p->pos];

	if (c == '('){
		p->pos++;
		expr_parse_expression(p);
		p_skip_ws(p);
		if (p->src[p->pos] != ')'){
			p->err = EXPR_ERR_SYNTAX;
			return false;
		}
		p->pos++;
		return true;
	}

	if (isdigit((unsigned char) c) || c == '.'){
		char* end = NULL;
		double v = strtod(p->src + p->pos, &end);
		if (end == p->src + p->pos){
			p->err = EXPR_ERR_SYNTAX;
			return false;
		}
		p->pos = (int)(end - p->src);
		p_emit(p, EXPR_KIND_VALUE, 0, v);
		return true;
	}

	if (isalpha((unsigned char) c) || c == '_'){
		// munch the whole identifier token
		int start = p->pos;
		int len = 0;
		while (isalnum((unsigned char) p->src[start + len]) || p->src[start + len] == '_') len++;
		const char* tok = p->src + start;
		int tpos = 0;
		int fn_len = 0;
		int fid = -1;
		int pieces = 0;
		while (tpos < len && fid < 0){
			int const_len = 0;
			if (tok[tpos] == 'x'){
				p_emit(p, EXPR_KIND_VARX, 0, 0);
				tpos++;
			}else if ((fid = expr_fn_find(tok + tpos, len - tpos, &fn_len)) >= 0){
				break; // function argument parsing continues below
			}else{
				int ci = expr_const_find(tok + tpos, len - tpos, &const_len);
				if (ci < 0){
					p->err = EXPR_ERR_UNKNOWN_NAME;
					return false;
				}
				p_emit(p, EXPR_KIND_VALUE, 0, expr_consts[ci].value);
				tpos += const_len;
			}
			pieces++;
			if (pieces > 1) p_emit(p, EXPR_KIND_OP, EXPR_OP_MUL, 0); // implicit multiply between pieces
		}
		if (fid >= 0){
			bool had_chain = pieces > 0;
			// rewind the raw position to the end of the function name so
			// the parser picks up the '(' (or whatever comes next)
			p->pos = start + tpos + fn_len;
			p_skip_ws(p);
			if (p->src[p->pos] != '('){
				p->err = EXPR_ERR_SYNTAX;
				return false;
			}
			p->pos++;
			expr_parse_expression(p);
			p_skip_ws(p);
			if (p->src[p->pos] == ','){
				// two argument builtin: min(a,b) mod(a,b) pow(a,b) ...
				p->pos++;
				expr_parse_expression(p);
				p_skip_ws(p);
			}
			if (p->src[p->pos] != ')'){
				p->err = EXPR_ERR_SYNTAX;
				return false;
			}
			p->pos++;
			p_emit(p, EXPR_KIND_OP, (unsigned char) fid, 0);
			if (had_chain) p_emit(p, EXPR_KIND_OP, EXPR_OP_MUL, 0);
		}else{
			p->pos = start + tpos;
		}
		return true;
	}

	p->err = EXPR_ERR_SYNTAX;
	return false;
}

static bool expr_parse_unary(Parser* p)
{
	p_skip_ws(p);
	char c = p->src[p->pos];
	if (c == '-'){
		p->pos++;
		if (!expr_parse_unary(p)) return false;
		p_emit(p, EXPR_KIND_OP, EXPR_OP_NEG, 0);
		return true;
	}
	if (c == '+'){
		p->pos++;
		return expr_parse_unary(p);
	}
	if (!expr_parse_primary(p)) return false;

	// right-associative power (peek only, keep the untouched position for the caller)
	if (!p->err){
		int save = p->pos;
		p_skip_ws(p);
		if (p->src[p->pos] == '^'){
			p->pos++;
			if (!expr_parse_unary(p)) return false;
			p_emit(p, EXPR_KIND_OP, EXPR_OP_POW, 0);
		}else{
			p->pos = save;
		}
	}
	return true;
}

static void expr_parse_term(Parser* p)
{
	if (!expr_parse_unary(p)) return;
	while (!p->err){
		char raw = p->src[p->pos];
		char c = raw;
		bool was_space = isspace((unsigned char) raw);
		p_skip_ws(p);
		c = p->src[p->pos];
		if (c == '*' || c == '/' || c == '%'){
			unsigned char op = c == '*' ? EXPR_OP_MUL : c == '/' ? EXPR_OP_DIV : EXPR_OP_MOD;
			p->pos++;
			if (!expr_parse_unary(p)) return;
			p_emit(p, EXPR_KIND_OP, op, 0);
		}else if (c == '(' || (!was_space && (isdigit((unsigned char) c) || c == '.' || isalpha((unsigned char) c) || c == '_'))){
			// implicit multiplication, but only when tokens are adjacent
			// (2x, 2(x+1), xsin(x)) ... unless it is a paren, whitespace is
			// allowed there: "2 (x+1)"
			if (!expr_parse_unary(p)) return;
			p_emit(p, EXPR_KIND_OP, EXPR_OP_MUL, 0);
		}else if (was_space && (isdigit((unsigned char) c) || c == '.' || isalpha((unsigned char) c) || c == '_')){
			// "2 3" style separation is not implicit multiplication but an error,
			// keep the char for the top-level leftover check
			p->err = EXPR_ERR_SYNTAX;
			return;
		}else{
			break;
		}
	}
}

static double expr_parse_expression(Parser* p)
{
	expr_parse_term(p);
	while (!p->err){
		p_skip_ws(p);
		char c = p->src[p->pos];
		if (c == '+' || c == '-'){
			unsigned char op = c == '+' ? EXPR_OP_ADD : EXPR_OP_SUB;
			p->pos++;
			expr_parse_term(p);
			if (p->err) break;
			p_emit(p, EXPR_KIND_OP, op, 0);
		}else{
			break;
		}
	}
	return 0;
}

static ExprResult expr_compile(const char* text, Expr* out)
{
	memset(out, 0, sizeof(*out));
	Parser p = {0};
	p.src = text;
	p.out = out;
	expr_parse_expression(&p);
	if (p.err) return p.err;
	p_skip_ws(&p);
	if (text[p.pos] != '\0') return EXPR_ERR_SYNTAX;
	if (out->count == 0) return EXPR_ERR_EMPTY;
	return EXPR_OK;
}

static double expr_eval(const Expr* e, double x)
{
	double st[EXPR_STACK_MAX];
	int sp = 0;
	for (int i = 0; i < e->count; i++){
		const ExprItem* it = &e->items[i];
		switch (it->kind){
		case EXPR_KIND_VALUE:
			if (sp >= EXPR_STACK_MAX) return NAN;
			st[sp++] = it->value;
			break;
		case EXPR_KIND_VARX:
			if (sp >= EXPR_STACK_MAX) return NAN;
			st[sp++] = x;
			break;
		case EXPR_KIND_OP:{
			if (sp < 1) return NAN;
			switch (it->op){
			case EXPR_OP_NEG: st[sp-1] = -st[sp-1]; break;
			case EXPR_OP_ADD:
			case EXPR_OP_SUB:
			case EXPR_OP_MUL:
			case EXPR_OP_DIV:
			case EXPR_OP_MOD:
			case EXPR_OP_POW:
				if (sp < 2) return NAN;
				sp--;
				switch (it->op){
				case EXPR_OP_ADD: st[sp-1] = st[sp-1] + st[sp]; break;
				case EXPR_OP_SUB: st[sp-1] = st[sp-1] - st[sp]; break;
				case EXPR_OP_MUL: st[sp-1] = st[sp-1] * st[sp]; break;
				case EXPR_OP_DIV: st[sp-1] = st[sp-1] / st[sp]; break;
				case EXPR_OP_MOD: st[sp-1] = fmod(st[sp-1], st[sp]); break;
				case EXPR_OP_POW: st[sp-1] = pow(st[sp-1], st[sp]); break;
				}
				break;
			default:{
				// builtins
				const ExprFnInfo* f = NULL;
				for (size_t j = 0; j < sizeof(expr_fns) / sizeof(expr_fns[0]); j++){
					if (expr_fns[j].op == it->op){ f = &expr_fns[j]; break; }
				}
				if (f == NULL) return NAN;
				if (f->arity == 1){
					double a = st[sp-1];
					switch (it->op){
					case EXPR_FN_SIN:   a = sin(a); break;
					case EXPR_FN_COS:   a = cos(a); break;
					case EXPR_FN_TAN:   a = tan(a); break;
					case EXPR_FN_ASIN:  a = asin(a); break;
					case EXPR_FN_ACOS:  a = acos(a); break;
					case EXPR_FN_ATAN:  a = atan(a); break;
					case EXPR_FN_SINH:  a = sinh(a); break;
					case EXPR_FN_COSH:  a = cosh(a); break;
					case EXPR_FN_TANH:  a = tanh(a); break;
					case EXPR_FN_SQRT:  a = sqrt(a); break;
					case EXPR_FN_ABS:   a = fabs(a); break;
					case EXPR_FN_EXP:   a = exp(a); break;
					case EXPR_FN_LN:    a = log(a); break;
					case EXPR_FN_LOG:   a = log10(a); break;
					case EXPR_FN_LOG2:  a = log2(a); break;
					case EXPR_FN_FLOOR: a = floor(a); break;
					case EXPR_FN_CEIL:  a = ceil(a); break;
					}
					st[sp-1] = a;
				}else{
					if (sp < 2) return NAN;
					double a = st[sp-2], b = st[sp-1];
					sp--;
					switch (it->op){
					case EXPR_FN_MIN:   a = fmin(a, b); break;
					case EXPR_FN_MAX:   a = fmax(a, b); break;
					case EXPR_FN_POW:   a = pow(a, b); break;
					case EXPR_FN_MOD:   a = fmod(a, b); break;
					case EXPR_FN_ATAN2: a = atan2(a, b); break;
					case EXPR_FN_HYPOT: a = hypot(a, b); break;
					}
					st[sp-1] = a;
				}
				break;
			}
			}
			break;
		}
		default: return NAN;
		}
	}
	if (sp != 1) return NAN;
	return st[0];
}

#endif // EXPR_IMPLEMENTATION
#endif // EXPR_H
