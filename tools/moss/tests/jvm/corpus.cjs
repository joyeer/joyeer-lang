'use strict';
// Differential corpus check: Moss `disasm` output must equal `javap -c -p -l` byte for byte
// for real javac-compiled classes. With --cfg it also checks every method's control-flow graph
// against structural invariants and an independent dominator/loop oracle. Development gate only;
// needs an explicit JDK javap.
//
//   node corpus.cjs <moss> <javap> <class-root> [--limit N] [--stride K] [--cfg]
//
// <class-root> is a directory tree of .class files (for example `jmod extract --dir`
// output). Files are visited in sorted order, taking every K-th file up to N classes.
// Classes whose names or constants contain non-ASCII text are skipped: javap prints those as
// `?`, while Moss keeps them lossless as \uXXXX.

const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { spawnSync } = require('node:child_process');

const args = process.argv.slice(2);
if (args.length < 3) throw new Error('usage: node corpus.cjs <moss> <javap> <class-root> [--limit N] [--stride K] [--cfg]');
const [moss, javap, rootDir] = args.slice(0, 3).map(p => path.resolve(p));
let limit = 300;
let stride = 1;
let cfgMode = false;
for (let i = 3; i < args.length; i += (args[i] === '--cfg' ? 1 : 2)) {
  if (args[i] === '--cfg') cfgMode = true;
  else if (args[i] === '--limit') limit = Number(args[i + 1]);
  else if (args[i] === '--stride') stride = Number(args[i + 1]);
  else throw new Error(`unknown option ${args[i]}`);
}

function flow(mnemonic) {
  if (mnemonic === 'goto' || mnemonic === 'goto_w') return 'goto';
  if (mnemonic === 'tableswitch' || mnemonic === 'lookupswitch') return 'switch';
  if (mnemonic === 'athrow') return 'throw';
  if (/^[ildfa]?return$/.test(mnemonic)) return 'return';
  if (/^if/.test(mnemonic)) return 'conditional';
  return 'fallthrough';
}

// Methods of a Moss/javap listing: declaration order index, instructions with branch targets, handlers.
function parseListing(listing) {
  const methods = [];
  let index = -1;
  let current = null;
  let inSwitch = null;
  let inTable = false;
  for (const line of listing.split('\n')) {
    if (/^  \S/.test(line) && (line.includes('(') || /^  static \{\};/.test(line)) && line.trimEnd().endsWith(';')) {
      index++;
      current = { index, code: false, instructions: [], handlers: [] };
      methods.push(current);
      inSwitch = null;
      inTable = false;
      continue;
    }
    if (!current) continue;
    if (/^    Code:\s*$/.test(line)) { current.code = true; continue; }
    if (/^      Exception table:/.test(line)) { inTable = true; continue; }
    if (/^      (LineNumberTable|LocalVariableTable):/.test(line)) { inTable = false; continue; }
    if (inTable) {
      const row = /^\s+(\d+)\s+(\d+)\s+(\d+)\s+(any|Class .*)$/.exec(line);
      if (row) current.handlers.push({ start: Number(row[1]), end: Number(row[2]), handler: Number(row[3]) });
      continue;
    }
    const instruction = /^\s+(\d+): ([a-z][a-z0-9_]*)\b\s*(.*)$/.exec(line);
    if (instruction) {
      let mnemonic = instruction[2];
      if (/^(?:[ilfda](?:load|store)|iinc|ret)_w$/.test(mnemonic)) mnemonic = mnemonic.slice(0, -2);
      const item = { offset: Number(instruction[1]), mnemonic, targets: [] };
      if (/^(?:if|goto|jsr)/.test(mnemonic)) item.targets.push(Number(/^(\d+)/.exec(instruction[3])[1]));
      current.instructions.push(item);
      inSwitch = /switch$/.test(mnemonic) ? item : null;
      continue;
    }
    if (inSwitch) {
      const arm = /^\s+(?:-?\d+|default): (\d+)$/.exec(line);
      if (arm) inSwitch.targets.push(Number(arm[1]));
      else if (/^\s+\}$/.test(line)) inSwitch = null;
    }
  }
  return methods;
}

