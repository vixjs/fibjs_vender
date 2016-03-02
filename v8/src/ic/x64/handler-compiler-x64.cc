#include "src/v8.h"

#if V8_TARGET_ARCH_X64

// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if V8_TARGET_ARCH_X64

#include "src/ic/call-optimization.h"
#include "src/ic/handler-compiler.h"
#include "src/ic/ic.h"
#include "src/isolate-inl.h"

namespace v8 {
namespace internal {

#define __ ACCESS_MASM(masm)

void PropertyHandlerCompiler::PushVectorAndSlot(Register vector,
                                                Register slot) {
  MacroAssembler* masm = this->masm();
  __ Push(vector);
  __ Push(slot);
}


void PropertyHandlerCompiler::PopVectorAndSlot(Register vector, Register slot) {
  MacroAssembler* masm = this->masm();
  __ Pop(slot);
  __ Pop(vector);
}


void PropertyHandlerCompiler::DiscardVectorAndSlot() {
  MacroAssembler* masm = this->masm();
  // Remove vector and slot.
  __ addp(rsp, Immediate(2 * kPointerSize));
}


void PropertyHandlerCompiler::GenerateDictionaryNegativeLookup(
    MacroAssembler* masm, Label* miss_label, Register receiver,
    Handle<Name> name, Register scratch0, Register scratch1) {
  DCHECK(name->IsUniqueName());
  DCHECK(!receiver.is(scratch0));
  Counters* counters = masm->isolate()->counters();
  __ IncrementCounter(counters->negative_lookups(), 1);
  __ IncrementCounter(counters->negative_lookups_miss(), 1);

  __ movp(scratch0, FieldOperand(receiver, HeapObject::kMapOffset));

  const int kInterceptorOrAccessCheckNeededMask =
      (1 << Map::kHasNamedInterceptor) | (1 << Map::kIsAccessCheckNeeded);

  // Bail out if the receiver has a named interceptor or requires access checks.
  __ testb(FieldOperand(scratch0, Map::kBitFieldOffset),
           Immediate(kInterceptorOrAccessCheckNeededMask));
  __ j(not_zero, miss_label);

  // Check that receiver is a JSObject.
  __ CmpInstanceType(scratch0, FIRST_JS_RECEIVER_TYPE);
  __ j(below, miss_label);

  // Load properties array.
  Register properties = scratch0;
  __ movp(properties, FieldOperand(receiver, JSObject::kPropertiesOffset));

  // Check that the properties array is a dictionary.
  __ CompareRoot(FieldOperand(properties, HeapObject::kMapOffset),
                 Heap::kHashTableMapRootIndex);
  __ j(not_equal, miss_label);

  Label done;
  NameDictionaryLookupStub::GenerateNegativeLookup(masm, miss_label, &done,
                                                   properties, name, scratch1);
  __ bind(&done);
  __ DecrementCounter(counters->negative_lookups_miss(), 1);
}


void NamedLoadHandlerCompiler::GenerateDirectLoadGlobalFunctionPrototype(
    MacroAssembler* masm, int index, Register result, Label* miss) {
  __ LoadNativeContextSlot(index, result);
  // Load its initial map. The global functions all have initial maps.
  __ movp(result,
          FieldOperand(result, JSFunction::kPrototypeOrInitialMapOffset));
  // Load the prototype from the initial map.
  __ movp(result, FieldOperand(result, Map::kPrototypeOffset));
}


void NamedLoadHandlerCompiler::GenerateLoadFunctionPrototype(
    MacroAssembler* masm, Register receiver, Register result, Register scratch,
    Label* miss_label) {
  __ TryGetFunctionPrototype(receiver, result, miss_label);
  if (!result.is(rax)) __ movp(rax, result);
  __ ret(0);
}


static void PushInterceptorArguments(MacroAssembler* masm, Register receiver,
                                     Register holder, Register name,
                                     Handle<JSObject> holder_obj) {
  STATIC_ASSERT(NamedLoadHandlerCompiler::kInterceptorArgsNameIndex == 0);
  STATIC_ASSERT(NamedLoadHandlerCompiler::kInterceptorArgsThisIndex == 1);
  STATIC_ASSERT(NamedLoadHandlerCompiler::kInterceptorArgsHolderIndex == 2);
  STATIC_ASSERT(NamedLoadHandlerCompiler::kInterceptorArgsLength == 3);
  __ Push(name);
  __ Push(receiver);
  __ Push(holder);
}


static void CompileCallLoadPropertyWithInterceptor(
    MacroAssembler* masm, Register receiver, Register holder, Register name,
    Handle<JSObject> holder_obj, Runtime::FunctionId id) {
  DCHECK(NamedLoadHandlerCompiler::kInterceptorArgsLength ==
         Runtime::FunctionForId(id)->nargs);
  PushInterceptorArguments(masm, receiver, holder, name, holder_obj);
  __ CallRuntime(id);
}


// Generate call to api function.
void PropertyHandlerCompiler::GenerateApiAccessorCall(
    MacroAssembler* masm, const CallOptimization& optimization,
    Handle<Map> receiver_map, Register receiver, Register scratch,
    bool is_store, Register store_parameter, Register accessor_holder,
    int accessor_index) {
  DCHECK(!accessor_holder.is(scratch));
  DCHECK(optimization.is_simple_api_call());

  __ PopReturnAddressTo(scratch);
  // receiver
  __ Push(receiver);
  // Write the arguments to stack frame.
  if (is_store) {
    DCHECK(!receiver.is(store_parameter));
    DCHECK(!scratch.is(store_parameter));
    __ Push(store_parameter);
  }
  __ PushReturnAddressFrom(scratch);
  // Stack now matches JSFunction abi.

  // Abi for CallApiFunctionStub.
  Register callee = rdi;
  Register data = rbx;
  Register holder = rcx;
  Register api_function_address = rdx;
  scratch = no_reg;

  // Put callee in place.
  __ LoadAccessor(callee, accessor_holder, accessor_index,
                  is_store ? ACCESSOR_SETTER : ACCESSOR_GETTER);

  // Put holder in place.
  CallOptimization::HolderLookup holder_lookup;
  int holder_depth = 0;
  optimization.LookupHolderOfExpectedType(receiver_map, &holder_lookup,
                                          &holder_depth);
  switch (holder_lookup) {
    case CallOptimization::kHolderIsReceiver:
      __ Move(holder, receiver);
      break;
    case CallOptimization::kHolderFound:
      __ movp(holder, FieldOperand(receiver, HeapObject::kMapOffset));
      __ movp(holder, FieldOperand(holder, Map::kPrototypeOffset));
      for (int i = 1; i < holder_depth; i++) {
        __ movp(holder, FieldOperand(holder, HeapObject::kMapOffset));
        __ movp(holder, FieldOperand(holder, Map::kPrototypeOffset));
      }
      break;
    case CallOptimization::kHolderNotFound:
      UNREACHABLE();
      break;
  }

  Isolate* isolate = masm->isolate();
  Handle<CallHandlerInfo> api_call_info = optimization.api_call_info();
  bool call_data_undefined = false;
  // Put call data in place.
  if (api_call_info->data()->IsUndefined()) {
    call_data_undefined = true;
    __ LoadRoot(data, Heap::kUndefinedValueRootIndex);
  } else {
    if (optimization.is_constant_call()) {
      __ movp(data,
              FieldOperand(callee, JSFunction::kSharedFunctionInfoOffset));
      __ movp(data,
              FieldOperand(data, SharedFunctionInfo::kFunctionDataOffset));
      __ movp(data, FieldOperand(data, FunctionTemplateInfo::kCallCodeOffset));
    } else {
      __ movp(data,
              FieldOperand(callee, FunctionTemplateInfo::kCallCodeOffset));
    }
    __ movp(data, FieldOperand(data, CallHandlerInfo::kDataOffset));
  }

  if (api_call_info->fast_handler()->IsCode()) {
    // Just tail call into the fast handler if present.
    __ Jump(handle(Code::cast(api_call_info->fast_handler())),
            RelocInfo::CODE_TARGET);
    return;
  }

  // Put api_function_address in place.
  Address function_address = v8::ToCData<Address>(api_call_info->callback());
  __ Move(api_function_address, function_address,
          RelocInfo::EXTERNAL_REFERENCE);

  // Jump to stub.
  CallApiAccessorStub stub(isolate, is_store, call_data_undefined,
                           !optimization.is_constant_call());
  __ TailCallStub(&stub);
}


void PropertyHandlerCompiler::GenerateCheckPropertyCell(
    MacroAssembler* masm, Handle<JSGlobalObject> global, Handle<Name> name,
    Register scratch, Label* miss) {
  Handle<PropertyCell> cell = JSGlobalObject::EnsurePropertyCell(global, name);
  DCHECK(cell->value()->IsTheHole());
  Factory* factory = masm->isolate()->factory();
  Handle<WeakCell> weak_cell = factory->NewWeakCell(cell);
  __ LoadWeakValue(scratch, weak_cell, miss);
  __ Cmp(FieldOperand(scratch, PropertyCell::kValueOffset),
         factory->the_hole_value());
  __ j(not_equal, miss);
}


void NamedStoreHandlerCompiler::GenerateStoreViaSetter(
    MacroAssembler* masm, Handle<Map> map, Register receiver, Register holder,
    int accessor_index, int expected_arguments, Register scratch) {
  // ----------- S t a t e -------------
  //  -- rsp[0] : return address
  // -----------------------------------
  {
    FrameScope scope(masm, StackFrame::INTERNAL);

    // Save value register, so we can restore it later.
    __ Push(value());

    if (accessor_index >= 0) {
      DCHECK(!holder.is(scratch));
      DCHECK(!receiver.is(scratch));
      DCHECK(!value().is(scratch));
      // Call the JavaScript setter with receiver and value on the stack.
      if (map->IsJSGlobalObjectMap()) {
        // Swap in the global receiver.
        __ movp(scratch,
                FieldOperand(receiver, JSGlobalObject::kGlobalProxyOffset));
        receiver = scratch;
      }
      __ Push(receiver);
      __ Push(value());
      ParameterCount actual(1);
      ParameterCount expected(expected_arguments);
      __ LoadAccessor(rdi, holder, accessor_index, ACCESSOR_SETTER);
      __ InvokeFunction(rdi, no_reg, expected, actual, CALL_FUNCTION,
                        CheckDebugStepCallWrapper());
    } else {
      // If we generate a global code snippet for deoptimization only, remember
      // the place to continue after deoptimization.
      masm->isolate()->heap()->SetSetterStubDeoptPCOffset(masm->pc_offset());
    }

    // We have to return the passed value, not the return value of the setter.
    __ Pop(rax);

    // Restore context register.
    __ movp(rsi, Operand(rbp, StandardFrameConstants::kContextOffset));
  }
  __ ret(0);
}


void NamedLoadHandlerCompiler::GenerateLoadViaGetter(
    MacroAssembler* masm, Handle<Map> map, Register receiver, Register holder,
    int accessor_index, int expected_arguments, Register scratch) {
  // ----------- S t a t e -------------
  //  -- rax    : receiver
  //  -- rcx    : name
  //  -- rsp[0] : return address
  // -----------------------------------
  {
    FrameScope scope(masm, StackFrame::INTERNAL);

    if (accessor_index >= 0) {
      DCHECK(!holder.is(scratch));
      DCHECK(!receiver.is(scratch));
      // Call the JavaScript getter with the receiver on the stack.
      if (map->IsJSGlobalObjectMap()) {
        // Swap in the global receiver.
        __ movp(scratch,
                FieldOperand(receiver, JSGlobalObject::kGlobalProxyOffset));
        receiver = scratch;
      }
      __ Push(receiver);
      ParameterCount actual(0);
      ParameterCount expected(expected_arguments);
      __ LoadAccessor(rdi, holder, accessor_index, ACCESSOR_GETTER);
      __ InvokeFunction(rdi, no_reg, expected, actual, CALL_FUNCTION,
                        CheckDebugStepCallWrapper());
    } else {
      // If we generate a global code snippet for deoptimization only, remember
      // the place to continue after deoptimization.
      masm->isolate()->heap()->SetGetterStubDeoptPCOffset(masm->pc_offset());
    }

    // Restore context register.
    __ movp(rsi, Operand(rbp, StandardFrameConstants::kContextOffset));
  }
  __ ret(0);
}


static void StoreIC_PushArgs(MacroAssembler* masm) {
  Register receiver = StoreDescriptor::ReceiverRegister();
  Register name = StoreDescriptor::NameRegister();
  Register value = StoreDescriptor::ValueRegister();

  Register slot = VectorStoreICDescriptor::SlotRegister();
  Register vector = VectorStoreICDescriptor::VectorRegister();

  __ PopReturnAddressTo(r11);
  __ Push(receiver);
  __ Push(name);
  __ Push(value);
  __ Push(slot);
  __ Push(vector);
  __ PushReturnAddressFrom(r11);
}


void NamedStoreHandlerCompiler::GenerateSlow(MacroAssembler* masm) {
  // Return address is on the stack.
  StoreIC_PushArgs(masm);

  // Do tail-call to runtime routine.
  __ TailCallRuntime(Runtime::kStoreIC_Slow);
}


void ElementHandlerCompiler::GenerateStoreSlow(MacroAssembler* masm) {
  // Return address is on the stack.
  StoreIC_PushArgs(masm);

  // Do tail-call to runtime routine.
  __ TailCallRuntime(Runtime::kKeyedStoreIC_Slow);
}


#undef __
#define __ ACCESS_MASM((masm()))


void NamedStoreHandlerCompiler::GenerateRestoreName(Label* label,
                                                    Handle<Name> name) {
  if (!label->is_unused()) {
    __ bind(label);
    __ Move(this->name(), name);
  }
}


void NamedStoreHandlerCompiler::GenerateRestoreName(Handle<Name> name) {
  __ Move(this->name(), name);
}


void NamedStoreHandlerCompiler::RearrangeVectorAndSlot(
    Register current_map, Register destination_map) {
  DCHECK(false);  // Not implemented.
}


void NamedStoreHandlerCompiler::GenerateRestoreMap(Handle<Map> transition,
                                                   Register map_reg,
                                                   Register scratch,
                                                   Label* miss) {
  Handle<WeakCell> cell = Map::WeakCellForMap(transition);
  DCHECK(!map_reg.is(scratch));
  __ LoadWeakValue(map_reg, cell, miss);
  if (transition->CanBeDeprecated()) {
    __ movl(scratch, FieldOperand(map_reg, Map::kBitField3Offset));
    __ andl(scratch, Immediate(Map::Deprecated::kMask));
    __ j(not_zero, miss);
  }
}


void NamedStoreHandlerCompiler::GenerateConstantCheck(Register map_reg,
                                                      int descriptor,
                                                      Register value_reg,
                                                      Register scratch,
                                                      Label* miss_label) {
  DCHECK(!map_reg.is(scratch));
  DCHECK(!map_reg.is(value_reg));
  DCHECK(!value_reg.is(scratch));
  __ LoadInstanceDescriptors(map_reg, scratch);
  __ movp(scratch,
          FieldOperand(scratch, DescriptorArray::GetValueOffset(descriptor)));
  __ cmpp(value_reg, scratch);
  __ j(not_equal, miss_label);
}

void NamedStoreHandlerCompiler::GenerateFieldTypeChecks(FieldType* field_type,
                                                        Register value_reg,
                                                        Label* miss_label) {
  Register map_reg = scratch1();
  Register scratch = scratch2();
  DCHECK(!value_reg.is(map_reg));
  DCHECK(!value_reg.is(scratch));
  __ JumpIfSmi(value_reg, miss_label);
  if (field_type->IsClass()) {
    Label do_store;
    __ movp(map_reg, FieldOperand(value_reg, HeapObject::kMapOffset));
    __ CmpWeakValue(map_reg, Map::WeakCellForMap(field_type->AsClass()),
                    scratch);
    __ j(not_equal, miss_label);
  }
}


Register PropertyHandlerCompiler::CheckPrototypes(
    Register object_reg, Register holder_reg, Register scratch1,
    Register scratch2, Handle<Name> name, Label* miss, PrototypeCheckType check,
    ReturnHolder return_what) {
  Handle<Map> receiver_map = map();

  // Make sure there's no overlap between holder and object registers.
  DCHECK(!scratch1.is(object_reg) && !scratch1.is(holder_reg));
  DCHECK(!scratch2.is(object_reg) && !scratch2.is(holder_reg) &&
         !scratch2.is(scratch1));

  if (FLAG_eliminate_prototype_chain_checks) {
    Handle<Cell> validity_cell =
        Map::GetOrCreatePrototypeChainValidityCell(receiver_map, isolate());
    if (!validity_cell.is_null()) {
      DCHECK_EQ(Smi::FromInt(Map::kPrototypeChainValid),
                validity_cell->value());
      __ Move(scratch1, validity_cell, RelocInfo::CELL);
      // Move(..., CELL) loads the payload's address!
      __ SmiCompare(Operand(scratch1, 0),
                    Smi::FromInt(Map::kPrototypeChainValid));
      __ j(not_equal, miss);
    }

    // The prototype chain of primitives (and their JSValue wrappers) depends
    // on the native context, which can't be guarded by validity cells.
    // |object_reg| holds the native context specific prototype in this case;
    // we need to check its map.
    if (check == CHECK_ALL_MAPS) {
      __ movp(scratch1, FieldOperand(object_reg, HeapObject::kMapOffset));
      Handle<WeakCell> cell = Map::WeakCellForMap(receiver_map);
      __ CmpWeakValue(scratch1, cell, scratch2);
      __ j(not_equal, miss);
    }
  }

  // Keep track of the current object in register reg.  On the first
  // iteration, reg is an alias for object_reg, on later iterations,
  // it is an alias for holder_reg.
  Register reg = object_reg;
  int depth = 0;

  Handle<JSObject> current = Handle<JSObject>::null();
  if (receiver_map->IsJSGlobalObjectMap()) {
    current = isolate()->global_object();
  }

  // Check access rights to the global object.  This has to happen after
  // the map check so that we know that the object is actually a global
  // object.
  // This allows us to install generated handlers for accesses to the
  // global proxy (as opposed to using slow ICs). See corresponding code
  // in LookupForRead().
  if (receiver_map->IsJSGlobalProxyMap()) {
    __ CheckAccessGlobalProxy(reg, scratch2, miss);
  }

  Handle<JSObject> prototype = Handle<JSObject>::null();
  Handle<Map> current_map = receiver_map;
  Handle<Map> holder_map(holder()->map());
  // Traverse the prototype chain and check the maps in the prototype chain for
  // fast and global objects or do negative lookup for normal objects.
  while (!current_map.is_identical_to(holder_map)) {
    ++depth;

    // Only global objects and objects that do not require access
    // checks are allowed in stubs.
    DCHECK(current_map->IsJSGlobalProxyMap() ||
           !current_map->is_access_check_needed());

    prototype = handle(JSObject::cast(current_map->prototype()));
    if (current_map->is_dictionary_map() &&
        !current_map->IsJSGlobalObjectMap()) {
      DCHECK(!current_map->IsJSGlobalProxyMap());  // Proxy maps are fast.
      if (!name->IsUniqueName()) {
        DCHECK(name->IsString());
        name = factory()->InternalizeString(Handle<String>::cast(name));
      }
      DCHECK(current.is_null() ||
             current->property_dictionary()->FindEntry(name) ==
                 NameDictionary::kNotFound);

      if (FLAG_eliminate_prototype_chain_checks && depth > 1) {
        // TODO(jkummerow): Cache and re-use weak cell.
        __ LoadWeakValue(reg, isolate()->factory()->NewWeakCell(current), miss);
      }
      GenerateDictionaryNegativeLookup(masm(), miss, reg, name, scratch1,
                                       scratch2);

      if (!FLAG_eliminate_prototype_chain_checks) {
        __ movp(scratch1, FieldOperand(reg, HeapObject::kMapOffset));
        __ movp(holder_reg, FieldOperand(scratch1, Map::kPrototypeOffset));
      }
    } else {
      Register map_reg = scratch1;
      if (!FLAG_eliminate_prototype_chain_checks) {
        __ movp(map_reg, FieldOperand(reg, HeapObject::kMapOffset));
      }
      if (current_map->IsJSGlobalObjectMap()) {
        GenerateCheckPropertyCell(masm(), Handle<JSGlobalObject>::cast(current),
                                  name, scratch2, miss);
      } else if (!FLAG_eliminate_prototype_chain_checks &&
                 (depth != 1 || check == CHECK_ALL_MAPS)) {
        Handle<WeakCell> cell = Map::WeakCellForMap(current_map);
        __ CmpWeakValue(map_reg, cell, scratch2);
        __ j(not_equal, miss);
      }
      if (!FLAG_eliminate_prototype_chain_checks) {
        __ movp(holder_reg, FieldOperand(map_reg, Map::kPrototypeOffset));
      }
    }

    reg = holder_reg;  // From now on the object will be in holder_reg.
    // Go to the next object in the prototype chain.
    current = prototype;
    current_map = handle(current->map());
  }

  DCHECK(!current_map->IsJSGlobalProxyMap());

  // Log the check depth.
  LOG(isolate(), IntEvent("check-maps-depth", depth + 1));

  if (!FLAG_eliminate_prototype_chain_checks &&
      (depth != 0 || check == CHECK_ALL_MAPS)) {
    // Check the holder map.
    __ movp(scratch1, FieldOperand(reg, HeapObject::kMapOffset));
    Handle<WeakCell> cell = Map::WeakCellForMap(current_map);
    __ CmpWeakValue(scratch1, cell, scratch2);
    __ j(not_equal, miss);
  }

  bool return_holder = return_what == RETURN_HOLDER;
  if (FLAG_eliminate_prototype_chain_checks && return_holder && depth != 0) {
    __ LoadWeakValue(reg, isolate()->factory()->NewWeakCell(current), miss);
  }

  // Return the register containing the holder.
  return return_holder ? reg : no_reg;
}


void NamedLoadHandlerCompiler::FrontendFooter(Handle<Name> name, Label* miss) {
  if (!miss->is_unused()) {
    Label success;
    __ jmp(&success);
    __ bind(miss);
    if (IC::ICUseVector(kind())) {
      DCHECK(kind() == Code::LOAD_IC);
      PopVectorAndSlot();
    }
    TailCallBuiltin(masm(), MissBuiltin(kind()));
    __ bind(&success);
  }
}


void NamedStoreHandlerCompiler::FrontendFooter(Handle<Name> name, Label* miss) {
  if (!miss->is_unused()) {
    Label success;
    __ jmp(&success);
    GenerateRestoreName(miss, name);
    if (IC::ICUseVector(kind())) PopVectorAndSlot();
    TailCallBuiltin(masm(), MissBuiltin(kind()));
    __ bind(&success);
  }
}


void NamedLoadHandlerCompiler::GenerateLoadCallback(
    Register reg, Handle<AccessorInfo> callback) {
  DCHECK(!AreAliased(kScratchRegister, scratch2(), scratch3(), receiver()));
  DCHECK(!AreAliased(kScratchRegister, scratch2(), scratch3(), reg));

  // Insert additional parameters into the stack frame above return address.
  __ PopReturnAddressTo(scratch3());

  // Build v8::PropertyCallbackInfo::args_ array on the stack and push property
  // name below the exit frame to make GC aware of them.
  STATIC_ASSERT(PropertyCallbackArguments::kShouldThrowOnErrorIndex == 0);
  STATIC_ASSERT(PropertyCallbackArguments::kHolderIndex == 1);
  STATIC_ASSERT(PropertyCallbackArguments::kIsolateIndex == 2);
  STATIC_ASSERT(PropertyCallbackArguments::kReturnValueDefaultValueIndex == 3);
  STATIC_ASSERT(PropertyCallbackArguments::kReturnValueOffset == 4);
  STATIC_ASSERT(PropertyCallbackArguments::kDataIndex == 5);
  STATIC_ASSERT(PropertyCallbackArguments::kThisIndex == 6);
  STATIC_ASSERT(PropertyCallbackArguments::kArgsLength == 7);

  __ Push(receiver());  // receiver
  Handle<Object> data(callback->data(), isolate());
  if (data->IsUndefined() || data->IsSmi()) {
    __ Push(data);
  } else {
    Handle<WeakCell> cell =
        isolate()->factory()->NewWeakCell(Handle<HeapObject>::cast(data));
    // The callback is alive if this instruction is executed,
    // so the weak cell is not cleared and points to data.
    __ GetWeakValue(scratch2(), cell);
    __ Push(scratch2());
  }
  __ LoadRoot(kScratchRegister, Heap::kUndefinedValueRootIndex);
  __ Push(kScratchRegister);  // return value
  __ Push(kScratchRegister);  // return value default
  __ PushAddress(ExternalReference::isolate_address(isolate()));
  __ Push(reg);     // holder
  __ Push(Smi::FromInt(0));  // should_throw_on_error -> false

  __ Push(name());  // name
  __ PushReturnAddressFrom(scratch3());

  // Abi for CallApiGetter
  Register api_function_address = ApiGetterDescriptor::function_address();
  Address getter_address = v8::ToCData<Address>(callback->getter());
  __ Move(api_function_address, getter_address, RelocInfo::EXTERNAL_REFERENCE);

  CallApiGetterStub stub(isolate());
  __ TailCallStub(&stub);
}


void NamedLoadHandlerCompiler::GenerateLoadConstant(Handle<Object> value) {
  // Return the constant value.
  __ Move(rax, value);
  __ ret(0);
}


void NamedLoadHandlerCompiler::GenerateLoadInterceptorWithFollowup(
    LookupIterator* it, Register holder_reg) {
  DCHECK(holder()->HasNamedInterceptor());
  DCHECK(!holder()->GetNamedInterceptor()->getter()->IsUndefined());

  // Compile the interceptor call, followed by inline code to load the
  // property from further up the prototype chain if the call fails.
  // Check that the maps haven't changed.
  DCHECK(holder_reg.is(receiver()) || holder_reg.is(scratch1()));

  // Preserve the receiver register explicitly whenever it is different from the
  // holder and it is needed should the interceptor return without any result.
  // The ACCESSOR case needs the receiver to be passed into C++ code, the FIELD
  // case might cause a miss during the prototype check.
  bool must_perform_prototype_check =
      !holder().is_identical_to(it->GetHolder<JSObject>());
  bool must_preserve_receiver_reg =
      !receiver().is(holder_reg) &&
      (it->state() == LookupIterator::ACCESSOR || must_perform_prototype_check);

  // Save necessary data before invoking an interceptor.
  // Requires a frame to make GC aware of pushed pointers.
  {
    FrameScope frame_scope(masm(), StackFrame::INTERNAL);

    if (must_preserve_receiver_reg) {
      __ Push(receiver());
    }
    __ Push(holder_reg);
    __ Push(this->name());
    InterceptorVectorSlotPush(holder_reg);

    // Invoke an interceptor.  Note: map checks from receiver to
    // interceptor's holder has been compiled before (see a caller
    // of this method.)
    CompileCallLoadPropertyWithInterceptor(
        masm(), receiver(), holder_reg, this->name(), holder(),
        Runtime::kLoadPropertyWithInterceptorOnly);

    // Check if interceptor provided a value for property.  If it's
    // the case, return immediately.
    Label interceptor_failed;
    __ CompareRoot(rax, Heap::kNoInterceptorResultSentinelRootIndex);
    __ j(equal, &interceptor_failed);
    frame_scope.GenerateLeaveFrame();
    __ ret(0);

    __ bind(&interceptor_failed);
    InterceptorVectorSlotPop(holder_reg);
    __ Pop(this->name());
    __ Pop(holder_reg);
    if (must_preserve_receiver_reg) {
      __ Pop(receiver());
    }

    // Leave the internal frame.
  }

  GenerateLoadPostInterceptor(it, holder_reg);
}


void NamedLoadHandlerCompiler::GenerateLoadInterceptor(Register holder_reg) {
  // Call the runtime system to load the interceptor.
  DCHECK(holder()->HasNamedInterceptor());
  DCHECK(!holder()->GetNamedInterceptor()->getter()->IsUndefined());
  __ PopReturnAddressTo(scratch2());
  PushInterceptorArguments(masm(), receiver(), holder_reg, this->name(),
                           holder());
  __ PushReturnAddressFrom(scratch2());

  __ TailCallRuntime(Runtime::kLoadPropertyWithInterceptor);
}


Handle<Code> NamedStoreHandlerCompiler::CompileStoreCallback(
    Handle<JSObject> object, Handle<Name> name, Handle<AccessorInfo> callback,
    LanguageMode language_mode) {
  Register holder_reg = Frontend(name);

  __ PopReturnAddressTo(scratch1());
  __ Push(receiver());
  __ Push(holder_reg);
  // If the callback cannot leak, then push the callback directly,
  // otherwise wrap it in a weak cell.
  if (callback->data()->IsUndefined() || callback->data()->IsSmi()) {
    __ Push(callback);
  } else {
    Handle<WeakCell> cell = isolate()->factory()->NewWeakCell(callback);
    __ Push(cell);
  }
  __ Push(name);
  __ Push(value());
  __ Push(Smi::FromInt(language_mode));
  __ PushReturnAddressFrom(scratch1());

  // Do tail-call to the runtime system.
  __ TailCallRuntime(Runtime::kStoreCallbackProperty);

  // Return the generated code.
  return GetCode(kind(), Code::FAST, name);
}


Handle<Code> NamedStoreHandlerCompiler::CompileStoreInterceptor(
    Handle<Name> name) {
  __ PopReturnAddressTo(scratch1());
  __ Push(receiver());
  __ Push(this->name());
  __ Push(value());
  __ PushReturnAddressFrom(scratch1());

  // Do tail-call to the runtime system.
  __ TailCallRuntime(Runtime::kStorePropertyWithInterceptor);

  // Return the generated code.
  return GetCode(kind(), Code::FAST, name);
}


Register NamedStoreHandlerCompiler::value() {
  return StoreDescriptor::ValueRegister();
}


Handle<Code> NamedLoadHandlerCompiler::CompileLoadGlobal(
    Handle<PropertyCell> cell, Handle<Name> name, bool is_configurable) {
  Label miss;
  if (IC::ICUseVector(kind())) {
    PushVectorAndSlot();
  }
  FrontendHeader(receiver(), name, &miss, DONT_RETURN_ANYTHING);

  // Get the value from the cell.
  Register result = StoreDescriptor::ValueRegister();
  Handle<WeakCell> weak_cell = factory()->NewWeakCell(cell);
  __ LoadWeakValue(result, weak_cell, &miss);
  __ movp(result, FieldOperand(result, PropertyCell::kValueOffset));

  // Check for deleted property if property can actually be deleted.
  if (is_configurable) {
    __ CompareRoot(result, Heap::kTheHoleValueRootIndex);
    __ j(equal, &miss);
  } else if (FLAG_debug_code) {
    __ CompareRoot(result, Heap::kTheHoleValueRootIndex);
    __ Check(not_equal, kDontDeleteCellsCannotContainTheHole);
  }

  Counters* counters = isolate()->counters();
  __ IncrementCounter(counters->ic_named_load_global_stub(), 1);
  if (IC::ICUseVector(kind())) {
    DiscardVectorAndSlot();
  }
  __ ret(0);

  FrontendFooter(name, &miss);

  // Return the generated code.
  return GetCode(kind(), Code::NORMAL, name);
}


#undef __
}  // namespace internal
}  // namespace v8

#endif  // V8_TARGET_ARCH_X64


#endif  // V8_TARGET_ARCH_X64