#ifndef __joyeer_compiler_node_debugprint_h__
#define __joyeer_compiler_node_debugprint_h__

#include "joyeer/compiler/node+visitor.h"
#include "joyeer/runtime/bytecode.h"

#include <fstream>
#include <sstream>

#define DEBUG_BLOCK_START \
    incTab();             \
    newline();

#define DEBUG_BLOCK_END decTab();

struct YMLPrinter {

    explicit YMLPrinter(const std::string& filename);

    void printTab();
    void incTab();
    void decTab();

    void newline() {
        output << std::endl;
        printTab();
    }

    void close();

    int size = 0;
    std::ofstream output;
};

// ASM node printer
struct NodeDebugPrinter : public NodeVisitor, public YMLPrinter {

    explicit NodeDebugPrinter(const std::string& filename);

    // print AST into debug file
    void print(const std::string& stage, const Node::Ptr& node) {
        output << stage << ":";
        DEBUG_BLOCK_START
            NodeVisitor::visit(node);
        DEBUG_BLOCK_END
        size = 0;
        newline();
        output << "########################################################" << std::endl;
        output.flush();
    }

    void print(const std::string& stage, const std::vector<Type*>& types) {
        output << stage << ":";
        DEBUG_BLOCK_START
        print(types);
        DEBUG_BLOCK_END
    }

    /// escaping c string for debug display
    static std::string escapeString(const std::string &s) {
        std::ostringstream o;
        for (char c : s) {
            switch (c) {
                case '"': o << "\\\""; break;
                case '\\': o << "\\\\"; break;
                case '\b': o << "\\b"; break;
                case '\f': o << "\\f"; break;
                case '\n': o << "\\n"; break;
                case '\r': o << "\\r"; break;
                case '\t': o << "\\t"; break;
                default:
                    if ('\x00' <= c && c <= '\x1f') {
                        o << "\\u"
                          << std::hex << std::setw(4) << std::setfill('0') << (int)c;
                    } else {
                        o << c;
                    }
            }
        }
        return "\"" + o.str() + "\"";
    }

    void print(const SymbolTable::Ptr& symtable) {
        output << "symbol-table:";
        for(auto &symbol : symtable->symbols) {
            newline();
            output << "- symbol:";
            DEBUG_BLOCK_START
            output << "name: \"" << symbol.second->name << "\"";
            newline();
            output << "flag: " << debugStringOfSymbolFlag(symbol.second->flag);
            DEBUG_BLOCK_END
        }
    }

protected:

    Node::Ptr visit(const ModuleDecl::Ptr& decl) override {
        output << "module:";
        DEBUG_BLOCK_START
        output << "simple-name: " << escapeString(decl->getSimpleName());
        newline();
        print(decl->symtable);
        if(decl->statements.size() > 0) {
            newline();
            output << "members:";
            DEBUG_BLOCK_START
            auto i = 0;
            for(const auto& member : decl->statements) {
                if(i > 0) {
                    newline();
                }
                output << "- ";
                NodeVisitor::visit(member);
                i ++;
            }
            DEBUG_BLOCK_END
        }
        DEBUG_BLOCK_END
        
        return decl;
    }

    Node::Ptr visit(const ClassDecl::Ptr& decl) override;

    Node::Ptr visit(const VarDecl::Ptr& decl) override {
        output << "varDecl:";
        DEBUG_BLOCK_START
        output << "simple-name: " << escapeString(decl->getSimpleName());
        newline();
        visit(decl->pattern);
        if(decl->initializer != nullptr) {
            newline();
            NodeVisitor::visit(decl->initializer);
        }
        DEBUG_BLOCK_END
        return decl;
    }

    Node::Ptr visit(const OptionalType::Ptr& decl) override {
        output << "optionalType:";
        DEBUG_BLOCK_START
        output << "type:" ;
        DEBUG_BLOCK_START
            NodeVisitor::visit(decl->type);
        DEBUG_BLOCK_END
        newline();
        output << "required: " << (decl->required ? "true": "false");
        DEBUG_BLOCK_END
        return decl;
    }

    Node::Ptr visit(const TypeIdentifier::Ptr& decl) override {
        output << "kind:";
        DEBUG_BLOCK_START
            NodeVisitor::visit(decl->identifier);
        DEBUG_BLOCK_END
        return decl;
    }

    Node::Ptr visit(const FuncCallExpr::Ptr& decl) override {
        output << "funcCallExpr:";
        DEBUG_BLOCK_START
            output << "callee-func-simple-name: \"" << decl->getCalleeFuncSimpleName() << "\"";
            newline();
            output << "identifier:";
            DEBUG_BLOCK_START
                NodeVisitor::visit(decl->identifier);
            DEBUG_BLOCK_END
            if(decl->arguments.size() > 0) {
                newline();
                output << "arguments:";
                DEBUG_BLOCK_START
                auto i = 0;
                for(const auto& argument: decl->arguments) {
                    if(i++ > 0) {
                        newline();
                    }
                    output << "- ";
                    visit(argument);
                }
                DEBUG_BLOCK_END
            }
        DEBUG_BLOCK_END
        return decl;
    }

