//===- ASTRecordLayoutBuilder.h - Helper class for building record layouts ===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_AST_RECORDLAYOUTBUILDER_H
#define LLVM_CLANG_AST_RECORDLAYOUTBUILDER_H

#include "clang/AST/RecordLayout.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/System/DataTypes.h"
#include <map>

namespace clang {
  class ASTContext;
  class ASTRecordLayout;
  class CXXRecordDecl;
  class FieldDecl;
  class ObjCImplementationDecl;
  class ObjCInterfaceDecl;
  class RecordDecl;

class ASTRecordLayoutBuilder {
  ASTContext &Ctx;

  /// Size - The current size of the record layout.
  uint64_t Size;
  
  /// Alignment - The current alignment of the record layout.
  unsigned Alignment;
  
  llvm::SmallVector<uint64_t, 16> FieldOffsets;

  /// Packed - Whether the record is packed or not.
  bool Packed;

  /// UnfilledBitsInLastByte - If the last field laid out was a bitfield,
  /// this contains the number of bits in the last byte that can be used for
  /// an adjacent bitfield if necessary.
  unsigned char UnfilledBitsInLastByte;
  
  /// MaxFieldAlignment - The maximum allowed field alignment. This is set by
  /// #pragma pack. 
  unsigned MaxFieldAlignment;
  
  /// DataSize - The data size of the record being laid out.
  uint64_t DataSize;
  
  bool IsUnion;

  uint64_t NonVirtualSize;
  unsigned NonVirtualAlignment;
  
  /// PrimaryBase - the primary base class (if one exists) of the class
  /// we're laying out.
  ASTRecordLayout::PrimaryBaseInfo PrimaryBase;

  /// Bases - base classes and their offsets in the record.
  ASTRecordLayout::BaseOffsetsMapTy Bases;
  
  // VBases - virtual base classes and their offsets in the record.
  ASTRecordLayout::BaseOffsetsMapTy VBases;

  /// IndirectPrimaryBases - Virtual base classes, direct or indirect, that are
  /// primary base classes for some other direct or indirect base class.
  llvm::SmallSet<const CXXRecordDecl*, 32> IndirectPrimaryBases;
  
  /// FirstNearlyEmptyVBase - The first nearly empty virtual base class in
  /// inheritance graph order. Used for determining the primary base class.
  const CXXRecordDecl *FirstNearlyEmptyVBase;

  /// VisitedVirtualBases - A set of all the visited virtual bases, used to
  /// avoid visiting virtual bases more than once.
  llvm::SmallPtrSet<const CXXRecordDecl *, 4> VisitedVirtualBases;
  
  /// EmptyClassOffsets - A map from offsets to empty record decls.
  typedef std::multimap<uint64_t, const CXXRecordDecl *> EmptyClassOffsetsTy;
  EmptyClassOffsetsTy EmptyClassOffsets;
  
  ASTRecordLayoutBuilder(ASTContext &Ctx);

  void Layout(const RecordDecl *D);
  void Layout(const CXXRecordDecl *D);
  void Layout(const ObjCInterfaceDecl *D,
              const ObjCImplementationDecl *Impl);

  void LayoutFields(const RecordDecl *D);
  void LayoutField(const FieldDecl *D);
  void LayoutBitField(const FieldDecl *D);

  /// DeterminePrimaryBase - Determine the primary base of the given class.
  void DeterminePrimaryBase(const CXXRecordDecl *RD);

  void SelectPrimaryVBase(const CXXRecordDecl *RD);
  
  /// IdentifyPrimaryBases - Identify all virtual base classes, direct or 
  /// indirect, that are primary base classes for some other direct or indirect 
  /// base class.
  void IdentifyPrimaryBases(const CXXRecordDecl *RD);
  
  bool IsNearlyEmpty(const CXXRecordDecl *RD) const;
  
  /// LayoutNonVirtualBases - Determines the primary base class (if any) and 
  /// lays it out. Will then proceed to lay out all non-virtual base clasess.
  void LayoutNonVirtualBases(const CXXRecordDecl *RD);

  /// LayoutNonVirtualBase - Lays out a single non-virtual base.
  void LayoutNonVirtualBase(const CXXRecordDecl *RD);

  void AddPrimaryVirtualBaseOffsets(const CXXRecordDecl *RD, uint64_t Offset,
                                    const CXXRecordDecl *MostDerivedClass);

  /// LayoutVirtualBases - Lays out all the virtual bases.
  void LayoutVirtualBases(const CXXRecordDecl *RD,
                          const CXXRecordDecl *MostDerivedClass);

  /// LayoutVirtualBase - Lays out a single virtual base.
  void LayoutVirtualBase(const CXXRecordDecl *RD);

  /// LayoutBase - Will lay out a base and return the offset where it was 
  /// placed, in bits.
  uint64_t LayoutBase(const CXXRecordDecl *RD);

  /// canPlaceRecordAtOffset - Return whether a record (either a base class
  /// or a field) can be placed at the given offset. 
  /// Returns false if placing the record will result in two components 
  /// (direct or indirect) of the same type having the same offset.
  bool canPlaceRecordAtOffset(const CXXRecordDecl *RD, uint64_t Offset) const;

  /// canPlaceFieldAtOffset - Return whether a field can be placed at the given
  /// offset.
  bool canPlaceFieldAtOffset(const FieldDecl *FD, uint64_t Offset) const;

  /// UpdateEmptyClassOffsets - Called after a record (either a base class
  /// or a field) has been placed at the given offset. Will update the
  /// EmptyClassOffsets map if the class is empty or has any empty bases or
  /// fields.
  void UpdateEmptyClassOffsets(const CXXRecordDecl *RD, uint64_t Offset);

  /// UpdateEmptyClassOffsets - Called after a field has been placed at the 
  /// given offset.
  void UpdateEmptyClassOffsets(const FieldDecl *FD, uint64_t Offset);
  
  /// FinishLayout - Finalize record layout. Adjust record size based on the
  /// alignment.
  void FinishLayout();

  void UpdateAlignment(unsigned NewAlignment);

  ASTRecordLayoutBuilder(const ASTRecordLayoutBuilder&);   // DO NOT IMPLEMENT
  void operator=(const ASTRecordLayoutBuilder&); // DO NOT IMPLEMENT
public:
  static const ASTRecordLayout *ComputeLayout(ASTContext &Ctx,
                                              const RecordDecl *RD);
  static const ASTRecordLayout *ComputeLayout(ASTContext &Ctx,
                                              const ObjCInterfaceDecl *D,
                                            const ObjCImplementationDecl *Impl);
  static const CXXMethodDecl *ComputeKeyFunction(const CXXRecordDecl *RD);
};

} // end namespace clang

#endif

