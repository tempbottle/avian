#include "common.h"
#include "system.h"
#include "constants.h"
#include "machine.h"
#include "processor.h"
#include "process.h"

using namespace vm;

extern "C" uint64_t
vmInvoke(void* function, void* stack, unsigned stackSize,
         unsigned returnType);

extern "C" void NO_RETURN
vmJump(void* address);

namespace {

const bool Verbose = false;

const unsigned FrameThread = BytesPerWord * 2;
const unsigned FrameMethod = FrameThread + BytesPerWord;
const unsigned FrameNext = FrameNext + BytesPerWord;
const unsigned FrameFootprint = BytesPerWord * 3;

class ArgumentList;

class Buffer {
 public:
  Buffer(System* s, unsigned minimumCapacity):
    s(s),
    data(0),
    position(0),
    capacity(0),
    minimumCapacity(minimumCapacity)
  { }

  ~Buffer() {
    if (data) {
      s->free(data);
    }
  }

  void ensure(unsigned space) {
    if (position + space > capacity) {
      unsigned newCapacity = max
        (position + space, max(minimumCapacity, capacity * 2));
      uint8_t* newData = static_cast<uint8_t*>(s->allocate(newCapacity));
      if (data) {
        memcpy(newData, data, position);
        s->free(data);
      }
      data = newData;
    }
  }

  void append(uint8_t v) {
    ensure(1);
    data[position++] = v;
  }

  void append2(uint16_t v) {
    ensure(2);
    memcpy(data + position, &v, 2);
    position += 2;
  }

  void append4(uint32_t v) {
    ensure(4);
    memcpy(data + position, &v, 4);
    position += 4;
  }

  void set2(unsigned offset, uint32_t v) {
    assert(s, offset + 2 <= position);
    memcpy(data + offset, &v, 2);
  }

  void set4(unsigned offset, uint32_t v) {
    assert(s, offset + 4 <= position);
    memcpy(data + offset, &v, 4); 
  }

  uint16_t get2(unsigned offset) {
    assert(s, offset + 2 <= position);
    uint16_t v; memcpy(&v, data + offset, 2);
    return v;
  }

  uint32_t get4(unsigned offset) {
    assert(s, offset + 4 <= position);
    uint32_t v; memcpy(&v, data + offset, 4);
    return v;
  }

  void appendAddress(uintptr_t v) {
    append4(v);
    if (BytesPerWord == 8) {
      // we have to use the preprocessor here to avoid a warning on
      // 32-bit systems
#ifdef __x86_64__
      append4(v >> 32);
#endif
    }
  }

  unsigned length() {
    return position;
  }

  void copyTo(void* b) {
    if (data) {
      memcpy(b, data, position);
    }
  }

  System* s;
  uint8_t* data;
  unsigned position;
  unsigned capacity;
  unsigned minimumCapacity;
};

class Code {
 public:
  Code(Buffer* code, Buffer* lineNumbers, Buffer* exceptionHandlers):
    codeLength_(code->length()),
    lineNumberTableLength_(lineNumbers->length()),
    exceptionHandlerTableLength_(exceptionHandlers->length())
  {
    code->copyTo(body);
    lineNumbers->copyTo(lineNumber(0));
    exceptionHandlers->copyTo(exceptionHandler(0));
  }

  uint8_t* code() {
    return body;
  }

  unsigned codeLength() {
    return codeLength_;
  }
  
  NativeLineNumber* lineNumber(unsigned index) {
    return reinterpret_cast<NativeLineNumber*>
      (body + pad(codeLength_)) + index;
  }

  unsigned lineNumberTableLength() {
    return lineNumberTableLength_ / sizeof(NativeLineNumber);
  }

  NativeExceptionHandler* exceptionHandler(unsigned index) {
    return reinterpret_cast<NativeExceptionHandler*>
      (body + pad(codeLength_) + pad(lineNumberTableLength_)) + index;
  }

  unsigned exceptionHandlerTableLength() {
    return exceptionHandlerTableLength_ / sizeof(NativeExceptionHandler);
  }

  uint32_t codeLength_;
  uint32_t lineNumberTableLength_;
  uint32_t exceptionHandlerTableLength_;
  uint8_t body[0];
};

class MyThread: public Thread {
 public:
  MyThread(Machine* m, object javaThread, vm::Thread* parent):
    vm::Thread(m, javaThread, parent),
    argumentList(0),
    frame(0),
    reference(0)
  { }

