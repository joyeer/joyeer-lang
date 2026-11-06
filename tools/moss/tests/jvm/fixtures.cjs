'use strict';

const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');

const names = (
  'nop aconst_null iconst_m1 iconst_0 iconst_1 iconst_2 iconst_3 iconst_4 iconst_5 ' +
  'lconst_0 lconst_1 fconst_0 fconst_1 fconst_2 dconst_0 dconst_1 bipush sipush ldc ldc_w ldc2_w ' +
  'iload lload fload dload aload iload_0 iload_1 iload_2 iload_3 lload_0 lload_1 lload_2 lload_3 ' +
  'fload_0 fload_1 fload_2 fload_3 dload_0 dload_1 dload_2 dload_3 aload_0 aload_1 aload_2 aload_3 ' +
  'iaload laload faload daload aaload baload caload saload istore lstore fstore dstore astore ' +
  'istore_0 istore_1 istore_2 istore_3 lstore_0 lstore_1 lstore_2 lstore_3 fstore_0 fstore_1 fstore_2 fstore_3 ' +
  'dstore_0 dstore_1 dstore_2 dstore_3 astore_0 astore_1 astore_2 astore_3 iastore lastore fastore dastore ' +
  'aastore bastore castore sastore pop pop2 dup dup_x1 dup_x2 dup2 dup2_x1 dup2_x2 swap ' +
  'iadd ladd fadd dadd isub lsub fsub dsub imul lmul fmul dmul idiv ldiv fdiv ddiv irem lrem frem drem ' +
  'ineg lneg fneg dneg ishl lshl ishr lshr iushr lushr iand land ior lor ixor lxor iinc ' +
  'i2l i2f i2d l2i l2f l2d f2i f2l f2d d2i d2l d2f i2b i2c i2s lcmp fcmpl fcmpg dcmpl dcmpg ' +
  'ifeq ifne iflt ifge ifgt ifle if_icmpeq if_icmpne if_icmplt if_icmpge if_icmpgt if_icmple if_acmpeq if_acmpne ' +
  'goto jsr ret tableswitch lookupswitch ireturn lreturn freturn dreturn areturn return ' +
  'getstatic putstatic getfield putfield invokevirtual invokespecial invokestatic invokeinterface invokedynamic ' +
  'new newarray anewarray arraylength athrow checkcast instanceof monitorenter monitorexit wide multianewarray ' +
  'ifnull ifnonnull goto_w jsr_w'
).split(' ');
assert.equal(names.length, 202);

function u1(n) { return Buffer.from([n]); }
function u2(n) { const b = Buffer.alloc(2); b.writeUInt16BE(n); return b; }
function u4(n) { const b = Buffer.alloc(4); b.writeUInt32BE(n); return b; }
function i4(n) { const b = Buffer.alloc(4); b.writeInt32BE(n); return b; }
function cat(...parts) { return Buffer.concat(parts.map(p => Buffer.isBuffer(p) ? p : Buffer.from(p))); }
function mutf8(text) {
  const bytes = [];
  for (let i = 0; i < text.length; i++) {
    const c = text.charCodeAt(i);
    if (c > 0 && c < 128) bytes.push(c);
    else if (c < 2048) bytes.push(192 | (c >> 6), 128 | (c & 63));
    else bytes.push(224 | (c >> 12), 128 | ((c >> 6) & 63), 128 | (c & 63));
  }
  return Buffer.from(bytes);
}

