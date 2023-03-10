// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/builtins/builtins-utils.h"
#include "src/builtins/builtins.h"
#include "src/conversions.h"
#include "src/counters.h"
#include "src/factory.h"
#include "src/isolate.h"
#include "src/objects-inl.h"

namespace v8 {
namespace internal {

// -----------------------------------------------------------------------------
// ES #sec-dataview-objects

// ES #sec-dataview-constructor
BUILTIN(DataViewConstructor) {
  HandleScope scope(isolate);
  if (args.new_target()->IsUndefined(isolate)) {  // [[Call]]
    THROW_NEW_ERROR_RETURN_FAILURE(
        isolate, NewTypeError(MessageTemplate::kConstructorNotFunction,
                              isolate->factory()->NewStringFromAsciiChecked(
                                  "DataView")));
  } else {  // [[Construct]]
    Handle<JSFunction> target = args.target();
    Handle<JSReceiver> new_target = Handle<JSReceiver>::cast(args.new_target());
    Handle<Object> buffer = args.atOrUndefined(isolate, 1);
    Handle<Object> byte_offset = args.atOrUndefined(isolate, 2);
    Handle<Object> byte_length = args.atOrUndefined(isolate, 3);

    // 2. If Type(buffer) is not Object, throw a TypeError exception.
    // 3. If buffer does not have an [[ArrayBufferData]] internal slot, throw a
    //    TypeError exception.
    if (!buffer->IsJSArrayBuffer()) {
      THROW_NEW_ERROR_RETURN_FAILURE(
          isolate, NewTypeError(MessageTemplate::kDataViewNotArrayBuffer));
    }
    Handle<JSArrayBuffer> array_buffer = Handle<JSArrayBuffer>::cast(buffer);

    // 4. Let offset be ? ToIndex(byteOffset).
    Handle<Object> offset;
    ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
        isolate, offset,
        Object::ToIndex(isolate, byte_offset, MessageTemplate::kInvalidOffset));

    // 5. If IsDetachedBuffer(buffer) is true, throw a TypeError exception.
    //    We currently violate the specification at this point. TODO: Fix that.

    // 6. Let bufferByteLength be the value of buffer's
    // [[ArrayBufferByteLength]] internal slot.
    double const buffer_byte_length = array_buffer->byte_length()->Number();

    // 7. If offset > bufferByteLength, throw a RangeError exception.
    if (offset->Number() > buffer_byte_length) {
      THROW_NEW_ERROR_RETURN_FAILURE(
          isolate, NewRangeError(MessageTemplate::kInvalidOffset, offset));
    }

    Handle<Object> view_byte_length;
    if (byte_length->IsUndefined(isolate)) {
      // 8. If byteLength is either not present or undefined, then
      //       a. Let viewByteLength be bufferByteLength - offset.
      view_byte_length =
          isolate->factory()->NewNumber(buffer_byte_length - offset->Number());
    } else {
      // 9. Else,
      //       a. Let viewByteLength be ? ToIndex(byteLength).
      //       b. If offset+viewByteLength > bufferByteLength, throw a
      //          RangeError exception.
      ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
          isolate, view_byte_length,
          Object::ToIndex(isolate, byte_length,
                          MessageTemplate::kInvalidDataViewLength));
      if (offset->Number() + view_byte_length->Number() > buffer_byte_length) {
        THROW_NEW_ERROR_RETURN_FAILURE(
            isolate, NewRangeError(MessageTemplate::kInvalidDataViewLength));
      }
    }

    // 10. Let O be ? OrdinaryCreateFromConstructor(NewTarget,
    //     "%DataViewPrototype%", ??[[DataView]], [[ViewedArrayBuffer]],
    //     [[ByteLength]], [[ByteOffset]]??).
    Handle<JSObject> result;
    ASSIGN_RETURN_FAILURE_ON_EXCEPTION(isolate, result,
                                       JSObject::New(target, new_target));
    for (int i = 0; i < ArrayBufferView::kEmbedderFieldCount; ++i) {
      Handle<JSDataView>::cast(result)->SetEmbedderField(i, Smi::kZero);
    }

    // 11. Set O's [[ViewedArrayBuffer]] internal slot to buffer.
    Handle<JSDataView>::cast(result)->set_buffer(*array_buffer);

