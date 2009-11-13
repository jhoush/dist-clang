//===--- CompilerInstance.cpp ---------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "clang/Frontend/CompilerInstance.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Lex/HeaderSearch.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/PTHManager.h"
#include "clang/Frontend/Utils.h"
#include "llvm/LLVMContext.h"
using namespace clang;

CompilerInstance::CompilerInstance(llvm::LLVMContext *_LLVMContext,
                                   bool _OwnsLLVMContext)
  : LLVMContext(_LLVMContext),
    OwnsLLVMContext(_OwnsLLVMContext) {
}

CompilerInstance::~CompilerInstance() {
  if (OwnsLLVMContext)
    delete LLVMContext;
}

void CompilerInstance::createFileManager() {
  FileMgr.reset(new FileManager());
}

void CompilerInstance::createSourceManager() {
  SourceMgr.reset(new SourceManager());
}

void CompilerInstance::createPreprocessor() {
  PP.reset(createPreprocessor(getDiagnostics(), getLangOpts(),
                              getPreprocessorOpts(), getHeaderSearchOpts(),
                              getDependencyOutputOpts(), getTarget(),
                              getSourceManager(), getFileManager()));
}

Preprocessor *
CompilerInstance::createPreprocessor(Diagnostic &Diags,
                                     const LangOptions &LangInfo,
                                     const PreprocessorOptions &PPOpts,
                                     const HeaderSearchOptions &HSOpts,
                                     const DependencyOutputOptions &DepOpts,
                                     const TargetInfo &Target,
                                     SourceManager &SourceMgr,
                                     FileManager &FileMgr) {
  // Create a PTH manager if we are using some form of a token cache.
  PTHManager *PTHMgr = 0;
  if (!PPOpts.getTokenCache().empty())
    PTHMgr = PTHManager::Create(PPOpts.getTokenCache(), Diags);

  // FIXME: Don't fail like this.
  if (Diags.hasErrorOccurred())
    exit(1);

  // Create the Preprocessor.
  HeaderSearch *HeaderInfo = new HeaderSearch(FileMgr);
  Preprocessor *PP = new Preprocessor(Diags, LangInfo, Target,
                                      SourceMgr, *HeaderInfo, PTHMgr,
                                      /*OwnsHeaderSearch=*/true);

  // Note that this is different then passing PTHMgr to Preprocessor's ctor.
  // That argument is used as the IdentifierInfoLookup argument to
  // IdentifierTable's ctor.
  if (PTHMgr) {
    PTHMgr->setPreprocessor(PP);
    PP->setPTHManager(PTHMgr);
  }

  InitializePreprocessor(*PP, PPOpts, HSOpts);

  // Handle generating dependencies, if requested.
  if (!DepOpts.OutputFile.empty())
    AttachDependencyFileGen(*PP, DepOpts);

  return PP;
}