  ArgumentList* argumentList;
  void* frame;
  Reference* reference;
};

inline bool
frameValid(void* frame)
{
  return frame != 0;
}

inline void*
frameBase(void* frame)
{
  return static_cast<void**>(frame)[- (FrameFootprint / BytesPerWord) - 1];
}

inline void*
frameNext(void* frame)
{
  return static_cast<void**>(frameBase(frame))[FrameNext / BytesPerWord];
}

inline object
frameMethod(void* frame)
{
  return static_cast<object*>(frameBase(frame))[FrameMethod / BytesPerWord];
}

inline void*
frameAddress(void* frame)
{
  return static_cast<void**>(frame)[- (FrameFootprint / BytesPerWord)];
}

inline void*
frameReturnAddress(void* frame)
{
  return static_cast<void**>(frameBase(frame))[1];
}

inline unsigned
addressOffset(Thread* t, object method, void* address)
{
  Code* code = static_cast<Code*>(methodCompiled(t, method));
  return static_cast<uint8_t*>(address) - code->code();
}

NativeExceptionHandler*
findExceptionHandler(Thread* t, void* frame)
{
  object method = frameMethod(frame);
  Code* code = static_cast<Code*>(methodCompiled(t, method));
      
  for (unsigned i = 0; i < code->exceptionHandlerTableLength(); ++i) {
    NativeExceptionHandler* handler = code->exceptionHandler(i);
    unsigned offset = addressOffset(t, method, frameAddress(frame));

    if (offset - 1 >= nativeExceptionHandlerStart(handler)
        and offset - 1 < nativeExceptionHandlerEnd(handler))
    {
      object catchType;
      if (nativeExceptionHandlerCatchType(handler)) {
        catchType = arrayBody
          (t, methodCode(t, method),
           nativeExceptionHandlerCatchType(handler) - 1);
      } else {
        catchType = 0;
      }

      if (catchType == 0 or instanceOf(t, catchType, t->exception)) {
        return handler;
      }
    }
  }

  return 0;
}

void NO_RETURN
unwind(MyThread* t)
{
  for (void* frame = t->frame; frameValid(frame); frame = frameNext(frame)) {
    void* next = frameNext(frame);
    if (not frameValid(next)
        or methodFlags(t, frameMethod(next)) & ACC_NATIVE)
    {
      t->frame = next;
      vmJump(frameReturnAddress(frame));
    } else if ((methodFlags(t, frameMethod(frame)) & ACC_NATIVE) == 0) {
      NativeExceptionHandler* eh = findExceptionHandler(t, frame);
      if (eh) {
        Code* code = static_cast<Code*>(methodCompiled(t, frameMethod(frame)));
        t->frame = frame;
        vmJump(code->code() + nativeExceptionHandlerIp(eh));
      }
    }
  }
  abort(t);
}

void NO_RETURN
throwNew(MyThread* t, object class_)
{
  t->exception = makeNew(t, class_);
  unwind(t);
}

void NO_RETURN
throw_(MyThread* t, object o)
{
  if (o) {
    t->exception = makeNullPointerException(t);
  } else {
    t->exception = o;
  }
  unwind(t);
}

object
makeBlankObjectArray(Thread* t, object class_, int32_t length)
{
  return makeObjectArray(t, class_, length, true);
}

object
makeBlankArray(Thread* t, object (*constructor)(Thread*, uintptr_t, bool),
               int32_t length)
{
  return constructor(t, length, true);
}

uint64_t
invokeNative2(MyThread* t, object method)
{
  PROTECT(t, method);

  if (objectClass(t, methodCode(t, method))
      == arrayBody(t, t->m->types, Machine::ByteArrayType))
  {
    void* function = resolveNativeMethod(t, method);
    if (UNLIKELY(function == 0)) {
      object message = makeString
        (t, "%s", &byteArrayBody(t, methodCode(t, method), 0));
      t->exception = makeUnsatisfiedLinkError(t, message);
      return 0;
    }

    object p = makePointer(t, function);
    set(t, methodCode(t, method), p);
  }

  object class_ = methodClass(t, method);
  PROTECT(t, class_);

  unsigned footprint = methodParameterFootprint(t, method) + 1;
  unsigned count = methodParameterCount(t, method) + 1;
  if (methodFlags(t, method) & ACC_STATIC) {
    ++ footprint;
    ++ count;
  }

  uintptr_t args[footprint];
  unsigned argOffset = 0;
  uint8_t types[count];
  unsigned typeOffset = 0;

  args[argOffset++] = reinterpret_cast<uintptr_t>(t);
  types[typeOffset++] = POINTER_TYPE;

  uintptr_t* sp = static_cast<uintptr_t*>(t->frame)
    + (methodParameterFootprint(t, method) + 1)
    + (FrameFootprint / BytesPerWord);

  if (methodFlags(t, method) & ACC_STATIC) {
    args[argOffset++] = reinterpret_cast<uintptr_t>(&class_);
  } else {
    args[argOffset++] = reinterpret_cast<uintptr_t>(sp--);
  }
  types[typeOffset++] = POINTER_TYPE;

  MethodSpecIterator it
    (t, reinterpret_cast<const char*>
     (&byteArrayBody(t, methodSpec(t, method), 0)));
  
  while (it.hasNext()) {
    unsigned type = types[typeOffset++]
      = fieldType(t, fieldCode(t, *it.next()));

    switch (type) {
    case INT8_TYPE:
    case INT16_TYPE:
    case INT32_TYPE:
    case FLOAT_TYPE:
      args[argOffset++] = *(sp--);
      break;

    case INT64_TYPE:
    case DOUBLE_TYPE: {
      if (BytesPerWord == 8) {
        uint64_t a = *(sp--);
        uint64_t b = *(sp--);
        args[argOffset++] = (a << 32) | b;
      } else {
        memcpy(args + argOffset, sp, 8);
        argOffset += 2;
        sp -= 2;
      }
    } break;

    case POINTER_TYPE: {
      args[argOffset++] = reinterpret_cast<uintptr_t>(sp--);
    } break;

    default: abort(t);
    }
  }

  void* function = pointerValue(t, methodCode(t, method));
  unsigned returnType = fieldType(t, methodReturnCode(t, method));
  uint64_t result;
  
  if (Verbose) {
    fprintf(stderr, "invoke native method %s.%s\n",
            &byteArrayBody(t, className(t, methodClass(t, method)), 0),
            &byteArrayBody(t, methodName(t, method), 0));
  }

  { ENTER(t, Thread::IdleState);

    result = t->m->system->call
      (function,
       args,
       types,
       count + 1,
       footprint,
       returnType);
  }

  if (Verbose) {
    fprintf(stderr, "return from native method %s.%s\n",
            &byteArrayBody(t, className(t, methodClass(t, method)), 0),
            &byteArrayBody(t, methodName(t, method), 0));
  }

  if (LIKELY(t->exception == 0) and returnType == POINTER_TYPE) {
    return *reinterpret_cast<uintptr_t*>(result);
  } else {
    return result;
  }
}

uint64_t
invokeNative(MyThread* t, object method)
{
  uint64_t result = invokeNative2(t, method);
  if (UNLIKELY(t->exception)) {
    unwind(t);
  } else {
    return result;
  }
}

void
compileMethod(MyThread* t, object method);

inline bool
isByte(int32_t v)
{
  return v == static_cast<int8_t>(v);
}

enum Register {
  rax = 0,
  rcx = 1,
  rdx = 2,
  rbx = 3,
  rsp = 4,
  rbp = 5,
  rsi = 6,
  rdi = 7,
  r8 = 8,
  r9 = 9,
  r10 = 10,
  r11 = 11,
  r12 = 12,
  r13 = 13,
  r14 = 14,
  r15 = 15,
};

enum SSERegister {
  xmm0 = 0,
  xmm1 = 1,
  xmm2 = 2,
  xmm3 = 3,
  xmm4 = 4,
  xmm5 = 5,
  xmm6 = 6,
  xmm7 = 7
};

class Assembler {
 public:
  class Label {
   public:
    static const unsigned Capacity = 8;

    Label(Assembler* a):
      code(&(a->code)),
      unresolvedCount(0),
      mark_(-1)
    { }

    void reference() {
      if (mark_ == -1) {
        expect(code->s, unresolvedCount < Capacity);
        unresolved[unresolvedCount] = code->length();
        ++ unresolvedCount;

        code->append4(0);
      } else {
        code->append4(mark_ - (code->length() + 4));
      }
    }

    void mark() {
      mark_ = code->length();
      for (unsigned i = 0; i < unresolvedCount; ++i) {
        code->set4(unresolved[i], mark_ - (unresolved[i] + 4));
      }
    }

    Buffer* code;
    unsigned unresolved[Capacity];
    unsigned unresolvedCount;
    int mark_;
  };

  Assembler(System* s):
    code(s, 1024),
    jumps(s, 32)
  { }

  void rex() {
    if (BytesPerWord == 8) {
      code.append(0x48);
    }
  }

  void mov(Register src, Register dst) {
    rex();
    code.append(0x89);
    code.append(0xc0 | (src << 3) | dst);
  }

  void offsetInstruction(uint8_t instruction, uint8_t zeroPrefix,
                         uint8_t bytePrefix, uint8_t wordPrefix,
                         unsigned a, unsigned b, int32_t offset)
  {
    code.append(instruction);

    uint8_t prefix;
    if (offset == 0 and b != rbp) {
      prefix = zeroPrefix;
    } else if (isByte(offset)) {
      prefix = bytePrefix;
    } else {
      prefix = wordPrefix;
    }

    code.append(prefix | (a << 3) | b);

    if (b == rsp) {
      code.append(0x24);
    }

    if (offset == 0 and b != rbp) {
      // do nothing
    } else if (isByte(offset)) {
      code.append(offset);
    } else {
      code.append4(offset);
    }    
  }

  void movz1(Register src, Register dst) {
    code.append(0x0f);
    code.append(0xb6);
    code.append(0xc0 | (dst << 3) | src);
  }

  void movz1(Register src, int32_t srcOffset, Register dst) {
    code.append(0x0f);
    offsetInstruction(0xb6, 0, 0x40, 0x80, dst, src, srcOffset);
  }

  void movs1(Register src, Register dst) {
    code.append(0x0f);
    code.append(0xbe);
    code.append(0xc0 | (dst << 3) | src);
  }

  void movs1(Register src, int32_t srcOffset, Register dst) {
    code.append(0x0f);
    offsetInstruction(0xbe, 0, 0x40, 0x80, dst, src, srcOffset);
  }

  void movz2(Register src, Register dst) {
    code.append(0x0f);
    code.append(0xb7);
    code.append(0xc0 | (dst << 3) | src);
  }

  void movz2(Register src, int32_t srcOffset, Register dst) {
    code.append(0x0f);
    offsetInstruction(0xb7, 0, 0x40, 0x80, dst, src, srcOffset);
  }

  void movs2(Register src, Register dst) {
    code.append(0x0f);
    code.append(0xbf);
    code.append(0xc0 | (dst << 3) | src);
  }

  void movs2(Register src, int32_t srcOffset, Register dst) {
    code.append(0x0f);
    offsetInstruction(0xbf, 0, 0x40, 0x80, dst, src, srcOffset);
  }

  void mov4(Register src, int32_t srcOffset, Register dst) {
    offsetInstruction(0x8b, 0, 0x40, 0x80, dst, src, srcOffset);
  }

  void mov1(Register src, Register dst, int32_t dstOffset) {
    offsetInstruction(0x88, 0, 0x40, 0x80, src, dst, dstOffset);
  }

  void mov2(Register src, Register dst, int32_t dstOffset) {
    code.append(0x66);
    offsetInstruction(0x89, 0, 0x40, 0x80, src, dst, dstOffset);
  }

  void mov4(Register src, Register dst, int32_t dstOffset) {
    offsetInstruction(0x89, 0, 0x40, 0x80, src, dst, dstOffset);
  }

  void mov(Register src, int32_t srcOffset, SSERegister dst) {
    code.append(0xf3);
    code.append(0x0f);
    offsetInstruction(0x7e, 0, 0x40, 0x80, dst, src, srcOffset);
  }

  void mov(Register src, int32_t srcOffset, Register dst) {
    rex();
    mov4(src, srcOffset, dst);
  }

