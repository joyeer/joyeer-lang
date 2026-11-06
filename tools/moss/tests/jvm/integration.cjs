'use strict';

const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const { performance } = require('node:perf_hooks');
const { ClassBuilder, cat, u1, u2, u4, i4 } = require('./fixtures.cjs');

const root = path.resolve(__dirname, '..', '..');
const fixtures = path.join(root, 'tests', 'fixtures', 'jvm');
if (![3, 5].includes(process.argv.length) || (process.argv.length === 5 && process.argv[3] !== '--javap')) {
  throw new Error('usage: node integration.cjs <moss-executable> [--javap <explicit-javap-path>]');
}
const executable = path.resolve(process.argv[2]);
const javap = process.argv.length === 5 ? path.resolve(process.argv[4]) : null;
const work = path.join(root, 'target', `jvm-integration-${process.pid}-${crypto.randomUUID()}`);
fs.mkdirSync(work);
const owned = new Set();
let sequence = 0;
let checks = 0;

function invoke(args) {
  const result = spawnSync(executable, args, { cwd: root, encoding: 'utf8', timeout: 30000, maxBuffer: 32 * 1024 * 1024 });
  if (result.error) throw result.error;
  assert.equal(result.signal, null, `process signaled: ${result.signal}`);
  assert.ok([0, 1, 2].includes(result.status), `unexpected status ${result.status}: ${result.stderr}`);
  return result;
}
function check(condition, label) { assert.ok(condition, label); checks++; }
function same(actual, expected, label) { assert.equal(actual, expected, label); checks++; }
function file(name) { return path.join(fixtures, name); }
function temporary(bytes, suffix = '.class') {
  const location = path.join(work, `input-${sequence++}${suffix}`);
  fs.writeFileSync(location, bytes, { flag: 'wx' });
  owned.add(location);
  return location;
}
// Runs a query that must succeed, writing to a fresh file; returns the exact file contents.
function text(args) {
  const output = path.join(work, `output-${sequence++}.txt`);
  const result = invoke([...args, '--output', output]);
  if (fs.existsSync(output)) owned.add(output);
  assert.equal(result.status, 0, result.stderr);
  assert.equal(result.stdout, '');
  assert.equal(result.stderr, '');
  const raw = fs.readFileSync(output);
  assert.ok(raw.length <= 8388608);
  assert.equal(raw.at(-1), 10);
  checks++;
  return raw.toString('utf8');
}
function failure(args, code, status = 1, output = true) {
  const destination = path.join(work, `failure-${sequence++}.txt`);
  const result = invoke(output ? [...args, '--output', destination] : args);
  if (fs.existsSync(destination)) owned.add(destination);
  assert.equal(result.status, status, result.stderr);
  assert.equal(result.stdout, '');
  assert.ok(result.stderr.includes(code), result.stderr);
  if (output) assert.equal(fs.existsSync(destination), false, 'failed query created output');
  checks++;
}
function mutated(source, offset, bytes) {
  const copy = Buffer.from(source);
  Buffer.from(bytes).copy(copy, offset);
  return temporary(copy);
}
function outline(bytes) {
  const pool = [null];
  let cursor = 10;
  for (let i = 1; i < bytes.readUInt16BE(8); i++) {
    const start = cursor;
    const tag = bytes[cursor++];
    let size;
    if (tag === 1) { size = 2 + bytes.readUInt16BE(cursor); }
    else if ([3, 4, 9, 10, 11, 12, 17, 18].includes(tag)) size = 4;
    else if ([5, 6].includes(tag)) size = 8;
    else if (tag === 15) size = 3;
    else size = 2;
    cursor += size;
    pool[i] = { start, tag, size: cursor - start };
    if ([5, 6].includes(tag)) { pool[++i] = null; }
  }
  return { pool, header: cursor };
}
// Byte offsets of the structures the mutation tests patch (a minimal independent class-file scan).
function scan(bytes) {
  const { pool, header } = outline(bytes);
  const name = index => bytes.toString('latin1', pool[index].start + 3, pool[index].start + pool[index].size);
  let cursor = header + 6;
  cursor += 2 + 2 * bytes.readUInt16BE(cursor);
  function attributes(position) {
    const count = bytes.readUInt16BE(position);
    position += 2;
    const list = [];
    for (let i = 0; i < count; i++) {
      const length = bytes.readUInt32BE(position + 2);
      list.push({ name: name(bytes.readUInt16BE(position)), header: position, body: position + 6, length });
      position += 6 + length;
    }
    return { list, end: position };
  }
  function members(position) {
    const count = bytes.readUInt16BE(position);
    position += 2;
    const list = [];
    for (let i = 0; i < count; i++) {
      const start = position;
      const parsed = attributes(position + 6);
      const member = { offset: start, name: name(bytes.readUInt16BE(start + 2)), attributes: parsed.list, code: null };
      const code = parsed.list.find(a => a.name === 'Code');
      if (code) {
        const codeLength = bytes.readUInt32BE(code.body + 4);
        const tableAt = code.body + 8 + codeLength;
        const inner = attributes(tableAt + 2 + 8 * bytes.readUInt16BE(tableAt));
        const lines = inner.list.find(a => a.name === 'LineNumberTable');
        member.code = {
          attributeOffset: code.header,
          codeOffset: code.body + 8,
          firstException: tableAt + 2,
          firstLine: lines ? lines.body + 2 : null,
        };
      }
      list.push(member);
      position = parsed.end;
    }
    return { list, end: position };
  }
  const fields = members(cursor);
  const methods = members(fields.end);
  return { pool, header, fields: fields.list, methods: methods.list, attributes: attributes(methods.end).list };
}
function oneMethod(bytes, descriptor = '()V', options = {}) {
  const builder = new ClassBuilder('Malformed');
  builder.method('test', descriptor, bytes, { maxLocals: 8, ...options });
  return temporary(builder.bytes());
}
function instructionLines(output) {
  return output.split('\n').filter(line => /^ +\d+: /.test(line));
}
function methodLines(output) {
  return output.split('\n').filter(line => /;\s+\/\/ index \d+, descriptor /.test(line));
}

