use crate::compiler::node::*;
use crate::compiler::token::*;

pub struct Parser {
    tokens: Vec<Token>,
    pos: usize,
}

impl Parser {
    pub fn new(tokens: Vec<Token>) -> Self {
        Self { tokens, pos: 0 }
    }

    pub fn parse(&mut self) -> Option<Box<Node>> {
        let mut stmts = Vec::new();
        while self.pos < self.tokens.len() {
            if let Some(stmt) = self.parse_stmt() {
                stmts.push(stmt);
            } else {
                return None;
            }
        }
        Some(Box::new(Node::Module(ModuleDecl {
            filename: String::new(),
            statements: stmts,
            type_slot: -1,
        })))
    }

    // ── Helpers ─────────────────────────────────────

    fn peek(&self) -> Option<&Token> {
        self.tokens.get(self.pos)
    }

    fn at_end(&self) -> bool {
        self.pos >= self.tokens.len()
    }

    fn try_eat(&mut self, kind: TokenKind, value: &str) -> Option<Token> {
        if let Some(tok) = self.peek() {
            if tok.kind == kind && tok.raw_value == value {
                let tok = tok.clone();
                self.pos += 1;
                return Some(tok);
            }
        }
        None
    }

    fn try_eat_kind(&mut self, kind: TokenKind) -> Option<Token> {
        if let Some(tok) = self.peek() {
            if tok.kind == kind {
                let tok = tok.clone();
                self.pos += 1;
                return Some(tok);
            }
        }
        None
    }

    fn previous(&mut self) {
        if self.pos > 0 {
            self.pos -= 1;
        }
    }

    // ── Statements ──────────────────────────────────

    fn parse_stmt(&mut self) -> Option<Box<Node>> {
        if let Some(n) = self.parse_expr() {
            return Some(n);
        }
        if let Some(n) = self.parse_decl() {
            return Some(n);
        }
        if let Some(n) = self.parse_for_in_stmt() {
            return Some(n);
        }
        if let Some(n) = self.parse_if_stmt() {
            return Some(n);
        }
        if let Some(n) = self.parse_return_stmt() {
            return Some(n);
        }
        if let Some(n) = self.parse_while_stmt() {
            return Some(n);
        }
        None
    }

    fn parse_stmts_block(&mut self) -> Option<Box<Node>> {
        self.try_eat(TokenKind::Punctuation, punctuations::OPEN_CURLY)?;
        let mut stmts = Vec::new();
        while let Some(stmt) = self.parse_stmt() {
            stmts.push(stmt);
        }
        self.try_eat(TokenKind::Punctuation, punctuations::CLOSE_CURLY)?;
        Some(Box::new(Node::StmtsBlock(StmtsBlock { statements: stmts })))
    }

    // ── Declarations ────────────────────────────────

    fn parse_decl(&mut self) -> Option<Box<Node>> {
        if let Some(n) = self.parse_let_decl() {
            return Some(n);
        }
        if let Some(n) = self.parse_var_decl() {
            return Some(n);
        }
        if let Some(n) = self.parse_class_decl() {
            return Some(n);
        }
        if let Some(n) = self.parse_func_decl() {
            return Some(n);
        }
        if let Some(n) = self.parse_constructor_decl() {
            return Some(n);
        }
        if let Some(n) = self.parse_fileimport() {
            return Some(n);
        }
        None
    }

    fn parse_let_decl(&mut self) -> Option<Box<Node>> {
        self.try_eat(TokenKind::Keyword, keywords::LET)?;
        let pattern = self.parse_pattern()?;
        let init = if self.try_eat(TokenKind::Operator, operators::EQUALS).is_some() {
            self.parse_expr()
        } else {
            None
        };
        Some(Box::new(Node::VarDecl(VarDecl {
            pattern,
            initializer: init,
            is_let: true,
            type_slot: -1,
        })))
    }

    fn parse_var_decl(&mut self) -> Option<Box<Node>> {
        self.try_eat(TokenKind::Keyword, keywords::VAR)?;
        let pattern = self.parse_pattern()?;
        let init = if self.try_eat(TokenKind::Operator, operators::EQUALS).is_some() {
            self.parse_expr()
        } else {
            None
        };
        Some(Box::new(Node::VarDecl(VarDecl {
            pattern,
            initializer: init,
            is_let: false,
            type_slot: -1,
        })))
    }