function parseGraph(text) {
  const graph = { blocks: [], loops: [], irreducibleEdges: 0 };
  const list = value => value.length === 0 ? [] : value.split(',').map(Number);
  for (const line of text.split('\n')) {
    let m;
    if ((m = /^Cfg blocks (\d+) edges (\d+) unreachable (\d+) loops (\d+) irreducible-edges (\d+)$/.exec(line))) {
      graph.irreducibleEdges = Number(m[5]);
    } else if ((m = /^Block (\d+) \[(\d+),(\d+)\) instructions (\d+)\+(\d+) (reachable|UNREACHABLE) terminator=(\S+) preds=\[([\d,]*)\] idom=(\S+) loopDepth=(\d+)$/.exec(line))) {
      graph.blocks.push({
        index: Number(m[1]), start: Number(m[2]), end: Number(m[3]), first: Number(m[4]), count: Number(m[5]),
        reachable: m[6] === 'reachable', terminator: m[7], predecessors: list(m[8]),
        idom: m[9] === 'none' ? null : Number(m[9]), loopDepth: Number(m[10]), successors: [],
      });
    } else if ((m = /^  -> (\d+) (\S+)/.exec(line))) {
      graph.blocks.at(-1).successors.push({ to: Number(m[1]), kind: m[2] });
    } else if ((m = /^Loop header=(\d+) latches=\[([\d,]*)\] blocks=\[([\d,]*)\] depth=(\d+)$/.exec(line))) {
      graph.loops.push({ header: Number(m[1]), latches: list(m[2]), blocks: list(m[3]), depth: Number(m[4]) });
    }
  }
  return graph;
}

function cfgInvariants(label, graph, method) {
  const problems = [];
  const instructions = method.instructions;
  const blocks = graph.blocks;
  const starts = new Set(blocks.map(b => b.start));
  if (blocks.length === 0) return [`${label}: no blocks`];
  if (blocks[0].start !== 0) problems.push('entry block does not start at 0');
  let expectedInstruction = 0;
  blocks.forEach((b, i) => {
    if (i > 0 && b.start !== blocks[i - 1].end) problems.push(`block ${i} is not contiguous`);
    if (b.first !== expectedInstruction) problems.push(`block ${i} first instruction`);
    expectedInstruction += b.count;
    const last = instructions[b.first + b.count - 1];
    if (!last || flow(last.mnemonic) !== b.terminator) problems.push(`block ${i} terminator ${b.terminator} vs ${last && last.mnemonic}`);
    const normal = b.successors.filter(e => e.kind !== 'exception');
    const expected = { goto: 1, return: 0, throw: 0, fallthrough: 1, conditional: 2 }[b.terminator];
    if (expected !== undefined && normal.length !== expected) problems.push(`block ${i} has ${normal.length} normal successors`);
    if (b.terminator === 'switch' && !normal.some(e => e.kind === 'default')) problems.push(`block ${i} switch lacks default`);
    const covering = method.handlers.filter(r => b.start >= r.start && b.start < r.end).length;
    if (b.successors.filter(e => e.kind === 'exception').length !== covering) problems.push(`block ${i} exception edges`);
    for (const e of b.successors) {
      if (e.to < 0 || e.to >= blocks.length) problems.push(`block ${i} edge target ${e.to}`);
      else if (!blocks[e.to].predecessors.includes(i)) problems.push(`block ${e.to} lacks predecessor ${i}`);
    }
  });
  if (expectedInstruction !== instructions.length) problems.push('instruction count mismatch');
  for (const instruction of instructions) {
    for (const target of instruction.targets) {
      if (!starts.has(target)) problems.push(`target ${target} is not a block start`);
    }
  }
  for (const r of method.handlers) {
    if (!starts.has(r.handler)) problems.push(`handler ${r.handler} is not a block start`);
  }
  return problems.map(p => `${label}: ${p}`);
}

