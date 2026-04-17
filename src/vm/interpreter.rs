use std::collections::HashMap;
use crate::runtime::bytecode::*;
use crate::runtime::types::*;

/// Sentinel value for nil
const NIL_VALUE: Value = i64::MIN;

/// Stack-based bytecode interpreter
pub struct VM {
    pub types: TypeTable,
    pub strings: StringTable,
    stack: Vec<Value>,
    frames: Vec<Frame>,
    output: String,
    /// Heap for arrays: each array is a Vec<Value>
    arrays: Vec<Vec<Value>>,
    /// Heap for dicts: each dict is a HashMap<Value, Value>
    dicts: Vec<HashMap<Value, Value>>,
    /// Heap for class objects: each object is a Vec<Value> (fields)
    /// Object ref is encoded as index + 2_000_000 (positive, distinct from arrays/dicts)
    objects: Vec<(Slot, Vec<Value>)>, // (class_type_slot, field_values)
    /// Heap for strings: each string is a String
    /// String ref is encoded as index + 3_000_000
    string_heap: Vec<String>,
    /// Flag: next print value should be formatted as bool
    next_print_is_bool: bool,
}

struct Frame {
    /// Index into the stack where this frame's local vars start
    local_base: usize,
    /// Return value slot
    return_value: Value,
}

impl VM {
    pub fn new(types: TypeTable, strings: StringTable) -> Self {
        Self {
            types,
            strings,
            stack: Vec::with_capacity(4096),
            frames: Vec::new(),
            output: String::new(),
            arrays: Vec::new(),
            dicts: Vec::new(),
            objects: Vec::new(),
            string_heap: Vec::new(),
            next_print_is_bool: false,
        }
    }

    /// Execute the module's static initializer
    pub fn run(&mut self, module_slot: Slot) -> String {
        let init_slot = match self.types.get(module_slot) {
            JoyeerType::Module(m) => m.static_initializer_slot,
            _ => panic!("expected module type"),
        };
        self.execute(init_slot);
        std::mem::take(&mut self.output)
    }

    fn execute(&mut self, func_slot: Slot) {
        let (func_kind, bytecodes, local_var_count, param_count, return_type_slot, _is_static) = {
            match self.types.get(func_slot) {
                JoyeerType::Function(f) => (
                    f.func_kind,
                    f.bytecodes.clone(),
                    f.local_var_count(),
                    f.param_count,
                    f.return_type_slot,
                    f.is_static,
                ),
                _ => panic!("expected function type at slot {}", func_slot),
            }
        };

        match func_kind {
            FuncKind::NativeFunc | FuncKind::NativeClassInit => {
                // For native functions, params are on the stack — native_print will pop them
                self.invoke_native(func_slot);
                // Native functions don't push return value for void functions
            }
            FuncKind::VMFunc | FuncKind::VMClassInit => {
                let bytecodes = bytecodes.expect("VM function has no bytecodes");

                // Pop params from stack
                let mut params = Vec::new();
                for _ in 0..param_count {
                    params.push(self.stack.pop().unwrap_or(0));
                }
                params.reverse();

                let local_base = self.stack.len();

                // Push frame
                self.frames.push(Frame {
                    local_base,
                    return_value: 0,
                });

                // Allocate local vars (params first, then zeros for the rest)
                for i in 0..local_var_count {
                    if i < param_count {
                        self.stack.push(params[i]);
                    } else {
                        self.stack.push(0);
                    }
                }

                // Run interpreter
                self.interpret(&bytecodes);

                // Get return value
                let return_value = self.frames.last().unwrap().return_value;

                // Pop frame
                self.frames.pop();

                // Remove locals and any leftover stack values
                self.stack.truncate(local_base);

                // Push return value if non-void
                if return_type_slot != ValueType::Void as Slot {
                    self.stack.push(return_value);
                }
            }
        }
    }

