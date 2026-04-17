use crate::compiler::context::CompileContext;
use crate::compiler::node::*;
use crate::compiler::token::{TokenKind, literals, operators};
use crate::runtime::bytecode::*;
use crate::runtime::types::*;

/// Whether an operator is high precedence (* / %) or low (+ - && etc.)
fn is_high_priority(op: &str) -> bool {
    matches!(op, "*" | "/" | "%" | ">" | ">=" | "<" | "<=" | "!=" | "==" | "!" | "?")
}

/// IR Generation: AST → Bytecode
pub struct IRGen<'a> {
    ctx: &'a mut CompileContext,
    writer: BytecodeWriter,
}

impl<'a> IRGen<'a> {
    pub fn new(ctx: &'a mut CompileContext) -> Self {
        Self {
            ctx,
            writer: BytecodeWriter::new(),
        }
    }

    /// Emit bytecodes for the module, returning the module type slot
    pub fn emit_module(mut self, module: &ModuleDecl) -> Slot {
        // Push module scope
        self.ctx.scopes.push();

        // Register print built-in
        {
            use crate::compiler::symtable::*;
            let mut sym = Symbol::new(SymbolKind::Func, "print(message:)".into());
            sym.type_slot = BuiltIn::FuncPrint as Slot;
            self.ctx.scopes.define(sym);
        }

        // Register all top-level decls in scope
        for stmt in &module.statements {
            self.pre_register(stmt);
        }

        // Process class declarations first (they need to be defined before vars can reference them)
        for stmt in &module.statements {
            if let Node::ClassDecl(decl) = stmt.as_ref() {
                self.emit_class_decl(decl);
            }
        }

        // Track local var slots for top-level vars
        let mut local_slot: i64 = 0;
        for stmt in &module.statements {
            match stmt.as_ref() {
                Node::VarDecl(decl) => {
                    use crate::compiler::symtable::*;
                    let mut sym = Symbol::new(SymbolKind::Var, decl.pattern.name.clone());
                    sym.type_slot = self.infer_var_type(decl);
                    sym.location = local_slot as i32;
                    self.ctx.scopes.define(sym);
                    local_slot += 1;
                }
                _ => {}
            }
        }

        // Emit statements (skip class decls, already processed)
        for stmt in &module.statements {
            if matches!(stmt.as_ref(), Node::ClassDecl(_)) {
                continue;
            }
            self.emit(stmt);
        }

        self.writer.write(Bytecode::new(Opcode::Return, 0));

        // Create module initializer function
        let mut init_fn = FunctionType::new(module.filename.clone(), true);
        init_fn.func_kind = FuncKind::VMFunc;
        init_fn.bytecodes = Some(self.writer.into_bytecodes());

        // Count local vars from module
        let mut local_var_count = 0;
        for stmt in &module.statements {
            if matches!(stmt.as_ref(), Node::VarDecl(_)) {
                let name = stmt.simple_name();
                init_fn.local_vars.push(Variable::new(name));
                local_var_count += 1;
            }
        }

        let init_slot = self.ctx.declare_type(JoyeerType::Function(init_fn));

        self.ctx.scopes.pop();

        // Update module with init slot
        if let JoyeerType::Module(m) = self.ctx.get_type_mut(module.type_slot) {
            m.static_initializer_slot = init_slot;
        }

        module.type_slot
    }

    fn pre_register(&mut self, node: &Node) {
        use crate::compiler::symtable::*;
        match node {
            Node::FuncDecl(decl) => {
                let callee_name = self.build_func_name(&decl.name, &decl.params);
                let mut sym = Symbol::new(SymbolKind::Func, callee_name);
                sym.type_slot = decl.type_slot;
                self.ctx.scopes.define(sym);
            }
            Node::ClassDecl(decl) => {
                let mut sym = Symbol::new(SymbolKind::Class, decl.name.clone());
                sym.type_slot = decl.type_slot;
                self.ctx.scopes.define(sym);
            }
            _ => {}
        }
    }

