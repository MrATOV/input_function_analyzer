#ifndef VARIABLE_ANALYZER_H
#define VARIABLE_ANALYZER_H

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Tooling/Tooling.h>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

using namespace clang;

struct Variable {
    std::string type;
    std::string name;
    std::pair<int, int> pos;
};

using Data = std::vector<Variable>;

inline void to_json(json& j, const Variable& v) {
    j = json{
        {"name", v.name},
        {"type", v.type},
        {"pos", {v.pos.first, v.pos.second}}
    };
}


class VariableVisitor : public RecursiveASTVisitor<VariableVisitor> {
public:
    explicit VariableVisitor(ASTContext &Context, SourceManager &SM, Data &data)
        : Context(Context), SM(SM), _data(data) {}

    bool VisitCallExpr(CallExpr *CE) {
        if (FunctionDecl *FD = CE->getDirectCallee()) {
            if (FD->getIdentifier() && FD->getName() == "scanf") {
                processScanfArguments(CE);
            }
        }
        return true;
    }

    bool VisitCXXOperatorCallExpr(CXXOperatorCallExpr *OCE) {
        if (OCE->getOperator() == OO_GreaterGreater) {
            processCinOperator(OCE);
        }
        return true;
    }

private:
    ASTContext &Context;
    SourceManager &SM;
    Data &_data;

    void processScanfArguments(CallExpr *CE) {
        SourceLocation CallLoc = CE->getBeginLoc();
        for (unsigned i = 1; i < CE->getNumArgs(); ++i) {
            Expr *Arg = CE->getArg(i)->IgnoreParenCasts();
            
            if (UnaryOperator *UO = dyn_cast<UnaryOperator>(Arg)) {
                if (UO->getOpcode() == UO_AddrOf) {
                    Arg = UO->getSubExpr()->IgnoreParenCasts();
                }
            }

            addVariable(Arg, CallLoc);
        }
    }

    void processCinOperator(CXXOperatorCallExpr *OCE) {
        SourceLocation OpLoc = OCE->getBeginLoc();
        Expr *LHS = OCE->getArg(0);
        if (refersToCin(LHS)) {
            Expr *RHS = OCE->getArg(1);
            addVariable(RHS, OpLoc);
        }
    }

    bool refersToCin(Expr *E) {
        E = E->IgnoreParenCasts();
        if (DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E)) {
            if (VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
                return VD->getIdentifier() && 
                       VD->getName() == "cin" &&
                       VD->isInStdNamespace();
            }
        } else if (CXXOperatorCallExpr *OCE = dyn_cast<CXXOperatorCallExpr>(E)) {
            if (OCE->getOperator() == OO_GreaterGreater) {
                return refersToCin(OCE->getArg(0));
            }
        }
        return false;
    }

    void addVariable(Expr *E, SourceLocation Loc) {
        if (DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E->IgnoreParenCasts())) {
            if (VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
                if (!VD->getIdentifier() || VD->getName().empty()) return;

                QualType QT = VD->getType().getUnqualifiedType();
                PrintingPolicy PP(Context.getLangOpts());
                std::string TypeStr = QT.getAsString(PP);
                PresumedLoc PLoc = SM.getPresumedLoc(Loc);

                _data.push_back({TypeStr, VD->getName().str(), {PLoc.getLine(), PLoc.getColumn()}});
            }
        }
    }
};

class Consumer : public ASTConsumer {
public:
    explicit Consumer(ASTContext &Context, SourceManager &SM, Data &data) : Visitor(Context, SM, data) {}

    void HandleTranslationUnit(ASTContext &Context) override {
        Visitor.TraverseDecl(Context.getTranslationUnitDecl());
    }

private:
    VariableVisitor Visitor;
};

class Action : public ASTFrontendAction {
public:
    Action(Data &data) : _data(data) {}

    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI, StringRef file) override {
        return std::make_unique<Consumer>(CI.getASTContext(), CI.getSourceManager(), _data);
    }
private:
    Data &_data;
};

class Factory : public tooling::FrontendActionFactory {
public:
    Factory(Data& data) : _data(data) {}

    std::unique_ptr<FrontendAction> create() override {
        return std::make_unique<Action>(_data);
    }
private:
    Data &_data;
};

#endif // ANALYZER_H