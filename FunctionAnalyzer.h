#ifndef FUNCTION_ANALYZER_H
#define FUNCTION_ANALYZER_H

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Tooling/Tooling.h>
#include <memory>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

using namespace clang;

struct Function {
    std::string returnType;
    std::string name;
    std::vector<std::pair<std::string, std::string>> parameters;
    std::pair<int, int> startPos;
    std::pair<int, int> endPos;
    std::string type;
    std::vector<std::pair<std::string, std::vector<std::string>>> enumValues;
    std::vector<std::pair<std::string, std::vector<std::string>>> argumentVariables;
};

using FunctionData = std::vector<Function>;

inline void to_json(json& j, const Function &f) {
    json parametersArray = json::array();
    for (const auto& param : f.parameters) {
        parametersArray.push_back({{"type", param.first}, {"title", param.second}});
    }
    json enumValues = json::array();
    for(const auto& value : f.enumValues) {
        enumValues.push_back({{"var", value.first}, {"enum", value.second}});
    }
    json argumentVars = json::array();
    for(const auto& var : f.argumentVariables) {
        argumentVars.push_back({{"var", var.first}, {"names", var.second}});
    }

    j = json{
        {"name", f.name},
        {"returnType", f.returnType},
        {"parameters", parametersArray},
        {"startPos", {f.startPos.first, f.startPos.second}},
        {"endPos", {f.endPos.first, f.endPos.second}},
        {"type", f.type},
        {"enumValues", enumValues},
        {"argumentVariables", argumentVars}
    };
}

class FunctionVisitor : public clang::RecursiveASTVisitor<FunctionVisitor> {
public:
    explicit FunctionVisitor(clang::ASTContext &Context, clang::SourceManager &SM, FunctionData &data) 
        : Context(Context), SM(SM), _data{data} {}

    bool VisitFunctionDecl(clang::FunctionDecl *FD) {
        if (Context.getSourceManager().isInSystemHeader(FD->getBeginLoc())) {
            return true;
        }
        if (FD->isThisDeclarationADefinition()) {
            clang::QualType QT = FD->getReturnType();
            clang::PrintingPolicy PP(Context.getLangOpts());
            std::string ReturnTypeStr = QT.getAsString(PP);

            std::string FunctionName = FD->getNameInfo().getName().getAsString();

            std::vector<std::pair<std::string, std::string>> Parameters;
            std::vector<std::pair<std::string, std::vector<std::string>>> EnumValues;
            std::vector<std::pair<std::string, std::vector<std::string>>> ArgumentVariables;

            for(unsigned i = 0; i < FD->getNumParams(); ++i) {
                clang::ParmVarDecl *Param = FD->getParamDecl(i);
                clang::QualType ParamType = Param->getType();
                std::string ParamTypeStr = ParamType.getAsString(PP);
                std::string ParamName = Param->getNameAsString();

                if (const clang::EnumType *EnumT = ParamType->getAs<clang::EnumType>()) {
                    ParamTypeStr = "enumeration " + ParamTypeStr;
                }

                Parameters.push_back({ParamTypeStr, ParamName});
            }

            std::string FunctionType = determineFunctionType(Parameters);

            if (FunctionType != "unknown") {
                int startParams = (FunctionType.find("array") != std::string::npos || FunctionType.find("text") != std::string::npos) ? 2 : 3;
                for(unsigned i = startParams; i < FD->getNumParams(); ++i) {
                    clang::ParmVarDecl *Param = FD->getParamDecl(i);
                    clang::QualType ParamType = Param->getType();

                    if (const clang::EnumType *EnumT = ParamType->getAs<clang::EnumType>()) {
                        clang::EnumDecl *EnumD = EnumT->getDecl();
                        std::vector<std::string> Values;
                        std::string EnumName = EnumD->getNameAsString();
                        for (auto EnumValue : EnumD->enumerators()) {
                            Values.push_back(EnumName + "::" + EnumValue->getNameAsString());
                        }
                        EnumValues.push_back({Param->getNameAsString(), Values});
                    }

                    std::vector<std::string> Variables;
                    for (auto Decl : Context.getTranslationUnitDecl()->decls()) {
                        if (auto VarDecl = llvm::dyn_cast<clang::VarDecl>(Decl)) {
                            if (VarDecl->isDefinedOutsideFunctionOrMethod() && 
                                VarDecl->getType().getAsString() == ParamType.getAsString()) {
                                Variables.push_back(VarDecl->getNameAsString());
                            }
                        }
                    }
                    ArgumentVariables.push_back({Param->getNameAsString(), Variables});
                }
            }

            clang::SourceLocation StartLoc = FD->getBeginLoc();
            clang::SourceLocation EndLoc = FD->getEndLoc();
            clang::PresumedLoc PStartLoc = SM.getPresumedLoc(StartLoc);
            clang::PresumedLoc PEndLoc = SM.getPresumedLoc(EndLoc);

            _data.push_back({
                ReturnTypeStr, 
                FunctionName, 
                Parameters, 
                {PStartLoc.getLine(), PStartLoc.getColumn()}, 
                {PEndLoc.getLine(), PEndLoc.getColumn()}, 
                FunctionType, 
                EnumValues, 
                ArgumentVariables
            });
        }
        return true;
    }

private:
    clang::ASTContext &Context;
    clang::SourceManager &SM;
    FunctionData &_data;