    fn emit(&mut self, node: &Node) {
        match node {
            Node::Module(_) => unreachable!("use emit_module"),
            Node::VarDecl(decl) => self.emit_var_decl(decl),
            Node::FuncDecl(decl) => self.emit_func_decl(decl),
            Node::ClassDecl(decl) => self.emit_class_decl(decl),
            Node::StmtsBlock(block) => {
                for stmt in &block.statements {
                    self.emit(stmt);
                }
            }
            Node::IfStmt(stmt) => self.emit_if(stmt),
            Node::WhileStmt(stmt) => self.emit_while(stmt),
            Node::ReturnStmt(stmt) => self.emit_return(stmt),
            Node::Expr(expr) => self.emit_expr(expr),
            Node::LiteralExpr(expr) => self.emit_literal(expr),
            Node::IdentifierExpr(expr) => self.emit_identifier(expr),
            Node::FuncCallExpr(call) => self.emit_func_call(call),
            Node::MemberAccessExpr(expr) => self.emit_member_access(expr),
            Node::MemberFuncCallExpr(expr) => self.emit_member_func_call(expr),
            Node::ArguCallExpr(arg) => {
                self.emit(&arg.expr);
            }
            Node::PrefixExpr(expr) => self.emit_prefix(expr),
            Node::PostfixExpr(expr) => {
                self.emit(&expr.expr);
            }
            Node::BinaryExpr(expr) => {
                self.emit(&expr.expr);
            }
            Node::AssignExpr(expr) => {
                self.emit(&expr.expr);
            }
            Node::ParenthesizedExpr(expr) => {
                self.emit(&expr.expr);
            }
            Node::ArrayLiteralExpr(expr) => self.emit_array_literal(expr),
            Node::DictLiteralExpr(expr) => self.emit_dict_literal(expr),
            Node::SubscriptExpr(expr) => self.emit_subscript(expr),
            Node::SelfNode(_) => {
                // Push self reference
                self.writer.write(Bytecode::new(Opcode::OLoad, 0));
            }
            Node::SelfExpr(expr) => {
                // self.field — load self, then get field
                self.writer.write(Bytecode::new(Opcode::OLoad, 0));
                let field_name = expr.member.simple_name();
                // Look up as a field specifically
                if let Some(loc) = self.find_field_location(&field_name) {
                    self.writer.write(Bytecode::new(Opcode::IConst, loc as i64));
                    self.writer.write(Bytecode::new(Opcode::GetField, 0));
                }
            }
            Node::OperatorExpr(_) => {}
            _ => {}
        }
    }

    fn emit_var_decl(&mut self, decl: &VarDecl) {
        if let Some(ref init) = decl.initializer {
            self.emit(init);
            // Find the local slot
            if let Some(sym) = self.ctx.scopes.lookup(&decl.pattern.name) {
                self.writer
                    .write(Bytecode::new(Opcode::IStore, sym.location as i64));
            }
        }
    }

    fn emit_func_decl(&mut self, decl: &FuncDecl) {
        let mut sub_writer = BytecodeWriter::new();
        std::mem::swap(&mut self.writer, &mut sub_writer);

        self.ctx.scopes.push();

        // Register params as locals (index 0..param_count-1)
        for (i, p) in decl.params.iter().enumerate() {
            use crate::compiler::symtable::*;
            let mut sym = Symbol::new(SymbolKind::Var, p.name.clone());
            sym.location = i as i32;
            if let Some(ref t) = p.type_annotation {
                sym.type_slot = self.resolve_type_name(t);
            }
            self.ctx.scopes.define(sym);
        }

        // Pre-scan body for local var declarations to assign slots
        let param_count = decl.params.len();
        if let Node::StmtsBlock(block) = decl.body.as_ref() {
            let mut local_idx = param_count;
            for stmt in &block.statements {
                if let Node::VarDecl(vd) = stmt.as_ref() {
                    use crate::compiler::symtable::*;
                    let mut sym = Symbol::new(SymbolKind::Var, vd.pattern.name.clone());
                    sym.location = local_idx as i32;
                    sym.type_slot = self.infer_var_type(vd);
                    self.ctx.scopes.define(sym);
                    local_idx += 1;
                }
            }
        }

        self.emit(&decl.body);

        self.ctx.scopes.pop();

        std::mem::swap(&mut self.writer, &mut sub_writer);

        let bytecodes = sub_writer.into_bytecodes();
        if let JoyeerType::Function(f) = self.ctx.get_type_mut(decl.type_slot) {
            f.bytecodes = Some(bytecodes);
            // Also count local vars
            if let Node::StmtsBlock(block) = decl.body.as_ref() {
                for stmt in &block.statements {
                    if let Node::VarDecl(vd) = stmt.as_ref() {
                        f.local_vars.push(Variable::new(vd.pattern.name.clone()));
                    }
                }
            }
        }
    }
    fn emit_class_decl(&mut self, decl: &ClassDecl) {
        // Collect field default values as bytecodes for the class initializer
        // We build a small bytecode sequence: for each field with an initializer,
        // emit: DUP (obj ref), ICONST(field_index), <init_expr>, PUTFIELD
        let class_slot = decl.type_slot;

        // Create an init function that sets default field values
        let mut init_writer = BytecodeWriter::new();

        // The init function receives the object ref as local var 0
        self.ctx.scopes.push();
        {
            use crate::compiler::symtable::*;
            let mut self_sym = Symbol::new(SymbolKind::Var, "self".into());
            self_sym.location = 0;
            self_sym.type_slot = class_slot;
            self.ctx.scopes.define(self_sym);
        }

        let mut field_idx = 0;
        for m in &decl.members {
            if let Node::VarDecl(vd) = m.as_ref() {
                if let Some(ref init) = vd.initializer {
                    // OLoad 0 (self), ICONST field_idx, <init_expr>, PUTFIELD
                    init_writer.write(Bytecode::new(Opcode::OLoad, 0));
                    init_writer.write(Bytecode::new(Opcode::IConst, field_idx));

                    // We need to emit the initializer — swap writers
                    std::mem::swap(&mut self.writer, &mut init_writer);
                    self.emit(init);
                    std::mem::swap(&mut self.writer, &mut init_writer);

                    init_writer.write(Bytecode::new(Opcode::PutField, 0));
                }
                field_idx += 1;
            }
        }

        // Return self
        init_writer.write(Bytecode::new(Opcode::OLoad, 0));
        init_writer.write(Bytecode::new(Opcode::IReturn, 0));

        self.ctx.scopes.pop();

        // Register the init function
        let mut init_fn = FunctionType::new(format!("{}.<init>", decl.name), false);
        init_fn.func_kind = FuncKind::VMFunc;
        init_fn.param_count = 1; // self
        init_fn.local_vars.push(Variable::new("self".into()));
        init_fn.return_type_slot = class_slot;
        init_fn.bytecodes = Some(init_writer.into_bytecodes());
        let init_slot = self.ctx.declare_type(JoyeerType::Function(init_fn));

        // Update class with init slot
        if let JoyeerType::Class(c) = self.ctx.get_type_mut(class_slot) {
            c.default_initializer_slot = init_slot;
        }

        // Also handle member functions and custom constructors
        for m in &decl.members {
            if let Node::FuncDecl(fd) = m.as_ref() {
                if fd.is_constructor {
                    // Custom init constructor
                    self.emit_class_constructor(decl, fd);
                } else {
                    // Regular class method
                    self.emit_class_method(decl, fd);
                }
            }
        }
    }

