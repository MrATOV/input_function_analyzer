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

    bool shouldVisitTemplateInstantiations() const { return false; }
    bool shouldVisitImplicitCode() const { return false; }

    bool shouldVisitDecl(Decl *D) {
        return SM.isWrittenInMainFile(D->getLocation());
    }

    bool TraverseCallExpr(CallExpr *CE) {
        if (!SM.isWrittenInMainFile(CE->getBeginLoc())) {
            return true;
        }
        return RecursiveASTVisitor<VariableVisitor>::TraverseCallExpr(CE);
    }

    bool VisitCallExpr(CallExpr *CE) {
        if (!SM.isWrittenInMainFile(CE->getBeginLoc())) {
            return true;
        }

        if (FunctionDecl *FD = CE->getDirectCallee()) {
            if (FD->getIdentifier() && FD->getName() == "scanf") {
                processScanfArguments(CE);
            }
        }
        return true;
    }

    bool TraverseCXXOperatorCallExpr(CXXOperatorCallExpr *OCE) {
        if (!SM.isWrittenInMainFile(OCE->getBeginLoc())) {
            return true;
        }
        return RecursiveASTVisitor<VariableVisitor>::TraverseCXXOperatorCallExpr(OCE);
    }

    bool VisitCXXOperatorCallExpr(CXXOperatorCallExpr *OCE) {
        if (!SM.isWrittenInMainFile(OCE->getBeginLoc())) {
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
        static llvm::SmallPtrSet<VarDecl*, 4> cinDecls;
        
        E = E->IgnoreParenCasts();
        if (DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E)) {
            if (VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
                if (cinDecls.count(VD)) return true;
                bool isCin = VD->getIdentifier() && 
                            VD->getName() == "cin" &&
                            VD->isInStdNamespace();
                if (isCin) cinDecls.insert(VD);
                return isCin;
            }
        } else if (CXXOperatorCallExpr *OCE = dyn_cast<CXXOperatorCallExpr>(E)) {
            if (OCE->getOperator() == OO_GreaterGreater) {
                return refersToCin(OCE->getArg(0));
            }
        }
        return false;
    }

    void addVariable(Expr *E, SourceLocation Loc) {
        static llvm::DenseMap<VarDecl*, std::pair<std::string, std::string>> cache;
        
        E = E->IgnoreParenCasts();
        if (DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E)) {
            if (VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
                if (!VD->getIdentifier() || VD->getName().empty()) return;
                
                auto it = cache.find(VD);
                if (it != cache.end()) {
                    PresumedLoc PLoc = SM.getPresumedLoc(Loc);
                    _data.push_back({it->second.first, it->second.second, 
                                    {PLoc.getLine(), PLoc.getColumn()}});
                    return;
                }
                
                QualType QT = VD->getType().getUnqualifiedType();
                PrintingPolicy PP(Context.getLangOpts());
                std::string TypeStr = QT.getAsString(PP);
                std::string Name = VD->getName().str();
                
                cache[VD] = {TypeStr, Name};
                
                PresumedLoc PLoc = SM.getPresumedLoc(Loc);
                _data.push_back({TypeStr, Name, {PLoc.getLine(), PLoc.getColumn()}});
            }
        }
    }
};
    
class TestVisitor : public RecursiveASTVisitor<TestVisitor> {
public:
    explicit TestVisitor(ASTContext &Context, SourceManager &SM, std::vector<std::string> &strings, bool& canTest)
        : Context(Context), SM(SM), strings(strings), _canTest(canTest) {}

    bool shouldVisitTemplateInstantiations() const { return false; }
    bool shouldVisitImplicitCode() const { return false; }

    bool shouldVisitDecl(Decl *D) {
        return SM.isWrittenInMainFile(D->getLocation());
    }

    bool TraverseFunctionDecl(FunctionDecl *FD) {
        bool wasInMain = currentFunctionIsMain;
        if (FD->getNameAsString() == "main") {
            currentFunctionIsMain = true;
        }
        
        bool result = RecursiveASTVisitor<TestVisitor>::TraverseFunctionDecl(FD);
        
        currentFunctionIsMain = wasInMain;
        return result;
    }

