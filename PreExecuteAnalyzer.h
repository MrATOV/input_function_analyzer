#ifndef VARIABLE_ANALYZER_H
#define VARIABLE_ANALYZER_H

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Tooling/Tooling.h>
#include <llvm-18/llvm/Support/raw_ostream.h>
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
            if (isInSystemHeader(FD->getLocation())) {
                return true;
            }
            if (FD->getIdentifier() && FD->getName() == "scanf") {
                processScanfArguments(CE);
            }
        }
        return true;
    }

    bool VisitCXXOperatorCallExpr(CXXOperatorCallExpr *OCE) {
        if (isInSystemHeader(OCE->getBeginLoc())) {
            return true;
        }
        if (OCE->getOperator() == OO_GreaterGreater) {
            processCinOperator(OCE);
        }
        return true;
    }

private:
    ASTContext &Context;
    SourceManager &SM;
    Data &_data;

    bool isInSystemHeader(SourceLocation loc) const {
        if (loc.isInvalid()) return false;
        
        if (SM.isInSystemHeader(loc) || SM.isInSystemMacro(loc)) {
            return true;
        }
        
        if (SM.isInMainFile(loc)) {
            return false;
        }
        
        FileID FID = SM.getFileID(loc);
        if (FID.isInvalid()) return false;
        
        SourceLocation IncludeLoc = SM.getIncludeLoc(FID);
        if (IncludeLoc.isInvalid()) return false;
        
        if (!SM.isWrittenInMainFile(IncludeLoc)) {
            return true;
        }
        
        return false;
    }

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

class TestVisitor : public RecursiveASTVisitor<TestVisitor> {
public:
    explicit TestVisitor(ASTContext &Context, SourceManager &SM, std::vector<std::string> &strings, bool& canTest)
        : Context(Context), SM(SM), strings(strings), _canTest(canTest) {}

    bool VisitFunctionDecl(FunctionDecl *FD) {
        if (isInSystemHeader(FD->getLocation())) {
            return true;
        }
        if (FD->getNameInfo().getName().getAsString() == "main") {
            hasMain = true;
        }
        return true;
    }

    bool VisitVarDecl(VarDecl *VD) {
        if (isInSystemHeader(VD->getLocation())) {
            return true;
        }
        if (!hasMain) return true;

        std::string typeName = VD->getType().getAsString();

        if (typeName.find("TestOptions") != std::string::npos  || typeName.find("FunctionManager") != std::string::npos || 
            typeName.find("DataManager") != std::string::npos || typeName.find("TestFunctions") != std::string::npos) {
            requiredObjectsFound++;
        }
        return true;
    }

    bool VisitCXXMemberCallExpr(CXXMemberCallExpr *CE) {
        if (isInSystemHeader(CE->getExprLoc())) {
            return true;
        }
        if (!hasMain || requiredObjectsFound < 4) return true;
        if (CE->getMethodDecl()->getNameAsString() == "run") {
            Expr *base = CE->getImplicitObjectArgument()->IgnoreParenImpCasts();
            if (DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(base)) {
                if (DRE->getType().getAsString().find("TestFunctions") != std::string::npos ) {
                    _canTest = true;
                    llvm::outs() << "Found all required objects and TestFunctions::run() call in main()\n";
                }
            }
        }
        return true;
    }

    bool VisitCXXConstructExpr(CXXConstructExpr *CE) {
        if (isInSystemHeader(CE->getExprLoc())) {
            return true;
        }
        std::string className = CE->getConstructor()->getParent()->getNameAsString();
        if (className.find("DataImage") != std::string::npos || className.find("DataArray") != std::string::npos || 
            className.find("DataMatrix") != std::string::npos || className.find("DataText") != std::string::npos) {
            if (CE->getNumArgs() > 0) {
                Expr *firstArg = CE->getArg(0)->IgnoreParenImpCasts();
                std::string strValue = getStringValue(firstArg);
                if (!strValue.empty()) {
                    strings.push_back(strValue);
                }
            }
        }
        return true;
    }

private:
    ASTContext &Context;
    SourceManager &SM;
    std::vector<std::string> &strings;
    bool hasMain = false;
    int requiredObjectsFound = 0;
    bool& _canTest;