    fn emit_class_constructor(&mut self, class_decl: &ClassDecl, func_decl: &FuncDecl) {
        let class_slot = class_decl.type_slot;
        let mut sub_writer = BytecodeWriter::new();
        std::mem::swap(&mut self.writer, &mut sub_writer);

        self.ctx.scopes.push();

        // Register "self" as local 0
        {
            use crate::compiler::symtable::*;
            let mut self_sym = Symbol::new(SymbolKind::Var, "self".into());
            self_sym.location = 0;
            self_sym.type_slot = class_slot;
            self.ctx.scopes.define(self_sym);
        }

        // Register class fields for self.field access
        let fields: Vec<(String, i32, Slot)> = if let JoyeerType::Class(c) = self.ctx.get_type(class_slot) {
            c.instance_fields.iter().enumerate()
                .map(|(i, f)| (f.name.clone(), i as i32, f.type_slot))
                .collect()
        } else {
            Vec::new()
        };
        for (name, loc, ts) in &fields {
            use crate::compiler::symtable::*;
            let mut sym = Symbol::new(SymbolKind::Field, name.clone());
            sym.location = *loc;
            sym.type_slot = *ts;
            sym.parent_type_slot = class_slot;
            self.ctx.scopes.define(sym);
        }

        // Register params as locals starting at index 1
        for (i, p) in func_decl.params.iter().enumerate() {
            use crate::compiler::symtable::*;
            let mut sym = Symbol::new(SymbolKind::Var, p.name.clone());
            sym.location = (i + 1) as i32;
            if let Some(ref t) = p.type_annotation {
                sym.type_slot = self.resolve_type_name(t);
            }
            self.ctx.scopes.define(sym);
        }

        // Emit body
        self.emit(&func_decl.body);

        // Return self
        self.writer.write(Bytecode::new(Opcode::OLoad, 0));
        self.writer.write(Bytecode::new(Opcode::OReturn, 0));

        self.ctx.scopes.pop();
        std::mem::swap(&mut self.writer, &mut sub_writer);

        // Create the constructor function type
        let mut ctor = FunctionType::new(format!("{}.init", class_decl.name), false);
        ctor.func_kind = FuncKind::VMFunc;
        ctor.param_count = func_decl.params.len() + 1; // +1 for self
        ctor.local_vars.push(Variable::new("self".into()));
        for p in &func_decl.params {
            ctor.local_vars.push(Variable::new(p.name.clone()));
        }
        ctor.return_type_slot = class_slot;
        ctor.bytecodes = Some(sub_writer.into_bytecodes());
        let ctor_slot = self.ctx.declare_type(JoyeerType::Function(ctor));

        // Register with callee name: "ClassName.init(param1:param2:)"
        {
            use crate::compiler::symtable::*;
            let mut callee_name = format!("{}.init(", class_decl.name);
            for p in &func_decl.params {
                callee_name.push_str(&p.name);
                callee_name.push(':');
            }
            callee_name.push(')');
            let mut sym = Symbol::new(SymbolKind::Constructor, callee_name);
            sym.type_slot = ctor_slot;
            self.ctx.scopes.define(sym);
        }
    }

