#include "PreExecuteAnalyzer.h"
#include "FunctionAnalyzer.h"

#include <clang/Tooling/Tooling.h>
#include <clang/Tooling/CommonOptionsParser.h>
#include <llvm/Support/CommandLine.h>
#include <iostream>
#include <nlohmann/json_fwd.hpp>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

enum AnalysisMode {
    Variables,
    Functions,
};

static llvm::cl::opt<AnalysisMode> Mode(
    "mode",
    llvm::cl::desc("Choose analysis mode:"),
    llvm::cl::values(
        clEnumValN(Variables, "vars", "Analyze variables"),
        clEnumValN(Functions, "funcs", "Analyze functions")
    ),
    llvm::cl::init(Variables)
);

static llvm::cl::OptionCategory MyToolCategory("My tool options");

int main(int argc, const char **argv) {
    auto OptionsParser = clang::tooling::CommonOptionsParser::create(argc, argv, MyToolCategory);
    if (!OptionsParser) {
        std::cerr << "Error parsing options" << std::endl;
        return 1;
    }

    clang::tooling::ClangTool Tool(OptionsParser->getCompilations(), OptionsParser->getSourcePathList());

    json result;

    if (Mode == Variables) {
        Data variables;
        std::vector<std::pair<std::string, std::string>> strings;
        bool canTest;
        Factory f(variables, strings, canTest);
        Tool.run(&f);

        result["strings"] = json::array();
        for (const auto& [type, filename] : strings) {
            result["strings"].push_back({
                {"type", type},
                {"filename", filename}
            });
        }
        result["can_test"] = canTest;
        result["variables"] = variables;
    } else {
        FunctionData functions;
        FunctionFactory f(functions);
        Tool.run(&f);
        result = {{"functions", functions}};
    }

    std::cout << result.dump(4) << std::endl;

    return 0;
}