  void mov(Register src, Register dst, int32_t dstOffset) {
    rex();
    mov4(src, dst, dstOffset);
  }

  void mov(uintptr_t v, Register dst) {
    rex();
    code.append(0xb8 | dst);
    code.appendAddress(v);
  }

  void alignedMov(uintptr_t v, Register dst) {
    while ((code.length() + (BytesPerWord == 8 ? 2 : 1)) % BytesPerWord) {
      nop();
    }
    rex();
    code.append(0xb8 | dst);
    code.appendAddress(v);
  }

  void nop() {
    code.append(0x90);
  }

  void push(Register reg) {
    code.append(0x50 | reg);
  }

  void push(Register reg, int32_t offset) {
    offsetInstruction(0xff, 0x30, 0x70, 0xb0, rax, reg, offset);
  }

  void push(int32_t v) {
    if (isByte(v)) {
      code.append(0x6a);
      code.append(v);
    } else {
      code.append(0x68);
      code.append4(v);
    }
  }

  void push4(Register reg, int32_t offset) {
    if (BytesPerWord == 8) {
      mov4(reg, offset, rsi);
      push(rsi);
    } else {
      push(reg, offset);
    }
  }

  void pushAddress(uintptr_t v) {
    if (BytesPerWord == 8) {
      mov(v, rsi);
      push(rsi);
    } else {
      push(v);
    }
  }

  void pop(Register dst) {
    code.append(0x58 | dst);
  }

  void pop(Register dst, int32_t offset) {
    offsetInstruction(0x8f, 0, 0x40, 0x80, rax, dst, offset);
  }

  void pop4(Register reg, int32_t offset) {
    if (BytesPerWord == 8) {
      pop(rsi);
      mov4(rsi, reg, offset);
    } else {
      pop(reg, offset);
    }
  }

  void add(Register src, Register dst) {
    rex();
    code.append(0x01);
    code.append(0xc0 | (src << 3) | dst);
  }

  void add(int32_t v, Register dst) {
    assert(code.s, isByte(v)); // todo

    rex();
    code.append(0x83);
    code.append(0xc0 | dst);
    code.append(v);
  }

  void add(int32_t v, Register dst, unsigned offset) {
    rex();
    unsigned i = (isByte(v) ? 0x83 : 0x81);
    offsetInstruction(i, 0, 0x40, 0x80, rax, dst, offset);
    if (isByte(v)) {
      code.append(v);
    } else {
      code.append4(v);
    }
  }

  void sub(Register src, Register dst) {
    rex();
    code.append(0x29);
    code.append(0xc0 | (src << 3) | dst);
  }

  void sub(int32_t v, Register dst) {
    assert(code.s, isByte(v)); // todo

    rex();
    code.append(0x83);
    code.append(0xe8 | dst);
    code.append(v);
  }

  void or_(Register src, Register dst) {
    rex();
    code.append(0x09);
    code.append(0xc0 | (src << 3) | dst);
  }

  void or_(int32_t v, Register dst) {
    assert(code.s, isByte(v)); // todo

    rex();
    code.append(0x83);
    code.append(0xc8 | dst);
    code.append(v);
  }

  void and_(Register src, Register dst) {
    rex();
    code.append(0x21);
    code.append(0xc0 | (src << 3) | dst);
  }

  void and_(int32_t v, Register dst) {
    assert(code.s, isByte(v)); // todo

    rex();
    code.append(0x83);
    code.append(0xe0 | dst);
    code.append(v);
  }

  void shl(int8_t v, Register dst) {
    rex();
    if (v == 1) {
      code.append(0xd1);
      code.append(0xe0 | dst);
    } else {
      code.append(0xc1);
      code.append(0xe0 | dst);
      code.append(v);
    }
  }

  void ret() {
    code.append(0xc3);
  }

  void jmp(Label& label) {
    code.append(0xE9);
    label.reference();
  }

  void jmp(unsigned javaIP) {
    code.append(0xE9);

    jumps.append4(javaIP);
    jumps.append4(code.length());

    code.append4(0);
  }

  void jmp(Register reg) {
    code.append(0xff);
    code.append(0xe0 | reg);
  }

  void conditional(Label& label, unsigned condition) {
    code.append(0x0f);
    code.append(condition);
    label.reference();
  }

  void conditional(unsigned javaIP, unsigned condition) {
    code.append(0x0f);
    code.append(condition);

    jumps.append4(javaIP);
    jumps.append4(code.length());

    code.append4(0);
  }

  void je(Label& label) {
    conditional(label, 0x84);
  }

  void je(unsigned javaIP) {
    conditional(javaIP, 0x84);
  }

  void jne(Label& label) {
    conditional(label, 0x85);
  }

  void jne(unsigned javaIP) {
    conditional(javaIP, 0x85);
  }

  void jg(Label& label) {
    conditional(label, 0x8f);
  }

  void jg(unsigned javaIP) {
    conditional(javaIP, 0x8f);
  }

  void jge(Label& label) {
    conditional(label, 0x8d);
  }

  void jge(unsigned javaIP) {
    conditional(javaIP, 0x8d);
  }

  void jl(Label& label) {
    conditional(label, 0x8c);
  }

  void jl(unsigned javaIP) {
    conditional(javaIP, 0x8c);
  }

  void jle(Label& label) {
    conditional(label, 0x8e);
  }

  void jle(unsigned javaIP) {
    conditional(javaIP, 0x8e);
  }

  void cmp(int v, Register reg) {
    assert(code.s, isByte(v)); // todo

    code.append(0x83);
    code.append(0xf8 | reg);
    code.append(v);
  }

  void cmp(Register a, Register b) {
    code.append(0x39);
    code.append(0xc0 | (a << 3) | b);
  }

  void call(Register reg) {
    code.append(0xff);
    code.append(0xd0 | reg);
  }

  Buffer code;
  Buffer jumps;
};

int
localOffset(int v, int parameterFootprint)
{
  v *= BytesPerWord;
  if (v < parameterFootprint) {
    return (parameterFootprint - v - BytesPerWord) + (BytesPerWord * 2)
      + FrameFootprint;
  } else {
    return -(v + BytesPerWord - parameterFootprint);
  }
}

Register
gpRegister(Thread* t, unsigned index)
{
  switch (index) {
  case 0:
    return rdi;
  case 1:
    return rsi;
  case 2:
    return rdx;
  case 3:
    return rcx;
  case 4:
    return r8;
  case 5:
    return r9;
  default:
    abort(t);
  }
}

SSERegister
sseRegister(Thread* t UNUSED, unsigned index)
{
  assert(t, index < 8);
  return static_cast<SSERegister>(index);
}

unsigned
parameterOffset(unsigned index)
{
  return FrameFootprint + ((index + 2) * BytesPerWord);
}

class Compiler: public Assembler {
 public:
  Compiler(Thread* t):
    Assembler(s),
    t(t),
    threadFrameOffset(reinterpret_cast<uintptr_t>(&(t->frame))
                      - reinterpret_cast<uintptr_t>(t))
    poolRegisterClobbered(true),
    javaIPs(s, 1024),
    machineIPs(s, 1024),
    lineNumbers(s, 256),
    exceptionHandlers(s, 256),
    pool(s, 256)
  { }

  void pushReturnValue(unsigned code) {
    switch (code) {
    case ByteField:
    case BooleanField:
    case CharField:
    case ShortField:
    case FloatField:
    case IntField:
    case ObjectField:
      push(rax);
      break;

    case LongField:
    case DoubleField:
      push(rax);
      push(rdx);
      break;

    case VoidField:
      break;

    default:
      abort(t);
    }
  }

  void compileDirectInvoke(object target) {
    unsigned footprint = FrameFootprint
      + (methodParameterFootprint(t, target) * BytesPerWord);

    Code* code = static_cast<Code*>(methodCompiled(t, target));
        
    push(rsp);
    push(poolRegister(), poolReference(target));
    push(rbp, FrameThread);

    callAlignedAddress(code->code());

    add(footprint, rsp);                     // pop arguments

    pushReturnValue(methodReturnCode(t, target));
  }

  void compileCall(void* function, object arg1) {
    if (BytesPerWord == 4) {
      push(poolRegister(), poolReference(arg1));
      push(rbp, FrameThread);
    } else {
      mov(poolRegister(), poolReference(arg1), rsi);
      mov(rbp, FrameThread, rdi);
    }

    mov(rbp, FrameThread, rax);
    mov(rbp, rax, threadFrameOffset);        // set thread frame to current

    callAddress(function);

    if (BytesPerWord == 4) {
      add(BytesPerWord * 2, rsp);
    }
  }