    fn parse_func_decl(&mut self) -> Option<Box<Node>> {
        self.try_eat(TokenKind::Keyword, keywords::FUNC)?;
        let name_tok = self.try_eat_kind(TokenKind::Identifier)?;
        let params = self.parse_parameter_clause()?;
        let ret_type = self.parse_type_annotation();
        let body = self.parse_stmts_block()?;
        Some(Box::new(Node::FuncDecl(FuncDecl {
            name: name_tok.raw_value,
            params,
            return_type: ret_type,
            body,
            is_constructor: false,
            type_slot: -1,
        })))
    }

    fn parse_constructor_decl(&mut self) -> Option<Box<Node>> {
        self.try_eat(TokenKind::Keyword, keywords::INIT)?;
        let params = self.parse_parameter_clause()?;
        let body = self.parse_stmts_block()?;
        Some(Box::new(Node::FuncDecl(FuncDecl {
            name: "init".into(),
            params,
            return_type: None,
            body,
            is_constructor: true,
            type_slot: -1,
        })))
    }

    fn parse_class_decl(&mut self) -> Option<Box<Node>> {
        self.try_eat(TokenKind::Keyword, keywords::CLASS)?;
        let name_tok = self.try_eat_kind(TokenKind::Identifier)?;
        self.try_eat(TokenKind::Punctuation, punctuations::OPEN_CURLY)?;
        let mut members = Vec::new();
        while let Some(m) = self.parse_decl() {
            members.push(m);
        }
        self.try_eat(TokenKind::Punctuation, punctuations::CLOSE_CURLY)?;
        Some(Box::new(Node::ClassDecl(ClassDecl {
            name: name_tok.raw_value,
            members,
            type_slot: -1,
        })))
    }

    fn parse_fileimport(&mut self) -> Option<Box<Node>> {
        self.try_eat(TokenKind::Keyword, keywords::FILEIMPORT)?;
        let tok = self.try_eat_kind(TokenKind::StringLiteral)?;
        Some(Box::new(Node::ImportStmt(ImportStmt { path: tok })))
    }

    // ── Parameters & Patterns ───────────────────────

    fn parse_parameter_clause(&mut self) -> Option<Vec<Pattern>> {
        self.try_eat(TokenKind::Punctuation, punctuations::OPEN_PAREN)?;
        let mut params = Vec::new();
        let mut i = 0;
        loop {
            if i > 0 {
                if self.try_eat(TokenKind::Punctuation, punctuations::COMMA).is_none() {
                    break;
                }
            }
            if let Some(p) = self.parse_pattern() {
                params.push(p);
                i += 1;
            } else {
                break;
            }
        }
        self.try_eat(TokenKind::Punctuation, punctuations::CLOSE_PAREN)?;
        Some(params)
    }

    fn parse_pattern(&mut self) -> Option<Pattern> {
        let id = self.try_eat_kind(TokenKind::Identifier)?;
        let type_ann = self.parse_type_annotation_name();
        Some(Pattern {
            name: id.raw_value,
            type_annotation: type_ann,
        })
    }

    fn parse_type_annotation(&mut self) -> Option<Box<Node>> {
        self.try_eat(TokenKind::Punctuation, punctuations::COLON)?;
        self.parse_type()
    }

    fn parse_type_annotation_name(&mut self) -> Option<String> {
        self.try_eat(TokenKind::Punctuation, punctuations::COLON)?;
        let type_node = self.parse_type()?;
        Some(type_node.simple_name())
    }

    fn parse_type(&mut self) -> Option<Box<Node>> {
        // Try array type [T]
        if let Some(n) = self.parse_array_type() {
            return Some(self.parse_optional_suffix(n));
        }
        // Try identifier type
        if let Some(id) = self.try_eat_kind(TokenKind::Identifier) {
            let node = Box::new(Node::TypeIdentifier(TypeIdentifier {
                name: id.raw_value,
            }));
            return Some(self.parse_optional_suffix(node));
        }
        None
    }

    fn parse_array_type(&mut self) -> Option<Box<Node>> {
        self.try_eat(TokenKind::Punctuation, punctuations::OPEN_BRACKET)?;
        let elem = self.parse_type()?;
        self.try_eat(TokenKind::Punctuation, punctuations::CLOSE_BRACKET)?;
        Some(Box::new(Node::ArrayType(ArrayTypeNode {
            element_type: elem,
        })))
    }

