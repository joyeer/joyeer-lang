#ifndef JOYEER_UNITTESTS_MODULE_LOWERING_FIXTURE_H
#define JOYEER_UNITTESTS_MODULE_LOWERING_FIXTURE_H

#include "joyeer/compiler/irlowering.h"
#include "joyeer/compiler/lexparser.h"
#include "joyeer/compiler/nameresolution.h"
#include "joyeer/compiler/parser.h"
#include "joyeer/compiler/sourcefile.h"
#include "joyeer/compiler/typechecking.h"
#include "joyeer/diagnostic/diagnostic.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace joyeer::testing {

struct GraphSource {
    std::string module;
    std::string name;
    std::string text;
};

inline lowering::Result lowerGraph(const std::vector<GraphSource>& files) {
    std::vector<semantic::ModuleInput> modules;
    std::vector<ir::SourceInfo> sources;
    for (const auto& file : files) {
        Diagnostics diagnostics;
        auto source = std::make_shared<SourceFile>(file.text);
        LexParser(&diagnostics).parse(source);
        if (!diagnostics.errors.empty()) {
            std::ostringstream message;
            message << "graph fixture lexing failed: " << file.module << '/' << file.name;
            for (const auto& error : diagnostics.errors) {
                message << '\n' << error.code << " at " << error.lineAt << ':'
                        << error.columnAt << ": " << error.message;
            }
            throw std::runtime_error(message.str());
        }
        const auto sourceId = static_cast<uint32_t>(sources.size());
        for (const auto& token : source->tokens) token->span.sourceId = sourceId;
        const auto parsed = parser::Parser(source->tokens).parse();
        if (!parsed.succeeded()) {
            throw std::runtime_error(file.module + '/' + file.name + ":\n" +
                                     parser::dump(parsed.diagnostics));
        }
        auto module = std::find_if(modules.begin(), modules.end(), [&file](const auto& item) {
            return item.name == file.module;
        });
        if (module == modules.end()) {
            modules.push_back(semantic::ModuleInput { file.module, {} });
            module = modules.end() - 1;
        }
        module->files.push_back(parsed.root);
        sources.push_back(ir::SourceInfo {
            file.name, "C:/joyeer-tests/" + file.module,
            static_cast<uint64_t>(file.text.size()), source->lineStarts,
        });
    }
    const auto resolved = semantic::NameResolver().resolve(modules);
    if (!resolved.succeeded()) throw std::runtime_error(semantic::dump(resolved.diagnostics));
    const auto checked = typing::TypeChecker().check(resolved.model);
    if (!checked.succeeded()) throw std::runtime_error(typing::dump(checked.diagnostics));
    return lowering::Lowerer().lower(checked.model, "graph", std::nullopt, std::move(sources));
}

inline std::vector<GraphSource> collidingGraphSources() {
    return {
        { "app", "main.joyeer",
          "import dep\n"
          "private struct Box { var value: Int }\n"
          "private func helper(): Int { return Box(value: 11).value }\n"
          "func main() {\nlet local = helper()\nprint(value: local)\n"
          "print(value: dep.produce())\n}\n" },
        { "app", "other.joyeer",
          "private struct Box { var value: String }\n"
          "private func helper(): Int { return 22 }\n" },
        { "dep", "library.joyeer",
          "private struct Box { var value: Bool }\n"
          "private func helper(): Int { let box = Box(value: true)\nreturn 33 }\n"
          "public func produce(): Int {\nlet local = helper()\nreturn local\n}\n"
          "func main(): Int { return 44 }\n" },
    };
}

} // namespace joyeer::testing

#endif