    // 12. Set O's [[ByteLength]] internal slot to viewByteLength.
    Handle<JSDataView>::cast(result)->set_byte_length(*view_byte_length);

    // 13. Set O's [[ByteOffset]] internal slot to offset.
    Handle<JSDataView>::cast(result)->set_byte_offset(*offset);

    // 14. Return O.
    return *result;
  }
}

// ES6 section 24.2.4.1 get DataView.prototype.buffer
BUILTIN(DataViewPrototypeGetBuffer) {
  HandleScope scope(isolate);
  CHECK_RECEIVER(JSDataView, data_view, "get DataView.prototype.buffer");
  return data_view->buffer();
}

// ES6 section 24.2.4.2 get DataView.prototype.byteLength
BUILTIN(DataViewPrototypeGetByteLength) {
  HandleScope scope(isolate);
  CHECK_RECEIVER(JSDataView, data_view, "get DataView.prototype.byteLength");
  // TODO(bmeurer): According to the ES6 spec, we should throw a TypeError
  // here if the JSArrayBuffer of the {data_view} was neutered.
  return data_view->byte_length();
}

// ES6 section 24.2.4.3 get DataView.prototype.byteOffset
BUILTIN(DataViewPrototypeGetByteOffset) {
  HandleScope scope(isolate);
  CHECK_RECEIVER(JSDataView, data_view, "get DataView.prototype.byteOffset");
  // TODO(bmeurer): According to the ES6 spec, we should throw a TypeError
  // here if the JSArrayBuffer of the {data_view} was neutered.
  return data_view->byte_offset();
}

namespace {

bool NeedToFlipBytes(bool is_little_endian) {
#ifdef V8_TARGET_LITTLE_ENDIAN
  return !is_little_endian;
#else
  return is_little_endian;
#endif
}

template <size_t n>
void CopyBytes(uint8_t* target, uint8_t const* source) {
  for (size_t i = 0; i < n; i++) {
    *(target++) = *(source++);
  }
}

template <size_t n>
void FlipBytes(uint8_t* target, uint8_t const* source) {
  source = source + (n - 1);
  for (size_t i = 0; i < n; i++) {
    *(target++) = *(source--);
  }
}

template <typename T>
MaybeHandle<Object> AllocateResult(Isolate* isolate, T value) {
  return isolate->factory()->NewNumber(value);
}

template <>
MaybeHandle<Object> AllocateResult(Isolate* isolate, int64_t value) {
  return BigInt::FromInt64(isolate, value);
}

template <>
MaybeHandle<Object> AllocateResult(Isolate* isolate, uint64_t value) {
  return BigInt::FromUint64(isolate, value);
}

// ES6 section 24.2.1.1 GetViewValue (view, requestIndex, isLittleEndian, type)
template <typename T>
MaybeHandle<Object> GetViewValue(Isolate* isolate, Handle<JSDataView> data_view,
                                 Handle<Object> request_index,
                                 bool is_little_endian, const char* method) {
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, request_index,
      Object::ToIndex(isolate, request_index,
                      MessageTemplate::kInvalidDataViewAccessorOffset),
      Object);
  size_t get_index = 0;
  if (!TryNumberToSize(*request_index, &get_index)) {
    THROW_NEW_ERROR(
        isolate, NewRangeError(MessageTemplate::kInvalidDataViewAccessorOffset),
        Object);
  }
  Handle<JSArrayBuffer> buffer(JSArrayBuffer::cast(data_view->buffer()),
                               isolate);
  if (buffer->was_neutered()) {
    Handle<String> operation =
        isolate->factory()->NewStringFromAsciiChecked(method);
    THROW_NEW_ERROR(
        isolate, NewTypeError(MessageTemplate::kDetachedOperation, operation),
        Object);
  }
  size_t const data_view_byte_offset = NumberToSize(data_view->byte_offset());
  size_t const data_view_byte_length = NumberToSize(data_view->byte_length());
  if (get_index + sizeof(T) > data_view_byte_length ||
      get_index + sizeof(T) < get_index) {  // overflow
    THROW_NEW_ERROR(
        isolate, NewRangeError(MessageTemplate::kInvalidDataViewAccessorOffset),
        Object);
  }
  union {
    T data;
    uint8_t bytes[sizeof(T)];
  } v;
  size_t const buffer_offset = data_view_byte_offset + get_index;
  DCHECK_GE(NumberToSize(buffer->byte_length()), buffer_offset + sizeof(T));
  uint8_t const* const source =
      static_cast<uint8_t*>(buffer->backing_store()) + buffer_offset;
  if (NeedToFlipBytes(is_little_endian)) {
    FlipBytes<sizeof(T)>(v.bytes, source);
  } else {
    CopyBytes<sizeof(T)>(v.bytes, source);
  }
  return AllocateResult<T>(isolate, v.data);
}

