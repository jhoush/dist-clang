//===--- FrontendActions.cpp ----------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "clang/Frontend/FrontendActions.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Parse/Parser.h"
#include "clang/Basic/FileManager.h"
#include "clang/Frontend/AnalysisConsumer.h"
#include "clang/Frontend/ASTConsumers.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FixItRewriter.h"
#include "clang/Frontend/FrontendDiagnostic.h"
#include "clang/Frontend/Utils.h"
#include "clang/Frontend/Distcc.h"
#include "clang/Frontend/DistccClientServer.h"

#include "llvm/Support/raw_ostream.h"


using namespace clang;

//===----------------------------------------------------------------------===//
// Custom Actions
//===----------------------------------------------------------------------===//

ASTConsumer *InitOnlyAction::CreateASTConsumer(CompilerInstance &CI,
                                               llvm::StringRef InFile) {
  return new ASTConsumer();
}

void InitOnlyAction::ExecuteAction() {
}

//===----------------------------------------------------------------------===//
// AST Consumer Actions
//===----------------------------------------------------------------------===//

ASTConsumer *AnalysisAction::CreateASTConsumer(CompilerInstance &CI,
                                               llvm::StringRef InFile) {
  return CreateAnalysisConsumer(CI.getPreprocessor(),
                                CI.getFrontendOpts().OutputFile,
                                CI.getAnalyzerOpts());
}

ASTConsumer *ASTPrintAction::CreateASTConsumer(CompilerInstance &CI,
                                               llvm::StringRef InFile) {
  if (llvm::raw_ostream *OS = CI.createDefaultOutputFile(false, InFile))
    return CreateASTPrinter(OS);
  return 0;
}

ASTConsumer *ASTPrintXMLAction::CreateASTConsumer(CompilerInstance &CI,
                                                  llvm::StringRef InFile) {
  if (llvm::raw_ostream *OS = CI.createDefaultOutputFile(false, InFile, "xml"))
    return CreateASTPrinterXML(OS);
  return 0;
}

ASTConsumer *ASTDumpAction::CreateASTConsumer(CompilerInstance &CI,
                                              llvm::StringRef InFile) {
  return CreateASTDumper();
}

ASTConsumer *ASTViewAction::CreateASTConsumer(CompilerInstance &CI,
                                              llvm::StringRef InFile) {
  return CreateASTViewer();
}

ASTConsumer *DeclContextPrintAction::CreateASTConsumer(CompilerInstance &CI,
                                                       llvm::StringRef InFile) {
  return CreateDeclContextPrinter();
}

ASTConsumer *GeneratePCHAction::CreateASTConsumer(CompilerInstance &CI,
                                                  llvm::StringRef InFile) {
  const std::string &Sysroot = CI.getHeaderSearchOpts().Sysroot;
  if (CI.getFrontendOpts().RelocatablePCH &&
      Sysroot.empty()) {
    CI.getDiagnostics().Report(diag::err_relocatable_without_without_isysroot);
    return 0;
  }

  llvm::raw_ostream *OS = CI.createDefaultOutputFile(true, InFile);
  if (!OS)
    return 0;

  if (CI.getFrontendOpts().RelocatablePCH)
    return CreatePCHGenerator(CI.getPreprocessor(), OS, Sysroot.c_str());

  return CreatePCHGenerator(CI.getPreprocessor(), OS);
}

ASTConsumer *HTMLPrintAction::CreateASTConsumer(CompilerInstance &CI,
                                                llvm::StringRef InFile) {
  if (llvm::raw_ostream *OS = CI.createDefaultOutputFile(false, InFile))
    return CreateHTMLPrinter(OS, CI.getPreprocessor());
  return 0;
}

ASTConsumer *InheritanceViewAction::CreateASTConsumer(CompilerInstance &CI,
                                                      llvm::StringRef InFile) {
  return CreateInheritanceViewer(CI.getFrontendOpts().ViewClassInheritance);
}

FixItAction::FixItAction() {}
FixItAction::~FixItAction() {}

ASTConsumer *FixItAction::CreateASTConsumer(CompilerInstance &CI,
                                            llvm::StringRef InFile) {
  return new ASTConsumer();
}