class ClassBuilder {
  constructor(name, major = 69) {
    this.major = major;
    this.minor = 0;
    this.flags = 0x21;
    this.pool = [null];
    this.texts = new Map();
    this.fields = [];
    this.methods = [];
    this.attributes = [];
    this.thisClass = this.klass(name);
    this.superClass = this.klass('java/lang/Object');
    this.codeName = this.utf8('Code');
  }
  add(tag, body, slots = 1) {
    const index = this.pool.length;
    this.pool.push(cat(u1(tag), body));
    if (slots === 2) this.pool.push(null);
    return index;
  }
  utf8(text) {
    if (this.texts.has(text)) return this.texts.get(text);
    const encoded = mutf8(text);
    const index = this.add(1, cat(u2(encoded.length), encoded));
    this.texts.set(text, index);
    return index;
  }
  klass(name) { return this.add(7, u2(this.utf8(name))); }
  nat(name, descriptor) { return this.add(12, cat(u2(this.utf8(name)), u2(this.utf8(descriptor)))); }
  ref(tag, owner, name, descriptor) {
    const ownerIndex = this.klass(owner);
    return this.add(tag, cat(u2(ownerIndex), u2(this.nat(name, descriptor))));
  }
  attr(name, body) { return cat(u2(this.utf8(name)), u4(body.length), body); }
  method(name, descriptor, bytes, options = {}) {
    const nameIndex = this.utf8(name);
    const typeIndex = this.utf8(descriptor);
    const attributes = options.attributes || [];
    if (bytes !== null) {
      const handlers = options.handlers || [];
      const nested = options.nested || [];
      const body = cat(
        u2(options.maxStack ?? 8), u2(options.maxLocals ?? 8), u4(bytes.length), bytes,
        u2(handlers.length), ...handlers.map(h => cat(...h.map(u2))),
        u2(nested.length), ...nested,
      );
      attributes.unshift(cat(u2(this.codeName), u4(body.length), body));
    }
    this.methods.push(cat(u2(options.flags ?? 9), u2(nameIndex), u2(typeIndex), u2(attributes.length), ...attributes));
  }
  bytes() {
    return cat(
      u4(0xcafebabe), u2(this.minor), u2(this.major), u2(this.pool.length),
      ...this.pool.slice(1).filter(Boolean), u2(this.flags), u2(this.thisClass), u2(this.superClass),
      u2(0), u2(this.fields.length), ...this.fields, u2(this.methods.length), ...this.methods,
      u2(this.attributes.length), ...this.attributes,
    );
  }
}

function switchCode(opcode, prefix, keys) {
  const pad = (4 - ((prefix.length + 1) % 4)) % 4;
  const headerSize = opcode === 170 ? 12 : 8;
  const targetsSize = keys.length * (opcode === 170 ? 4 : 8);
  const after = prefix.length + 1 + pad + headerSize + targetsSize;
  const opcodeOffset = prefix.length;
  const targets = keys.map((_, i) => after + i * 2);
  const fallback = after + keys.length * 2;
  let payload;
  if (opcode === 170) {
    payload = cat(i4(fallback - opcodeOffset), i4(keys[0]), i4(keys.at(-1)),
      ...targets.map(t => i4(t - opcodeOffset)));
  } else {
    payload = cat(i4(fallback - opcodeOffset), i4(keys.length),
      ...keys.map((k, i) => cat(i4(k), i4(targets[i] - opcodeOffset))));
  }
  const cases = keys.map((_, i) => [4 + i, 172]);
  return cat(prefix, u1(opcode), Buffer.alloc(pad, 0x42), payload, ...cases, [7, 172]);
}

