#include "parser.c"

Code_Type *builtin_Type = 0;
Code_Type *builtin_i32 = 0;

Arena _scratch_arena;
Arena *scratch_arena = &_scratch_arena;


char *tcstring(String str) {
  char *result = to_c_string(scratch_arena, str);
  return result;
}

void builder_write(String str) {
  printf(tcstring(str));
}

typedef struct {
  Code_Stmt_Decl **top_decls;
  Arena *arena;
  Code_Stmt_Decl *decl;
  Parser *parser;
  Code_Func *current_func;
  
  b32 is_inside_func_typedef;
} Resolver;


typedef struct {
  i32 indent;
  Resolver *res;
  String enum_name;
} Emitter;

Scope *alloc_scope(Arena *arena, Scope *parent) {
  Scope *result = arena_push_struct(arena, Scope);
  result->entries = sb_new(arena, Scope_Entry, 32);
  result->parent = parent;
  return result;
}

Scope_Entry *scope_get(Scope *scope, String name) {
  Scope_Entry *result = 0;
  for (u32 i = 0; i < sb_count(scope->entries); i++) {
    Scope_Entry *test = scope->entries + i;
    if (string_compare(test->decl->name, name)) {
      result = test;
      break;
    }
  }
  if (!result && scope->parent) {
    result = scope_get(scope->parent, name);
  }
  
  return result;
}

Scope_Entry *scope_add(Scope *scope, Code_Stmt_Decl *decl) {
  Scope_Entry *entry = scope_get(scope, decl->name);
  if (!entry) {
    sb_push(scope->entries, (Scope_Entry){0});
    entry = sb_peek(scope->entries);
  }
  
  entry->decl = decl;
  
  return entry;
}

Scope *get_global_scope(Scope *child) {
  Scope *result = child;
  while (result->parent) result = child->parent;
  return result;
}


b32 check_types(Code_Type *a, Code_Type *b) {
  b32 result = false;
  
  if (a->kind == b->kind) {
    switch (a->kind) {
      case Type_Kind_STRUCT: {
        result = false;
      } break;
      case Type_Kind_ENUM: {
        result = false;
      } break;
      case Type_Kind_PTR: {
        result = check_types(a->pointer.base, b->pointer.base);
      } break;
      case Type_Kind_ARRAY: {
        result = false;
        assert(false);
      } break;
      case Type_Kind_FUNC: {
        if (sb_count(a->func.params) == sb_count(b->func.params)) {
          result = true;
          for (u32 i = 0; i < sb_count(a->func.params); i++) {
            Code_Stmt_Decl *a_param = a->func.params[i];
            Code_Stmt_Decl *b_param = b->func.params[i];
            if (!check_types(a_param->type, b_param->type)) {
              result = false;
              break;
            }
          }
          if (!check_types(a->func.return_type, b->func.return_type)) {
            result = false;
          }
        }
      } break;
      case Type_Kind_ALIAS: {
        result = string_compare(a->alias.name, b->alias.name);
      } break;
      
      default: assert(false);
    }
  }
  
  assert(result);
  return result;
}


void resolve_name(Resolver, Scope *, String, Resolve_State);
void resolve_decl_partial(Resolver, Scope *, Code_Stmt_Decl *);
void resolve_decl_full(Resolver, Scope *, Code_Stmt_Decl *);
void resolve_expr(Resolver, Scope *, Code_Expr *);
void resolve_stmt(Resolver, Scope *, Code_Stmt *);
void resolve_stmt_block(Resolver, Scope *, Code_Stmt_Block *, b32);


