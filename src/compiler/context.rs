use crate::compiler::node::*;
use crate::compiler::symtable::*;
use crate::runtime::types::*;

/// Compiler context that holds shared state across compilation passes
pub struct CompileContext {
    pub types: TypeTable,
    pub strings: StringTable,
    pub scopes: ScopeStack,
}

impl CompileContext {
    pub fn new() -> Self {
        let mut ctx = Self {
            types: TypeTable::new(),
            strings: StringTable::new(),
            scopes: ScopeStack::new(),
        };
        ctx.register_builtins();
        ctx
    }

    fn register_builtins(&mut self) {
        // Register built-in print function at slot 7 (BuiltIn::FuncPrint)
        let mut print_fn = FunctionType::new("print(message:)".into(), true);
        print_fn.func_kind = FuncKind::NativeFunc;
        print_fn.param_count = 1;
        print_fn.local_vars.push(Variable::new("message".into()));
        print_fn.return_type_slot = ValueType::Void as Slot;
        self.types.register(JoyeerType::Function(print_fn));
        // Additional builtins can be registered here later
    }

    /// Look up a symbol by name across all scopes
    pub fn lookup(&self, name: &str) -> Option<&Symbol> {
        self.scopes.lookup(name)
    }

    /// Register a type and return its slot
    pub fn declare_type(&mut self, ty: JoyeerType) -> Slot {
        self.types.register(ty)
    }

    pub fn get_type(&self, slot: Slot) -> &JoyeerType {
        self.types.get(slot)
    }

    pub fn get_type_mut(&mut self, slot: Slot) -> &mut JoyeerType {
        self.types.get_mut(slot)
    }
}

// ── Type Generation Pass ──────────────────────────

/// First pass: create type entries for all declarations
pub fn type_gen(ctx: &mut CompileContext, node: &mut Node) {
    match node {
        Node::Module(module) => {
            // Create module type
            let mod_type = ModuleType::new(module.filename.clone());
            let slot = ctx.declare_type(JoyeerType::Module(mod_type));
            module.type_slot = slot;

            // Push module scope
            ctx.scopes.push();

            // Register print built-in in scope
            let mut print_sym = Symbol::new(SymbolKind::Func, "print(message:)".into());
            print_sym.type_slot = BuiltIn::FuncPrint as Slot;
            ctx.scopes.define(print_sym);

            // Process declarations
            for stmt in &mut module.statements {
                type_gen(ctx, stmt);
            }

            ctx.scopes.pop();
        }
        Node::VarDecl(decl) => {
            let mut sym = Symbol::new(SymbolKind::Var, decl.pattern.name.clone());
            // Determine type from annotation or infer later
            if let Some(ref type_ann) = decl.pattern.type_annotation {
                sym.type_slot = resolve_type_name(ctx, type_ann);
            }
            ctx.scopes.define(sym);
        }
        Node::FuncDecl(decl) => {
            let mut func = FunctionType::new(decl.name.clone(), true);
            func.func_kind = FuncKind::VMFunc;
            func.param_count = decl.params.len();

            for p in &decl.params {
                let mut v = Variable::new(p.name.clone());
                if let Some(ref t) = p.type_annotation {
                    v.type_slot = resolve_type_name(ctx, t);
                }
                func.local_vars.push(v);
            }

            if let Some(ref ret_type) = decl.return_type {
                func.return_type_slot = resolve_type_name_from_node(ctx, ret_type);
            }

            let slot = ctx.declare_type(JoyeerType::Function(func));
            decl.type_slot = slot;

            // Build callee name: funcName(param1:param2:)
            let callee_name = build_func_callee_name(&decl.name, &decl.params);
            let mut sym = Symbol::new(SymbolKind::Func, callee_name);
            sym.type_slot = slot;
            ctx.scopes.define(sym);
        }
        Node::ClassDecl(decl) => {
            let mut class = ClassType::new(decl.name.clone());

            // Pre-register slot
            let slot = ctx.declare_type(JoyeerType::Class(class));
            decl.type_slot = slot;

            let mut sym = Symbol::new(SymbolKind::Class, decl.name.clone());
            sym.type_slot = slot;
            ctx.scopes.define(sym);

            // Process members to collect fields and methods
            ctx.scopes.push();
            let mut field_index = 0;
            for m in &mut decl.members {
                match m.as_ref() {
                    Node::VarDecl(vd) => {
                        // Register as field
                        let mut field_sym = Symbol::new(SymbolKind::Var, vd.pattern.name.clone());
                        field_sym.location = field_index;
                        if let Some(ref t) = vd.pattern.type_annotation {
                            field_sym.type_slot = resolve_type_name(ctx, t);
                        }
                        ctx.scopes.define(field_sym);

                        // Add to class type
                        let mut var = Variable::new(vd.pattern.name.clone());
                        if let Some(ref t) = vd.pattern.type_annotation {
                            var.type_slot = resolve_type_name(ctx, t);
                        }
                        if let JoyeerType::Class(c) = ctx.get_type_mut(slot) {
                            c.instance_fields.push(var);
                        }
                        field_index += 1;
                    }
                    _ => {
                        type_gen(ctx, m);
                    }
                }
            }
            ctx.scopes.pop();
        }
        _ => {}
    }
}