function example(major, unicode = false) {
  const b = new ClassBuilder(unicode ? 'Example\u4e2d' : 'Example', major);
  b.flags = 0x421;
  const init = b.ref(10, 'java/lang/Object', '<init>', '()V');
  const runtimeException = b.klass('java/lang/RuntimeException');
  const valueName = b.utf8('value');
  const integerType = b.utf8('I');
  const constant = b.add(3, u4(42));
  const cv = b.attr('ConstantValue', u2(constant));
  b.fields.push(cat(u2(0x19), u2(valueName), u2(integerType), u2(1), cv));
  b.method('<init>', '()V', cat([42, 183], u2(init), [177]), { flags: 1, maxStack: 1, maxLocals: 1 });
  b.method(unicode ? 'sum\u4e2d' : 'sum', '(II)I', Buffer.from([26, 27, 96, 172]), { maxStack: 2, maxLocals: 2 });
  b.method(unicode ? 'sum\u4e2d' : 'sum', '(I)I', Buffer.from([26, 172]), { maxStack: 1, maxLocals: 1 });
  b.method('wide', '()I', Buffer.from([17, 3, 232, 196, 54, 1, 44, 196, 132, 1, 44, 128, 0, 196, 21, 1, 44, 172]),
    { maxStack: 1, maxLocals: 301 });
  b.method('loop', '(I)I', Buffer.from([26, 158, 0, 9, 132, 0, 255, 167, 255, 249, 3, 172]),
    { maxStack: 1, maxLocals: 1 });
  b.method('dense', '(I)I', switchCode(170, [26], [-1, 0, 1]), { maxStack: 1, maxLocals: 1 });
  b.method('sparse', '(I)I', switchCode(171, [26], [-100, 1000]), { maxStack: 1, maxLocals: 1 });
  b.method('nativeValue', '()I', null, { flags: 0x109 });
  b.method('abstractValue', '()I', null, { flags: 0x401 });
  const local = b.utf8('number');
  const lines = b.attr('LineNumberTable', cat(u2(2), u2(0), u2(10), u2(2), u2(12)));
  const locals = b.attr('LocalVariableTable', cat(u2(1), u2(0), u2(5), u2(local), u2(integerType), u2(0)));
  b.method('caught', '(I)I', Buffer.from([26, 172, 87, 2, 172]),
    { maxStack: 1, maxLocals: 1, handlers: [[0, 2, 2, runtimeException]], nested: [lines, locals] });
  b.attributes.push(b.attr('SourceFile', u2(b.utf8('Example.java'))));
  b.attributes.push(b.attr('MossUnknown', Buffer.from([0, 255, 13, 10])));
  return b.bytes();
}

function feature() {
  const b = new ClassBuilder('ConstantKinds');
  const stringText = b.utf8('quote" slash\\ nul\0 line\n \u4e2d \ud83d\ude00 lone\ud800');
  const string = b.add(8, u2(stringText));
  const integer = b.add(3, u4(0x80000000));
  const float = b.add(4, u4(0x7fc00001));
  const long = b.add(5, Buffer.from('ffffffffffffffff', 'hex'), 2);
  const double = b.add(6, Buffer.from('8000000000000000', 'hex'), 2);
  const klass = b.klass('[[I');
  const field = b.ref(9, 'ConstantKinds', 'field', 'I');
  const method = b.ref(10, 'ConstantKinds', 'bootstrap', '()I');
  const iface = b.ref(11, 'java/util/List', 'size', '()I');
  const handle = b.add(15, cat(u1(6), u2(method)));
  const type = b.add(16, u2(b.utf8('()I')));
  const dynamic = b.add(17, cat(u2(0), u2(b.nat('dynamicValue', 'I'))));
  const invoke = b.add(18, cat(u2(0), u2(b.nat('dynamicCall', '()I'))));
  const loads = cat([18, string, 87, 18, integer, 87, 18, float, 87, 20], u2(long), [88, 20], u2(double),
    [88, 18, klass, 87, 18, handle, 87, 18, type, 87, 18, dynamic, 87, 186], u2(invoke), [0, 0, 172]);
  b.method('constants', '()I', loads, { maxLocals: 0 });
  b.attributes.push(b.attr('BootstrapMethods', cat(u2(1), u2(handle), u2(2), u2(integer), u2(string))));
  return { bytes: b.bytes(), indexes: { stringText, string, integer, float, long, double, klass, field, method, iface, handle, type, dynamic, invoke } };
}