    bool VisitFunctionDecl(FunctionDecl *FD) {
        if (isInSystemHeader(FD->getLocation())) {
            return true;
        }
        
        if (FD->getNameInfo().getName().getAsString() == "main") {
            currentFunctionIsMain = true;
        } else {
            currentFunctionIsMain = false;
        }
        return true;
    }

    bool VisitVarDecl(VarDecl *VD) {
        if (isInSystemHeader(VD->getLocation())) {
            return true;
        }
        if (!currentFunctionIsMain) return true;

        std::string typeName = VD->getType().getAsString();

        if (typeName.find("TestOptions") != std::string::npos) requiredTestOptionsFound = true; 
        else if (typeName.find("FunctionManager") != std::string::npos) requiredFunctionManagerFound = true;
        else if (typeName.find("DataManager") != std::string::npos) requiredDataManagerFound = true;
        else if (typeName.find("TestFunctions") != std::string::npos) requiredTestFunctionsFound = true;
        return true;
    }

    bool VisitCXXMemberCallExpr(CXXMemberCallExpr *CE) {
        if (isInSystemHeader(CE->getExprLoc())) {
            return true;
        }
        if (!currentFunctionIsMain || !canTest()) return true;
        
        if (CE->getMethodDecl()->getNameAsString() == "run") {
            Expr *base = CE->getImplicitObjectArgument()->IgnoreParenImpCasts();
            if (DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(base)) {
                if (DRE->getType().getAsString().find("TestFunctions") != std::string::npos) {
                    _canTest = true;
                }
            }
        }
        return true;
    }

    bool VisitCXXConstructExpr(CXXConstructExpr *CE) {
        if (isInSystemHeader(CE->getExprLoc())) {
            return true;
        }
        if (!currentFunctionIsMain) return true;

        std::string className = CE->getConstructor()->getParent()->getNameAsString();
        if (className.find("DataImage") != std::string::npos || 
            className.find("DataArray") != std::string::npos || 
            className.find("DataMatrix") != std::string::npos || 
            className.find("DataText") != std::string::npos) {
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
    bool currentFunctionIsMain = false;
    bool requiredDataManagerFound = false;
    bool requiredFunctionManagerFound = false;
    bool requiredTestOptionsFound = false;
    bool requiredTestFunctionsFound = false;
    bool& _canTest;

    bool canTest() {
        if (requiredDataManagerFound 
            && requiredFunctionManagerFound 
            && requiredTestOptionsFound 
            && requiredTestFunctionsFound) {
            return true;
        }
        return false;
    }

    bool isInSystemHeader(SourceLocation loc) const {
        return loc.isInvalid() || SM.isInSystemHeader(loc) || SM.isInSystemMacro(loc);
    }

    std::string getStringValue(Expr *E) {
        E = E->IgnoreParenImpCasts();
        
        if (auto *SL = dyn_cast<StringLiteral>(E)) {
            return SL->getString().str();
        }
        
        if (auto *DRE = dyn_cast<DeclRefExpr>(E)) {
            if (VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
                return VD->hasInit() ? getStringValue(VD->getInit()) : "";
            }
        }
        
        if (auto *FCE = dyn_cast<CXXFunctionalCastExpr>(E)) {
            return getStringValue(FCE->getSubExpr());
        }

        if (auto *CBT = dyn_cast<CXXBindTemporaryExpr>(E)) {
            return getStringValue(CBT->getSubExpr());
        }
        
        if (auto *CE = dyn_cast<CXXConstructExpr>(E)) {
            if (CE->getConstructor()->getParent()->getNameAsString() == "basic_string" && 
                CE->getNumArgs() > 0) {
                return getStringValue(CE->getArg(0));
            }
        }
        
        if (auto *ILE = dyn_cast<InitListExpr>(E)) {
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