    fn emit_class_method(&mut self, class_decl: &ClassDecl, func_decl: &FuncDecl) {
        let class_slot = class_decl.type_slot;
        let mut sub_writer = BytecodeWriter::new();
        std::mem::swap(&mut self.writer, &mut sub_writer);

        self.ctx.scopes.push();

        // Register "self" as local 0
        {
            use crate::compiler::symtable::*;
            let mut self_sym = Symbol::new(SymbolKind::Var, "self".into());
            self_sym.location = 0;
            self_sym.type_slot = class_slot;
            self.ctx.scopes.define(self_sym);
        }

        // Register class fields in scope for direct access (like `i` in class methods)
        let fields: Vec<(String, i32, Slot)> = if let JoyeerType::Class(c) = self.ctx.get_type(class_slot) {
            c.instance_fields.iter().enumerate()
                .map(|(i, f)| (f.name.clone(), i as i32, f.type_slot))
                .collect()
        } else {
            Vec::new()
        };
        for (name, loc, ts) in fields {
            use crate::compiler::symtable::*;
            let mut sym = Symbol::new(SymbolKind::Field, name);
            sym.location = loc;
            sym.type_slot = ts;
            sym.parent_type_slot = class_slot;
            self.ctx.scopes.define(sym);
        }

        // Register params as locals starting at index 1
        for (i, p) in func_decl.params.iter().enumerate() {
            use crate::compiler::symtable::*;
            let mut sym = Symbol::new(SymbolKind::Var, p.name.clone());
            sym.location = (i + 1) as i32;
            if let Some(ref t) = p.type_annotation {
                sym.type_slot = self.resolve_type_name(t);
            }
            self.ctx.scopes.define(sym);
        }

        // Pre-scan body for local var declarations
        let next_local = 1 + func_decl.params.len();
        if let Node::StmtsBlock(block) = func_decl.body.as_ref() {
            let mut local_idx = next_local;
            for stmt in &block.statements {
                if let Node::VarDecl(vd) = stmt.as_ref() {
                    use crate::compiler::symtable::*;
                    let mut sym = Symbol::new(SymbolKind::Var, vd.pattern.name.clone());
                    sym.location = local_idx as i32;
                    self.ctx.scopes.define(sym);
                    local_idx += 1;
                }
            }
        }

        self.emit(&func_decl.body);

        self.ctx.scopes.pop();
        std::mem::swap(&mut self.writer, &mut sub_writer);

        // Create the function type
        let method_name = format!("{}.{}", class_decl.name, func_decl.name);
        let callee_name = {
            let mut s = func_decl.name.clone();
            s.push('(');
            for p in &func_decl.params {
                s.push_str(&p.name);
                s.push(':');
            }
            s.push(')');
            s
        };

        let mut method = FunctionType::new(method_name, false);
        method.func_kind = FuncKind::VMFunc;
        method.param_count = func_decl.params.len() + 1; // +1 for self
        method.local_vars.push(Variable::new("self".into()));
        for p in &func_decl.params {
            method.local_vars.push(Variable::new(p.name.clone()));
        }
        // Add body local vars
        if let Node::StmtsBlock(block) = func_decl.body.as_ref() {
            for stmt in &block.statements {
                if let Node::VarDecl(vd) = stmt.as_ref() {
                    method.local_vars.push(Variable::new(vd.pattern.name.clone()));
                }
            }
        }
        method.return_type_slot = ValueType::Void as Slot;
        method.bytecodes = Some(sub_writer.into_bytecodes());

        let method_slot = self.ctx.declare_type(JoyeerType::Function(method));

        // Register in the class's method table (store as a mapping from callee_name to method_slot)
        // For now, register in the current scope for member function call resolution
        // We'll need a way to resolve a.print(j:20) → class A's print(j:) method
        // Store in a special naming convention: "ClassName.methodName(params:)"
        {
            use crate::compiler::symtable::*;
            let full_name = format!("{}.{}", class_decl.name, callee_name);
            let mut sym = Symbol::new(SymbolKind::Func, full_name);
            sym.type_slot = method_slot;
            self.ctx.scopes.define(sym);
        }
    }
    fn emit_expr(&mut self, expr: &Expr) {
        // Handle assignment: prefix = expr
        if expr.binaries.len() == 1 {
            if let Node::AssignExpr(assign) = expr.binaries[0].as_ref() {

                // Check if assigning to a subscript: array[i] = value
                if let Node::SubscriptExpr(sub) = expr.prefix.as_ref() {
                    self.emit(&sub.callee);   // push array/dict ref
                    self.emit(&sub.index);     // push index
                    self.emit(&assign.expr);   // push new value
                    self.writer.write(Bytecode::new(Opcode::PutField, 0));
                    return;
                }
                // Check if assigning to a member: obj.field = value
                if let Node::MemberAccessExpr(mac) = expr.prefix.as_ref() {
                    self.emit(&mac.callee);     // push object ref
                    let member_name = mac.member.simple_name();
                    let field_index = self.resolve_field_index_from_expr(&mac.callee, &member_name);
                    self.writer.write(Bytecode::new(Opcode::IConst, field_index));
                    self.emit(&assign.expr);    // push new value
                    self.writer.write(Bytecode::new(Opcode::PutField, 0));
                    return;
                }
                // Check if assigning to self.field
                if let Node::SelfExpr(se) = expr.prefix.as_ref() {
                    let field_name = se.member.simple_name();
                    // Look up field by scanning for a Field symbol specifically
                    let field_loc = self.find_field_location(&field_name);
                    if let Some(loc) = field_loc {
                        self.writer.write(Bytecode::new(Opcode::OLoad, 0)); // self
                        self.writer.write(Bytecode::new(Opcode::IConst, loc as i64)); // field index
                        self.emit(&assign.expr); // value
                        self.writer.write(Bytecode::new(Opcode::PutField, 0));
                        return;
                    }
                }
                // Regular variable assignment
                self.emit(&assign.expr);
                if let Node::IdentifierExpr(id) = expr.prefix.as_ref() {
                    if let Some(sym) = self.ctx.scopes.lookup(&id.name) {
                        self.writer
                            .write(Bytecode::new(Opcode::IStore, sym.location as i64));
                    }
                }
                return;
            }
        }

        // Flatten prefix + binaries into [operand, op_str, operand, op_str, ...]
        // Then apply shunting-yard to respect operator precedence.

        // Collect operands and operators in order
        struct FlatItem<'b> {
            node: &'b Node,
            op: Option<String>,
        }