// Independent oracle: recompute dominator sets, immediate dominators, back edges and natural
// loops with the textbook set-based algorithm and compare them with Moss.
function dominanceOracle(label, graph) {
  const problems = [];
  const blocks = graph.blocks;
  const n = blocks.length;
  const successors = blocks.map(b => b.successors.map(e => e.to));
  const reached = new Array(n).fill(false);
  const stack = [0];
  reached[0] = true;
  while (stack.length) {
    const b = stack.pop();
    for (const t of successors[b]) if (!reached[t]) { reached[t] = true; stack.push(t); }
  }
  const preds = blocks.map((_, i) => blocks[i].predecessors.filter(p => reached[p]));
  const all = new Set(blocks.map((_, i) => i).filter(i => reached[i]));
  const dom = blocks.map((_, i) => i === 0 ? new Set([0]) : new Set(all));
  let changed = true;
  while (changed) {
    changed = false;
    for (let b = 1; b < n; b++) {
      if (!reached[b]) continue;
      let meet = null;
      for (const p of preds[b]) {
        meet = meet === null ? new Set(dom[p]) : new Set([...meet].filter(x => dom[p].has(x)));
      }
      meet = meet || new Set();
      meet.add(b);
      if (meet.size !== dom[b].size) { dom[b] = meet; changed = true; }
    }
  }
  for (let b = 0; b < n; b++) {
    let expected = null;
    if (reached[b] && b !== 0) {
      const strict = [...dom[b]].filter(x => x !== b);
      expected = strict.reduce((best, d) => best === null || dom[d].size > dom[best].size ? d : best, null);
    }
    if (blocks[b].idom !== expected) problems.push(`block ${b} idom ${blocks[b].idom} vs ${expected}`);
  }
  // Back edges and natural loops (one loop per header, merged latches).
  const loops = new Map();
  for (let u = 0; u < n; u++) {
    if (!reached[u]) continue;
    for (const h of new Set(successors[u])) {
      if (!dom[u].has(h)) continue;
      if (!loops.has(h)) loops.set(h, { latches: new Set(), body: new Set([h]) });
      loops.get(h).latches.add(u);
    }
  }
  for (const [h, loop] of loops) {
    const work = [...loop.latches].filter(l => !loop.body.has(l));
    work.forEach(l => loop.body.add(l));
    while (work.length) {
      const b = work.pop();
      for (const p of preds[b]) if (!loop.body.has(p)) { loop.body.add(p); work.push(p); }
    }
  }
  const mine = new Map(graph.loops.map(l => [l.header, l]));
  if (mine.size !== loops.size) problems.push(`loop count ${mine.size} vs ${loops.size}`);
  for (const [h, loop] of loops) {
    const ours = mine.get(h);
    if (!ours) { problems.push(`missing loop ${h}`); continue; }
    if (JSON.stringify([...loop.latches].sort((a, b) => a - b)) !== JSON.stringify([...ours.latches].sort((a, b) => a - b))) problems.push(`loop ${h} latches`);
    if (JSON.stringify([...loop.body].sort((a, b) => a - b)) !== JSON.stringify(ours.blocks)) problems.push(`loop ${h} body`);
  }
  for (let b = 0; b < n; b++) {
    const depth = [...loops.values()].filter(l => l.body.has(b)).length;
    if (blocks[b].loopDepth !== depth) problems.push(`block ${b} loopDepth ${blocks[b].loopDepth} vs ${depth}`);
  }
  // Irreducible iff some depth-first retreating edge is not a back edge (same successor order).
  const state = new Array(n).fill(0);
  let retreating = 0;
  const visit = [[0, 0]];
  state[0] = 1;
  while (visit.length) {
    const top = visit.at(-1);
    const [b, i] = top;
    if (i < successors[b].length) {
      top[1]++;
      const t = successors[b][i];
      if (state[t] === 0) { state[t] = 1; visit.push([t, 0]); }
      else if (state[t] === 1 && !dom[b].has(t)) retreating++;
    } else { state[b] = 2; visit.pop(); }
  }
  if ((graph.irreducibleEdges > 0) !== (retreating > 0)) problems.push(`irreducible ${graph.irreducibleEdges > 0} vs ${retreating > 0}`);
  return problems.map(p => `${label}: ${p}`);
}