void resolve_stmt(Resolver res, Scope *scope, Code_Stmt *stmt) {
  switch (stmt->kind) {
    case Stmt_Kind_DECL: {
      resolve_decl_full(res, scope, &stmt->decl);
    } break;
    case Stmt_Kind_ASSIGN: {
      resolve_expr(res, scope, stmt->assign.left);
      resolve_expr(res, scope, stmt->assign.right);
      // TODO(lvl5): make sure the operator makes sense
      // no += for structs, etc
      
      if (stmt->assign.left->kind == Expr_Kind_NAME) {
        // NOTE(lvl5): can't assign to const variables
        Code_Stmt_Decl *left = scope_get(scope, stmt->assign.left->name.name)->decl;
        assert(!left->is_const);
      }
      check_types(stmt->assign.left->type, stmt->assign.right->type);
    } break;
    case Stmt_Kind_EXPR: {
      resolve_expr(res, scope, stmt->expr.expr);
    } break;
    case Stmt_Kind_IF: {
      resolve_expr(res, scope, stmt->if_s.cond);
      resolve_stmt(res, scope, stmt->if_s.then_branch);
      if (stmt->if_s.else_branch) {
        resolve_stmt(res, scope, stmt->if_s.else_branch);
      }
    } break;
    case Stmt_Kind_BLOCK: {
      resolve_stmt_block(res, scope, &stmt->block, /*is_func_body*/ false);
    } break;
    case Stmt_Kind_FOR: {
      resolve_decl_full(res, scope, stmt->for_s.init);
      resolve_expr(res, scope, stmt->for_s.cond);
      resolve_stmt(res, scope, stmt->for_s.post);
      resolve_stmt(res, scope, stmt->for_s.body);
    } break;
    case Stmt_Kind_KEYWORD: {
      // TODO(lvl5): make sure the keyword makes sense?
      resolve_expr(res, scope, stmt->keyword.expr);
      if (stmt->keyword.keyword == T_RETURN) {
        check_types(stmt->keyword.expr->type, res.current_func->type->return_type);
      }
    } break;
    
    default: assert(false);
  }
}

void resolve_stmt_block(Resolver res, Scope *scope, Code_Stmt_Block *block, b32 is_func_body) {
  Scope *child_scope = scope;
  if (!is_func_body) {
    child_scope = alloc_scope(res.arena, scope);
  }
  for (u32 i = 0; i < sb_count(block->statements); i++) {
    Code_Stmt *stmt = block->statements[i];
    resolve_stmt(res, scope, stmt);
  }
}

Code_Type *get_builtin_type(Scope *scope, String name) {
  Code_Type *result = (Code_Type *)scope_get(get_global_scope(scope), name)->decl->value;
  return result;
}

void resolve_type(Resolver res, Scope *scope, Code_Type *type) {
  switch (type->kind) {
    case Type_Kind_ENUM: {
      Scope *child_scope = alloc_scope(res.arena, scope);
      type->enum_t.scope = child_scope;
      for (u32 i = 0; i < sb_count(type->enum_t.members); i++) {
        String member = type->enum_t.members[i];
        scope_add(child_scope,
                  code_stmt_decl(res.parser, member, builtin_i32,
                                 (Code_Node *)code_expr_int(res.parser, i), true));
      }
      type->enum_t.item_type = builtin_i32;
    } break;
    
    case Type_Kind_STRUCT: {
      Scope *child_scope = alloc_scope(res.arena, scope);
      type->struct_t.scope = child_scope;
      for (u32 i = 0; i < sb_count(type->struct_t.members); i++) {
        Code_Stmt_Decl *member = type->struct_t.members[i];
        resolve_decl_full(res, child_scope, member);
      }
    } break;
    
    case Type_Kind_PTR: {
      if (type->pointer.base->kind == Type_Kind_ALIAS) {
        resolve_name(res, scope, type->pointer.base->alias.name, Resolve_State_PARTIAL);
      } else {
        resolve_type(res, scope, type->pointer.base);
      }
    } break;
    
    case Type_Kind_ALIAS: {
      if (res.is_inside_func_typedef) {
        resolve_name(res, scope, type->alias.name, Resolve_State_PARTIAL);
      } else {
        resolve_name(res, scope, type->alias.name, Resolve_State_FULL);
      }
    } break;
    
    case Type_Kind_ARRAY: {
      resolve_type(res, scope, type->array.item_type);
    } break;
    
    case Type_Kind_FUNC: {
      sb_push(type->func.params, code_stmt_decl(res.parser, const_string("ctx"), (Code_Type *)code_type_alias(res.parser, const_string("Context")), 0, false));
      
      for (u32 i = 0; i < sb_count(type->func.params); i++) {
        Code_Stmt_Decl *param = type->func.params[i];
        resolve_decl_full(res, scope, param);
      }
    } break;
    
    default: assert(false);
  }
}