template <typename T>
MaybeHandle<Object> DataViewConvertInput(Isolate* isolate,
                                         Handle<Object> input) {
  return Object::ToNumber(input);
}

template <>
MaybeHandle<Object> DataViewConvertInput<int64_t>(Isolate* isolate,
                                                  Handle<Object> input) {
  return BigInt::FromObject(isolate, input);
}

template <>
MaybeHandle<Object> DataViewConvertInput<uint64_t>(Isolate* isolate,
                                                   Handle<Object> input) {
  return BigInt::FromObject(isolate, input);
}

template <typename T>
T DataViewConvertValue(Handle<Object> value);

template <>
int8_t DataViewConvertValue<int8_t>(Handle<Object> value) {
  return static_cast<int8_t>(DoubleToInt32(value->Number()));
}

template <>
int16_t DataViewConvertValue<int16_t>(Handle<Object> value) {
  return static_cast<int16_t>(DoubleToInt32(value->Number()));
}

template <>
int32_t DataViewConvertValue<int32_t>(Handle<Object> value) {
  return DoubleToInt32(value->Number());
}

template <>
uint8_t DataViewConvertValue<uint8_t>(Handle<Object> value) {
  return static_cast<uint8_t>(DoubleToUint32(value->Number()));
}

template <>
uint16_t DataViewConvertValue<uint16_t>(Handle<Object> value) {
  return static_cast<uint16_t>(DoubleToUint32(value->Number()));
}

template <>
uint32_t DataViewConvertValue<uint32_t>(Handle<Object> value) {
  return DoubleToUint32(value->Number());
}

template <>
float DataViewConvertValue<float>(Handle<Object> value) {
  return static_cast<float>(value->Number());
}

template <>
double DataViewConvertValue<double>(Handle<Object> value) {
  return value->Number();
}

template <>
int64_t DataViewConvertValue<int64_t>(Handle<Object> value) {
  return BigInt::cast(*value)->AsInt64();
}

template <>
uint64_t DataViewConvertValue<uint64_t>(Handle<Object> value) {
  return BigInt::cast(*value)->AsUint64();
}

// ES6 section 24.2.1.2 SetViewValue (view, requestIndex, isLittleEndian, type,
//                                    value)
template <typename T>
MaybeHandle<Object> SetViewValue(Isolate* isolate, Handle<JSDataView> data_view,
                                 Handle<Object> request_index,
                                 bool is_little_endian, Handle<Object> value,
                                 const char* method) {
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, request_index,
      Object::ToIndex(isolate, request_index,
                      MessageTemplate::kInvalidDataViewAccessorOffset),
      Object);
  ASSIGN_RETURN_ON_EXCEPTION(isolate, value,
                             DataViewConvertInput<T>(isolate, value), Object);
  size_t get_index = 0;
  if (!TryNumberToSize(*request_index, &get_index)) {
    THROW_NEW_ERROR(
        isolate, NewRangeError(MessageTemplate::kInvalidDataViewAccessorOffset),
        Object);
  }
  Handle<JSArrayBuffer> buffer(JSArrayBuffer::cast(data_view->buffer()),
                               isolate);
  if (buffer->was_neutered()) {
    Handle<String> operation =
        isolate->factory()->NewStringFromAsciiChecked(method);
    THROW_NEW_ERROR(
        isolate, NewTypeError(MessageTemplate::kDetachedOperation, operation),
        Object);
  }
  size_t const data_view_byte_offset = NumberToSize(data_view->byte_offset());
  size_t const data_view_byte_length = NumberToSize(data_view->byte_length());
  if (get_index + sizeof(T) > data_view_byte_length ||
      get_index + sizeof(T) < get_index) {  // overflow
    THROW_NEW_ERROR(
        isolate, NewRangeError(MessageTemplate::kInvalidDataViewAccessorOffset),
        Object);
  }
  union {
    T data;
    uint8_t bytes[sizeof(T)];
  } v;
  v.data = DataViewConvertValue<T>(value);
  size_t const buffer_offset = data_view_byte_offset + get_index;
  DCHECK(NumberToSize(buffer->byte_length()) >= buffer_offset + sizeof(T));
  uint8_t* const target =
      static_cast<uint8_t*>(buffer->backing_store()) + buffer_offset;
  if (NeedToFlipBytes(is_little_endian)) {
    FlipBytes<sizeof(T)>(target, v.bytes);
  } else {
    CopyBytes<sizeof(T)>(target, v.bytes);
  }
  return isolate->factory()->undefined_value();
}

}  // namespace

