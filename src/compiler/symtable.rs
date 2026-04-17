use crate::runtime::types::Slot;

/// Symbol flags
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SymbolKind {
    Var,
    Func,
    Constructor,
    Field,
    Class,
}

/// A declared symbol in a scope
#[derive(Debug, Clone)]
pub struct Symbol {
    pub kind: SymbolKind,
    pub name: String,
    pub type_slot: Slot,
    pub parent_type_slot: Slot,
    pub location: i32, // index in parent's field/param list
    pub is_static: bool,
}

impl Symbol {
    pub fn new(kind: SymbolKind, name: String) -> Self {
        Self {
            kind,
            name,
            type_slot: -1,
            parent_type_slot: -1,
            location: -1,
            is_static: false,
        }
    }
}

/// A scope containing symbols
#[derive(Debug)]
pub struct SymbolTable {
    pub symbols: Vec<Symbol>,
}

impl SymbolTable {
    pub fn new() -> Self {
        Self {
            symbols: Vec::new(),
        }
    }

    pub fn define(&mut self, sym: Symbol) {
        self.symbols.push(sym);
    }

    pub fn lookup(&self, name: &str) -> Option<&Symbol> {
        self.symbols.iter().rev().find(|s| s.name == name)
    }
}

/// Stack of scopes for hierarchical name resolution
pub struct ScopeStack {
    pub scopes: Vec<SymbolTable>,
}

impl ScopeStack {
    pub fn new() -> Self {
        Self {
            scopes: Vec::new(),
        }
    }

    pub fn push(&mut self) {
        self.scopes.push(SymbolTable::new());
    }

    pub fn pop(&mut self) -> Option<SymbolTable> {
        self.scopes.pop()
    }

    pub fn current_mut(&mut self) -> &mut SymbolTable {
        self.scopes.last_mut().expect("no scope")
    }

    pub fn define(&mut self, sym: Symbol) {
        self.current_mut().define(sym);
    }

    pub fn lookup(&self, name: &str) -> Option<&Symbol> {
        for scope in self.scopes.iter().rev() {
            if let Some(s) = scope.lookup(name) {
                return Some(s);
            }
        }
        None
    }
}