    Node::Ptr visit(const MemberFuncCallExpr::Ptr& decl) override {
        output << "memberFuncCallExpr:";
        DEBUG_BLOCK_START
            output << "callee:";
            DEBUG_BLOCK_START
            NodeVisitor::visit(decl->callee);
            DEBUG_BLOCK_END
            newline();
            output << "member:";
            DEBUG_BLOCK_START
            NodeVisitor::visit(decl->member);
            DEBUG_BLOCK_END
        DEBUG_BLOCK_END
        return decl;
    }

    Node::Ptr visit(const ArguCallExpr::Ptr& decl) override {
        output << "arguCallExpr:";
        DEBUG_BLOCK_START
            output << "label:";
            DEBUG_BLOCK_START
            visit(decl->label);
            DEBUG_BLOCK_END
            newline();
            output << "expr:";
            DEBUG_BLOCK_START
            NodeVisitor::visit(decl->expr);
            DEBUG_BLOCK_END
        DEBUG_BLOCK_END
        return decl;
    }

    Node::Ptr visit(const LiteralExpr::Ptr& decl) override {
        output << "literal: " << decl->literal->rawValue;
        return decl;
    }

    Node::Ptr visit(const PrefixExpr::Ptr& decl) override {
        output << "prefixExpr:";
        DEBUG_BLOCK_START
            output << "op:";
            DEBUG_BLOCK_START
                visit(decl->op);
            DEBUG_BLOCK_END
            newline();
            output << "expr:";
            DEBUG_BLOCK_START
                NodeVisitor::visit(decl->expr);
            DEBUG_BLOCK_END
        DEBUG_BLOCK_END
        return decl;
    }

    Node::Ptr visit(const PostfixExpr::Ptr& decl) override {
        output << "postfixExpr:";
        DEBUG_BLOCK_START
            output << "op:";
            DEBUG_BLOCK_START
                visit(decl->op);
            DEBUG_BLOCK_END
            newline();
            output << "expr:";
            DEBUG_BLOCK_START
                NodeVisitor::visit(decl->expr);
            DEBUG_BLOCK_END
        DEBUG_BLOCK_END
        return decl;
    }

    Node::Ptr visit(const IdentifierExpr::Ptr& decl) override {
        output << "identifierExpr:";
        DEBUG_BLOCK_START
        output << "simple-name: " << escapeString(decl->getSimpleName());
        DEBUG_BLOCK_END
        return decl;
    }

    Node::Ptr visit(const Expr::Ptr& decl) override {
        output << "expr:";
        DEBUG_BLOCK_START
            if(decl->prefix != nullptr) {
                output << "prefix:";
                DEBUG_BLOCK_START
                    NodeVisitor::visit(decl->prefix);
                DEBUG_BLOCK_END
            }

            newline();
            output << "binaries:";
            for(const auto& param: decl->binaries) {
                newline();
                output << "- ";
                NodeVisitor::visit(param);
            }
        DEBUG_BLOCK_END
        return decl;
    }

    Node::Ptr visit(const AssignExpr::Ptr& decl) override {
        output << "assignExpr:";
        DEBUG_BLOCK_START
        if(decl->expr != nullptr) {
            output << "expr:";
            DEBUG_BLOCK_START
            NodeVisitor::visit(decl->expr);
            DEBUG_BLOCK_END
        }
        if(decl->left != nullptr) {
            newline();
            output << "left:";
            DEBUG_BLOCK_START
            NodeVisitor::visit(decl->left);
            DEBUG_BLOCK_END
        }
        DEBUG_BLOCK_END
        return decl;
    }

    Node::Ptr visit(const BinaryExpr::Ptr& decl) override {
        output << "binaryExpr:";
        DEBUG_BLOCK_START
        output << "op:";
        DEBUG_BLOCK_START
            visit(decl->op);
        DEBUG_BLOCK_END
        newline();
        output << "expr:";
        DEBUG_BLOCK_START
        NodeVisitor::visit(decl->expr);
        DEBUG_BLOCK_END
        DEBUG_BLOCK_END
        return decl;
    }

    Node::Ptr visit(const OperatorExpr::Ptr& decl) override {
        output << "operatorExpr:";
        DEBUG_BLOCK_START
        output << "token: \"" << decl->token->rawValue << "\"";
        DEBUG_BLOCK_END
        return decl;
    }