function walk(directory, found = []) {
  for (const entry of fs.readdirSync(directory, { withFileTypes: true }).sort((a, b) => a.name < b.name ? -1 : 1)) {
    const full = path.join(directory, entry.name);
    if (entry.isDirectory()) walk(full, found);
    else if (entry.name.endsWith('.class')) found.push(full);
  }
  return found;
}

const work = fs.mkdtempSync(path.join(require('node:os').tmpdir(), 'moss-corpus-'));
const files = walk(rootDir).filter((_, index) => index % stride === 0).slice(0, limit);
let classes = 0;
let identical = 0;
let cfgMethods = 0;
let methodCount = 0;
let instructionCount = 0;
const failures = [];
const skipped = [];
const nonAscii = [];

try {
  for (const file of files) {
    const output = path.join(work, `${crypto.randomUUID()}.txt`);
    const run = spawnSync(moss, ['disasm', file, '--limit', '10000', '--output', output],
      { encoding: 'utf8', maxBuffer: 64 * 1024 * 1024 });
    const relative = path.relative(rootDir, file);
    if (run.status !== 0 && run.stderr.includes('output.budget-exceeded')) { skipped.push(relative); continue; }
    if (run.status !== 0) { failures.push(`${relative}: moss exit ${run.status}: ${run.stderr.trim()}`); continue; }
    const mine = fs.readFileSync(output, 'utf8');
    fs.unlinkSync(output);
    if (/\\u(?!00[0-7][0-9a-f])[0-9a-f]{4}/.test(mine)) { nonAscii.push(relative); continue; }
    const reference = spawnSync(javap, ['-c', '-p', '-l', file], { encoding: 'utf8', maxBuffer: 256 * 1024 * 1024 });
    if (reference.status !== 0) throw new Error(`javap failed for ${file}: ${reference.stderr}`);
    const theirs = reference.stdout.replace(/\r/g, '');
    classes++;
    const listing = parseListing(mine);
    for (const method of listing) {
      if (!method.code) continue;
      methodCount++;
      instructionCount += method.instructions.length;
    }
    if (mine === theirs) identical++;
    else {
      const a = mine.split('\n');
      const b = theirs.split('\n');
      let line = 0;
      while (line < a.length && line < b.length && a[line] === b[line]) line++;
      failures.push(`${relative}: differs from javap at line ${line + 1}:\n      moss : ${a[line]}\n      javap: ${b[line]}`);
      continue;
    }
    if (cfgMode) {
      for (const method of listing.filter(m => m.code)) {
        const graphFile = path.join(work, `${crypto.randomUUID()}.txt`);
        const graphRun = spawnSync(moss, ['cfg', file, '--method-index', String(method.index), '--limit', '10000',
          '--output', graphFile], { encoding: 'utf8', maxBuffer: 64 * 1024 * 1024 });
        const label = `${relative} #${method.index}`;
        if (graphRun.status !== 0) { failures.push(`${label}: cfg exit ${graphRun.status}: ${graphRun.stderr.trim()}`); continue; }
        const graph = parseGraph(fs.readFileSync(graphFile, 'utf8'));
        fs.unlinkSync(graphFile);
        cfgMethods++;
        failures.push(...cfgInvariants(label, graph, method));
        failures.push(...dominanceOracle(label, graph));
      }
    }
  }
} finally {
  fs.rmSync(work, { recursive: true, force: true });
}

if (skipped.length) console.log(`corpus: ${skipped.length} classes skipped (whole-class output over the 8 MiB budget): ${skipped.slice(0, 5).join(', ')}`);
if (nonAscii.length) console.log(`corpus: ${nonAscii.length} classes skipped (non-ASCII names or constants)`);
if (cfgMode) console.log(`corpus: ${cfgMethods} control-flow graphs checked`);
console.log(`corpus: ${classes} classes (${identical} identical to javap), ${methodCount} code methods, ${instructionCount} instructions`);
if (failures.length) {
  console.log(`corpus: ${failures.length} FAILURES`);
  for (const f of failures.slice(0, 25)) console.log(`  ${f}`);
  process.exit(1);
}
console.log('corpus: PASS');