function moduleInfo() {
  const b = new ClassBuilder('module-info', 53);
  b.flags = 0x8000;
  b.superClass = 0;
  const module = b.add(19, u2(b.utf8('moss.example')));
  const base = b.add(19, u2(b.utf8('java.base')));
  const pkg = b.add(20, u2(b.utf8('moss/example')));
  b.attributes.push(b.attr('Module', cat(u2(module), u2(0), u2(0),
    u2(1), u2(base), u2(0x8000), u2(0), u2(1), u2(pkg), u2(0), u2(0),
    u2(0), u2(0), u2(0))));
  b.attributes.push(b.attr('ModulePackages', cat(u2(1), u2(pkg))));
  return b.bytes();
}

function opcodeMatrix() {
  const b = new ClassBuilder('OpcodeMatrix');
  const int = b.add(3, u4(1));
  const long = b.add(5, Buffer.alloc(8), 2);
  const field = b.ref(9, 'OpcodeMatrix', 'field', 'I');
  const virtual = b.ref(10, 'java/lang/Object', 'hashCode', '()I');
  const special = b.ref(10, 'java/lang/Object', '<init>', '()V');
  const staticRef = b.ref(10, 'OpcodeMatrix', 'helper', '()V');
  const iface = b.ref(11, 'java/util/List', 'size', '()I');
  const handle = b.add(15, cat(u1(6), u2(staticRef)));
  const invoke = b.add(18, cat(u2(0), u2(b.nat('call', '()I'))));
  const klass = b.klass('java/lang/Object');
  const array = b.klass('[[I');
  const expected = [];
  for (let op = 0; op <= 201; op++) {
    if ([168, 169, 196, 201].includes(op)) continue;
    let bytes = u1(op);
    let type = '()V';
    let operands = [];
    if (op === 16) { bytes = cat(bytes, [128]); operands = [{ kind: 'immediate', value: -128 }]; }
    else if (op === 17) { bytes = cat(bytes, [128, 0]); operands = [{ kind: 'immediate', value: -32768 }]; }
    else if ([18, 19, 20].includes(op)) {
      const index = op === 20 ? long : int;
      bytes = cat(bytes, op === 18 ? u1(index) : u2(index));
      operands = [{ kind: 'constant', index }];
    } else if ((op >= 21 && op <= 25) || (op >= 54 && op <= 58) || op === 132) {
      bytes = cat(bytes, [255], op === 132 ? [128] : []);
      operands = [{ kind: 'local', index: 255 }];
      if (op === 132) operands.push({ kind: 'immediate', value: -128 });
    } else if ((op >= 26 && op <= 45) || (op >= 59 && op <= 78)) {
      operands = [{ kind: 'local', index: (op - (op >= 59 ? 59 : 26)) % 4 }];
    } else if ((op >= 153 && op <= 167) || [198, 199, 200].includes(op)) {
      bytes = cat(bytes, op === 200 ? i4(0) : u2(0));
      operands = [{ kind: 'target', methodOffset: 0 }];
    } else if (op === 170 || op === 171) {
      bytes = op === 170 ? cat(bytes, [1, 2, 3], i4(0), i4(-1), i4(-1), i4(0)) :
        cat(bytes, [1, 2, 3], i4(0), i4(1), i4(-10), i4(0));
      operands = [{ kind: 'defaultTarget', methodOffset: 0 },
        { kind: 'switchTarget', key: op === 170 ? -1 : -10, methodOffset: 0 }];
    } else if (op >= 172 && op <= 177) {
      type = ['()I', '()J', '()F', '()D', '()Ljava/lang/Object;', '()V'][op - 172];
    } else if ((op >= 178 && op <= 187) || [189, 192, 193, 197].includes(op)) {
      const index = op <= 181 ? field : op === 182 ? virtual : op === 183 ? special :
        op === 184 ? staticRef : op === 185 ? iface : op === 186 ? invoke : op === 197 ? array : klass;
      bytes = cat(bytes, u2(index));
      operands = [{ kind: 'constant', index }];
      if (op === 185) { bytes = cat(bytes, [1, 0]); operands.push({ kind: 'count', value: 1 }); }
      if (op === 186) bytes = cat(bytes, [0, 0]);
      if (op === 197) { bytes = cat(bytes, [2]); operands.push({ kind: 'dimensions', value: 2 }); }
    } else if (op === 188) {
      bytes = cat(bytes, [11]); operands = [{ kind: 'arrayType', value: 11 }];
    }
    const size = bytes.length;
    if (op < 172 || op > 177) bytes = cat(bytes, [177]);
    b.method(`op${op}`, type, bytes, { maxLocals: 65535 });
    expected.push({ name: `op${op}`, opcode: op, mnemonic: names[op], size, operands });
  }
  for (const modified of [21, 22, 23, 24, 25, 54, 55, 56, 57, 58, 132]) {
    let bytes = cat([196, modified], u2(300));
    const operands = [{ kind: 'local', index: 300 }];
    if (modified === 132) { bytes = cat(bytes, [128, 0]); operands.push({ kind: 'immediate', value: -32768 }); }
    const size = bytes.length;
    b.method(`wide${modified}`, '()V', cat(bytes, [177]), { maxLocals: 302 });
    expected.push({ name: `wide${modified}`, opcode: modified, rawOpcode: 196, mnemonic: names[modified], size, operands });
  }
  for (const op of [170, 171]) {
    for (let pc = 0; pc < 4; pc++) {
      const prefix = Buffer.alloc(pc);
      const pad = (4 - ((pc + 1) % 4)) % 4;
      const body = op === 170 ? cat(i4(0), i4(0), i4(0), i4(0)) : cat(i4(0), i4(0));
      const bytes = cat(prefix, [op], Buffer.alloc(pad, 0x61), body, [177]);
      b.method(`align${op}_${pc}`, '()V', bytes);
      expected.push({ name: `align${op}_${pc}`, instructionIndex: pc, methodOffset: pc, opcode: op,
        mnemonic: names[op], size: 1 + pad + body.length,
        operands: [{ kind: 'defaultTarget', methodOffset: pc },
          ...(op === 170 ? [{ kind: 'switchTarget', key: 0, methodOffset: pc }] : [])] });
    }
  }
  b.attributes.push(b.attr('BootstrapMethods', cat(u2(1), u2(handle), u2(0))));
  return { bytes: b.bytes(), expected };
}