void resolve_expr(Resolver res, Scope *scope, Code_Expr *expr) {
  // TODO(lvl5): set type type_info on the expression
  switch (expr->kind) {
    case Expr_Kind_TYPE: {
      resolve_type(res, scope, &expr->type_e);
      expr->type = builtin_Type;
    } break;
    
    case Expr_Kind_UNARY: {
      // TODO(lvl5): make sure the operator makes sense
      resolve_expr(res, scope, expr->unary.val);
      
      if (expr->unary.op == T_REF) {
        if (expr->unary.val->type == builtin_Type) {
          Code_Type *base = &expr->unary.val->type_e;
          expr->kind = Expr_Kind_TYPE;
          expr->type_e.kind = Type_Kind_PTR;
          expr->type_e.pointer.base = base;
          expr->type = builtin_Type;
        } else {
          expr->type = 
            (Code_Type *)code_type_pointer(res.parser, 
                                           expr->unary.val->type);
        }
      } else if (expr->unary.op == T_DEREF) {
        expr->type = expr->unary.val->type->pointer.base;
      } else {
        expr->type = expr->unary.val->type;
      }
    } break;
    case Expr_Kind_BINARY: {
      // TODO(lvl5): make sure the operator makes sense
      resolve_expr(res, scope, expr->binary.left);
      
      if (expr->binary.op == T_SUBSCRIPT) {
        resolve_expr(res, scope, expr->binary.right);
        check_types(expr->binary.right->type, scope_get(scope, const_string("i32"))->decl->type);
        
        Code_Type *left_type = expr->binary.left->type;
        assert(left_type->kind == Type_Kind_ARRAY);
        expr->type = left_type->array.item_type;
      } else if (expr->binary.op == T_MEMBER) {
        Code_Type *left_type = expr->binary.left->type;
        
        if (left_type->kind == Type_Kind_PTR) {
          left_type = left_type->pointer.base;
        }
        String member_name = expr->binary.right->name.name;
        
        if (left_type != builtin_Type) {
          assert(left_type->kind == Type_Kind_ALIAS);
          assert(expr->binary.right->kind == Expr_Kind_NAME);
          
          Code_Type *left_base = &scope_get(scope, left_type->alias.name)->decl->value->expr.type_e;
          if (left_base->kind == Type_Kind_STRUCT) {
            Code_Type_Struct *struct_t = &left_base->struct_t;
            Scope *struct_scope = struct_t->scope;
            resolve_expr(res, struct_scope, expr->binary.right);
            
            Code_Type *member_type = 0;
            for (u32 i = 0; i < sb_count(struct_t->members); i++) {
              Code_Stmt_Decl *member = struct_t->members[i];
              if (string_compare(member->name, member_name)) {
                member_type = member->type;
                break;
              }
            }
            assert(member_type);
            check_types(member_type, expr->binary.right->type);
            expr->type = member_type;
          }
        } else {
          expr->binary.is_enum_member = true;
          String enum_alias = expr->binary.left->type_e.alias.name;
          Code_Type *left_base = &scope_get(scope, enum_alias)->decl->value->expr.type_e;
          Code_Type_Enum *enum_t = &left_base->enum_t;
          Scope *enum_scope = enum_t->scope;
          resolve_expr(res, enum_scope, expr->binary.right);
          
          Code_Type *member_type = 0;
          for (u32 i = 0; i < sb_count(enum_t->members); i++) {
            String member = enum_t->members[i];
            if (string_compare(member, member_name)) {
              member_type = enum_t->item_type;
              break;
            }
          }
          assert(member_type);
          check_types(member_type, expr->binary.right->type);
          expr->type = (Code_Type *)code_type_alias(res.parser, enum_alias);
        }
      } else {
        resolve_expr(res, scope, expr->binary.right);
        // TODO(lvl5): need something like get_final_type to get rid of all the type aliases
        if (expr->binary.left->type->kind == Type_Kind_PTR) {
          switch (expr->binary.op) {
            case T_ADD: {
              // TODO(lvl5): is_type_integer or something
              // or add a typeinfo into symbol table
              String type_name = expr->binary.right->type->alias.name;
              b32 legal_pointer_arithmetic = 
                string_compare(type_name, const_string("i8")) ||
                string_compare(type_name, const_string("i16")) ||
                string_compare(type_name, const_string("i32")) ||
                string_compare(type_name, const_string("i64")) ||
                string_compare(type_name, const_string("u8")) ||
                string_compare(type_name, const_string("u16")) ||
                string_compare(type_name, const_string("u32")) ||
                string_compare(type_name, const_string("u64"));
              
              assert(legal_pointer_arithmetic);
            } break;
            
            case T_SUB: {
              String type_name = expr->binary.right->type->alias.name;
              b32 legal_pointer_arithmetic = 
                expr->binary.right->type->kind == Type_Kind_PTR ||
                string_compare(type_name, const_string("i8")) ||
                string_compare(type_name, const_string("i16")) ||
                string_compare(type_name, const_string("i32")) ||
                string_compare(type_name, const_string("i64")) ||
                string_compare(type_name, const_string("u8")) ||
                string_compare(type_name, const_string("u16")) ||
                string_compare(type_name, const_string("u32")) ||
                string_compare(type_name, const_string("u64"));
              
              assert(legal_pointer_arithmetic);
            } break;
            default: assert(false);
          }
        } else {
          check_types(expr->binary.left->type, expr->binary.right->type);
        }
        expr->type = expr->binary.left->type;
      }
      
      // TODO(lvl5): implicit conversions?
    } break;
    case Expr_Kind_CALL: {
      // NOTE(lvl5): only requires signature resolve
      if (expr->call.func->kind == Expr_Kind_NAME) {
        String func_name = expr->call.func->name.name;
        resolve_name(res, scope, func_name, Resolve_State_PARTIAL);
        expr->call.func->type = scope_get(scope, func_name)->decl->type;
        
        if (expr->call.func->type == builtin_Type) {
          String name = expr->call.func->name.name;
          expr->call.func->kind = Expr_Kind_TYPE;
          expr->call.func->type_e.kind = Type_Kind_ALIAS;
          expr->call.func->type_e.alias.name = name;
        }
      } else {
        resolve_expr(res, scope, expr->call.func);
      }
      
      if (expr->call.func->type == builtin_Type) {
        expr->kind = Expr_Kind_CAST;
        Code_Type *cast_type = (Code_Type *)expr->call.func;
        Code_Expr *cast_expr = expr->call.args[0];
        expr->cast.cast_type = cast_type;
        expr->cast.expr = cast_expr;
        goto RESOLVE_CAST_LABEL;
      }
      
      Code_Stmt_Decl **params = expr->call.func->type->func.params;
      Code_Expr **args = expr->call.args;
      sb_push(args, (Code_Expr *)code_expr_name(res.parser, const_string("ctx")));
      
      assert(sb_count(args) == sb_count(params));
      for (u32 i = 0; i < sb_count(params); i++) {
        Code_Expr *arg = args[i];
        resolve_expr(res, scope, arg);
        
        Code_Stmt_Decl *param = params[i];
        check_types(arg->type, param->type);
      }
      
      expr->type = expr->call.func->type->func.return_type;
    } break;
    case Expr_Kind_CAST: {
      resolve_type(res, scope, expr->cast.cast_type);
      RESOLVE_CAST_LABEL:
      resolve_expr(res, scope, expr->cast.expr);
      
      expr->type = expr->cast.cast_type;
    } break;
    case Expr_Kind_INT: {
      expr->type = &scope_get(scope, const_string("i32"))->decl->value->expr.type_e;
    } break;
    case Expr_Kind_FLOAT: {
      expr->type = &scope_get(scope, const_string("f32"))->decl->value->expr.type_e;
    } break;
    case Expr_Kind_STRING: {
      resolve_name(res, scope, const_string("string"), Resolve_State_FULL);
      resolve_name(res, scope, const_string("__string_make"), Resolve_State_FULL);
      expr->type = (Code_Type *)code_type_alias(res.parser, const_string("string"));
    } break;
    case Expr_Kind_NAME: {
      resolve_name(res, scope, expr->name.name, Resolve_State_FULL);
      
      expr->type = scope_get(scope, expr->name.name)->decl->type;
      if (expr->type == builtin_Type) {
        String name = expr->name.name;
        expr->kind = Expr_Kind_TYPE;
        expr->type_e.kind = Type_Kind_ALIAS;
        expr->type_e.alias.name = name;
      }
      assert(expr->type);
    } break;
    
    default: assert(false);
  }
  assert(expr->type);
}

