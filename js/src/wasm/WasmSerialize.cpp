/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "wasm/WasmSerialize.h"

#include "mozilla/EnumeratedRange.h"
#include "mozilla/Maybe.h"
#include "mozilla/RefPtr.h"
#include "mozilla/Try.h"
#include "mozilla/Vector.h"

#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>

#include "jit/ProcessExecutableMemory.h"
#include "js/StreamConsumer.h"
#include "wasm/WasmCode.h"
#include "wasm/WasmCodegenTypes.h"
#include "wasm/WasmGC.h"
#include "wasm/WasmInitExpr.h"
#include "wasm/WasmModule.h"
#include "wasm/WasmModuleTypes.h"
#include "wasm/WasmTypeDef.h"
#include "wasm/WasmValType.h"
#include "wasm/WasmValue.h"

using namespace js;
using namespace js::wasm;

using mozilla::Err;
using mozilla::Ok;

namespace js {
namespace wasm {

// The following assert is used as a tripwire for for new fields being added to
// types. If this assertion is broken by your code, verify the serialization
// code is correct, and then update the assertion.
//
// We only check serialized type sizes on one 'golden' platform. The platform
// is arbitrary, but must remain consistent. The platform should be as specific
// as possible so these assertions don't erroneously fire. Checkout the
// definition of ENABLE_WASM_VERIFY_SERIALIZATION_FOR_SIZE in js/moz.configure
// for the current platform.
//
// If this mechanism becomes a hassle, we can investigate other methods of
// achieving the same goal.
#if defined(ENABLE_WASM_VERIFY_SERIALIZATION_FOR_SIZE)

template <typename T, size_t Size>
struct Tripwire {
  // The following will print a compile error that contains the size of type,
  // and can be used for updating the assertion to the correct size.
  static char (*_Error)[sizeof(T)] = 1;