    bool isInSystemHeader(SourceLocation loc) const {
        if (loc.isInvalid()) return false;
        
        if (SM.isInSystemHeader(loc) || SM.isInSystemMacro(loc)) {
            return true;
        }
        
        if (SM.isInMainFile(loc)) {
            return false;
        }
        
        FileID FID = SM.getFileID(loc);
        if (FID.isInvalid()) return false;
        
        SourceLocation IncludeLoc = SM.getIncludeLoc(FID);
        if (IncludeLoc.isInvalid()) return false;
        
        if (!SM.isWrittenInMainFile(IncludeLoc)) {
            return true;
        }
        
        return false;
    }

    std::string getStringValue(Expr *E) {
        if (auto *SL = dyn_cast<StringLiteral>(E->IgnoreParenImpCasts())) {
            return SL->getString().str();
        }
        
        E = E->IgnoreParenImpCasts();

        if (auto *FCE = dyn_cast<CXXFunctionalCastExpr>(E)){
            return getStringValue(FCE->getSubExpr());
        }

        if (auto *CBT = dyn_cast<CXXBindTemporaryExpr>(E)) {
            return getStringValue(CBT->getSubExpr());
        }
        if (auto *CE = dyn_cast<CXXConstructExpr>(E)){
                
            if (CE->getConstructor()->getParent()->getNameAsString() == "basic_string" && CE->getNumArgs() > 0) {
                return getStringValue(CE->getArg(0));
            }
        }
        if (auto *DRE = dyn_cast<DeclRefExpr>(E->IgnoreParenImpCasts())) {
            if (VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
                if (VD->hasInit()) {
                    return getStringValue(VD->getInit());
                }
            }
        }
        
        if (auto *ILE = dyn_cast<InitListExpr>(E->IgnoreParenImpCasts())) {
            std::string result;
            for (unsigned i = 0; i < ILE->getNumInits(); ++i) {
                if (auto charVal = dyn_cast<CharacterLiteral>(ILE->getInit(i))) {
                    result += charVal->getValue();
                }
            }
            if (!result.empty()) return result;
        }
        
        return "";
    }
};

class Consumer : public ASTConsumer {
public:
    Consumer(ASTContext &Context, SourceManager &SM, Data &data, std::vector<std::string> &strings, bool& canTest)
        : varVisitor(Context, SM, data), testVisitor(Context, SM, strings, canTest) {}

    void HandleTranslationUnit(ASTContext &Context) override {
        varVisitor.TraverseDecl(Context.getTranslationUnitDecl());
        testVisitor.TraverseDecl(Context.getTranslationUnitDecl());
    }

private:
    VariableVisitor varVisitor;
    TestVisitor testVisitor;
};

class Action : public ASTFrontendAction {
public:
    Action(Data &data, std::vector<std::string> &strings, bool& canTest) : _data(data), _strings(strings), _canTest(canTest) {}

    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI, StringRef file) override {
        return std::make_unique<Consumer>(CI.getASTContext(), CI.getSourceManager(), _data, _strings, _canTest);
    }
private:
    Data &_data;
    std::vector<std::string> &_strings;
    bool& _canTest;
};

class Factory : public tooling::FrontendActionFactory {
public:
    Factory(Data& data, std::vector<std::string>& strings, bool& canTest) : _data(data), _strings(strings), _canTest(canTest) {}

    std::unique_ptr<FrontendAction> create() override {
        return std::make_unique<Action>(_data, _strings, _canTest);
    }
private:
    Data &_data;
    std::vector<std::string> &_strings;
    bool& _canTest;
};

#endif // ANALYZER_H