    fn interpret(&mut self, bytecodes: &[u64]) {
        let mut ip: usize = 0;

        while ip < bytecodes.len() {
            let bc = Bytecode(bytecodes[ip]);
            let opcode = bc.opcode();

            match opcode {
                Opcode::Nop => {}
                Opcode::OConstNil => {
                    // nil = special sentinel value
                    self.stack.push(NIL_VALUE);
                }
                Opcode::IConst => {
                    self.stack.push(bc.value());
                }
                Opcode::SConst => {
                    // String constant — allocate on string heap
                    let str_slot = bc.value();
                    let s = self.strings.get(str_slot).to_string();
                    let str_ref = self.string_heap.len() as Value;
                    self.string_heap.push(s);
                    self.stack.push(str_ref + 3_000_000);
                }
                Opcode::IStore => {
                    let slot = bc.value() as usize;
                    let val = self.stack.pop().unwrap();
                    let base = self.frames.last().unwrap().local_base;
                    self.stack[base + slot] = val;
                }
                Opcode::OLoad | Opcode::ILoad => {
                    let slot = bc.value() as usize;
                    let base = self.frames.last().unwrap().local_base;
                    let val = self.stack[base + slot];
                    self.stack.push(val);
                }
                Opcode::GetField => {
                    let index = self.stack.pop().unwrap();
                    let obj_ref = self.stack.pop().unwrap();
                    if obj_ref >= 2_000_000 {
                        // Class object reference
                        let obj_idx = (obj_ref - 2_000_000) as usize;
                        let val = self.objects[obj_idx].1[index as usize];
                        self.stack.push(val);
                    } else if obj_ref <= -1_000_000 {
                        // Dict reference — returns NIL_VALUE for missing keys
                        let dict_idx = (-(obj_ref + 1_000_000)) as usize;
                        let val = self.dicts[dict_idx].get(&index).copied().unwrap_or(NIL_VALUE);
                        self.stack.push(val);
                    } else if obj_ref < 0 {
                        // Array reference
                        let arr_idx = (-(obj_ref + 1)) as usize;
                        let val = self.arrays[arr_idx][index as usize];
                        self.stack.push(val);
                    } else {
                        self.stack.push(0);
                    }
                }
                Opcode::PutField => {
                    let value = self.stack.pop().unwrap();
                    let index = self.stack.pop().unwrap();
                    let obj_ref = self.stack.pop().unwrap();

                    if obj_ref >= 2_000_000 {
                        // Class object reference
                        let obj_idx = (obj_ref - 2_000_000) as usize;
                        self.objects[obj_idx].1[index as usize] = value;
                    } else if obj_ref <= -1_000_000 {
                        // Dict reference
                        let dict_idx = (-(obj_ref + 1_000_000)) as usize;
                        self.dicts[dict_idx].insert(index, value);
                    } else if obj_ref < 0 {
                        // Array reference
                        let arr_idx = (-(obj_ref + 1)) as usize;
                        self.arrays[arr_idx][index as usize] = value;
                    }
                }
                Opcode::New => {
                    let class_slot = bc.value();
                    let field_count = match self.types.get(class_slot) {
                        JoyeerType::Class(c) => c.instance_fields.len(),
                        _ => 0,
                    };
                    let obj_idx = self.objects.len();
                    self.objects.push((class_slot, vec![0; field_count]));
                    let obj_ref = obj_idx as Value + 2_000_000;

                    self.stack.push(obj_ref);
                }
                Opcode::IAdd => {
                    let b = self.stack.pop().unwrap();
                    let a = self.stack.pop().unwrap();
                    // Check if either is a string reference
                    if a >= 3_000_000 || b >= 3_000_000 {
                        let sa = self.value_to_string(a);
                        let sb = self.value_to_string(b);
                        let result = sa + &sb;
                        let str_ref = self.string_heap.len() as Value;
                        self.string_heap.push(result);
                        self.stack.push(str_ref + 3_000_000);
                    } else {
                        self.stack.push(a + b);
                    }
                }
                Opcode::ISub => {
                    let b = self.stack.pop().unwrap();
                    let a = self.stack.pop().unwrap();
                    self.stack.push(a - b);
                }
                Opcode::IMul => {
                    let b = self.stack.pop().unwrap();
                    let a = self.stack.pop().unwrap();
                    self.stack.push(a * b);
                }
                Opcode::IDiv => {
                    let b = self.stack.pop().unwrap();
                    let a = self.stack.pop().unwrap();
                    self.stack.push(a / b);
                }
                Opcode::IRem => {
                    let b = self.stack.pop().unwrap();
                    let a = self.stack.pop().unwrap();
                    self.stack.push(a % b);
                }
                Opcode::INeg => {
                    let a = self.stack.pop().unwrap();
                    self.stack.push(-a);
                }
                Opcode::IAnd => {
                    let b = self.stack.pop().unwrap();
                    let a = self.stack.pop().unwrap();
                    self.stack.push(if a > 0 && b > 0 { 1 } else { 0 });
                }
                Opcode::ICmpG => {
                    let b = self.stack.pop().unwrap();
                    let a = self.stack.pop().unwrap();
                    self.stack.push(if a > b { 1 } else { 0 });
                }
                Opcode::ICmpGE => {
                    let b = self.stack.pop().unwrap();
                    let a = self.stack.pop().unwrap();
                    self.stack.push(if a >= b { 1 } else { 0 });
                }
                Opcode::ICmpL => {
                    let b = self.stack.pop().unwrap();
                    let a = self.stack.pop().unwrap();
                    self.stack.push(if a < b { 1 } else { 0 });
                }
                Opcode::ICmpLE => {
                    let b = self.stack.pop().unwrap();
                    let a = self.stack.pop().unwrap();
                    self.stack.push(if a <= b { 1 } else { 0 });
                }
                Opcode::ICmpNE => {
                    let b = self.stack.pop().unwrap();
                    let a = self.stack.pop().unwrap();
                    self.stack.push(if a != b { 1 } else { 0 });
                }
                Opcode::ICmpEQ => {
                    let b = self.stack.pop().unwrap();
                    let a = self.stack.pop().unwrap();
                    self.stack.push(if a == b { 1 } else { 0 });
                }
                Opcode::IfNe => {
                    let val = self.stack.pop().unwrap();
                    if val == 0 {
                        let offset = bc.value();
                        ip = (ip as i64 + offset) as usize;
                    }
                }
                Opcode::IfEq => {
                    let val = self.stack.pop().unwrap();
                    if val != 0 {
                        let offset = bc.value();
                        ip = (ip as i64 + offset) as usize;
                    }
                }
                Opcode::IfLe => {
                    let val = self.stack.pop().unwrap();
                    if val <= 0 {
                        let offset = bc.value();
                        ip = (ip as i64 + offset) as usize;
                    }
                }
                Opcode::Goto => {
                    let offset = bc.value();
                    ip = (ip as i64 + offset) as usize;
                }
                Opcode::Invoke => {
                    let method_slot = bc.value();
                    self.execute(method_slot);
                }
                Opcode::Return => {
                    return;
                }
                Opcode::IReturn | Opcode::OReturn => {
                    let val = self.stack.pop().unwrap();
                    self.frames.last_mut().unwrap().return_value = val;
                    return;
                }
                Opcode::Dup => {
                    let val = *self.stack.last().unwrap();
                    self.stack.push(val);
                }
                Opcode::Pop => {
                    self.stack.pop();
                }
                Opcode::ONewArray => {
                    let count = self.stack.pop().unwrap() as usize;
                    let mut elements = Vec::with_capacity(count);
                    for _ in 0..count {
                        elements.push(self.stack.pop().unwrap());
                    }
                    elements.reverse();
                    // Allocate on the heap, push reference (index into arrays vec)
                    let array_ref = self.arrays.len() as Value;
                    self.arrays.push(elements);
                    // Encode as negative to distinguish from regular values
                    // Use a tag: array references are stored as -(array_ref + 1)
                    self.stack.push(-(array_ref + 1));
                }
                _ => {
                    // Handle Debug tag opcodes
                    if opcode == Opcode::Debug {
                        let tag = bc.value();
                        if tag == 1 {
                            self.next_print_is_bool = true;
                        } else if tag == 2 {
                            // array.size(): pop array ref, push its length
                            let obj_ref = self.stack.pop().unwrap();
                            if obj_ref < 0 && obj_ref > -1_000_000 {
                                let arr_idx = (-(obj_ref + 1)) as usize;
                                let len = self.arrays[arr_idx].len() as Value;
                                self.stack.push(len);
                            } else {
                                self.stack.push(0);
                            }
                        } else if tag == 3 {
                            // Create dict from stack: count is on top
                            let count = self.stack.pop().unwrap() as usize;
                            let mut dict = HashMap::new();
                            let mut pairs = Vec::new();
                            for _ in 0..count {
                                let val = self.stack.pop().unwrap();
                                let key = self.stack.pop().unwrap();
                                pairs.push((key, val));
                            }
                            for (k, v) in pairs {
                                dict.insert(k, v);
                            }
                            let dict_ref = self.dicts.len() as Value;
                            self.dicts.push(dict);
                            self.stack.push(-(dict_ref + 1_000_000));
                        } else if tag >= 4 {
                            // Class default initializer: tag = 4 + class_slot * 256
                            let class_slot = (tag - 4) / 256;
                            let init_slot = match self.types.get(class_slot) {
                                JoyeerType::Class(c) => c.default_initializer_slot,
                                _ => -1,
                            };
                            if init_slot >= 0 {
                                // The object ref is on top of the stack
                                // The init function expects it as a parameter
                                self.execute(init_slot);
                            }
                        }
                    }
                }
            }

            ip += 1;
        }
    }

    fn invoke_native(&mut self, func_slot: Slot) {
        if func_slot == BuiltIn::FuncPrint as Slot {
            self.native_print();
        }
    }

    fn native_print(&mut self) {
        let val = self.stack.pop().unwrap_or(0);
        let text = self.format_value(val);

        if !self.output.is_empty() {
            self.output.push('\n');
        }
        self.output.push_str(&text);
        println!("{}", text);
    }

    fn format_value(&mut self, val: Value) -> String {
        if self.next_print_is_bool {
            self.next_print_is_bool = false;
            return if val != 0 { "true".to_string() } else { "false".to_string() };
        }
        if val == NIL_VALUE {
            return "nil".to_string();
        }
        if val >= 3_000_000 {
            // String reference
            let str_idx = (val - 3_000_000) as usize;
            return self.string_heap[str_idx].clone();
        }
        format!("{}", val)
    }

    fn value_to_string(&self, val: Value) -> String {
        if val >= 3_000_000 {
            let str_idx = (val - 3_000_000) as usize;
            return self.string_heap[str_idx].clone();
        }
        format!("{}", val)
    }
}