/// Second pass: type-bind expressions (simplified for initial version)
pub fn type_bind(ctx: &mut CompileContext, node: &mut Node) {
    match node {
        Node::Module(module) => {
            ctx.scopes.push();

            // Re-register print
            let mut print_sym = Symbol::new(SymbolKind::Func, "print(message:)".into());
            print_sym.type_slot = BuiltIn::FuncPrint as Slot;
            ctx.scopes.define(print_sym);

            // Register all top-level declarations first
            for stmt in &mut module.statements {
                pre_bind_decl(ctx, stmt);
            }

            // Bind each statement
            let stmts: Vec<_> = (0..module.statements.len()).collect();
            for i in stmts {
                type_bind(ctx, &mut module.statements[i]);
            }

            ctx.scopes.pop();
        }
        Node::VarDecl(decl) => {
            if let Some(ref mut init) = decl.initializer {
                type_bind(ctx, init);
                decl.type_slot = init.type_slot();
            }
            // Update symbol with resolved type
            let name = decl.pattern.name.clone();
            if let Some(sym) = ctx.scopes.lookup(&name) {
                if sym.type_slot == -1 {
                    // Update the symbol's type_slot
                    let ts = decl.type_slot;
                    let sym = Symbol::new(SymbolKind::Var, name);
                    // We need a mutable lookup - for simplicity, redefine
                    let mut new_sym = sym;
                    new_sym.type_slot = ts;
                    ctx.scopes.define(new_sym);
                }
            }
        }
        Node::FuncDecl(decl) => {
            ctx.scopes.push();
            // Register params as local vars
            for p in &decl.params {
                let mut sym = Symbol::new(SymbolKind::Var, p.name.clone());
                if let Some(ref t) = p.type_annotation {
                    sym.type_slot = resolve_type_name(ctx, t);
                }
                ctx.scopes.define(sym);
            }
            type_bind(ctx, &mut decl.body);
            ctx.scopes.pop();
        }
        Node::StmtsBlock(block) => {
            for stmt in &mut block.statements {
                type_bind(ctx, stmt);
            }
        }
        Node::Expr(expr) => {
            type_bind(ctx, &mut expr.prefix);
            for bin in &mut expr.binaries {
                type_bind(ctx, bin);
            }
            expr.type_slot = expr.prefix.type_slot();
        }
        Node::BinaryExpr(expr) => {
            type_bind(ctx, &mut expr.expr);
            expr.type_slot = expr.expr.type_slot();
        }
        Node::AssignExpr(expr) => {
            type_bind(ctx, &mut expr.expr);
            expr.type_slot = expr.expr.type_slot();
        }
        Node::PrefixExpr(expr) => {
            type_bind(ctx, &mut expr.expr);
            expr.type_slot = expr.expr.type_slot();
        }
        Node::PostfixExpr(expr) => {
            type_bind(ctx, &mut expr.expr);
            expr.type_slot = expr.expr.type_slot();
        }
        Node::IdentifierExpr(expr) => {
            if let Some(sym) = ctx.scopes.lookup(&expr.name) {
                expr.type_slot = sym.type_slot;
            }
        }
        Node::LiteralExpr(expr) => {
            use crate::compiler::token::TokenKind;
            match expr.token.kind {
                TokenKind::DecimalLiteral => {
                    expr.type_slot = ValueType::Int as Slot;
                }
                TokenKind::BooleanLiteral => {
                    expr.type_slot = ValueType::Bool as Slot;
                }
                TokenKind::StringLiteral => {
                    expr.type_slot = ValueType::String as Slot;
                }
                TokenKind::NilLiteral => {
                    expr.type_slot = ValueType::Nil as Slot;
                }
                _ => {}
            }
        }
        Node::FuncCallExpr(call) => {
            // Resolve function
            let callee_name = build_call_name(call);
            if let Some(sym) = ctx.scopes.lookup(&callee_name) {
                call.func_type_slot = sym.type_slot;
                // Get return type
                if let JoyeerType::Function(f) = ctx.get_type(sym.type_slot) {
                    call.type_slot = f.return_type_slot;
                }
            }
            // Bind arguments
            for arg in &mut call.arguments {
                type_bind(ctx, &mut arg.expr);
            }
        }
        Node::MemberAccessExpr(expr) => {
            type_bind(ctx, &mut expr.callee);
            // member binding would need class field resolution - simplified
        }
        Node::MemberFuncCallExpr(expr) => {
            type_bind(ctx, &mut expr.callee);
            type_bind(ctx, &mut expr.member);
        }
        Node::SubscriptExpr(expr) => {
            type_bind(ctx, &mut expr.callee);
            type_bind(ctx, &mut expr.index);
        }
        Node::ArrayLiteralExpr(expr) => {
            for elem in &mut expr.elements {
                type_bind(ctx, elem);
            }
        }
        Node::DictLiteralExpr(expr) => {
            for (k, v) in &mut expr.entries {
                type_bind(ctx, k);
                type_bind(ctx, v);
            }
        }
        Node::ParenthesizedExpr(expr) => {
            type_bind(ctx, &mut expr.expr);
            expr.type_slot = expr.expr.type_slot();
        }
        Node::IfStmt(stmt) => {
            type_bind(ctx, &mut stmt.condition);
            type_bind(ctx, &mut stmt.then_block);
            if let Some(ref mut else_block) = stmt.else_block {
                type_bind(ctx, else_block);
            }
        }
        Node::WhileStmt(stmt) => {
            type_bind(ctx, &mut stmt.condition);
            type_bind(ctx, &mut stmt.body);
        }
        Node::ForInStmt(stmt) => {
            type_bind(ctx, &mut stmt.iterable);
            type_bind(ctx, &mut stmt.body);
        }
        Node::ReturnStmt(stmt) => {
            if let Some(ref mut expr) = stmt.expr {
                type_bind(ctx, expr);
            }
        }
        _ => {}
    }
}