    fn parse_optional_suffix(&mut self, inner: Box<Node>) -> Box<Node> {
        if self.try_eat(TokenKind::Operator, operators::QUESTION).is_some() {
            return Box::new(Node::OptionalType(OptionalTypeNode {
                inner_type: inner,
                required: false,
            }));
        }
        if self.try_eat(TokenKind::Operator, operators::BANG).is_some() {
            return Box::new(Node::OptionalType(OptionalTypeNode {
                inner_type: inner,
                required: true,
            }));
        }
        inner
    }

    // ── Control Flow ────────────────────────────────

    fn parse_if_stmt(&mut self) -> Option<Box<Node>> {
        self.try_eat(TokenKind::Keyword, keywords::IF)?;
        let cond = self.parse_expr()?;
        let then_block = self.parse_stmts_block()?;
        let else_block = if self.try_eat(TokenKind::Keyword, keywords::ELSE).is_some() {
            // else if ...
            if let Some(elif) = self.parse_if_stmt() {
                Some(elif)
            } else {
                self.parse_stmts_block()
            }
        } else {
            None
        };
        Some(Box::new(Node::IfStmt(IfStmt {
            condition: cond,
            then_block,
            else_block,
        })))
    }

    fn parse_while_stmt(&mut self) -> Option<Box<Node>> {
        self.try_eat(TokenKind::Keyword, keywords::WHILE)?;
        let cond = self.parse_expr()?;
        let body = self.parse_stmts_block()?;
        Some(Box::new(Node::WhileStmt(WhileStmt {
            condition: cond,
            body,
        })))
    }

    fn parse_for_in_stmt(&mut self) -> Option<Box<Node>> {
        self.try_eat(TokenKind::Keyword, keywords::FOR)?;
        let pat = self.parse_pattern()?;
        self.try_eat(TokenKind::Keyword, keywords::IN)?;
        let iter = self.parse_expr()?;
        let body = self.parse_stmts_block()?;
        Some(Box::new(Node::ForInStmt(ForInStmt {
            pattern: pat,
            iterable: iter,
            body,
        })))
    }

    fn parse_return_stmt(&mut self) -> Option<Box<Node>> {
        self.try_eat(TokenKind::Keyword, keywords::RETURN)?;
        let expr = self.parse_expr();
        Some(Box::new(Node::ReturnStmt(ReturnStmt { expr })))
    }

    // ── Expressions ─────────────────────────────────

    fn parse_expr(&mut self) -> Option<Box<Node>> {
        let prefix = self.parse_prefix_expr()?;
        let mut binaries = Vec::new();
        while let Some(bin) = self.parse_binary_expr() {
            binaries.push(bin);
        }
        if binaries.is_empty() {
            Some(prefix)
        } else {
            Some(Box::new(Node::Expr(Expr {
                prefix,
                binaries,
                nodes: Vec::new(),
                type_slot: -1,
            })))
        }
    }

    fn parse_binary_expr(&mut self) -> Option<Box<Node>> {
        // Assignment
        if self.try_eat(TokenKind::Operator, operators::EQUALS).is_some() {
            let rhs = self.parse_expr()?;
            return Some(Box::new(Node::AssignExpr(AssignExpr {
                expr: rhs,
                left: None,
                type_slot: -1,
            })));
        }
        // Binary operator
        if let Some(op) = self.parse_operator() {
            let rhs = self.parse_prefix_expr()?;
            return Some(Box::new(Node::BinaryExpr(BinaryExpr {
                op,
                expr: rhs,
                type_slot: -1,
            })));
        }
        None
    }

    fn parse_prefix_expr(&mut self) -> Option<Box<Node>> {
        let op = self.parse_operator();
        let postfix = self.parse_postfix_expr(None);
        if postfix.is_none() {
            if op.is_some() {
                self.previous();
            }
            return None;
        }
        let postfix = postfix.unwrap();
        if let Some(op) = op {
            Some(Box::new(Node::PrefixExpr(PrefixExpr {
                op,
                expr: postfix,
                type_slot: -1,
            })))
        } else {
            Some(postfix)
        }
    }

