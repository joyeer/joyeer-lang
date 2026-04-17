mod compiler;
mod runtime;
mod vm;
mod diagnostic;

use std::env;
use std::fs;
use std::process;

use compiler::lexer::Lexer;
use compiler::parser::Parser;
use compiler::context::{CompileContext, type_gen, type_bind};
use compiler::irgen::IRGen;
use vm::interpreter::VM;

fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() < 2 {
        eprintln!("Usage: joyeer <file.joyeer>");
        process::exit(1);
    }

    let filename = &args[1];
    let source = match fs::read_to_string(filename) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("Error reading file '{}': {}", filename, e);
            process::exit(1);
        }
    };

    // 1. Lex
    let tokens = match Lexer::new(&source).parse() {
        Ok(tokens) => tokens,
        Err(diag) => {
            diag.print_errors();
            process::exit(1);
        }
    };

    // 2. Parse
    let mut parser = Parser::new(tokens);
    let mut ast = match parser.parse() {
        Some(ast) => ast,
        None => {
            eprintln!("Parse error");
            process::exit(1);
        }
    };

    // Set filename on module
    if let compiler::node::Node::Module(ref mut m) = *ast {
        m.filename = filename.clone();
    }

    // 3. Type generation
    let mut ctx = CompileContext::new();
    type_gen(&mut ctx, &mut ast);

    // 4. Type binding
    type_bind(&mut ctx, &mut ast);

    // 5. IR Generation
    let module_slot = if let compiler::node::Node::Module(ref m) = *ast {
        let irgen = IRGen::new(&mut ctx);
        irgen.emit_module(m)
    } else {
        eprintln!("Expected module");
        process::exit(1);
    };

    // 6. Execute
    let mut vm = VM::new(ctx.types, ctx.strings);
    vm.run(module_slot);
}