fn pre_bind_decl(ctx: &mut CompileContext, node: &mut Node) {
    match node {
        Node::VarDecl(decl) => {
            let mut sym = Symbol::new(SymbolKind::Var, decl.pattern.name.clone());
            if let Some(ref t) = decl.pattern.type_annotation {
                sym.type_slot = resolve_type_name(ctx, t);
            }
            ctx.scopes.define(sym);
        }
        Node::FuncDecl(decl) => {
            let callee_name = build_func_callee_name(&decl.name, &decl.params);
            let mut sym = Symbol::new(SymbolKind::Func, callee_name);
            sym.type_slot = decl.type_slot;
            ctx.scopes.define(sym);
        }
        _ => {}
    }
}

fn resolve_type_name(ctx: &CompileContext, name: &str) -> Slot {
    match name {
        "Int" => ValueType::Int as Slot,
        "Bool" => ValueType::Bool as Slot,
        "String" => ValueType::String as Slot,
        "Void" => ValueType::Void as Slot,
        "Any" => ValueType::Any as Slot,
        _ => {
            // Look up in type table
            if let Some(sym) = ctx.scopes.lookup(name) {
                sym.type_slot
            } else {
                -1
            }
        }
    }
}

fn resolve_type_name_from_node(ctx: &CompileContext, node: &Node) -> Slot {
    resolve_type_name(ctx, &node.simple_name())
}

fn build_func_callee_name(name: &str, params: &[Pattern]) -> String {
    let mut s = String::from(name);
    s.push('(');
    for p in params {
        s.push_str(&p.name);
        s.push(':');
    }
    s.push(')');
    s
}

fn build_call_name(call: &FuncCallExpr) -> String {
    let mut s = call.callee.simple_name();
    s.push('(');
    for arg in &call.arguments {
        s.push_str(&arg.label);
        s.push(':');
    }
    s.push(')');
    s
}