        let mut items: Vec<FlatItem> = Vec::new();
        items.push(FlatItem { node: expr.prefix.as_ref(), op: None });

        for bin in &expr.binaries {
            if let Node::BinaryExpr(binary) = bin.as_ref() {
                items.push(FlatItem { node: bin.as_ref(), op: Some(binary.op.raw_value.clone()) });
            }
        }

        // If only one item (no binaries), just emit the prefix
        if items.len() == 1 {
            self.emit(items[0].node);
            return;
        }

        // Shunting-yard: fold high-priority ops first, then emit low-priority in order
        // Build a list of "terms" (already folded high-priority) and low-priority operators

        // Phase 1: Walk through, fold high-priority operations
        // temps = deque of "emitter closures" (we'll represent as indices)
        // We process left-to-right, when we hit a high-priority op, we immediately group it

        // Simpler approach: directly emit with correct precedence
        // Collect: operands[0] op[0] operands[1] op[1] operands[2] ...
        let mut operands: Vec<&Node> = Vec::new();
        let mut ops: Vec<String> = Vec::new();

        operands.push(expr.prefix.as_ref());
        for bin in &expr.binaries {
            if let Node::BinaryExpr(binary) = bin.as_ref() {
                ops.push(binary.op.raw_value.clone());
                operands.push(binary.expr.as_ref());
            }
        }

        // Phase 1: Fold high-priority ops into sub-groups
        // Result: terms[] and low_ops[]
        // Start with first operand
        let mut term_groups: Vec<Vec<(usize, Option<String>)>> = Vec::new();
        // Each group: sequence of (operand_index, op_to_apply_after)

        let mut current_group: Vec<(usize, Option<String>)> = vec![(0, None)];

        for i in 0..ops.len() {
            if is_high_priority(&ops[i]) {
                // Add to current group
                current_group.last_mut().unwrap().1 = Some(ops[i].clone());
                current_group.push((i + 1, None));
            } else {
                // Close current group, start new one
                term_groups.push(current_group);
                current_group = vec![(i + 1, None)];
            }
        }
        term_groups.push(current_group);

        // Collect low-priority ops
        let low_ops: Vec<&str> = ops.iter()
            .filter(|op| !is_high_priority(op))
            .map(|s| s.as_str())
            .collect();