    std::string determineFunctionType(std::vector<std::pair<std::string, std::string>>& parameters) {
        std::vector<std::string> types;

        if (parameters.size() >= 3) {
            std::string firstParamType = parameters[0].first;
            std::string secondParamType = parameters[1].first;
            std::string thirdParamType = parameters[2].first;
            bool isSizeT1 = (secondParamType == "size_t" || secondParamType == "unsigned long");
            bool isSizeT2 = (thirdParamType == "size_t" || thirdParamType == "unsigned long");
            if (firstParamType.find("**") != std::string::npos && isSizeT1 && isSizeT2) {
                types.push_back("matrix");
                if (firstParamType.find("RGBImage") != std::string::npos) {
                    types.push_back("image");
                }
                parameters.erase(parameters.begin(), parameters.begin() + 3);
                goto end;
            }
        } 
        if (parameters.size() >= 2) {
            std::string firstParamType = parameters[0].first;
            std::string secondParamType = parameters[1].first;

            bool isSizeT = (secondParamType == "size_t" || secondParamType == "unsigned long");

            if (firstParamType.find('*') != std::string::npos && isSizeT) {
                types.push_back("array");                
                if (firstParamType.find("char") != std::string::npos) {
                    types.push_back("text");
                }
                parameters.erase(parameters.begin(), parameters.begin() + 2);
            }
        }
        end:
        if (types.empty()) {
            return "unknown";
        } else {
            std::string result;
            for (size_t i = 0; i < types.size(); ++i) {
                result += types[i];
                if (i != types.size() - 1) {
                    result += " ";
                }
            }
            return result;
        }
    }
};

class FunctionConsumer : public ASTConsumer {
public:
    explicit FunctionConsumer(ASTContext &Context, SourceManager &SM, FunctionData &data) : Visitor(Context, SM, data) {}
    void HandleTranslationUnit(ASTContext &Context) override {
        Visitor.TraverseDecl(Context.getTranslationUnitDecl());
    }

private:
    FunctionVisitor Visitor;
};

class FunctionAction : public ASTFrontendAction {
public:
    FunctionAction(FunctionData &data) : _data(data) {}
    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI, StringRef file) override {
        return std::make_unique<FunctionConsumer>(CI.getASTContext(), CI.getSourceManager(), _data);
    }
private:
    FunctionData &_data;
};

class FunctionFactory : public tooling::FrontendActionFactory {
public:
    FunctionFactory(FunctionData& data) : _data(data) {}
    
    std::unique_ptr<FrontendAction> create() override {
        return std::make_unique<FunctionAction>(_data);    
    }

private:
    FunctionData &_data;
};

#endif