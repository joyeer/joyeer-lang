/// Bytecode opcodes matching the C++ implementation
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum Opcode {
    Nop = 0x00,

    OConstNil,
    IConst,
    SConst,

    IStore,

    OLoad,
    ILoad,

    New,
    PutField,
    GetField,
    PutStatic,
    GetStatic,

    IAnd,
    ICmpG,
    ICmpGE,
    ICmpL,
    ICmpLE,
    ICmpNE,
    ICmpEQ,
    IAdd,
    ISub,
    IMul,
    IDiv,
    IRem,
    INeg,

    IfEq,  // == 0
    IfNe,  // != 0
    IfLt,  // < 0
    IfLe,  // <= 0
    IfGt,  // > 0
    IfGe,  // >= 0

    Return,
    IReturn,
    OReturn,

    ONewArray,

    Invoke,
    Dup,
    Pop,
    Goto,

    Debug,
}

/// An 8-byte bytecode instruction.
///
/// Format 1: | opcode(8) | value(56) |
/// Format 2: | opcode(8) | value1(24) | value2(32) |
///
/// We store them as u64 for simplicity.
#[derive(Debug, Clone, Copy)]
pub struct Bytecode(pub u64);

impl Bytecode {
    /// Create a format-1 bytecode: opcode + 56-bit signed value
    pub fn new(op: Opcode, value: i64) -> Self {
        let bytes = ((value << 8) as u64) | (op as u64);
        Self(bytes)
    }

    /// Create a format-2 bytecode: opcode + 24-bit value1 + 32-bit value2
    pub fn new2(op: Opcode, value1: i32, value2: i32) -> Self {
        let mut val: u64 = 0;
        val |= op as u64;
        val |= ((value1 as u64) & 0x00FFFFFF) << 8;
        val |= (value2 as u64) << 32;
        Self(val)
    }

    pub fn opcode(self) -> Opcode {
        let op = (self.0 & 0xFF) as u8;
        // SAFETY: all opcodes are in range
        unsafe { std::mem::transmute(op) }
    }

    pub fn value(self) -> i64 {
        (self.0 as i64) >> 8
    }

    pub fn value1(self) -> i32 {
        ((self.0 & 0x00000000FFFFFFFF) >> 8) as i32
    }

    pub fn value2(self) -> i32 {
        (self.0 >> 32) as i32
    }
}

/// A sequence of bytecodes for a function
pub struct BytecodeWriter {
    instructions: Vec<Bytecode>,
}

impl BytecodeWriter {
    pub fn new() -> Self {
        Self {
            instructions: Vec::new(),
        }
    }

    pub fn write(&mut self, bc: Bytecode) {
        self.instructions.push(bc);
    }

    pub fn size(&self) -> usize {
        self.instructions.len()
    }

    /// Returns the current instruction index (for patching jumps)
    pub fn current_pos(&self) -> usize {
        self.instructions.len()
    }

    /// Patch a previously written bytecode (for jump targets)
    pub fn patch(&mut self, pos: usize, bc: Bytecode) {
        self.instructions[pos] = bc;
    }

    pub fn into_bytecodes(self) -> Vec<u64> {
        self.instructions.into_iter().map(|b| b.0).collect()
    }
}
