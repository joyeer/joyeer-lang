/// Value types in joyeer
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum ValueType {
    Nil = 0,
    Unspecified,
    Void,
    Int,
    Bool,
    Any,
    String,
    Optional,
    Module,
    Class,
    Func,
}

/// Slot index into the type table
pub type Slot = i64;
/// Runtime value (64-bit)
pub type Value = i64;

/// Built-in type/function slots (assigned in order after primary types)
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u16)]
pub enum BuiltIn {
    // Primary types: 0=Nil, 1=Unspecified, 2=Void, 3=Int, 4=Bool, 5=Any, 6=String
    FuncPrint = 7,
    FuncAutoWrappingInt,
    FuncAutoWrappingBool,
    FuncAutoWrappingClass,
    FuncAutoUnwrapping,

    ObjectOptionalInt,
    ObjectOptionalBool,

    ObjectArray,
    ObjectArrayFuncSize,
    ObjectArrayFuncGet,
    ObjectArrayFuncSet,

    ObjectDict,
    ObjectDictInit,
    ObjectDictFuncInsert,
    ObjectDictFuncGet,

    ObjectDictEntry,

    ObjectStringBuilder,
    ObjectStringBuilderFuncAppend,
    ObjectStringBuilderFuncToString,
}

/// Function implementation kind
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FuncKind {
    NativeFunc,     // Implemented in Rust
    NativeClassInit, // Class constructor in Rust
    VMFunc,         // Interpreted bytecodes
    VMClassInit,    // Class constructor in bytecodes
}

/// A variable/field declaration
#[derive(Debug, Clone)]
pub struct Variable {
    pub name: String,
    pub type_slot: Slot,
    pub loc: i32,
    pub is_static: bool,
}

impl Variable {
    pub fn new(name: String) -> Self {
        Self {
            name,
            type_slot: -1,
            loc: -1,
            is_static: false,
        }
    }
}

/// A type registered in the type table
#[derive(Debug)]
pub enum JoyeerType {
    Int,
    Bool,
    Nil,
    Void,
    Any,
    Unspecified,
    String(StringType),
    Optional(OptionalType),
    Function(FunctionType),
    Class(ClassType),
    Module(ModuleType),
}

impl JoyeerType {
    pub fn name(&self) -> &str {
        match self {
            JoyeerType::Int => "Int",
            JoyeerType::Bool => "Bool",
            JoyeerType::Nil => "Nil",
            JoyeerType::Void => "Void",
            JoyeerType::Any => "Any",
            JoyeerType::Unspecified => "Unspecified",
            JoyeerType::String(_) => "String",
            JoyeerType::Optional(o) => &o.name,
            JoyeerType::Function(f) => &f.name,
            JoyeerType::Class(c) => &c.name,
            JoyeerType::Module(m) => &m.name,
        }
    }

    pub fn value_type(&self) -> ValueType {
        match self {
            JoyeerType::Int => ValueType::Int,
            JoyeerType::Bool => ValueType::Bool,
            JoyeerType::Nil => ValueType::Nil,
            JoyeerType::Void => ValueType::Void,
            JoyeerType::Any => ValueType::Any,
            JoyeerType::Unspecified => ValueType::Unspecified,
            JoyeerType::String(_) => ValueType::String,
            JoyeerType::Optional(_) => ValueType::Optional,
            JoyeerType::Function(_) => ValueType::Func,
            JoyeerType::Class(_) => ValueType::Class,
            JoyeerType::Module(_) => ValueType::Module,
        }
    }
}

#[derive(Debug)]
pub struct StringType;

#[derive(Debug)]
pub struct OptionalType {
    pub name: String,
    pub wrapped_type_slot: Slot,
}

#[derive(Debug)]
pub struct FunctionType {
    pub name: String,
    pub func_kind: FuncKind,
    pub param_count: usize,
    pub local_vars: Vec<Variable>,
    pub return_type_slot: Slot,
    pub is_static: bool,
    pub bytecodes: Option<Vec<u64>>,
}

impl FunctionType {
    pub fn new(name: String, is_static: bool) -> Self {
        Self {
            name,
            func_kind: FuncKind::VMFunc,
            param_count: 0,
            local_vars: Vec::new(),
            return_type_slot: ValueType::Void as Slot,
            is_static,
            bytecodes: None,
        }
    }

    pub fn local_var_count(&self) -> usize {
        self.local_vars.len()
    }
}

#[derive(Debug)]
pub struct ClassType {
    pub name: String,
    pub static_fields: Vec<Variable>,
    pub instance_fields: Vec<Variable>,
    pub static_initializer_slot: Slot,
    pub default_initializer_slot: Slot,
    /// Stores static field values at runtime
    pub static_area: Vec<Value>,
}

impl ClassType {
    pub fn new(name: String) -> Self {
        Self {
            name,
            static_fields: Vec::new(),
            instance_fields: Vec::new(),
            static_initializer_slot: -1,
            default_initializer_slot: -1,
            static_area: Vec::new(),
        }
    }

    pub fn instance_size(&self) -> usize {
        1 + self.instance_fields.len() // 1 for object header
    }
}

#[derive(Debug)]
pub struct ModuleType {
    pub name: String,
    pub static_fields: Vec<Variable>,
    pub static_initializer_slot: Slot,
    pub static_area: Vec<Value>,
}

impl ModuleType {
    pub fn new(name: String) -> Self {
        Self {
            name,
            static_fields: Vec::new(),
            static_initializer_slot: -1,
            static_area: Vec::new(),
        }
    }
}

/// The global type table
pub struct TypeTable {
    pub types: Vec<JoyeerType>,
}

impl TypeTable {
    pub fn new() -> Self {
        let mut table = Self { types: Vec::new() };
        // Register primary types in order matching ValueType enum
        table.types.push(JoyeerType::Nil);          // 0
        table.types.push(JoyeerType::Unspecified);   // 1
        table.types.push(JoyeerType::Void);          // 2
        table.types.push(JoyeerType::Int);           // 3
        table.types.push(JoyeerType::Bool);          // 4
        table.types.push(JoyeerType::Any);           // 5
        table.types.push(JoyeerType::String(StringType)); // 6
        table
    }

    pub fn register(&mut self, ty: JoyeerType) -> Slot {
        let slot = self.types.len() as Slot;
        self.types.push(ty);
        slot
    }

    pub fn get(&self, slot: Slot) -> &JoyeerType {
        &self.types[slot as usize]
    }

    pub fn get_mut(&mut self, slot: Slot) -> &mut JoyeerType {
        &mut self.types[slot as usize]
    }
}

/// String resource table (string pooling)
pub struct StringTable {
    pub strings: Vec<String>,
}

impl StringTable {
    pub fn new() -> Self {
        Self {
            strings: Vec::new(),
        }
    }

    pub fn import(&mut self, s: &str) -> Slot {
        // Check for existing
        for (i, existing) in self.strings.iter().enumerate() {
            if existing == s {
                return i as Slot;
            }
        }
        let slot = self.strings.len() as Slot;
        self.strings.push(s.to_string());
        slot
    }

    pub fn get(&self, slot: Slot) -> &str {
        &self.strings[slot as usize]
    }
}
