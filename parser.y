/* -*- fundamental -*- */

%{
#include <stdio.h>

#include "mathmap.h"
#include "exprtree.h"
#include "builtins.h"
#include "jump.h"

extern exprtree *theExprtree;
%}

%union {
    ident ident;
    exprtree *exprtree;
}

%token T_IDENT T_NUMBER
%token T_IF T_THEN T_ELSE T_END
%token T_WHILE T_DO

%right ';'
%right '='
%left T_OR T_AND
%left T_EQUAL '<' '>' T_LESSEQUAL T_GREATEREQUAL T_NOTEQUAL
%left '+' '-'
%left '*' '/' '%'
%right '^'
%left UNARY
%right ':' T_CONVERT
%left '['

%%

start :   expr              { theExprtree = $<exprtree>1; }
        ;

expr :   T_NUMBER            { $<exprtree>$ = $<exprtree>1; }
       | T_IDENT             { $<exprtree>$ = make_var($<ident>1); }
       | '[' exprlist ']'    { $<exprtree>$ = make_tuple($<exprtree>2); }
       | expr '[' expr ']'   { $<exprtree>$ = make_select($<exprtree>1, $<exprtree>3); }
       | T_IDENT ':' expr    { $<exprtree>$ = make_cast($<ident>1, $<exprtree>3); }
       | T_IDENT T_CONVERT expr { $<exprtree>$ = make_convert($<ident>1, $<exprtree>3); }
       | expr '+' expr       { $<exprtree>$ = make_function("__add",
							    exprlist_append($<exprtree>1, $<exprtree>3)); }
       | expr '-' expr       { $<exprtree>$ = make_function("__sub",
							    exprlist_append($<exprtree>1, $<exprtree>3)); }
       | expr '*' expr       { $<exprtree>$ = make_function("__mul",
							    exprlist_append($<exprtree>1, $<exprtree>3)); }
       | expr '/' expr       { $<exprtree>$ = make_function("__div",
							    exprlist_append($<exprtree>1, $<exprtree>3)); }
       | expr '%' expr       { $<exprtree>$ = make_function("__mod",
							    exprlist_append($<exprtree>1, $<exprtree>3)); }
       | expr '^' expr       { $<exprtree>$ = make_function("__pow",
							    exprlist_append($<exprtree>1, $<exprtree>3)); }
       | expr T_EQUAL expr   { $<exprtree>$ = make_function("__equal",
							    exprlist_append($<exprtree>1, $<exprtree>3)); }
       | expr '<' expr       { $<exprtree>$ = make_function("__less",
							    exprlist_append($<exprtree>1, $<exprtree>3)); }
       | expr '>' expr       { $<exprtree>$ = make_function("__greater",
							    exprlist_append($<exprtree>1, $<exprtree>3)); }
       | expr T_LESSEQUAL expr
                             { $<exprtree>$ = make_function("__lessequal",
							    exprlist_append($<exprtree>1, $<exprtree>3)); }
       | expr T_GREATEREQUAL expr
                             { $<exprtree>$ = make_function("__greaterequal",
							    exprlist_append($<exprtree>1, $<exprtree>3)); }
       | expr T_NOTEQUAL expr
                             { $<exprtree>$ = make_function("__notequal",
							    exprlist_append($<exprtree>1, $<exprtree>3)); }
       | expr T_OR expr      { $<exprtree>$ = make_function("__or",
							    exprlist_append($<exprtree>1, $<exprtree>3)); }
       | expr T_AND expr     { $<exprtree>$ = make_function("__and",
							    exprlist_append($<exprtree>1, $<exprtree>3)); }
       | '-' expr %prec UNARY
                             { $<exprtree>$ = make_function("__neg", $<exprtree>2); }
       | '!' expr %prec UNARY
                             { $<exprtree>$ = make_function("__not", $<exprtree>2); }
       | '(' expr ')'        { $<exprtree>$ = $<exprtree>2; };
       | T_IDENT '(' arglist ')'
                             { $<exprtree>$ = make_function($<ident>1, $<exprtree>3); }
       | T_IDENT '=' expr    { $<exprtree>$ = make_assignment($<ident>1, $<exprtree>3); }
       | expr ';' expr       { $<exprtree>$ = make_sequence($<exprtree>1, $<exprtree>3); }
       | T_IF expr T_THEN expr T_END
                             { $<exprtree>$ = make_if_then($<exprtree>2, $<exprtree>4); }
       | T_IF expr T_THEN expr T_ELSE expr T_END
                             { $<exprtree>$ = make_if_then_else($<exprtree>2,
								$<exprtree>4,
								$<exprtree>6); }
       | T_WHILE expr T_DO expr T_END
                             { $<exprtree>$ = make_while($<exprtree>2, $<exprtree>4); }
       | T_DO expr T_WHILE expr T_END
                             { $<exprtree>$ = make_do_while($<exprtree>2, $<exprtree>4); }
       ;

arglist :                    { $<exprtree>$ = 0; }
          | exprlist         { $<exprtree>$ = $<exprtree>1; }
          ;

exprlist : expr                    { $<exprtree>$ = $<exprtree>1; }
         | exprlist ',' expr       { $<exprtree>$ = exprlist_append($<exprtree>1, $<exprtree>3); }
         ;

%%

int
yyerror (char *s)
{
    sprintf(error_string, "Parse error.");
    JUMP(0);

    return 0;
}