    fn parse_postfix_expr(&mut self, existing: Option<Box<Node>>) -> Option<Box<Node>> {
        let mut result = if let Some(e) = existing {
            e
        } else {
            let primary = self.parse_primary_expr()?;
            if let Some(op) = self.parse_postfix_operator() {
                Box::new(Node::PostfixExpr(PostfixExpr {
                    expr: primary,
                    op,
                    type_slot: -1,
                }))
            } else {
                primary
            }
        };

        loop {
            // Try function call
            let saved = self.pos;
            if self.try_eat(TokenKind::Punctuation, punctuations::OPEN_PAREN).is_some() {
                let mut arguments = Vec::new();
                if let Some(arg) = self.parse_argu_call() {
                    arguments.push(arg);
                    while self.try_eat(TokenKind::Punctuation, punctuations::COMMA).is_some() {
                        if let Some(arg) = self.parse_argu_call() {
                            arguments.push(arg);
                        }
                    }
                }
                if self.try_eat(TokenKind::Punctuation, punctuations::CLOSE_PAREN).is_none() {
                    self.pos = saved;
                    break;
                }
                result = Box::new(Node::FuncCallExpr(FuncCallExpr {
                    callee: result,
                    arguments,
                    func_type_slot: -1,
                    type_slot: -1,
                }));
                result = self.maybe_postfix_operator(result);
                continue;
            }

            // Try subscript [expr]
            if self.try_eat(TokenKind::Punctuation, punctuations::OPEN_BRACKET).is_some() {
                if let Some(idx) = self.parse_expr() {
                    if self.try_eat(TokenKind::Punctuation, punctuations::CLOSE_BRACKET).is_some() {
                        result = Box::new(Node::SubscriptExpr(SubscriptExpr {
                            callee: result,
                            index: idx,
                            type_slot: -1,
                        }));
                        result = self.maybe_postfix_operator(result);
                        continue;
                    }
                }
            }

            // Try member access .identifier
            if self.try_eat(TokenKind::Punctuation, punctuations::DOT).is_some() {
                if let Some(id_tok) = self.try_eat_kind(TokenKind::Identifier) {
                    let member = Box::new(Node::IdentifierExpr(IdentifierExpr {
                        name: id_tok.raw_value.clone(),
                        token: id_tok,
                        type_slot: -1,
                    }));
                    result = Box::new(Node::MemberAccessExpr(MemberAccessExpr {
                        callee: result,
                        member,
                        type_slot: -1,
                    }));
                    result = self.maybe_postfix_operator(result);
                    continue;
                }
            }

            break;
        }

        Some(result)
    }

    fn maybe_postfix_operator(&mut self, node: Box<Node>) -> Box<Node> {
        if let Some(op) = self.parse_postfix_operator() {
            Box::new(Node::PostfixExpr(PostfixExpr {
                expr: node,
                op,
                type_slot: -1,
            }))
        } else {
            node
        }
    }

    fn parse_primary_expr(&mut self) -> Option<Box<Node>> {
        // Identifier
        if let Some(tok) = self.try_eat_kind(TokenKind::Identifier) {
            return Some(Box::new(Node::IdentifierExpr(IdentifierExpr {
                name: tok.raw_value.clone(),
                token: tok,
                type_slot: -1,
            })));
        }
        // Literal
        if let Some(n) = self.parse_literal_expr() {
            return Some(n);
        }
        // Self
        if let Some(n) = self.parse_self_expr() {
            return Some(n);
        }
        // Parenthesized
        if let Some(n) = self.parse_parenthesized_expr() {
            return Some(n);
        }
        None
    }

    fn parse_literal_expr(&mut self) -> Option<Box<Node>> {
        // Numeric, bool, nil, string literals
        if let Some(tok) = self.peek() {
            match tok.kind {
                TokenKind::DecimalLiteral
                | TokenKind::FloatLiteral
                | TokenKind::StringLiteral
                | TokenKind::BooleanLiteral
                | TokenKind::NilLiteral => {
                    let tok = tok.clone();
                    self.pos += 1;
                    return Some(Box::new(Node::LiteralExpr(LiteralExpr {
                        token: tok,
                        type_slot: -1,
                    })));
                }
                _ => {}
            }
        }
        // Array or dict literal [...]
        if let Some(n) = self.parse_array_or_dict_literal() {
            return Some(n);
        }
        None
    }