void resolve_decl_partial(Resolver res, Scope *scope, Code_Stmt_Decl *decl) {
  if (decl->type) {
    resolve_type(res, scope, decl->type);
  }
  
  if (decl->value) {
    Code_Type *value_type = 0;
    
    switch (decl->value->kind) {
      case Code_Kind_EXPR: {
        Code_Expr *expr = &decl->value->expr;
        if (expr->kind == Expr_Kind_TYPE &&
            expr->type_e.kind == Type_Kind_STRUCT) {
          value_type = builtin_Type;
          decl->check_state = Resolve_State_PARTIAL;
        } else if (expr->kind == Expr_Kind_TYPE &&
                   expr->type_e.kind == Type_Kind_FUNC) {
          Resolver child_res = res;
          child_res.is_inside_func_typedef = true;
          resolve_expr(child_res, scope, expr);
          value_type = expr->type;
          decl->check_state = Resolve_State_FULL;
        }else {
          resolve_expr(res, scope, expr);
          value_type = expr->type;
          decl->check_state = Resolve_State_FULL;
        }
      } break;
      
      case Code_Kind_FUNC: {
        Code_Func *func = &decl->value->func;
        value_type = (Code_Type *)func->type;
        
        Scope *child_scope = alloc_scope(res.arena, scope);
        func->scope = child_scope;
        resolve_type(res, child_scope, value_type);
        decl->check_state = Resolve_State_PARTIAL;
      } break;
      
      default: assert(false);
    }
    
    if (decl->type) {
      check_types(decl->type, value_type);
    } else {
      decl->type = value_type;
    }
    if (decl->type == builtin_Type) {
      assert(decl->is_const);
    }
  }
  scope_add(scope, decl);
}