  void compileCall(void* function, Register arg1) {
    if (BytesPerWord == 4) {
      push(arg1);
      push(rbp, FrameThread);
    } else {
      mov(arg1, rsi);
      mov(rbp, FrameThread, rdi);
    }

    mov(rbp, FrameThread, rax);
    mov(rbp, rax, threadFrameOffset);        // set thread frame to current

    callAddress(function);

    if (BytesPerWord == 4) {
      add(BytesPerWord * 2, rsp);
    }
  }

  void compileCall(void* function, object arg1, Register arg2) {
    if (BytesPerWord == 4) {
      push(arg2);
      push(poolRegister(), poolReference(arg1));
      push(rbp, FrameThread);
    } else {
      mov(arg2, rdx);
      mov(poolRegister(), poolReference(arg1), rsi);
      mov(rbp, FrameThread, rdi);
    }

    mov(rbp, FrameThread, rax);
    mov(rbp, rax, threadFrameOffset);        // set thread frame to current

    callAddress(function);

    if (BytesPerWord == 4) {
      add(BytesPerWord * 3, rsp);
    }
  }

  void compileCall(void* function, void* arg1, Register arg2) {
    mov(rbp, FrameThread, rax);
    mov(rbp, rax, frameOffset);              // set thread frame to current

    if (BytesPerWord == 4) {
      push(arg2);
      pushAddress(arg1);
      push(rbp, FrameThread);
    } else {
      mov(arg2, rdx);
      mov(arg1, rsi);
      mov(rbp, FrameThread, rdi);
    }

    mov(rbp, FrameThread, rax);
    mov(rbp, rax, threadFrameOffset);        // set thread frame to current

    callAddress(function);

    if (BytesPerWord == 4) {
      add(BytesPerWord * 3, rsp);
    }
  }

  void compileCall(void* function, Register arg1, Register arg2) {
    if (BytesPerWord == 4) {
      push(arg2);
      push(arg1);
      push(rbp, FrameThread);
    } else {
      mov(arg2, rdx);
      mov(arg1, rsi);
      mov(rbp, FrameThread, rdi);
    }

    mov(rbp, FrameThread, rax);
    mov(rbp, rax, threadFrameOffset);        // set thread frame to current

    callAddress(function);

    if (BytesPerWord == 4) {
      add(BytesPerWord * 3, rsp);
    }
  }