  // We must reference the bad declaration to work around SFINAE.
  static const bool Value = !_Error;
};

template <typename T>
struct Tripwire<T, sizeof(T)> {
  static const bool Value = true;
};

#  define WASM_VERIFY_SERIALIZATION_FOR_SIZE(Type, Size) \
    static_assert(Tripwire<Type, Size>::Value);

#else
#  define WASM_VERIFY_SERIALIZATION_FOR_SIZE(Type, Size) static_assert(true);
#endif

// A pointer is not cacheable POD
static_assert(!is_cacheable_pod<const uint8_t*>);

// A non-fixed sized array is not cacheable POD
static_assert(!is_cacheable_pod<uint8_t[]>);

// Cacheable POD is not inherited
struct TestPodBase {};
WASM_DECLARE_CACHEABLE_POD(TestPodBase);
struct InheritTestPodBase : public TestPodBase {};
static_assert(is_cacheable_pod<TestPodBase> &&
              !is_cacheable_pod<InheritTestPodBase>);

// Coding functions for containers and smart pointers need to know which code
// function to apply to the inner value. 'CodeFunc' is the common signature to
// be used for this.
template <CoderMode mode, typename T>
using CodeFunc = CoderResult (*)(Coder<mode>&, CoderArg<mode, T>);

// Some functions are generic for MODE_SIZE and MODE_ENCODE, but not
// MODE_DECODE. This assert is used to ensure that the right function overload
// is chosen in these cases.
#define STATIC_ASSERT_ENCODING_OR_SIZING \
  static_assert(mode == MODE_ENCODE || mode == MODE_SIZE, "wrong overload");

CoderResult Coder<MODE_SIZE>::writeBytes(const void* unusedSrc, size_t length) {
  size_ += length;
  if (!size_.isValid()) {
    return Err(OutOfMemory());
  }
  return Ok();
}

CoderResult Coder<MODE_ENCODE>::writeBytes(const void* src, size_t length) {
  MOZ_RELEASE_ASSERT(buffer_ + length <= end_);
  memcpy(buffer_, src, length);
  buffer_ += length;
  return Ok();
}

CoderResult Coder<MODE_DECODE>::readBytes(void* dest, size_t length) {
  MOZ_RELEASE_ASSERT(buffer_ + length <= end_);
  memcpy(dest, buffer_, length);
  buffer_ += length;
  return Ok();
}

CoderResult Coder<MODE_DECODE>::readBytesRef(size_t length,
                                             const uint8_t** bytesBegin) {
  MOZ_RELEASE_ASSERT(buffer_ + length <= end_);
  *bytesBegin = buffer_;
  buffer_ += length;
  return Ok();
}

// Cacheable POD coding functions

template <typename T,
          typename std::enable_if_t<is_cacheable_pod<T>, bool> = true>
CoderResult CodePod(Coder<MODE_DECODE>& coder, T* item) {
  return coder.readBytes((void*)item, sizeof(T));
}

template <CoderMode mode, typename T,
          typename std::enable_if_t<is_cacheable_pod<T>, bool> = true>
CoderResult CodePod(Coder<mode>& coder, const T* item) {
  STATIC_ASSERT_ENCODING_OR_SIZING;
  return coder.writeBytes((const void*)item, sizeof(T));
}

// "Magic Marker". Use to sanity check the serialization process.

enum class Marker : uint32_t {
  LinkData = 0x49102278,
  Imports,
  Exports,
  DataSegments,
  ElemSegments,
  CustomSections,
  Code,
  Metadata,
  ModuleMetadata,
  CodeMetadata,
  CodeBlock,
  CodeSegment,
};

template <CoderMode mode>
CoderResult Magic(Coder<mode>& coder, Marker item) {
  if constexpr (mode == MODE_DECODE) {
    // Assert the specified marker is in the binary
    Marker decoded;
    MOZ_TRY(CodePod(coder, &decoded));
    MOZ_RELEASE_ASSERT(decoded == item);
    return Ok();
  } else {
    // Encode the specified marker in the binary
    return CodePod(coder, &item);
  }
}

// mozilla::Maybe coding functions
//
// These functions will only code the inner value if Maybe.isSome(). The
// coding function to use for the inner value is specified by a template
// parameter.

template <CoderMode _, typename T, CodeFunc<MODE_DECODE, T> CodeT>
CoderResult CodeMaybe(Coder<MODE_DECODE>& coder, Maybe<T>* item) {
  // Decode 'isSome()'
  uint8_t isSome;
  MOZ_TRY(CodePod(coder, &isSome));

  if (isSome == 1) {
    // Initialize to Some with default constructor
    item->emplace();
    // Code the inner type
    MOZ_TRY(CodeT(coder, item->ptr()));
  } else {
    // Initialize to nothing
    *item = mozilla::Nothing();
  }
  return Ok();
}

template <CoderMode mode, typename T, CodeFunc<mode, T> CodeT>
CoderResult CodeMaybe(Coder<mode>& coder, const Maybe<T>* item) {
  STATIC_ASSERT_ENCODING_OR_SIZING;

  // Encode or size 'isSome()'
  const uint8_t isSome = item->isSome() ? 1 : 0;
  MOZ_TRY(CodePod(coder, &isSome));

  if (item->isSome()) {
    // Encode or size the inner value
    MOZ_TRY(CodeT(coder, item->ptr()));
  }
  return Ok();
}

// Cacheable POD mozilla::Vector coding functions
//
// These functions are only available if the element type is cacheable POD. In
// this case, the whole contents of the vector are copied directly to/from the
// buffer.

template <typename T, size_t N,
          typename std::enable_if_t<is_cacheable_pod<T>, bool> = true>
CoderResult CodePodVector(Coder<MODE_DECODE>& coder,
                          Vector<T, N, SystemAllocPolicy>* item) {
  // Decode the length
  size_t length;
  MOZ_TRY(CodePod(coder, &length));

  // Prepare to copy into the vector
  if (!item->initLengthUninitialized(length)) {
    return Err(OutOfMemory());
  }

  // Copy directly from the buffer to the vector
  const size_t byteLength = length * sizeof(T);
  return coder.readBytes((void*)item->begin(), byteLength);
}

template <CoderMode mode, typename T, size_t N,
          typename std::enable_if_t<is_cacheable_pod<T>, bool> = true>
CoderResult CodePodVector(Coder<mode>& coder,
                          const Vector<T, N, SystemAllocPolicy>* item) {
  STATIC_ASSERT_ENCODING_OR_SIZING;

  // Encode the length
  const size_t length = item->length();
  MOZ_TRY(CodePod(coder, &length));

  // Copy directly from the vector to the buffer
  const size_t byteLength = length * sizeof(T);
  return coder.writeBytes((const void*)item->begin(), byteLength);
}

// Non-cacheable-POD mozilla::Vector coding functions
//
// These functions implement the general case of coding a vector of some type.
// The coding function to use on the vector elements is provided through a
// template parameter.

template <CoderMode _, typename T, CodeFunc<MODE_DECODE, T> CodeT, size_t N,
          typename std::enable_if_t<!is_cacheable_pod<T>, bool> = true>
CoderResult CodeVector(Coder<MODE_DECODE>& coder,
                       Vector<T, N, SystemAllocPolicy>* item) {
  // Decode the length
  size_t length;
  MOZ_TRY(CodePod(coder, &length));

  // Attempt to grow the buffer to length, this will default initialize each
  // element
  if (!item->resize(length)) {
    return Err(OutOfMemory());
  }

  // Decode each child element from the buffer
  for (auto iter = item->begin(); iter != item->end(); iter++) {
    MOZ_TRY(CodeT(coder, iter));
  }
  return Ok();
}

template <CoderMode mode, typename T, CodeFunc<mode, T> CodeT, size_t N,
          typename std::enable_if_t<!is_cacheable_pod<T>, bool> = true>
CoderResult CodeVector(Coder<mode>& coder,
                       const Vector<T, N, SystemAllocPolicy>* item) {
  STATIC_ASSERT_ENCODING_OR_SIZING;

  // Encode the length
  const size_t length = item->length();
  MOZ_TRY(CodePod(coder, &length));

  // Encode each child element
  for (auto iter = item->begin(); iter != item->end(); iter++) {
    MOZ_TRY(CodeT(coder, iter));
  }
  return Ok();
}

// This function implements encoding and decoding of RefPtr<T>. A coding
// function is provided for the inner value through a template parameter.
//
// The special handling of const qualification allows a RefPtr<const T> to be
// decoded correctly.
template <CoderMode mode, typename T,
          CodeFunc<mode, std::remove_const_t<T>> CodeT>
CoderResult CodeRefPtr(Coder<mode>& coder, CoderArg<mode, RefPtr<T>> item) {
  if constexpr (mode == MODE_DECODE) {
    // The RefPtr should not be initialized yet
    MOZ_ASSERT(!item->get());

    // Allocate and default construct the inner type
    auto* allocated = js_new<std::remove_const_t<T>>();
    if (!allocated) {
      return Err(OutOfMemory());
    }

    // Initialize the RefPtr
    *item = allocated;

    // Decode the inner type
    MOZ_TRY(CodeT(coder, allocated));
    return Ok();
  } else {
    // Encode the inner type
    return CodeT(coder, item->get());
  }
}

// This function implements encoding and decoding of UniquePtr<T>.
// A coding function is provided for the inner value as a function parameter.
template <CoderMode mode, typename T,
          CodeFunc<mode, std::remove_const_t<T>> CodeT>
CoderResult CodeUniquePtr(Coder<mode>& coder,
                          CoderArg<mode, UniquePtr<T>> item) {
  if constexpr (mode == MODE_DECODE) {
    // The UniquePtr should not be initialized yet
    MOZ_ASSERT(!item->get());

    // Allocate and default construct the inner type
    auto allocated = js::MakeUnique<std::remove_const_t<T>>();
    if (!allocated.get()) {
      return Err(OutOfMemory());
    }

    // Decode the inner type
    MOZ_TRY(CodeT(coder, allocated.get()));

    // Initialize the UniquePtr
    *item = std::move(allocated);
    return Ok();
  } else {
    // Encode the inner type
    return CodeT(coder, item->get());
  }
}

// UniqueChars coding functions

static size_t StringLengthWithNullChar(const char* chars) {
  return chars ? strlen(chars) + 1 : 0;
}

CoderResult CodeUniqueChars(Coder<MODE_DECODE>& coder, UniqueChars* item) {
  uint32_t lengthWithNullChar;
  MOZ_TRY(CodePod(coder, &lengthWithNullChar));

  // Decode the bytes, if any
  if (lengthWithNullChar) {
    item->reset(js_pod_malloc<char>(lengthWithNullChar));
    if (!item->get()) {
      return Err(OutOfMemory());
    }
    return coder.readBytes((char*)item->get(), lengthWithNullChar);
  }

  // If there were no bytes to write, the string should be null
  MOZ_ASSERT(!item->get());
  return Ok();
}

template <CoderMode mode>
CoderResult CodeUniqueChars(Coder<mode>& coder, const UniqueChars* item) {
  WASM_VERIFY_SERIALIZATION_FOR_SIZE(UniqueChars, 8);
  STATIC_ASSERT_ENCODING_OR_SIZING;

  // Encode the length
  const uint32_t lengthWithNullChar = StringLengthWithNullChar(item->get());
  MOZ_TRY(CodePod(coder, &lengthWithNullChar));

  // Write the bytes, if any
  if (lengthWithNullChar) {
    return coder.writeBytes((const void*)item->get(), lengthWithNullChar);
  }

  // If there were no bytes to write, the string should be null
  MOZ_ASSERT(!item->get());
  return Ok();
}

// Code a CacheableChars. This just forwards to UniqueChars, as that's the
// only data in the class, via inheritance.
template <CoderMode mode>
CoderResult CodeCacheableChars(Coder<mode>& coder,
                               CoderArg<mode, CacheableChars> item) {
  WASM_VERIFY_SERIALIZATION_FOR_SIZE(wasm::CacheableChars, 8);
  return CodeUniqueChars(coder, (UniqueChars*)item);
}

// Code a CacheableName
template <CoderMode mode>
CoderResult CodeCacheableName(Coder<mode>& coder,
                              CoderArg<mode, CacheableName> item) {
  WASM_VERIFY_SERIALIZATION_FOR_SIZE(wasm::CacheableName, 40);
  MOZ_TRY(CodePodVector(coder, &item->bytes_));
  return Ok();
}

// Code a ShareableBytes. This function only needs to forward to the inner
// bytes vector.
template <CoderMode mode>
CoderResult CodeShareableBytes(Coder<mode>& coder,
                               CoderArg<mode, ShareableBytes> item) {
  WASM_VERIFY_SERIALIZATION_FOR_SIZE(wasm::ShareableBytes, 48);
  return CodePodVector(coder, &item->bytes);
}

// WasmValType.h

/* static */
SerializableTypeCode SerializableTypeCode::serialize(PackedTypeCode ptc,
                                                     const TypeContext& types) {
  SerializableTypeCode stc = {};
  stc.typeCode = PackedRepr(ptc.typeCode());
  stc.typeIndex = ptc.typeDef() ? types.indexOf(*ptc.typeDef())
                                : SerializableTypeCode::NoTypeIndex;
  stc.nullable = ptc.isNullable();
  return stc;
}

PackedTypeCode SerializableTypeCode::deserialize(const TypeContext& types) {
  if (typeIndex == SerializableTypeCode::NoTypeIndex) {
    return PackedTypeCode::pack(TypeCode(typeCode), nullable);
  }
  const TypeDef* typeDef = &types.type(typeIndex);
  return PackedTypeCode::pack(TypeCode(typeCode), typeDef, nullable);
}

template <CoderMode mode>
CoderResult CodePackedTypeCode(Coder<mode>& coder,
                               CoderArg<mode, PackedTypeCode> item) {
  if constexpr (mode == MODE_DECODE) {
    SerializableTypeCode stc;
    MOZ_TRY(CodePod(coder, &stc));
    *item = stc.deserialize(*coder.types_);
    return Ok();
  } else if constexpr (mode == MODE_SIZE) {
    return coder.writeBytes(nullptr, sizeof(SerializableTypeCode));
  } else {
    SerializableTypeCode stc =
        SerializableTypeCode::serialize(*item, *coder.types_);
    return CodePod(coder, &stc);
  }
}

template <CoderMode mode>
CoderResult CodeTypeDefRef(Coder<mode>& coder,
                           CoderArg<mode, const TypeDef*> item) {
  static constexpr uint32_t NullTypeIndex = UINT32_MAX;
  static_assert(NullTypeIndex > MaxTypes, "invariant");

  if constexpr (mode == MODE_DECODE) {
    uint32_t typeIndex;
    MOZ_TRY(CodePod(coder, &typeIndex));
    if (typeIndex != NullTypeIndex) {
      *item = &coder.types_->type(typeIndex);
    }
    return Ok();
  } else if constexpr (mode == MODE_SIZE) {
    return coder.writeBytes(nullptr, sizeof(uint32_t));
  } else {
    uint32_t typeIndex = !*item ? NullTypeIndex : coder.types_->indexOf(**item);
    return CodePod(coder, &typeIndex);
  }
}

template <CoderMode mode>
CoderResult CodeValType(Coder<mode>& coder, CoderArg<mode, ValType> item) {
  return CodePackedTypeCode(coder, item->addressOfPacked());
}

template <CoderMode mode>
CoderResult CodeStorageType(Coder<mode>& coder,
                            CoderArg<mode, StorageType> item) {
  return CodePackedTypeCode(coder, item->addressOfPacked());
}

template <CoderMode mode>
CoderResult CodeRefType(Coder<mode>& coder, CoderArg<mode, RefType> item) {
  return CodePackedTypeCode(coder, item->addressOfPacked());
}

// WasmValue.h

template <CoderMode mode>
CoderResult CodeLitVal(Coder<mode>& coder, CoderArg<mode, LitVal> item) {
  MOZ_TRY(CodeValType(coder, &item->type_));
  MOZ_TRY(CodePod(coder, &item->cell_));
  return Ok();
}

// WasmInitExpr.h

template <CoderMode mode>
CoderResult CodeInitExpr(Coder<mode>& coder, CoderArg<mode, InitExpr> item) {
  WASM_VERIFY_SERIALIZATION_FOR_SIZE(wasm::InitExpr, 80);
  MOZ_TRY(CodePod(coder, &item->kind_));
  MOZ_TRY(CodeValType(coder, &item->type_));
  switch (item->kind_) {
    case InitExprKind::Literal:
      MOZ_TRY(CodeLitVal(coder, &item->literal_));
      break;
    case InitExprKind::Variable:
      MOZ_TRY(CodePodVector(coder, &item->bytecode_));
      break;
    default:
      MOZ_CRASH();
  }
  return Ok();
}

// WasmTypeDef.h

template <CoderMode mode>
CoderResult CodeFuncType(Coder<mode>& coder, CoderArg<mode, FuncType> item) {
  WASM_VERIFY_SERIALIZATION_FOR_SIZE(wasm::FuncType, 344);
  MOZ_TRY((CodeVector<mode, ValType, &CodeValType<mode>>(coder, &item->args_)));
  MOZ_TRY(
      (CodeVector<mode, ValType, &CodeValType<mode>>(coder, &item->results_)));
  MOZ_TRY(CodePod(coder, &item->immediateTypeId_));
  return Ok();
}

template <CoderMode mode>
CoderResult CodeStructField(Coder<mode>& coder,
                            CoderArg<mode, StructField> item) {
  MOZ_TRY(CodeStorageType(coder, &item->type));
  MOZ_TRY(CodePod(coder, &item->offset));
  MOZ_TRY(CodePod(coder, &item->isMutable));
  return Ok();
}

template <CoderMode mode>
CoderResult CodeStructType(Coder<mode>& coder,
                           CoderArg<mode, StructType> item) {
  WASM_VERIFY_SERIALIZATION_FOR_SIZE(wasm::StructType, 136);
  MOZ_TRY((CodeVector<mode, StructField, &CodeStructField<mode>>(
      coder, &item->fields_)));
  if constexpr (mode == MODE_DECODE) {
    if (!item->init()) {
      return Err(OutOfMemory());
    }
  }
  return Ok();
}

template <CoderMode mode>
CoderResult CodeArrayType(Coder<mode>& coder, CoderArg<mode, ArrayType> item) {
  WASM_VERIFY_SERIALIZATION_FOR_SIZE(wasm::ArrayType, 16);
  MOZ_TRY(CodeStorageType(coder, &item->elementType_));
  MOZ_TRY(CodePod(coder, &item->isMutable_));
  return Ok();
}

template <CoderMode mode>
CoderResult CodeTypeDef(Coder<mode>& coder, CoderArg<mode, TypeDef> item) {
  WASM_VERIFY_SERIALIZATION_FOR_SIZE(wasm::TypeDef, 376);
  MOZ_TRY(CodeTypeDefRef(coder, &item->superTypeDef_));
  MOZ_TRY(CodePod(coder, &item->subTypingDepth_));
  MOZ_TRY(CodePod(coder, &item->isFinal_));
  // TypeDef is a tagged union containing kind = None. This implies that
  // we must manually initialize the variant that we decode.
  if constexpr (mode == MODE_DECODE) {
    MOZ_RELEASE_ASSERT(item->kind_ == TypeDefKind::None);
  }
  MOZ_TRY(CodePod(coder, &item->kind_));
  switch (item->kind_) {
    case TypeDefKind::Struct: {
      if constexpr (mode == MODE_DECODE) {
        new (&item->structType_) StructType();
      }
      MOZ_TRY(CodeStructType(coder, &item->structType_));
      break;
    }
    case TypeDefKind::Func: {
      if constexpr (mode == MODE_DECODE) {
        new (&item->funcType_) FuncType();
      }
      MOZ_TRY(CodeFuncType(coder, &item->funcType_));
      break;
    }
    case TypeDefKind::Array: {
      if constexpr (mode == MODE_DECODE) {
        new (&item->arrayType_) ArrayType();
      }
      MOZ_TRY(CodeArrayType(coder, &item->arrayType_));
      break;
    }
    case TypeDefKind::None: {
      break;
    }
    default:
      MOZ_ASSERT_UNREACHABLE();
  }
  return Ok();
}

using RecGroupIndexMap =
    HashMap<const RecGroup*, uint32_t, PointerHasher<const RecGroup*>,
            SystemAllocPolicy>;

template <CoderMode mode>
CoderResult CodeTypeContext(Coder<mode>& coder,
                            CoderArg<mode, TypeContext> item) {
  if constexpr (mode == MODE_DECODE) {
    // Decoding type definitions needs to reference the type context of the
    // module
    MOZ_ASSERT(!coder.types_);
    coder.types_ = item;

    // Decode the number of recursion groups in the module
    uint32_t numRecGroups;
    MOZ_TRY(CodePod(coder, &numRecGroups));

    // Decode each recursion group
    for (uint32_t recGroupIndex = 0; recGroupIndex < numRecGroups;
         recGroupIndex++) {
      // Decode if this recursion group is equivalent to a previous recursion
      // group
      uint32_t canonRecGroupIndex;
      MOZ_TRY(CodePod(coder, &canonRecGroupIndex));
      MOZ_RELEASE_ASSERT(canonRecGroupIndex <= recGroupIndex);

      // If the decoded index is not ours, we must re-use the previous decoded
      // recursion group.
      if (canonRecGroupIndex != recGroupIndex) {
        SharedRecGroup recGroup = item->groups()[canonRecGroupIndex];
        if (!item->addRecGroup(recGroup)) {
          return Err(OutOfMemory());
        }
        continue;
      }

      // Decode the number of types in the recursion group
      uint32_t numTypes;
      MOZ_TRY(CodePod(coder, &numTypes));

      MutableRecGroup recGroup = item->startRecGroup(numTypes);
      if (!recGroup) {
        return Err(OutOfMemory());
      }

      // Decode the type definitions
      for (uint32_t groupTypeIndex = 0; groupTypeIndex < numTypes;
           groupTypeIndex++) {
        MOZ_TRY(CodeTypeDef(coder, &recGroup->type(groupTypeIndex)));
      }

      // Finish the recursion group
      if (!item->endRecGroup()) {
        return Err(OutOfMemory());
      }
    }
  } else {
    // Encode the number of recursion groups in the module
    uint32_t numRecGroups = item->groups().length();
    MOZ_TRY(CodePod(coder, &numRecGroups));

    // We must be careful to only encode every unique recursion group only once
    // and in module order. The reason for this is that encoding type def
    // references uses the module type index map, which only stores the first
    // type index a type was canonicalized to.
    //
    // Using this map to encode both recursion groups would turn the following
    // type section from:
    //
    // 0: (type (struct (field 0)))
    // 1: (type (struct (field 1))) ;; identical to 0
    //
    // into:
    //
    // 0: (type (struct (field 0)))
    // 1: (type (struct (field 0))) ;; not identical to 0!
    RecGroupIndexMap canonRecGroups;

    // Encode each recursion group
    for (uint32_t groupIndex = 0; groupIndex < numRecGroups; groupIndex++) {
      SharedRecGroup group = item->groups()[groupIndex];

      // Find the index of the first time this recursion group was encoded, or
      // set it to this index if it hasn't been encoded.
      RecGroupIndexMap::AddPtr canonRecGroupIndex =
          canonRecGroups.lookupForAdd(group.get());
      if (!canonRecGroupIndex) {
        if (!canonRecGroups.add(canonRecGroupIndex, group.get(), groupIndex)) {
          return Err(OutOfMemory());
        }
      }

      // Encode the canon index for this recursion group
      MOZ_TRY(CodePod(coder, &canonRecGroupIndex->value()));

      // Don't encode this recursion group if we've already encoded it
      if (canonRecGroupIndex->value() != groupIndex) {
        continue;
      }

      // Encode the number of types in the recursion group
      uint32_t numTypes = group->numTypes();
      MOZ_TRY(CodePod(coder, &numTypes));

      // Encode the type definitions
      for (uint32_t i = 0; i < numTypes; i++) {
        MOZ_TRY(CodeTypeDef(coder, &group->type(i)));
      }
    }
  }
  return Ok();
}

// WasmModuleTypes.h

template <CoderMode mode>
CoderResult CodeImport(Coder<mode>& coder, CoderArg<mode, Import> item) {
  WASM_VERIFY_SERIALIZATION_FOR_SIZE(wasm::Import, 88);
  MOZ_TRY(CodeCacheableName(coder, &item->module));
  MOZ_TRY(CodeCacheableName(coder, &item->field));
  MOZ_TRY(CodePod(coder, &item->kind));
  return Ok();
}

template <CoderMode mode>
CoderResult CodeExport(Coder<mode>& coder, CoderArg<mode, Export> item) {
  WASM_VERIFY_SERIALIZATION_FOR_SIZE(wasm::Export, 48);
  MOZ_TRY(CodeCacheableName(coder, &item->fieldName_));
  MOZ_TRY(CodePod(coder, &item->pod));
  return Ok();
}

template <CoderMode mode>
CoderResult CodeGlobalDesc(Coder<mode>& coder,
                           CoderArg<mode, GlobalDesc> item) {
  WASM_VERIFY_SERIALIZATION_FOR_SIZE(wasm::GlobalDesc, 104);
  MOZ_TRY(CodePod(coder, &item->kind_));
  MOZ_TRY(CodeInitExpr(coder, &item->initial_));
  MOZ_TRY(CodePod(coder, &item->offset_));
  MOZ_TRY(CodePod(coder, &item->isMutable_));
  MOZ_TRY(CodePod(coder, &item->isWasm_));
  MOZ_TRY(CodePod(coder, &item->isExport_));
  MOZ_TRY(CodePod(coder, &item->importIndex_));
  return Ok();
}

template <CoderMode mode>
CoderResult CodeTagType(Coder<mode>& coder, CoderArg<mode, TagType> item) {
  WASM_VERIFY_SERIALIZATION_FOR_SIZE(wasm::TagType, 232);
  // We skip serializing/deserializing the size and argOffsets fields because
  // those are computed from the argTypes field when we deserialize.
  if constexpr (mode == MODE_DECODE) {
    ValTypeVector argTypes;
    MOZ_TRY((CodeVector<MODE_DECODE, ValType, &CodeValType<MODE_DECODE>>(
        coder, &argTypes)));
    if (!item->initialize(std::move(argTypes))) {
      return Err(OutOfMemory());
    }
  } else {
    MOZ_TRY((CodeVector<mode, ValType, &CodeValType<mode>>(coder,
                                                           &item->argTypes())));
  }
  return Ok();
}

template <CoderMode mode>
CoderResult CodeTagDesc(Coder<mode>& coder, CoderArg<mode, TagDesc> item) {
  WASM_VERIFY_SERIALIZATION_FOR_SIZE(wasm::TagDesc, 24);
  MOZ_TRY(CodePod(coder, &item->kind));
  MOZ_TRY((
      CodeRefPtr<mode, const TagType, &CodeTagType<mode>>(coder, &item->type)));
  MOZ_TRY(CodePod(coder, &item->isExport));
  return Ok();
}

template <CoderMode mode>
CoderResult CodeModuleElemSegment(Coder<mode>& coder,
                                  CoderArg<mode, ModuleElemSegment> item) {
  WASM_VERIFY_SERIALIZATION_FOR_SIZE(wasm::ModuleElemSegment, 232);
  MOZ_TRY(CodePod(coder, &item->kind));
  MOZ_TRY(CodePod(coder, &item->tableIndex));
  MOZ_TRY(CodeRefType(coder, &item->elemType));
  MOZ_TRY((CodeMaybe<mode, InitExpr, &CodeInitExpr<mode>>(
      coder, &item->offsetIfActive)));
  MOZ_TRY(CodePod(coder, &item->encoding));
  MOZ_TRY(CodePodVector(coder, &item->elemIndices));
  MOZ_TRY(CodePod(coder, &item->elemExpressions.count));
  MOZ_TRY(CodePodVector(coder, &item->elemExpressions.exprBytes));
  return Ok();
}

template <CoderMode mode>
CoderResult CodeDataSegment(Coder<mode>& coder,
                            CoderArg<mode, DataSegment> item) {
  WASM_VERIFY_SERIALIZATION_FOR_SIZE(wasm::DataSegment, 144);
  MOZ_TRY(CodePod(coder, &item->memoryIndex));
  MOZ_TRY((CodeMaybe<mode, InitExpr, &CodeInitExpr<mode>>(
      coder, &item->offsetIfActive)));
  MOZ_TRY(CodePodVector(coder, &item->bytes));
  return Ok();
}

template <CoderMode mode>
CoderResult CodeCustomSection(Coder<mode>& coder,
                              CoderArg<mode, CustomSection> item) {
  WASM_VERIFY_SERIALIZATION_FOR_SIZE(wasm::CustomSection, 48);
  MOZ_TRY(CodePodVector(coder, &item->name));
  MOZ_TRY((CodeRefPtr<mode, const ShareableBytes, &CodeShareableBytes<mode>>(
      coder, &item->payload)));
  return Ok();
}

template <CoderMode mode>
CoderResult CodeTableDesc(Coder<mode>& coder, CoderArg<mode, TableDesc> item) {
  WASM_VERIFY_SERIALIZATION_FOR_SIZE(wasm::TableDesc, 144);
  MOZ_TRY(CodeRefType(coder, &item->elemType));
  MOZ_TRY(CodePod(coder, &item->isImported));
  MOZ_TRY(CodePod(coder, &item->isExported));
  MOZ_TRY(CodePod(coder, &item->isAsmJS));
  MOZ_TRY(CodePod(coder, &item->limits));
  MOZ_TRY(
      (CodeMaybe<mode, InitExpr, &CodeInitExpr<mode>>(coder, &item->initExpr)));
  return Ok();
}

// WasmCodegenTypes.h

template <CoderMode mode>
CoderResult CodeTrapSiteVectorArray(Coder<mode>& coder,
                                    CoderArg<mode, TrapSiteVectorArray> item) {
  WASM_VERIFY_SERIALIZATION_FOR_SIZE(wasm::TrapSiteVectorArray, 560);
  for (Trap trap : mozilla::MakeEnumeratedRange(Trap::Limit)) {
    MOZ_TRY(CodePodVector(coder, &(*item)[trap]));
  }
  return Ok();
}

// WasmCompileArgs.h

template <CoderMode mode>
CoderResult CodeScriptedCaller(Coder<mode>& coder,
                               CoderArg<mode, ScriptedCaller> item) {
  WASM_VERIFY_SERIALIZATION_FOR_SIZE(wasm::ScriptedCaller, 16);
  MOZ_TRY((CodeUniqueChars(coder, &item->filename)));
  MOZ_TRY((CodePod(coder, &item->filenameIsURL)));
  MOZ_TRY((CodePod(coder, &item->line)));
  return Ok();
}

template <CoderMode mode>
CoderResult CodeCompileArgs(Coder<mode>& coder,
                            CoderArg<mode, CompileArgs> item) {
  WASM_VERIFY_SERIALIZATION_FOR_SIZE(wasm::CompileArgs, 64);
  MOZ_TRY((CodeScriptedCaller(coder, &item->scriptedCaller)));
  MOZ_TRY((CodeUniqueChars(coder, &item->sourceMapURL)));
  MOZ_TRY((CodePod(coder, &item->baselineEnabled)));
  MOZ_TRY((CodePod(coder, &item->ionEnabled)));
  MOZ_TRY((CodePod(coder, &item->debugEnabled)));
  MOZ_TRY((CodePod(coder, &item->forceTiering)));
  return Ok();
}

// WasmGC.h

CoderResult CodeStackMap(Coder<MODE_DECODE>& coder,
                         CoderArg<MODE_DECODE, wasm::StackMap*> item) {
  WASM_VERIFY_SERIALIZATION_FOR_SIZE(wasm::StackMap, 12);
  // Decode the stack map header
  StackMapHeader header;
  MOZ_TRY(CodePod(coder, &header));

  // Allocate a stack map for the header
  StackMap* map = StackMap::create(header);
  if (!map) {
    return Err(OutOfMemory());
  }

  // Decode the bitmap into the stackmap
  MOZ_TRY(coder.readBytes(map->rawBitmap(), map->rawBitmapLengthInBytes()));

  *item = map;
  return Ok();
}

template <CoderMode mode>
CoderResult CodeStackMap(Coder<mode>& coder,
                         CoderArg<mode, wasm::StackMap> item) {
  WASM_VERIFY_SERIALIZATION_FOR_SIZE(wasm::StackMap, 12);
  STATIC_ASSERT_ENCODING_OR_SIZING;

  // Encode the stackmap header
  MOZ_TRY(CodePod(coder, &item->header));

  // Encode the stackmap bitmap
  MOZ_TRY(coder.writeBytes(item->rawBitmap(), item->rawBitmapLengthInBytes()));

  return Ok();
}

static inline uint32_t ComputeCodeOffset(const uint8_t* codeStart,
                                         const uint8_t* codePtr) {
  MOZ_RELEASE_ASSERT(codePtr >= codeStart);
#ifdef JS_64BIT
  MOZ_RELEASE_ASSERT(codePtr < codeStart + UINT32_MAX);
#endif
  return (uint32_t)(codePtr - codeStart);
}

CoderResult CodeStackMaps(Coder<MODE_DECODE>& coder,
                          CoderArg<MODE_DECODE, wasm::StackMaps> item,
                          const uint8_t* codeStart) {
  WASM_VERIFY_SERIALIZATION_FOR_SIZE(wasm::StackMaps, 48);
  // Decode the amount of stack maps
  size_t length;
  MOZ_TRY(CodePod(coder, &length));

  for (size_t i = 0; i < length; i++) {
    // Decode the offset relative to codeStart
    uint32_t codeOffset;
    MOZ_TRY(CodePod(coder, &codeOffset));

    // Decode the stack map
    StackMap* map;
    MOZ_TRY(CodeStackMap(coder, &map));

    // Add it to the map
    const uint8_t* nextInsnAddr = codeStart + codeOffset;
    if (!item->add(nextInsnAddr, map)) {
      return Err(OutOfMemory());
    }
  }

  // Finish the maps, asserting they are sorted
  item->finishAlreadySorted();
  return Ok();
}

template <CoderMode mode>
CoderResult CodeStackMaps(Coder<mode>& coder,
                          CoderArg<mode, wasm::StackMaps> item,
                          const uint8_t* codeStart) {
  WASM_VERIFY_SERIALIZATION_FOR_SIZE(wasm::StackMaps, 48);
  STATIC_ASSERT_ENCODING_OR_SIZING;

  // Encode the amount of stack maps
  size_t length = item->length();
  MOZ_TRY(CodePod(coder, &length));

  for (size_t i = 0; i < length; i++) {
    StackMaps::Maplet maplet = item->get(i);
    uint32_t codeOffset = ComputeCodeOffset(codeStart, maplet.nextInsnAddr);

    // Encode the offset relative to codeStart
    MOZ_TRY(CodePod(coder, &codeOffset));

    // Encode the stack map
    MOZ_TRY(CodeStackMap(coder, maplet.map));
  }
  return Ok();
}

// WasmCode.h

template <CoderMode mode>
CoderResult CodeSymbolicLinkArray(
    Coder<mode>& coder,
    CoderArg<mode, wasm::LinkData::SymbolicLinkArray> item) {
  for (SymbolicAddress address :
       mozilla::MakeEnumeratedRange(SymbolicAddress::Limit)) {
    MOZ_TRY(CodePodVector(coder, &(*item)[address]));
  }
  return Ok();
}

template <CoderMode mode>
CoderResult CodeLinkData(Coder<mode>& coder,
                         CoderArg<mode, wasm::LinkData> item) {
  // SymbolicLinkArray depends on SymbolicAddress::Limit, which is changed
  // often. Exclude symbolicLinks field from trip wire value calculation.
  WASM_VERIFY_SERIALIZATION_FOR_SIZE(
      wasm::LinkData, 88 + sizeof(wasm::LinkData::SymbolicLinkArray));
  MOZ_TRY(CodePod(coder, &item->pod()));
  MOZ_TRY(CodePodVector(coder, &item->internalLinks));
  MOZ_TRY(CodePodVector(coder, &item->callFarJumps));
  MOZ_TRY(CodeSymbolicLinkArray(coder, &item->symbolicLinks));
  return Ok();
}

CoderResult CodeCodeSegment(Coder<MODE_DECODE>& coder,
                            wasm::SharedCodeSegment* item,
                            const wasm::LinkData& linkData) {
  WASM_VERIFY_SERIALIZATION_FOR_SIZE(wasm::CodeSegment, 40);
  // Assert we're decoding a CodeSegment
  MOZ_TRY(Magic(coder, Marker::CodeSegment));

  // Decode the code bytes length
  size_t length;
  MOZ_TRY(CodePod(coder, &length));

  // Decode the code bytes
  const uint8_t* codeBytes;
  MOZ_TRY(coder.readBytesRef(length, &codeBytes));

  // Initialize the CodeSegment
  *item = CodeSegment::createFromBytes(codeBytes, length, linkData);
  if (!*item) {
    return Err(OutOfMemory());
  }
  return Ok();
}

template <CoderMode mode>
CoderResult CodeCodeSegment(Coder<mode>& coder,
                            CoderArg<mode, wasm::SharedCodeSegment> item,
                            const wasm::LinkData& linkData) {
  WASM_VERIFY_SERIALIZATION_FOR_SIZE(wasm::CodeSegment, 40);
  STATIC_ASSERT_ENCODING_OR_SIZING;

  // Mark that we're encoding a CodeSegment
  MOZ_TRY(Magic(coder, Marker::CodeSegment));

  // Encode the length
  size_t length = (*item)->lengthBytes();
  MOZ_TRY(CodePod(coder, &length));

  if constexpr (mode == MODE_SIZE) {
    // Just calculate the length of bytes written
    MOZ_TRY(coder.writeBytes((*item)->base(), length));
  } else {
    // Get the start of where the code bytes will be written
    uint8_t* serializedBase = coder.buffer_;

    // Write the code bytes
    MOZ_TRY(coder.writeBytes((*item)->base(), length));

    // Unlink the code bytes written to the buffer
    StaticallyUnlink(serializedBase, linkData);
  }
  return Ok();
}

// WasmMetadata.h

template <CoderMode mode>
CoderResult CodeCodeMetadata(Coder<mode>& coder,
                             CoderArg<mode, wasm::CodeMetadata> item) {
  // NOTE: keep the field sequence here in sync with the sequence in the
  // declaration of CodeMetadata.

  WASM_VERIFY_SERIALIZATION_FOR_SIZE(wasm::CodeMetadata, 664);
  // Serialization doesn't handle asm.js or debug enabled modules
  MOZ_RELEASE_ASSERT(mode == MODE_SIZE || !item->isAsmJS());
  if constexpr (mode == MODE_ENCODE) {
    MOZ_ASSERT(!item->debugEnabled);
  }

  MOZ_TRY(Magic(coder, Marker::CodeMetadata));
  // not serialized: kind
  MOZ_TRY((CodeRefPtr<mode, const CompileArgs, &CodeCompileArgs>(
      coder, &item->compileArgs)));
  // not serialized: dataCount
  MOZ_TRY(CodePodVector(coder, &item->memories));
  // Types go in relatively early, because deserialisation of various other
  // fields (globals, at least) depends on types having been deserialised
  // first.
  MOZ_TRY(
      (CodeRefPtr<mode, TypeContext, &CodeTypeContext>(coder, &item->types)));
  // not serialized: branchHints
  MOZ_TRY(CodePod(coder, &item->numFuncImports));
  MOZ_TRY(CodePod(coder, &item->numGlobalImports));
  MOZ_TRY((CodeVector<mode, GlobalDesc, &CodeGlobalDesc<mode>>(
      coder, &item->globals)));
  MOZ_TRY((CodeVector<mode, TagDesc, &CodeTagDesc<mode>>(coder, &item->tags)));
  MOZ_TRY((
      CodeVector<mode, TableDesc, &CodeTableDesc<mode>>(coder, &item->tables)));
  MOZ_TRY(CodePod(coder, &item->funcImportsOffsetStart));
  MOZ_TRY(CodePod(coder, &item->typeDefsOffsetStart));
  MOZ_TRY(CodePod(coder, &item->memoriesOffsetStart));
  MOZ_TRY(CodePod(coder, &item->tablesOffsetStart));
  MOZ_TRY(CodePod(coder, &item->tagsOffsetStart));
  MOZ_TRY(CodePod(coder, &item->instanceDataLength));
  // not serialized: namePayload
  MOZ_TRY(CodePod(coder, &item->moduleName));
  MOZ_TRY(CodePodVector(coder, &item->funcNames));
  MOZ_TRY((CodeMaybe<mode, uint32_t, &CodePod>(coder, &item->startFuncIndex)));
  MOZ_TRY((CodeMaybe<mode, uint32_t, &CodePod>(coder,
                                               &item->nameCustomSectionIndex)));
  MOZ_TRY(CodePodVector(coder, &item->funcs));
  // not serialized: elemSegmentTypes
  // not serialized: asmJSSigToTableIndex
  // not serialized: codeSection
  // not serialized: customSectionRanges
  MOZ_TRY(CodePodVector(coder, &item->funcDefRanges));
  MOZ_TRY(CodePod(coder, &item->parsedBranchHints));
  if constexpr (mode == MODE_DECODE) {
    // Initialize debugging state to disabled
    item->debugEnabled = false;
    MOZ_ASSERT(!item->isAsmJS());
  }
  // not serialized: debugHash

  return Ok();
}

template <CoderMode mode>
CoderResult CodeModuleMetadata(Coder<mode>& coder,
                               CoderArg<mode, wasm::ModuleMetadata> item) {
  // NOTE: keep the field sequence here in sync with the sequence in the
  // declaration of ModuleMetadata.

  WASM_VERIFY_SERIALIZATION_FOR_SIZE(wasm::ModuleMetadata, 264);
  MOZ_TRY(Magic(coder, Marker::ModuleMetadata));

  MOZ_TRY((CodeRefPtr<mode, CodeMetadata, &CodeCodeMetadata>(coder,
                                                             &item->codeMeta)));
  MOZ_TRY(Magic(coder, Marker::Imports));
  MOZ_TRY((CodeVector<mode, Import, &CodeImport<mode>>(coder, &item->imports)));
  MOZ_TRY(Magic(coder, Marker::Exports));
  MOZ_TRY((CodeVector<mode, Export, &CodeExport<mode>>(coder, &item->exports)));
  MOZ_TRY(Magic(coder, Marker::ElemSegments));
  MOZ_TRY((CodeVector<mode, ModuleElemSegment, CodeModuleElemSegment<mode>>(
      coder, &item->elemSegments)));
  // not serialized: dataSegmentRanges
  MOZ_TRY(Magic(coder, Marker::DataSegments));
  MOZ_TRY(
      (CodeVector<mode, SharedDataSegment,
                  &CodeRefPtr<mode, const DataSegment, CodeDataSegment<mode>>>(
          coder, &item->dataSegments)));
  MOZ_TRY(Magic(coder, Marker::CustomSections));
  MOZ_TRY((CodeVector<mode, CustomSection, &CodeCustomSection<mode>>(
      coder, &item->customSections)));
  MOZ_TRY(CodePod(coder, &item->featureUsage));
  return Ok();
}

// WasmCode.h

template <CoderMode mode>
CoderResult CodeFuncToCodeRangeMap(
    Coder<mode>& coder, CoderArg<mode, wasm::FuncToCodeRangeMap> item) {
  WASM_VERIFY_SERIALIZATION_FOR_SIZE(wasm::FuncToCodeRangeMap, 80);
  MOZ_TRY(CodePod(coder, &item->startFuncIndex_));
  MOZ_TRY(CodePodVector(coder, &item->funcToCodeRange_));
  return Ok();
}

CoderResult CodeCodeBlock(Coder<MODE_DECODE>& coder,
                          wasm::UniqueCodeBlock* item,
                          const wasm::LinkData& linkData) {
  WASM_VERIFY_SERIALIZATION_FOR_SIZE(wasm::CodeBlock, 944);
  *item = js::MakeUnique<CodeBlock>(CodeBlock::kindFromTier(Tier::Serialized));
  if (!*item) {
    return Err(OutOfMemory());
  }
  MOZ_TRY(Magic(coder, Marker::CodeBlock));
  SharedCodeSegment codeSegment;
  MOZ_TRY(CodeCodeSegment(coder, &codeSegment, linkData));
  (*item)->segment = codeSegment;
  (*item)->codeBase = codeSegment->base();
  (*item)->codeLength = codeSegment->lengthBytes();
  MOZ_TRY(CodeFuncToCodeRangeMap(coder, &(*item)->funcToCodeRange));
  MOZ_TRY(CodePodVector(coder, &(*item)->codeRanges));
  MOZ_TRY(CodePodVector(coder, &(*item)->callSites));
  MOZ_TRY(CodeTrapSiteVectorArray(coder, &(*item)->trapSites));
  MOZ_TRY(CodePodVector(coder, &(*item)->funcExports));
  MOZ_TRY(CodeStackMaps(coder, &(*item)->stackMaps, (*item)->segment->base()));
  MOZ_TRY(CodePodVector(coder, &(*item)->tryNotes));
  MOZ_TRY(CodePodVector(coder, &(*item)->codeRangeUnwindInfos));
  return Ok();
}

template <CoderMode mode>
CoderResult CodeCodeBlock(Coder<mode>& coder,
                          CoderArg<mode, wasm::CodeBlock> item,
                          const wasm::LinkData& linkData) {
  WASM_VERIFY_SERIALIZATION_FOR_SIZE(wasm::CodeBlock, 944);
  STATIC_ASSERT_ENCODING_OR_SIZING;
  MOZ_TRY(Magic(coder, Marker::CodeBlock));
  // We don't support serializing sub-ranges yet. These only happen with
  // lazy stubs, which we don't serialize.
  MOZ_ASSERT(item->codeBase == item->segment->base() &&
             item->codeLength == item->segment->lengthBytes());
  MOZ_TRY(CodeCodeSegment(coder, &item->segment, linkData));
  MOZ_TRY(CodeFuncToCodeRangeMap(coder, &item->funcToCodeRange));
  MOZ_TRY(CodePodVector(coder, &item->codeRanges));
  MOZ_TRY(CodePodVector(coder, &item->callSites));
  MOZ_TRY(CodeTrapSiteVectorArray(coder, &item->trapSites));
  MOZ_TRY(CodePodVector(coder, &item->funcExports));
  MOZ_TRY(CodeStackMaps(coder, &item->stackMaps, item->segment->base()));
  MOZ_TRY(CodePodVector(coder, &item->tryNotes));
  MOZ_TRY(CodePodVector(coder, &item->codeRangeUnwindInfos));
  return Ok();
}

CoderResult CodeSharedCode(Coder<MODE_DECODE>& coder, wasm::SharedCode* item,
                           const wasm::CodeMetadata& codeMeta) {
  WASM_VERIFY_SERIALIZATION_FOR_SIZE(wasm::Code, 760);

  FuncImportVector funcImports;
  MOZ_TRY(CodePodVector(coder, &funcImports));

  UniqueCodeBlock sharedStubs;
  UniqueLinkData sharedStubsLinkData;
  MOZ_TRY((CodeUniquePtr<MODE_DECODE, LinkData, CodeLinkData>(
      coder, &sharedStubsLinkData)));
  MOZ_TRY(CodeCodeBlock(coder, &sharedStubs, *sharedStubsLinkData));
  UniqueLinkData optimizedCodeLinkData;
  UniqueCodeBlock optimizedCode;
  MOZ_TRY((CodeUniquePtr<MODE_DECODE, LinkData, CodeLinkData>(
      coder, &optimizedCodeLinkData)));
  MOZ_TRY(CodeCodeBlock(coder, &optimizedCode, *optimizedCodeLinkData));

  // Create and initialize the code
  MutableCode code =
      js_new<Code>(CompileMode::Once, codeMeta, /*codeMetaForAsmJS=*/nullptr,
                   /*maybeBytecode=*/nullptr);
  if (!code || !code->initialize(std::move(funcImports), std::move(sharedStubs),
                                 std::move(sharedStubsLinkData),
                                 std::move(optimizedCode),
                                 std::move(optimizedCodeLinkData))) {
    return Err(OutOfMemory());
  }
  *item = code;
  return Ok();
}

template <CoderMode mode>
CoderResult CodeSharedCode(Coder<mode>& coder,
                           CoderArg<mode, wasm::SharedCode> item) {
  WASM_VERIFY_SERIALIZATION_FOR_SIZE(wasm::Code, 760);
  STATIC_ASSERT_ENCODING_OR_SIZING;
  // Don't encode the CodeMetadata, that is handled by wasm::Module
  MOZ_TRY(CodePodVector(coder, &(*item)->funcImports()));
  const CodeBlock& sharedStubsCodeBlock = (*item)->sharedStubs();
  const LinkData& sharedStubsLinkData =
      *(*item)->codeBlockLinkData(sharedStubsCodeBlock);
  MOZ_TRY(CodeLinkData(coder, &sharedStubsLinkData));
  MOZ_TRY(CodeCodeBlock(coder, &sharedStubsCodeBlock, sharedStubsLinkData));
  const CodeBlock& optimizedCodeBlock =
      (*item)->completeTierCodeBlock(Tier::Serialized);
  const LinkData& optimizedLinkData =
      *(*item)->codeBlockLinkData(optimizedCodeBlock);
  MOZ_TRY(CodeLinkData(coder, &optimizedLinkData));
  MOZ_TRY(CodeCodeBlock(coder, &optimizedCodeBlock, optimizedLinkData));
  return Ok();
}

// WasmModule.h

CoderResult CodeModule(Coder<MODE_DECODE>& coder, MutableModule* item) {
  WASM_VERIFY_SERIALIZATION_FOR_SIZE(wasm::Module, 56);
  JS::BuildIdCharVector currentBuildId;
  if (!GetOptimizedEncodingBuildId(&currentBuildId)) {
    return Err(OutOfMemory());
  }
  JS::BuildIdCharVector deserializedBuildId;
  MOZ_TRY(CodePodVector(coder, &deserializedBuildId));

  MOZ_RELEASE_ASSERT(EqualContainers(currentBuildId, deserializedBuildId));

  MutableModuleMetadata moduleMeta;
  MOZ_TRY((CodeRefPtr<MODE_DECODE, ModuleMetadata, &CodeModuleMetadata>(
      coder, &moduleMeta)));

  SharedCode code;
  MOZ_TRY(Magic(coder, Marker::Code));
  MOZ_TRY(CodeSharedCode(coder, &code, *moduleMeta->codeMeta));

  // Initialize metadata's name payload from the custom section.  This
  // requires the kludge that CodeMetadata::namePayload is marked `mutable`.
  if (code->codeMeta().nameCustomSectionIndex) {
    code->codeMeta().namePayload =
        moduleMeta->customSections[*code->codeMeta().nameCustomSectionIndex]
            .payload;
  } else {
    MOZ_RELEASE_ASSERT(!code->codeMeta().moduleName);
    MOZ_RELEASE_ASSERT(code->codeMeta().funcNames.empty());
  }

  *item = js_new<Module>(*moduleMeta, *code,
                         /* loggingDeserialized = */ true);
  return Ok();
}

template <CoderMode mode>
CoderResult CodeModule(Coder<mode>& coder, CoderArg<mode, Module> item) {
  WASM_VERIFY_SERIALIZATION_FOR_SIZE(wasm::Module, 56);
  STATIC_ASSERT_ENCODING_OR_SIZING;
  MOZ_RELEASE_ASSERT(!item->codeMeta().debugEnabled);
  MOZ_RELEASE_ASSERT(item->code_->hasCompleteTier(Tier::Serialized));

  JS::BuildIdCharVector currentBuildId;
  if (!GetOptimizedEncodingBuildId(&currentBuildId)) {
    return Err(OutOfMemory());
  }
  MOZ_TRY(CodePodVector(coder, &currentBuildId));
  MOZ_TRY((CodeRefPtr<mode, const ModuleMetadata, &CodeModuleMetadata>(
      coder, &item->moduleMeta_)));
  MOZ_TRY(Magic(coder, Marker::Code));
  MOZ_TRY(CodeSharedCode(coder, &item->code_));
  return Ok();
}

}  // namespace wasm
}  // namespace js