#define DATA_VIEW_PROTOTYPE_GET(Type, type)                                \
  BUILTIN(DataViewPrototypeGet##Type) {                                    \
    HandleScope scope(isolate);                                            \
    CHECK_RECEIVER(JSDataView, data_view, "DataView.prototype.get" #Type); \
    Handle<Object> byte_offset = args.atOrUndefined(isolate, 1);           \
    Handle<Object> is_little_endian = args.atOrUndefined(isolate, 2);      \
    Handle<Object> result;                                                 \
    ASSIGN_RETURN_FAILURE_ON_EXCEPTION(                                    \
        isolate, result,                                                   \
        GetViewValue<type>(isolate, data_view, byte_offset,                \
                           is_little_endian->BooleanValue(),               \
                           "DataView.prototype.get" #Type));               \
    return *result;                                                        \
  }
DATA_VIEW_PROTOTYPE_GET(Int8, int8_t)
DATA_VIEW_PROTOTYPE_GET(Uint8, uint8_t)
DATA_VIEW_PROTOTYPE_GET(Int16, int16_t)
DATA_VIEW_PROTOTYPE_GET(Uint16, uint16_t)
DATA_VIEW_PROTOTYPE_GET(Int32, int32_t)
DATA_VIEW_PROTOTYPE_GET(Uint32, uint32_t)
DATA_VIEW_PROTOTYPE_GET(Float32, float)
DATA_VIEW_PROTOTYPE_GET(Float64, double)
DATA_VIEW_PROTOTYPE_GET(BigInt64, int64_t)
DATA_VIEW_PROTOTYPE_GET(BigUint64, uint64_t)
#undef DATA_VIEW_PROTOTYPE_GET

#define DATA_VIEW_PROTOTYPE_SET(Type, type)                                \
  BUILTIN(DataViewPrototypeSet##Type) {                                    \
    HandleScope scope(isolate);                                            \
    CHECK_RECEIVER(JSDataView, data_view, "DataView.prototype.set" #Type); \
    Handle<Object> byte_offset = args.atOrUndefined(isolate, 1);           \
    Handle<Object> value = args.atOrUndefined(isolate, 2);                 \
    Handle<Object> is_little_endian = args.atOrUndefined(isolate, 3);      \
    Handle<Object> result;                                                 \
    ASSIGN_RETURN_FAILURE_ON_EXCEPTION(                                    \
        isolate, result,                                                   \
        SetViewValue<type>(isolate, data_view, byte_offset,                \
                           is_little_endian->BooleanValue(), value,        \
                           "DataView.prototype.get" #Type));               \
    return *result;                                                        \
  }
DATA_VIEW_PROTOTYPE_SET(Int8, int8_t)
DATA_VIEW_PROTOTYPE_SET(Uint8, uint8_t)
DATA_VIEW_PROTOTYPE_SET(Int16, int16_t)
DATA_VIEW_PROTOTYPE_SET(Uint16, uint16_t)
DATA_VIEW_PROTOTYPE_SET(Int32, int32_t)
DATA_VIEW_PROTOTYPE_SET(Uint32, uint32_t)
DATA_VIEW_PROTOTYPE_SET(Float32, float)
DATA_VIEW_PROTOTYPE_SET(Float64, double)
DATA_VIEW_PROTOTYPE_SET(BigInt64, int64_t)
DATA_VIEW_PROTOTYPE_SET(BigUint64, uint64_t)
#undef DATA_VIEW_PROTOTYPE_SET

}  // namespace internal
}  // namespace v8