  void compile(MyThread* t, object method) {
    PROTECT(t, method);

    object code = methodCode(t, method);
    PROTECT(t, code);

    unsigned parameterFootprint
      = methodParameterFootprint(t, method) * BytesPerWord;

    unsigned localFootprint = codeMaxLocals(t, code) * BytesPerWord;

    push(rbp);
    mov(rsp, rbp);

    if (localFootprint > parameterFootprint) {
      // reserve space for local variables
      sub(localFootprint - parameterFootprint, rsp);
    }

    int lineNumberIndex;
    object lnt = codeLineNumberTable(t, code);
    if (lnt and lineNumberTableLength(t, lnt)) {
      lineNumberIndex = 0;
    } else {
      lineNumberIndex = 0;
    }
    
    for (unsigned ip = 0; ip < codeLength(t, code);) {
      javaIPs.append2(ip);
      machineIPs.append4(this->code.length());

      if (lineNumberIndex >= 0) {
        object lnt = codeLineNumberTable(t, code);
        LineNumber* ln = lineNumberTableBody(t, lnt, lineNumberIndex);

        if (lineNumberIp(t, ln) == ip) {
          lineNumbers.append4(this->code.length());
          lineNumbers.append4(lineNumberLine(t, ln));
          if (lineNumberIndex < lineNumberTableLength(t, lnt)) {
            ++ lineNumberIndex;
          }
        }
      }

      unsigned instruction = codeBody(t, code, ip++);

      switch (instruction) {
      case aaload:
      case baload:
      case caload:
      case daload:
      case faload:
      case iaload:
      case laload:
      case saload: {
        Label next(this);
        Label outOfBounds(this);

        pop(rcx);
        pop(rax);

        cmp(0, rcx);
        jl(outOfBounds);

        mov(rax, BytesPerWord, rdx);
        cmp(rdx, rcx);
        jge(outOfBounds);

        add(BytesPerWord * 2, rax);

        switch (instruction) {
        case aaload:
        case faload:
        case iaload:
          shl(log(BytesPerWord), rcx);
          add(rcx, rax);
          push(rax, 0);
          break;

        case baload:
          add(rcx, rax);
          movs1(rax, 0, rax);
          push(rax);
          break;

        case caload:
          shl(1, rcx);
          add(rcx, rax);
          movz2(rax, 0, rax);
          push(rax);
          break;

        case daload:
        case laload:
          shl(3, rcx);
          add(rcx, rax);
          push4(rax, 0);
          push4(rax, 4);
          break;

        case saload:
          shl(1, rcx);
          add(rcx, rax);
          movs2(rax, 0, rax);
          push(rax);
          break;
        }

        jmp(next);

        outOfBounds.mark();
        compileCall
          (throwNew,
           arrayBody
           (t, t->m->types, Machine::ArrayIndexOutOfBoundsExceptionType));

        next.mark();
      } break;

      case aastore:
      case bastore:
      case castore:
      case dastore:
      case fastore:
      case iastore:
      case lastore:
      case sastore: {
        Label next(this);
        Label outOfBounds(this);

        if (instruction == dastore or instruction == lastore) {
          pop(rdx);
        }
        pop(rbx);
        pop(rcx);
        pop(rax);

        cmp(0, rcx);
        jl(outOfBounds);

        mov(rax, BytesPerWord, rsi);
        cmp(rsi, rcx);
        jge(outOfBounds);

        add(BytesPerWord * 2, rax);

        switch (instruction) {
        case aastore:
        case fastore:
        case iastore:
          shl(log(BytesPerWord), rcx);
          add(rcx, rax);
          mov(rbx, rax, 0);
          break;

        case bastore:
          add(rcx, rax);
          mov1(rbx, rax, 0);
          break;

        case castore:
        case sastore:
          shl(1, rcx);
          add(rcx, rax);
          mov2(rbx, rax, 0);
          break;

        case dastore:
        case lastore:
          shl(3, rcx);
          add(rcx, rax);
          mov4(rbx, rax, 0);
          mov4(rdx, rax, 4);
          break;
        }

        jmp(next);

        outOfBounds.mark();
        compileCall
          (throwNew,
           arrayBody
           (t, t->m->types, Machine::ArrayIndexOutOfBoundsExceptionType));

        next.mark();
      } break;

      case aconst_null:
        push(0);
        break;

      case aload:
      case iload:
      case fload:
        push(rbp, localOffset(codeBody(t, code, ip++), parameterFootprint));
        break;

      case aload_0:
      case iload_0:
      case fload_0:
        push(rbp, localOffset(0, parameterFootprint));
        break;

      case aload_1:
      case iload_1:
      case fload_1:
        push(rbp, localOffset(1, parameterFootprint));
        break;

      case aload_2:
      case iload_2:
      case fload_2:
        push(rbp, localOffset(2, parameterFootprint));
        break;

      case aload_3:
      case iload_3:
      case fload_3:
        push(rbp, localOffset(3, parameterFootprint));
        break;

      case anewarray: {
        uint16_t index = codeReadInt16(t, code, ip);
      
        object class_ = resolveClass(t, codePool(t, code), index - 1);
        if (UNLIKELY(t->exception)) return;

        Label nonnegative(this);

        pop(rax);
        cmp(0, rax);
        jle(nonnegative);

        compileCall
          (throwNew,
           arrayBody(t, t->m->types, Machine::NegativeArraySizeExceptionType));

        nonnegative.mark();
        compileCall(makeBlankObjectArray, class_, rax);
        push(rax);
      } break;

      case areturn:
      case ireturn:
      case freturn:
        pop(rax);
        mov(rbp, rsp);
        pop(rbp);
        ret();
        break;

      case arraylength:
        pop(rax);
        push(rax, BytesPerWord);
        break;

      case astore:
      case istore:
      case fstore:
        pop(rbp, localOffset(codeBody(t, code, ip++), parameterFootprint));
        break;

      case astore_0:
      case istore_0:
      case fstore_0:
        pop(rbp, localOffset(0, parameterFootprint));
        break;

      case astore_1:
      case istore_1:
      case fstore_1:
        pop(rbp, localOffset(1, parameterFootprint));
        break;

      case astore_2:
      case istore_2:
      case fstore_2:
        pop(rbp, localOffset(2, parameterFootprint));
        break;

      case astore_3:
      case istore_3:
      case fstore_3:
        pop(rbp, localOffset(3, parameterFootprint));
        break;

      case athrow:
        pop(rax);
        compileCall(throw_, rax);      
        break;

      case bipush: {
        push(static_cast<int8_t>(codeBody(t, code, ip++)));
      } break;

      case checkcast: {
        uint16_t index = codeReadInt16(t, code, ip);

        object class_ = resolveClass(t, codePool(t, code), index - 1);
        if (UNLIKELY(t->exception)) return;

        Label next(this);
        
        mov(rsp, 0, rax);
        cmp(0, rax);
        je(next);

        mov(poolRegister(), poolReference(class_), rcx);
        mov(rax, 0, rax);
        cmp(rcx, rax);
        je(next);

        compileCall(isAssignableFrom, rcx, rax);
        cmp(0, rax);
        jne(next);
        
        compileCall
          (throwNew,
           arrayBody(t, t->m->types, Machine::ClassCastExceptionType));

        next.mark();        
      } break;

      case dup:
        push(rsp, 0);
        break;

      case getfield: {
        uint16_t index = codeReadInt16(t, code, ip);
        
        object field = resolveField(t, codePool(t, code), index - 1);
        if (UNLIKELY(t->exception)) return;
      
        pop(rax);

        switch (fieldCode(t, field)) {
        case ByteField:
        case BooleanField:
          movs1(rax, fieldOffset(t, field), rax);
          push(rax);
          break;

        case CharField:
          movz2(rax, fieldOffset(t, field), rax);
          push(rax);
          break;

        case ShortField:
          movs2(rax, fieldOffset(t, field), rax);
          push(rax);
          break;

        case FloatField:
        case IntField:
          push4(rax, fieldOffset(t, field));
          break;

        case DoubleField:
        case LongField:
          push4(rax, fieldOffset(t, field));
          push4(rax, fieldOffset(t, field) + 4);
          break;

        case ObjectField:
          push(rax, fieldOffset(t, field));
          break;

        default:
          abort(t);
        }
      } break;

      case getstatic: {
        uint16_t index = codeReadInt16(t, code, ip);
        
        object field = resolveField(t, codePool(t, code), index - 1);
        if (UNLIKELY(t->exception)) return;
        PROTECT(t, field);
        
        initClass(t, fieldClass(t, field));
        if (UNLIKELY(t->exception)) return;
        
        object table = classStaticTable(t, fieldClass(t, field));

        mov(poolRegister(), poolReference(table), rax);
        add((fieldOffset(t, field) * BytesPerWord) + ArrayBody, rax);
        
        switch (fieldCode(t, field)) {
        case ByteField:
        case BooleanField:
        case CharField:
        case ShortField:
        case FloatField:
        case IntField: {
          Label zero(this);
          Label next(this);

          cmp(0, rax);
          je(zero);

          push4(rax, IntValue);
          jmp(next);

          zero.mark();
          push(0);

          next.mark();
        } break;

        case DoubleField:
        case LongField: {
          Label zero(this);
          Label next(this);

          cmp(0, rax);
          je(zero);

          push4(rax, LongValue);
          push4(rax, LongValue + 4);
          jmp(next);

          zero.mark();
          push(0);
          push(0);

          next.mark();
        } break;

        case ObjectField: {
          push(rax, 0);
        } break;

        default: abort(t);
        }
      } break;

      case goto_: {
        int16_t offset = codeReadInt16(t, code, ip);
        jmp((ip - 3) + offset);
      } break;

      case goto_w: {
        int32_t offset = codeReadInt32(t, code, ip);
        jmp((ip - 5) + offset);
      } break;

      case i2b:
        mov(rsp, 0, rax);
        movs1(rax, rax);
        mov(rax, rsp, 0);
        break;

      case i2c:
        mov(rsp, 0, rax);
        movz2(rax, rax);
        mov(rax, rsp, 0);
        break;

      case i2s:
        mov(rsp, 0, rax);
        movs2(rax, rax);
        mov(rax, rsp, 0);
        break;

      case iadd:
        pop(rax);
        pop(rcx);
        add(rax, rcx);
        push(rcx);
        break;

      case iconst_m1:
        push(-1);
        break;

      case iconst_0:
        push(0);
        break;

      case iconst_1:
        push(1);
        break;

      case iconst_2:
        push(2);
        break;

      case iconst_3:
        push(3);
        break;

      case iconst_4:
        push(4);
        break;

      case iconst_5:
        push(5);
        break;

      case if_acmpeq:
      case if_icmpeq: {
        int16_t offset = codeReadInt16(t, code, ip);
        
        pop(rax);
        pop(rcx);
        cmp(rax, rcx);
        je((ip - 3) + offset);
      } break;

      case if_acmpne:
      case if_icmpne: {
        int16_t offset = codeReadInt16(t, code, ip);
        
        pop(rax);
        pop(rcx);
        cmp(rax, rcx);
        jne((ip - 3) + offset);
      } break;

      case if_icmpgt: {
        int16_t offset = codeReadInt16(t, code, ip);
        
        pop(rax);
        pop(rcx);
        cmp(rax, rcx);
        jg((ip - 3) + offset);
      } break;

      case if_icmpge: {
        int16_t offset = codeReadInt16(t, code, ip);
        
        pop(rax);
        pop(rcx);
        cmp(rax, rcx);
        jge((ip - 3) + offset);
      } break;

      case if_icmplt: {
        int16_t offset = codeReadInt16(t, code, ip);
        
        pop(rax);
        pop(rcx);
        cmp(rax, rcx);
        jl((ip - 3) + offset);
      } break;

      case if_icmple: {
        int16_t offset = codeReadInt16(t, code, ip);
        
        pop(rax);
        pop(rcx);
        cmp(rax, rcx);
        jle((ip - 3) + offset);
      } break;

      case ifeq:
      case ifnull: {
        int16_t offset = codeReadInt16(t, code, ip);
        
        pop(rax);
        cmp(0, rax);
        je((ip - 3) + offset);
      } break;

      case ifne:
      case ifnonnull: {
        int16_t offset = codeReadInt16(t, code, ip);
        
        pop(rax);
        cmp(0, rax);
        jne((ip - 3) + offset);
      } break;

      case ifgt: {
        int16_t offset = codeReadInt16(t, code, ip);
        
        pop(rax);
        cmp(0, rax);
        jg((ip - 3) + offset);
      } break;

      case ifge: {
        int16_t offset = codeReadInt16(t, code, ip);
        
        pop(rax);
        cmp(0, rax);
        jge((ip - 3) + offset);
      } break;

      case iflt: {
        int16_t offset = codeReadInt16(t, code, ip);
        
        pop(rax);
        cmp(0, rax);
        jl((ip - 3) + offset);
      } break;

      case ifle: {
        int16_t offset = codeReadInt16(t, code, ip);
        
        pop(rax);
        cmp(0, rax);
        jle((ip - 3) + offset);
      } break;

      case iinc: {
        uint8_t index = codeBody(t, code, ip++);
        int8_t c = codeBody(t, code, ip++);
    
        add(c, rbp, localOffset(index, parameterFootprint));
      } break;

      case instanceof: {
        uint16_t index = codeReadInt16(t, code, ip);

        object class_ = resolveClass(t, codePool(t, code), index - 1);
        if (UNLIKELY(t->exception)) return;

        Label call(this);
        Label zero(this);
        Label next(this);
        
        pop(rax);
        cmp(0, rax);
        je(zero);

        mov(poolRegister(), poolReference(class_), rcx);
        mov(rax, 0, rax);
        cmp(rcx, rax);
        jne(call);
        
        push(1);
        jmp(next);

        call.mark();
        compileCall(isAssignableFrom, rcx, rax);
        push(rax);
        jmp(next);

        zero.mark();
        push(0);

        next.mark();
      } break;

      case invokespecial: {
        uint16_t index = codeReadInt16(t, code, ip);

        object target = resolveMethod(t, codePool(t, code), index - 1);
        if (UNLIKELY(t->exception)) return;

        object class_ = methodClass(t, target);
        if (isSpecialMethod(t, target, class_)) {
          target = findMethod(t, target, classSuper(t, class_));
        }

        compileDirectInvoke(t, target);
      } break;

      case invokestatic: {
        uint16_t index = codeReadInt16(t, code, ip);

        object target = resolveMethod(t, codePool(t, code), index - 1);
        if (UNLIKELY(t->exception)) return;
        PROTECT(t, target);

        initClass(t, methodClass(t, target));
        if (UNLIKELY(t->exception)) return;

        compileDirectInvoke(t, target);
      } break;

      case invokevirtual: {
        uint16_t index = codeReadInt16(t, code, ip);
        
        object target = resolveMethod(t, codePool(t, code), index - 1);
        if (UNLIKELY(t->exception)) return;

        unsigned parameterFootprint
          = methodParameterFootprint(t, target) * BytesPerWord;

        unsigned instance = parameterFootprint - BytesPerWord;

        unsigned footprint = FrameFootprint + parameterFootprint;

        unsigned offset = ArrayBody + (methodOffset(t, target) * BytesPerWord);
                
        mov(rsp, instance, rax);          // load instance
        mov(rax, 0, rax);                 // load class
        mov(rax, ClassVirtualTable, rax); // load vtable
        mov(rax, offset, rax);            // load method

        push(rsp);
        push(rax);
        push(rbp, FrameThread);

        mov(rax, MethodCompiled, rax);    // load compiled code
        add(CompiledBody, rax);
        call(rax);                        // call compiled code
        poolRegisterClobbered = true;

        add(footprint, rsp);              // pop arguments

        pushReturnValue(t, methodReturnCode(t, target));
      } break;

      case isub:
        pop(rax);
        pop(rcx);
        sub(rax, rcx);
        push(rcx);
        break;

      case ldc:
      case ldc_w: {
        uint16_t index;

        if (instruction == ldc) {
          index = codeBody(t, code, ip++);
        } else {
          uint8_t index1 = codeBody(t, code, ip++);
          uint8_t index2 = codeBody(t, code, ip++);
          index = (index1 << 8) | index2;
        }

        object v = arrayBody(t, codePool(t, code), index - 1);

        if (objectClass(t, v) == arrayBody(t, t->m->types, Machine::IntType)) {
          push(intValue(t, v));
        } else if (objectClass(t, v)
                   == arrayBody(t, t->m->types, Machine::FloatType))
        {
          push(floatValue(t, v));
        } else if (objectClass(t, v)
                   == arrayBody(t, t->m->types, Machine::StringType))
        {
          push(poolRegister(), poolReference(v));
        } else {
          object class_ = resolveClass(t, codePool(t, code), index - 1);

          push(poolRegister(), poolReference(class_));
        }
      } break;

      case new_: {
        uint16_t index = codeReadInt16(t, code, ip);
        
        object class_ = resolveClass(t, codePool(t, code), index - 1);
        if (UNLIKELY(t->exception)) return;
        PROTECT(t, class_);
        
        initClass(t, class_);
        if (UNLIKELY(t->exception)) return;

        if (classVmFlags(t, class_) & WeakReferenceFlag) {
          compileCall(makeNewWeakReference, class_);
        } else {
          compileCall(makeNew, class_);
        }

        push(rax);
      } break;

      case newarray: {
        uint8_t type = codeBody(t, code, ip++);

        Label nonnegative(this);

        pop(rax);
        cmp(0, rax);
        jge(nonnegative);

        compileCall
          (throwNew,
           arrayBody
           (t, t->m->types, Machine::NegativeArraySizeExceptionType));

        nonnegative.mark();

        object (*constructor)(Thread*, uintptr_t, bool);
        switch (type) {
        case T_BOOLEAN:
          constructor = makeBooleanArray;
          break;

        case T_CHAR:
          constructor = makeCharArray;
          break;

        case T_FLOAT:
          constructor = makeFloatArray;
          break;

        case T_DOUBLE:
          constructor = makeDoubleArray;
          break;

        case T_BYTE:
          constructor = makeByteArray;
          break;

        case T_SHORT:
          constructor = makeShortArray;
          break;

        case T_INT:
          constructor = makeIntArray;
          break;

        case T_LONG:
          constructor = makeLongArray;
          break;

        default: abort(t);
        }

        compileCall(makeBlankArray, constructor, rax);
        push(rax);
      } break;

      case pop_: {
        add(BytesPerWord, rsp);
      } break;

      case putfield: {
        uint16_t index = codeReadInt16(t, code, ip);
    
        object field = resolveField(t, codePool(t, code), index - 1);
        if (UNLIKELY(t->exception)) return;
      
        switch (fieldCode(t, field)) {
        case ByteField:
        case BooleanField:
        case CharField:
        case ShortField:
        case FloatField:
        case IntField: {
          pop(rcx);
          pop(rax);
          switch (fieldCode(t, field)) {
          case ByteField:
          case BooleanField:
            mov1(rcx, rax, fieldOffset(t, field));
            break;
            
          case CharField:
          case ShortField:
            mov2(rcx, rax, fieldOffset(t, field));
            break;
            
          case FloatField:
          case IntField:
            mov4(rcx, rax, fieldOffset(t, field));
            break;
          }
        } break;

        case DoubleField:
        case LongField: {
          pop(rcx);
          pop(rdx);
          pop(rax);
          mov4(rcx, rax, fieldOffset(t, field));
          mov4(rdx, rax, fieldOffset(t, field) + 4);
        } break;

        case ObjectField: {
          pop(rcx);
          pop(rax);
          mov(rcx, rax, fieldOffset(t, field));
        } break;

        default: abort(t);
        }
      } break;

      case putstatic: {
        uint16_t index = codeReadInt16(t, code, ip);
        
        object field = resolveField(t, codePool(t, code), index - 1);
        if (UNLIKELY(t->exception)) return;

        initClass(t, fieldClass(t, field));
        if (UNLIKELY(t->exception)) return;
        
        object table = classStaticTable(t, fieldClass(t, field));

        mov(poolRegister(), poolReference(table), rax);
        add((fieldOffset(t, field) * BytesPerWord) + ArrayBody, rax);
        
        switch (fieldCode(t, field)) {
        case ByteField:
        case BooleanField:
        case CharField:
        case ShortField:
        case FloatField:
        case IntField: {
          compileCall(makeNew, arrayBody(t, t->m->types, Machine::IntType));

          pop4(rax, IntValue);
        } break;

        case DoubleField:
        case LongField: {
          compileCall(makeNew, arrayBody(t, t->m->types, Machine::LongType));

          pop4(rax, LongValue);
          pop4(rax, LongValue + 4);
        } break;

        case ObjectField:
          pop(rax, 0);
          break;

        default: abort(t);
        }
      } break;

      case return_:
        mov(rbp, rsp);
        pop(rbp);
        ret();
        break;

      case sipush: {
        push(static_cast<int16_t>(codeReadInt16(t, code, ip)));
      } break;

      default:
        abort(t);
      }
    }

    resolveJumps();
    buildExceptionHandlerTable(code);

    return finish();
  }