try {
  const sampleBytes = fs.readFileSync(file('Example25.class'));

  // The byte-exact contract: javap -c -p -l output (JDK 25) for the ASCII fixtures.
  for (const name of ['Example8', 'Example17', 'Example21', 'Example25', 'OpcodeMatrix']) {
    same(text(['disasm', file(`${name}.class`)]), fs.readFileSync(file(`${name}.javap.txt`), 'utf8'), `${name} matches javap`);
  }
  // The same class read from the archive prints identically.
  const jar = file('Fixtures.jar');
  same(text(['disasm', jar, '--entry', 'pkg/Example21.class']), fs.readFileSync(file('Example21.javap.txt'), 'utf8'),
    'archive entry matches javap');

  // Inventory (javap -p -v style, without bytecode).
  const inventory = text(['inspect', file('Example25.class')]);
  check(inventory.startsWith('Compiled from "Example.java"\npublic abstract class Example {\n'), 'inspect header');
  check(inventory.includes('// version 69.0, flags 0x0421, constant pool 33, fields 1, methods 10'), 'inspect summary');
  check(inventory.includes('  Constant pool:\n') && inventory.includes('Methodref') && inventory.includes('Utf8'),
    'inspect lists the constant pool');
  check(inventory.includes('  public static final int value;\n'), 'inspect lists fields');
  check(inventory.includes('native') && inventory.includes('abstract'), 'native and abstract methods remain visible');
  for (let major = 52; major <= 69; major++) {
    const bytes = Buffer.from(sampleBytes);
    bytes.writeUInt16BE(major, 6);
    check(text(['inspect', temporary(bytes)]).includes(`// version ${major}.0,`), `version ${major}`);
  }
  failure(['inspect', file('Preview.class')], 'jvm.unsupported-version');
  check(text(['inspect', file('Preview.class'), '--allow-preview']).includes('// version 69.65535,'), 'preview opt-in');
  const oldPreview = Buffer.from(sampleBytes);
  oldPreview.writeUInt16BE(65535, 4);
  oldPreview.writeUInt16BE(65, 6);
  failure(['inspect', temporary(oldPreview), '--allow-preview'], 'jvm.unsupported-version');
  for (const major of [0, 51, 70, 65535]) {
    const bytes = Buffer.from(sampleBytes);
    bytes.writeUInt16BE(major, 6);
    failure(['inspect', temporary(bytes)], 'jvm.unsupported-version');
  }

  // Constants of every kind, as javap comments them (non-ASCII is escaped, never replaced).
  const constants = text(['disasm', file('ConstantKinds.class'), '--method', 'constants']);
  for (const expected of [
    '// String quote\\" slash\\\\ nul\\u0000 line\\n \\u4e2d \\ud83d\\ude00 lone\\ud800',
    '// int -2147483648', '// float NaNf', '// long -1l', '// double -0.0d', '// class "[[I"',
    '// MethodHandle REF_invokeStatic ConstantKinds.bootstrap:()I', '// MethodType ()I',
    '// Dynamic #0:dynamicValue:I', '// InvokeDynamic #0:dynamicCall:()I']) {
    check(constants.includes(expected), `constant comment ${expected}`);
  }
  const indexes = JSON.parse(fs.readFileSync(file('constants.json'), 'utf8'));
  const moduleText = text(['inspect', file('Module.class')]);
  check(moduleText.includes(' = Module ') && moduleText.includes(' = Package '), 'module and package constants');

  const unicodePath = path.join(work, '分析 input.class');
  fs.writeFileSync(unicodePath, fs.readFileSync(file('Unicode.class')), { flag: 'wx' });
  owned.add(unicodePath);
  const unicode = text(['disasm', unicodePath, '--method', 'sum中', '--descriptor', '(II)I']);
  check(unicode.includes('public abstract class Example\\u4e2d {') && unicode.includes('public static int sum\\u4e2d(int, int);'),
    'Unicode path, class, and method selection');
  const weird = text(['disasm', file('WeirdNames.class'), '--method-index', '0']);
  check(weird.includes('class Nul\\u0000Class {') && weird.includes('zero\\u0000method('),
    'NUL code units in legal JVM names are not replaced or rejected');
  check(text(['disasm', file('WeirdNames.class'), '--method-index', '1']).includes(' \\ud800('),
    'inventory-index selection reaches names not encodable in UTF-8 argv');

  // Every opcode of the matrix prints at its offset with javap's mnemonic.
  const matrix = JSON.parse(fs.readFileSync(file('opcodes.json'), 'utf8'));
  for (const expected of matrix) {
    const lines = instructionLines(text(['disasm', file('OpcodeMatrix.class'), '--method', expected.name]));
    const offset = expected.methodOffset || 0;
    const mnemonic = expected.rawOpcode === 196 ? `${expected.mnemonic}_w` : expected.mnemonic;
    check(lines.some(line => new RegExp(`^ +${offset}: ${mnemonic}(\\s|$)`).test(line)), `${expected.name} ${mnemonic}@${offset}`);
  }

  // Selection and paging.
  const full = text(['disasm', file('Example25.class'), '--method', 'wide']);
  same(text(['disasm', file('Example25.class'), '--method-index', '3']), full, 'name and inventory-index selectors agree');
  check(full.includes('iinc_w        300, -32768'), 'wide iinc is not narrowed');
  const everything = instructionLines(full);
  let offset = 0;
  const paged = [];
  for (;;) {
    const page = text(['disasm', file('Example25.class'), '--method', 'wide', '--offset', String(offset), '--limit', '2']);
    paged.push(...instructionLines(page));
    const more = /\/\/ more: continue with --offset (\d+)\n$/.exec(page);
    if (!more) break;
    assert.ok(Number(more[1]) > offset);
    offset = Number(more[1]);
  }
  assert.deepEqual(paged, everything);
  checks++;
  const endPage = text(['disasm', file('Example25.class'), '--method', 'wide', '--offset', '18']);
  check(instructionLines(endPage).length === 0 && !endPage.includes('// more'), 'terminal offset');
  const native = text(['disasm', file('Example25.class'), '--method', 'nativeValue']);
  check(native.includes(' native ') && !native.includes('Code:'), 'native method disassembly has no fabricated code');
  failure(['disasm', file('Example25.class'), '--method', 'sum'], 'query.ambiguous-method');
  failure(['disasm', file('Example25.class'), '--method', 'sum', '--descriptor', '()I'], 'query.method-not-found');
  failure(['disasm', file('Example25.class'), '--method-index', '10'], 'query.method-not-found');
  failure(['disasm', file('Example25.class'), '--method', 'wide', '--offset', '4'], 'query.invalid-offset');
  failure(['disasm', file('Example25.class'), '--method', 'wide', '--offset', '19'], 'query.invalid-offset');
  failure(['methods', file('Example25.class'), '--offset', '11'], 'query.invalid-offset');
  const methods = text(['methods', file('Example25.class'), '--offset', '7', '--limit', '2']);
  check(methodLines(methods).length === 2 && methods.includes('// index 7, descriptor') &&
    methods.includes('// index 8, descriptor') && methods.endsWith('// more: continue with --offset 9\n'),
    'method-index pagination');
  check(invoke(['methods', file('Example25.class')]).stdout.includes('nativeValue'), 'human text on stdout');
  failure(['methods', file('Example25.class'), '--format', 'json'], 'cli.usage', 2, false);
  failure(['methods', file('Example25.class'), '--unchecked-stdout'], 'cli.usage', 2, false);
  for (const args of [
    ['disasm'], ['unknown'], ['--help', 'extra'],
    ['methods', file('Example25.class'), '--limit', '0'],
    ['methods', file('Example25.class'), '--limit', '10001'],
    ['methods', file('Example25.class'), '--offset', '-1'],
    ['methods', file('Example25.class'), '--offset', '99999999999999999999999999999999'],
    ['methods', file('Example25.class'), '--limit', '1', '--limit', '2'],
    ['inspect', file('Example25.class'), '--offset', '0'],
    ['methods', file('Example25.class'), '--method', 'wide'],
    ['disasm', file('Example25.class'), '--method', 'wide', '--method-index', '3'],
    ['disasm', file('Example25.class'), '--method-index', '3', '--descriptor', '()I'],
    ['methods', file('Example25.class'), '--output'],
  ]) failure(args, 'cli.usage', 2, false);
  failure(['inspect', path.join(work, 'missing.class')], 'input.inspect-failed');
  failure(['inspect', work], 'input.not-regular');
  failure(['inspect', temporary(Buffer.alloc(0))], 'binary.truncated');
  failure(['inspect', temporary(Buffer.from('not a class'))], 'jvm.invalid-magic');

  const out = path.join(work, 'protected.txt');
  fs.writeFileSync(out, 'preserve me', { flag: 'wx' });
  owned.add(out);
  const existing = invoke(['inspect', file('Example25.class'), '--output', out]);
  check(existing.status === 1 && existing.stdout === '' && existing.stderr.includes('output.write-failed') &&
    fs.readFileSync(out, 'utf8') === 'preserve me', 'existing output is never overwritten');

  // Malformed input never traps: each mutation reports a stable diagnostic code.
  const shape = outline(sampleBytes);
  const sample = scan(sampleBytes);
  failure(['inspect', mutated(sampleBytes, 8, [0, 0])], 'jvm.invalid-constant-count');
  failure(['inspect', mutated(sampleBytes, shape.pool[1].start, [255])], 'jvm.unsupported-constant-tag');
  for (const encoding of [[0], [128], [240], [193, 191], [224, 128, 128]]) {
    failure(['inspect', mutated(sampleBytes, shape.pool[1].start + 3, encoding)], 'jvm.invalid-mutf8');
  }
  failure(['inspect', mutated(sampleBytes, shape.pool[1].start + shape.pool[1].size - 1, [194])], 'binary.truncated');
  failure(['inspect', mutated(sampleBytes, shape.pool[2].start + 1, [0, 0])], 'jvm.invalid-constant-index');
  failure(['inspect', mutated(sampleBytes, shape.pool[2].start + 1, u2(2))], 'jvm.invalid-constant-tag');
  failure(['inspect', mutated(sampleBytes, shape.header + 2, [0, 0])], 'jvm.invalid-constant-index');
  failure(['inspect', mutated(sampleBytes, shape.header + 4, [0, 0])], 'jvm.invalid-super-class');
  failure(['inspect', mutated(sampleBytes, shape.header, u2(0x431))], 'jvm.invalid-class-flags');
  failure(['inspect', temporary(new ClassBuilder('java/lang/Object').bytes())], 'jvm.invalid-super-class');
  failure(['inspect', mutated(sampleBytes, sample.fields[0].offset, u2(0x59))], 'jvm.invalid-member-flags');
  failure(['inspect', mutated(sampleBytes, sample.methods[1].offset, u2(0x0b))], 'jvm.invalid-member-flags');
  failure(['inspect', temporary(cat(sampleBytes, [0]))], 'jvm.trailing-bytes');
  failure(['inspect', mutated(sampleBytes, sample.methods[0].offset, u2(0x101))], 'jvm.invalid-method');
  const wide = sample.methods[3].code;
  failure(['inspect', mutated(sampleBytes, wide.codeOffset - 4, u4(0))], 'jvm.invalid-code-length');
  failure(['inspect', mutated(sampleBytes, wide.codeOffset - 4, u4(65536))], 'jvm.invalid-code-length');
  failure(['inspect', mutated(sampleBytes, sample.methods[1].code.attributeOffset + 8, u2(1))], 'jvm.invalid-local-index');
  const duplicate = new ClassBuilder('Duplicates');
  duplicate.method('test', '()V', Buffer.from([177]));
  duplicate.method('test', '()V', Buffer.from([177]));
  failure(['inspect', temporary(duplicate.bytes())], 'jvm.duplicate-member');
  const noCode = new ClassBuilder('NoCode');
  noCode.method('test', '()V', null);
  failure(['inspect', temporary(noCode.bytes())], 'jvm.invalid-method-code');
  const dual = new ClassBuilder('Dual');
  dual.add(5, Buffer.alloc(8), 2);
  const dualBytes = dual.bytes();
  dualBytes.writeUInt16BE(dual.pool.length - 1, 8);
  failure(['inspect', temporary(dualBytes)], 'jvm.invalid-constant-count');

  const constantBytes = fs.readFileSync(file('ConstantKinds.class'));
  const constantShape = outline(constantBytes);
  failure(['inspect', mutated(constantBytes, constantShape.pool[indexes.handle].start + 1, [0])], 'jvm.invalid-method-handle');
  failure(['inspect', mutated(constantBytes, constantShape.pool[indexes.handle].start + 1, [8])], 'jvm.invalid-method-handle');
  failure(['inspect', mutated(constantBytes, constantShape.pool[indexes.dynamic].start + 1, u2(65535))], 'jvm.invalid-bootstrap');
  const oldDynamic = Buffer.from(constantBytes);
  oldDynamic.writeUInt16BE(52, 6);
  failure(['inspect', temporary(oldDynamic)], 'jvm.invalid-constant-version');
  const moduleBytes = fs.readFileSync(file('Module.class'));
  const moduleAttribute = scan(moduleBytes).attributes.find(a => a.name === 'Module');
  failure(['inspect', mutated(moduleBytes, moduleAttribute.body, u2(0))], 'jvm.invalid-constant-index');
  const caught = sample.methods[9].code;
  failure(['inspect', mutated(sampleBytes, caught.firstException, u2(2))], 'jvm.invalid-exception-range');
  failure(['inspect', mutated(sampleBytes, caught.firstLine, u2(5))], 'jvm.invalid-debug-range');

  for (const bytes of [[202, 177], [254, 177], [255, 177], [168, 0, 0, 177], [169, 0, 177], [201, 0, 0, 0, 0, 177]]) {
    failure(['disasm', oneMethod(Buffer.from(bytes)), '--method', 'test'], 'jvm.invalid-opcode');
  }
  for (const bytes of [[196, 177], [196, 169, 0, 0, 177]]) {
    failure(['disasm', oneMethod(Buffer.from(bytes)), '--method', 'test'], 'jvm.invalid-wide');
  }
  for (const bytes of [[16], [17, 0], [196, 21, 0], [196, 132, 0, 0, 0], [200, 0, 0, 0]]) {
    failure(['disasm', oneMethod(Buffer.from(bytes)), '--method', 'test'], 'binary.truncated');
  }
  failure(['disasm', oneMethod(Buffer.from([0])), '--method', 'test'], 'jvm.fallthrough-end');
  failure(['disasm', oneMethod(Buffer.from([172])), '--method', 'test'], 'jvm.invalid-return');
  failure(['disasm', oneMethod(Buffer.from([21, 8, 177])), '--method', 'test'], 'jvm.invalid-local-index');
  failure(['disasm', oneMethod(Buffer.from([196, 21, 255, 255, 177]), '()V', { maxLocals: 65535 }),
    '--method', 'test'], 'jvm.invalid-local-index');
  failure(['disasm', oneMethod(Buffer.from([167, 0, 1, 177])), '--method', 'test'], 'jvm.invalid-branch-target');
  failure(['disasm', oneMethod(cat([200], i4(-2147483648), [177])), '--method', 'test'], 'jvm.invalid-branch-target');
  failure(['disasm', oneMethod(Buffer.from([188, 3, 177])), '--method', 'test'], 'jvm.invalid-array');
  failure(['disasm', oneMethod(Buffer.from([16, 0, 87, 177]), '()V', { handlers: [[0, 2, 1, 0]] }),
    '--method', 'test'], 'jvm.invalid-exception-range');
  failure(['disasm', oneMethod(cat([170, 0, 0, 0], i4(0), i4(-2147483648), i4(2147483647), [177])),
    '--method', 'test'], 'jvm.invalid-switch');
  failure(['disasm', oneMethod(cat([171, 0, 0, 0], i4(0), i4(-1), [177])),
    '--method', 'test'], 'jvm.invalid-switch');
  failure(['disasm', oneMethod(cat([171, 0, 0, 0], i4(0), i4(2), i4(1), i4(0), i4(1), i4(0), [177])),
    '--method', 'test'], 'jvm.invalid-switch');
  const matrixBytes = fs.readFileSync(file('OpcodeMatrix.class'));
  const matrixMethods = new Map(scan(matrixBytes).methods.map(m => [m.name, m]));
  failure(['disasm', mutated(matrixBytes, matrixMethods.get('op185').code.codeOffset + 4, [1]),
    '--method', 'op185'], 'jvm.invalid-invoke');
  failure(['disasm', mutated(matrixBytes, matrixMethods.get('op186').code.codeOffset + 3, [1]),
    '--method', 'op186'], 'jvm.invalid-invoke');
  failure(['disasm', mutated(matrixBytes, matrixMethods.get('op197').code.codeOffset + 3, [0]),
    '--method', 'op197'], 'jvm.invalid-array');
  failure(['disasm', mutated(matrixBytes, matrixMethods.get('op18').code.codeOffset + 1, [0]),
    '--method', 'op18'], 'jvm.invalid-constant-index');

  // Limits: the largest legal Code attribute, parse budgets, and the 8 MiB output ceiling.
  const large = new ClassBuilder('MaximumCode');
  const maximumCode = Buffer.alloc(65535);
  maximumCode[65534] = 177;
  large.method('large', '()V', maximumCode, { maxLocals: 0 });
  const largePath = temporary(large.bytes());
  const largePage = text(['disasm', largePath, '--method', 'large', '--limit', '10000']);
  check(instructionLines(largePage).length === 10000 && largePage.endsWith('// more: continue with --offset 10000\n'),
    'maximum code length and page ceiling');
  check(instructionLines(text(['disasm', largePath, '--method', 'large', '--offset', '65535'])).length === 0,
    'maximum terminal offset');
  const textBudget = new ClassBuilder('TextBudget');
  for (let index = 0; index < 17; index++) {
    textBudget.add(1, cat(u2(65535), Buffer.alloc(65535, 65 + index)));
  }
  failure(['inspect', temporary(textBudget.bytes())], 'jvm.resource-limit');
  const metadataBudget = new ClassBuilder('MetadataBudget');
  metadataBudget.method('first', '()V', null, { flags: 0x109 });
  const opaque = metadataBudget.attr('Opaque', Buffer.alloc(0));
  for (let index = 0; index < 4; index++) {
    metadataBudget.method(`metadata${index}`, '()V', null,
      { flags: 0x109, attributes: Array.from({ length: 65535 }, () => opaque) });
  }
  failure(['methods', temporary(metadataBudget.bytes()), '--limit', '1'], 'jvm.resource-limit');
  // Ten 65,535-instruction methods print about 10 MB: the whole class exceeds the ceiling, a page does not.
  const output = new ClassBuilder('OutputBudget');
  for (let index = 0; index < 10; index++) {
    const code = Buffer.alloc(65535);
    code[65534] = 177;
    output.method(`m${index}`, '()V', code, { maxLocals: 0 });
  }
  const outputPath = temporary(output.bytes());
  failure(['disasm', outputPath, '--limit', '10000'], 'output.budget-exceeded');
  const onePage = text(['disasm', outputPath, '--limit', '1']);
  check(onePage.includes('public static void m0();') && onePage.endsWith('// more: continue with --offset 1\n'),
    'a page of a class that is too large in total still prints');

  let seed = 0x13579bdf;
  for (let index = 0; index < 64; index++) {
    seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
    const changed = Buffer.from(sampleBytes);
    const at = seed % changed.length;
    changed[at] ^= 1 << (index % 8);
    const input = temporary(changed);
    const destination = path.join(work, `fuzz-${index}.txt`);
    const result = invoke(['inspect', input, '--output', destination]);
    if (fs.existsSync(destination)) owned.add(destination);
    assert.equal(result.stdout, '');
    if (result.status === 0) {
      assert.ok(fs.readFileSync(destination, 'utf8').endsWith('}\n'));
    } else {
      assert.equal(result.status, 1);
      assert.ok(result.stderr.includes('moss:'), result.stderr);
      assert.equal(fs.existsSync(destination), false);
    }
    checks++;
  }

  if (javap) {
    // Optional JDK gate: every ASCII fixture must match this javap's own output, not just the stored goldens.
    for (const name of fs.readdirSync(fixtures).filter(n => n.endsWith('.class') && !['Preview.class', 'Module.class',
      'Unicode.class', 'WeirdNames.class', 'ConstantKinds.class'].includes(n))) {
      const result = spawnSync(javap, ['-c', '-p', '-l', file(name)],
        { cwd: root, encoding: 'utf8', timeout: 30000, maxBuffer: 16 * 1024 * 1024 });
      if (result.error) throw result.error;
      assert.equal(result.status, 0, result.stderr);
      same(text(['disasm', file(name)]), result.stdout.replace(/\r/g, ''), `${name} matches this javap`);
    }
    console.log('PASS: javap differential (complete -c -p -l output)');
  } else {
    console.log('JDK differential gate not executed: supply --javap <explicit path>.');
  }

  {
    const listing = text(['entries', jar]);
    same(listing, 'META-INF/MANIFEST.MF\npkg/\npkg/Example8.class\npkg/Example21.class\npkg/Unicode.class\n', 'entries listing');
    same(text(['entries', jar, '--offset', '2', '--limit', '1']),
      'pkg/Example8.class\n// more: continue with --offset 3\n', 'entries paging');
    // A class read from the archive must print identically to the standalone file.
    for (const [entry, name] of [['pkg/Example8.class', 'Example8.class'], ['pkg/Example21.class', 'Example21.class'],
      ['pkg/Unicode.class', 'Unicode.class']]) {
      same(text(['methods', jar, '--entry', entry]), text(['methods', file(name)]), `${entry} methods`);
      same(text(['disasm', jar, '--entry', entry]), text(['disasm', file(name)]), `${entry} disasm`);
    }
    same(text(['disasm', jar, '--class', 'pkg.Example21', '--method', 'sum', '--descriptor', '(II)I']),
      text(['disasm', file('Example21.class'), '--method', 'sum', '--descriptor', '(II)I']), 'single method from the archive');
    same(text(['methods', jar, '--class', 'pkg/Example21.class']), text(['methods', jar, '--class', 'pkg.Example21']),
      '--class spellings agree');
    failure(['methods', jar], 'input.archive-needs-entry');
    failure(['methods', jar, '--entry', 'pkg/Missing.class'], 'archive.entry-not-found');
    failure(['methods', jar, '--entry', 'pkg/example8.class'], 'archive.entry-not-found');
    failure(['methods', jar, '--entry', 'META-INF/MANIFEST.MF'], 'jvm.invalid-magic');
    failure(['methods', jar, '--class', 'pkg.Missing'], 'archive.entry-not-found');
    failure(['entries', file('Example8.class')], 'archive.invalid');
    failure(['entries', jar, '--entry', 'pkg/Example8.class'], 'cli.usage', 2);
    failure(['entries', jar, '--offset', '6'], 'query.invalid-offset');
    const corrupt = Buffer.from(fs.readFileSync(jar));
    corrupt.fill(0, corrupt.length - 22, corrupt.length - 18);
    failure(['entries', temporary(corrupt, '.jar')], 'archive.invalid');
    // Prefix filtering keeps archive order and pages over the selected names.
    same(text(['entries', jar, '--prefix', 'pkg/Example']), 'pkg/Example8.class\npkg/Example21.class\n', 'prefix filter');
    same(text(['entries', jar, '--prefix', 'pkg/Example', '--offset', '1', '--limit', '1']),
      'pkg/Example21.class\n', 'prefix filter paging');
    same(text(['entries', jar, '--prefix', 'zzz/']), '\n', 'empty prefix match');
    failure(['entries', jar, '--prefix', 'pkg/', '--offset', '5'], 'query.invalid-offset');

    // Whole-class output pages by method.
    const whole = text(['disasm', file('Example25.class')]);
    const firstPage = text(['disasm', file('Example25.class'), '--limit', '2']);
    check(firstPage.endsWith('// more: continue with --offset 2\n') && !firstPage.includes('public static int loop(int);'),
      'whole-class paging stops after the first methods');
    const secondPage = text(['disasm', file('Example25.class'), '--offset', '2', '--limit', '2']);
    check(secondPage.includes('public static int sum(int);') || secondPage.includes('public static int wide();'),
      'second page starts at method 2');
    check(whole.startsWith(firstPage.slice(0, firstPage.indexOf('  public static int sum(int, int);') + 1)),
      'pages are prefixes of the whole class');
    failure(['disasm', file('Example25.class'), '--offset', '999'], 'query.invalid-offset');
    console.log('PASS: JAR/ZIP entries and --entry equivalence');
  }
  {
    // Control-flow graph: exact shapes for synthetic methods.
    const branchBytes = Buffer.from([3, 153, 0, 8, 4, 87, 167, 0, 3, 177]);
    const branch = text(['cfg', oneMethod(branchBytes), '--method', 'test']);
    check(branch.includes('Cfg blocks 3 edges 3 unreachable 0 loops 0 irreducible-edges 0'), 'branch summary');
    check(branch.includes('Block 0 [0,4) instructions 0+2 reachable terminator=conditional'), 'branch block 0');
    check(branch.includes('Block 1 [4,9) instructions 2+3 reachable terminator=goto'), 'branch block 1');
    check(branch.includes('Block 2 [9,10) instructions 5+1 reachable terminator=return preds=[0,1]'), 'branch block 2');
    check(branch.includes('  -> 2 branch\n  -> 1 fallthrough\n') && branch.includes('  -> 2 jump\n'), 'branch edges');
    const handler = text(['cfg', oneMethod(Buffer.from([0, 0, 177, 87, 177]), '()V', { handlers: [[0, 2, 3, 0]] }), '--method', 'test']);
    check(handler.includes('Block 0 [0,2) instructions 0+2 reachable terminator=fallthrough') &&
      handler.includes('  -> 2 exception catch=any') && handler.includes('Block 2 [3,5)') && handler.includes('preds=[0]'),
      'exception edge to the handler');
    const dead = text(['cfg', oneMethod(Buffer.from([177, 177])), '--method', 'test']);
    check(dead.includes('unreachable 1') && dead.includes('UNREACHABLE'), 'unreachable block');
    const paged = text(['cfg', oneMethod(branchBytes), '--method', 'test', '--offset', '1', '--limit', '1']);
    check(paged.includes('Block 1 ') && !paged.includes('Block 0 ') && !paged.includes('Block 2 ') &&
      paged.endsWith('// more: continue with --offset 2\n'), 'block paging');
    check(invoke(['cfg', oneMethod(branchBytes), '--method', 'test']).stdout.includes('terminator=conditional'), 'cfg on stdout');
    failure(['cfg', file('Example25.class')], 'cli.usage', 2, false);
    failure(['cfg', file('Example25.class'), '--method', 'wide', '--offset', '99'], 'query.invalid-offset');
    failure(['cfg', file('Example25.class'), '--method-index', '99'], 'query.method-not-found');
    same(text(['cfg', jar, '--class', 'pkg.Example21', '--method', 'sum', '--descriptor', '(II)I']),
      text(['cfg', file('Example21.class'), '--method', 'sum', '--descriptor', '(II)I']), 'cfg via the archive');
  }
  {
    // Dominators and natural loops: exact shapes for synthetic methods.
    const loopBytes = Buffer.from([3, 153, 0, 7, 0, 167, 255, 251, 177]);
    const loopy = text(['cfg', oneMethod(loopBytes), '--method', 'test']);
    check(loopy.includes('loops 1 irreducible-edges 0'), 'loop summary');
    check(loopy.includes('Block 0 [0,4) instructions 0+2 reachable terminator=conditional preds=[1] idom=none loopDepth=1'),
      'loop header block');
    check(loopy.includes('Block 1 [4,8) instructions 2+2 reachable terminator=goto preds=[0] idom=0 loopDepth=1'), 'latch block');
    check(loopy.includes('Loop header=0 latches=[1] blocks=[0,1] depth=1'), 'natural loop');
    // Two entries into the B <-> C cycle make it irreducible: no natural loop is reported.
    const knot = text(['cfg', oneMethod(Buffer.from([3, 153, 0, 7, 3, 153, 0, 7, 3, 153, 255, 251, 177])), '--method', 'test']);
    check(knot.includes('irreducible-edges 1') && knot.includes('Irreducible edges [1->2]') && !knot.includes('Loop header='),
      'irreducible cycle');
    const straight = text(['cfg', oneMethod(Buffer.from([0, 0, 177])), '--method', 'test']);
    check(straight.includes('loops 0 irreducible-edges 0') && straight.includes('idom=none'), 'straight-line method');
    const unreachable = text(['cfg', oneMethod(Buffer.from([177, 177])), '--method', 'test']);
    check(unreachable.includes('Block 1 [1,2) instructions 1+1 UNREACHABLE') && unreachable.includes('idom=none'),
      'unreachable blocks have no dominator');
  }
  const timings = [];
  for (let index = 0; index < 5; index++) {
    const start = performance.now();
    const result = invoke(['disasm', file('Example25.class'), '--method', 'wide']);
    assert.equal(result.status, 0, result.stderr);
    timings.push(performance.now() - start);
  }
  timings.sort((a, b) => a - b);
  console.log(`Latency sample: ${sampleBytes.length} input bytes, 5 decoded instructions, median ${timings[2].toFixed(2)} ms (includes process launch).`);
  console.log(`PASS: ${checks} JVM/CLI integration checks, including ${matrix.length} opcode/encoding cases.`);
} finally {
  for (const location of owned) fs.unlinkSync(location);
  fs.rmdirSync(work);
}