function generate(directory) {
  fs.mkdirSync(directory, { recursive: true });
  const write = (name, bytes) => fs.writeFileSync(path.join(directory, name), bytes);
  for (const [java, major] of [[8, 52], [17, 61], [21, 65], [25, 69]]) write(`Example${java}.class`, example(major));
  write('Unicode.class', example(69, true));
  const weird = new ClassBuilder('Nul\0Class');
  weird.method('zero\0method', '()V', Buffer.from([177]), { maxLocals: 0 });
  weird.method('\ud800', '()V', Buffer.from([177]), { maxLocals: 0 });
  write('WeirdNames.class', weird.bytes());
  const preview = example(69);
  preview.writeUInt16BE(65535, 4);
  write('Preview.class', preview);
  const constants = feature();
  write('ConstantKinds.class', constants.bytes);
  write('constants.json', JSON.stringify(constants.indexes, null, 2) + '\n');
  write('Module.class', moduleInfo());
  const matrix = opcodeMatrix();
  write('OpcodeMatrix.class', matrix.bytes);
  write('opcodes.json', JSON.stringify(matrix.expected, null, 2) + '\n');
  console.log(`Generated self-authored JVM fixtures in ${directory}`);
}

if (require.main === module) {
  if (process.argv.length !== 3) throw new Error('usage: node fixtures.cjs <output-directory>');
  generate(path.resolve(process.argv[2]));
}

module.exports = { ClassBuilder, cat, u1, u2, u4, i4, example, feature, opcodeMatrix, names, generate };
