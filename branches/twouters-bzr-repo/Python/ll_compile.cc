#include "Python/ll_compile.h"

#include "Python.h"
#include "code.h"
#include "opcode.h"
#include "frameobject.h"

#include "Util/TypeBuilder.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/BasicBlock.h"
#include "llvm/Constant.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Function.h"
#include "llvm/Instructions.h"
#include "llvm/Intrinsics.h"
#include "llvm/Module.h"
#include "llvm/Type.h"

#include <vector>

using llvm::BasicBlock;
using llvm::Constant;
using llvm::ConstantInt;
using llvm::Function;
using llvm::FunctionType;
using llvm::IntegerType;
using llvm::Module;
using llvm::PHINode;
using llvm::PointerType;
using llvm::Type;
using llvm::Value;
using llvm::array_endof;

namespace py {

static ConstantInt *
get_signed_constant_int(const Type *type, int64_t v)
{
    // This is an LLVM idiom. It expects an unsigned integer but does
    // different conversions internally depending on whether it was
    // originally signed or not.
    return ConstantInt::get(type, static_cast<uint64_t>(v), true /* signed */);
}

template<> class TypeBuilder<PyObject> {
public:
    static const Type *cache(Module *module) {
        std::string pyobject_name("__pyobject");
        const Type *result = module->getTypeByName(pyobject_name);
        if (result != NULL)
            return result;

        // Keep this in sync with object.h.
        llvm::PATypeHolder object_ty = llvm::OpaqueType::get();
        Type *p_object_ty = PointerType::getUnqual(object_ty);
        llvm::StructType *temp_object_ty = llvm::StructType::get(
            // Fields from PyObject_HEAD.
#ifdef Py_TRACE_REFS
            // _ob_next, _ob_prev
            p_object_ty, p_object_ty,
#endif
            TypeBuilder<ssize_t>::cache(module),
            p_object_ty,
            NULL);
	// Unifies the OpaqueType fields with the whole structure.  We
	// couldn't do that originally because the type's recursive.
        llvm::cast<llvm::OpaqueType>(object_ty.get())
            ->refineAbstractTypeTo(temp_object_ty);
        module->addTypeName(pyobject_name, object_ty.get());
        return object_ty.get();
    }

    enum Fields {
#ifdef Py_TRACE_REFS
        FIELD_NEXT,
        FIELD_PREV,
#endif
        FIELD_REFCNT,
        FIELD_TYPE,
    };
};
typedef TypeBuilder<PyObject> ObjectTy;

template<> class TypeBuilder<PyTupleObject> {
public:
    static const Type *cache(Module *module) {
        std::string pytupleobject_name("__pytupleobject");
        const Type *result = module->getTypeByName(pytupleobject_name);
        if (result != NULL)
            return result;

        // Keep this in sync with code.h.
        result = llvm::StructType::get(
            // From PyObject_HEAD. In C these are directly nested
            // fields, but the layout should be the same when it's
            // represented as a nested struct.
            TypeBuilder<PyObject>::cache(module),
            // From PyObject_VAR_HEAD
            TypeBuilder<ssize_t>::cache(module),
            // From PyTupleObject
            TypeBuilder<PyObject*[]>::cache(module),  // ob_item
            NULL);

        module->addTypeName(pytupleobject_name, result);
        return result;
    }

    enum Fields {
        FIELD_OBJECT,
        FIELD_SIZE,
        FIELD_ITEM,
    };
};
typedef TypeBuilder<PyTupleObject> TupleTy;

template<> class TypeBuilder<PyListObject> {
public:
    static const Type *cache(Module *module) {
        std::string pylistobject_name("__pylistobject");
        const Type *result = module->getTypeByName(pylistobject_name);
        if (result != NULL)
            return result;

        // Keep this in sync with code.h.
        result = llvm::StructType::get(
            // From PyObject_HEAD. In C these are directly nested
            // fields, but the layout should be the same when it's
            // represented as a nested struct.
            TypeBuilder<PyObject>::cache(module),
            // From PyObject_VAR_HEAD
            TypeBuilder<ssize_t>::cache(module),
            // From PyListObject
            TypeBuilder<PyObject**>::cache(module),  // ob_item
            TypeBuilder<Py_ssize_t>::cache(module),  // allocated
            NULL);

        module->addTypeName(pylistobject_name, result);
        return result;
    }

    enum Fields {
        FIELD_OBJECT,
        FIELD_SIZE,
        FIELD_ITEM,
        FIELD_ALLOCATED,
    };
};
typedef TypeBuilder<PyListObject> ListTy;

template<> class TypeBuilder<PyTypeObject> {
public:
    static const Type *cache(Module *module) {
        std::string pytypeobject_name("__pytypeobject");
        const Type *result = module->getTypeByName(pytypeobject_name);
        if (result != NULL)
            return result;

        // Keep this in sync with code.h.
        result = llvm::StructType::get(
            // From PyObject_HEAD. In C these are directly nested
            // fields, but the layout should be the same when it's
            // represented as a nested struct.
            TypeBuilder<PyObject>::cache(module),
            // From PyObject_VAR_HEAD
            TypeBuilder<ssize_t>::cache(module),
            // From PyTYPEObject
            TypeBuilder<const char *>::cache(module),  // tp_name
            TypeBuilder<Py_ssize_t>::cache(module),  // tp_basicsize
            TypeBuilder<Py_ssize_t>::cache(module),  // tp_itemsize
            TypeBuilder<destructor>::cache(module),  // tp_dealloc
            // tp_print
            TypeBuilder<int (*)(PyObject*, char*, int)>::cache(module),
            TypeBuilder<getattrfunc>::cache(module),  // tp_getattr
            TypeBuilder<setattrfunc>::cache(module),  // tp_setattr
            TypeBuilder<cmpfunc>::cache(module),  // tp_compare
            TypeBuilder<reprfunc>::cache(module),  // tp_repr
            TypeBuilder<char *>::cache(module),  // tp_as_number
            TypeBuilder<char *>::cache(module),  // tp_as_sequence
            TypeBuilder<char *>::cache(module),  // tp_as_mapping
            TypeBuilder<hashfunc>::cache(module),  // tp_hash
            TypeBuilder<ternaryfunc>::cache(module),  // tp_call
            TypeBuilder<reprfunc>::cache(module),  // tp_str
            TypeBuilder<getattrofunc>::cache(module),  // tp_getattro
            TypeBuilder<setattrofunc>::cache(module),  // tp_setattro
            TypeBuilder<char *>::cache(module),  // tp_as_buffer
            TypeBuilder<long>::cache(module),  // tp_flags
            TypeBuilder<const char *>::cache(module),  // tp_doc
            TypeBuilder<traverseproc>::cache(module),  // tp_traverse
            TypeBuilder<inquiry>::cache(module),  // tp_clear
            TypeBuilder<richcmpfunc>::cache(module),  // tp_richcompare
            TypeBuilder<Py_ssize_t>::cache(module),  // tp_weaklistoffset
            TypeBuilder<getiterfunc>::cache(module),  // tp_iter
            TypeBuilder<iternextfunc>::cache(module),  // tp_iternext
            TypeBuilder<char *>::cache(module),  // tp_methods
            TypeBuilder<char *>::cache(module),  // tp_members
            TypeBuilder<char *>::cache(module),  // tp_getset
            TypeBuilder<PyObject *>::cache(module),  // tp_base
            TypeBuilder<PyObject *>::cache(module),  // tp_dict
            TypeBuilder<descrgetfunc>::cache(module),  // tp_descr_get
            TypeBuilder<descrsetfunc>::cache(module),  // tp_descr_set
            TypeBuilder<Py_ssize_t>::cache(module),  // tp_dictoffset
            TypeBuilder<initproc>::cache(module),  // tp_init
            // Can't use newfunc or allocfunc because they refer to
            // PyTypeObject.
            TypeBuilder<PyObject *(*)(PyObject *,
                                      Py_ssize_t)>::cache(module),  // tp_alloc
            TypeBuilder<PyObject *(*)(PyObject *, PyObject *,
                                      PyObject *)>::cache(module),  // tp_new
            TypeBuilder<freefunc>::cache(module),  // tp_free
            TypeBuilder<inquiry>::cache(module),  // tp_is_gc
            TypeBuilder<PyObject *>::cache(module),  // tp_bases
            TypeBuilder<PyObject *>::cache(module),  // tp_mro
            TypeBuilder<PyObject *>::cache(module),  // tp_cache
            TypeBuilder<PyObject *>::cache(module),  // tp_subclasses
            TypeBuilder<PyObject *>::cache(module),  // tp_weaklist
            TypeBuilder<destructor>::cache(module),  // tp_del
            TypeBuilder<unsigned int>::cache(module),  // tp_version_tag
#ifdef COUNT_ALLOCS
            TypeBuilder<Py_ssize_t>::cache(module),  // tp_allocs
            TypeBuilder<Py_ssize_t>::cache(module),  // tp_frees
            TypeBuilder<Py_ssize_t>::cache(module),  // tp_maxalloc
            TypeBuilder<PyObject *>::cache(module),  // tp_prev
            TypeBuilder<PyObject *>::cache(module),  // tp_next
#endif
            NULL);

        module->addTypeName(pytypeobject_name, result);
        return result;
    }

    enum Fields {
        FIELD_OBJECT,
        FIELD_SIZE,
        FIELD_NAME,
        FIELD_BASICSIZE,
        FIELD_ITEMSIZE,
        FIELD_DEALLOC,
        FIELD_PRINT,
        FIELD_GETATTR,
        FIELD_SETATTR,
        FIELD_COMPARE,
        FIELD_REPR,
        FIELD_AS_NUMBER,
        FIELD_AS_SEQUENCE,
        FIELD_AS_MAPPING,
        FIELD_HASH,
        FIELD_CALL,
        FIELD_STR,
        FIELD_GETATTRO,
        FIELD_SETATTRO,
        FIELD_AS_BUFFER,
        FIELD_FLAGS,
        FIELD_DOC,
        FIELD_TRAVERSE,
        FIELD_CLEAR,
        FIELD_RICHCOMPARE,
        FIELD_WEAKLISTOFFSET,
        FIELD_ITER,
        FIELD_ITERNEXT,
        FIELD_METHODS,
        FIELD_MEMBERS,
        FIELD_GETSET,
        FIELD_BASE,
        FIELD_DICT,
        FIELD_DESCR_GET,
        FIELD_DESCR_SET,
        FIELD_DICTOFFSET,
        FIELD_INIT,
        FIELD_ALLOC,
        FIELD_NEW,
        FIELD_FREE,
        FIELD_IS_GC,
        FIELD_BASES,
        FIELD_MRO,
        FIELD_CACHE,
        FIELD_SUBCLASSES,
        FIELD_WEAKLIST,
        FIELD_DEL,
        FIELD_TP_VERSION_TAG,
#ifdef COUNT_ALLOCS
        FIELD_ALLOCS,
        FIELD_FREES,
        FIELD_MAXALLOC,
        FIELD_PREV,
        FIELD_NEXT,
#endif
    };
};
typedef TypeBuilder<PyTypeObject> TypeTy;

template<> class TypeBuilder<PyCodeObject> {
public:
    static const Type *cache(Module *module) {
        std::string pycodeobject_name("__pycodeobject");
        const Type *result = module->getTypeByName(pycodeobject_name);
        if (result != NULL)
            return result;

        // Keep this in sync with code.h.
        const Type *p_pyobject_type = TypeBuilder<PyObject*>::cache(module);
        const Type *int_type = TypeBuilder<int>::cache(module);
        result = llvm::StructType::get(
            // From PyObject_HEAD. In C these are directly nested
            // fields, but the layout should be the same when it's
            // represented as a nested struct.
            TypeBuilder<PyObject>::cache(module),
            // From PyCodeObject
            int_type,  // co_argcount
            int_type,  // co_nlocals
            int_type,  // co_stacksize
            int_type,  // co_flags
            p_pyobject_type,  // co_code
            p_pyobject_type,  // co_consts
            p_pyobject_type,  // co_names
            p_pyobject_type,  // co_varnames
            p_pyobject_type,  // co_freevars
            p_pyobject_type,  // co_cellvars
            //  Not bothering with defining the Inst struct.
            TypeBuilder<char*>::cache(module),  // co_tcode
            p_pyobject_type,  // co_filename
            p_pyobject_type,  // co_name
            int_type,  // co_firstlineno
            p_pyobject_type,  // co_lnotab
            TypeBuilder<char*>::cache(module),  //co_zombieframe
            p_pyobject_type,  // co_llvm_function
            NULL);

        module->addTypeName(pycodeobject_name, result);
        return result;
    }

    enum Fields {
        FIELD_OBJECT,
        FIELD_ARGCOUNT,
        FIELD_NLOCALS,
        FIELD_STACKSIZE,
        FIELD_FLAGS,
        FIELD_CODE,
        FIELD_CONSTS,
        FIELD_NAMES,
        FIELD_VARNAMES,
        FIELD_FREEVARS,
        FIELD_CELLVARS,
        FIELD_TCODE,
        FIELD_FILENAME,
        FIELD_NAME,
        FIELD_FIRSTLINENO,
        FIELD_LNOTAB,
        FIELD_ZOMBIEFRAME,
        FIELD_LLVM_FUNCTION,
    };
};
typedef TypeBuilder<PyCodeObject> CodeTy;

template<> class TypeBuilder<PyTryBlock> {
public:
    static const Type *cache(Module *module) {
        const Type *int_type = TypeBuilder<int>::cache(module);
        return llvm::StructType::get(
            // b_type, b_handler, b_level
            int_type, int_type, int_type, NULL);
    }
    enum Fields {
        FIELD_TYPE,
        FIELD_HANDLER,
        FIELD_LEVEL,
    };
};

template<> class TypeBuilder<PyFrameObject> {
public:
    static const Type *cache(Module *module) {
        std::string pyframeobject_name("__pyframeobject");
        const Type *result = module->getTypeByName(pyframeobject_name);
        if (result != NULL)
            return result;

        // Keep this in sync with frameobject.h.
        const Type *p_pyobject_type = TypeBuilder<PyObject*>::cache(module);
        const Type *int_type = TypeBuilder<int>::cache(module);
        result = llvm::StructType::get(
            // From PyObject_HEAD. In C these are directly nested
            // fields, but the layout should be the same when it's
            // represented as a nested struct.
            ObjectTy::cache(module),
            // From PyObject_VAR_HEAD
            TypeBuilder<ssize_t>::cache(module),
            // From struct _frame
            p_pyobject_type,  // f_back
            TypeBuilder<PyCodeObject*>::cache(module),  // f_code
            p_pyobject_type,  // f_builtins
            p_pyobject_type,  // f_globals
            p_pyobject_type,  // f_locals
            TypeBuilder<PyObject**>::cache(module),  // f_valuestack
            TypeBuilder<PyObject**>::cache(module),  // f_stacktop
            p_pyobject_type,  // f_trace
            p_pyobject_type,  // f_exc_type
            p_pyobject_type,  // f_exc_value
            p_pyobject_type,  // f_exc_traceback
            // f_tstate; punt on the type:
            TypeBuilder<char*>::cache(module),
            int_type,  // f_lasti
            int_type,  // f_lineno
            int_type,  // f_iblock
            // f_blockstack:
            TypeBuilder<PyTryBlock[CO_MAXBLOCKS]>::cache(module),
            // f_localsplus, flexible array.
            TypeBuilder<PyObject*[]>::cache(module),
            NULL);

        module->addTypeName(pyframeobject_name, result);
        return result;
    }

    enum Fields {
        FIELD_OBJECT_HEAD,
        FIELD_OB_SIZE,
        FIELD_BACK,
        FIELD_CODE,
        FIELD_BUILTINS,
        FIELD_GLOBALS,
        FIELD_LOCALS,
        FIELD_VALUESTACK,
        FIELD_STACKTOP,
        FIELD_TRACE,
        FIELD_EXC_TYPE,
        FIELD_EXC_VALUE,
        FIELD_EXC_TRACEBACK,
        FIELD_TSTATE,
        FIELD_LASTI,
        FIELD_LINENO,
        FIELD_IBLOCK,
        FIELD_BLOCKSTACK,
        FIELD_LOCALSPLUS,
    };
};
typedef TypeBuilder<PyFrameObject> FrameTy;

static const FunctionType *
get_function_type(Module *module)
{
    std::string function_type_name("__function_type");
    const FunctionType *result =
        llvm::cast_or_null<FunctionType>(
            module->getTypeByName(function_type_name));
    if (result != NULL)
        return result;

    result = TypeBuilder<PyObject*(PyFrameObject*)>::cache(module);
    module->addTypeName(function_type_name, result);
    return result;
}

LlvmFunctionBuilder::LlvmFunctionBuilder(
    Module *module, const std::string& name)
    : module_(module),
      function_(Function::Create(
                    get_function_type(module),
                    llvm::GlobalValue::ExternalLinkage,
                    name,
                    module))
{
    Function::arg_iterator args = function()->arg_begin();
    this->frame_ = args++;
    assert(args == function()->arg_end() &&
           "Unexpected number of arguments");
    this->frame_->setName("frame");

    builder().SetInsertPoint(BasicBlock::Create("entry", function()));
    this->return_block_ = BasicBlock::Create("return_block", function());

    this->stack_pointer_addr_ = builder().CreateAlloca(
        TypeBuilder<PyObject**>::cache(module),
        0, "stack_pointer_addr");
    this->retval_addr_ = builder().CreateAlloca(
        TypeBuilder<PyObject*>::cache(module),
        0, "retval_addr_");

    Value *initial_stack_pointer =
        builder().CreateLoad(
            builder().CreateStructGEP(this->frame_, FrameTy::FIELD_STACKTOP),
            "initial_stack_pointer");
    builder().CreateStore(initial_stack_pointer, this->stack_pointer_addr_);

    Value *code = builder().CreateLoad(
        builder().CreateStructGEP(this->frame_, FrameTy::FIELD_CODE), "co");
    this->varnames_ = builder().CreateLoad(
        builder().CreateStructGEP(code, CodeTy::FIELD_VARNAMES),
        "varnames");

    Value *names_tuple = builder().CreateBitCast(
        builder().CreateLoad(
            builder().CreateStructGEP(code, CodeTy::FIELD_NAMES)),
        TypeBuilder<PyTupleObject*>::cache(module),
        "names");
    // The next GEP-magic assigns this->names_ to &names_tuple[0].ob_item[0].
    Value *names_item_indices[] = {
        ConstantInt::get(Type::Int32Ty, 0),
        ConstantInt::get(Type::Int32Ty, TupleTy::FIELD_ITEM),
        ConstantInt::get(Type::Int32Ty, 0),
    };
    this->names_ =
        builder().CreateGEP(
            names_tuple,
            names_item_indices, array_endof(names_item_indices),
            "names");

    Value *consts_tuple =  // (PyTupleObject*)code->co_consts
        builder().CreateBitCast(
            builder().CreateLoad(
                builder().CreateStructGEP(code, CodeTy::FIELD_CONSTS)),
            TypeBuilder<PyTupleObject*>::cache(module));
    // The next GEP-magic assigns this->consts_ to &consts_tuple[0].ob_item[0].
    Value *consts_item_indices[] = {
        ConstantInt::get(Type::Int32Ty, 0),
        ConstantInt::get(Type::Int32Ty, TupleTy::FIELD_ITEM),
        ConstantInt::get(Type::Int32Ty, 0),
    };
    this->consts_ =
        builder().CreateGEP(
            consts_tuple,
            consts_item_indices, array_endof(consts_item_indices),
            "consts");

    // The next GEP-magic assigns this->fastlocals_ to
    // &frame_[0].f_localsplus[0].
    Value* fastlocals_indices[] = {
        ConstantInt::get(Type::Int32Ty, 0),
        ConstantInt::get(Type::Int32Ty, FrameTy::FIELD_LOCALSPLUS),
        ConstantInt::get(Type::Int32Ty, 0),
    };
    this->fastlocals_ =
        builder().CreateGEP(this->frame_,
                            fastlocals_indices, array_endof(fastlocals_indices),
                            "fastlocals");
    Value *nlocals = builder().CreateLoad(
        builder().CreateStructGEP(code, CodeTy::FIELD_NLOCALS), "nlocals");

    this->freevars_ =
        builder().CreateGEP(this->fastlocals_, nlocals, "freevars");

    this->globals_ =
        builder().CreateBitCast(
            builder().CreateLoad(
                builder().CreateStructGEP(this->frame_,
                                          FrameTy::FIELD_GLOBALS)),
            TypeBuilder<PyObject *>::cache(module));

    this->builtins_ =
        builder().CreateBitCast(
            builder().CreateLoad(
                builder().CreateStructGEP(this->frame_,
                                          FrameTy::FIELD_BUILTINS)),
            TypeBuilder<PyObject *>::cache(module));

    FillReturnBlock(this->return_block_);
}

void
LlvmFunctionBuilder::FillReturnBlock(BasicBlock *return_block)
{
    BasicBlock *const orig_block = builder().GetInsertBlock();
    builder().SetInsertPoint(this->return_block_);
    Value *stack_bottom = builder().CreateLoad(
        builder().CreateStructGEP(this->frame_, FrameTy::FIELD_VALUESTACK),
        "stack_bottom");

    BasicBlock *pop_loop = BasicBlock::Create("pop_loop", function());
    BasicBlock *pop_block = BasicBlock::Create("pop_stack", function());
    BasicBlock *do_return = BasicBlock::Create("do_return", function());

    FallThroughTo(pop_loop);
    Value *stack_pointer = builder().CreateLoad(this->stack_pointer_addr_);
    Value *finished_popping = builder().CreateICmpULE(
        stack_pointer, stack_bottom);
    builder().CreateCondBr(finished_popping, do_return, pop_block);

    builder().SetInsertPoint(pop_block);
    XDecRef(Pop());
    builder().CreateBr(pop_loop);

    builder().SetInsertPoint(do_return);
    Value *retval = builder().CreateLoad(this->retval_addr_, "retval");
    builder().CreateRet(retval);

    builder().SetInsertPoint(orig_block);
}

void
LlvmFunctionBuilder::FallThroughTo(BasicBlock *next_block)
{
    if (builder().GetInsertBlock()->getTerminator() == NULL) {
        // If the block doesn't already end with a branch or
        // return, branch to the next block.
        builder().CreateBr(next_block);
    }
    builder().SetInsertPoint(next_block);
}

void
LlvmFunctionBuilder::Return(Value *retval)
{
    builder().CreateStore(retval, this->retval_addr_);
    builder().CreateBr(this->return_block_);
}

void
LlvmFunctionBuilder::LOAD_CONST(int index)
{
    Value *const_ = builder().CreateLoad(
        builder().CreateGEP(this->consts_,
                            ConstantInt::get(Type::Int32Ty, index)));
    IncRef(const_);
    Push(const_);
}

void
LlvmFunctionBuilder::LOAD_GLOBAL(int names_index)
{
    BasicBlock *global_missing = BasicBlock::Create(
        "GetGlobal_global_missing", function());
    BasicBlock *global_success = BasicBlock::Create(
        "GetGlobal_global_success", function());
    BasicBlock *builtin_missing = BasicBlock::Create(
        "GetGlobal_builtin_missing", function());
    BasicBlock *builtin_success = BasicBlock::Create(
        "GetGlobal_builtin_success", function());
    BasicBlock *done = BasicBlock::Create("GetGlobal_done", function());
    Value *name = LookupName(names_index);
    Function *pydict_getitem = GetGlobalFunction<
        PyObject *(PyObject *, PyObject *)>("PyDict_GetItem");
    Value *global = builder().CreateCall2(
        pydict_getitem, this->globals_, name, "global_value");
    builder().CreateCondBr(IsNull(global), global_missing, global_success);

    builder().SetInsertPoint(global_success);
    IncRef(global);
    Push(global);
    builder().CreateBr(done);

    builder().SetInsertPoint(global_missing);
    Value *builtin = builder().CreateCall2(
        pydict_getitem, this->builtins_, name, "builtin_value");
    builder().CreateCondBr(IsNull(builtin), builtin_missing, builtin_success);

    builder().SetInsertPoint(builtin_missing);
    Function *do_raise = GetGlobalFunction<
        void(PyFrameObject *, PyObject *)>("_PyEval_RaiseForGlobalNameError");
    builder().CreateCall2(do_raise, this->frame_, name);
    Return(Constant::getNullValue(function()->getReturnType()));

    builder().SetInsertPoint(builtin_success);
    IncRef(builtin);
    Push(builtin);
    builder().CreateBr(done);

    builder().SetInsertPoint(done);
}

void
LlvmFunctionBuilder::STORE_GLOBAL(int names_index)
{
    BasicBlock *failure = BasicBlock::Create("STORE_GLOBAL_failure",
                                             function());
    BasicBlock *success = BasicBlock::Create("STORE_GLOBAL_success",
                                             function());
    Value *name = LookupName(names_index);
    Value *value = Pop();
    Function *pydict_setitem = GetGlobalFunction<
        int(PyObject *, PyObject *, PyObject *)>("PyDict_SetItem");
    Value *result = builder().CreateCall3(
        pydict_setitem, this->globals_, name, value,
        "pydict_setitem_result");
    DecRef(value);
    builder().CreateCondBr(IsNonZero(result), failure, success);

    builder().SetInsertPoint(failure);
    Return(Constant::getNullValue(function()->getReturnType()));

    builder().SetInsertPoint(success);
}    

void
LlvmFunctionBuilder::DELETE_GLOBAL(int names_index)
{
    BasicBlock *failure = BasicBlock::Create("DELETE_GLOBAL_failure",
                                             function());
    BasicBlock *success = BasicBlock::Create("DELETE_GLOBAL_success",
                                             function());
    Value *name = LookupName(names_index);
    Function *pydict_setitem = GetGlobalFunction<
        int(PyObject *, PyObject *)>("PyDict_DelItem");
    Value *result = builder().CreateCall2(
        pydict_setitem, this->globals_, name, "pydict_setitem_result");
    builder().CreateCondBr(IsNonZero(result), failure, success);

    builder().SetInsertPoint(failure);
    Function *do_raise = GetGlobalFunction<
        void(PyFrameObject *, PyObject *)>("_PyEval_RaiseForGlobalNameError");
    builder().CreateCall2(do_raise, this->frame_, name);
    Return(Constant::getNullValue(function()->getReturnType()));

    builder().SetInsertPoint(success);
}    

void
LlvmFunctionBuilder::LOAD_FAST(int index)
{
    BasicBlock *unbound_local =
        BasicBlock::Create("LOAD_FAST_unbound", function());
    BasicBlock *success =
        BasicBlock::Create("LOAD_FAST_success", function());

    Value *local = builder().CreateLoad(
        builder().CreateGEP(this->fastlocals_,
                            ConstantInt::get(Type::Int32Ty, index)),
        "FAST_loaded");
    builder().CreateCondBr(IsNull(local), unbound_local, success);

    builder().SetInsertPoint(unbound_local);
    Function *do_raise =
        GetGlobalFunction<void(PyFrameObject*, int)>(
            "_PyEval_RaiseForUnboundLocal");
    builder().CreateCall2(
        do_raise, this->frame_,
        ConstantInt::get(TypeBuilder<int>::cache(this->module_),
                         index, true /* signed */));
    Return(Constant::getNullValue(function()->getReturnType()));

    builder().SetInsertPoint(success);
    IncRef(local);
    Push(local);
}

void
LlvmFunctionBuilder::LOAD_DEREF(int index)
{
    BasicBlock *failed_load =
        BasicBlock::Create("LOAD_DEREF_failed_load", function());
    BasicBlock *unbound_local =
        BasicBlock::Create("LOAD_DEREF_unbound_local", function());
    BasicBlock *error =
        BasicBlock::Create("LOAD_DEREF_error", function());
    BasicBlock *success =
        BasicBlock::Create("LOAD_DEREF_success", function());

    Value *cell = builder().CreateLoad(
        builder().CreateGEP(this->freevars_,
                            ConstantInt::get(Type::Int32Ty, index, true)));
    Function *pycell_get = GetGlobalFunction<
        PyObject *(PyObject *)>("PyCell_Get");
    Value *value = builder().CreateCall(
        pycell_get, cell, "LOAD_DEREF_cell_contents");
    builder().CreateCondBr(IsNull(value), failed_load, success);

    builder().SetInsertPoint(failed_load);
    Function *pyerr_occurred =
        GetGlobalFunction<PyObject *()>("PyErr_Occurred");
    Value *was_err =
        builder().CreateCall(pyerr_occurred, "LOAD_DEREF_err_occurred");
    builder().CreateCondBr(IsNull(was_err), unbound_local, error);

    builder().SetInsertPoint(unbound_local);
    Function *do_raise =
        GetGlobalFunction<void(PyFrameObject*, int)>(
            "_PyEval_RaiseForUnboundLocal");
    builder().CreateCall2(
        do_raise, this->frame_,
        ConstantInt::get(TypeBuilder<int>::cache(this->module_),
                         index, true /* signed */));
    Return(Constant::getNullValue(function()->getReturnType()));

    builder().SetInsertPoint(error);
    Return(Constant::getNullValue(function()->getReturnType()));

    builder().SetInsertPoint(success);
    Push(value);
}

void
LlvmFunctionBuilder::STORE_DEREF(int index)
{
    BasicBlock *failure =
        BasicBlock::Create("STORE_DEREF_failure", function());
    BasicBlock *success =
        BasicBlock::Create("STORE_DEREF_success", function());

    Value *value = Pop();
    Value *cell = builder().CreateLoad(
        builder().CreateGEP(
            this->freevars_, ConstantInt::get(Type::Int32Ty, index, true)));
    Function *pycell_set = GetGlobalFunction<
        int(PyObject *, PyObject *)>("PyCell_Set");
    Value *result = builder().CreateCall2(
        pycell_set, cell, value, "STORE_DEREF_result");
    DecRef(value);
    // ceval.c doesn't actually check the return value of this, I guess
    // we are a little more likely to do things wrong.
    builder().CreateCondBr(IsNonZero(result), failure, success);

    builder().SetInsertPoint(failure);
    Return(Constant::getNullValue(function()->getReturnType()));

    builder().SetInsertPoint(success);
}

void
LlvmFunctionBuilder::LOAD_ATTR(int names_index)
{
    BasicBlock *failure = BasicBlock::Create(
        "LOAD_ATTR_failure", function());
    BasicBlock *success = BasicBlock::Create(
        "LOAD_ATTR_success", function());
    Value *attr_name = LookupName(names_index);
    Value *obj = Pop();
    Function *pyobject_getattr = GetGlobalFunction<
        PyObject *(PyObject *, PyObject *)>("PyObject_GetAttr");
    Value *result = builder().CreateCall2(
        pyobject_getattr, obj, attr_name, "LOAD_ATTR_result");
    DecRef(obj);
    builder().CreateCondBr(IsNull(result), failure, success);

    builder().SetInsertPoint(failure);
    Return(Constant::getNullValue(function()->getReturnType()));

    builder().SetInsertPoint(success);
    Push(result);
}

void
LlvmFunctionBuilder::STORE_ATTR(int names_index)
{
    BasicBlock *failure = BasicBlock::Create("STORE_ATTR_failure",
                                             function());
    BasicBlock *success = BasicBlock::Create("STORE_ATTR_success",
                                             function());
    Value *attr_name = LookupName(names_index);
    Value *obj = Pop();
    Value *value = Pop();
    Function *pyobject_setattr = GetGlobalFunction<
        int(PyObject *, PyObject *, PyObject *)>("PyObject_SetAttr");
    Value *result = builder().CreateCall3(
        pyobject_setattr, obj, attr_name, value,
        "STORE_ATTR_result");
    DecRef(value);
    DecRef(obj);
    builder().CreateCondBr(IsNonZero(result), failure, success);

    builder().SetInsertPoint(failure);
    Return(Constant::getNullValue(function()->getReturnType()));

    builder().SetInsertPoint(success);
}    

void
LlvmFunctionBuilder::DELETE_ATTR(int names_index)
{
    BasicBlock *failure = BasicBlock::Create("DELETE_ATTR_failure",
                                             function());
    BasicBlock *success = BasicBlock::Create("DELETE_ATTR_success",
                                             function());
    Value *attr_name = LookupName(names_index);
    Value *obj = Pop();
    Value *value = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    Function *pyobject_setattr = GetGlobalFunction<
        int(PyObject *, PyObject *, PyObject *)>("PyObject_SetAttr");
    Value *result = builder().CreateCall3(
        pyobject_setattr, obj, attr_name, value,
        "DELETE_ATTR_result");
    DecRef(obj);
    builder().CreateCondBr(IsNonZero(result), failure, success);

    builder().SetInsertPoint(failure);
    Return(Constant::getNullValue(function()->getReturnType()));

    builder().SetInsertPoint(success);
}    

void
LlvmFunctionBuilder::CALL_FUNCTION(int num_args)
{
    BasicBlock *failure =
        BasicBlock::Create("CALL_FUNCTION_failure", function());
    BasicBlock *success =
        BasicBlock::Create("CALL_FUNCTION_success", function());
    Function *call_function = GetGlobalFunction<
        PyObject *(PyObject ***, int)>("_PyEval_CallFunction");
    Value *temp_stack_pointer_addr = builder().CreateAlloca(
        TypeBuilder<PyObject**>::cache(this->module_),
        0, "CALL_FUNCTION_stack_pointer_addr");
    builder().CreateStore(
        builder().CreateLoad(this->stack_pointer_addr_),
        temp_stack_pointer_addr);
    Value *result = builder().CreateCall2(
        call_function,
        temp_stack_pointer_addr,
        ConstantInt::get(TypeBuilder<int>::cache(this->module_), num_args),
        "CALL_FUNCTION_result");
    builder().CreateStore(
        builder().CreateLoad(temp_stack_pointer_addr),
        this->stack_pointer_addr_);
    builder().CreateCondBr(IsNull(result), failure, success);

    builder().SetInsertPoint(failure);
    Return(Constant::getNullValue(function()->getReturnType()));

    builder().SetInsertPoint(success);
    Push(result);
}  

void
LlvmFunctionBuilder::CALL_FUNCTION_VAR_KW(int num_args)
{
    BasicBlock *failure =
        BasicBlock::Create("CALL_FUNCTION_VAR_KW_failure", function());
    BasicBlock *success =
        BasicBlock::Create("CALL_FUNCTION_VAR_KW_success", function());
    Function *call_function = GetGlobalFunction<
        PyObject *(PyObject ***, int)>("_PyEval_CallFunctionVarKw");
    Value *result = builder().CreateCall2(
        call_function,
        this->stack_pointer_addr_,
        ConstantInt::get(TypeBuilder<int>::cache(this->module_), num_args),
        "CALL_FUNCTION_VAR_KW_result");
    builder().CreateCondBr(IsNonZero(result), failure, success);

    builder().SetInsertPoint(failure);
    Return(Constant::getNullValue(function()->getReturnType()));

    builder().SetInsertPoint(success);
}  

void
LlvmFunctionBuilder::JUMP_ABSOLUTE(llvm::BasicBlock *target,
                                   llvm::BasicBlock *fallthrough)
{
    builder().CreateBr(target);
}

void
LlvmFunctionBuilder::POP_JUMP_IF_FALSE(llvm::BasicBlock *target,
                                       llvm::BasicBlock *fallthrough)
{
    Value *test_value = Pop();
    Value *is_true = IsTrue(test_value);
    DecRef(test_value);
    builder().CreateCondBr(is_true, fallthrough, target);
}

void
LlvmFunctionBuilder::POP_JUMP_IF_TRUE(llvm::BasicBlock *target,
                                      llvm::BasicBlock *fallthrough)
{
    Value *test_value = Pop();
    Value *is_true = IsTrue(test_value);
    DecRef(test_value);
    builder().CreateCondBr(is_true, target, fallthrough);
}

void
LlvmFunctionBuilder::JUMP_IF_FALSE_OR_POP(llvm::BasicBlock *target,
                                          llvm::BasicBlock *fallthrough)
{
    BasicBlock *true_path =
        BasicBlock::Create("JUMP_IF_FALSE_OR_POP_pop", function());
    Value *test_value = Pop();
    Push(test_value);
    Value *is_true = IsTrue(test_value);
    builder().CreateCondBr(is_true, true_path, target);
    builder().SetInsertPoint(true_path);
    test_value = Pop();
    DecRef(test_value);
    builder().CreateBr(fallthrough);
    
}

void
LlvmFunctionBuilder::JUMP_IF_TRUE_OR_POP(llvm::BasicBlock *target,
                                         llvm::BasicBlock *fallthrough)
{
    BasicBlock *false_path =
        BasicBlock::Create("JUMP_IF_TRUE_OR_POP_pop", function());
    Value *test_value = Pop();
    Push(test_value);
    Value *is_true = IsTrue(test_value);
    builder().CreateCondBr(is_true, target, false_path);
    builder().SetInsertPoint(false_path);
    test_value = Pop();
    DecRef(test_value);
    builder().CreateBr(fallthrough);
}

void
LlvmFunctionBuilder::STORE_FAST(int index)
{
    SetLocal(index, Pop());
}

void
LlvmFunctionBuilder::DELETE_FAST(int index)
{
    SetLocal(index, Constant::getNullValue(
                   TypeBuilder<PyObject *>::cache(this->module_)));
}

void
LlvmFunctionBuilder::SETUP_LOOP(llvm::BasicBlock *target,
                                llvm::BasicBlock *fallthrough)
{
    // TODO: I think we can ignore this until we have an exception story.
    //InsertAbort("SETUP_LOOP");
}

void
LlvmFunctionBuilder::GET_ITER()
{
    Value *obj = Pop();
    Function *pyobject_getiter = GetGlobalFunction<PyObject*(PyObject*)>(
        "PyObject_GetIter");
    Value *iter = builder().CreateCall(pyobject_getiter, obj);
    DecRef(obj);
    BasicBlock *got_iter = BasicBlock::Create("got_iter", function());
    BasicBlock *was_exception = BasicBlock::Create("was_exception", function());
    builder().CreateCondBr(IsNull(iter), was_exception, got_iter);

    builder().SetInsertPoint(was_exception);
    Return(Constant::getNullValue(function()->getReturnType()));

    builder().SetInsertPoint(got_iter);
    Push(iter);
}

void
LlvmFunctionBuilder::FOR_ITER(llvm::BasicBlock *target,
                              llvm::BasicBlock *fallthrough)
{
    Value *iter = Pop();
    Value *iter_tp = builder().CreateBitCast(
        builder().CreateLoad(
            builder().CreateStructGEP(iter, ObjectTy::FIELD_TYPE)),
        TypeBuilder<PyTypeObject *>::cache(this->module_),
        "iter_type");
    Value *iternext = builder().CreateLoad(
        builder().CreateStructGEP(iter_tp, TypeTy::FIELD_ITERNEXT),
        "iternext");
    Value *next = builder().CreateCall(iternext, iter, "next");
    BasicBlock *got_next = BasicBlock::Create("got_next", function());
    BasicBlock *next_null = BasicBlock::Create("next_null", function());
    builder().CreateCondBr(IsNull(next), next_null, got_next);

    builder().SetInsertPoint(next_null);
    Value *err_occurred = builder().CreateCall(
        GetGlobalFunction<PyObject*()>("PyErr_Occurred"));
    BasicBlock *iter_ended = BasicBlock::Create("iter_ended", function());
    BasicBlock *exception = BasicBlock::Create("exception", function());
    builder().CreateCondBr(IsNull(err_occurred), iter_ended, exception);

    builder().SetInsertPoint(exception);
    Value *exc_stopiteration = builder().CreateLoad(
        GetGlobalVariable<PyObject*>("PyExc_StopIteration"));
    Value *was_stopiteration = builder().CreateCall(
        GetGlobalFunction<int(PyObject *)>("PyErr_ExceptionMatches"),
        exc_stopiteration);
    BasicBlock *clear_err = BasicBlock::Create("clear_err", function());
    BasicBlock *propagate = BasicBlock::Create("propagate", function());
    builder().CreateCondBr(IsNonZero(was_stopiteration), clear_err, propagate);

    builder().SetInsertPoint(propagate);
    DecRef(iter);
    Return(Constant::getNullValue(function()->getReturnType()));

    builder().SetInsertPoint(clear_err);
    builder().CreateCall(GetGlobalFunction<void()>("PyErr_Clear"));
    builder().CreateBr(iter_ended);

    builder().SetInsertPoint(iter_ended);
    DecRef(iter);
    builder().CreateBr(target);

    builder().SetInsertPoint(got_next);
    Push(iter);
    Push(next);
}

void
LlvmFunctionBuilder::POP_BLOCK()
{
    // TODO: I think we can ignore this until we have an exception story.
    //InsertAbort("POP_BLOCK");
}

void
LlvmFunctionBuilder::RETURN_VALUE()
{
    Value *retval = Pop();
    Return(retval);
} 

void
LlvmFunctionBuilder::DoRaise(Value *exc_type, Value *exc_inst, Value *exc_tb)
{
    BasicBlock *raise_block = BasicBlock::Create("raise_block", function());
    BasicBlock *dead_code = BasicBlock::Create("dead_code", function());
    // Accept code after a raise statement, even though it's never executed.
    builder().CreateCondBr(
        ConstantInt::get(Type::Int1Ty, 1), raise_block, dead_code);

    // TODO(twouters): look for exception handling in this function.
    builder().SetInsertPoint(raise_block);
    Function *do_raise = GetGlobalFunction<
        void(PyObject*, PyObject *, PyObject *)>("_PyEval_DoRaise");
    // _PyEval_DoRaise eats references.
    builder().CreateCall3(do_raise, exc_type, exc_inst, exc_tb);
    Return(Constant::getNullValue(function()->getReturnType()));

    builder().SetInsertPoint(dead_code);
}

void
LlvmFunctionBuilder::RAISE_VARARGS_ZERO()
{
    Value *exc_tb = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    Value *exc_inst = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    Value *exc_type = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    DoRaise(exc_type, exc_inst, exc_tb);
}

void
LlvmFunctionBuilder::RAISE_VARARGS_ONE()
{
    Value *exc_tb = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    Value *exc_inst = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    Value *exc_type = Pop();
    DoRaise(exc_type, exc_inst, exc_tb);
}

void
LlvmFunctionBuilder::RAISE_VARARGS_TWO()
{
    Value *exc_tb = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    Value *exc_inst = Pop();
    Value *exc_type = Pop();
    DoRaise(exc_type, exc_inst, exc_tb);
}

void
LlvmFunctionBuilder::RAISE_VARARGS_THREE()
{
    Value *exc_tb = Pop();
    Value *exc_inst = Pop();
    Value *exc_type = Pop();
    DoRaise(exc_type, exc_inst, exc_tb);
}

void
LlvmFunctionBuilder::STORE_SUBSCR()
{
    BasicBlock *failure = BasicBlock::Create("STORE_SUBSCR_failure",
                                             function());
    BasicBlock *success = BasicBlock::Create("STORE_SUBSCR_success",
                                             function());
    // Performing obj[key] = val
    Value *key = Pop();
    Value *obj = Pop();
    Value *value = Pop();
    Function *setitem = GetGlobalFunction<
          int(PyObject *, PyObject *, PyObject *)>("PyObject_SetItem");
    Value *result = builder().CreateCall3(setitem, obj, key, value,
                                          "STORE_SUBSCR_result");
    DecRef(value);
    DecRef(obj);
    DecRef(key);
    builder().CreateCondBr(IsNonZero(result), failure, success);
    
    builder().SetInsertPoint(failure);
    Return(Constant::getNullValue(function()->getReturnType()));
    
    builder().SetInsertPoint(success);
}

// Common code for almost all binary operations
void
LlvmFunctionBuilder::GenericBinOp(const char *apifunc)
{
    BasicBlock *failure = BasicBlock::Create("binop_failure", function());
    BasicBlock *success = BasicBlock::Create("binop_success", function());
    Value *rhs = Pop();
    Value *lhs = Pop();
    Function *op =
        GetGlobalFunction<PyObject*(PyObject*, PyObject*)>(apifunc);
    Value *result = builder().CreateCall2(op, lhs, rhs, "binop_result");
    DecRef(lhs);
    DecRef(rhs);
    builder().CreateCondBr(IsNull(result), failure, success);
    
    builder().SetInsertPoint(failure);
    Return(Constant::getNullValue(function()->getReturnType()));

    builder().SetInsertPoint(success);
    Push(result);
}

#define BINOP_METH(OPCODE, APIFUNC) 		\
void						\
LlvmFunctionBuilder::OPCODE()			\
{						\
    GenericBinOp(#APIFUNC);			\
}

BINOP_METH(BINARY_ADD, PyNumber_Add)
BINOP_METH(BINARY_SUBTRACT, PyNumber_Subtract)
BINOP_METH(BINARY_MULTIPLY, PyNumber_Multiply)
BINOP_METH(BINARY_TRUE_DIVIDE, PyNumber_TrueDivide)
BINOP_METH(BINARY_DIVIDE, PyNumber_Divide)
BINOP_METH(BINARY_MODULO, PyNumber_Remainder)
BINOP_METH(BINARY_LSHIFT, PyNumber_Lshift)
BINOP_METH(BINARY_RSHIFT, PyNumber_Rshift)
BINOP_METH(BINARY_OR, PyNumber_Or)
BINOP_METH(BINARY_XOR, PyNumber_Xor)
BINOP_METH(BINARY_AND, PyNumber_And)
BINOP_METH(BINARY_FLOOR_DIVIDE, PyNumber_FloorDivide)
BINOP_METH(BINARY_SUBSCR, PyObject_GetItem)

BINOP_METH(INPLACE_ADD, PyNumber_InPlaceAdd)
BINOP_METH(INPLACE_SUBTRACT, PyNumber_InPlaceSubtract)
BINOP_METH(INPLACE_MULTIPLY, PyNumber_InPlaceMultiply)
BINOP_METH(INPLACE_TRUE_DIVIDE, PyNumber_InPlaceTrueDivide)
BINOP_METH(INPLACE_DIVIDE, PyNumber_InPlaceDivide)
BINOP_METH(INPLACE_MODULO, PyNumber_InPlaceRemainder)
BINOP_METH(INPLACE_LSHIFT, PyNumber_InPlaceLshift)
BINOP_METH(INPLACE_RSHIFT, PyNumber_InPlaceRshift)
BINOP_METH(INPLACE_OR, PyNumber_InPlaceOr)
BINOP_METH(INPLACE_XOR, PyNumber_InPlaceXor)
BINOP_METH(INPLACE_AND, PyNumber_InPlaceAnd)
BINOP_METH(INPLACE_FLOOR_DIVIDE, PyNumber_InPlaceFloorDivide)

#undef BINOP_METH

// PyNumber_Power() and PyNumber_InPlacePower() take three arguments, the
// third should be Py_None when calling from BINARY_POWER/INPLACE_POWER.
void
LlvmFunctionBuilder::GenericPowOp(const char *apifunc)
{
    BasicBlock *failure = BasicBlock::Create("powop_failure", function());
    BasicBlock *success = BasicBlock::Create("powop_success", function());
    Value *rhs = Pop();
    Value *lhs = Pop();
    Function *op = GetGlobalFunction<PyObject*(PyObject*, PyObject*,
        PyObject *)>(apifunc);
    Value *pynone = GetGlobalVariable<PyObject>("_Py_NoneStruct");
    Value *result = builder().CreateCall3(op, lhs, rhs, pynone,
                                          "powop_result");
    DecRef(lhs);
    DecRef(rhs);
    builder().CreateCondBr(IsNull(result), failure, success);
    
    builder().SetInsertPoint(failure);
    Return(Constant::getNullValue(function()->getReturnType()));

    builder().SetInsertPoint(success);
    Push(result);
}

void
LlvmFunctionBuilder::BINARY_POWER()
{
    GenericPowOp("PyNumber_Power");
}

void
LlvmFunctionBuilder::INPLACE_POWER()
{
    GenericPowOp("PyNumber_InPlacePower");
}

void
LlvmFunctionBuilder::DELETE_SUBSCR()
{
    BasicBlock *failure = BasicBlock::Create("DELETE_SUBSCR_failure",
                                             function());
    BasicBlock *success = BasicBlock::Create("DELETE_SUBSCR_success",
                                             function());
    Value *key = Pop();
    Value *obj = Pop();
    Function *delitem = GetGlobalFunction<
          int(PyObject *, PyObject *)>("PyObject_DelItem");
    Value *result = builder().CreateCall2(delitem, obj, key,
                                          "DELETE_SUBSCR_result");
    DecRef(obj);
    DecRef(key);
    builder().CreateCondBr(IsNonZero(result), failure, success);
    
    builder().SetInsertPoint(failure);
    Return(Constant::getNullValue(function()->getReturnType()));
    
    builder().SetInsertPoint(success);
}

void
LlvmFunctionBuilder::POP_TOP()
{
    Value *top = Pop();
    DecRef(top);
}

void
LlvmFunctionBuilder::DUP_TOP()
{
    Value *first = Pop();
    IncRef(first);
    Push(first);
    Push(first);
}

void
LlvmFunctionBuilder::DUP_TOP_TWO()
{
    Value *first = Pop();
    Value *second = Pop();
    IncRef(first);
    IncRef(second);
    Push(second);
    Push(first);
    Push(second);
    Push(first);
}

// untested; only used in augmented slice assignment.
void
LlvmFunctionBuilder::DUP_TOP_THREE()
{
    Value *first = Pop();
    Value *second = Pop();
    Value *third = Pop();
    IncRef(first);
    IncRef(second);;
    IncRef(third);
    Push(third);
    Push(second);
    Push(first);
    Push(third);
    Push(second);
    Push(first);
}

// untested; used in comparisons, with stmt, attribute access, slicing
void
LlvmFunctionBuilder::ROT_TWO()
{
    Value *first = Pop();
    Value *second = Pop();
    Push(first);
    Push(second);
}

void
LlvmFunctionBuilder::ROT_THREE()
{
    Value *first = Pop();
    Value *second = Pop();
    Value *third = Pop();
    Push(first);
    Push(third);
    Push(second);
}

// untested; only used in slice assignment.
void
LlvmFunctionBuilder::ROT_FOUR()
{
    Value *first = Pop();
    Value *second = Pop();
    Value *third = Pop();
    Value *fourth = Pop();
    Push(first);
    Push(fourth);
    Push(third);
    Push(second);
}

void
LlvmFunctionBuilder::LIST_APPEND()
{
    BasicBlock *failure = BasicBlock::Create("LIST_APPEND_failure",
                                             function());
    BasicBlock *success = BasicBlock::Create("LIST_APPEND_success",
                                             function());
    Value *item = Pop();
    Value *listobj = Pop();
    Function *list_append = GetGlobalFunction<
        int(PyObject *, PyObject *)>("PyList_Append");
    Value *result = builder().CreateCall2(list_append, listobj, item,
                                          "LIST_APPEND_result");
    DecRef(listobj);
    DecRef(item);
    builder().CreateCondBr(IsNonZero(result), failure, success);
    
    builder().SetInsertPoint(failure);
    Return(Constant::getNullValue(function()->getReturnType()));
    
    builder().SetInsertPoint(success);
}

void
LlvmFunctionBuilder::STORE_MAP()
{
    BasicBlock *failure = BasicBlock::Create("STORE_MAP_failure", function());
    BasicBlock *success = BasicBlock::Create("STORE_MAP_success", function());
    Value *key = Pop();
    Value *value = Pop();
    Value *dict = Pop();
    Push(dict);
    // old ceval loop does assert(PyDict_CheckExact()), should we?
    Function *setitem = GetGlobalFunction<
        int(PyObject *, PyObject *, PyObject *)>("PyDict_SetItem");
    Value *result = builder().CreateCall3(setitem, dict, key, value,
                                          "STORE_MAP_result");
    DecRef(value);
    DecRef(key);
    builder().CreateCondBr(IsNonZero(result), failure, success);
    
    builder().SetInsertPoint(failure);
    Return(Constant::getNullValue(function()->getReturnType()));
    
    builder().SetInsertPoint(success);
}

void
LlvmFunctionBuilder::List_SET_ITEM(Value *lst, Value *idx, Value *item)
{
    Value *listobj = builder().CreateBitCast(
        lst, TypeBuilder<PyListObject*>::cache(this->module_));
    Value *list_items = builder().CreateLoad(
        builder().CreateStructGEP(listobj, ListTy::FIELD_ITEM));
    Value *itemslot = builder().CreateGEP(list_items, idx, "list_item_slot");
    builder().CreateStore(item, itemslot);
}

void
LlvmFunctionBuilder::Tuple_SET_ITEM(Value *tup, Value *idx, Value *item)
{
    Value *tupobj = builder().CreateBitCast(
        tup, TypeBuilder<PyTupleObject*>::cache(this->module_));
    Value *tup_item_indices[] = {
        ConstantInt::get(Type::Int32Ty, 0), // deref the Value*
        ConstantInt::get(Type::Int32Ty, TupleTy::FIELD_ITEM), // get ob_item
        idx, // get the item we want
    };
    Value *itemslot = builder().CreateGEP(tupobj, tup_item_indices,
                                          array_endof(tup_item_indices),
                                          "tuple_item_slot");
    builder().CreateStore(item, itemslot);
}

void
LlvmFunctionBuilder::SequenceBuilder(int size, const char *createname,
    void (LlvmFunctionBuilder::*method)(Value*, Value*, Value*))
{
    BasicBlock *failure = BasicBlock::Create("SeqBuild_failure", function());
    BasicBlock *loop_start = BasicBlock::Create("SeqBuild_loop_start",
                                                function());
    BasicBlock *loop_body = BasicBlock::Create("SeqBuild_loop_body",
                                               function());
    BasicBlock *end = BasicBlock::Create("SeqBuild_end", function());

    const Type *IntSsizeTy = TypeBuilder<Py_ssize_t>::cache(this->module_);
    Value *seqsize = ConstantInt::get(IntSsizeTy, size, true /* signed */);
    Value *zero = Constant::getNullValue(IntSsizeTy);
    Value *one = ConstantInt::get(IntSsizeTy, 1, true /* signed */);

    Function *create = GetGlobalFunction<PyObject *(Py_ssize_t)>(createname);
    Value *seq = builder().CreateCall(create, seqsize, "SeqBuild_seq");
    BasicBlock *preamble = builder().GetInsertBlock();
    builder().CreateCondBr(IsNull(seq), failure, loop_start);

    builder().SetInsertPoint(failure);
    Return(Constant::getNullValue(function()->getReturnType()));

    builder().SetInsertPoint(loop_start);
    PHINode *phi = builder().CreatePHI(IntSsizeTy, "SeqBuild_loop_var");
    phi->addIncoming(seqsize, preamble);
    Value *done = builder().CreateICmpSLE(phi, zero,
                                          "SeqBuild_loop_check");
    builder().CreateCondBr(done, end, loop_body);

    builder().SetInsertPoint(loop_body);
    Value *item = Pop();
    Value *nextval = builder().CreateSub(phi, one, "SeqBuild_next_loop_var");
    (this->*method)(seq, nextval, item);
    phi->addIncoming(nextval, builder().GetInsertBlock()); 
    builder().CreateBr(loop_start);

    builder().SetInsertPoint(end);
    Push(seq);
}

void
LlvmFunctionBuilder::BUILD_LIST(int size)
{
   SequenceBuilder(size, "PyList_New",
                   &LlvmFunctionBuilder::List_SET_ITEM);
}

void
LlvmFunctionBuilder::BUILD_TUPLE(int size)
{
   SequenceBuilder(size, "PyTuple_New",
                   &LlvmFunctionBuilder::Tuple_SET_ITEM);
}

// Implementation of almost all unary operations
void
LlvmFunctionBuilder::GenericUnaryOp(const char *apifunc)
{
    BasicBlock *failure = BasicBlock::Create("unaryop_failure", function());
    BasicBlock *success = BasicBlock::Create("unaryop_success", function());
    Value *value = Pop();
    Function *op = GetGlobalFunction<PyObject*(PyObject*)>(apifunc);
    Value *result = builder().CreateCall(op, value, "unaryop_result");
    DecRef(value);
    builder().CreateCondBr(IsNull(result), failure, success);
    
    builder().SetInsertPoint(failure);
    Return(Constant::getNullValue(function()->getReturnType()));

    builder().SetInsertPoint(success);
    Push(result);
}

#define UNARYOP_METH(NAME, APIFUNC)			\
void							\
LlvmFunctionBuilder::NAME()				\
{							\
    GenericUnaryOp(#APIFUNC);				\
}

UNARYOP_METH(UNARY_CONVERT, PyObject_Repr)
UNARYOP_METH(UNARY_INVERT, PyNumber_Invert)
UNARYOP_METH(UNARY_POSITIVE, PyNumber_Positive)
UNARYOP_METH(UNARY_NEGATIVE, PyNumber_Negative)

#undef UNARYOP_METH

void
LlvmFunctionBuilder::UNARY_NOT()
{
    BasicBlock *success = BasicBlock::Create("UNARY_NOT_success", function());
    BasicBlock *failure = BasicBlock::Create("UNARY_NOT_failure", function());

    Value *value = Pop();
    Function *pyobject_istrue = GetGlobalFunction<
        int(PyObject *)>("PyObject_IsTrue");
    Value *result = builder().CreateCall(pyobject_istrue, value,
                                         "UNARY_NOT_obj_as_bool");
    Value *zero = Constant::getNullValue(result->getType());
    Value *iserr = builder().CreateICmpSLT(result, zero, "UNARY_NOT_is_err");
    DecRef(value);
    builder().CreateCondBr(iserr, failure, success);

    builder().SetInsertPoint(failure);
    Return(Constant::getNullValue(function()->getReturnType()));

    builder().SetInsertPoint(success);
    Value *istrue = builder().CreateICmpSGT(result, zero,
                                            "UNARY_NOT_is_true");
    Value *retval = builder().CreateSelect(
        istrue,
        GetGlobalVariable<PyObject>("_Py_ZeroStruct"),
        GetGlobalVariable<PyObject>("_Py_TrueStruct"),
        "UNARY_NOT_result");
    IncRef(retval);
    Push(retval);
}

Value *
LlvmFunctionBuilder::ContainerContains(Value *container, Value *item)
{
    BasicBlock *err = BasicBlock::Create("ContainerContains_err",
                                         function());
    BasicBlock *non_err = BasicBlock::Create("ContainerContains_non_err",
                                             function());
    Function *contains = GetGlobalFunction<
        int(PyObject *, PyObject *)>("PySequence_Contains");
    Value *zero = ConstantInt::get(TypeBuilder<int>::cache(this->module_), 0);
    Value *result = builder().CreateCall2(
        contains, container, item, "ContainerContains_result");
    DecRef(item);
    DecRef(container);
    builder().CreateCondBr(
        builder().CreateICmpSLT(result, zero), err, non_err);

    builder().SetInsertPoint(err);
    Return(Constant::getNullValue(function()->getReturnType()));

    builder().SetInsertPoint(non_err);
    Value *bool_result = builder().CreateICmpSGT(result, zero,
                                                 "COMPARE_OP_IN");
    return bool_result;
}

void
LlvmFunctionBuilder::RichCompare(Value *lhs, Value *rhs, int cmp_op)
{
    BasicBlock *failure = BasicBlock::Create("RichCompare_failure",
                                             function());
    BasicBlock *success = BasicBlock::Create("RichCompare_success",
                                             function());
    Function *pyobject_richcompare = GetGlobalFunction<
        PyObject *(PyObject *, PyObject *, int)>("PyObject_RichCompare");
    Value *result = builder().CreateCall3(
        pyobject_richcompare, lhs, rhs,
        ConstantInt::get(TypeBuilder<int>::cache(this->module_), cmp_op),
        "RichCompare_result");
    DecRef(lhs);
    DecRef(rhs);
    builder().CreateCondBr(IsNull(result), failure, success);
    
    builder().SetInsertPoint(failure);
    Return(Constant::getNullValue(function()->getReturnType()));

    builder().SetInsertPoint(success);
    Push(result);
}

// Untested (used in exception handling.)
Value *
LlvmFunctionBuilder::ExceptionMatches(Value *exc, Value *exc_type)
{
    BasicBlock *err = BasicBlock::Create("ExceptionMatches_err",
                                         function());
    BasicBlock *no_err = BasicBlock::Create("ExceptionMatches_no_err",
                                            function());
    Value *zero = ConstantInt::get(
        TypeBuilder<int>::cache(this->module_), 0);
    Function *exc_matches = GetGlobalFunction<
        int(PyObject *, PyObject *)>("_PyEval_CheckedExceptionMatches");
    Value *result = builder().CreateCall2(
        exc_matches, exc, exc_type);
    DecRef(exc_type);
    DecRef(exc);
    builder().CreateCondBr(IsNull(result), err, no_err);
    
    builder().SetInsertPoint(err);
    Return(Constant::getNullValue(function()->getReturnType()));
    
    builder().SetInsertPoint(no_err);
    Value *bool_result = builder().CreateICmpSGT(result, zero,
                                                 "COMPARE_OP_EXC_MATCH");
    return bool_result;
}

void
LlvmFunctionBuilder::COMPARE_OP(int cmp_op)
{
    Value *rhs = Pop();
    Value *lhs = Pop();
    Value *result;
    switch (cmp_op) {
    case PyCmp_IS:
        result = builder().CreateICmpEQ(lhs, rhs, "COMPARE_OP_IS");
        DecRef(lhs);
        DecRef(rhs);
        break;
    case PyCmp_IS_NOT:
        result = builder().CreateICmpNE(lhs, rhs, "COMPARE_OP_IS_NOT");
        DecRef(lhs);
        DecRef(rhs);
        break;
    case PyCmp_IN:
        // item in seq -> ContainerContains(seq, item)
        result = ContainerContains(rhs, lhs);
        break;
    case PyCmp_NOT_IN:
    {
        Value *inverted_result = ContainerContains(rhs, lhs);
        result = builder().CreateICmpEQ(
            inverted_result, ConstantInt::get(Type::Int1Ty, 0),
            "COMPARE_OP_NOT_IN");
        break;
    }
    case PyCmp_EXC_MATCH:
        result = ExceptionMatches(lhs, rhs);
        break;
    case PyCmp_EQ:
    case PyCmp_NE:
    case PyCmp_LT:
    case PyCmp_LE:
    case PyCmp_GT:
    case PyCmp_GE:
        RichCompare(lhs, rhs, cmp_op);
        return;
    default:
        Py_FatalError("unknown COMPARE_OP oparg");;
    }
    Value *value = builder().CreateSelect(
        result,
        GetGlobalVariable<PyObject>("_Py_TrueStruct"),
        GetGlobalVariable<PyObject>("_Py_ZeroStruct"),
        "COMPARE_OP_result");
    IncRef(value);
    Push(value);
}

void
LlvmFunctionBuilder::BUILD_MAP(int size)
{
    BasicBlock *failure = BasicBlock::Create("BUILD_MAP_failure", function());
    BasicBlock *success = BasicBlock::Create("BUILD_MAP_success", function());
    Value *sizehint = ConstantInt::get(
        TypeBuilder<Py_ssize_t>::cache(this->module_), size, true);
    Function *create_dict = GetGlobalFunction<
        PyObject *(Py_ssize_t)>("_PyDict_NewPresized");
    Value *result = builder().CreateCall(create_dict, sizehint,
                                         "BULD_MAP_result");
    builder().CreateCondBr(IsNull(result), failure, success);

    builder().SetInsertPoint(failure);
    Return(Constant::getNullValue(function()->getReturnType()));

    builder().SetInsertPoint(success);
    Push(result);
}

void
LlvmFunctionBuilder::BuildSlice(Value *start, Value *stop, Value *step)
{
    BasicBlock *failure = BasicBlock::Create("BuildSlice_failure",
                                             function());
    BasicBlock *success = BasicBlock::Create("BuildSlice_success",
                                             function());
    Function *build_slice = GetGlobalFunction<
        PyObject *(PyObject *, PyObject *, PyObject *)>("PySlice_New");
    Value *result = builder().CreateCall3(
        build_slice, start, stop, step, "BUILD_SLICE_result");
    DecRef(start);
    DecRef(stop);
    XDecRef(step);
    builder().CreateCondBr(IsNull(result), failure, success);

    builder().SetInsertPoint(failure);
    Return(Constant::getNullValue(function()->getReturnType()));

    builder().SetInsertPoint(success);
    Push(result);
}

void
LlvmFunctionBuilder::BUILD_SLICE_TWO()
{
    Value *step = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    Value *stop = Pop();
    Value *start = Pop();
    BuildSlice(start, stop, step);
}

void
LlvmFunctionBuilder::BUILD_SLICE_THREE()
{
    Value *step = Pop();
    Value *stop = Pop();
    Value *start = Pop();
    BuildSlice(start, stop, step);
}

// Implement seq[start:stop]
void
LlvmFunctionBuilder::ApplySlice(Value *seq, Value *start, Value *stop)
{
    BasicBlock *failure = BasicBlock::Create("ApplySlice_failure",
                                             function());
    BasicBlock *success = BasicBlock::Create("ApplySlice_success",
                                             function());
    Function *build_slice = GetGlobalFunction<
        PyObject *(PyObject *, PyObject *, PyObject *)>("_PyEval_ApplySlice");
    Value *result = builder().CreateCall3(
        build_slice, seq, start, stop, "ApplySlice_result");
    XDecRef(stop);
    XDecRef(start);
    DecRef(seq);
    builder().CreateCondBr(IsNull(result), failure, success);

    builder().SetInsertPoint(failure);
    Return(Constant::getNullValue(function()->getReturnType()));

    builder().SetInsertPoint(success);
    Push(result);
}

void
LlvmFunctionBuilder::SLICE_BOTH()
{
    Value *stop = Pop();
    Value *start = Pop();
    Value *seq = Pop();
    ApplySlice(seq, start, stop);
}

void
LlvmFunctionBuilder::SLICE_LEFT()
{
    Value *stop = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    Value *start = Pop();
    Value *seq = Pop();
    ApplySlice(seq, start, stop);
}

void
LlvmFunctionBuilder::SLICE_RIGHT()
{
    Value *stop = Pop();
    Value *start = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    Value *seq = Pop();
    ApplySlice(seq, start, stop);
}

void
LlvmFunctionBuilder::SLICE_NONE()
{
    Value *stop = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    Value *start = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    Value *seq = Pop();
    ApplySlice(seq, start, stop);
}

// Implement seq[start:stop] = source, and del seq[start:stop]
void
LlvmFunctionBuilder::AssignSlice(Value *seq, Value *start, Value *stop,
                                 Value *source)
{
    BasicBlock *failure = BasicBlock::Create("AssignSlice_failure",
                                             function());
    BasicBlock *success = BasicBlock::Create("AssignSlice_success",
                                             function());
    Function *assign_slice = GetGlobalFunction<
        int (PyObject *, PyObject *, PyObject *, PyObject *)>(
            "_PyEval_AssignSlice");
    Value *result = builder().CreateCall4(
        assign_slice, seq, start, stop, source, "ApplySlice_result");
    XDecRef(source);
    XDecRef(stop);
    XDecRef(start);
    DecRef(seq);
    builder().CreateCondBr(IsNonZero(result), failure, success);

    builder().SetInsertPoint(failure);
    Return(Constant::getNullValue(function()->getReturnType()));

    builder().SetInsertPoint(success);
}

void
LlvmFunctionBuilder::STORE_SLICE_BOTH()
{
    Value *stop = Pop();
    Value *start = Pop();
    Value *seq = Pop();
    Value *source = Pop();
    AssignSlice(seq, start, stop, source);
}

void
LlvmFunctionBuilder::STORE_SLICE_LEFT()
{
    Value *stop = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    Value *start = Pop();
    Value *seq = Pop();
    Value *source = Pop();
    AssignSlice(seq, start, stop, source);
}

void
LlvmFunctionBuilder::STORE_SLICE_RIGHT()
{
    Value *stop = Pop();
    Value *start = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    Value *seq = Pop();
    Value *source = Pop();
    AssignSlice(seq, start, stop, source);
}

void
LlvmFunctionBuilder::STORE_SLICE_NONE()
{
    Value *stop = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    Value *start = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    Value *seq = Pop();
    Value *source = Pop();
    AssignSlice(seq, start, stop, source);
}

void
LlvmFunctionBuilder::DELETE_SLICE_BOTH()
{
    Value *stop = Pop();
    Value *start = Pop();
    Value *seq = Pop();
    Value *source = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    AssignSlice(seq, start, stop, source);
}

void
LlvmFunctionBuilder::DELETE_SLICE_LEFT()
{
    Value *stop = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    Value *start = Pop();
    Value *seq = Pop();
    Value *source = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    AssignSlice(seq, start, stop, source);
}

void
LlvmFunctionBuilder::DELETE_SLICE_RIGHT()
{
    Value *stop = Pop();
    Value *start = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    Value *seq = Pop();
    Value *source = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    AssignSlice(seq, start, stop, source);
}

void
LlvmFunctionBuilder::DELETE_SLICE_NONE()
{
    Value *stop = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    Value *start = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    Value *seq = Pop();
    Value *source = Constant::getNullValue(
        TypeBuilder<PyObject *>::cache(this->module_));
    AssignSlice(seq, start, stop, source);
}

void
LlvmFunctionBuilder::UNPACK_SEQUENCE(int size)
{
    BasicBlock *failure =
        BasicBlock::Create("UNPACK_SEQUENCE_failure", function());
    BasicBlock *success =
        BasicBlock::Create("UNPACK_SEQUENCE_success", function());

    Value *iterable = Pop();
    // We could speed up the common case quite a bit by doing the unpacking
    // inline, like ceval.c does; that would allow LLVM to optimize the heck
    // out of it as well. Then again, we could do even better by combining
    // this opcode and STORE_* ones that follow into a single block of code
    // circumventing the stack altogether. And omitting the horrible
    // external stack munging that UnpackIterable does.
    Function *unpack_iterable = GetGlobalFunction<
        int(PyObject *, int, PyObject **)>("_PyEval_UnpackIterable");
    Value *stack_pointer = builder().CreateLoad(this->stack_pointer_addr_);
    Value *result = builder().CreateCall3(
        unpack_iterable, iterable,
        ConstantInt::get(TypeBuilder<int>::cache(this->module_), size, true),
        builder().CreateGEP(
            stack_pointer,
            ConstantInt::get(
                TypeBuilder<Py_ssize_t>::cache(this->module_), size, true)));
    DecRef(iterable);
    // Absurdly, _PyEval_UnpackIterable returns 1/0 for success/failure,
    // instead of the 0/-1 that all other int-returning calls use.
    builder().CreateCondBr(IsNonZero(result), success, failure);
    
    builder().SetInsertPoint(failure);
    Return(Constant::getNullValue(function()->getReturnType()));
    
    builder().SetInsertPoint(success);
    Value *new_stack_pointer = builder().CreateGEP(
        stack_pointer, ConstantInt::get(
            TypeBuilder<Py_ssize_t>::cache(this->module_), size, true));
    builder().CreateStore(new_stack_pointer, this->stack_pointer_addr_);
}

// Adds delta to *addr, and returns the new value.
static Value *
increment_and_get(llvm::IRBuilder<>& builder, Value *addr, int64_t delta)
{
    Value *orig = builder.CreateLoad(addr);
    Value *new_ = builder.CreateAdd(
        orig,
        get_signed_constant_int(orig->getType(), delta));
    builder.CreateStore(new_, addr);
    return new_;
}

void
LlvmFunctionBuilder::IncRef(Value *value)
{
#ifdef Py_REF_DEBUG
    // Increment the global reference count.
    Value *reftotal_addr = GetGlobalVariable<Py_ssize_t>("_Py_RefTotal");
    increment_and_get(builder(), reftotal_addr, 1);
#endif

    Value *as_pyobject = builder().CreateBitCast(
        value, TypeBuilder<PyObject*>::cache(this->module_));
    Value *refcnt_addr =
        builder().CreateStructGEP(as_pyobject, ObjectTy::FIELD_REFCNT);
    increment_and_get(builder(), refcnt_addr, 1);
}

void
LlvmFunctionBuilder::DecRef(Value *value)
{
#ifdef Py_REF_DEBUG
    // Decrement the global reference count.
    Value *reftotal_addr = GetGlobalVariable<Py_ssize_t>("_Py_RefTotal");
    increment_and_get(builder(), reftotal_addr, -1);
#endif

    Value *as_pyobject = builder().CreateBitCast(
        value, TypeBuilder<PyObject*>::cache(this->module_));
    Value *refcnt_addr =
        builder().CreateStructGEP(as_pyobject, ObjectTy::FIELD_REFCNT);
    Value *new_refcnt = increment_and_get(builder(), refcnt_addr, -1);

    // Check if we need to deallocate the object.
    BasicBlock *block_dealloc = BasicBlock::Create("dealloc", this->function_);
    BasicBlock *block_tail = BasicBlock::Create("decref_tail", this->function_);
    BasicBlock *block_ref_ne_zero = block_tail;
#ifdef Py_REF_DEBUG
    block_ref_ne_zero = BasicBlock::Create("check_refcnt", this->function_);
#endif

    builder().CreateCondBr(IsNonZero(new_refcnt),
                           block_ref_ne_zero, block_dealloc);

#ifdef Py_REF_DEBUG
    builder().SetInsertPoint(block_ref_ne_zero);
    Value *less_zero = builder().CreateICmpSLT(
        new_refcnt, Constant::getNullValue(new_refcnt->getType()));
    BasicBlock *block_ref_lt_zero = BasicBlock::Create("negative_refcount",
                                                 this->function_);
    builder().CreateCondBr(less_zero, block_ref_lt_zero, block_tail);

    builder().SetInsertPoint(block_ref_lt_zero);
    Value *neg_refcount = GetGlobalFunction<void(const char*, int, PyObject*)>(
        "_Py_NegativeRefcount");
    // TODO: Well that __FILE__ and __LINE__ are going to be useless!
    builder().CreateCall3(
        neg_refcount,
        builder().CreateGlobalStringPtr(__FILE__, __FILE__),
        ConstantInt::get(TypeBuilder<int>::cache(this->module_), __LINE__),
        as_pyobject);
    builder().CreateBr(block_tail);
#endif

    builder().SetInsertPoint(block_dealloc);
    Value *dealloc = GetGlobalFunction<void(PyObject *)>("_PyLlvm_WrapDealloc");
    builder().CreateCall(dealloc, as_pyobject);
    builder().CreateBr(block_tail);

    builder().SetInsertPoint(block_tail);
}

void
LlvmFunctionBuilder::XDecRef(Value *value)
{
    BasicBlock *do_decref = BasicBlock::Create("decref", function());
    BasicBlock *decref_end = BasicBlock::Create("decref_end", function());
    builder().CreateCondBr(IsNull(value), decref_end, do_decref);

    builder().SetInsertPoint(do_decref);
    DecRef(value);
    builder().CreateBr(decref_end);

    builder().SetInsertPoint(decref_end);
}

void
LlvmFunctionBuilder::Push(Value *value)
{
    Value *stack_pointer = builder().CreateLoad(this->stack_pointer_addr_);
    builder().CreateStore(value, stack_pointer);
    Value *new_stack_pointer = builder().CreateGEP(
        stack_pointer, ConstantInt::get(Type::Int32Ty, 1));
    builder().CreateStore(new_stack_pointer, this->stack_pointer_addr_);
}

Value *
LlvmFunctionBuilder::Pop()
{
    Value *stack_pointer = builder().CreateLoad(this->stack_pointer_addr_);
    Value *new_stack_pointer = builder().CreateGEP(
        stack_pointer, get_signed_constant_int(Type::Int32Ty, -1));
    Value *former_top = builder().CreateLoad(new_stack_pointer);
    builder().CreateStore(new_stack_pointer, this->stack_pointer_addr_);
    return former_top;
}

void
LlvmFunctionBuilder::SetLocal(int locals_index, llvm::Value *new_value)
{
    Value *local_slot = builder().CreateGEP(
        this->fastlocals_, ConstantInt::get(Type::Int32Ty, locals_index));
    Value *orig_value = builder().CreateLoad(local_slot, "local_overwritten");
    builder().CreateStore(new_value, local_slot);
    XDecRef(orig_value);
}

Value *
LlvmFunctionBuilder::LookupName(int names_index)
{
    Value *name = builder().CreateLoad(
        builder().CreateGEP(
            this->names_, ConstantInt::get(Type::Int32Ty, names_index),
            "global_name"));
    return name;
}

void
LlvmFunctionBuilder::InsertAbort(const char *opcode_name)
{
    std::string message("Undefined opcode: ");
    message.append(opcode_name);
    builder().CreateCall(GetGlobalFunction<int(const char*)>("puts"),
                         builder().CreateGlobalStringPtr(message.c_str(),
                                                         message.c_str()));
    builder().CreateCall(GetGlobalFunction<void()>("abort"));
}

template<typename FunctionType> Function *
LlvmFunctionBuilder::GetGlobalFunction(const std::string &name)
{
    return llvm::cast<Function>(
        this->module_->getOrInsertFunction(
            name, TypeBuilder<FunctionType>::cache(this->module_)));
}

template<typename VariableType> Constant *
LlvmFunctionBuilder::GetGlobalVariable(const std::string &name)
{
    return this->module_->getOrInsertGlobal(
        name, TypeBuilder<VariableType>::cache(this->module_));
}

Value *
LlvmFunctionBuilder::IsNull(Value *value)
{
    return builder().CreateICmpEQ(
        value, Constant::getNullValue(value->getType()));
}

Value *
LlvmFunctionBuilder::IsNonZero(Value *value)
{
    return builder().CreateICmpNE(
        value, Constant::getNullValue(value->getType()));
}

llvm::Value *
LlvmFunctionBuilder::IsTrue(Value *value)
{
    BasicBlock *not_py_true =
        BasicBlock::Create("IsTrue_is_not_PyTrue", function());
    BasicBlock *not_py_false =
        BasicBlock::Create("IsTrue_is_not_PyFalse", function());
    BasicBlock *failure =
        BasicBlock::Create("IsTrue_failure", function());
    BasicBlock *success =
        BasicBlock::Create("IsTrue_success", function());
    BasicBlock *done =
        BasicBlock::Create("IsTrue_done", function());
    BasicBlock *entry = builder().GetInsertBlock();
    
    Value *py_false = GetGlobalVariable<PyObject>("_Py_ZeroStruct");
    Value *py_true = GetGlobalVariable<PyObject>("_Py_TrueStruct");
    Value *zero = ConstantInt::get(
        TypeBuilder<int>::cache(this->module_), 0, true /* signed */);

    Value *is_PyTrue = builder().CreateICmpEQ(
        py_true, value, "IsTrue_is_PyTrue");
    builder().CreateCondBr(is_PyTrue, done, not_py_true);

    builder().SetInsertPoint(not_py_true);
    Value *is_PyFalse = builder().CreateICmpEQ(
        py_false, value, "IsTrue_is_PyFalse");
    builder().CreateCondBr(is_PyFalse, done, not_py_false);

    builder().SetInsertPoint(not_py_false);
    Function *pyobject_istrue =
        GetGlobalFunction<int(PyObject *)>("PyObject_IsTrue");
    Value *istrue_result = builder().CreateCall(
        pyobject_istrue, value, "PyObject_IsTrue_result");
    Value *is_error = builder().CreateICmpSLT(
        istrue_result, zero, "PyObject_IsTrue_is_error");
    builder().CreateCondBr(is_error, failure, success);
    
    builder().SetInsertPoint(failure);
    Return(Constant::getNullValue(function()->getReturnType()));
    
    builder().SetInsertPoint(success);
    Value *is_nonzero = builder().CreateICmpSGT(
        istrue_result, zero, "PyObject_IsTrue_is_true");
    builder().CreateBr(done);

    builder().SetInsertPoint(done);    
    PHINode *phi = builder().CreatePHI(Type::Int1Ty, "IsTrue_bool_result");
    phi->addIncoming(is_PyTrue, entry);
    // If we come from not_py_true, we want to return the i1 for false,
    // and is_PyTrue will conveniently be that.
    phi->addIncoming(is_PyTrue, not_py_true);
    phi->addIncoming(is_nonzero, success);
    return phi;

}

}  // namespace py


// Helper functions for the LLVM IR. These exist for
// non-speed-critical code that's easier to write in C, or for calls
// that are functions in pydebug mode and macros otherwise.
extern "C" {

void
_PyLlvm_WrapDealloc(PyObject *obj)
{
    _Py_Dealloc(obj);
}

}