  uint32_t machineIpForJavaIp(uint16_t javaIP) {
    unsigned bottom = 0;
    unsigned top = javaIPs.length() / 2;
    bool success = false;
    for (unsigned span = top - bottom; span; span = top - bottom) {
      unsigned middle = bottom + (span / 2);
      uint32_t k = javaIPs.get2(middle * 2);

      if (javaIP < k) {
        top = middle;
      } else if (javaIP > k) {
        bottom = middle + 1;
      } else {
        return machineIPs.get4(middle * 4);
      }
    }
    abort(code.s);
  }

  void resolveJumps() {
    for (unsigned i = 0; i < jumps.length(); i += 8) {
      uint32_t ip = jumps.get4(i);
      uint32_t offset = jumps.get4(i + 4);

      code.set4(offset, machineIpForJavaIp(ip) - (offset + 4));
    }
  }

  void buildExceptionHandlerTable(object code) {
    PROTECT(t, code);

    object eht = codeExceptionHandlerTable(t, code);
    PROTECT(t, eht);

    for (unsigned i = 0; i < exceptionHandlerTableLength(t, eht); ++i) {
      ExceptionHandler* eh = exceptionHandlerTableBody(t, eht, i);
      
      exceptionHandlers.append4(machineIpForJavaIp(exceptionHandlerStart(eh)));
      exceptionHandlers.append4(machineIpForJavaIp(exceptionHandlerEnd(eh)));
      exceptionHandlers.append4(machineIpForJavaIp(exceptionHandlerIp(eh)));

      unsigned ct = exceptionHandlerCatchType(eh);
      object catchType;
      if (ct) {
        catchType = resolveClass
          (t, codePool(t, code), exceptionHandlerCatchType(eh) - 1);
      } else {
        catchType = 0;
      }

      exceptionHandlers.append4
        (catchType ? (poolReference(catchType) / BytesPerWord) - 1 : 0);
    }
  }