    Node::Ptr visit(const ParenthesizedExpr::Ptr& decl) override {
        output << "parenthesizedExpr:";
        DEBUG_BLOCK_START
        output << "expr: ";
        DEBUG_BLOCK_START
            NodeVisitor::visit(decl->expr);
        DEBUG_BLOCK_END
        DEBUG_BLOCK_END
        return decl;
    }

    Node::Ptr visit(const IfStmt::Ptr& decl) override {
        output << "ifStmt:";
        DEBUG_BLOCK_START
        output << "condition:";
        DEBUG_BLOCK_START
            NodeVisitor::visit(decl->condition);
        DEBUG_BLOCK_END
        newline();
        output << "if-block:";
        DEBUG_BLOCK_START
            NodeVisitor::visit(decl->ifCodeBlock);
        DEBUG_BLOCK_END
        if(decl->elseCodeBlock) {
            newline();
            output << "else-block:";
            DEBUG_BLOCK_START
            NodeVisitor::visit(decl->elseCodeBlock);
            DEBUG_BLOCK_END
        }
        DEBUG_BLOCK_END
        return decl;
    }

    Node::Ptr visit(const WhileStmt::Ptr& decl) override {
        output << "whileStmt:";
        DEBUG_BLOCK_START
        output << "expr:";
        DEBUG_BLOCK_START
        NodeVisitor::visit(decl->expr);
        DEBUG_BLOCK_END
        newline();
        output << "codeBlock:";
        DEBUG_BLOCK_START
        NodeVisitor::visit(decl->codeBlock);
        DEBUG_BLOCK_END
        DEBUG_BLOCK_END
        return decl;
    }

    Node::Ptr visit(const StmtsBlock::Ptr& decl) override {
        output << "statements:";
        incTab();
        for(const auto& statement: decl->statements) {
            newline();
            output << "- ";
            NodeVisitor::visit(statement);
        }
        decTab();
        return decl;
    }

    Node::Ptr visit(const FuncDecl::Ptr& decl) override {
        output << "funcDecl:";
        incTab();
        newline();
        output << "simple-name: " << escapeString(decl->getSimpleName());
        if(decl->parameterClause != nullptr) {
            newline();
            NodeVisitor::visit(decl->parameterClause);
        }

        if(decl->identifier != nullptr) {
            newline();
            NodeVisitor::visit(decl->identifier);
        }

        if(decl->returnType != nullptr) {
            newline();
            output << "return-kind:";
            incTab();
            newline();
            NodeVisitor::visit(decl->returnType);
            decTab();
        }

        newline();

        for(const auto& statement: decl->statements) {
            NodeVisitor::visit(statement);
            newline();
        }

        decTab();
        return decl;
    }

    Node::Ptr visit(const ParameterClause::Ptr& decl) override {
        output << "parameters:";
        DEBUG_BLOCK_START
        auto i = 0;
        for(const auto& param: decl->parameters) {
            if(i > 0) {
                newline();
            }

            output << "- ";
            visit(param);

            i++;
        }
        DEBUG_BLOCK_END
        return decl;
    }

    Node::Ptr visit(const Pattern::Ptr& decl) override {
        output << "patten:";
        DEBUG_BLOCK_START
        output << "label:";
        DEBUG_BLOCK_START
            visit(decl->identifier);
        DEBUG_BLOCK_END
        if(decl->typeExpr != nullptr) {
            newline();
            output << "expr:";
            DEBUG_BLOCK_START
            NodeVisitor::visit(decl->typeExpr);
            DEBUG_BLOCK_END
        }
        DEBUG_BLOCK_END
        return  decl;
    }

    Node::Ptr visit(const ReturnStmt::Ptr& decl) override {
        output << "return:" ;
        return decl;
    }

    Node::Ptr visit(const Self::Ptr& decl) override {
        output << "self";
        return decl;
    }

    Node::Ptr visit(const SelfExpr::Ptr& decl) override {
        output << "self-expr:";
        DEBUG_BLOCK_START
            visit(decl->identifier);
        DEBUG_BLOCK_END
        return decl;
    }

    Node::Ptr visit(const ArrayLiteralExpr::Ptr& decl) override {
        output << "arrayLiteralExpr:";
        DEBUG_BLOCK_START
        auto i = 0;
        for(const auto& expr : decl->items) {
            output << "- " << i ++ << ": ";
            DEBUG_BLOCK_START
                NodeVisitor::visit(expr);
            DEBUG_BLOCK_END
            if(i < decl->items.size()) {
                newline();
            }
        }

        DEBUG_BLOCK_END
        return decl;
    }

