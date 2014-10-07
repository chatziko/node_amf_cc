/*
"If possible, build it in Javascript."
   - https://kkaefer.com/node-cpp-modules/#benchmark-thread-pool
*/
#define BUILDING_NODE_EXTENSION
#include <math.h>  // isnan
#include <node.h>

#include "amf.h"
#include "serializer.h"
#include "utils.h"

using namespace v8;

namespace {
const int INSTANCE_NO_TRAITS_NO_EXTERNALIZABLE = 11;

const char SERIALIZED_NaN[] = "\0\0\0\0\0\0\xF8\x7F"; 
}

Persistent<Function> Serializer::constructor;

int Serializer::bigEndian = 0;

Serializer::Serializer() {
  clear(); 
}

Serializer::~Serializer() { 
}

void Serializer::Init(Handle<Object> exports) {
  // Endian test
  const int endian_test = 1;
  bigEndian = !!is_bigendian();

  // Prepare constructor template
  Local<FunctionTemplate> tpl = FunctionTemplate::New(New);
  tpl->SetClassName(String::NewSymbol("serializer"));
  tpl->InstanceTemplate()->SetInternalFieldCount(1);
  // Prototype
  tpl->PrototypeTemplate()->Set(String::NewSymbol("serialize"),
      FunctionTemplate::New(Serialize)->GetFunction());
  constructor = Persistent<Function>::New(tpl->GetFunction());
  exports->Set(String::NewSymbol("serializer"), constructor);
}

Handle<Value> Serializer::New(const Arguments& args) {
  HandleScope scope;

  if (args.IsConstructCall()) {
    // Invoked as constructor: `new Serializer(...)`
    Serializer* obj = new Serializer();
    obj->Wrap(args.This());
    return args.This();
  } else {
    // Invoked as plain function `Serializer(...)`, turn into construct call.
    return scope.Close(constructor->NewInstance());
  }
}

Handle<Value> Serializer::Serialize(const Arguments& args) {
  HandleScope scope;

  Serializer* obj = ObjectWrap::Unwrap<Serializer>(args.This());

  if (args.Length() != 1) {
    die("Need exactly one argument");
  }

  obj->writeValue(args[0]);

  return scope.Close(String::New(obj->buffer_.data(), obj->buffer_.size()));
}

void Serializer::clear() {
  buffer_.clear();
}

// Begin straight-up port of node-amf's serializer.js

/**
 * Write any JavaScript value, automatically chooses which data type to use
 * @param mixed
 */
void Serializer::writeValue(Handle<Value> value) {
  if (value.IsEmpty() || value->IsUndefined()) {
    writeUndefined();
  } else if (value->IsNull()) {
    writeNull();
  } else if (value->IsString()) {
    writeUTF8(value->ToString(), true);
  } else if (value->IsNumber()) {
    writeNumber(value, true);
  } else if (value->IsBoolean()) {
    writeBool(value->ToBoolean());
  } else if (value->IsArray()) {
    writeArray(Handle<Array>(Array::Cast(*value)));
  } else if (value->IsObject()) {
    // special object types 
    if (!writeDateIf(value->ToObject())) {
      // else write vanilla Object
      writeObject(value->ToObject());
    }
  }
}

void Serializer::writeUndefined() {
  writeU8(AMF::AMF3_UNDEFINED);
} 

void Serializer::writeNull() {
  writeU8(AMF::AMF3_NULL);
}

void Serializer::writeBool(Handle<Boolean> value) {
  writeU8(value->Value() ? AMF::AMF3_TRUE : AMF::AMF3_FALSE);
}

void Serializer::writeUTF8(Handle<String> value, bool writeMarker) {
  int encodedLen = value->Utf8Length();
  if (writeMarker) {
    writeU8(AMF::AMF3_STRING);
  }
  int flag = (encodedLen << 1) | 1;
  writeU29(flag);

  // TODO: Fix annoying bug due to String class inability to read binary 
  std::vector<char> utf8str(encodedLen+1);
  value->WriteUtf8(utf8str.data());
  for (int i = 0; i < encodedLen; ++i) {
    buffer_.write(utf8str.data()[i]);
  }
}