void resolve_decl_full(Resolver res, Scope *scope, Code_Stmt_Decl *decl) {
  if (decl->check_state == Resolve_State_UNRESOLVED) {
    resolve_decl_partial(res, scope, decl);
  }
  
  if (decl->check_state == Resolve_State_PARTIAL) {
    if (decl->value) {
      switch (decl->value->kind) {
        case Code_Kind_EXPR: {
          resolve_expr(res, scope, &decl->value->expr);
        } break;
        
        case Code_Kind_FUNC: {
          Resolver child_res = res;
          res.current_func = &decl->value->func;
          Scope *child_scope = res.current_func->scope;
          resolve_stmt_block(res, child_scope, res.current_func->body, /* is_func_body */ true);
        } break;
        
        default: assert(false);
      }
    }
    decl->check_state = Resolve_State_FULL;
  }
}

void emit_decl(Emitter *, Code_Stmt_Decl *, Resolve_State);
void resolve_and_emit_decl(Resolver res, Scope *scope, Code_Stmt_Decl *decl, Resolve_State state) {
  if (decl->check_state < state) {
    Resolver child_res = res;
    child_res.decl = decl;
    switch (state) {
      case Resolve_State_PARTIAL:
      resolve_decl_partial(res, scope, decl);
      break;
      
      case Resolve_State_FULL:
      resolve_decl_full(res, scope, decl);
      break;
      
      default: assert(false);
    }
  }
  
  // emitting business
  
  if (decl->emit_state < state) {
    Emitter emitter = {0};
    emitter.indent = 0;
    emitter.res = &res;
    emit_decl(&emitter, decl, state);
    
    if (decl->value && decl->type->kind == Type_Kind_FUNC) {
      builder_write(const_string("\n\n"));
    } else if (decl->value && decl->value->kind == Code_Kind_EXPR &&
               decl->value->expr.kind == Expr_Kind_TYPE &&
               (decl->value->expr.type_e.kind == Type_Kind_STRUCT ||
                decl->value->expr.type_e.kind == Type_Kind_ENUM)) {
      builder_write(const_string(";\n\n"));
    } else {
      builder_write(const_string(";\n"));
    }
  }
}

void resolve_name(Resolver res, Scope *scope, String name, Resolve_State state) {
  for (u32 i = 0; i < sb_count(res.top_decls); i++) {
    Code_Stmt_Decl *decl = res.top_decls[i];
    if (string_compare(decl->name, name)) {
      resolve_and_emit_decl(res, get_global_scope(scope), decl, state);
      break;
    }
  }
}