    Node::Ptr visit(const DictLiteralExpr::Ptr& decl) override {
        output << "dictLiteralExpr:";
        DEBUG_BLOCK_START
        output << "items:";
        for(const auto& item : decl->items) {
            newline();
            output << "-";
            DEBUG_BLOCK_START
                output << "key:";
                    DEBUG_BLOCK_START
                    NodeVisitor::visit(std::get<0>(item));
                    DEBUG_BLOCK_END
                newline();
                output << "value:";
                    DEBUG_BLOCK_START
                    NodeVisitor::visit(std::get<1>(item));
                    DEBUG_BLOCK_END
            DEBUG_BLOCK_END
        }
        DEBUG_BLOCK_END
        return decl;
    }

    Node::Ptr visit(const MemberAccessExpr::Ptr& decl) override {
        output << "memberAccessExpr:";
        DEBUG_BLOCK_START
        output << "callee:";
        DEBUG_BLOCK_START
        NodeVisitor::visit(decl->callee);
        DEBUG_BLOCK_END
        newline();
        output << "member:";
        DEBUG_BLOCK_START
        NodeVisitor::visit(decl->member);
        DEBUG_BLOCK_END
        DEBUG_BLOCK_END
        return decl;
    }

    Node::Ptr visit(const MemberAssignExpr::Ptr& decl) override {
        output << "memberAssignExpr:";
        DEBUG_BLOCK_START
            output << "member:";
            DEBUG_BLOCK_START
            visit(decl->member);
            DEBUG_BLOCK_END
            newline();
            output << "expr:";
            DEBUG_BLOCK_START
            NodeVisitor::visit(decl->expr);
            DEBUG_BLOCK_END
        DEBUG_BLOCK_END
        return decl;
    }

    Node::Ptr visit(const SubscriptExpr::Ptr& decl) override {
        output << "subscriptExpr:";
        DEBUG_BLOCK_START
        output << "identifier:";
        DEBUG_BLOCK_START
        NodeVisitor::visit(decl->identifier);
        DEBUG_BLOCK_END
        newline();
        output << "indexExpr:";
        DEBUG_BLOCK_START
        NodeVisitor::visit(decl->indexExpr);
        DEBUG_BLOCK_END
        DEBUG_BLOCK_END
        return decl;
    }

    Node::Ptr visit(const OptionalChainingExpr::Ptr& decl) override {
        output << "optional-chaining:";
        DEBUG_BLOCK_START
        output << "wrappedExpr:";
        DEBUG_BLOCK_START
            NodeVisitor::visit(decl->wrappedExpr);
        DEBUG_BLOCK_END
        DEBUG_BLOCK_END
        return decl;
    }

    Node::Ptr visit(const ForceUnwrappingExpr::Ptr& decl) override {
        output << "force-unwrapping:";
        DEBUG_BLOCK_START
        output << "wrappedExpr:";
        DEBUG_BLOCK_START
        NodeVisitor::visit(decl->wrappedExpr);
        DEBUG_BLOCK_END
        DEBUG_BLOCK_END
        return decl;
    }

    Node::Ptr visit(const ArrayType::Ptr& decl) override {
        output << "array:";
        DEBUG_BLOCK_START
        output << "type:";
        DEBUG_BLOCK_START
        NodeVisitor::visit(decl->valueType);
        DEBUG_BLOCK_END
        DEBUG_BLOCK_END
        return decl;
    }

    Node::Ptr visit(const ImportStmt::Ptr& decl) override {
        assert(false);
        return decl;
    }

    void print(const std::vector<Type*>& types) {
        output << "types:";

        auto i = 0;
        for(const auto& tf : types) {
            newline();
            output << "-";
            DEBUG_BLOCK_START
            output << "slot: " << i;
            newline();
            switch (tf->kind) {
                case ValueType::Module:
                    output << "type: Module" ;
                    newline();
                    print((ModuleClass*)(tf));
                    break;
                case ValueType::Nil:
                    output << "type: Nil" ;
                    break;
                case ValueType::Bool:
                    output << "kind: Boolean" ;
                    break;
                case ValueType::Int:
                    output << "type: Integer";
                    break;
                case ValueType::Void:
                    output << "type: Void";
                    break;
                case ValueType::Any:
                    output << "type: Any";
                    break;
                case ValueType::Class:
                    output << "type: Class";
                    break;
                case ValueType::Func:
                    output << "type: Func";
                    newline();
                    print((Function*)(tf), types);
                    break;
                case ValueType::Unspecified:
                    output << "type: Unspecified";
                    break;
                case ValueType::String:
                    output << "type: String";
                    break;
                case ValueType::Optional:
                    output << "type: optional";
                    break;
                default:
                    assert(false);
            }

            DEBUG_BLOCK_END
            i ++ ;
        }
    }

    void print(Function* func, const std::vector<Type*>& types);

    void print(ModuleClass* module) {
        output << "name: " << module->name;
        newline();
        output << "staticInitializer: " << module->staticInitializerSlot;
    }

    // debug output the instruction
    std::string debugPrint(Bytecode* bytecode, const std::vector<Type*>& types);

};

#endif