static bool GetSerializedSize(const Module& module, size_t* size) {
  Coder<MODE_SIZE> coder(module.codeMeta().types.get());
  auto result = CodeModule(coder, &module);
  if (result.isErr()) {
    return false;
  }
  *size = coder.size_.value();
  return true;
}

bool Module::serialize(Bytes* bytes) const {
  MOZ_RELEASE_ASSERT(!codeMeta().debugEnabled);
  MOZ_RELEASE_ASSERT(code_->hasCompleteTier(Tier::Serialized));

  size_t serializedSize;
  if (!GetSerializedSize(*this, &serializedSize)) {
    // An error is an overflow, return false
    return false;
  }

  // Try to allocate the destination buffer
  if (!bytes->resizeUninitialized(serializedSize)) {
    return false;
  }

  Coder<MODE_ENCODE> coder(codeMeta().types.get(), bytes->begin(),
                           serializedSize);
  CoderResult result = CodeModule(coder, this);
  if (result.isErr()) {
    // An error is an OOM, return false
    return false;
  }
  // Every byte is accounted for
  MOZ_RELEASE_ASSERT(coder.buffer_ == coder.end_);

  // Clear out link data now, it's no longer needed.
  code().clearLinkData();

  return true;
}

/* static */
MutableModule Module::deserialize(const uint8_t* begin, size_t size) {
  Coder<MODE_DECODE> coder(begin, size);
  MutableModule module;
  CoderResult result = CodeModule(coder, &module);
  if (result.isErr()) {
    // An error is an OOM, return nullptr
    return nullptr;
  }
  // Every byte is accounted for
  MOZ_RELEASE_ASSERT(coder.buffer_ == coder.end_);
  return module;
}

void Module::initGCMallocBytesExcludingCode() {
  // The size doesn't have to be exact so use the serialization framework to
  // calculate a value. We consume all errors, as they can only be overflow and
  // can be ignored until the end.
  constexpr CoderMode MODE = MODE_SIZE;
  Coder<MODE> coder(codeMeta().types.get());

  // Add the size of the ModuleMetadata
  (void)CodeModuleMetadata<MODE>(coder, moduleMeta_);
  // .. and the size of the CodeMetadata, but not of the Code itself
  if (!code().codeMeta().isAsmJS()) {
    (void)CodeCodeMetadata<MODE>(coder, &code().codeMeta());
  }

  // Overflow really shouldn't be possible here, but handle it anyways.
  size_t serializedSize = coder.size_.isValid() ? coder.size_.value() : 0;
  gcMallocBytesExcludingCode_ = sizeof(*this) + serializedSize;
}