void Serializer::writeArray(Handle<Array> value) {
  writeU8(AMF::AMF3_ARRAY);
  // TODO: support object references
  int len = value->Length(); 
  // flag with XXXXXXX1 indicating length of dense portion with instance
  int flag = ( len << 1 ) | 1;
  writeU29(flag);
  writeUTF8(String::Empty());
  for (int i = 0; i < len; i++) {
    writeValue(value->CloneElementAt(i));
  }
}

void Serializer::writeObject(Handle<Object> value) {
  writeU8(AMF::AMF3_OBJECT);
  // TODO support object references
  writeU29(INSTANCE_NO_TRAITS_NO_EXTERNALIZABLE);

  // TODO support named classes
  writeUTF8(String::New("Object")); 
 
  // write serializable properties
  Local<Array> propertyNames = value->GetPropertyNames();
  for (uint i = 0; i < propertyNames->Length(); i++) {
    Handle<String> propName = propertyNames->Get(i)->ToString();
    if (!value->HasRealNamedProperty(propName)) {
      continue;
    }

    Local<Value> propValue = value->Get(propName);
    if (propValue->IsFunction()) {
      continue;
    }
    writeUTF8(propName->ToString());
    writeValue(propValue); 
  }
  writeUTF8(String::Empty());
}

bool Serializer::writeDateIf(Handle<Object> date) {
  Local<Value> getTime = date->Get(String::New("getTime"));
  if (!getTime->IsFunction()) {
    return false;
  }
  Local<Function> getTimeFn = Function::Cast(*getTime);
  Local<Value> dateDouble = getTimeFn->Call(date, 0, NULL);

  writeU8(AMF::AMF3_DATE);
  writeU29(1);
  writeDouble(dateDouble);
  return true;
}

void Serializer::writeNumber(Handle<Value> value, bool writeMarker) {
  Local<Integer> integer = value->ToInteger();
  if (!integer->IsNull()) {
    int64_t val = integer->Value();
    if (val == value->NumberValue() && val >= 0 && val < 0x2000000) {
      writeU29(val, writeMarker);
      return;
    }
  }
  writeDouble(value, writeMarker);
}

void Serializer::writeDouble(Handle<Value> value, bool writeMarker) {
  if (writeMarker) {
    writeU8(AMF::AMF3_DOUBLE);
  }
  double doubleValue = value->NumberValue();
  if (isnan(doubleValue)) {
    writeBinaryString(SERIALIZED_NaN, sizeof(SERIALIZED_NaN));
    return;
  }
  // from amfast
  // https://code.google.com/p/amfast/source/browse/trunk/amfast/ext_src/encoder.c
  union aligned {
    double d_value;
    char c_value[8];
  } d_aligned;
  char *char_value = d_aligned.c_value;
  d_aligned.d_value = doubleValue;

  if (bigEndian) {
    for (int i = 0; i < 7; ++i) {
      writeU8(char_value[i]);
    }
  } else {
    for (int i = 7; i >= 0; --i) {
      writeU8(char_value[i]);
    }
  }
}

void Serializer::writeU8(char n) {
  buffer_.write(n);
}

void Serializer::writeBinaryString(const char* str, int len) {
  for (int i = 0; i < len; ++i) {
    writeU8(str[i]);
  }
}

void Serializer::writeU29(int64_t n, bool writeMarker) {
  std::vector<char> bytes;
  if (n < 0) {
    die("U29 range error - negative number");
  }
  bytes.push_back(n & 0x7F);
  while (n >= 0x80) {
    bytes.push_back(0x80 | ( n>>=7 & 0x7F ));
  }
  if (writeMarker) {
    writeU8(AMF::AMF3_INTEGER);
  }
  for (int i = bytes.size() - 1; i >= 0; --i) {
    writeU8(bytes[i]);
  }
}