    fn parse_array_or_dict_literal(&mut self) -> Option<Box<Node>> {
        self.try_eat(TokenKind::Punctuation, punctuations::OPEN_BRACKET)?;

        // Empty dict literal [:]
        if self.try_eat(TokenKind::Punctuation, punctuations::COLON).is_some() {
            self.try_eat(TokenKind::Punctuation, punctuations::CLOSE_BRACKET)?;
            return Some(Box::new(Node::DictLiteralExpr(DictLiteralExpr {
                entries: Vec::new(),
                type_slot: -1,
            })));
        }

        // First element
        let first = self.parse_expr()?;

        // Check if it's a dict (key: value)
        if self.try_eat(TokenKind::Punctuation, punctuations::COLON).is_some() {
            let val = self.parse_expr()?;
            let mut entries = vec![(first, val)];
            while self.try_eat(TokenKind::Punctuation, punctuations::COMMA).is_some() {
                let k = self.parse_expr()?;
                self.try_eat(TokenKind::Punctuation, punctuations::COLON)?;
                let v = self.parse_expr()?;
                entries.push((k, v));
            }
            self.try_eat(TokenKind::Punctuation, punctuations::CLOSE_BRACKET)?;
            return Some(Box::new(Node::DictLiteralExpr(DictLiteralExpr {
                entries,
                type_slot: -1,
            })));
        }

        // Array literal
        let mut elements = vec![first];
        while self.try_eat(TokenKind::Punctuation, punctuations::COMMA).is_some() {
            if let Some(e) = self.parse_expr() {
                elements.push(e);
            }
        }
        self.try_eat(TokenKind::Punctuation, punctuations::CLOSE_BRACKET)?;
        Some(Box::new(Node::ArrayLiteralExpr(ArrayLiteralExpr {
            elements,
            type_slot: -1,
        })))
    }

    fn parse_self_expr(&mut self) -> Option<Box<Node>> {
        self.try_eat(TokenKind::Keyword, keywords::SELF)?;
        if self.try_eat(TokenKind::Punctuation, punctuations::DOT).is_some() {
            let id = self.try_eat_kind(TokenKind::Identifier)?;
            let member = Box::new(Node::IdentifierExpr(IdentifierExpr {
                name: id.raw_value.clone(),
                token: id,
                type_slot: -1,
            }));
            Some(Box::new(Node::SelfExpr(SelfExpr {
                member,
                type_slot: -1,
            })))
        } else {
            Some(Box::new(Node::SelfNode(SelfNode { type_slot: -1 })))
        }
    }

    fn parse_parenthesized_expr(&mut self) -> Option<Box<Node>> {
        self.try_eat(TokenKind::Punctuation, punctuations::OPEN_PAREN)?;
        let expr = self.parse_expr()?;
        self.try_eat(TokenKind::Punctuation, punctuations::CLOSE_PAREN)?;
        Some(Box::new(Node::ParenthesizedExpr(ParenthesizedExpr {
            expr,
            type_slot: -1,
        })))
    }

    fn parse_argu_call(&mut self) -> Option<ArguCallExprData> {
        let saved = self.pos;
        let id = self.try_eat_kind(TokenKind::Identifier)?;
        if self.try_eat(TokenKind::Punctuation, punctuations::COLON).is_none() {
            self.pos = saved;
            return None;
        }
        let expr = self.parse_expr()?;
        Some(ArguCallExprData {
            label: id.raw_value,
            expr,
        })
    }

    fn parse_operator(&mut self) -> Option<Token> {
        if let Some(tok) = self.peek() {
            if tok.kind == TokenKind::Operator && tok.raw_value != operators::EQUALS {
                let tok = tok.clone();
                self.pos += 1;
                return Some(tok);
            }
        }
        None
    }

    fn parse_postfix_operator(&mut self) -> Option<Token> {
        if let Some(tok) = self.peek() {
            if tok.kind == TokenKind::Operator
                && (tok.raw_value == operators::BANG || tok.raw_value == operators::QUESTION)
            {
                let tok = tok.clone();
                self.pos += 1;
                return Some(tok);
            }
        }
        None
    }

    // Not used directly, kept for completeness
    fn parse_func_call_expr(&mut self, _callee: Box<Node>) -> Option<Box<Node>> {
        None // handled in parse_postfix_expr_v2
    }
}