  Code* compileNativeInvoker() {
    push(rbp);
    mov(rsp, rbp);

    mov(rbp, FrameThread, rax);
    mov(rbp, rax, threadFrameOffset);      // set thread frame to current

    if (BytesPerWord == 4) {
      push(rbp, FrameMethod);
      push(rbp, FrameThread);
    } else {
      mov(rbp, FrameMethod, rsi);
      mov(rbp, FrameThread, rdi);
    }

    mov(reinterpret_cast<uintptr_t>(invokeNative), rax);
    call(rax);

    if (BytesPerWord == 4) {
      add(BytesPerWord * 2, rsp);
    }

    mov(rbp, rsp);
    pop(rbp);
    ret();

    return finish();
  }

  Code* compileStub() {
    push(rbp);
    mov(rsp, rbp);

    mov(rbp, FrameThread, rax);
    mov(rbp, rax, threadFrameOffset);      // set thread frame to current

    if (BytesPerWord == 4) {
      push(rbp, FrameMethod);
      push(rbp, FrameThread);
    } else {
      mov(rbp, FrameMethod, rsi);
      mov(rbp, FrameThread, rdi);
    }

    mov(reinterpret_cast<uintptr_t>(compileMethod), rax);
    call(rax);

    if (BytesPerWord == 4) {
      add(BytesPerWord * 2, rsp);
    }

    mov(rbp, FrameMethod, rax);
    mov(rax, MethodCompiled, rax);           // load compiled code

    mov(rbp, rsp);
    pop(rbp);
    
    add(CompiledBody, rax);
    jmp(rax);                                // call compiled code

    return finish();
  }

  Code* finish() {
    unsigned footprint = pad(code.length())
      + pad(lineNumbers.length())
      + pad(exceptionHandlers.length());

    return new (t->m->system->allocate(sizeof(Code) + footprint))
      Code(&code, &lineNumbers, &exceptionHandlers);
  }

  object makePool() {
    if (pool.length()) {
      object array = makeArray(t, pool.length() / BytesPerWord, false);
      pool.copyTo(&arrayBody(t, array));
      return array;
    } else {
      return 0;
    }
  }

  Register poolRegister() {
    return rdi;
  }

  uint32_t poolReference(object o) {
    if (poolRegisterClobbered) {
      mov(rbp, FrameMethod, rdi);
      mov(rdi, MethodCode, rdi);
      poolRegisterClobbered = false;
    }
    pool.appendAddress(o);
    return pool.length() + BytesPerWord;
  }

  void callAddress(void* function) {
    mov(reinterpret_cast<uintptr_t>(function), rax);
    call(rax);
    poolRegisterClobbered = true;
  }

  void callAlignedAddress(void* function) {
    alignedMov(reinterpret_cast<uintptr_t>(function), rax);
    call(rax);
    poolRegisterClobbered = true;
  }

  MyThread* t;
  unsigned threadFrameOffset;
  bool poolRegisterClobbered;
  Buffer javaIPs;
  Buffer machineIPs;
  Buffer lineNumbers;
  Buffer exceptionHandlers;
  Buffer pool;
};

void
compileMethod2(MyThread* t, object method)
{
  if (methodCompiled(t, method) == t->m->processor->methodStub(t)) {
    PROTECT(t, method);

    ACQUIRE(t, t->m->classLock);
    
    if (methodCompiled(t, method) == t->m->processor->methodStub(t)) {
      if (Verbose) {
        fprintf(stderr, "compiling %s.%s\n",
                &byteArrayBody(t, className(t, methodClass(t, method)), 0),
                &byteArrayBody(t, methodName(t, method), 0));
      }

      Compiler c(t);
      Code* code = c.compile(method);
    
      if (Verbose) {
        fprintf(stderr, "compiled %s.%s from %p to %p\n",
                &byteArrayBody(t, className(t, methodClass(t, method)), 0),
                &byteArrayBody(t, methodName(t, method), 0),
                code->code(),
                code->code() + code->codeLength());
      }

      set(t, methodCompiled(t, method), compiled);

      object pool = c.makePool();
      set(t, methodCode(t, method), pool);
    }
  }
}

void
updateCaller(MyThread* t, object method)
{
  uintptr_t stub = reinterpret_cast<uintptr_t>
    (&compiledBody(t, t->m->processor->methodStub(t), 0));

  Assembler a(t->m->system);
  a.mov(stub, rax);
  unsigned offset = a.code.length() - BytesPerWord;

  a.call(rax);

  uint8_t* caller = static_cast<uint8_t**>(t->frame)[1] - a.code.length();
  if (memcmp(a.code.data, caller, a.code.length()) == 0) {
    // it's a direct call - update caller to point to new code

    // address must be aligned on a word boundary for this write to
    // be atomic
    assert(t, reinterpret_cast<uintptr_t>(caller + offset)
           % BytesPerWord == 0);

    *reinterpret_cast<void**>(caller + offset)
      = &compiledBody(t, methodCompiled(t, method), 0);
  }
}

void
compileMethod(MyThread* t, object method)
{
  compileMethod2(t, method);

  if (UNLIKELY(t->exception)) {
    unwind(t);
  } else if (not methodVirtual(t, method)) {
    updateCaller(t, method);
  }
}

class ArgumentList {
 public:
  ArgumentList(Thread* t, uintptr_t* array, bool* objectMask, object this_,
               const char* spec, bool indirectObjects, va_list arguments):
    t(static_cast<MyThread*>(t)),
    next(this->t->argumentList),
    array(array),
    objectMask(objectMask),
    position(0)
  {
    this->t->argumentList = this;

    addInt(reinterpret_cast<uintptr_t>(t));
    addObject(0); // reserve space for method
    addInt(reinterpret_cast<uintptr_t>(this->t->frame));

    if (this_) {
      addObject(this_);
    }

    const char* s = spec;
    ++ s; // skip '('
    while (*s and *s != ')') {
      switch (*s) {
      case 'L':
        while (*s and *s != ';') ++ s;
        ++ s;

        if (indirectObjects) {
          object* v = va_arg(arguments, object*);
          addObject(v ? *v : 0);
        } else {
          addObject(va_arg(arguments, object));
        }
        break;

      case '[':
        while (*s == '[') ++ s;
        switch (*s) {
        case 'L':
          while (*s and *s != ';') ++ s;
          ++ s;
          break;

        default:
          ++ s;
          break;
        }

        if (indirectObjects) {
          object* v = va_arg(arguments, object*);
          addObject(v ? *v : 0);
        } else {
          addObject(va_arg(arguments, object));
        }
        break;
      
      case 'J':
      case 'D':
        ++ s;
        addLong(va_arg(arguments, uint64_t));
        break;
          
      default:
        ++ s;
        addInt(va_arg(arguments, uint32_t));
        break;
      }
    }    
  }

  ArgumentList(Thread* t, uintptr_t* array, bool* objectMask, object this_,
               const char* spec, object arguments):
    t(static_cast<MyThread*>(t)),
    next(this->t->argumentList),
    array(array),
    objectMask(objectMask),
    position(0)
  {
    this->t->argumentList = this;

    addInt(0); // reserve space for trace pointer
    addObject(0); // reserve space for method pointer

    if (this_) {
      addObject(this_);
    }

    unsigned index = 0;
    const char* s = spec;
    ++ s; // skip '('
    while (*s and *s != ')') {
      switch (*s) {
      case 'L':
        while (*s and *s != ';') ++ s;
        ++ s;
        addObject(objectArrayBody(t, arguments, index++));
        break;

      case '[':
        while (*s == '[') ++ s;
        switch (*s) {
        case 'L':
          while (*s and *s != ';') ++ s;
          ++ s;
          break;

        default:
          ++ s;
          break;
        }
        addObject(objectArrayBody(t, arguments, index++));
        break;
      
      case 'J':
      case 'D':
        ++ s;
        addLong(cast<int64_t>(objectArrayBody(t, arguments, index++),
                              BytesPerWord));
        break;

      default:
        ++ s;
        addInt(cast<int32_t>(objectArrayBody(t, arguments, index++),
                             BytesPerWord));
        break;
      }
    }
  }