/// AddFixItLocations - Add any individual user specified "fix-it" locations,
/// and return true on success.
static bool AddFixItLocations(CompilerInstance &CI,
                              FixItRewriter &FixItRewrite) {
  const std::vector<ParsedSourceLocation> &Locs =
    CI.getFrontendOpts().FixItLocations;
  for (unsigned i = 0, e = Locs.size(); i != e; ++i) {
    const FileEntry *File = CI.getFileManager().getFile(Locs[i].FileName);
    if (!File) {
      CI.getDiagnostics().Report(diag::err_fe_unable_to_find_fixit_file)
        << Locs[i].FileName;
      return false;
    }

    RequestedSourceLocation Requested;
    Requested.File = File;
    Requested.Line = Locs[i].Line;
    Requested.Column = Locs[i].Column;
    FixItRewrite.addFixItLocation(Requested);
  }

  const std::string &OutputFile = CI.getFrontendOpts().OutputFile;
  if (Locs.empty() && !OutputFile.empty()) {
    // FIXME: we will issue "FIX-IT applied suggested code changes" for every
    // input, but only the main file will actually be rewritten.
    const std::vector<std::pair<FrontendOptions::InputKind, std::string> > &Inputs =
      CI.getFrontendOpts().Inputs;
    for (unsigned i = 0, e = Inputs.size(); i != e; ++i) {
      const FileEntry *File = CI.getFileManager().getFile(Inputs[i].second);
      assert(File && "Input file not found in FileManager");
      RequestedSourceLocation Requested;
      Requested.File = File;
      Requested.Line = 0;
      Requested.Column = 0;
      FixItRewrite.addFixItLocation(Requested);
    }
  }

  return true;
}

bool FixItAction::BeginSourceFileAction(CompilerInstance &CI,
                                        llvm::StringRef Filename) {
  Rewriter.reset(new FixItRewriter(CI.getDiagnostics(), CI.getSourceManager(),
                                   CI.getLangOpts()));
  if (!AddFixItLocations(CI, *Rewriter))
    return false;

  return true;
}

void FixItAction::EndSourceFileAction() {
  const FrontendOptions &FEOpts = getCompilerInstance().getFrontendOpts();
  if (!FEOpts.OutputFile.empty()) {
    // When called with 'clang -fixit -o filename' output only the main file.

    const SourceManager &SM = getCompilerInstance().getSourceManager();
    FileID MainFileID = SM.getMainFileID();
    if (!Rewriter->IsModified(MainFileID)) {
      getCompilerInstance().getDiagnostics().Report(
          diag::note_fixit_main_file_unchanged);
      return;
    }

    llvm::OwningPtr<llvm::raw_ostream> OwnedStream;
    llvm::raw_ostream *OutFile;
    if (FEOpts.OutputFile == "-") {
      OutFile = &llvm::outs();
    } else {
      std::string Err;
      OutFile = new llvm::raw_fd_ostream(FEOpts.OutputFile.c_str(), Err,
                                         llvm::raw_fd_ostream::F_Binary);
      OwnedStream.reset(OutFile);
    }

    Rewriter->WriteFixedFile(MainFileID, *OutFile);
    return;
  }

  // Otherwise rewrite all files.
  Rewriter->WriteFixedFiles();
}

ASTConsumer *RewriteObjCAction::CreateASTConsumer(CompilerInstance &CI,
                                                  llvm::StringRef InFile) {
  if (llvm::raw_ostream *OS = CI.createDefaultOutputFile(false, InFile, "cpp"))
    return CreateObjCRewriter(InFile, OS,
                              CI.getDiagnostics(), CI.getLangOpts(),
                              CI.getDiagnosticOpts().NoRewriteMacros);
  return 0;
}

ASTConsumer *SyntaxOnlyAction::CreateASTConsumer(CompilerInstance &CI,
                                                 llvm::StringRef InFile) {
  return new ASTConsumer();
}

//===----------------------------------------------------------------------===//
// Preprocessor Actions
//===----------------------------------------------------------------------===//

