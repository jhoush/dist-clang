//===--- FixItRewriter.cpp - Fix-It Rewriter Diagnostic Client --*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This is a diagnostic client adaptor that performs rewrites as
// suggested by code modification hints attached to diagnostics. It
// then forwards any diagnostics to the adapted diagnostic client.
//
//===----------------------------------------------------------------------===//

#include "clang/Frontend/FixItRewriter.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/FrontendDiagnostic.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/System/Path.h"
#include "llvm/ADT/OwningPtr.h"
#include <cstdio>

using namespace clang;

FixItRewriter::FixItRewriter(Diagnostic &Diags, SourceManager &SourceMgr,
                             const LangOptions &LangOpts)
  : Diags(Diags), Rewrite(SourceMgr, LangOpts), NumFailures(0) {
  Client = Diags.getClient();
  Diags.setClient(this);
}

FixItRewriter::~FixItRewriter() {
  Diags.setClient(Client);
}

bool FixItRewriter::WriteFixedFile(FileID ID, llvm::raw_ostream &OS) {
  const RewriteBuffer *RewriteBuf = Rewrite.getRewriteBufferFor(ID);
  if (!RewriteBuf) return true;
  OS << std::string(RewriteBuf->begin(), RewriteBuf->end());
  OS.flush();
  return false;
}

bool FixItRewriter::WriteFixedFiles() {
  if (NumFailures > 0) {
    Diag(FullSourceLoc(), diag::warn_fixit_no_changes);
    return true;
  }

  for (iterator I = buffer_begin(), E = buffer_end(); I != E; ++I) {
    const FileEntry *Entry = Rewrite.getSourceMgr().getFileEntryForID(I->first);
    llvm::sys::Path Path(Entry->getName());
    std::string Suffix = Path.getSuffix();
    Path.eraseSuffix();
    Path.appendSuffix("fixit." + Suffix);
    std::string Err;
    llvm::raw_fd_ostream OS(Path.c_str(), Err, llvm::raw_fd_ostream::F_Binary);
    if (!Err.empty()) {
      Diags.Report(clang::diag::err_fe_unable_to_open_output)
          << Path.c_str() << Err;
      continue;
    }
    RewriteBuffer &RewriteBuf = I->second;
    OS << std::string(RewriteBuf.begin(), RewriteBuf.end());
    OS.flush();
  }

  return false;
}

bool FixItRewriter::IncludeInDiagnosticCounts() const {
  return Client ? Client->IncludeInDiagnosticCounts() : true;
}

void FixItRewriter::HandleDiagnostic(Diagnostic::Level DiagLevel,
                                     const DiagnosticInfo &Info) {
  Client->HandleDiagnostic(DiagLevel, Info);

  // Skip over any diagnostics that are ignored.
  if (DiagLevel == Diagnostic::Ignored)
    return;

  const SourceManager &SM = Rewrite.getSourceMgr();
  if (!FixItLocations.empty()) {
    // The user has specified the locations where we should perform
    // the various fix-it modifications.

    // If this diagnostic does not have any code modifications,
    // completely ignore it, even if it's an error: fix-it locations
    // are meant to perform specific fix-ups even in the presence of
    // other errors.
    if (Info.getNumFixItHints() == 0)
      return;

    // See if the location of the error is one that matches what the
    // user requested.
    bool AcceptableLocation = false;
    const FileEntry *File = SM.getFileEntryForID(
                                                Info.getLocation().getFileID());
    unsigned Line = Info.getLocation().getSpellingLineNumber();
    unsigned Column = Info.getLocation().getSpellingColumnNumber();
    for (llvm::SmallVector<RequestedSourceLocation, 4>::iterator
           Loc = FixItLocations.begin(), LocEnd = FixItLocations.end();
         Loc != LocEnd; ++Loc) {
      if (Loc->File == File &&
          ((Loc->Line == 0 && Loc->Column == 0 &&
            DiagLevel > Diagnostic::Note) ||
           (Loc->Line == Line && Loc->Column == Column))) {
        AcceptableLocation = true;
        break;
      }
    }

    if (!AcceptableLocation)
      return;
  } else if (DiagLevel == Diagnostic::Note) {
    // Don't apply fix-it modifications in notes.
    return;
  }

  // Make sure that we can perform all of the modifications we
  // in this diagnostic.
  bool CanRewrite = Info.getNumFixItHints() > 0;
  for (unsigned Idx = 0, Last = Info.getNumFixItHints();
       Idx < Last; ++Idx) {
    const FixItHint &Hint = Info.getFixItHint(Idx);
    if (Hint.RemoveRange.isValid() &&
        Rewrite.getRangeSize(Hint.RemoveRange) == -1) {
      CanRewrite = false;
      break;
    }

    if (Hint.InsertionLoc.isValid() &&
        !Rewrite.isRewritable(Hint.InsertionLoc)) {
      CanRewrite = false;
      break;
    }
  }

  if (!CanRewrite) {
    if (Info.getNumFixItHints() > 0)
      Diag(Info.getLocation(), diag::note_fixit_in_macro);

    // If this was an error, refuse to perform any rewriting.
    if (DiagLevel == Diagnostic::Error || DiagLevel == Diagnostic::Fatal) {
      if (++NumFailures == 1)
        Diag(Info.getLocation(), diag::note_fixit_unfixed_error);
    }
    return;
  }

  bool Failed = false;
  for (unsigned Idx = 0, Last = Info.getNumFixItHints();
       Idx < Last; ++Idx) {
    const FixItHint &Hint = Info.getFixItHint(Idx);
    if (!Hint.RemoveRange.isValid()) {
      // We're adding code.
      if (Rewrite.InsertTextBefore(Hint.InsertionLoc, Hint.CodeToInsert))
        Failed = true;
      continue;
    }

    if (Hint.CodeToInsert.empty()) {
      // We're removing code.
      if (Rewrite.RemoveText(Hint.RemoveRange.getBegin(),
                             Rewrite.getRangeSize(Hint.RemoveRange)))
        Failed = true;
      continue;
    }

    // We're replacing code.
    if (Rewrite.ReplaceText(Hint.RemoveRange.getBegin(),
                            Rewrite.getRangeSize(Hint.RemoveRange),
                            Hint.CodeToInsert))
      Failed = true;
  }

  if (Failed) {
    ++NumFailures;
    Diag(Info.getLocation(), diag::note_fixit_failed);
    return;
  }

  Diag(Info.getLocation(), diag::note_fixit_applied);
}

/// \brief Emit a diagnostic via the adapted diagnostic client.
void FixItRewriter::Diag(FullSourceLoc Loc, unsigned DiagID) {
  // When producing this diagnostic, we temporarily bypass ourselves,
  // clear out any current diagnostic, and let the downstream client
  // format the diagnostic.
  Diags.setClient(Client);
  Diags.Clear();
  Diags.Report(Loc, DiagID);
  Diags.setClient(this);
}