  ~ArgumentList() {
    t->argumentList = next;
  }

  void addObject(object v) {
    array[position] = reinterpret_cast<uintptr_t>(v);
    objectMask[position] = true;
    ++ position;
  }

  void addInt(uint32_t v) {
    array[position] = v;
    objectMask[position] = false;
    ++ position;
  }

  void addLong(uint64_t v) {
    memcpy(array + position, &v, 8);
    objectMask[position] = false;
    objectMask[position] = false;
    position += 2;
  }

  MyThread* t;
  ArgumentList* next;
  uintptr_t* array;
  bool* objectMask;
  unsigned position;
};

object
invoke(Thread* thread, object method, ArgumentList* arguments)
{
  MyThread* t = static_cast<MyThread*>(thread);

  arguments->array[1] = reinterpret_cast<uintptr_t>(method);
  
  unsigned returnCode = methodReturnCode(t, method);
  unsigned returnType = fieldType(t, returnCode);

  void* frame = t->frame;
  Reference* reference = t->reference;

  uint64_t result = vmInvoke
    (&compiledBody(t, methodCompiled(t, method), 0), arguments->array,
     arguments->position * BytesPerWord, returnType);

  while (t->reference != reference) {
    dispose(t, t->reference);
  }
  t->frame = frame;

  object r;
  switch (returnCode) {
  case ByteField:
  case BooleanField:
  case CharField:
  case ShortField:
  case FloatField:
  case IntField:
    r = makeInt(t, result);
    break;

  case LongField:
  case DoubleField:
    r = makeLong(t, result);
    break;

  case ObjectField:
    r = (result == 0 ? 0 :
         *reinterpret_cast<object*>(static_cast<uintptr_t>(result)));
    break;

  case VoidField:
    r = 0;
    break;

  default:
    abort(t);
  };

  return r;
}

class MyProcessor: public Processor {
 public:
  MyProcessor(System* s):
    s(s),
    methodStub_(0),
    nativeInvoker_(0)
  { }

  virtual Thread*
  makeThread(Machine* m, object javaThread, Thread* parent)
  {
    return new (s->allocate(sizeof(MyThread))) MyThread(m, javaThread, parent);
  }

  virtual void*
  methodStub(Thread* t)
  {
    if (methodStub_ == 0) {
      Compiler c(static_cast<MyThread*>(t));
      methodStub_ = c.compileStub();
    }
    return methodStub_;
  }

  virtual void*
  nativeInvoker(Thread* t)
  {
    if (nativeInvoker_ == 0) {
      Compiler c(static_cast<MyThread*>(t));
      nativeInvoker_ = c.compileNativeInvoker();
    }
    return nativeInvoker_;
  }

  virtual unsigned
  parameterFootprint(vm::Thread*, const char* s, bool static_)
  {
    unsigned footprint = 0;
    ++ s; // skip '('
    while (*s and *s != ')') {
      switch (*s) {
      case 'L':
        while (*s and *s != ';') ++ s;
        ++ s;
        break;

      case '[':
        while (*s == '[') ++ s;
        switch (*s) {
        case 'L':
          while (*s and *s != ';') ++ s;
          ++ s;
          break;

        default:
          ++ s;
          break;
        }
        break;
      
      case 'J':
      case 'D':
        ++ s;
        if (BytesPerWord == 4) {
          ++ footprint;
        }
        break;

      default:
        ++ s;
        break;
      }

      ++ footprint;
    }

    if (not static_) {
      ++ footprint;
    }
    return footprint;
  }

  virtual void
  initClass(Thread* t, object c)
  {
    PROTECT(t, c);
    
    ACQUIRE(t, t->m->classLock);
    if (classVmFlags(t, c) & NeedInitFlag
        and (classVmFlags(t, c) & InitFlag) == 0)
    {
      classVmFlags(t, c) |= InitFlag;
      invoke(t, classInitializer(t, c), 0);
      if (t->exception) {
        t->exception = makeExceptionInInitializerError(t, t->exception);
      }
      classVmFlags(t, c) &= ~(NeedInitFlag | InitFlag);
    }
  }

  virtual void
  visitObjects(Thread* t, Heap::Visitor*)
  {
    abort(t);
  }

  virtual uintptr_t
  frameStart(Thread* vmt)
  {
    return static_cast<MyThread*>(vmt)->frame;
  }

  virtual uintptr_t
  frameNext(Thread*, uintptr_t frame)
  {
    ::frameNext(reinterpret_cast<void*>(frame));
  }

  virtual bool
  frameValid(Thread*, uintptr_t frame)
  {
    return ::frameValid(frame);
  }

  virtual object
  frameMethod(Thread*, uintptr_t frame)
  {
    return ::frameMethod(frame);
  }

  virtual unsigned
  frameIp(Thread* t, uintptr_t frame)
  {
    return addressOffset(t, frameMethod(frame), frameAddress(frame));
  }

  virtual object*
  makeLocalReference(Thread* vmt, object o)
  {
    if (o) {
      MyThread* t = static_cast<MyThread*>(vmt);

      Reference* r = new (t->m->system->allocate(sizeof(Reference)))
        Reference(o, &(t->reference));

      return &(r->target);
    } else {
      return 0;
    }
  }

  virtual void
  disposeLocalReference(Thread* t, object* r)
  {
    if (r) {
      vm::dispose(t, reinterpret_cast<Reference*>(r));
    }
  }

  virtual object
  invokeArray(Thread* t, object method, object this_, object arguments)
  {
    assert(t, t->state == Thread::ActiveState
           or t->state == Thread::ExclusiveState);

    assert(t, ((methodFlags(t, method) & ACC_STATIC) == 0) xor (this_ == 0));

    const char* spec = reinterpret_cast<char*>
      (&byteArrayBody(t, methodSpec(t, method), 0));

    unsigned size = methodParameterFootprint(t, method) + FrameFootprint;
    uintptr_t array[size];
    bool objectMask[size];
    ArgumentList list(t, array, objectMask, this_, spec, arguments);
    
    return ::invoke(t, method, &list);
  }

  virtual object
  invokeList(Thread* t, object method, object this_, bool indirectObjects,
             va_list arguments)
  {
    assert(t, t->state == Thread::ActiveState
           or t->state == Thread::ExclusiveState);

    assert(t, ((methodFlags(t, method) & ACC_STATIC) == 0) xor (this_ == 0));
    
    const char* spec = reinterpret_cast<char*>
      (&byteArrayBody(t, methodSpec(t, method), 0));

    unsigned size = methodParameterFootprint(t, method) + FrameFootprint;
    uintptr_t array[size];
    bool objectMask[size];
    ArgumentList list
      (t, array, objectMask, this_, spec, indirectObjects, arguments);

    return ::invoke(t, method, &list);
  }

  virtual object
  invokeList(Thread* t, const char* className, const char* methodName,
             const char* methodSpec, object this_, va_list arguments)
  {
    assert(t, t->state == Thread::ActiveState
           or t->state == Thread::ExclusiveState);

    unsigned size = parameterFootprint(t, methodSpec, false) + FrameFootprint;
    uintptr_t array[size];
    bool objectMask[size];
    ArgumentList list
      (t, array, objectMask, this_, methodSpec, false, arguments);

    object method = resolveMethod(t, className, methodName, methodSpec);
    if (LIKELY(t->exception == 0)) {
      assert(t, ((methodFlags(t, method) & ACC_STATIC) == 0) xor (this_ == 0));

      return ::invoke(t, method, &list);
    } else {
      return 0;
    }
  }

  virtual void dispose() {
    if (methodStub_) {
      s->free(methodStub_);
    }

    if (nativeInvoker_) {
      s->free(nativeInvoker_);
    }

    s->free(this);
  }
  
  System* s;
  void* methodStub_;
  void* nativeInvoker_;
};

} // namespace

namespace vm {

Processor*
makeProcessor(System* system)
{
  return new (system->allocate(sizeof(MyProcessor))) MyProcessor(system);
}

} // namespace vm