void DumpRawTokensAction::ExecuteAction() {
  Preprocessor &PP = getCompilerInstance().getPreprocessor();
  SourceManager &SM = PP.getSourceManager();

  // Start lexing the specified input file.
  const llvm::MemoryBuffer *FromFile = SM.getBuffer(SM.getMainFileID());
  Lexer RawLex(SM.getMainFileID(), FromFile, SM, PP.getLangOptions());
  RawLex.SetKeepWhitespaceMode(true);

  Token RawTok;
  RawLex.LexFromRawLexer(RawTok);
  while (RawTok.isNot(tok::eof)) {
    PP.DumpToken(RawTok, true);
    llvm::errs() << "\n";
    RawLex.LexFromRawLexer(RawTok);
  }
}

void DumpTokensAction::ExecuteAction() {
  Preprocessor &PP = getCompilerInstance().getPreprocessor();
  // Start preprocessing the specified input file.
  Token Tok;
  if (PP.EnterMainSourceFile())
    return;
  do {
    PP.Lex(Tok);
    PP.DumpToken(Tok, true);
    llvm::errs() << "\n";
  } while (Tok.isNot(tok::eof));
}

void GeneratePTHAction::ExecuteAction() {
  CompilerInstance &CI = getCompilerInstance();
  if (CI.getFrontendOpts().OutputFile.empty() ||
      CI.getFrontendOpts().OutputFile == "-") {
    // FIXME: Don't fail this way.
    // FIXME: Verify that we can actually seek in the given file.
    llvm::report_fatal_error("PTH requires a seekable file for output!");
  }
  llvm::raw_fd_ostream *OS =
    CI.createDefaultOutputFile(true, getCurrentFile());
  if (!OS) return;

  CacheTokens(CI.getPreprocessor(), OS);
}

void ParseOnlyAction::ExecuteAction() {
  Preprocessor &PP = getCompilerInstance().getPreprocessor();
  llvm::OwningPtr<Action> PA(new MinimalAction(PP));

  Parser P(PP, *PA);
  if (PP.EnterMainSourceFile())
    return;
  P.ParseTranslationUnit();
}

void PreprocessOnlyAction::ExecuteAction() {
  Preprocessor &PP = getCompilerInstance().getPreprocessor();

  Token Tok;
  // Start parsing the specified input file.
  if (PP.EnterMainSourceFile())
    return;
  do {
    PP.Lex(Tok);
  } while (Tok.isNot(tok::eof));
}

void PrintParseAction::ExecuteAction() {
  CompilerInstance &CI = getCompilerInstance();
  Preprocessor &PP = getCompilerInstance().getPreprocessor();
  llvm::raw_ostream *OS = CI.createDefaultOutputFile(false, getCurrentFile());
  if (!OS) return;

  llvm::OwningPtr<Action> PA(CreatePrintParserActionsAction(PP, OS));

  Parser P(PP, *PA);
  if (PP.EnterMainSourceFile())
    return;
  P.ParseTranslationUnit();
}

void PrintPreprocessedAction::ExecuteAction() {
  CompilerInstance &CI = getCompilerInstance();
  // Output file needs to be set to 'Binary', to avoid converting Unix style
  // line feeds (<LF>) to Microsoft style line feeds (<CR><LF>).
  llvm::raw_ostream *OS = CI.createDefaultOutputFile(true, getCurrentFile());
  if (!OS) return;

  DoPrintPreprocessedInput(CI.getPreprocessor(), OS,
                           CI.getPreprocessorOutputOpts());
}

void RewriteMacrosAction::ExecuteAction() {
  CompilerInstance &CI = getCompilerInstance();
  llvm::raw_ostream *OS = CI.createDefaultOutputFile(true, getCurrentFile());
  if (!OS) return;

  RewriteMacrosInInput(CI.getPreprocessor(), OS);
}

void RewriteTestAction::ExecuteAction() {
  CompilerInstance &CI = getCompilerInstance();
  llvm::raw_ostream *OS = CI.createDefaultOutputFile(false, getCurrentFile());
  if (!OS) return;

  DoRewriteTest(CI.getPreprocessor(), OS);
}


//===----------------------------------------------------------------------===//
// Distcc Actions
//===----------------------------------------------------------------------===//

void DistributeAndPreprocessAction::ExecuteAction() {
	
	CompilerInstance &CI = getCompilerInstance();
	Distcc d(CI);
	llvm::errs() << "Distcc constructor returned!\n";
	llvm::errs().flush();
}

void StartClientServerAction::ExecuteAction() {
	DistccClientServer d;
	llvm::errs() << "Started Distributed server\n";
	llvm::errs().flush();
}