        // Phase 2: Emit each term group, then apply low-priority ops
        for (gi, group) in term_groups.iter().enumerate() {
            // Emit first operand of the group
            self.emit(operands[group[0].0]);

            // For each subsequent item in the group, emit operand then apply high-priority op
            for j in 0..group.len() {
                if let Some(ref op) = group[j].1 {
                    self.emit(operands[group[j + 1].0]);
                    self.emit_binary_op(op);
                }
            }

            // Apply low-priority op between groups
            if gi > 0 && gi - 1 < low_ops.len() {
                self.emit_binary_op(low_ops[gi - 1]);
            }
        }
    }

    fn emit_binary_op(&mut self, op: &str) {
        let opcode = match op {
            "+" => Opcode::IAdd,
            "-" => Opcode::ISub,
            "*" => Opcode::IMul,
            "/" => Opcode::IDiv,
            "%" => Opcode::IRem,
            ">" => Opcode::ICmpG,
            ">=" => Opcode::ICmpGE,
            "<" => Opcode::ICmpL,
            "<=" => Opcode::ICmpLE,
            "!=" => Opcode::ICmpNE,
            "==" => Opcode::ICmpEQ,
            "&&" => Opcode::IAnd,
            _ => return,
        };
        self.writer.write(Bytecode::new(opcode, 0));
    }

    fn emit_literal(&mut self, expr: &LiteralExpr) {
        match expr.token.kind {
            TokenKind::DecimalLiteral => {
                self.writer
                    .write(Bytecode::new(Opcode::IConst, expr.token.int_value));
            }
            TokenKind::BooleanLiteral => {
                let val = if expr.token.raw_value == literals::TRUE {
                    1
                } else {
                    0
                };
                self.writer.write(Bytecode::new(Opcode::IConst, val));
            }
            TokenKind::StringLiteral => {
                let slot = self.ctx.strings.import(&expr.token.raw_value);
                self.writer.write(Bytecode::new(Opcode::SConst, slot));
            }
            TokenKind::NilLiteral => {
                self.writer.write(Bytecode::new(Opcode::OConstNil, 0));
            }
            _ => {}
        }
    }

    fn emit_identifier(&mut self, expr: &IdentifierExpr) {
        if let Some(sym) = self.ctx.scopes.lookup(&expr.name) {
            use crate::compiler::symtable::SymbolKind;
            if sym.kind == SymbolKind::Field {
                // Class field access: emit self.field
                // OLoad 0 (self), IConst(field_index), GetField
                self.writer.write(Bytecode::new(Opcode::OLoad, 0));
                self.writer.write(Bytecode::new(Opcode::IConst, sym.location as i64));
                self.writer.write(Bytecode::new(Opcode::GetField, 0));
            } else {
                self.writer
                    .write(Bytecode::new(Opcode::OLoad, sym.location as i64));
            }
        }
    }

    fn emit_func_call(&mut self, call: &FuncCallExpr) {
        // Check for member function calls like array.size() or obj.method()
        if let Node::MemberAccessExpr(mac) = call.callee.as_ref() {
            let member_name = mac.member.simple_name();
            match member_name.as_str() {
                "size" => {
                    self.emit(&mac.callee);
                    self.writer.write(Bytecode::new(Opcode::Debug, 2));
                    return;
                }
                _ => {
                    // Try to resolve as a class method: ClassName.methodName(params:)
                    let class_name = self.resolve_class_name_of(&mac.callee);
                    if let Some(class_name) = class_name {
                        let mut method_callee = format!("{}.{}(", class_name, member_name);
                        for arg in &call.arguments {
                            method_callee.push_str(&arg.label);
                            method_callee.push(':');
                        }
                        method_callee.push(')');

                        if let Some(sym) = self.ctx.scopes.lookup(&method_callee) {
                            let method_slot = sym.type_slot;
                            // Push self (the object) as first argument
                            self.emit(&mac.callee);
                            // Push remaining arguments
                            for arg in &call.arguments {
                                self.emit(&arg.expr);
                            }
                            self.writer.write(Bytecode::new(Opcode::Invoke, method_slot));
                            return;
                        }
                    }

                    // Fallback: generic member function call
                    self.emit(&mac.callee);
                    for arg in &call.arguments {
                        self.emit(&arg.expr);
                    }
                    let callee_name = self.build_call_name(call);
                    if let Some(sym) = self.ctx.scopes.lookup(&callee_name) {
                        self.writer
                            .write(Bytecode::new(Opcode::Invoke, sym.type_slot));
                    }
                    return;
                }
            }
        }

        // Resolve function slot
        let callee_name = self.build_call_name(call);
        let func_slot = self.ctx.scopes.lookup(&callee_name).map(|s| s.type_slot);

        // If callee is a dict/array type constructor like [Int:Int](), create empty collection
        if let Node::DictLiteralExpr(_) = call.callee.as_ref() {
            // Create empty dict
            self.writer.write(Bytecode::new(Opcode::IConst, 0));
            self.writer.write(Bytecode::new(Opcode::Debug, 3));
            return;
        }
        if let Node::ArrayLiteralExpr(_) = call.callee.as_ref() {
            // Create empty array
            self.writer.write(Bytecode::new(Opcode::IConst, 0));
            self.writer.write(Bytecode::new(Opcode::ONewArray, 0));
            return;
        }

        // If not found as a function, check if it's a class constructor
        // e.g. A() where A is a class name
        if func_slot.is_none() {
            let class_name = call.callee.simple_name();
            if let Some(sym) = self.ctx.scopes.lookup(&class_name) {
                if let JoyeerType::Class(_) = self.ctx.get_type(sym.type_slot) {
                    let class_slot = sym.type_slot;
                    // Allocate the object
                    self.writer.write(Bytecode::new(Opcode::New, class_slot));
                    // Run default initializer (set field defaults)
                    self.writer.write(Bytecode::new(Opcode::Debug, 4 + class_slot * 256));

                    // Check for a custom init constructor
                    // Build callee name: "ClassName.init(param1:param2:)"
                    if !call.arguments.is_empty() {
                        let mut init_name = format!("{}.init(", class_name);
                        for arg in &call.arguments {
                            init_name.push_str(&arg.label);
                            init_name.push(':');
                        }
                        init_name.push(')');

                        if let Some(init_sym) = self.ctx.scopes.lookup(&init_name) {
                            let init_slot = init_sym.type_slot;
                            // Push object ref (dup from top of stack — default init returns it)
                            // Actually, after Debug(4+...) the object ref is on the stack
                            // We need to pass it + args to the custom init
                            // The init function expects self as first param
                            // Push args
                            for arg in &call.arguments {
                                self.emit(&arg.expr);
                            }
                            self.writer.write(Bytecode::new(Opcode::Invoke, init_slot));
                        }
                    }
                    return;
                }
            }
        }

        // Check if this is the built-in print function
        let is_print = func_slot == Some(BuiltIn::FuncPrint as Slot);

        // Emit arguments
        for arg in &call.arguments {
            self.emit(&arg.expr);

            // If calling print, check if the argument is a bool expression
            if is_print {
                let is_bool_arg = self.is_bool_expr(&arg.expr);
                if is_bool_arg {
                    self.writer.write(Bytecode::new(Opcode::Debug, 1));
                }
            }
        }

        if let Some(slot) = func_slot {
            self.writer.write(Bytecode::new(Opcode::Invoke, slot));
        }
    }

    fn is_bool_expr(&self, node: &Node) -> bool {
        match node {
            Node::LiteralExpr(lit) => lit.token.kind == TokenKind::BooleanLiteral,
            Node::IdentifierExpr(id) => {
                if let Some(sym) = self.ctx.scopes.lookup(&id.name) {
                    sym.type_slot == ValueType::Bool as Slot
                } else {
                    false
                }
            }
            Node::Expr(expr) => {
                // If any binary op is a comparison or logical, it's a bool
                for bin in &expr.binaries {
                    if let Node::BinaryExpr(b) = bin.as_ref() {
                        if matches!(b.op.raw_value.as_str(),
                            ">" | ">=" | "<" | "<=" | "==" | "!=" | "&&") {
                            return true;
                        }
                    }
                }
                false
            }
            _ => false,
        }
    }

    fn emit_member_access(&mut self, expr: &MemberAccessExpr) {
        self.emit(&expr.callee);
        // Resolve field index for class member access
        let member_name = expr.member.simple_name();
        let field_index = self.resolve_field_index_from_expr(&expr.callee, &member_name);
        self.writer.write(Bytecode::new(Opcode::IConst, field_index));
        self.writer.write(Bytecode::new(Opcode::GetField, 0));
    }

    fn resolve_field_index_from_expr(&self, callee: &Node, field_name: &str) -> i64 {
        // Try to find the class type from the callee identifier
        let class_name = match callee {
            Node::IdentifierExpr(id) => {
                if let Some(sym) = self.ctx.scopes.lookup(&id.name) {
                    // sym.type_slot points to the class type
                    if let JoyeerType::Class(c) = self.ctx.get_type(sym.type_slot) {
                        for (i, f) in c.instance_fields.iter().enumerate() {
                            if f.name == field_name {
                                return i as i64;
                            }
                        }
                    }
                }
                return 0;
            }
            _ => return 0,
        };
    }

    fn emit_member_func_call(&mut self, expr: &MemberFuncCallExpr) {
        self.emit(&expr.callee);
        self.emit(&expr.member);
    }

    fn emit_prefix(&mut self, expr: &PrefixExpr) {
        self.emit(&expr.expr);
        if expr.op.raw_value == operators::MINUS {
            self.writer.write(Bytecode::new(Opcode::INeg, 0));
        }
    }

    fn emit_if(&mut self, stmt: &IfStmt) {
        // Emit condition
        self.emit(&stmt.condition);

        // IFNE (jump if zero) - we'll patch the offset later
        let jump_pos = self.writer.current_pos();
        self.writer.write(Bytecode::new(Opcode::IfNe, 0)); // placeholder

        // Emit then block
        self.emit(&stmt.then_block);

        if let Some(ref else_block) = stmt.else_block {
            // GOTO past else block
            let goto_pos = self.writer.current_pos();
            self.writer.write(Bytecode::new(Opcode::Goto, 0)); // placeholder

            // Patch IFNE to jump here
            let else_offset = (self.writer.current_pos() - jump_pos - 1) as i64;
            self.writer.patch(jump_pos, Bytecode::new(Opcode::IfNe, else_offset));

            // Emit else block
            self.emit(else_block);

            // Patch GOTO to jump past else
            let end_offset = (self.writer.current_pos() - goto_pos - 1) as i64;
            self.writer.patch(goto_pos, Bytecode::new(Opcode::Goto, end_offset));
        } else {
            // Patch IFNE
            let offset = (self.writer.current_pos() - jump_pos - 1) as i64;
            self.writer.patch(jump_pos, Bytecode::new(Opcode::IfNe, offset));
        }
    }

    fn emit_while(&mut self, stmt: &WhileStmt) {
        let loop_start = self.writer.current_pos();

        // Emit condition
        self.emit(&stmt.condition);

        // IFNE (jump out if zero)
        let jump_pos = self.writer.current_pos();
        self.writer.write(Bytecode::new(Opcode::IfNe, 0));

        // Emit body
        self.emit(&stmt.body);

        // GOTO back to loop start
        let back_offset = -((self.writer.current_pos() - loop_start + 1) as i64);
        self.writer.write(Bytecode::new(Opcode::Goto, back_offset));

        // Patch IFNE
        let exit_offset = (self.writer.current_pos() - jump_pos - 1) as i64;
        self.writer.patch(jump_pos, Bytecode::new(Opcode::IfNe, exit_offset));
    }

    fn emit_return(&mut self, stmt: &ReturnStmt) {
        if let Some(ref expr) = stmt.expr {
            self.emit(expr);
            self.writer.write(Bytecode::new(Opcode::IReturn, 0));
        } else {
            self.writer.write(Bytecode::new(Opcode::Return, 0));
        }
    }

    fn emit_array_literal(&mut self, expr: &ArrayLiteralExpr) {
        // Push all elements
        for elem in &expr.elements {
            self.emit(elem);
        }
        // Push count then ONewArray
        self.writer
            .write(Bytecode::new(Opcode::IConst, expr.elements.len() as i64));
        self.writer.write(Bytecode::new(Opcode::ONewArray, 0));
    }

    fn emit_subscript(&mut self, expr: &SubscriptExpr) {
        // Push callee (array or dict ref), then index/key, then GetField
        self.emit(&expr.callee);
        self.emit(&expr.index);
        self.writer.write(Bytecode::new(Opcode::GetField, 0));
    }

    fn emit_dict_literal(&mut self, expr: &DictLiteralExpr) {
        // Push key-value pairs: key1, val1, key2, val2, ...
        for (k, v) in &expr.entries {
            self.emit(k);
            self.emit(v);
        }
        // Push count of entries
        self.writer
            .write(Bytecode::new(Opcode::IConst, expr.entries.len() as i64));
        // Debug(3) = create dict from stack
        self.writer.write(Bytecode::new(Opcode::Debug, 3));
    }

    fn build_func_name(&self, name: &str, params: &[Pattern]) -> String {
        let mut s = String::from(name);
        s.push('(');
        for p in params {
            s.push_str(&p.name);
            s.push(':');
        }
        s.push(')');
        s
    }

    fn build_call_name(&self, call: &FuncCallExpr) -> String {
        let mut s = call.callee.simple_name();
        s.push('(');
        for arg in &call.arguments {
            s.push_str(&arg.label);
            s.push(':');
        }
        s.push(')');
        s
    }

    fn resolve_type_name(&self, name: &str) -> Slot {
        match name {
            "Int" => ValueType::Int as Slot,
            "Bool" => ValueType::Bool as Slot,
            "String" => ValueType::String as Slot,
            "Void" => ValueType::Void as Slot,
            _ => {
                if let Some(sym) = self.ctx.scopes.lookup(name) {
                    sym.type_slot
                } else {
                    -1
                }
            }
        }
    }

    fn resolve_class_name_of(&self, node: &Node) -> Option<String> {
        if let Node::IdentifierExpr(id) = node {
            if let Some(sym) = self.ctx.scopes.lookup(&id.name) {
                if let JoyeerType::Class(c) = self.ctx.get_type(sym.type_slot) {
                    return Some(c.name.clone());
                }
            }
        }
        None
    }

    /// Find a Field symbol by name, skipping Var symbols with the same name
    fn find_field_location(&self, name: &str) -> Option<i32> {
        use crate::compiler::symtable::SymbolKind;
        for scope in self.ctx.scopes.scopes.iter().rev() {
            for sym in scope.symbols.iter().rev() {
                if sym.name == name && sym.kind == SymbolKind::Field {
                    return Some(sym.location);
                }
            }
        }
        None
    }

    /// Infer the type of a variable from its annotation or initializer
    fn infer_var_type(&self, decl: &VarDecl) -> Slot {
        // From explicit type annotation
        if let Some(ref ann) = decl.pattern.type_annotation {
            let slot = self.resolve_type_name(ann);
            if slot >= 0 {
                return slot;
            }
        }
        // From initializer expression
        if let Some(ref init) = decl.initializer {
            return self.infer_expr_type(init);
        }
        -1
    }

    fn infer_expr_type(&self, node: &Node) -> Slot {
        match node {
            Node::LiteralExpr(lit) => match lit.token.kind {
                TokenKind::DecimalLiteral => ValueType::Int as Slot,
                TokenKind::BooleanLiteral => ValueType::Bool as Slot,
                TokenKind::StringLiteral => ValueType::String as Slot,
                TokenKind::NilLiteral => ValueType::Nil as Slot,
                _ => -1,
            },
            Node::FuncCallExpr(call) => {
                // If calling a class constructor, type is the class
                let name = call.callee.simple_name();
                if let Some(sym) = self.ctx.scopes.lookup(&name) {
                    if let JoyeerType::Class(_) = self.ctx.get_type(sym.type_slot) {
                        return sym.type_slot;
                    }
                }
                // Otherwise, look up the function's return type
                let callee_name = self.build_call_name(call);
                if let Some(sym) = self.ctx.scopes.lookup(&callee_name) {
                    if let JoyeerType::Function(f) = self.ctx.get_type(sym.type_slot) {
                        return f.return_type_slot;
                    }
                }
                -1
            }
            Node::ArrayLiteralExpr(_) => -1, // array type
            Node::DictLiteralExpr(_) => -1,  // dict type
            Node::Expr(expr) => self.infer_expr_type(&expr.prefix),
            Node::IdentifierExpr(id) => {
                if let Some(sym) = self.ctx.scopes.lookup(&id.name) {
                    sym.type_slot
                } else {
                    -1
                }
            }
            _ => -1,
        }
    }
}
